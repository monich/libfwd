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

#ifndef FWD_SOCKET_CLIENT_H
#define FWD_SOCKET_CLIENT_H

#include "fwd_types_p.h"

#include <gio/gio.h>

typedef
void
(*FwdSocketCallStatusFunc)(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    void* user_data);

typedef
void
(*FwdSocketCallAcceptFunc)(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    guint rid,  /* Remote socket's id (zero on error) */
    const GUtilData* src_sockaddr, /* struct sockaddr (NULL on error) */
    const GUtilData* dest_sockaddr, /* struct sockaddr (NULL on error) */
    void* user_data);

typedef
void
(*FwdSocketCallDataFunc)(
    int result, /* Zero on success,  > 0 error code, -1 on RPC failure */
    guint nbytes, /* Number of bytes sent (zero on error) */
    void* user_data);

guint
fwd_socket_call_accept(
    GIoRpcPeer* rpc,
    guint id,
    FwdSocketCallAcceptFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
    G_GNUC_INTERNAL;

guint
fwd_socket_call_accepted(
    GIoRpcPeer* rpc,
    guint id,
    guint cid,
    gpointer user_data,
    GDestroyNotify destroy)
    G_GNUC_INTERNAL;

guint
fwd_socket_call_connect(
    GIoRpcPeer* rpc,
    guint id,
    GInetSocketAddress* sa,
    FwdSocketCallStatusFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
    G_GNUC_INTERNAL;

guint
fwd_socket_call_data(
    GIoRpcPeer* rpc,
    guint id,
    const GUtilData* data,
    GInetSocketAddress* from,
    FwdSocketCallDataFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
    G_GNUC_INTERNAL;

guint
fwd_socket_call_close(
    GIoRpcPeer* rpc,
    guint id)
    G_GNUC_INTERNAL;

#endif /* FWD_SOCKET_CLIENT_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
