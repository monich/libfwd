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

#ifndef FWD_PEER_H
#define FWD_PEER_H

#include "fwd_types.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum fwd_flags {
    FWD_FLAGS_NONE = 0,
    FWD_FLAG_ALLOW_REMOTE = 0x01,
    FWD_FLAG_REUSE_ADDRESS = 0x02
} FWD_FLAGS;

#define FWD_BACKLOG_DEFAULT (-1)
#define FWD_TIMEOUT_DEFAULT (0)
#define FWD_LIMIT_DEFAULT (0)

struct fwd_peer {
    FWD_STATE state;
};

typedef
void
(*FwdPeerFunc)(
    FwdPeer* fp,
    void* user_data);

typedef
void
(*FwdSocketFunc)(
    FwdPeer* fp,
    guint id,
    void* user_data);

typedef
void
(*FwdSocketStateFunc)(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data);

FwdPeer*
fwd_peer_new(
    GIOStream* stream,
    GMainContext* context);

struct giorpc_peer; /* GIoRpcPeer */
FwdPeer*
fwd_peer_new_with_rpc(
    struct giorpc_peer* rpc);

FwdPeer*
fwd_peer_ref(
    FwdPeer* fp);

void
fwd_peer_unref(
    FwdPeer* fp);

gboolean
fwd_peer_sync(
    FwdPeer* fp,
    guint timeout_ms,
    GError** error);

guint
fwd_peer_add_local_datagram_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* remote,
    guint inactivity_timeout_ms,
    guint conn_limit,
    FWD_FLAGS flags,
    GError** error);

guint
fwd_peer_add_local_stream_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* to,
    int backlog,
    FWD_FLAGS flags,
    GError** error);

guint
fwd_peer_add_remote_datagram_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* local,
    int backlog,
    guint inactivity_timeout_ms,
    guint conn_limit,
    FWD_FLAGS flags,
    GError** error);

guint
fwd_peer_add_remote_stream_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* to,
    int backlog,
    FWD_FLAGS flags,
    GError** error);

void
fwd_peer_remove_forwarder(
    FwdPeer* fp,
    guint id);

gulong
fwd_peer_add_socket_added_handler(
    FwdPeer* fp,
    FwdSocketStateFunc fn,
    gpointer user_data);

gulong
fwd_peer_add_socket_removed_handler(
    FwdPeer* fp,
    guint id,
    FwdSocketFunc fn,
    gpointer user_data);

gulong
fwd_peer_add_socket_state_handler(
    FwdPeer* fp,
    guint id,
    FwdSocketStateFunc fn,
    gpointer user_data);

gulong
fwd_peer_add_state_handler(
    FwdPeer* fp,
    FwdPeerFunc fn,
    gpointer user_data);

void
fwd_peer_remove_handler(
    FwdPeer* fp,
    gulong id);

void
fwd_peer_clear_handlers(
    FwdPeer* fp,
    gulong* ids,
    int count);

#define fwd_peer_clear_handler(fp, id) \
    fwd_peer_clear_handlers(fp, id, 1)
#define fwd_peer_clear_all_handlers(fp, ids) \
    fwd_peer_clear_handlers(fp, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* FWD_PEER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
