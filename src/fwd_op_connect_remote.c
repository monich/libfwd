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

#include "fwd_op_connect_remote.h"

#include "fwd_control_client.h"
#include "fwd_entry.h"
#include "fwd_entry_stream_socket.h"
#include "fwd_log_p.h"
#include "fwd_op.h"
#include "fwd_peer_p.h"
#include "fwd_socket_client.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

typedef struct fwd_op_connect_remote {
    FwdOp op;
    GIoRpcPeer* rpc;
    GSocket* socket;
    GInetSocketAddress* to;
    guint rid;
    guint call_id;
} FwdOpConnectRemote;

static inline FwdOpConnectRemote*
fwd_op_connect_remote_cast(FwdOp* op)
    { return G_CAST(op, FwdOpConnectRemote, op); }

static
void
fwd_op_connect_remote_done(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    void* user_data)
{
    FwdOpConnectRemote* connect = user_data;
    FwdOp* op = &connect->op;
    FwdEntry* entry = op->entry;

    connect->call_id = 0;
    if (result) {
        GDEBUG("[%u] Remote connection failed (%d)", entry->id, result);
        fwd_entry_remove(entry);
    } else {
        FwdEntryStreamSocket* ss = fwd_entry_stream_socket_cast(entry);

        /* Start receiving the data */
        GDEBUG("[%u] Remote connection established", entry->id);
        fwd_entry_stream_socket_connected(ss);
    }
}

static
void
fwd_op_connect_remote_socket_created(
    int rid, /* Socket id > 0, error < 0, RPC error = 0 */
    GInetSocketAddress* sa,
    void* user_data)
{
    FwdOpConnectRemote* rc = user_data;
    FwdOp* op = &rc->op;
    FwdEntry* entry = op->entry;
    FwdEntryStreamSocket* ss = fwd_entry_stream_socket_cast(entry);

    if (rid > 0) {
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] %s <=> [%d] %s", entry->id,
                fwd_format_socket_local_address(rc->socket), rid,
                fwd_format_inet_socket_address(sa));
        }
#endif /* GUTIL_LOG_DEBUG */

        /* Store the remote socket id */
        ss->rid = rid;

        /* And try to connect it */
        rc->call_id = fwd_socket_call_connect(rc->rpc, rid, rc->to,
            fwd_op_connect_remote_done, rc, NULL);
    } else {
        GDEBUG("[%u] Error %d creating remote socket", entry->id, -rid);
        rc->call_id = 0;
        fwd_entry_remove(entry);
    }
}

static
void
fwd_op_connect_remote_destroy(
    FwdOp* op)
{
    FwdOpConnectRemote* rc = fwd_op_connect_remote_cast(op);

    giorpc_peer_cancel(rc->rpc, rc->call_id);
    giorpc_peer_unref(rc->rpc);
    g_object_unref(rc->socket);
    g_object_unref(rc->to);
    gutil_slice_free(rc);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

void
fwd_op_connect_remote_start(
    FwdEntry* entry,
    GSocket* s,
    GInetSocketAddress* to)
{
    static const FwdOpType remote_connect_op = {
        fwd_op_default_dispose,
        fwd_op_connect_remote_destroy
    };
    FwdPeer* owner = entry->owner;
    FwdEntryStreamSocket* ss = fwd_entry_stream_socket_new(owner, s, 0);
    FwdOpConnectRemote* rc = g_slice_new0(FwdOpConnectRemote);
    FwdEntry* new_entry = &ss->entry;
    FwdOp* op = &rc->op;

    fwd_op_init(op, &remote_connect_op, new_entry);
    g_object_ref(rc->socket = s);
    g_object_ref(rc->to = to);
    rc->rpc = fwd_peer_rpc_ref(owner);
    rc->call_id = fwd_control_call_socket(rc->rpc, new_entry->id,
        g_socket_get_family(s), g_socket_get_socket_type(s), NULL,
        FWD_SOCKET_NO_FLAGS, 0, 0, 0, fwd_op_connect_remote_socket_created,
        rc, NULL);

    /* Start watching socket errors right away */
    fwd_entry_stream_socket_start_io(ss);

    /* Ownership of the new entry is transferred to FwdPeer */
    fwd_entry_insert(new_entry);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
