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

#ifndef FWD_UTIL_PRIVATE_H
#define FWD_UTIL_PRIVATE_H

#include "fwd_types_p.h"
#include "fwd_util.h"

#include <gio/gio.h>

#include <sys/socket.h>

guint8*
fwd_tlv_encode_umbn(
    guint8* ptr,
    guint tag,
    guint64 val)
    G_GNUC_INTERNAL;

guint8*
fwd_tlv_encode_umbn2(
    guint8* ptr,
    guint tag,
    guint len,
    guint64 val)
    G_GNUC_INTERNAL;

GByteArray*
fwd_tlv_append(
    GByteArray* out,
    guint tag,
    const GUtilData* value)
    G_GNUC_INTERNAL;

GByteArray*
fwd_tlv_append_umbn(
    GByteArray* out,
    guint tag,
    guint64 val)
    G_GNUC_INTERNAL;

GBytes*
fwd_tlv_new_umbn(
    guint tag,
    guint64 value)
    G_GNUC_INTERNAL;

GBytes*
fwd_build_tlv(
    guint tag,
    const GUtilData* value)
    G_GNUC_INTERNAL;

guint
fwd_decode_socket_id(
    const GUtilData* value)
    G_GNUC_INTERNAL;

gboolean
fwd_decode_uint_mbn(
    const GUtilData* value,
    guint* out)
    G_GNUC_INTERNAL;

gboolean
fwd_decode_uint64_mbn(
    const GUtilData* value,
    guint64* out)
    G_GNUC_INTERNAL;

void
fwd_request_complete_with_umbn_tlv(
    GIoRpcRequest* req,
    guint tag,
    guint64 value)
    G_GNUC_INTERNAL;

GSocket*
fwd_socket_new(
    GSocketType type,
    struct sockaddr* sa,
    gsize sa_len,
    gboolean allow_reuse,
    gboolean retry_any,
    GError** error)
    G_GNUC_INTERNAL;

GSocket*
fwd_socket_new_from_data(
    GSocketType type,
    GUtilData* sa, /* Note: data buffer gets modified */
    gboolean allow_reuse,
    gboolean retry_any,
    GError** error)
    G_GNUC_INTERNAL;

GInetSocketAddress*
fwd_inet_socket_address_new(
    GSocketFamily af,
    gushort port,
    gboolean any)
    G_GNUC_INTERNAL;

GUtilData*
fwd_socket_address_to_native(
    GSocketAddress* address,
    GError** error)
    G_GNUC_INTERNAL;

GUtilData*
fwd_inet_socket_address_to_native(
    GInetSocketAddress* address,
    GError** error)
    G_GNUC_INTERNAL;

GInetSocketAddress*
fwd_inet_socket_address_from_data(
    const GUtilData* data)
    G_GNUC_INTERNAL;

guint
fwd_inet_socket_address_hash(
    gconstpointer /* GInetSocketAddress* */ isa)
    G_GNUC_INTERNAL;

gboolean
fwd_inet_socket_address_equal(
    gconstpointer /* GInetSocketAddress* */ isa1,
    gconstpointer /* GInetSocketAddress* */ isa2)
    G_GNUC_INTERNAL;

const char*
fwd_format_sockaddr(
    const struct sockaddr* sa,
    gsize sa_len)
    G_GNUC_INTERNAL;

const char*
fwd_format_sockaddr_data(
    const GUtilData* value)
    G_GNUC_INTERNAL;

const char*
fwd_format_socket_address(
    GSocketAddress* address)
    G_GNUC_INTERNAL;

const char*
fwd_format_inet_socket_address(
    GInetSocketAddress* isa)
    G_GNUC_INTERNAL;

const char*
fwd_format_socket_remote_address(
    GSocket* socket)
    G_GNUC_INTERNAL;

const char*
fwd_format_socket_local_address(
    GSocket* socket)
    G_GNUC_INTERNAL;

const char*
fwd_format_connection_remote_address(
    GSocketConnection* connection)
    G_GNUC_INTERNAL;

int
fwd_error_code(
    const GError* error,
    int default_error_code)
    G_GNUC_INTERNAL;

int
fwd_error_to_code(
    GError* error,
    int default_error_code)
    G_GNUC_INTERNAL;

#endif /* FWD_UTIL_PRIVATE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
