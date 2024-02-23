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

#include "fwd_entry_datagram_forward_remote.h"

#include "fwd_control_client.h"
#include "fwd_entry.h"
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

/*
 * Matching local UDP replies to the remote datagrams:
 *
 *             local               |             remote
 *             =====               |             ======
 *                               giorpc
 *            network              |
 *               ^                 |
 *               |                 |
 *               v                 |
 *          +---------+            |
 *          | GSocket |            |         +--------------------+
 *       +----------------+        |       +--------------------+ |
 *       | DatagramSocket | <--- DATA ---> | DatagramConnection |-+
 *       +----------------+        |       +--------------------+  \
 *                         \       |       ^      ^  ^           \  \
 *                          \   ACCEPTED  /       |  |            \  \
 *                           +-----------+        |  |             \  \
 *                          /      |              |  |              v  v
 * +=======================+       |       +----------------+     +---------+
 * | DatagramForwardRemote | <------------ | DatagramListen | <-- | GSocket |
 * +=======================+ ------------> +----------------+     +---------+
 *                               ACCEPT                               ^
 *                                 |                                  |
 *                                 |                                  v
 *                                 |                               network
 *
 * DatagramListen is receiving datagrams from the network on the remote side,
 * and either uses the existing DatagramConnection to forward the data to the
 * coresponding DatagramSocket via the RPC connection, or completes a pending
 * ACCEPT when the data arrives from a previously unknown remote adress (i.e.
 * the host/port pair).
 *
 * On each completed ACCEPT from DatagramListen, DatagramForwardRemote
 * creates a new DatagramSocket with its own GSocket (and therefore
 * unique port number), returns its id via the ACCEPTED call to its
 * counterpart DatagramConnection. That DatagramConnection knows where
 * to forward the datagrams arriving from the DatagramSocket on the left
 * (local) side of the RPC connection and can forward that data directly
 * to GSocket which it shares with DatagramListen which completed the
 * original ACCEPT.
 *
 * The DatagramListen object is responsible for cleaning up the connection
 * entries after the inactivity period* expires. If no inactivity period is
 * provided with the ACCEPTED call, the default value will be used. When
 * DatagramConnection is destroyed due to inactivity, it closes and destroyes
 * the counterpart DatagramSocket object on the local side as well.
 */

typedef struct fwd_entry_datagram_forward_remote_accept {
    guint call_id;
    struct fwd_entry_datagram_forward_remote* self;
} FwdEntryDatagramForwardRemoteAccept;

typedef struct fwd_entry_datagram_forward_remote {
    FwdEntry entry;
    gboolean allow_reuse;
    guint backlog;
    guint conn_timeout_ms;
    guint conn_limit;
    GInetSocketAddress* remote;
    GInetSocketAddress* to;
    GSocketAddress* any;
    GSocketFamily af;
    guint rid;
    FwdEntryDatagramForwardRemoteAccept accept[2];
} FwdEntryDatagramForwardRemote;

static inline FwdEntryDatagramForwardRemote*
fwd_entry_datagram_forward_remote_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryDatagramForwardRemote, entry); }

static
GSocket*
fwd_entry_datagram_forward_remote_socket_new(
    FwdEntryDatagramForwardRemote* self)
{
    GError* error = NULL;
    GSocket* s = g_socket_new(self->af, G_SOCKET_TYPE_DATAGRAM,
        G_SOCKET_PROTOCOL_DEFAULT, &error);

    if (s) {
        if (g_socket_bind(s, self->any, FALSE, &error)) {
            g_socket_set_blocking(s, FALSE);
            return s;
        }
        GERR("[%u] Bind error: %s", self->entry.id, GERRMSG(error));
        g_object_unref(s);
    } else {
        GERR("[%u] Socket creation error: %s", self->entry.id, GERRMSG(error));
    }
    g_error_free(error);
    return NULL;
}

static
void
fwd_entry_datagram_forward_remote_accept_complete(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    guint rid,  /* Remote socket's id (zero on error) */
    const GUtilData* src_sa, /* struct sockaddr (NULL on error) */
    const GUtilData* dest_sa, /* struct sockaddr (NULL on error) */
    void* user_data)
{
    FwdEntryDatagramForwardRemoteAccept* accept = user_data;
    FwdEntryDatagramForwardRemote* self = accept->self;
    FwdEntry* entry = &self->entry;

    if (rid) {
        GIoRpcPeer* rpc = fwd_entry_rpc(entry);
        GInetSocketAddress* src = fwd_inet_socket_address_from_data(src_sa);

        if (src) {
            GSocket* s = fwd_entry_datagram_forward_remote_socket_new(self);

            if (s) {
                FwdPeer* owner = entry->owner;
                FwdEntry* se = fwd_entry_datagram_socket_new(owner, s,
                    self->to, rid);

                /* Transfer ownership of the new entry to FwdPeer */
                fwd_entry_set_state(se, FWD_SOCKET_STATE_CONNECTED);
                fwd_entry_insert(se);
                g_object_unref(s);
#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    GDEBUG("[%u] Accepting connection %u <= %u %s", entry->id,
                        se->id, rid, fwd_format_sockaddr_data(src_sa));
                }
#endif /* GUTIL_LOG_DEBUG */
                fwd_socket_call_accepted(rpc, rid, se->id, NULL, NULL);
            } else {
                fwd_socket_call_close(rpc, rid);
            }
            g_object_unref(src);
        } else {
            /* Source address isn't really required... but */
            GWARN("[%u] Bad source in the ACCEPT response", entry->id);
            fwd_socket_call_close(rpc, rid);
        }

        /* Submit the next one */
        accept->call_id = fwd_socket_call_accept(rpc, self->rid,
            fwd_entry_datagram_forward_remote_accept_complete,
            accept, NULL);
    } else {
        accept->call_id = 0;
        fwd_entry_set_state(entry, FWD_SOCKET_STATE_LISTEN_REMOTE_ERROR);
    }
}
static
void
fwd_entry_datagram_forward_remote_socket_complete(
    int rid,  /* Socket id > 0, error < 0, RPC error = 0 */
    GInetSocketAddress* address,
    void* user_data)
{
    FwdOp* op = user_data;
    FwdEntry* entry = op->entry;

    fwd_op_call_cast(op)->call_id = 0;
    if (rid > 0) {
        int i;
        GIoRpcPeer* rpc = fwd_entry_rpc(entry);
        FwdEntryDatagramForwardRemote* self =
            fwd_entry_datagram_forward_remote_cast(entry);

        self->rid = rid;
        GDEBUG("[%u] Remote datagram socket %u", entry->id, rid);

        /* Submit ACCEPTs */
        for (i = 0; i < G_N_ELEMENTS(self->accept); i++) {
            FwdEntryDatagramForwardRemoteAccept* accept = self->accept + i;

            accept->call_id = fwd_socket_call_accept(rpc, rid,
                fwd_entry_datagram_forward_remote_accept_complete,
                accept, NULL);
        }
        fwd_entry_set_state(entry, FWD_SOCKET_STATE_LISTEN_REMOTE);
    } else {
        fwd_entry_set_state(entry, FWD_SOCKET_STATE_LISTEN_REMOTE_ERROR);
    }
}

/*==========================================================================*
 * FwdEntryType
 *==========================================================================*/

static
void
fwd_entry_datagram_forward_remote_start(
    FwdEntry* entry)
{
    FwdEntryDatagramForwardRemote* self =
        fwd_entry_datagram_forward_remote_cast(entry);
    FwdOpCall* call = fwd_op_call_new(entry);
    FWD_SOCKET_FLAGS flags = FWD_SOCKET_FLAG_LISTEN;

    if (self->allow_reuse) {
        flags |= FWD_SOCKET_FLAG_REUSADDR;
    }

    call = fwd_op_call_new(entry);
    fwd_op_call_start(call, fwd_control_call_socket(call->rpc,
        entry->id, self->af, G_SOCKET_TYPE_DATAGRAM, self->remote, flags,
        self->backlog, self->conn_timeout_ms, self->conn_limit,
        fwd_entry_datagram_forward_remote_socket_complete,
        &call->op, fwd_op_dispose_unref_cb));
}

static
void
fwd_entry_datagram_forward_remote_dispose(
    FwdEntry* entry)
{
    int i;
    GIoRpcPeer* rpc = fwd_entry_rpc(entry);
    FwdEntryDatagramForwardRemote* self =
        fwd_entry_datagram_forward_remote_cast(entry);

    /* Cancel pending ACCEPTs */
    for (i = 0; i < G_N_ELEMENTS(self->accept); i++) {
        FwdEntryDatagramForwardRemoteAccept* accept = self->accept + i;

        giorpc_peer_cancel(rpc, accept->call_id);
        accept->call_id = 0;
    }
    fwd_socket_call_close(rpc, self->rid);
    fwd_entry_base_dispose(entry);
}

static
void
fwd_entry_datagram_forward_remote_free(
    FwdEntry* entry)
{
    FwdEntryDatagramForwardRemote* self =
        fwd_entry_datagram_forward_remote_cast(entry);

    g_object_unref(self->remote);
    g_object_unref(self->to);
    g_object_unref(self->any);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdEntry*
fwd_entry_datagram_forward_remote_new(
    FwdPeer* owner,
    GInetSocketAddress* remote,
    GInetSocketAddress* to,
    gboolean allow_reuse,
    guint backlog,
    guint timeout_ms,
    guint conn_limit,
    GError** error)
{
    static const FwdEntryType datagram_forward_remote_entry_type = {
        fwd_entry_datagram_forward_remote_start,
        NULL, /* listen */
        NULL, /* accept */
        NULL, /* connect */
        NULL, /* data */
        NULL, /* close */
        fwd_entry_datagram_forward_remote_dispose,
        fwd_entry_datagram_forward_remote_free
    };

    FwdEntryDatagramForwardRemote* self =
        g_slice_new0(FwdEntryDatagramForwardRemote);
    FwdEntry* entry = &self->entry;
    GInetAddress* ia = g_inet_socket_address_get_address(to);
    GSocketFamily af = g_inet_address_get_family(ia);
    GInetAddress* any = g_inet_address_new_any(af);
    int i;

    fwd_entry_base_init(entry, &datagram_forward_remote_entry_type, owner,
        FWD_SOCKET_STATE_LISTEN_REMOTE_STARTING);

    self->conn_timeout_ms = (timeout_ms == FWD_TIMEOUT_DEFAULT) ?
        DEFAULT_CONN_TIMEOUT_MS : timeout_ms;
    self->backlog = backlog;
    self->conn_limit = conn_limit;
    self->allow_reuse = allow_reuse;
    g_object_ref(self->to = to);
    g_object_ref(self->remote = remote);
    self->af = af;
    self->any = g_inet_socket_address_new(any, 0);
    for (i = 0; i < G_N_ELEMENTS(self->accept); i++) {
        self->accept[i].self = self;
    }
    g_object_unref(any);
    return entry;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
