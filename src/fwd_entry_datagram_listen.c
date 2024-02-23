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

#include "fwd_entry_datagram_listen.h"

#include "fwd_entry_datagram_connection.h"
#include "fwd_log_p.h"
#include "fwd_op_call.h"
#include "fwd_peer_p.h"
#include "fwd_socket_client.h"
#include "fwd_socket_io.h"
#include "fwd_socket_service.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_intarray.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#define DEFAULT_BACKLOG 10

typedef struct fwd_entry_datagram_listen {
    FwdEntry entry;
    FwdSocketIoHandler io_handler;
    FwdSocketIo* io;
    GSocket* socket;
    GInetSocketAddress* sa;
    GHashTable* addr_map; /* contains FwdEntryDatagramConnection refs */
    GHashTable* id_map;   /* contains FwdEntryDatagramConnection ptrs */
    GQueue accept_queue;  /* contains FwdEntryDatagramAccept */
    GQueue backlog_queue; /* contains FwdEntryDatagramPendingConnection */
    guint inactivity_timer_id;
    guint64 conn_timeout;
    ulong remove_id;
    guint backlog;
    guint conn_limit;
    guint rid;
} FwdEntryDatagramListen;

static inline FwdEntryDatagramListen*
fwd_entry_datagram_listen_entry_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryDatagramListen, entry); }

static inline FwdEntryDatagramListen*
fwd_entry_datagram_listen_io_handler_cast(FwdSocketIoHandler* h)
    { return G_CAST(h, FwdEntryDatagramListen, io_handler); }

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

/*==========================================================================*
 * FwdEntryDatagramAccept
 *
 * Information about a single incoming ACCEPT request.
 * These are linked into the accept_queue.
 * Both link and the associated data are allocated from a single memory block.
 *==========================================================================*/

typedef struct fwd_entry_datagram_accept {
    GList link; /* Must be first */
    GIoRpcRequest* req;
    gulong req_cancel_id;
} FwdEntryDatagramAccept;

static
gboolean
fwd_entry_datagram_accept_free(
    FwdEntryDatagramAccept* accept)
{
    if (accept) {
        accept->link.data = NULL;
        giorpc_request_remove_cancel_handler(accept->req,
            accept->req_cancel_id);
        giorpc_request_unref(accept->req);
        gutil_slice_free(accept);
        return TRUE;
    } else {
        return FALSE;
    }
}

static
void
fwd_entry_datagram_accept_cancelled(
    GIoRpcRequest* req,
    gpointer listen)
{
    FwdEntryDatagramListen* self = listen;
    GQueue* q = &self->accept_queue;
    GList* l;

    /* The remote side has cancelled its ACCEPT request */
    for (l = q->head; l; l = l->next) {
        FwdEntryDatagramAccept* a = l->data;

        /* Link's data is supposed to point to itself */
       GASSERT(l->data == l);
        if (a->req == req) {
            GDEBUG("[%u] ACCEPT request [%06x] has been cancelled",
                self->entry.id, giorpc_request_id(req));
            g_queue_unlink(q, l);
            giorpc_request_remove_cancel_handler(req, a->req_cancel_id);
            a->req_cancel_id = 0;
            fwd_entry_datagram_accept_free(a);
            break;
        }
    }
}

static
FwdEntryDatagramAccept*
fwd_entry_datagram_accept_new(
    FwdEntryDatagramListen* self,
    GIoRpcRequest* req)
{
    FwdEntryDatagramAccept* accept = g_slice_new0(FwdEntryDatagramAccept);

    GDEBUG("[%u] ACCEPT request [%06x]", self->entry.id,
        giorpc_request_id(req));
    accept->link.data = accept; /* Point to self */
    accept->req = giorpc_request_ref(req);
    accept->req_cancel_id = giorpc_request_add_cancel_handler(req,
        fwd_entry_datagram_accept_cancelled, self);
    return accept;
}

static
FwdEntryDatagramAccept*
fwd_entry_datagram_accept_pop(
    FwdEntryDatagramListen* self)
{
    /* Link's data point to itself i.e. FwdEntryDatagramAccept */
    return (FwdEntryDatagramAccept*)
        g_queue_pop_head_link(&self->accept_queue);
}

/*==========================================================================*
 * FwdEntryDatagramPendingConnection
 *
 * Information about a new incoming "connection" which couldn't be accepted
 * because there was no queued ACCEPT request.
 *
 * These are linked into the backlog_queue
 * Both link and the associated data are allocated from a single memory block.
 *==========================================================================*/

typedef struct fwd_entry_datagram_pending_connection {
    GList link; /* Must be first */
    gint64 expiry; /* The monotonic time, in microseconds */
    GInetSocketAddress* addr;
    GPtrArray* packets;
} FwdEntryDatagramPendingConnection;

static
gboolean
fwd_entry_datagram_pending_connection_free(
    FwdEntryDatagramPendingConnection* pc)
{
    if (pc) {
        pc->link.data = NULL;
        g_object_unref(pc->addr);
        g_ptr_array_free(pc->packets, TRUE);
        gutil_slice_free(pc);
        return TRUE;
    } else {
        return FALSE;
    }
}

static
FwdEntryDatagramPendingConnection*
fwd_entry_datagram_pending_connection_new(
    GInetSocketAddress* from,
    gint64 expiry,
    GBytes* packet)
{
    FwdEntryDatagramPendingConnection* pc =
        g_slice_new0(FwdEntryDatagramPendingConnection);

    pc->link.data = pc;
    pc->expiry = expiry;
    g_object_ref(pc->addr = from);
    pc->packets = g_ptr_array_new_with_free_func((GDestroyNotify)
        g_bytes_unref);
    g_ptr_array_add(pc->packets, g_bytes_ref(packet));
    return pc;
}

static
FwdEntryDatagramPendingConnection*
fwd_entry_datagram_pending_connection_peek(
    FwdEntryDatagramListen* self)
{
    return (FwdEntryDatagramPendingConnection*)
        g_queue_peek_head(&self->backlog_queue);
}

static
FwdEntryDatagramPendingConnection*
fwd_entry_datagram_pending_connection_pop(
    FwdEntryDatagramListen* self)
{
    /* Link's data point to itself i.e. FwdEntryDatagramPendingConnection */
    return (FwdEntryDatagramPendingConnection*)
        g_queue_pop_head_link(&self->backlog_queue);
}

static
gint
fwd_entry_datagram_pending_connection_sort_func(
    gconstpointer a,
    gconstpointer b,
    gpointer user_data)
{
    const FwdEntryDatagramPendingConnection* pc1 = a;
    const FwdEntryDatagramPendingConnection* pc2 = b;

    /* Oldest entries first */
    return (pc1->expiry < pc2->expiry) ? -1 :
        (pc1->expiry > pc2->expiry) ? 1 : 0;
}

/*==========================================================================*
 * Inactivity timer
 *==========================================================================*/

static
void
fwd_entry_datagram_listen_schedule_inactivity_check(
    FwdEntry* entry);

static
gboolean
fwd_entry_datagram_listen_inactivity_check_func(
    gpointer user_data)
{
    const gint64 now = g_get_monotonic_time();
    FwdEntryDatagramListen* self = user_data;
    FwdEntryDatagramPendingConnection* pc;

    GDEBUG("[%u] Running inactivity check", self->entry.id);
    self->inactivity_timer_id = 0;

    /* Drop expired pending connections */
    while ((pc = fwd_entry_datagram_pending_connection_peek(self)) &&
        pc->expiry <= now) {
        E_DEBUG1(&self->entry, "Pending datagram connection %s has expired",
            fwd_format_inet_socket_address(pc->addr));
        fwd_entry_datagram_pending_connection_free
            (fwd_entry_datagram_pending_connection_pop(self));
    }

    /* Drop expired active connections */
    fwd_entry_datagram_connection_expire(&self->entry, self->addr_map);

    /* Schedule the next check if necessary */
    fwd_entry_datagram_listen_schedule_inactivity_check(&self->entry);
    return G_SOURCE_REMOVE;
}

static
void
fwd_entry_datagram_listen_schedule_inactivity_check(
    FwdEntry* entry)
{
    FwdEntryDatagramListen* self = fwd_entry_datagram_listen_entry_cast(entry);

    if (!self->inactivity_timer_id) {
        FwdEntryDatagramPendingConnection* pc =
            fwd_entry_datagram_pending_connection_peek(self);
        const int timeout =
            fwd_entry_datagram_connection_expiration_timeout_ms(self->addr_map,
                pc ? pc->expiry : 0);

        if (timeout >= 0) {
            GDEBUG("[%u] Scheduling inactivity check in %u ms",
                self->entry.id, timeout);
            self->inactivity_timer_id = g_timeout_add(timeout,
                fwd_entry_datagram_listen_inactivity_check_func, self);
        }
    }
}

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
void
fwd_entry_datagram_listen_connection_remove(
    gpointer user_data)
{
    FwdEntryDatagramConnection* dc = user_data;

    fwd_entry_datagram_connection_stop(dc);
    fwd_entry_remove(&dc->entry);
    fwd_entry_unref(&dc->entry);
}

static
void
fwd_entry_datagram_listen_entry_removed(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    gpointer idkey = GUINT_TO_POINTER(id);
    FwdEntryDatagramListen* self = user_data;
    FwdEntryDatagramConnection* dc = g_hash_table_lookup(self->id_map, idkey);

    if (dc) {
        g_hash_table_remove(self->id_map, idkey);
        g_hash_table_remove(self->addr_map, dc->addr);
        GDEBUG("[%u] %u datagram connection(s) left", self->entry.id,
            g_hash_table_size(self->addr_map));
    }
}

static
FwdEntryDatagramConnection*
fwd_entry_datagram_listen_connection_new(
    FwdEntryDatagramListen* self,
    GInetSocketAddress* from,
    GBytes** packets,
    gsize n)
{
    FwdEntryDatagramConnection* dc =
        fwd_entry_datagram_connection_new_remote(&self->entry,
            fwd_entry_datagram_listen_schedule_inactivity_check,
            self->io, from, self->conn_timeout, packets, n);
    FwdEntry* entry = &dc->entry;

    g_hash_table_insert(self->addr_map, g_object_ref(from),
        fwd_entry_ref(entry));
    g_hash_table_insert(self->id_map, GUINT_TO_POINTER(entry->id), entry);
    if (g_hash_table_size(self->addr_map) > self->conn_limit) {
        fwd_entry_datagram_connection_drop_oldest(&self->entry, self->addr_map);
    }
    fwd_entry_insert(entry);
    E_DEBUG2(&self->entry, "Accepting datagram connection #%u from %s",
        dc->entry.id, fwd_format_inet_socket_address(from));
    return dc;
}

static
void
fwd_entry_datagram_listen_purge(
    FwdEntryDatagramListen* self)
{
    /* Drop everything that has been pending */
    while (fwd_entry_datagram_accept_free(
           fwd_entry_datagram_accept_pop(self)));
    while (fwd_entry_datagram_pending_connection_free(
           fwd_entry_datagram_pending_connection_pop(self)));
}

/*==========================================================================*
 * FwdSocketIoHandler
 *==========================================================================*/

static
void
fwd_entry_datagram_listen_io_error(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    FwdEntryDatagramListen* self =
        fwd_entry_datagram_listen_io_handler_cast(handler);
    FwdEntry* entry = &self->entry;

    GDEBUG("[%u] I/O error", entry->id);
    fwd_entry_remove(entry);
}

static
void
fwd_entry_datagram_listen_io_receive(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    FwdEntryDatagramListen* self =
        fwd_entry_datagram_listen_io_handler_cast(handler);
    FwdEntryDatagramConnection* dc =
        g_hash_table_lookup(self->addr_map, from);

    if (dc) {
        fwd_entry_datagram_connection_send(dc, data);
    } else {
        FwdEntry* entry = &self->entry;
        GBytes* packet = g_bytes_new(data->bytes, data->size);
        FwdEntryDatagramAccept* accept =
            fwd_entry_datagram_accept_pop(self);

        if (accept) {
            /* Accept the connection */
            dc = fwd_entry_datagram_listen_connection_new(self, from,
                &packet, 1);
            fwd_socket_service_accept_ok(accept->req, dc->entry.id, from,
                self->sa);
            fwd_entry_datagram_accept_free(accept);
            fwd_entry_datagram_listen_schedule_inactivity_check(entry);
        } else {
            const gint64 expiry = g_get_monotonic_time() + self->conn_timeout;
            FwdEntryDatagramPendingConnection* pc = NULL;
            GQueue* q = &self->backlog_queue;
            GList* l;

            /* Purge the backlog */
            while (g_queue_get_length(q) >= self->backlog) {
                pc = fwd_entry_datagram_pending_connection_pop(self);
                E_DEBUG1(entry, "Dropping datagram connection %s",
                    fwd_format_inet_socket_address(pc->addr));
                fwd_entry_datagram_pending_connection_free(pc);
                pc = NULL;
            }

            for (l = q->head; l; l = l->next) {
                pc = l->data;
                if (fwd_inet_socket_address_equal(pc->addr, from)) {
                    /* Update the existing entry */
                    g_ptr_array_add(pc->packets, g_bytes_ref(packet));
                    pc->expiry = expiry;
                    E_DEBUG1(entry, "Queued datagram packet from %s",
                        fwd_format_inet_socket_address(from));

                    /* Keep the queue sorted */
                    g_queue_sort(&self->backlog_queue,
                        fwd_entry_datagram_pending_connection_sort_func,
                        NULL);
                    break;
                }
                pc = NULL;
            }

            if (!pc) {
                /* No connection for this address yet, create one */
                pc = fwd_entry_datagram_pending_connection_new(from,
                    expiry, packet);

                /* The latest connection goes to the end of the queue */
                g_queue_push_tail_link(q, &pc->link);
                E_DEBUG2(entry, "Queuing datagram packet from %s (total %u)",
                    fwd_format_inet_socket_address(from), q->length);
                fwd_entry_datagram_listen_schedule_inactivity_check(entry);
            }
        }
        g_bytes_unref(packet);
    }
}

/*==========================================================================*
 * FwdEntry
 *==========================================================================*/

static
void
fwd_entry_datagram_listen_start(
    FwdEntry* entry)
{
    FwdEntryDatagramListen* self =
        fwd_entry_datagram_listen_entry_cast(entry);

    fwd_socket_io_start_receive(self->io);
}

static
void
fwd_entry_datagram_listen_accept(
   FwdEntry* entry,
   GIoRpcRequest* req)
{
    FwdEntryDatagramListen* self =
        fwd_entry_datagram_listen_entry_cast(entry);
    FwdEntryDatagramPendingConnection* pc =
        fwd_entry_datagram_pending_connection_pop(self);

    if (pc) {
        /* Accept the pending connection */
        FwdEntryDatagramConnection* dc =
            fwd_entry_datagram_listen_connection_new(self, pc->addr,
                (GBytes**) pc->packets->pdata, pc->packets->len);

        fwd_socket_service_accept_ok(req, dc->entry.id, pc->addr, self->sa);
        fwd_entry_datagram_pending_connection_free(pc);
    } else {
        /* Queue the acceptor */
        g_queue_push_tail_link(&self->accept_queue,
            &fwd_entry_datagram_accept_new(self, req)->link);
    }
}

static
void
fwd_entry_datagram_listen_close(
    FwdEntry* entry)
{
    FwdEntryDatagramListen* self =
        fwd_entry_datagram_listen_entry_cast(entry);

    if (self->rid) {
        GDEBUG("[%u] Remote socket %u is gone", entry->id, self->rid);
        self->rid = 0;
        fwd_entry_remove(entry);
    }
}

static
void
fwd_entry_datagram_listen_dispose(
    FwdEntry* entry)
{
    FwdEntryDatagramListen* self =
        fwd_entry_datagram_listen_entry_cast(entry);

    gutil_source_clear(&self->inactivity_timer_id);
    fwd_entry_datagram_listen_purge(self);
    g_hash_table_remove_all(self->id_map);
    g_hash_table_remove_all(self->addr_map);
    if (self->rid) {
        GDEBUG("[%u] Closing remote socket %u", entry->id, self->rid);
        fwd_socket_call_close(fwd_entry_rpc(entry), self->rid);
        self->rid = 0;
    }
    fwd_peer_clear_handler(entry->owner, &self->remove_id);
    fwd_socket_io_stop_receive(self->io);
    fwd_entry_base_dispose(entry);
}

static
void
fwd_entry_datagram_listen_free(
    FwdEntry* entry)
{
    FwdEntryDatagramListen* self =
        fwd_entry_datagram_listen_entry_cast(entry);

    fwd_peer_remove_handler(entry->owner, self->remove_id);
    fwd_entry_datagram_listen_purge(self);
    fwd_socket_io_free(self->io);
    g_hash_table_destroy(self->id_map);
    g_hash_table_destroy(self->addr_map);
    g_object_unref(self->sa);
    g_object_unref(self->socket);
    gutil_source_remove(self->inactivity_timer_id);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdEntry*
fwd_entry_datagram_listen_new(
    FwdPeer* owner,
    GSocket* socket,
    guint rid,
    guint backlog,
    guint conn_timeout,
    guint conn_limit)
{
    static const FwdEntryType datagram_listen_type = {
        fwd_entry_datagram_listen_start,
        fwd_entry_datagram_listen_accept,
        NULL, /* accepted */
        NULL, /* connect */
        NULL, /* data */
        fwd_entry_datagram_listen_close,
        fwd_entry_datagram_listen_dispose,
        fwd_entry_datagram_listen_free
    };
    static const FwdSocketIoCallbacks datagram_listen_io = {
        fwd_entry_datagram_listen_io_error,
        fwd_entry_datagram_listen_io_receive
    };
    FwdEntryDatagramListen* self = g_slice_new0(FwdEntryDatagramListen);
    FwdEntry* entry = &self->entry;

    fwd_entry_base_init(entry, &datagram_listen_type, owner,
        FWD_SOCKET_STATE_LISTEN_LOCAL);
    g_queue_init(&self->accept_queue);
    g_queue_init(&self->backlog_queue);
    g_object_ref(self->socket = socket);
    self->sa = G_INET_SOCKET_ADDRESS(g_socket_get_local_address(socket, NULL));
    self->rid = rid;
    self->backlog = backlog ? backlog : DEFAULT_BACKLOG;
    self->conn_timeout = conn_timeout ? conn_timeout : DEFAULT_CONN_TIMEOUT;
    self->conn_limit = conn_limit ? conn_limit : DEFAULT_CONN_LIMIT;
    self->io_handler.cb = &datagram_listen_io;

    /* GInetSocketAddress => FwdEntryDatagramConnection ref */
    self->addr_map = g_hash_table_new_full(fwd_inet_socket_address_hash,
        fwd_inet_socket_address_equal, g_object_unref,
        fwd_entry_datagram_listen_connection_remove);

    /* id => FwdEntryDatagramConnection ptr */
    self->id_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->remove_id = fwd_peer_add_socket_removed_handler(owner, 0,
        fwd_entry_datagram_listen_entry_removed, self);
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
