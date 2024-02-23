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

#include "fwd_socket_io.h"
#include "fwd_log_p.h"

#include <gutil_macros.h>
#include <gutil_misc.h>

#define FWD_SOCKET_IO_MAX_READ (64 * 1024)

struct fwd_socket_io {
    gint ref_count;
    GSocket* socket;
    GMainContext* context;
    FwdSocketIoHandler* handler;
    GSource* error_source;
    GSource* read_source;
    GSource* write_source;
    void* read_buf;
    guint read_bufsize;
    GQueue writeq;
};

typedef struct fwd_socket_io_send_data {
    GBytes* bytes;
    GInetSocketAddress* to;
    FwdSocketIoSentFunc complete;
    GDestroyNotify destroy;
    gpointer user_data;
    gsize sent;
} FwdSocketIoSendData;

static
FwdSocketIoSendData*
fwd_socket_io_send_data_new(
    GBytes* bytes,
    GInetSocketAddress* to,
    FwdSocketIoSentFunc complete,
    gpointer user_data,
    GDestroyNotify destroy)
{
    FwdSocketIoSendData* data = g_slice_new(FwdSocketIoSendData);

    data->to = to;
    data->bytes = g_bytes_ref(bytes);
    data->complete = complete;
    data->destroy = destroy;
    data->user_data = user_data;
    data->sent = 0;
    if (to) {
        g_object_ref(to);
    }
    return data;
}

static
void
fwd_socket_io_send_data_free(
    FwdSocketIoSendData* data)
{
    gutil_object_unref(data->to);
    g_bytes_unref(data->bytes);
    if (data->destroy) {
        data->destroy(data->user_data);
    }
    gutil_slice_free(data);
}

static
gboolean
fwd_socket_io_clear_source(
    GSource** source)
{
    if (*source) {
        g_source_destroy(*source);
        g_source_unref(*source);
        *source = NULL;
        return TRUE;
    } else {
        return FALSE;
    }
}

static
void
fwd_socket_io_dispose(
    FwdSocketIo* self)
{
    fwd_socket_io_clear_source(&self->error_source);
    fwd_socket_io_clear_source(&self->read_source);
    fwd_socket_io_clear_source(&self->write_source);
    while (!g_queue_is_empty(&self->writeq)) {
        fwd_socket_io_send_data_free(g_queue_pop_head(&self->writeq));
    }
    g_queue_clear(&self->writeq);
}

static
void
fwd_socket_io_finalize(
    FwdSocketIo* self)
{
    fwd_socket_io_dispose(self);
    g_free(self->read_buf);
    g_main_context_unref(self->context);
    g_object_unref(self->socket);
}

static
FwdSocketIo*
fwd_socket_io_ref(
    FwdSocketIo* self)
{
    self->ref_count++;
    return self;
}

static
void
fwd_socket_io_unref(
    FwdSocketIo* self)
{
    self->ref_count--;
    if (!self->ref_count) {
        fwd_socket_io_finalize(self);
        gutil_slice_free(self);
    }
}

static
gboolean
fwd_socket_io_handle_read(
    FwdSocketIo* self,
    GIOCondition condition)
{
    if (condition & G_IO_IN) {
        FwdSocketIoHandler* handler = self->handler;
        GSocketAddress* from = NULL;
        GError* error = NULL;
        gssize received;

        if (!self->read_buf) {
            self->read_buf = g_malloc(self->read_bufsize);
        }
        received = g_socket_receive_from(self->socket, &from,
            self->read_buf, self->read_bufsize, NULL, &error);
        if (received > 0) {
            GUtilData data;

            data.bytes = self->read_buf;
            data.size = received;
            handler->cb->receive(handler, G_INET_SOCKET_ADDRESS(from), &data);
        }
        gutil_object_unref(from);
        if (received > 0) {
            return G_SOURCE_CONTINUE;
        } else {
            /*
             * g_socket_receive_from() returns number of bytes read,
             * or 0 if the connection was closed by the peer, or -1
             * on error.
             */
            if (fwd_socket_io_clear_source(&self->error_source)) {
                handler->cb->error(handler, error);
            }
            if (error) {
                g_error_free(error);
            }
        }
    }

    fwd_socket_io_clear_source(&self->read_source);
    return G_SOURCE_REMOVE;
}

static
gboolean
fwd_socket_io_handle_write(
    FwdSocketIo* self,
    GIOCondition condition)
{
    if ((condition & G_IO_OUT) && !g_queue_is_empty(&self->writeq)) {
        FwdSocketIoSendData* data = g_queue_peek_head(&self->writeq);
        GSocketAddress* sa = (data->to ? G_SOCKET_ADDRESS(data->to) : NULL);
        GError* error = NULL;
        gsize size;
        const gchar* buffer = g_bytes_get_data(data->bytes, &size);
        const gssize sent = g_socket_send_to(self->socket, sa,
            buffer + data->sent, size - data->sent, NULL, &error);

        if (sent >= 0) {
            data->sent += sent;
            if (data->sent == size) {
                /* The whole buffer has been written */
                g_queue_pop_head(&self->writeq);
                if (data->complete) {
                    data->complete(size, NULL, data->user_data);
                }
                fwd_socket_io_send_data_free(data);
            }
            if (!g_queue_is_empty(&self->writeq)) {
                return G_SOURCE_CONTINUE;
            }
        } else {
            FwdSocketIoHandler* handler = self->handler;

            g_queue_pop_head(&self->writeq);
            if (data->complete) {
                data->complete(data->sent, error, data->user_data);
            }
            if (fwd_socket_io_clear_source(&self->error_source)) {
                handler->cb->error(handler, error);
            }
            g_error_free(error);
            fwd_socket_io_send_data_free(data);
        }
    }

    fwd_socket_io_clear_source(&self->write_source);
    return G_SOURCE_REMOVE;
}

static
gboolean
fwd_socket_io_read_source_cb(
    GSocket* socket,
    GIOCondition condition,
    gpointer user_data)
{
    FwdSocketIo* self = fwd_socket_io_ref(user_data);
    const gboolean result = fwd_socket_io_handle_read(self, condition);

    fwd_socket_io_unref(self);
    return result;
}

static
gboolean
fwd_socket_io_write_source_cb(
    GSocket* socket,
    GIOCondition condition,
    gpointer user_data)
{
    FwdSocketIo* self = fwd_socket_io_ref(user_data);
    const gboolean result = fwd_socket_io_handle_write(self, condition);

    fwd_socket_io_unref(self);
    return result;
}

static
gboolean
fwd_socket_io_error_source_cb(
    GSocket* socket,
    GIOCondition condition,
    gpointer user_data)
{
    FwdSocketIo* self = fwd_socket_io_ref(user_data);
    FwdSocketIoHandler* handler = self->handler;

    g_source_unref(self->error_source);
    self->error_source = NULL;
    handler->cb->error(handler, NULL);
    fwd_socket_io_unref(self);
    return G_SOURCE_REMOVE;
}

static
GSource*
fwd_socket_io_attach_source(
    FwdSocketIo* self,
    GIOCondition condition,
    GSocketSourceFunc fn)
{
    GSource* source = g_socket_create_source(self->socket, condition, NULL);

    g_source_set_callback(source, (GSourceFunc) fn, self, NULL);
    g_source_attach(source, self->context);
    return source;
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdSocketIo*
fwd_socket_io_new(
    GSocket* socket,
    GMainContext* context,
    FwdSocketIoHandler* handler)
{
    FwdSocketIo* self = g_slice_new0(FwdSocketIo);

    g_queue_init(&self->writeq);
    g_object_ref(self->socket = socket);
    self->context = g_main_context_ref(context);
    self->handler = handler;
    self->read_bufsize = FWD_SOCKET_IO_MAX_READ;
    self->ref_count = 1;

    /* Error source is always there, even when nothing is read or written */
    self->error_source = fwd_socket_io_attach_source(self,
        G_IO_HUP | G_IO_ERR | G_IO_NVAL, fwd_socket_io_error_source_cb);
    return self;
}

void
fwd_socket_io_free(
    FwdSocketIo* self)
{
    if (self) {
        fwd_socket_io_dispose(self);
        fwd_socket_io_unref(self);
    }
}

void
fwd_socket_io_clear(
    FwdSocketIo** io)
{
    if (*io) {
        fwd_socket_io_free(*io);
        *io = NULL;
    }
}

void
fwd_socket_io_start_receive(
    FwdSocketIo* self)
{
    if (!self->read_source) {
        self->read_source = fwd_socket_io_attach_source(self, G_IO_IN,
            fwd_socket_io_read_source_cb);
    }
}

void
fwd_socket_io_stop_receive(
    FwdSocketIo* self)
{
    if (self->read_source) {
        g_free(self->read_buf);
        g_source_destroy(self->read_source);
        g_source_unref(self->read_source);
        self->read_source = NULL;
        self->read_buf = NULL;
    }
}

void
fwd_socket_io_send(
    FwdSocketIo* self,
    GInetSocketAddress* to,
    GBytes* bytes,
    FwdSocketIoSentFunc complete,
    gpointer user_data,
    GDestroyNotify destroy)
{
    g_queue_push_tail(&self->writeq, fwd_socket_io_send_data_new(bytes, to,
        complete, user_data, destroy));
    if (!self->write_source) {
        self->write_source = fwd_socket_io_attach_source(self, G_IO_OUT,
            fwd_socket_io_write_source_cb);
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
