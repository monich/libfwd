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

#include "fwd_entry_stream_listen.h"

#include "fwd_entry.h"
#include "fwd_entry_stream_socket.h"
#include "fwd_log_p.h"
#include "fwd_op_connect_remote.h"
#include "fwd_peer_p.h"
#include "fwd_socket_client.h"
#include "fwd_socket_service.h"
#include "fwd_util_p.h"

#include <giorpc_request.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

typedef struct fwd_entry_stream_listen {
    FwdEntry entry;
    guint rid;
    GSocket* socket;
    GSocketListener* listener;
    GQueue accept_reqs;
    GCancellable* cancel;
} FwdEntryStreamListen;

typedef struct fwd_entry_stream_accept_req {
    GList link;
    FwdEntryStreamListen* listen;
    GIoRpcRequest* req;
    gulong req_cancel_id;
} FwdEntryStreamAcceptReq;

static inline FwdEntryStreamListen*
fwd_entry_stream_listen_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryStreamListen, entry); }

/*==========================================================================*
 * FwdEntryStreamAcceptReq
 *==========================================================================*/

static
void
fwd_entry_stream_accept_req_done(
    FwdEntryStreamAcceptReq* accept,
    void (*drop_req)(GIoRpcRequest* req))
{
    if (accept->req) {
        giorpc_request_remove_cancel_handler(accept->req,
            accept->req_cancel_id);
        drop_req(accept->req);
        accept->req_cancel_id = 0;
        accept->req = NULL;
    }
}

static
void
fwd_entry_stream_accept_req_free(
    FwdEntryStreamAcceptReq* accept)
{
    fwd_entry_stream_accept_req_done(accept, giorpc_request_unref);
    gutil_slice_free(accept);
}

static
void
fwd_entry_stream_accept_req_cancelled(
    GIoRpcRequest* req,
    gpointer user_data)
{
    FwdEntryStreamAcceptReq* accept = user_data;
    FwdEntryStreamListen* listen = accept->listen;

    /* The remote side has cancelled its ACCEPT request */
    g_queue_unlink(&listen->accept_reqs, &accept->link);
    fwd_entry_stream_accept_req_done(accept, giorpc_request_drop);
    gutil_slice_free(accept);

    /* If there are no accept requests left, cancel the async accept */
    if (g_queue_is_empty(&listen->accept_reqs)) {
        g_cancellable_cancel(listen->cancel);
        listen->cancel = NULL;
    }
}

static
FwdEntryStreamAcceptReq*
fwd_entry_stream_accept_req_new(
    FwdEntryStreamListen* listen,
    GIoRpcRequest* req)
{
    FwdEntryStreamAcceptReq* accept =
        g_slice_new0(FwdEntryStreamAcceptReq);

    accept->link.data = accept;
    accept->listen = listen;
    accept->req = giorpc_request_ref(req);
    accept->req_cancel_id = giorpc_request_add_cancel_handler(req,
        fwd_entry_stream_accept_req_cancelled, accept);
    return accept;
}

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
void
fwd_entry_stream_listen_accept_complete(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    FwdEntryStreamListen* self = user_data;
    FwdEntry* entry = &self->entry;
    GError* error = NULL;
    GSocketListener* listener = G_SOCKET_LISTENER(object);
    GSocket* s = g_socket_listener_accept_socket_finish(listener, result,
        NULL, &error);

    if (s) {
        /* Something must be there in the accept queue */
        FwdEntryStreamAcceptReq* accept =
            g_queue_pop_head_link(&self->accept_reqs)->data;
        GSocketAddress* src = g_socket_get_remote_address(s, NULL);
        GSocketAddress* dest = g_socket_get_local_address(s, NULL);
        FwdEntryStreamSocket* ss =
            fwd_entry_stream_socket_new(entry->owner, s, 0);

        /* Transfer ownership of the new entry to FwdPeer */
        fwd_entry_insert(&ss->entry);
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] Incoming connection %u from %s", entry->id,
                ss->entry.id, fwd_format_socket_address(src));
        }
#endif /* GUTIL_LOG_DEBUG */

        /*
         * Complete one pending ACCEPT and expect the other side to
         * confirm the connection with ACCEPTED or refuse it with CLOSE.
         */
        if (accept->req) {
            fwd_socket_service_accept_ok(accept->req, ss->entry.id,
                G_INET_SOCKET_ADDRESS(src), G_INET_SOCKET_ADDRESS(dest));
        }
        fwd_entry_stream_accept_req_free(accept);
        gutil_object_unref(src);
        gutil_object_unref(dest);
        g_object_unref(s);
    } else {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            GERR("[%u] Listening error: %s", entry->id, GERRMSG(error));
        }
        g_error_free(error);
    }

    if (self->cancel) {
        if (!g_queue_is_empty(&self->accept_reqs)) {
            g_socket_listener_accept_socket_async(self->listener, self->cancel,
                fwd_entry_stream_listen_accept_complete, fwd_entry_ref(entry));
        } else {
            /* There are no more listen requests */
            g_object_unref(self->cancel);
            self->cancel = NULL;
        }
    }

    fwd_entry_unref(entry);
}

/*==========================================================================*
 * FwdEntry
 *==========================================================================*/

static
void
fwd_entry_stream_listen_accept(
   FwdEntry* entry,
   GIoRpcRequest* req)
{
    FwdEntryStreamListen* self = fwd_entry_stream_listen_cast(entry);

    /* Push the new entry to the list of accept requests */
    g_queue_push_tail_link(&self->accept_reqs,
        &fwd_entry_stream_accept_req_new(self, req)->link);

    /*
     * GCancellable doubles as the flag indicating whether asynchronous
     * accept is pending. If it's already pending, there's nothing to
     * be done here.
     */
    if (!self->cancel) {
        self->cancel = g_cancellable_new();
        g_socket_listener_accept_socket_async(self->listener, self->cancel,
            fwd_entry_stream_listen_accept_complete, fwd_entry_ref(entry));
    }
}

static
void
fwd_entry_stream_listen_close(
    FwdEntry* entry)
{
    FwdEntryStreamListen* self = fwd_entry_stream_listen_cast(entry);

    GDEBUG("[%u] Remote listening socket %u is gone", entry->id, self->rid);
    self->rid = 0;
    fwd_entry_remove(entry);
}

static
void
fwd_entry_stream_listen_dispose(
    FwdEntry* entry)
{
    FwdEntryStreamListen* self = fwd_entry_stream_listen_cast(entry);
    GList* accept_link;

    /* Complete pending RPC requests */
    while ((accept_link = g_queue_pop_head_link(&self->accept_reqs)) != NULL) {
        fwd_entry_stream_accept_req_free(accept_link->data);
    }

    if (self->rid) {
        GDEBUG("[%u] Closing remote socket %u", entry->id, self->rid);
        fwd_socket_call_close(fwd_entry_rpc(entry), self->rid);
        self->rid = 0;
    }

    if (self->cancel) {
        g_cancellable_cancel(self->cancel);
        self->cancel = NULL;
    }
    g_socket_listener_close(self->listener);
    fwd_entry_base_dispose(entry);
}

static
void
fwd_entry_stream_listen_free(
    FwdEntry* entry)
{
    FwdEntryStreamListen* self =
        fwd_entry_stream_listen_cast(entry);

    /* Accept request queue is cleared by the dispose callback */
    GASSERT(g_queue_is_empty(&self->accept_reqs));
    g_object_unref(self->socket);
    g_object_unref(self->listener);
    gutil_object_unref(self->cancel);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdEntry*
fwd_entry_stream_listen_new(
    FwdPeer* owner,
    GSocket* socket,
    guint rid,
    int backlog,
    GError** error)
{
    static const FwdEntryType stream_listen_entry_type = {
        NULL, /* start */
        fwd_entry_stream_listen_accept,
        NULL, /* accepted */
        NULL, /* connect */
        NULL, /* data */
        fwd_entry_stream_listen_close,
        fwd_entry_stream_listen_dispose,
        fwd_entry_stream_listen_free
    };

    if (g_socket_listen(socket, error)) {
        GSocketListener* listener = g_socket_listener_new();

        if (backlog >= 0) {
            g_socket_listener_set_backlog(listener, backlog);
        }

        if (g_socket_listener_add_socket(listener, socket, NULL, error)) {
            FwdEntryStreamListen* self = g_slice_new0(FwdEntryStreamListen);
            FwdEntry* entry = &self->entry;

            fwd_entry_base_init(entry, &stream_listen_entry_type, owner,
                FWD_SOCKET_STATE_LISTEN_LOCAL);
            self->listener = listener;  /* Grab the ref */
            self->rid = rid;
            g_object_ref(self->socket = socket);
            g_queue_init(&self->accept_reqs);
            return entry;
        }

        g_object_unref(listener);
    }
    return NULL;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
