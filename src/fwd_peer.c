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

#include "fwd_peer_p.h"

#include "fwd_control_client.h"
#include "fwd_control_service.h"
#include "fwd_entry_datagram_forward_local.h"
#include "fwd_entry_datagram_forward_remote.h"
#include "fwd_entry_datagram_listen.h"
#include "fwd_entry_datagram_socket.h"
#include "fwd_entry_stream_forward_local.h"
#include "fwd_entry_stream_forward_remote.h"
#include "fwd_entry_stream_listen.h"
#include "fwd_entry_stream_socket.h"
#include "fwd_log_p.h"
#include "fwd_protocol.h"
#include "fwd_socket_client.h"
#include "fwd_socket_service.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_idlepool.h>
#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_weakref.h>

#include <errno.h>

typedef GObjectClass FwdPeerObjectClass;
typedef struct fwd_peer_object {
    GObject object;
    FwdPeer pub;
    GIoRpcPeer* rpc;
    FwdControlServiceHandler control_handler;
    FwdSocketServiceHandler socket_handler;
    FwdControlService* control_service;
    FwdSocketService* socket_service;
    GHashTable* sockets; /* ID => FwdEntry */
    GUtilWeakRef* ref;
    guint info_call;
    gulong rpc_state_event_id;
} FwdPeerObject;

#define PARENT_CLASS fwd_peer_object_parent_class
#define THIS_TYPE fwd_peer_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, FwdPeerObject)

GType THIS_TYPE G_GNUC_INTERNAL;
G_DEFINE_TYPE(FwdPeerObject, fwd_peer_object, G_TYPE_OBJECT)

typedef enum fwd_signal {
    SIGNAL_STATE_CHANGED,
    SIGNAL_SOCKET_ADDED,
    SIGNAL_SOCKET_REMOVED,
    SIGNAL_SOCKET_STATE_CHANGED,
    SIGNAL_COUNT
} FWD_SIGNAL;

#define SIGNAL_STATE_CHANGED_NAME        "fwd-peer-state-changed"
#define SIGNAL_SOCKET_ADDED_NAME         "fwd-peer-socket-added"
#define SIGNAL_SOCKET_REMOVED_NAME       "fwd-peer-socket-removed"
#define SIGNAL_SOCKET_STATE_CHANGED_NAME "fwd-peer-socket-state-changed"

static guint fwd_signals[SIGNAL_COUNT] = { 0 };

typedef struct fwd_closure {
    GCClosure cclosure;
    union fwd_func {
        GCallback cb;
        FwdPeerFunc peer;
        FwdSocketFunc socket;
        FwdSocketStateFunc socket_state;
    } fn;
    void* user_data;
} FwdClosure;

#define fwd_closure_new_with_type(type)  \
    ((type*)g_closure_new_simple(sizeof(type), NULL))

static inline FwdPeerObject*
fwd_peer_cast(FwdPeer* pub)
    { return pub ? THIS(G_CAST(pub, FwdPeerObject, pub)) : NULL; }

static inline FwdPeerObject*
fwd_control_service_handler_cast(FwdControlServiceHandler* handler)
    { return THIS(G_CAST(handler, FwdPeerObject, control_handler)); }

static inline FwdPeerObject*
fwd_socket_service_handler_cast(FwdSocketServiceHandler* handler)
    { return THIS(G_CAST(handler, FwdPeerObject, socket_handler)); }

#if GUTIL_LOG_DEBUG
static
const char*
fwd_state_name(
    FWD_STATE state)
{
    switch (state) {
    #define STATE_(X) case FWD_STATE_##X: return #X
    STATE_(STARTING);
    STATE_(STARTED);
    STATE_(STOPPED);
    #undef STATE_
    }
    return gutil_idle_pool_add(NULL, g_strdup_printf("%d", state), g_free);
}

static
const char*
fwd_socket_state_name(
    FWD_SOCKET_STATE state)
{
    switch (state) {
    #define SOCKET_STATE_(X) case FWD_SOCKET_STATE_##X: return #X
    SOCKET_STATE_(INIT);
    SOCKET_STATE_(LISTEN_REMOTE_STARTING);
    SOCKET_STATE_(LISTEN_REMOTE);
    SOCKET_STATE_(LISTEN_REMOTE_ERROR);
    SOCKET_STATE_(LISTEN_LOCAL);
    SOCKET_STATE_(CONNECTED);
    #undef SOCKET_STATE_
    }
    return gutil_idle_pool_add(NULL, g_strdup_printf("%d", state), g_free);
}
#endif

static
GClosure*
fwd_closure_new(
    GCallback callback,
    GCallback fn,
    gpointer user_data)
{
    FwdClosure* c =  fwd_closure_new_with_type(FwdClosure);
    GCClosure* cc = &c->cclosure;

    /*
     * We can't directly connect the provided callback because it expects
     * the first parameter to point to public part of the object but glib
     * will call it with FwdPeerObject as the first argument.
     */
    cc->closure.data = c;
    cc->callback = callback;
    c->fn.cb = fn;
    c->user_data = user_data;
    return &cc->closure;
}

static
void
fwd_closure_peer_callback(
    FwdPeerObject* self,
    FwdClosure* closure)
{
    closure->fn.peer(&self->pub, closure->user_data);
}

static
void
fwd_closure_socket_callback(
    FwdPeerObject* self,
    guint id,
    FwdClosure* closure)
{
    closure->fn.socket(&self->pub, id, closure->user_data);
}

static
void
fwd_closure_socket_state_callback(
    FwdPeerObject* self,
    guint id,
    FWD_SOCKET_STATE state,
    FwdClosure* closure)
{
    closure->fn.socket_state(&self->pub, id, state, closure->user_data);
}

/*==========================================================================*
 * Connection state management
 *==========================================================================*/

static
void
fwd_peer_set_state(
    FwdPeerObject* self,
    FWD_STATE state)
{
    FwdPeer* pub = &self->pub;

    if (state > pub->state) {
        GDEBUG("%s => %s", fwd_state_name(pub->state),
            fwd_state_name(state));
        pub->state = state;

        if (state == FWD_STATE_STARTED) {
            const guint n = g_hash_table_size(self->sockets);

            /* Postponed start */
            if (n > 0) {
                guint i;
                gpointer value;
                GHashTableIter it;
                GPtrArray* a = g_ptr_array_new_full(n, fwd_entry_destroy);

                /* Build the list of entries that need to be started */
                g_hash_table_iter_init(&it, self->sockets);
                while (g_hash_table_iter_next(&it, NULL, &value)) {
                    FwdEntry* entry = value;

                    if (entry->type->start) {
                        g_ptr_array_add(a, fwd_entry_ref(value));
                    }
                }

                /* Start the entries */
                for (i = 0; i < a->len; i++) {
                    FwdEntry* entry = a->pdata[i];

                    entry->type->start(entry);
                }

                g_ptr_array_free(a, TRUE);
            }
        }

        g_signal_emit(self, fwd_signals[SIGNAL_STATE_CHANGED], 0);
    }
}

static
void
fwd_control_call_info_done(
    const FwdControlInfo* info,
    gpointer user_data)
{
    FwdPeerObject* self = gutil_weakref_get(user_data);

    if (self) {
        self->info_call = 0;
        if (info) {
            GDEBUG("Remote version range %u..%u", info->v1, info->v2);
            fwd_peer_set_state(self, FWD_STATE_STARTED);
        } else {
            fwd_peer_set_state(self, FWD_STATE_STOPPED);
        }
        g_object_unref(self);
    }
}

static
void
fwd_control_start(
    FwdPeerObject* self)
{
    GDEBUG("Requesting remote info");
    self->info_call = fwd_control_call_info(self->rpc,
        fwd_control_call_info_done, gutil_weakref_ref(self->ref),
        (GDestroyNotify) gutil_weakref_unref);
}

static
void
fwd_control_handle_peer_state(
    GIoRpcPeer* rpc,
    void* user_data)
{
    FwdPeerObject* self = THIS(user_data);

    g_object_ref(self);
    if (rpc->state == GIORPC_PEER_STATE_STARTED) {
        fwd_control_start(self);
    }
    if (rpc->state >= GIORPC_PEER_STATE_STOPPING) {
        fwd_peer_set_state(self, FWD_STATE_STOPPED);
        /* No longer need this event */
        giorpc_peer_remove_handlers(rpc, &self->rpc_state_event_id, 1);
    }
    g_object_unref(self);
}

/*==========================================================================*
 * FwdControlServiceHandler
 *==========================================================================*/

static
void
fwd_peer_handle_control_socket(
    FwdControlServiceHandler* handler,
    GIoRpcRequest* req,
    guint rid,
    GSocketType type,
    GInetSocketAddress* isa,
    FWD_SOCKET_FLAGS flags,
    guint backlog,
    guint timeout_ms,
    guint maxconn)
{
    GUtilData* addr = fwd_inet_socket_address_to_native(isa, NULL);

    if (addr) {
        GError* error = NULL;
        FwdPeerObject* self = fwd_control_service_handler_cast(handler);
        FwdPeer* fp = &self->pub;
        /* On success, addr->bytes receives the real address */
        GSocket* socket = fwd_socket_new_from_data(type, addr,
            (flags & FWD_SOCKET_FLAG_REUSADDR) != 0,
            (flags & FWD_SOCKET_FLAG_RETRY_ANY) != 0,
            &error);

        if (socket) {
            FwdEntry* entry = NULL;

            if (type == G_SOCKET_TYPE_STREAM) {
                if (flags & FWD_SOCKET_FLAG_LISTEN) {
                    entry = fwd_entry_stream_listen_new(fp, socket,
                        rid, backlog, &error);
                } else {
                    entry = &fwd_entry_stream_socket_new(fp, socket,
                        rid)->entry;
                }
            } else if (type == G_SOCKET_TYPE_DATAGRAM) {
                if (flags & FWD_SOCKET_FLAG_LISTEN) {
                    entry = fwd_entry_datagram_listen_new(fp, socket,
                        rid, backlog, timeout_ms * G_TIME_SPAN_MILLISECOND,
                        maxconn);
                } else {
                    entry = fwd_entry_datagram_socket_new(fp, socket,
                        NULL, rid);
                }
            }

            if (entry) {
#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    GDEBUG("[%u] Local address %s", entry->id,
                        fwd_format_sockaddr_data(addr));
                }
#endif /* GUTIL_LOG_DEBUG */

                /* Ownership of the new entry is transferred to FwdPeer */
                fwd_entry_insert(entry);
                fwd_control_service_socket_ok(req, entry->id, addr);
            }
            g_object_unref(socket);
        } else {
            fwd_control_service_socket_error(req,
                fwd_error_to_code(error, EINVAL));
        }
        g_free(addr);
    } else {
        fwd_control_service_socket_error(req, EINVAL);
    }
}

/*==========================================================================*
 * FwdSocketServiceHandler
 *==========================================================================*/

static
void
fwd_peer_handle_socket_accept(
    FwdSocketServiceHandler* handler,
    GIoRpcRequest* req,
    guint id)
{
    const gconstpointer key = GUINT_TO_POINTER(id);
    FwdPeerObject* self = fwd_socket_service_handler_cast(handler);
    FwdEntry* entry = g_hash_table_lookup(self->sockets, key);

    if (entry) {
        if (entry->type->accept) {
            entry->type->accept(entry, req);
            return;
        } else {
            GDEBUG("[%u] Unexpected accept request", id);
        }
    } else {
        GDEBUG("Invalid socket id %u", id);
    }

    /* Error path */
    fwd_socket_service_accept_error(req, EINVAL);
}

static
guint
fwd_peer_handle_socket_accepted(
    FwdSocketServiceHandler* handler,
    guint id,
    guint rid)
{
    const gconstpointer key = GUINT_TO_POINTER(id);
    FwdPeerObject* self = fwd_socket_service_handler_cast(handler);
    FwdEntry* entry = g_hash_table_lookup(self->sockets, key);

    if (entry) {
        if (entry->type->accepted) {
            return entry->type->accepted(entry, rid);
        } else {
            GDEBUG("[%u] Unexpected accept confirmation", id);
        }
    } else {
        GDEBUG("Invalid socket id %u", id);
    }

    /* Error path */
    return EINVAL;
}

static
void
fwd_peer_handle_socket_connect(
    FwdSocketServiceHandler* handler,
    GIoRpcRequest* req,
    guint id,
    GInetSocketAddress* to)
{
    const gconstpointer key = GUINT_TO_POINTER(id);
    FwdPeerObject* self = fwd_socket_service_handler_cast(handler);
    FwdEntry* entry = g_hash_table_lookup(self->sockets, key);

    if (entry) {
        if (entry->type->connect) {
            entry->type->connect(entry, req, to);
            return;
        } else {
            GDEBUG("[%u] Unexpected connect request", id);
        }
    } else {
        GDEBUG("Invalid socket id %u", id);
    }

    /* Error path */
    fwd_socket_service_connect_error(req, EINVAL);
}

static
void
fwd_peer_handle_socket_data(
    FwdSocketServiceHandler* handler,
    GIoRpcRequest* req,
    guint id,
    const GUtilData* data,
    GInetSocketAddress* from)
{
    const gconstpointer key = GUINT_TO_POINTER(id);
    FwdPeerObject* self = fwd_socket_service_handler_cast(handler);
    FwdEntry* entry = g_hash_table_lookup(self->sockets, key);

    if (entry) {
        if (entry->type->data) {
            entry->type->data(entry, req, data, from);
            return;
        } else {
            GDEBUG("[%u] Unexpected connect request", id);
        }
    } else {
        GDEBUG("Invalid socket id %u", id);
    }
}

static
void
fwd_peer_handle_socket_close(
    FwdSocketServiceHandler* handler,
    guint id)
{
    const gconstpointer key = GUINT_TO_POINTER(id);
    FwdPeerObject* self = fwd_socket_service_handler_cast(handler);
    FwdEntry* entry = g_hash_table_lookup(self->sockets, key);

    if (entry) {
        /* Entries without close callback can't be closed */
        if (entry->type->close) {
            entry->type->close(entry);
            if (g_hash_table_remove(self->sockets, key)) {
                g_signal_emit(self, fwd_signals
                    [SIGNAL_SOCKET_REMOVED], id, id);
            }
        } else {
            GDEBUG("[%u] Unexpected close request", id);
        }
    } else {
        GDEBUG("Invalid socket id %u", id);
    }
}

/*==========================================================================*
 * GObject
 *==========================================================================*/

static
void
fwd_peer_object_dispose(
    GObject* object)
{
    g_hash_table_remove_all(THIS(object)->sockets);
    G_OBJECT_CLASS(PARENT_CLASS)->dispose(object);
}

static
void
fwd_peer_object_finalize(
    GObject* object)
{
    FwdPeerObject* self = THIS(object);

    g_hash_table_destroy(self->sockets);
    fwd_control_service_free(self->control_service);
    fwd_socket_service_free(self->socket_service);
    giorpc_peer_remove_handler(self->rpc, self->rpc_state_event_id);
    giorpc_peer_unref(self->rpc);
    gutil_weakref_unref(self->ref);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
fwd_peer_object_init(
    FwdPeerObject* self)
{
    self->sockets = g_hash_table_new_full(g_direct_hash, g_direct_equal,
        NULL, fwd_entry_dispose_cb);
}

static
void
fwd_peer_object_class_init(
    FwdPeerObjectClass* klass)
{
    const GType type = G_OBJECT_CLASS_TYPE(klass);
    GObjectClass* object = G_OBJECT_CLASS(klass);

    object->dispose = fwd_peer_object_dispose;
    object->finalize = fwd_peer_object_finalize;

    fwd_signals[SIGNAL_STATE_CHANGED] =
        g_signal_new(SIGNAL_STATE_CHANGED_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 0);
    fwd_signals[SIGNAL_SOCKET_ADDED] =
        g_signal_new(SIGNAL_SOCKET_ADDED_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_INT);
    fwd_signals[SIGNAL_SOCKET_REMOVED] =
        g_signal_new(SIGNAL_SOCKET_REMOVED_NAME, type,
            G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_UINT);
    fwd_signals[SIGNAL_SOCKET_STATE_CHANGED] =
        g_signal_new(SIGNAL_SOCKET_STATE_CHANGED_NAME, type,
            G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_INT);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

GIoRpcPeer*
fwd_peer_rpc(
    FwdPeer* fp)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    return self ? self->rpc : NULL;
}

GIoRpcPeer*
fwd_peer_rpc_ref(
    FwdPeer* fp)
{
    return giorpc_peer_ref(fwd_peer_rpc(fp));
}

guint
fwd_peer_next_id(
    FwdPeer* fp)
{
    static gint next_id = 0;
    FwdPeerObject* self = fwd_peer_cast(fp);
    guint id = g_atomic_int_add(&next_id, 1) & FWD_SOCKET_ID_MASK;

    while (!id || g_hash_table_contains(self->sockets, GUINT_TO_POINTER(id))) {
        id = g_atomic_int_add(&next_id, 1) & FWD_SOCKET_ID_MASK;
    }

    return id;
}

guint
fwd_entry_insert(
    FwdEntry* entry)
{
    if (entry) {
        const guint id = entry->id;
        FwdPeerObject* self = fwd_peer_cast(entry->owner);

        /* Ownership is transferred to FwdPeer */
        g_hash_table_insert(self->sockets, GUINT_TO_POINTER(id), entry);
        g_signal_emit(self, fwd_signals
            [SIGNAL_SOCKET_ADDED], 0, id, entry->state);

        /* Wait for completion of the INFO call before starting */
        if (entry->owner->state == FWD_STATE_STARTED) {
            if (entry->type->start) {
                entry->type->start(entry);
            }
        }
        return id;
    } else {
        return 0;
    }
}

void
fwd_entry_remove(
    FwdEntry* entry)
{
    FwdPeerObject* self = fwd_peer_cast(entry->owner);
    const guint id = entry->id;

    if (g_hash_table_remove(self->sockets, GUINT_TO_POINTER(id))) {
        g_signal_emit(self, fwd_signals[SIGNAL_SOCKET_REMOVED], id, id);
    }
}

void
fwd_entry_set_state(
    FwdEntry* entry,
    FWD_SOCKET_STATE state)
{
    if (entry->state != state) {
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] %s => %s", entry->id,
                fwd_socket_state_name(entry->state),
                fwd_socket_state_name(state));
        }
#endif /* GUTIL_LOG_DEBUG */
        entry->state = state;
        g_signal_emit(fwd_peer_cast(entry->owner), fwd_signals
            [SIGNAL_SOCKET_STATE_CHANGED], entry->id, entry->id, state);
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

FwdPeer*
fwd_peer_new(
    GIOStream* stream,
    GMainContext* context)
{
    GIoRpcPeer* rpc = giorpc_peer_new(stream, context);

    if (rpc) {
         FwdPeer* fp = fwd_peer_new_with_rpc(rpc);

         giorpc_peer_start(rpc);
         giorpc_peer_unref(rpc);
         return fp;
    }
    return NULL;
}

FwdPeer*
fwd_peer_new_with_rpc(
    GIoRpcPeer* rpc)
{
    static const FwdControlServiceCallbacks control_service_callbacks = {
        fwd_peer_handle_control_socket
    };
    static const FwdSocketServiceCallbacks socket_service_callbacks = {
        fwd_peer_handle_socket_accept,
        fwd_peer_handle_socket_accepted,
        fwd_peer_handle_socket_connect,
        fwd_peer_handle_socket_data,
        fwd_peer_handle_socket_close
    };

    if (G_LIKELY(rpc)) {
        if (g_main_context_acquire(rpc->context)) {
            FwdPeerObject* self = g_object_new(THIS_TYPE, NULL);
            FwdPeer* fp = &self->pub;

            self->ref = gutil_weakref_new(self);
            self->rpc = giorpc_peer_ref(rpc);

            /* FwdControlServiceHandler */
            self->control_handler.cb = &control_service_callbacks;
            self->control_service = fwd_control_service_new(rpc,
                &self->control_handler);

            /* FwdSocketServiceHandler */
            self->socket_handler.cb = &socket_service_callbacks;
            self->socket_service = fwd_socket_service_new(rpc,
                &self->socket_handler);

            if (rpc->state > GIORPC_PEER_STATE_STOPPING) {
                fp->state = FWD_STATE_STOPPED;
            } else {
                /* Watch the RPC state */
                self->rpc_state_event_id = giorpc_peer_add_state_handler(rpc,
                    fwd_control_handle_peer_state, self);
                if (rpc->state == GIORPC_PEER_STATE_STARTED) {
                    fwd_control_start(self);
                }
            }
            g_main_context_release(rpc->context);
            return fp;
        }
    }
    return NULL;
}

FwdPeer*
fwd_peer_ref(
    FwdPeer* fp)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return fp;
}

void
fwd_peer_unref(
    FwdPeer* fp)
{
    gutil_object_unref(fwd_peer_cast(fp));
}

gboolean
fwd_peer_sync(
    FwdPeer* fp,
    guint timeout_ms,
    GError** error)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    if (self) {
        GBytes* in = g_bytes_new(NULL, 0);
        GBytes* out;

        GDEBUG("< ECHO");
        out = giorpc_peer_call_sync(self->rpc, FWD_IID_CONTROL,
        FWD_CONTROL_ECHO, in, GIORPC_SYNC_FLAG_BLOCK_ALL,
            timeout_ms, NULL, error);

        g_bytes_unref(in);
        if (out) {
            g_bytes_unref(out);
            return TRUE;
        }
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid argument");
    }
    return FALSE;
}

guint
fwd_peer_add_local_datagram_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* to,
    guint timeout_ms,
    guint conn_limit,
    FWD_FLAGS flags,
    GError** error)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    if (G_LIKELY(self) && G_LIKELY(to)) {
        GIoRpcPeer* rpc = self->rpc;

        if (g_main_context_acquire(rpc->context)) {
            guint id = 0;
            GInetSocketAddress* isa = fwd_inet_socket_address_new(af, port,
                (flags & FWD_FLAG_ALLOW_REMOTE) != 0);
            GUtilData* sa = fwd_inet_socket_address_to_native(isa, error);
            GSocket* socket = fwd_socket_new_from_data(G_SOCKET_TYPE_DATAGRAM,
                sa, (flags & FWD_FLAG_REUSE_ADDRESS) != 0, FALSE, error);

            if (socket) {
#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    GDEBUG("Created socket %s", fwd_format_sockaddr_data(sa));
                }
#endif /* GUTIL_LOG_DEBUG */

                /* Ownership of the new entry is transferred to FwdPeer */
                id = fwd_entry_insert(fwd_entry_datagram_forward_local_new
                    (fp, socket, to, timeout_ms, conn_limit, error));
                g_object_unref(socket);
            }

            g_main_context_release(rpc->context);
            g_object_unref(isa);
            g_free(sa);
            return id;
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Invalid context");
        }
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid arguments");
    }
    return 0;
}

guint
fwd_peer_add_local_stream_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* to,
    int backlog,
    FWD_FLAGS flags,
    GError** error)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    if (G_LIKELY(self) && G_LIKELY(to)) {
        GIoRpcPeer* rpc = self->rpc;

        if (g_main_context_acquire(rpc->context)) {
            guint id = 0;
            GInetSocketAddress* isa = fwd_inet_socket_address_new(af, port,
                (flags & FWD_FLAG_ALLOW_REMOTE) != 0);
            GUtilData* sa = fwd_inet_socket_address_to_native(isa, error);
            GSocket* socket = fwd_socket_new_from_data(G_SOCKET_TYPE_STREAM,
                sa, (flags & FWD_FLAG_REUSE_ADDRESS) != 0, FALSE, error);

            if (socket) {
#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    GDEBUG("Created socket %s", fwd_format_sockaddr_data(sa));
                }
#endif /* GUTIL_LOG_DEBUG */

                /* Ownership of the new entry is transferred to FwdPeer */
                id = fwd_entry_insert(fwd_entry_stream_forward_local_new
                    (fp, socket, to, backlog, error));
                g_object_unref(socket);
            }

            g_main_context_release(rpc->context);
            g_object_unref(isa);
            g_free(sa);
            return id;
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Invalid context");
        }
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid arguments");
    }
    return 0;
}

guint
fwd_peer_add_remote_stream_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* to,
    int backlog,
    FWD_FLAGS flags,
    GError** error)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    if (G_LIKELY(self) && G_LIKELY(to)) {
        GIoRpcPeer* rpc = self->rpc;

        if (g_main_context_acquire(rpc->context)) {
            GInetSocketAddress* remote = fwd_inet_socket_address_new(af, port,
                (flags & FWD_FLAG_ALLOW_REMOTE) != 0);
            guint id = fwd_entry_insert(fwd_entry_stream_forward_remote_new(fp,
                remote, to, (flags &  FWD_FLAG_REUSE_ADDRESS) != 0, backlog,
                error));

            g_main_context_release(rpc->context);
            g_object_unref(remote);
            return id;
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Invalid context");
        }
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid arguments");
    }
    return 0;
}

guint
fwd_peer_add_remote_datagram_forwarder(
    FwdPeer* fp,
    GSocketFamily af,
    gushort port,
    GInetSocketAddress* to,
    int backlog,
    guint timeout_ms,
    guint conn_limit,
    FWD_FLAGS flags,
    GError** error)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    if (G_LIKELY(self) && G_LIKELY(to)) {
        GIoRpcPeer* rpc = self->rpc;

        if (g_main_context_acquire(rpc->context)) {
            GInetSocketAddress* remote = fwd_inet_socket_address_new(af, port,
                (flags & FWD_FLAG_ALLOW_REMOTE) != 0);
            guint id = fwd_entry_insert(fwd_entry_datagram_forward_remote_new
                (fp, remote, to, (flags &  FWD_FLAG_REUSE_ADDRESS) != 0,
                    backlog, timeout_ms, conn_limit, error));

            g_main_context_release(rpc->context);
            g_object_unref(remote);
            return id;
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Invalid context");
        }
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid arguments");
    }
    return 0;
}

void
fwd_peer_remove_forwarder(
    FwdPeer* fp,
    guint id)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    if (self && g_hash_table_remove(self->sockets, GUINT_TO_POINTER(id))) {
        g_signal_emit(self, fwd_signals[SIGNAL_SOCKET_REMOVED], id, id);
    }
}

gulong
fwd_peer_add_state_handler(
    FwdPeer* fp,
    FwdPeerFunc fn,
    gpointer user_data)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    return (G_LIKELY(self) && G_LIKELY(fn)) ?
        g_signal_connect_closure_by_id(self, fwd_signals
            [SIGNAL_STATE_CHANGED], 0, fwd_closure_new(
            G_CALLBACK(fwd_closure_peer_callback),
            G_CALLBACK(fn), user_data), FALSE) : 0;
}

gulong
fwd_peer_add_socket_added_handler(
    FwdPeer* fp,
    FwdSocketStateFunc fn,
    gpointer user_data)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    return (G_LIKELY(self) && G_LIKELY(fn)) ?
        g_signal_connect_closure_by_id(self, fwd_signals
            [SIGNAL_SOCKET_ADDED], 0, fwd_closure_new(
            G_CALLBACK(fwd_closure_socket_state_callback),
            G_CALLBACK(fn), user_data), FALSE) : 0;
}

gulong
fwd_peer_add_socket_removed_handler(
    FwdPeer* fp,
    guint id,
    FwdSocketFunc fn,
    gpointer user_data)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    /*
     * Note: we are using id as a quark. Converting socket id into a real
     * quark would fill up the static quark table and eventually eat up
     * all the memory in thew world. Real quarks are only necessary when
     * detailed signal names are being parsed (which we don't need).
     */
    return (G_LIKELY(self) && G_LIKELY(fn)) ?
        g_signal_connect_closure_by_id(self, fwd_signals
            [SIGNAL_SOCKET_REMOVED], id, fwd_closure_new(
            G_CALLBACK(fwd_closure_socket_callback),
            G_CALLBACK(fn), user_data), FALSE) : 0;
}

gulong
fwd_peer_add_socket_state_handler(
    FwdPeer* fp,
    guint id,
    FwdSocketStateFunc fn,
    gpointer user_data)
{
    FwdPeerObject* self = fwd_peer_cast(fp);

    /*
     * Note: we are using id as a quark. Converting socket id into a real
     * quark would fill up the static quark table and eventually eat up
     * all the memory in thew world. Real quarks are only necessary when
     * detailed signal names are being parsed (which we don't need).
     */
    return (G_LIKELY(self) && G_LIKELY(fn)) ?
        g_signal_connect_closure_by_id(self, fwd_signals
            [SIGNAL_SOCKET_STATE_CHANGED], id, fwd_closure_new(
            G_CALLBACK(fwd_closure_socket_state_callback),
            G_CALLBACK(fn), user_data), FALSE) : 0;
}

void
fwd_peer_remove_handler(
    FwdPeer* fp,
    gulong id)
{
    if (G_LIKELY(id)) {
        FwdPeerObject* self = fwd_peer_cast(fp);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
fwd_peer_clear_handlers(
    FwdPeer* fp,
    gulong* ids,
    int count)
{
    gutil_disconnect_handlers(fwd_peer_cast(fp), ids, count);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
