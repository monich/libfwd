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

#ifndef FWD_SOCKET_SERVICE_H
#define FWD_SOCKET_SERVICE_H

#include "fwd_types_p.h"

#include <gio/gio.h>

typedef struct fwd_socket_service FwdSocketService;
typedef struct fwd_socket_service_callbacks FwdSocketServiceCallbacks;

typedef struct fwd_socket_service_handler {
    const FwdSocketServiceCallbacks* cb;
} FwdSocketServiceHandler;

struct fwd_socket_service_callbacks {
    void (*accept)(FwdSocketServiceHandler* handler, GIoRpcRequest* req,
        guint id);
    guint (*accepted)(FwdSocketServiceHandler* handler, guint id, guint rid);
    void (*connect)(FwdSocketServiceHandler* handler, GIoRpcRequest* req,
        guint id, GInetSocketAddress* to);
    void (*data)(FwdSocketServiceHandler* handler, GIoRpcRequest* req,
        guint id, const GUtilData* data, GInetSocketAddress* from);
    void (*close)(FwdSocketServiceHandler* handler, guint id);
};

FwdSocketService*
fwd_socket_service_new(
    GIoRpcPeer* rpc,
    FwdSocketServiceHandler* handler)
    G_GNUC_INTERNAL;

void
fwd_socket_service_free(
    FwdSocketService* service)
    G_GNUC_INTERNAL;

/* Request completions */

void
fwd_socket_service_accept_ok(
    GIoRpcRequest* req,
    guint cid,
    GInetSocketAddress* src_address,
    GInetSocketAddress* dest_address)
    G_GNUC_INTERNAL;

void
fwd_socket_service_accept_error(
    GIoRpcRequest* req,
    guint error)
    G_GNUC_INTERNAL;

void
fwd_socket_service_connect_ok(
    GIoRpcRequest* req)
    G_GNUC_INTERNAL;

void
fwd_socket_service_connect_error(
    GIoRpcRequest* req,
    guint error)
    G_GNUC_INTERNAL;

void
fwd_socket_service_data_ok(
    GIoRpcRequest* req,
    gsize nbytes)
    G_GNUC_INTERNAL;

void
fwd_socket_service_data_error(
    GIoRpcRequest* req,
    guint error)
    G_GNUC_INTERNAL;

#endif /* FWD_SOCKET_SERVICE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
