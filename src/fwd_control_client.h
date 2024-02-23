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

#ifndef FWD_CONTROL_CLIENT_H
#define FWD_CONTROL_CLIENT_H

#include "fwd_types_p.h"

#include <gio/gio.h>

typedef struct fwd_control_info {
    guint v1, v2;
    guint32 byteorder;
} FwdControlInfo;

typedef
void
(*FwdControlCallInfoFunc)(
    const FwdControlInfo* info, /* Protocol info (NULL on error) */
    void* user_data);

typedef
void
(*FwdControlCallSocketFunc)(
    int id,                   /* Socket id > 0, error < 0, RPC error = 0 */
    GInetSocketAddress* address, /* NULL on error */
    void* user_data);

guint
fwd_control_call_info(
    GIoRpcPeer* rpc,
    FwdControlCallInfoFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
    G_GNUC_INTERNAL;

guint
fwd_control_call_socket(
    GIoRpcPeer* rpc,
    guint id,
    GSocketFamily af,
    GSocketType type,
    GInetSocketAddress* address,
    FWD_SOCKET_FLAGS flags,
    guint backlog,
    guint conn_timeout_ms,
    guint conn_limit,
    FwdControlCallSocketFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
    G_GNUC_INTERNAL;

#endif /* FWD_CONTROL_CLIENT_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
