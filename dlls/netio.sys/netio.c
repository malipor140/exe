/*
 * WSK (Winsock Kernel) driver library.
 *
 * Copyright 2020 Paul Gofman <pgofman@codeweavers.com> for Codeweavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winioctl.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ddk/wsk.h"
#include "wine/debug.h"
#include "winsock2.h"
#include "ws2tcpip.h"

#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(netio);

struct _WSK_CLIENT
{
    WSK_REGISTRATION *registration;
    WSK_CLIENT_NPI *client_npi;
};

struct listen_socket_callback_context
{
    SOCKADDR *remote_address;
    const void *client_dispatch;
    void *client_context;
    char addr_buffer[2 * (sizeof(SOCKADDR) + 16)];
    SOCKET acceptor;
};

#define MAX_PENDING_IO 10

struct wsk_pending_io
{
    OVERLAPPED ovr;
    TP_WAIT *tp_wait;
    IRP *irp;
};

struct wsk_socket_internal
{
    WSK_SOCKET wsk_socket;
    SOCKET s;
    const void *client_dispatch;
    void *client_context;
    ULONG flags;
    ADDRESS_FAMILY address_family;
    USHORT socket_type;
    ULONG protocol;

    CRITICAL_SECTION cs_socket;

    struct wsk_pending_io pending_io[MAX_PENDING_IO];

    union
    {
        struct listen_socket_callback_context listen_socket_callback_context;
    }
    callback_context;
};

static LPFN_ACCEPTEX pAcceptEx;
static const WSK_PROVIDER_CONNECTION_DISPATCH wsk_provider_connection_dispatch;

static inline struct wsk_socket_internal *wsk_socket_internal_from_wsk_socket(WSK_SOCKET *wsk_socket)
{
    return CONTAINING_RECORD(wsk_socket, struct wsk_socket_internal, wsk_socket);
}

static NTSTATUS sock_error_to_ntstatus(DWORD err)
{
    switch (err)
    {
        case 0:                    return STATUS_SUCCESS;
        case WSAEBADF:             return STATUS_INVALID_HANDLE;
        case WSAEACCES:            return STATUS_ACCESS_DENIED;
        case WSAEFAULT:            return STATUS_NO_MEMORY;
        case WSAEINVAL:            return STATUS_INVALID_PARAMETER;
        case WSAEMFILE:            return STATUS_TOO_MANY_OPENED_FILES;
        case WSAEWOULDBLOCK:       return STATUS_CANT_WAIT;
        case WSAEINPROGRESS:       return STATUS_PENDING;
        case WSAEALREADY:          return STATUS_NETWORK_BUSY;
        case WSAENOTSOCK:          return STATUS_OBJECT_TYPE_MISMATCH;
        case WSAEDESTADDRREQ:      return STATUS_INVALID_PARAMETER;
        case WSAEMSGSIZE:          return STATUS_BUFFER_OVERFLOW;
        case WSAEPROTONOSUPPORT:
        case WSAESOCKTNOSUPPORT:
        case WSAEPFNOSUPPORT:
        case WSAEAFNOSUPPORT:
        case WSAEPROTOTYPE:        return STATUS_NOT_SUPPORTED;
        case WSAENOPROTOOPT:       return STATUS_INVALID_PARAMETER;
        case WSAEOPNOTSUPP:        return STATUS_NOT_IMPLEMENTED;
        case WSAEADDRINUSE:        return STATUS_ADDRESS_ALREADY_ASSOCIATED;
        case WSAEADDRNOTAVAIL:     return STATUS_INVALID_PARAMETER;
        case WSAECONNREFUSED:      return STATUS_CONNECTION_REFUSED;
        case WSAESHUTDOWN:         return STATUS_PIPE_DISCONNECTED;
        case WSAENOTCONN:          return STATUS_CONNECTION_DISCONNECTED;
        case WSAETIMEDOUT:         return STATUS_IO_TIMEOUT;
        case WSAENETUNREACH:       return STATUS_NETWORK_UNREACHABLE;
        case WSAENETDOWN:          return STATUS_NETWORK_BUSY;
        case WSAECONNRESET:        return STATUS_CONNECTION_RESET;
        case WSAECONNABORTED:      return STATUS_CONNECTION_ABORTED;
        case WSAHOST_NOT_FOUND:    return STATUS_NOT_FOUND;
        default:
            FIXME("Unmapped error %u.\n", err);
            return STATUS_UNSUCCESSFUL;
    }
}

static inline void lock_socket(struct wsk_socket_internal *socket)
{
    EnterCriticalSection(&socket->cs_socket);
}

static inline void unlock_socket(struct wsk_socket_internal *socket)
{
    LeaveCriticalSection(&socket->cs_socket);
}

static void socket_init(struct wsk_socket_internal *socket)
{
    InitializeCriticalSection(&socket->cs_socket);
}

static void dispatch_irp(IRP *irp, NTSTATUS status)
{
    irp->IoStatus.u.Status = status;
    --irp->CurrentLocation;
    --irp->Tail.Overlay.s.u2.CurrentStackLocation;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static struct wsk_pending_io *allocate_pending_io(struct wsk_socket_internal *socket,
        PTP_WAIT_CALLBACK socket_async_callback, IRP *irp)
{
    struct wsk_pending_io *io = socket->pending_io;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(socket->pending_io); ++i)
        if (!io[i].irp)
            break;

    if (i == ARRAY_SIZE(socket->pending_io))
    {
        FIXME("Pending io requests count exceeds limit.\n");
        return NULL;
    }

    io[i].irp = irp;

    if (io[i].tp_wait)
        return &io[i];

    io[i].ovr.hEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    io[i].tp_wait = CreateThreadpoolWait(socket_async_callback, socket, NULL);

    return &io[i];
}

static struct wsk_pending_io *find_pending_io(struct wsk_socket_internal *socket, TP_WAIT *tp_wait)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(socket->pending_io); ++i)
    {
        if (socket->pending_io[i].tp_wait == tp_wait)
            return &socket->pending_io[i];
    }

    FIXME("Pending io not found for tp_wait %p.\n", tp_wait);
    return NULL;
}

static void dispatch_pending_io(struct wsk_pending_io *io, NTSTATUS status, ULONG_PTR information)
{
    TRACE("io %p, status %#x, information %#lx.\n", io, status, information);

    io->irp->IoStatus.Information = information;
    dispatch_irp(io->irp, status);
    io->irp = NULL;
}

static NTSTATUS WINAPI wsk_control_socket(WSK_SOCKET *socket, WSK_CONTROL_SOCKET_TYPE request_type,
        ULONG control_code, ULONG level, SIZE_T input_size, void *input_buffer, SIZE_T output_size,
        void *output_buffer, SIZE_T *output_size_returned, IRP *irp)
{
    FIXME("socket %p, request_type %u, control_code %#x, level %u, input_size %lu, input_buffer %p, "
            "output_size %lu, output_buffer %p, output_size_returned %p, irp %p stub.\n",
            socket, request_type, control_code, level, input_size, input_buffer, output_size,
            output_buffer, output_size_returned, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_close_socket(WSK_SOCKET *socket, IRP *irp)
{
    struct wsk_socket_internal *s = wsk_socket_internal_from_wsk_socket(socket);
    NTSTATUS status;
    unsigned int i;

    TRACE("socket %p, irp %p.\n", socket, irp);

    lock_socket(s);

    for (i = 0; i < ARRAY_SIZE(s->pending_io); ++i)
    {
        struct wsk_pending_io *io = &s->pending_io[i];

        if (io->tp_wait)
        {
            CancelIoEx((HANDLE)s->s, &io->ovr);
            SetThreadpoolWait(io->tp_wait, NULL, NULL);
            unlock_socket(s);
            WaitForThreadpoolWaitCallbacks(io->tp_wait, FALSE);
            lock_socket(s);
            CloseThreadpoolWait(io->tp_wait);
            CloseHandle(io->ovr.hEvent);
        }

        if (io->irp)
            dispatch_pending_io(io, STATUS_CANCELLED, 0);
    }

    if (s->flags & WSK_FLAG_LISTEN_SOCKET && s->callback_context.listen_socket_callback_context.acceptor)
        closesocket(s->callback_context.listen_socket_callback_context.acceptor);

    status = closesocket(s->s) ? sock_error_to_ntstatus(WSAGetLastError()) : STATUS_SUCCESS;

    unlock_socket(s);
    DeleteCriticalSection(&s->cs_socket);
    heap_free(socket);

    irp->IoStatus.Information = 0;
    dispatch_irp(irp, status);

    return status ? status : STATUS_PENDING;
}

static NTSTATUS WINAPI wsk_bind(WSK_SOCKET *socket, SOCKADDR *local_address, ULONG flags, IRP *irp)
{
    struct wsk_socket_internal *s = wsk_socket_internal_from_wsk_socket(socket);
    NTSTATUS status;

    TRACE("socket %p, local_address %p, flags %#x, irp %p.\n",
            socket, local_address, flags, irp);

    if (!irp)
        return STATUS_INVALID_PARAMETER;

    if (bind(s->s, local_address, sizeof(*local_address)))
        status = sock_error_to_ntstatus(WSAGetLastError());
    else if (s->flags & WSK_FLAG_LISTEN_SOCKET && listen(s->s, SOMAXCONN))
        status = sock_error_to_ntstatus(WSAGetLastError());
    else
        status = STATUS_SUCCESS;

    TRACE("status %#x.\n", status);
    irp->IoStatus.Information = 0;
    dispatch_irp(irp, status);
    return STATUS_PENDING;
}

static void create_accept_socket(struct wsk_socket_internal *socket, struct wsk_pending_io *io)
{
    struct listen_socket_callback_context *context
            = &socket->callback_context.listen_socket_callback_context;
    struct wsk_socket_internal *accept_socket;

    if (!(accept_socket = heap_alloc_zero(sizeof(*accept_socket))))
    {
        ERR("No memory.\n");
        dispatch_pending_io(io, STATUS_NO_MEMORY, 0);
    }
    else
    {
        TRACE("accept_socket %p.\n", accept_socket);
        accept_socket->wsk_socket.Dispatch = &wsk_provider_connection_dispatch;
        accept_socket->s = context->acceptor;
        accept_socket->client_dispatch = context->client_dispatch;
        accept_socket->client_context = context->client_context;
        accept_socket->socket_type = socket->socket_type;
        accept_socket->address_family = socket->address_family;
        accept_socket->protocol = socket->protocol;
        accept_socket->flags = WSK_FLAG_CONNECTION_SOCKET;
        socket_init(accept_socket);
        /* TODO: fill local and remote addresses. */

        dispatch_pending_io(io, STATUS_SUCCESS, (ULONG_PTR)&accept_socket->wsk_socket);
    }
}

static void WINAPI accept_callback(TP_CALLBACK_INSTANCE *instance, void *socket_, TP_WAIT *wait,
        TP_WAIT_RESULT wait_result)
{
    struct listen_socket_callback_context *context;
    struct wsk_socket_internal *socket = socket_;
    struct wsk_pending_io *io;
    DWORD size;

    TRACE("instance %p, socket %p, wait %p, wait_result %#x.\n", instance, socket, wait, wait_result);

    lock_socket(socket);
    context = &socket->callback_context.listen_socket_callback_context;
    io = find_pending_io(socket, wait);

    if (GetOverlappedResult((HANDLE)socket->s, &io->ovr, &size, FALSE))
    {
        create_accept_socket(socket, io);
    }
    else
    {
        closesocket(context->acceptor);
        context->acceptor = 0;
        dispatch_pending_io(io, io->ovr.Internal, 0);
    }
    unlock_socket(socket);
}

static BOOL WINAPI init_accept_functions(INIT_ONCE *once, void *param, void **context)
{
    GUID acceptex_guid = WSAID_ACCEPTEX;
    SOCKET s = (SOCKET)param;
    DWORD size;

    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &acceptex_guid, sizeof(acceptex_guid),
            &pAcceptEx, sizeof(pAcceptEx), &size, NULL, NULL))
    {
        ERR("Could not get AcceptEx address, error %u.\n", WSAGetLastError());
        return FALSE;
    }
    return TRUE;
}

static NTSTATUS WINAPI wsk_accept(WSK_SOCKET *listen_socket, ULONG flags, void *accept_socket_context,
        const WSK_CLIENT_CONNECTION_DISPATCH *accept_socket_dispatch, SOCKADDR *local_address,
        SOCKADDR *remote_address, IRP *irp)
{
    struct wsk_socket_internal *s = wsk_socket_internal_from_wsk_socket(listen_socket);
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    struct listen_socket_callback_context *context;
    struct wsk_pending_io *io;
    SOCKET acceptor;
    DWORD size;
    int error;

    TRACE("listen_socket %p, flags %#x, accept_socket_context %p, accept_socket_dispatch %p, "
            "local_address %p, remote_address %p, irp %p.\n",
            listen_socket, flags, accept_socket_context, accept_socket_dispatch, local_address,
            remote_address, irp);

    if (!irp)
        return STATUS_INVALID_PARAMETER;

    if (!InitOnceExecuteOnce(&init_once, init_accept_functions, (void *)s->s, NULL))
    {
        dispatch_irp(irp, STATUS_UNSUCCESSFUL);
        return STATUS_PENDING;
    }

    lock_socket(s);
    if (!(io = allocate_pending_io(s, accept_callback, irp)))
    {
        irp->IoStatus.Information = 0;
        dispatch_irp(irp, STATUS_UNSUCCESSFUL);
        unlock_socket(s);
        return STATUS_PENDING;
    }

    context = &s->callback_context.listen_socket_callback_context;
    if ((acceptor = WSASocketW(s->address_family, s->socket_type, s->protocol, NULL, 0, WSA_FLAG_OVERLAPPED))
            == INVALID_SOCKET)
    {
        dispatch_pending_io(io, sock_error_to_ntstatus(WSAGetLastError()), 0);
        unlock_socket(s);
        return STATUS_PENDING;
    }

    context->remote_address = remote_address;
    context->client_dispatch = accept_socket_dispatch;
    context->client_context = accept_socket_context;
    context->acceptor = acceptor;

    if (pAcceptEx(s->s, acceptor, context->addr_buffer, 0,
            sizeof(SOCKADDR) + 16, sizeof(SOCKADDR) + 16, &size, &io->ovr))
    {
        create_accept_socket(s, io);
    }
    else if ((error = WSAGetLastError()) == ERROR_IO_PENDING)
    {
        SetThreadpoolWait(io->tp_wait, io->ovr.hEvent, NULL);
    }
    else
    {
        closesocket(acceptor);
        context->acceptor = 0;
        dispatch_pending_io(io, sock_error_to_ntstatus(error), 0);
    }
    unlock_socket(s);

    return STATUS_PENDING;
}

static NTSTATUS WINAPI wsk_inspect_complete(WSK_SOCKET *listen_socket, WSK_INSPECT_ID *inspect_id,
        WSK_INSPECT_ACTION action, IRP *irp)
{
    FIXME("listen_socket %p, inspect_id %p, action %u, irp %p stub.\n",
            listen_socket, inspect_id, action, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_get_local_address(WSK_SOCKET *socket, SOCKADDR *local_address, IRP *irp)
{
    FIXME("socket %p, local_address %p, irp %p stub.\n", socket, local_address, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static const WSK_PROVIDER_LISTEN_DISPATCH wsk_provider_listen_dispatch =
{
    {
        wsk_control_socket,
        wsk_close_socket,
    },
    wsk_bind,
    wsk_accept,
    wsk_inspect_complete,
    wsk_get_local_address,
};

static NTSTATUS WINAPI wsk_connect(WSK_SOCKET *socket, SOCKADDR *remote_address, ULONG flags, IRP *irp)
{
    FIXME("socket %p, remote_address %p, flags %#x, irp %p stub.\n", socket, remote_address, flags, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_get_remote_address(WSK_SOCKET *socket, SOCKADDR *remote_address, IRP *irp)
{
    FIXME("socket %p, remote_address %p, irp %p stub.\n", socket, remote_address, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_send(WSK_SOCKET *socket, WSK_BUF *buffer, ULONG flags, IRP *irp)
{
    FIXME("socket %p, buffer %p, flags %#x, irp %p stub.\n", socket, buffer, flags, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_receive(WSK_SOCKET *socket, WSK_BUF *buffer, ULONG flags, IRP *irp)
{
    FIXME("socket %p, buffer %p, flags %#x, irp %p stub.\n", socket, buffer, flags, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_disconnect(WSK_SOCKET *socket, WSK_BUF *buffer, ULONG flags, IRP *irp)
{
    FIXME("socket %p, buffer %p, flags %#x, irp %p stub.\n", socket, buffer, flags, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_release(WSK_SOCKET *socket, WSK_DATA_INDICATION *data_indication)
{
    FIXME("socket %p, data_indication %p stub.\n", socket, data_indication);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_connext_ex(WSK_SOCKET *socket, SOCKADDR *remote_address, WSK_BUF *buffer,
        ULONG flags, IRP *irp)
{
    FIXME("socket %p, remote_address %p, buffer %p, flags %#x, irp %p stub.\n",
            socket, remote_address, buffer, flags, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_send_ex(void)
{
    FIXME("stub (no prototype, will crash).\n");

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_receive_ex(void)
{
    FIXME("stub (no prototype, will crash).\n");

    return STATUS_NOT_IMPLEMENTED;
}

static const WSK_PROVIDER_CONNECTION_DISPATCH wsk_provider_connection_dispatch =
{
    {
        wsk_control_socket,
        wsk_close_socket,
    },
    wsk_bind,
    wsk_connect,
    wsk_get_local_address,
    wsk_get_remote_address,
    wsk_send,
    wsk_receive,
    wsk_disconnect,
    wsk_release,
    wsk_connext_ex,
    wsk_send_ex,
    wsk_receive_ex,
};

static NTSTATUS WINAPI wsk_socket(WSK_CLIENT *client, ADDRESS_FAMILY address_family, USHORT socket_type,
        ULONG protocol, ULONG flags, void *socket_context, const void *dispatch, PEPROCESS owning_process,
        PETHREAD owning_thread, SECURITY_DESCRIPTOR *security_descriptor, IRP *irp)
{
    struct wsk_socket_internal *socket;
    NTSTATUS status;
    SOCKET s;

    TRACE("client %p, address_family %#x, socket_type %#x, protocol %#x, flags %#x, socket_context %p, dispatch %p, "
            "owning_process %p, owning_thread %p, security_descriptor %p, irp %p.\n",
            client, address_family, socket_type, protocol, flags, socket_context, dispatch, owning_process,
            owning_thread, security_descriptor, irp);

    if (!irp)
        return STATUS_INVALID_PARAMETER;

    if (!client)
        return STATUS_INVALID_HANDLE;

    irp->IoStatus.Information = 0;

    if ((s = WSASocketW(address_family, socket_type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED)) == INVALID_SOCKET)
    {
        status = sock_error_to_ntstatus(WSAGetLastError());
        goto done;
    }

    if (!(socket = heap_alloc_zero(sizeof(*socket))))
    {
        status = STATUS_NO_MEMORY;
        closesocket(s);
        goto done;
    }

    socket->s = s;
    socket->client_dispatch = dispatch;
    socket->client_context = socket_context;
    socket->socket_type = socket_type;
    socket->flags = flags;
    socket->address_family = address_family;
    socket->protocol = protocol;

    switch (flags)
    {
        case WSK_FLAG_LISTEN_SOCKET:
            socket->wsk_socket.Dispatch = &wsk_provider_listen_dispatch;
            break;

        case WSK_FLAG_CONNECTION_SOCKET:
            socket->wsk_socket.Dispatch = &wsk_provider_connection_dispatch;
            break;

        default:
            FIXME("Flags %#x not implemented.\n", flags);
            closesocket(s);
            heap_free(socket);
            status = STATUS_NOT_IMPLEMENTED;
            goto done;
    }

    socket_init(socket);

    irp->IoStatus.Information = (ULONG_PTR)&socket->wsk_socket;
    status = STATUS_SUCCESS;

done:
    dispatch_irp(irp, status);
    return status ? status : STATUS_PENDING;
}

static NTSTATUS WINAPI wsk_socket_connect(WSK_CLIENT *client, USHORT socket_type, ULONG protocol,
        SOCKADDR *local_address, SOCKADDR *remote_address, ULONG flags, void *socket_context,
        const WSK_CLIENT_CONNECTION_DISPATCH *dispatch, PEPROCESS owning_process, PETHREAD owning_thread,
        SECURITY_DESCRIPTOR *security_descriptor, IRP *irp)
{
    FIXME("client %p, socket_type %#x, protocol %#x, local_address %p, remote_address %p, "
            "flags %#x, socket_context %p, dispatch %p, owning_process %p, owning_thread %p, "
            "security_descriptor %p, irp %p stub.\n",
            client, socket_type, protocol, local_address, remote_address, flags, socket_context,
            dispatch, owning_process, owning_thread, security_descriptor, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI wsk_control_client(WSK_CLIENT *client, ULONG control_code, SIZE_T input_size,
        void *input_buffer, SIZE_T output_size, void *output_buffer, SIZE_T *output_size_returned,
        IRP *irp
)
{
    FIXME("client %p, control_code %#x, input_size %lu, input_buffer %p, output_size %lu, "
            "output_buffer %p, output_size_returned %p, irp %p, stub.\n",
            client, control_code, input_size, input_buffer, output_size, output_buffer,
            output_size_returned, irp);

    return STATUS_NOT_IMPLEMENTED;
}

struct wsk_get_address_info_context
{
    UNICODE_STRING *node_name;
    UNICODE_STRING *service_name;
    ULONG namespace;
    GUID *provider;
    ADDRINFOEXW *hints;
    ADDRINFOEXW **result;
    IRP *irp;
};

static void WINAPI get_address_info_callback(TP_CALLBACK_INSTANCE *instance, void *context_)
{
    struct wsk_get_address_info_context *context = context_;
    INT ret;

    TRACE("instance %p, context %p.\n", instance, context);

    ret = GetAddrInfoExW( context->node_name ? context->node_name->Buffer : NULL,
            context->service_name ? context->service_name->Buffer : NULL, context->namespace,
            context->provider, context->hints, context->result, NULL, NULL, NULL, NULL);

    context->irp->IoStatus.Information = 0;
    dispatch_irp(context->irp, sock_error_to_ntstatus(ret));
    heap_free(context);
}

static NTSTATUS WINAPI wsk_get_address_info(WSK_CLIENT *client, UNICODE_STRING *node_name,
        UNICODE_STRING *service_name, ULONG name_space, GUID *provider, ADDRINFOEXW *hints,
        ADDRINFOEXW **result, PEPROCESS owning_process, PETHREAD owning_thread, IRP *irp)
{
    struct wsk_get_address_info_context *context;
    NTSTATUS status;

    TRACE("client %p, node_name %p, service_name %p, name_space %#x, provider %p, hints %p, "
            "result %p, owning_process %p, owning_thread %p, irp %p.\n",
            client, node_name, service_name, name_space, provider, hints, result,
            owning_process, owning_thread, irp);

    if (!irp)
        return STATUS_INVALID_PARAMETER;

    if (!(context = heap_alloc(sizeof(*context))))
    {
        ERR("No memory.\n");
        status = STATUS_NO_MEMORY;
        dispatch_irp(irp, status);
        return status;
    }

    context->node_name = node_name;
    context->service_name = service_name;
    context->namespace = name_space;
    context->provider = provider;
    context->hints = hints;
    context->result = result;
    context->irp = irp;

    if (!TrySubmitThreadpoolCallback(get_address_info_callback, context, NULL))
    {
        ERR("Could not submit thread pool callback.\n");
        status = STATUS_UNSUCCESSFUL;
        dispatch_irp(irp, status);
        heap_free(context);
        return status;
    }
    TRACE("Submitted threadpool callback, context %p.\n", context);
    return STATUS_PENDING;
}

static void WINAPI wsk_free_address_info(WSK_CLIENT *client, ADDRINFOEXW *addr_info)
{
    TRACE("client %p, addr_info %p.\n", client, addr_info);

    FreeAddrInfoExW(addr_info);
}

static NTSTATUS WINAPI wsk_get_name_info(WSK_CLIENT *client, SOCKADDR *sock_addr, ULONG sock_addr_length,
        UNICODE_STRING *node_name, UNICODE_STRING *service_name, ULONG flags, PEPROCESS owning_process,
        PETHREAD owning_thread, IRP *irp)
{
    FIXME("client %p, sock_addr %p, sock_addr_length %u, node_name %p, service_name %p, "
            "flags %#x, owning_process %p, owning_thread %p, irp %p stub.\n",
            client, sock_addr, sock_addr_length, node_name, service_name, flags,
            owning_process, owning_thread, irp);

    return STATUS_NOT_IMPLEMENTED;
}

static const WSK_PROVIDER_DISPATCH wsk_dispatch =
{
    MAKE_WSK_VERSION(1, 0), 0,
    wsk_socket,
    wsk_socket_connect,
    wsk_control_client,
    wsk_get_address_info,
    wsk_free_address_info,
    wsk_get_name_info,
};

NTSTATUS WINAPI WskCaptureProviderNPI(WSK_REGISTRATION *wsk_registration, ULONG wait_timeout,
        WSK_PROVIDER_NPI *wsk_provider_npi)
{
    WSK_CLIENT *client = wsk_registration->ReservedRegistrationContext;

    TRACE("wsk_registration %p, wait_timeout %u, wsk_provider_npi %p.\n",
            wsk_registration, wait_timeout, wsk_provider_npi);

    wsk_provider_npi->Client = client;
    wsk_provider_npi->Dispatch = &wsk_dispatch;
    return STATUS_SUCCESS;
}

void WINAPI WskReleaseProviderNPI(WSK_REGISTRATION *wsk_registration)
{
    TRACE("wsk_registration %p.\n", wsk_registration);

}

NTSTATUS WINAPI WskRegister(WSK_CLIENT_NPI *wsk_client_npi, WSK_REGISTRATION *wsk_registration)
{
    static const WORD version = MAKEWORD( 2, 2 );
    WSADATA data;

    WSK_CLIENT *client;

    TRACE("wsk_client_npi %p, wsk_registration %p.\n", wsk_client_npi, wsk_registration);

    if (!(client = heap_alloc(sizeof(*client))))
    {
        ERR("No memory.\n");
        return STATUS_NO_MEMORY;
    }

    client->registration = wsk_registration;
    client->client_npi = wsk_client_npi;
    wsk_registration->ReservedRegistrationContext = client;

    if (WSAStartup(version, &data))
        return STATUS_INTERNAL_ERROR;

    return STATUS_SUCCESS;
}

void WINAPI WskDeregister(WSK_REGISTRATION *wsk_registration)
{
    TRACE("wsk_registration %p.\n", wsk_registration);

    heap_free(wsk_registration->ReservedRegistrationContext);
}

static void WINAPI driver_unload(DRIVER_OBJECT *driver)
{
    TRACE("driver %p.\n", driver);
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    TRACE("driver %p, path %s.\n", driver, debugstr_w(path->Buffer));

    driver->DriverUnload = driver_unload;
    return STATUS_SUCCESS;
}
