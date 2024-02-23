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

#include "fwd_entry_datagram_connection.h"

#include "fwd_control_client.h"
#include "fwd_log_p.h"
#include "fwd_op_call.h"
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

typedef struct fwd_entry_datagram_connection_impl {
    FwdEntryDatagramConnection dc;
    FwdEntryDatagramConnectionInactivityCheckFunc check;
    FwdEntry* parent;
    GInetSocketAddress* to;
    FwdSocketIo* io;
    GIoRpcPeer* rpc;
    GTimeSpan conn_timeout;
    guint rid;
    guint flush_id;
    guint init_id;
    GPtrArray* q;
} FwdEntryDatagramConnectionImpl;

static inline FwdEntryDatagramConnectionImpl*
fwd_entry_datagram_connection_impl_cast(FwdEntryDatagramConnection* dc)
    { return G_CAST(dc, FwdEntryDatagramConnectionImpl, dc); }

static inline FwdEntryDatagramConnectionImpl*
fwd_entry_datagram_connection_impl_entry_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryDatagramConnectionImpl, dc.entry); }

static
void
fwd_entry_datagram_connection_local_data_sent(
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
fwd_entry_datagram_connection_remote_data_sent(
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

static
void
fwd_entry_datagram_connection_touch(
    FwdEntryDatagramConnectionImpl* self)
{
    self->dc.expiry = g_get_monotonic_time() + self->conn_timeout;
    if (self->check) {
        self->check(self->parent);
    }
}

static
void
fwd_entry_datagram_connection_flush(
    FwdEntryDatagramConnectionImpl* self)
{
    if (self->q) {
        GInetSocketAddress* from = self->dc.addr;
        FwdEntry* entry = &self->dc.entry;
        GPtrArray* packets = self->q;
        guint i;

        self->q = NULL;
        for (i = 0; i < packets->len; i++) {
            GBytes* packet = packets->pdata[i];
            FwdOpCall* call = fwd_op_call_new(entry);
            GUtilData data;

            fwd_op_call_start(call, fwd_socket_call_data(self->rpc,
                self->rid, gutil_data_from_bytes(&data, packet), from,
                fwd_entry_datagram_connection_remote_data_sent,
                &call->op, fwd_op_dispose_unref_cb));
            from = NULL; /* Send it only once */
        }
        g_ptr_array_free(packets, TRUE);
        fwd_entry_datagram_connection_touch(self);
    }
}

static
gboolean
fwd_entry_datagram_connection_flush_cb(
    gpointer user_data)
{
    FwdEntryDatagramConnectionImpl* self = user_data;

    self->flush_id = 0;
    fwd_entry_datagram_connection_flush(self);
    return G_SOURCE_REMOVE;
}

static
void
fwd_entry_datagram_connection_socket_connected(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    void* user_data)
{
    FwdEntryDatagramConnectionImpl* self = user_data;
    FwdEntry* entry = &self->dc.entry;

    self->init_id = 0;
    if (result) {
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] Failed to connect to %s (%d)", entry->id,
                fwd_format_inet_socket_address(self->to), result);
            }
#endif /* GUTIL_LOG_DEBUG */
        fwd_entry_remove(entry);
    } else {
        fwd_entry_datagram_connection_flush(self);
    }
}

static
void
fwd_entry_datagram_connection_socket_created(
    int rid, /* Socket id > 0, error < 0, RPC error = 0 */
    GInetSocketAddress* sa,
    void* user_data)
{
    FwdEntryDatagramConnectionImpl* self = user_data;
    FwdEntry* entry = &self->dc.entry;

    self->init_id = 0;
    if (rid > 0) {
        if (self->rid) {
            /* oops, close it */
            GDEBUG("[%u] Dropping remote socket %u", entry->id, rid);
            fwd_socket_call_close(self->rpc, rid);
        } else {
#if GUTIL_LOG_DEBUG
            if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                GDEBUG("[%u] %s <=> [%d] %s", entry->id,
                    fwd_format_inet_socket_address(self->dc.addr), rid,
                    fwd_format_inet_socket_address(sa));
            }
#endif /* GUTIL_LOG_DEBUG */

            /* Store the remote socket id */
            self->rid = rid;

            /* And try to connect it */
            self->init_id = fwd_socket_call_connect(self->rpc, rid,
                self->to, fwd_entry_datagram_connection_socket_connected,
                self, NULL);
        }
    } else {
        GDEBUG("[%u] Error %d creating remote socket", entry->id, -rid);
        fwd_entry_remove(entry);
    }
}

/*==========================================================================*
 * FwdEntry
 *==========================================================================*/

static
void
fwd_entry_datagram_connection_start(
    FwdEntry* entry)
{
    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_impl_entry_cast(entry);
    GInetSocketAddress* from = self->dc.addr;

    /* Try to bind to the same port */
    const gushort port = g_inet_socket_address_get_port(from);
    GSocketFamily af = g_socket_address_get_family(G_SOCKET_ADDRESS(from));
    GInetAddress* ia = g_inet_address_new_any(af);
    GInetSocketAddress* isa = G_INET_SOCKET_ADDRESS
        (g_inet_socket_address_new(ia, port));

    /* Create and connect our own socket */
    GASSERT(!self->init_id);
    GASSERT(!self->rid);
    self->init_id = fwd_control_call_socket(self->rpc, entry->id, af,
        G_SOCKET_TYPE_DATAGRAM, isa, FWD_SOCKET_FLAG_RETRY_ANY, 0, 0, 0,
        fwd_entry_datagram_connection_socket_created, self, NULL);
    g_object_unref(isa);
    g_object_unref(ia);
}

static
guint
fwd_entry_datagram_connection_accepted(
   FwdEntry* entry,
   guint rid)
{
    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_impl_entry_cast(entry);

    if (rid && !self->rid) {
        self->rid = rid;
        /* First complete the call and then send the queued packets */
        self->flush_id = g_idle_add
            (fwd_entry_datagram_connection_flush_cb, self);
        fwd_entry_set_state(entry, FWD_SOCKET_STATE_CONNECTED);
        return FWD_SOCKET_RESULT_OK;
    }
    return EFAULT;
}

static
void
fwd_entry_datagram_connection_data(
    FwdEntry* entry,
    GIoRpcRequest* req,
    const GUtilData* data,
    GInetSocketAddress* from)
{
    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_impl_entry_cast(entry);

    if (self->io) {
        GBytes* bytes = g_bytes_new(data->bytes, data->size);

#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] Forwarding %u byte(s) from remote to %s",
                entry->id, (guint) data->size,
                fwd_format_inet_socket_address(self->dc.addr));
        }
#endif /* GUTIL_LOG_DEBUG */

        fwd_entry_datagram_connection_touch(self);
        fwd_socket_io_send(self->io, self->dc.addr, bytes,
            fwd_entry_datagram_connection_local_data_sent,
            giorpc_request_ref(req), (GDestroyNotify)
            giorpc_request_unref);
        g_bytes_unref(bytes);
    }
}

static
void
fwd_entry_datagram_connection_close(
    FwdEntry* entry)
{
    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_impl_entry_cast(entry);

    if (self->rid) {
        GDEBUG("[%u] Remote socket %u is gone", entry->id, self->rid);
        self->rid = 0;
        fwd_entry_remove(entry);
    }
}

static
void
fwd_entry_datagram_connection_dispose(
    FwdEntry* entry)
{
    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_impl_entry_cast(entry);

    self->io = NULL;
    self->check = NULL;
    self->parent = NULL;
    gutil_source_clear(&self->flush_id);
    if (self->init_id) {
        giorpc_peer_cancel(self->rpc, self->init_id);
        self->init_id = 0;
    }
    if (self->rid) {
        GDEBUG("[%u] Closing remote socket %u", entry->id, self->rid);
        fwd_socket_call_close(self->rpc, self->rid);
        self->rid = 0;
    }
    if (self->q) {
        g_ptr_array_free(self->q, TRUE);
        self->q = NULL;
    }
}

static
void
fwd_entry_datagram_connection_free(
    FwdEntry* entry)
{
    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_impl_entry_cast(entry);

    if (self->q) {
        g_ptr_array_free(self->q, TRUE);
    }
    giorpc_peer_cancel(self->rpc, self->init_id);
    giorpc_peer_unref(self->rpc);
    gutil_object_unref(self->to);
    g_object_unref(self->dc.addr);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

static
FwdEntryDatagramConnectionImpl*
fwd_entry_datagram_connection_new(
    FwdEntry* parent,
    FwdEntryDatagramConnectionInactivityCheckFunc check,
    FwdSocketIo* io,
    GInetSocketAddress* from,
    GTimeSpan conn_timeout,
    GBytes** packets,
    gsize n,
    const FwdEntryType* type)
{
    FwdEntryDatagramConnectionImpl* self =
        g_slice_new0(FwdEntryDatagramConnectionImpl);
    FwdEntryDatagramConnection* dc = &self->dc;
    FwdEntry* entry = &dc->entry;
    gsize i;

    fwd_entry_base_init(entry, type, parent->owner, FWD_SOCKET_STATE_INIT);
    self->conn_timeout = conn_timeout;
    g_object_ref(self->dc.addr = from);
    self->parent = parent;
    self->check = check;
    self->io = io;
    self->rpc = fwd_entry_rpc_ref(parent);
    self->q = g_ptr_array_new_full(n, (GDestroyNotify) g_bytes_unref);
    for (i = 0; i < n; i++) {
        g_ptr_array_add(self->q, g_bytes_ref(packets[i]));
    }
    fwd_entry_datagram_connection_touch(self);
    return self;
}

FwdEntryDatagramConnection*
fwd_entry_datagram_connection_new_local(
    FwdEntry* parent,
    FwdEntryDatagramConnectionInactivityCheckFunc check,
    FwdSocketIo* io,
    GInetSocketAddress* from,
    GInetSocketAddress* to,
    GTimeSpan conn_timeout,
    GBytes* packet)
{
    static const FwdEntryType datagram_connection_type_local = {
        fwd_entry_datagram_connection_start,
        NULL, /* accept */
        NULL, /* accepted */
        NULL, /* connect*/
        fwd_entry_datagram_connection_data,
        fwd_entry_datagram_connection_close,
        fwd_entry_datagram_connection_dispose,
        fwd_entry_datagram_connection_free
    };

    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_new(parent, check, io, from,
            conn_timeout, &packet, 1, &datagram_connection_type_local);

    /* The counterpart socket will be created by the start() callback */
    g_object_ref(self->to = to);
    return &self->dc;
}

FwdEntryDatagramConnection*
fwd_entry_datagram_connection_new_remote(
    FwdEntry* parent,
    FwdEntryDatagramConnectionInactivityCheckFunc check,
    FwdSocketIo* io,
    GInetSocketAddress* from,
    GTimeSpan conn_timeout,
    GBytes** packets,
    gsize n)
{
    static const FwdEntryType datagram_connection_type_remote = {
        NULL, /* start */
        NULL, /* accept */
        fwd_entry_datagram_connection_accepted,
        NULL, /* connect*/
        fwd_entry_datagram_connection_data,
        fwd_entry_datagram_connection_close,
        fwd_entry_datagram_connection_dispose,
        fwd_entry_datagram_connection_free
    };

    /* rid will be assigned by the ACCEPTED call */
    return &fwd_entry_datagram_connection_new(parent, check, io, from,
        conn_timeout, packets, n, &datagram_connection_type_remote)->dc;
}

void
fwd_entry_datagram_connection_send(
    FwdEntryDatagramConnection* dc,
    const GUtilData* packet)
{
    FwdEntryDatagramConnectionImpl* self =
        fwd_entry_datagram_connection_impl_cast(dc);

    if (self->rid) {
        FwdEntry* entry = &dc->entry;
        FwdOpCall* call = fwd_op_call_new(entry);

        fwd_entry_datagram_connection_touch(self);
        fwd_op_call_start(call, fwd_socket_call_data(self->rpc,
            self->rid, packet, NULL,
            fwd_entry_datagram_connection_remote_data_sent,
            &call->op, fwd_op_dispose_unref_cb));
    } else if (self->q) {
        g_ptr_array_add(self->q, g_bytes_new(packet->bytes, packet->size));
    }
}

void
fwd_entry_datagram_connection_stop(
    FwdEntryDatagramConnection* dc)
{
    fwd_entry_datagram_connection_impl_cast(dc)->io = NULL;
}

void
fwd_entry_datagram_connection_expire(
    FwdEntry* owner,
    GHashTable* map)
{
    /* The map contains pointers to FwdEntryDatagramConnection */
    if (g_hash_table_size(map)) {
        gpointer value;
        GHashTableIter it;
        GPtrArray* expired = NULL;

        /*
         * glib doesn't allow timers with less than a millisecond precision
         * (and it doesn't really make sense).
         */
        const gint64 deadline = g_get_monotonic_time() +
            G_TIME_SPAN_MILLISECOND;

        g_hash_table_iter_init(&it, map);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            FwdEntryDatagramConnection* dc = value;

            if (dc->expiry < deadline) {
                if (!expired) {
                    expired = g_ptr_array_new_full(1, fwd_entry_destroy);
                }
                /* Grab a temporary ref to the connection entry */
                g_ptr_array_add(expired, fwd_entry_ref(&dc->entry));
#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    GDEBUG("[%u] Datagram connection #%u %s has expired",
                        owner->id, dc->entry.id,
                        fwd_format_inet_socket_address(dc->addr));
                }
#endif /* GUTIL_LOG_DEBUG */
            }
        }

        if (expired) {
            guint i;

            for (i = 0; i < expired->len; i++) {
                fwd_entry_remove(expired->pdata[i]);
            }

            /* Drop temporary refs */
            g_ptr_array_free(expired, TRUE);
        }
    }
}

void
fwd_entry_datagram_connection_drop_oldest(
    FwdEntry* owner,
    GHashTable* map)
{
    /* The map contains pointers to FwdEntryDatagramConnection */
    gpointer value;
    GHashTableIter it;
    FwdEntryDatagramConnection* oldest = NULL;

    g_hash_table_iter_init(&it, map);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        FwdEntryDatagramConnection* dc = value;

        if (!oldest || dc->expiry < oldest->expiry) {
            oldest = dc;
        }
    }

    /*
     * The caller makes sure that the map is non-empty, we must
     * find something in there.
     */

#if GUTIL_LOG_DEBUG
    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
        GDEBUG("[%u] Dropping datagram connection #%u %s", owner->id,
            oldest->entry.id, fwd_format_inet_socket_address(oldest->addr));
    }
#endif /* GUTIL_LOG_DEBUG */

    fwd_entry_remove(&oldest->entry);
}

int
fwd_entry_datagram_connection_expiration_timeout_ms(
    GHashTable* map,
    gint64 expiry)
{
    /* The map contains pointers to FwdEntryDatagramConnection */
    if (g_hash_table_size(map)) {
        const gint64 now = g_get_monotonic_time();
        GHashTableIter it;
        gpointer value;

        g_hash_table_iter_init(&it, map);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            FwdEntryDatagramConnection* dc = value;

            if (dc->expiry < expiry || expiry <= 0) {
                expiry = dc->expiry;
            }
        }

        return expiry > now ? (int)((expiry - now)/G_TIME_SPAN_MILLISECOND) : 0;
    }
    return -1;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
