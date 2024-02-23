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

#include "fwd_entry_stream_forward_local.h"

#include "fwd_entry.h"
#include "fwd_log_p.h"
#include "fwd_op_connect_remote.h"
#include "fwd_util_p.h"

#include <gutil_macros.h>
#include <gutil_misc.h>

typedef struct fwd_entry_stream_forward_local {
    FwdEntry entry;
    GSocket* socket;
    GCancellable* cancel;
    GSocketListener* listener;
    GInetSocketAddress* to;
} FwdEntryStreamForwardLocal;

static inline FwdEntryStreamForwardLocal*
fwd_entry_stream_forward_local_cast(FwdEntry* entry)
    { return G_CAST(entry, FwdEntryStreamForwardLocal, entry); }

static
void
fwd_entry_stream_forward_local_connection_accepted(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    GError* error = NULL;
    FwdEntryStreamForwardLocal* self = user_data;
    FwdEntry* entry = &self->entry;
    GSocketListener* listener = G_SOCKET_LISTENER(object);
    GSocket* s = g_socket_listener_accept_socket_finish(listener, result,
        NULL, &error);

    if (s) {
        FwdPeer* owner = entry->owner;

#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] Incoming connection from %s", entry->id,
                fwd_format_socket_remote_address(s));
        }
#endif /* GUTIL_LOG_DEBUG */

        if (owner) {
            fwd_op_connect_remote_start(entry, s, self->to);

            /* Accept the next one */
            g_socket_listener_accept_async(listener, self->cancel,
                fwd_entry_stream_forward_local_connection_accepted,
                fwd_entry_ref(entry));
        }
        g_object_unref(s);
    } else {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            GERR("[%u] Listening error: %s", entry->id, GERRMSG(error));
        }
        g_error_free(error);
    }
    fwd_entry_unref(entry);
}

/*==========================================================================*
 * FwdEntry
 *==========================================================================*/

static
void
fwd_entry_stream_forward_local_start(
    FwdEntry* entry)
{
    FwdEntryStreamForwardLocal* self =
        fwd_entry_stream_forward_local_cast(entry);

    g_socket_listener_accept_async(self->listener, self->cancel,
        fwd_entry_stream_forward_local_connection_accepted,
        fwd_entry_ref(entry));
}

static
void
fwd_entry_stream_forward_local_dispose(
    FwdEntry* entry)
{
    FwdEntryStreamForwardLocal* self =
        fwd_entry_stream_forward_local_cast(entry);

    g_cancellable_cancel(self->cancel);
    g_socket_listener_close(self->listener);
    fwd_entry_base_dispose(entry);
}

static
void
fwd_entry_stream_forward_local_free(
    FwdEntry* entry)
{
    FwdEntryStreamForwardLocal* self =
        fwd_entry_stream_forward_local_cast(entry);

    g_object_unref(self->to);
    g_object_unref(self->cancel);
    g_object_unref(self->socket);
    g_object_unref(self->listener);
    fwd_entry_base_destroy(entry);
    gutil_slice_free(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdEntry*
fwd_entry_stream_forward_local_new(
    FwdPeer* owner,
    GSocket* socket,
    GInetSocketAddress* to,
    int backlog,
    GError** error)
{
    static const FwdEntryType stream_forward_local_type = {
        fwd_entry_stream_forward_local_start,
        NULL, /* accept */
        NULL, /* accepted */
        NULL, /* connect */
        NULL, /* data */
        NULL, /* close */
        fwd_entry_stream_forward_local_dispose,
        fwd_entry_stream_forward_local_free
    };

    if (g_socket_listen(socket, error)) {
        GSocketListener* listener = g_socket_listener_new();

        if (backlog >= 0) {
            g_socket_listener_set_backlog(listener, backlog);
        }

        if (g_socket_listener_add_socket(listener, socket, NULL, error)) {
            FwdEntryStreamForwardLocal* self =
                g_slice_new0(FwdEntryStreamForwardLocal);
            FwdEntry* entry = &self->entry;

            fwd_entry_base_init(entry, &stream_forward_local_type, owner,
                FWD_SOCKET_STATE_LISTEN_LOCAL);
            self->cancel = g_cancellable_new();
            self->listener = listener;  /* Grab the ref */
            g_object_ref(self->to = to);
            g_object_ref(self->socket = socket);
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
