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

#include "fwd_entry_stream_socket.h"

#include "fwd_log_p.h"
#include "fwd_op_call.h"
#include "fwd_op_connect_local.h"
#include "fwd_peer_p.h"
#include "fwd_protocol.h"
#include "fwd_socket_client.h"
#include "fwd_socket_io.h"
#include "fwd_socket_service.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <errno.h>

typedef struct fwd_entry_stream_socket_impl {
    FwdEntryStreamSocket ss;
    FwdSocketIoHandler io_handler;
    FwdSocketIo* io;
} FwdEntryStreamSocketImpl;

static inline FwdEntryStreamSocketImpl*
fwd_entry_stream_socket_impl_cast(FwdEntryStreamSocket* ss)
    { return G_CAST(ss, FwdEntryStreamSocketImpl, ss); }

static inline FwdEntryStreamSocketImpl*
fwd_entry_stream_socket_entry_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryStreamSocketImpl, ss.entry); }

static inline FwdEntryStreamSocketImpl*
fwd_entry_stream_socket_io_handler_cast(FwdSocketIoHandler* h)
    { return G_CAST(h, FwdEntryStreamSocketImpl, io_handler); }

static
void
fwd_entry_stream_socket_io_remote_data_sent(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    guint nbytes, /* Number of bytes sent (zero on error) */
    void* user_data)
{
    FwdOp* op = user_data;

    /* The call id has been zeroed by fwd_entry_stream_socket_io_receive */
    GASSERT(!fwd_op_call_cast(op)->call_id);
    if (result) {
        FwdEntry* entry = op->entry;

        /*
         * The associated entry may already be gone if this is one of
         * the data buffers delivered after the connection has already
         * been terminated.
         */
        if (entry) {
            GDEBUG("[%u] Data send error %d", entry->id, result);
            fwd_entry_remove(entry);
        }
    }
}

static
void
fwd_entry_stream_socket_connect_ok(
    GIoRpcRequest* req,
    FwdEntryStreamSocket* ss)
{
    fwd_socket_service_connect_ok(req);
}

static
void
fwd_entry_stream_socket_connect_error(
    GIoRpcRequest* req,
    FwdEntryStreamSocket* ss,
    guint error)
{
    fwd_socket_service_connect_error(req, error);
}

static
void
fwd_entry_stream_socket_local_data_sent(
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

/*==========================================================================*
 * FwdEntry
 *==========================================================================*/

static
guint
fwd_entry_stream_socket_accepted(
   FwdEntry* entry,
   guint rid)
{
    FwdEntryStreamSocket* ss = fwd_entry_stream_socket_cast(entry);

    if (rid && !ss->rid) {
        ss->rid = rid;
        fwd_entry_stream_socket_connected(ss);
        return FWD_SOCKET_RESULT_OK;
    }
    return EFAULT;
}

static
void
fwd_entry_stream_socket_connect(
    FwdEntry* entry,
    GIoRpcRequest* req,
    GInetSocketAddress* to)
{
    static const FwdOpConnectLocalCallbacks connect_cb = {
        fwd_entry_stream_socket_connect_ok,
        fwd_entry_stream_socket_connect_error
    };
    fwd_op_connect_local_start(fwd_entry_stream_socket_cast(entry),
        to, req, &connect_cb);
}

static
void
fwd_entry_stream_socket_data(
    FwdEntry* entry,
    GIoRpcRequest* req,
    const GUtilData* data,
    GInetSocketAddress* from)
{
    FwdEntryStreamSocketImpl* self =
        fwd_entry_stream_socket_entry_cast(entry);

    GASSERT(self->io);
    if (self->io) {
        GBytes* bytes = g_bytes_new(data->bytes, data->size);

        GDEBUG("[%u] Received %u byte(s) from remote", entry->id,
            (guint) data->size);
        fwd_socket_io_send(self->io, NULL, bytes,
            fwd_entry_stream_socket_local_data_sent,
            giorpc_request_ref(req), (GDestroyNotify)
            giorpc_request_unref);
        g_bytes_unref(bytes);
    }
}

static
void
fwd_entry_stream_socket_close(
    FwdEntry* entry)
{
    FwdEntryStreamSocket* ss =
        &fwd_entry_stream_socket_entry_cast(entry)->ss;

    if (ss->rid) {
        GDEBUG("[%u] Remote socket %u is gone", entry->id, ss->rid);
        ss->rid = 0;
    }
    fwd_entry_remove(entry);
}

static
void
fwd_entry_stream_socket_dispose(
    FwdEntry* entry)
{
    FwdEntryStreamSocketImpl* self =
        fwd_entry_stream_socket_entry_cast(entry);
    FwdEntryStreamSocket* ss = &self->ss;

    if (ss->rid) {
        GDEBUG("[%u] Closing remote socket %u", entry->id, ss->rid);
        fwd_socket_call_close(fwd_entry_rpc(entry), ss->rid);
        ss->rid = 0;
    }
    fwd_socket_io_clear(&self->io);
    if (ss->socket) {
        g_socket_close(ss->socket, NULL);
    }
    fwd_entry_base_dispose(entry);
}

static
void
fwd_entry_stream_socket_free(
    FwdEntry* entry)
{
    FwdEntryStreamSocketImpl* self =
        fwd_entry_stream_socket_entry_cast(entry);
    FwdEntryStreamSocket* ss = &self->ss;

    fwd_socket_io_free(self->io);
    gutil_object_unref(ss->socket);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * FwdSocketIoHandler
 *==========================================================================*/

static
void
fwd_entry_stream_socket_io_error(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    FwdEntryStreamSocketImpl* self =
        fwd_entry_stream_socket_io_handler_cast(handler);
    FwdEntry* entry = &self->ss.entry;

    GDEBUG("[%u] %s", entry->id, error ? error->message : "Connection closed");
    fwd_entry_remove(entry);
}

static
void
fwd_entry_stream_socket_io_receive(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    FwdEntryStreamSocketImpl* self =
        fwd_entry_stream_socket_io_handler_cast(handler);
    FwdEntryStreamSocket* ss = &self->ss;
    FwdEntry* entry = &ss->entry;
    FwdOpCall* call = fwd_op_call_new(entry);

    GDEBUG("[%u] Received %u bytes from socket", entry->id, (uint) data->size);
    fwd_op_call_start(call, fwd_socket_call_data(call->rpc,
        ss->rid, data, from, fwd_entry_stream_socket_io_remote_data_sent,
        &call->op, fwd_op_dispose_unref_cb));
    /*
     * Make the op non-cancelable, to make sure that we actually forward
     * all the data that we have received from the local socket.
     */
    call->call_id = 0;
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdEntryStreamSocket*
fwd_entry_stream_socket_new(
    FwdPeer* owner,
    GSocket* socket,
    guint rid)
{
    static const FwdEntryType stream_socket_type = {
        NULL, /* start */
        NULL, /* accept */
        fwd_entry_stream_socket_accepted,
        fwd_entry_stream_socket_connect,
        fwd_entry_stream_socket_data,
        fwd_entry_stream_socket_close,
        fwd_entry_stream_socket_dispose,
        fwd_entry_stream_socket_free
    };
    static const FwdSocketIoCallbacks stream_socket_io = {
        fwd_entry_stream_socket_io_error,
        fwd_entry_stream_socket_io_receive
    };
    FwdEntryStreamSocketImpl* self =
        g_slice_new0(FwdEntryStreamSocketImpl);
    FwdEntryStreamSocket* ss = &self->ss;
    FwdEntry* entry = &ss->entry;

    fwd_entry_base_init(entry, &stream_socket_type, owner,
        FWD_SOCKET_STATE_INIT);
    g_object_ref(ss->socket = socket);
    self->io_handler.cb = &stream_socket_io;
    ss->rid = rid;
    return ss;
}

void
fwd_entry_stream_socket_start_io(
    FwdEntryStreamSocket* ss)
{
    FwdEntryStreamSocketImpl* self =
        fwd_entry_stream_socket_impl_cast(ss);

    GASSERT(!self->io);
    self->io = fwd_socket_io_new(ss->socket,
        fwd_entry_rpc(&ss->entry)->context, &self->io_handler);
}

void
fwd_entry_stream_socket_connected(
    FwdEntryStreamSocket* ss)
{
    FwdEntryStreamSocketImpl* self =
        fwd_entry_stream_socket_impl_cast(ss);

    if (!self->io) {
        self->io = fwd_socket_io_new(ss->socket,
            fwd_entry_rpc(&ss->entry)->context, &self->io_handler);
    }
    fwd_socket_io_start_receive(self->io);
    fwd_entry_set_state(&ss->entry, FWD_SOCKET_STATE_CONNECTED);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
