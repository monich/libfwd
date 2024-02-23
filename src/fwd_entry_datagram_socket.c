/*
 * Copyright (C) 2023-2025 Slava Monich <slava@monich.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the
 *     distribution.
 *
 *  3. Neither the names of the copyright holders nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#include "fwd_entry_datagram_socket.h"

#include "fwd_log_p.h"
#include "fwd_op_call.h"
#include "fwd_peer_p.h"
#include "fwd_socket_client.h"
#include "fwd_socket_io.h"
#include "fwd_socket_service.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <errno.h>

typedef struct fwd_entry_datagram_socket {
    FwdEntry entry;
    FwdSocketIoHandler io_handler;
    FwdSocketIo* io;
    GInetSocketAddress* to;
    GSocket* socket;
    guint rid;
} FwdEntryDatagramSocket;

static inline FwdEntryDatagramSocket*
fwd_entry_datagram_socket_entry_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryDatagramSocket, entry); }

static inline FwdEntryDatagramSocket*
fwd_entry_datagram_socket_io_handler_cast(FwdSocketIoHandler* h)
    { return G_CAST(h, FwdEntryDatagramSocket, io_handler); }

static
void
fwd_entry_datagram_socket_local_data_sent(
    gsize sent,
    const GError* error,
    void* user_data)
{
    GIoRpcRequest* req = user_data;

    if (error) {
        fwd_socket_service_data_error(req, fwd_error_code(error, EFAULT));
    } else {
        fwd_socket_service_data_ok(req, sent);
    }
}

static
void
fwd_entry_datagram_socket_io_remote_data_sent(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    guint nbytes, /* Number of bytes sent (zero on error) */
    void* user_data)
{
    FwdOp* op = user_data;

    fwd_op_call_cast(op)->call_id = 0;
    if (result) {
        FwdEntry* entry = op->entry;

        GDEBUG("[%u] Data send error %d", entry->id, result);
        fwd_entry_remove(entry);
    }
}

/*==========================================================================*
 * FwdSocketIoHandler
 *==========================================================================*/

static
void
fwd_entry_datagram_socket_io_error(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_io_handler_cast(handler);
    FwdEntry* entry = &self->entry;

    GDEBUG("[%u] I/O error", entry->id);
    fwd_entry_remove(entry);
}

static
void
fwd_entry_datagram_socket_io_receive(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_io_handler_cast(handler);
    FwdEntry* entry = &self->entry;
    FwdOpCall* call = fwd_op_call_new(entry);

    GDEBUG("[%u] Received %u bytes from socket", entry->id, (uint) data->size);
    fwd_op_call_start(call, fwd_socket_call_data(call->rpc, self->rid,
        data, from, fwd_entry_datagram_socket_io_remote_data_sent,
        &call->op, fwd_op_dispose_unref_cb));
}

/*==========================================================================*
 * FwdEntry
 *==========================================================================*/

static
void
fwd_entry_datagram_socket_start(
    FwdEntry* entry)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_entry_cast(entry);

    fwd_socket_io_start_receive(self->io);
}

static
void
fwd_entry_datagram_socket_connect(
    FwdEntry* entry,
    GIoRpcRequest* req,
    GInetSocketAddress* to)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_entry_cast(entry);

#if GUTIL_LOG_DEBUG
    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
        GDEBUG("[%u] Connected to %s", entry->id,
            fwd_format_inet_socket_address(to));
    }
#endif /* GUTIL_LOG_DEBUG */
    gutil_object_unref(self->to);
    g_object_ref(self->to = to);
    fwd_socket_service_connect_ok(req);
}

static
void
fwd_entry_datagram_socket_data(
    FwdEntry* entry,
    GIoRpcRequest* req,
    const GUtilData* data,
    GInetSocketAddress* from)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_entry_cast(entry);

    GASSERT(self->io);
    GASSERT(self->to);
    if (self->io && self->to) {
        GBytes* bytes = g_bytes_new(data->bytes, data->size);

        GDEBUG("[%u] Received %u byte(s) from remote", entry->id,
            (guint) data->size);
        fwd_socket_io_send(self->io, self->to, bytes,
            fwd_entry_datagram_socket_local_data_sent,
            giorpc_request_ref(req), (GDestroyNotify)
            giorpc_request_unref);
        g_bytes_unref(bytes);
    } else {
        GDEBUG("[%u] Unexpected %u byte(s) from remote", entry->id,
            (guint) data->size);
        fwd_socket_service_data_error(req, ENOTCONN);
    }
}

static
void
fwd_entry_datagram_socket_close(
    FwdEntry* entry)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_entry_cast(entry);

    if (self->rid) {
        GDEBUG("[%u] Remote socket %u is gone", entry->id, self->rid);
        self->rid = 0;
    }
    if (self->to) {
        g_object_unref(self->to);
        self->to = NULL;
    }
    fwd_socket_io_clear(&self->io);
    fwd_entry_remove(entry);
}

static
void
fwd_entry_datagram_socket_dispose(
    FwdEntry* entry)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_entry_cast(entry);

    if (self->rid) {
        GDEBUG("[%u] Closing remote socket %u", entry->id, self->rid);
        fwd_socket_call_close(fwd_entry_rpc(entry), self->rid);
        self->rid = 0;
    }
    fwd_socket_io_clear(&self->io);
    fwd_entry_base_dispose(entry);
}

static
void
fwd_entry_datagram_socket_free(
    FwdEntry* entry)
{
    FwdEntryDatagramSocket* self =
        fwd_entry_datagram_socket_entry_cast(entry);

    fwd_socket_io_free(self->io);
    gutil_object_unref(self->to);
    gutil_object_unref(self->socket);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdEntry*
fwd_entry_datagram_socket_new(
    FwdPeer* owner,
    GSocket* socket,
    GInetSocketAddress* to,
    guint rid)
{
    static const FwdEntryType datagram_socket_type = {
        fwd_entry_datagram_socket_start,
        NULL, /* accept */
        NULL, /* accepted */
        fwd_entry_datagram_socket_connect,
        fwd_entry_datagram_socket_data,
        fwd_entry_datagram_socket_close,
        fwd_entry_datagram_socket_dispose,
        fwd_entry_datagram_socket_free
    };
    static const FwdSocketIoCallbacks datagram_socket_io = {
        fwd_entry_datagram_socket_io_error,
        fwd_entry_datagram_socket_io_receive
    };
    FwdEntryDatagramSocket* self = g_slice_new0(FwdEntryDatagramSocket);
    FwdEntry* entry = &self->entry;

    fwd_entry_base_init(entry, &datagram_socket_type, owner,
        FWD_SOCKET_STATE_INIT);
    g_object_ref(self->socket = socket);
    if (to) {
        g_object_ref(self->to = to);
    }
    self->rid = rid;
    self->io_handler.cb = &datagram_socket_io;
    self->io = fwd_socket_io_new(socket, fwd_peer_rpc(owner)->context,
        &self->io_handler);
    return entry;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
