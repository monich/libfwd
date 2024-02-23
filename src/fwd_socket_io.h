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

#ifndef FWD_SOCKET_IO_H
#define FWD_SOCKET_IO_H

#include "fwd_types_p.h"

#include <gio/gio.h>

typedef struct fwd_socket_io_callbacks FwdSocketIoCallbacks;

typedef struct fwd_socket_io_handler {
    const FwdSocketIoCallbacks* cb;
} FwdSocketIoHandler;

struct fwd_socket_io_callbacks {
    void (*error)(FwdSocketIoHandler* handler, const GError* error);
    void (*receive)(FwdSocketIoHandler* handler, GInetSocketAddress* from,
        const GUtilData* data);
};

typedef
void
(*FwdSocketIoSentFunc)(
    gsize sent,
    const GError* error,
    void* user_data);

FwdSocketIo*
fwd_socket_io_new(
    GSocket* socket,
    GMainContext* context,
    FwdSocketIoHandler* handler)
    G_GNUC_INTERNAL;

void
fwd_socket_io_free(
    FwdSocketIo* io)
    G_GNUC_INTERNAL;

void
fwd_socket_io_clear(
    FwdSocketIo** io)
    G_GNUC_INTERNAL;

void
fwd_socket_io_start_receive(
    FwdSocketIo* io)
    G_GNUC_INTERNAL;

void
fwd_socket_io_stop_receive(
    FwdSocketIo* io)
    G_GNUC_INTERNAL;

void
fwd_socket_io_send(
    FwdSocketIo* io,
    GInetSocketAddress* to,
    GBytes* data,
    FwdSocketIoSentFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
    G_GNUC_INTERNAL;

#endif /* GIORPC_SOCKET_IO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
