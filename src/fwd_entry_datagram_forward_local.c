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

#include "fwd_entry_datagram_forward_local.h"

/*
 * Matching remote UDP replies to the local datagrams:
 *
 *          local                  |             remote
 *          =====                  |             ======
 *                               giorpc
 *         network                 |
 *            ^                    |
 *            |                    |
 *            v                    |
 *       +---------+               |
 *       | GSocket | <--------+    |             network
 * +======================+   |    |               ^  ^
 * | DatagramForwardLocal |   |    |               |  |
 * +======================+  /|    |               |  |
 *           | |            / |    |               v  v
 *           v v           / /     |              +---------+
 *  +--------------------+  /      |           +- | GSocket |---+
 *  | +--------------------+ --- CONNECT --> +----------------+ |
 *  +-| DatagramConnection | <--- DATA ----> | DatagramSocket |-+
 *    +--------------------+       |         +----------------+
 *                                 |
 */

#include "fwd_entry_datagram_connection.h"
#include "fwd_log_p.h"
#include "fwd_peer_p.h"
#include "fwd_socket_io.h"
#include "fwd_util_p.h"

#include <giorpc_peer.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

typedef struct fwd_entry_datagram_forward_local {
    FwdEntry entry;
    FwdSocketIoHandler io_handler;
    FwdSocketIo* io;
    GInetSocketAddress* to;
    GSocket* socket;
    GHashTable* addr_map; /* contains FwdEntryDatagramConnection refs */
    GHashTable* id_map;   /* contains FwdEntryDatagramConnection ptrs */
    GTimeSpan conn_timeout;
    guint conn_limit;
    guint inactivity_timer_id;
    ulong remove_id;
} FwdEntryDatagramForwardLocal;

static inline FwdEntryDatagramForwardLocal*
fwd_entry_datagram_forward_local_entry_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryDatagramForwardLocal, entry); }

static inline FwdEntryDatagramForwardLocal*
fwd_entry_datagram_socket_io_handler_cast(FwdSocketIoHandler* h)
    { return G_CAST(h, FwdEntryDatagramForwardLocal, io_handler); }

#if GUTIL_LOG_DEBUG
/* Validate the type of the 'e' argument */
static inline const FwdEntry* e_(const FwdEntry* e) { return e; }
/*
 * The purpose of these macros is to avoid evaluating the arguments
 * if debug log is not enabled (note that this may have side effects).
 */
#  define E_DEBUG1(e,f,p1) (GLOG_ENABLED(GLOG_LEVEL_DEBUG) ? \
    (void) GDEBUG("[%u] " f, e_(e)->id, (p1)) : GLOG_NOTHING)
#  define E_DEBUG2(e,f,p1,p2) (GLOG_ENABLED(GLOG_LEVEL_DEBUG) ? \
    (void) GDEBUG("[%u] " f, e_(e)->id, (p1), (p2)) : GLOG_NOTHING)
#else
#  define E_DEBUG1(e,fmt,p1) GLOG_NOTHING
#  define E_DEBUG2(e,f,p1,p2) GLOG_NOTHING
#endif

static
void
fwd_entry_datagram_forward_local_connection_remove(
    gpointer user_data)
{
    FwdEntryDatagramConnection* dc = user_data;

    fwd_entry_datagram_connection_stop(dc);
    fwd_entry_remove(&dc->entry);
    fwd_entry_unref(&dc->entry);
}

static
void
fwd_entry_datagram_forward_local_entry_removed(
    FwdPeer* owner,
    guint id,
    void* user_data)
{
    gpointer idkey = GUINT_TO_POINTER(id);
    FwdEntryDatagramForwardLocal* self = user_data;
    FwdEntryDatagramConnection* dc = g_hash_table_lookup(self->id_map, idkey);

    if (dc) {
        g_hash_table_remove(self->id_map, idkey);
        g_hash_table_remove(self->addr_map, dc->addr);
    }
}

/*==========================================================================*
 * Inactivity timer
 *==========================================================================*/

static
void
fwd_entry_datagram_forward_local_schedule_inactivity_check(
    FwdEntry* entry);

static
gboolean
fwd_entry_datagram_forward_local_inactivity_check_func(
    gpointer user_data)
{
    FwdEntryDatagramForwardLocal* self = user_data;

    GDEBUG("[%u] Running inactivity check", self->entry.id);
    self->inactivity_timer_id = 0;

    /* Drop expired active connections */
    fwd_entry_datagram_connection_expire(&self->entry, self->addr_map);

    /* Schedule the next check if necessary */
    fwd_entry_datagram_forward_local_schedule_inactivity_check(&self->entry);
    return G_SOURCE_REMOVE;
}

static
void
fwd_entry_datagram_forward_local_schedule_inactivity_check(
    FwdEntry* entry)
{
    FwdEntryDatagramForwardLocal* self =
        fwd_entry_datagram_forward_local_entry_cast(entry);

    if (!self->inactivity_timer_id) {
        int timeout = fwd_entry_datagram_connection_expiration_timeout_ms
            (self->addr_map, 0);

        if (timeout >= 0) {
            GDEBUG("[%u] Scheduling inactivity check in %u ms",
                self->entry.id, timeout);
            self->inactivity_timer_id = g_timeout_add(timeout,
                fwd_entry_datagram_forward_local_inactivity_check_func,
                self);
        }
    }
}

/*==========================================================================*
 * FwdSocketIoHandler
 *==========================================================================*/

static
void
fwd_entry_datagram_forward_local_io_error(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    FwdEntryDatagramForwardLocal* self =
        fwd_entry_datagram_socket_io_handler_cast(handler);
    FwdEntry* entry = &self->entry;

    GDEBUG("[%u] I/O error", entry->id);
    fwd_entry_remove(entry);
}

static
void
fwd_entry_datagram_forward_local_io_receive(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    FwdEntryDatagramForwardLocal* self =
        fwd_entry_datagram_socket_io_handler_cast(handler);
    FwdEntryDatagramConnection* dc =
        g_hash_table_lookup(self->addr_map, from);

    E_DEBUG2(&self->entry, "Received %u bytes(s) from %s",
        (guint) data->size, fwd_format_inet_socket_address(from));

    if (dc) {
        fwd_entry_datagram_connection_send(dc, data);
    } else {
        GBytes* packet = g_bytes_new(data->bytes, data->size);
        FwdEntry* entry =
            &(dc = fwd_entry_datagram_connection_new_local(&self->entry,
            fwd_entry_datagram_forward_local_schedule_inactivity_check,
            self->io, from, self->to, self->conn_timeout, packet))->entry;

        if (g_hash_table_size(self->addr_map) >= self->conn_limit) {
            fwd_entry_datagram_connection_drop_oldest(&self->entry,
                self->addr_map);
        }
        g_bytes_unref(packet);
        g_hash_table_insert(self->addr_map, g_object_ref(from),
            fwd_entry_ref(entry));
        g_hash_table_insert(self->id_map, GUINT_TO_POINTER(entry->id), entry);
        fwd_entry_insert(entry);
        E_DEBUG2(&self->entry, "New datagram connection #%u for %s",
            entry->id, fwd_format_inet_socket_address(from));
    }
}

/*==========================================================================*
 * FwdEntry
 *==========================================================================*/

static
void
fwd_entry_datagram_forward_local_start(
    FwdEntry* entry)
{
    FwdEntryDatagramForwardLocal* self =
        fwd_entry_datagram_forward_local_entry_cast(entry);

    fwd_socket_io_start_receive(self->io);
}

static
void
fwd_entry_datagram_forward_local_dispose(
    FwdEntry* entry)
{
    FwdEntryDatagramForwardLocal* self =
        fwd_entry_datagram_forward_local_entry_cast(entry);

    gutil_source_clear(&self->inactivity_timer_id);
    fwd_peer_clear_handler(entry->owner, &self->remove_id);
    g_hash_table_remove_all(self->id_map);
    g_hash_table_remove_all(self->addr_map);
    fwd_socket_io_stop_receive(self->io);
    fwd_entry_base_dispose(entry);
}

static
void
fwd_entry_datagram_forward_local_free(
    FwdEntry* entry)
{
    FwdEntryDatagramForwardLocal* self =
        fwd_entry_datagram_forward_local_entry_cast(entry);

    fwd_peer_remove_handler(entry->owner, self->remove_id);
    fwd_socket_io_free(self->io);
    g_hash_table_destroy(self->id_map);
    g_hash_table_destroy(self->addr_map);
    g_object_unref(self->to);
    g_object_unref(self->socket);
    gutil_source_remove(self->inactivity_timer_id);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdEntry*
fwd_entry_datagram_forward_local_new(
    FwdPeer* owner,
    GSocket* socket,
    GInetSocketAddress* to,
    guint timeout_ms,
    guint conn_limit,
    GError** error)
{
    static const FwdEntryType datagram_forward_local_type = {
        fwd_entry_datagram_forward_local_start,
        NULL, /* accept */
        NULL, /* accepted */
        NULL, /* connect */
        NULL, /* data */
        NULL, /* close */
        fwd_entry_datagram_forward_local_dispose,
        fwd_entry_datagram_forward_local_free
    };
    static const FwdSocketIoCallbacks datagram_forward_local_io = {
        fwd_entry_datagram_forward_local_io_error,
        fwd_entry_datagram_forward_local_io_receive
    };
    FwdEntryDatagramForwardLocal* self =
        g_slice_new0(FwdEntryDatagramForwardLocal);
    FwdEntry* entry = &self->entry;

    fwd_entry_base_init(entry, &datagram_forward_local_type, owner,
        FWD_SOCKET_STATE_LISTEN_LOCAL);
    g_object_ref(self->to = to);
    g_object_ref(self->socket = socket);
    self->io_handler.cb = &datagram_forward_local_io;
    self->conn_timeout = (timeout_ms == FWD_TIMEOUT_DEFAULT) ?
        DEFAULT_CONN_TIMEOUT : (timeout_ms * G_TIME_SPAN_MILLISECOND);
    self->conn_limit = (conn_limit == FWD_LIMIT_DEFAULT) ?
        DEFAULT_CONN_LIMIT : conn_limit;

    /* GInetSocketAddress => FwdEntryDatagramConnection */
    self->addr_map = g_hash_table_new_full(fwd_inet_socket_address_hash,
        fwd_inet_socket_address_equal, g_object_unref,
        fwd_entry_datagram_forward_local_connection_remove);

    /* id => FwdEntryDatagramConnection */
    self->id_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->remove_id = fwd_peer_add_socket_removed_handler(owner, 0,
        fwd_entry_datagram_forward_local_entry_removed, self);
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
