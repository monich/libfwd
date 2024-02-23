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

#include "fwd_op_connect_local.h"

#include "fwd_entry_stream_socket.h"
#include "fwd_log_p.h"
#include "fwd_op.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <errno.h>

typedef struct fwd_op_connect_local {
    FwdOp op;
    GInetSocketAddress* to;
    GCancellable* cancel;
    GIoRpcRequest* req;
    const FwdOpConnectLocalCallbacks* cb;
} FwdOpConnectLocal;

static inline FwdOpConnectLocal*
fwd_op_connect_local_cast(FwdOp* op)
    { return G_CAST(op, FwdOpConnectLocal, op); }

static
void
fwd_op_connect_local_done(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    FwdOp* op = user_data;
    FwdOpConnectLocal* connect = fwd_op_connect_local_cast(op);
    const FwdOpConnectLocalCallbacks* cb = connect->cb;;
    FwdEntry* entry = op->entry;
    FwdEntryStreamSocket* ss = fwd_entry_stream_socket_cast(entry);
    GSocketConnection* sc = G_SOCKET_CONNECTION(object);
    GError* error = NULL;

    if (g_socket_connection_connect_finish(sc, result, &error)) {
        if (entry) {
#if GUTIL_LOG_DEBUG
            if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                GDEBUG("[%u] Connected to %s", entry->id,
                    fwd_format_connection_remote_address(sc));
            }
#endif /* GUTIL_LOG_DEBUG */

            /* Start receiving the data */
            fwd_entry_stream_socket_connected(ss);

            /* Success */
            cb->ok(connect->req, ss);
        }
    } else {
        if (entry) {
#if GUTIL_LOG_DEBUG
            if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                GDEBUG("[%u] Connection to %s failed: %s", entry->id,
                    fwd_format_inet_socket_address(connect->to),
                    GERRMSG(error));
            }
#endif /* GUTIL_LOG_DEBUG */

            /* Failure */
            cb->err(connect->req, ss, fwd_error_code(error, EFAULT));
        }
        g_error_free(error);
    }
    giorpc_request_unref(connect->req);
    connect->req = NULL;
    if (connect->cancel) {
        g_object_unref(connect->cancel);
        connect->cancel = NULL;
    }
    fwd_op_dispose(op);
    fwd_op_unref(op);
}

static
void
fwd_op_connect_local_dispose(
    FwdOp* op)
{
    FwdOpConnectLocal* connect = fwd_op_connect_local_cast(op);

    fwd_op_default_dispose(op);
    if (connect->cancel) {
        g_cancellable_cancel(connect->cancel);
        g_object_unref(connect->cancel);
    }
    if (connect->req) {
        giorpc_request_unref(connect->req);
        connect->req = NULL;
    }
}

static
void
fwd_op_connect_local_destroy(
    FwdOp* op)
{
    FwdOpConnectLocal* connect = fwd_op_connect_local_cast(op);

    g_object_unref(connect->to);
    gutil_object_unref(connect->cancel);
    giorpc_request_unref(connect->req);
    gutil_slice_free(connect);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

void
fwd_op_connect_local_start(
    FwdEntryStreamSocket* ss,
    GInetSocketAddress* to,
    GIoRpcRequest* req,
    const FwdOpConnectLocalCallbacks* cb)
{
    static const FwdOpType local_connect_op = {
        fwd_op_connect_local_dispose,
        fwd_op_connect_local_destroy
    };
    FwdOpConnectLocal* connect = g_slice_new0(FwdOpConnectLocal);
    FwdEntry* entry = &ss->entry;
    FwdOp* op = &connect->op;
    GSocketConnection* connection =
        g_socket_connection_factory_create_connection(ss->socket);

#if GUTIL_LOG_DEBUG
    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
        GDEBUG("[%u] Connecting to %s", entry->id,
            fwd_format_inet_socket_address(to));
    }
#endif /* GUTIL_LOG_DEBUG */

    fwd_op_init(op, &local_connect_op, entry);
    g_object_ref(connect->to = to);
    connect->cancel = g_cancellable_new();
    connect->req = giorpc_request_ref(req);
    connect->cb = cb;
    g_socket_connection_connect_async(connection, G_SOCKET_ADDRESS(to),
        connect->cancel, fwd_op_connect_local_done, fwd_op_ref(op));
    g_object_unref(connection);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
