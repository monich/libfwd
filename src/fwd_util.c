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

#include "fwd_util_p.h"

#include "fwd_log_p.h"
#include "fwd_protocol.h"
#include "fwd_version.h"

#include <giorpc.h>
#include <gutil_datapack.h>
#include <gutil_idlepool.h>
#include <gutil_misc.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>

GLOG_MODULE_DEFINE("fwd");

guint
fwd_version()
{
    return FWD_VERSION;
}

guint8*
fwd_tlv_encode_umbn(
    guint8* ptr,
    guint tag,
    guint64 val)
{
    return fwd_tlv_encode_umbn2(ptr, tag,
        gutil_unsigned_mbn_size(val), val);
}

guint8*
fwd_tlv_encode_umbn2(
    guint8* ptr,
    guint tag,
    guint len,
    guint64 val)
{
    ptr += gutil_unsigned_mbn_encode(ptr, tag);
    ptr += gutil_unsigned_mbn_encode(ptr, len);
    ptr += gutil_unsigned_mbn_encode2(ptr, val, len);
    return ptr;
}

GByteArray*
fwd_tlv_append(
    GByteArray* out,
    guint tag,
    const GUtilData* value)
{
    const guint size = gutil_tlv_size(tag, value ? value->size : 0);
    const guint off = out->len;

    g_byte_array_set_size(out, out->len + size);
    gutil_tlv_encode(out->data + off, tag, value);
    return out;
}

GByteArray*
fwd_tlv_append_umbn(
    GByteArray* out,
    guint tag,
    guint64 val)
{
    const guint len = gutil_unsigned_mbn_size(val);
    const guint size = gutil_tlv_size(tag, len);
    const guint off = out->len;

    g_byte_array_set_size(out, out->len + size);
    fwd_tlv_encode_umbn2(out->data + off, tag, len, val);
    return out;
}

GBytes*
fwd_tlv_new_umbn(
    guint tag,
    guint64 val)
{
    const guint len = gutil_unsigned_mbn_size(val);
    const guint size = gutil_tlv_size(tag, len);
    guint8* tlv = g_malloc(size);

    fwd_tlv_encode_umbn2(tlv, tag, len, val);
    return g_bytes_new_take(tlv, size);
}

GBytes*
fwd_build_tlv(
    guint tag,
    const GUtilData* value)
{
    gsize size = gutil_tlv_size(tag, value->size);
    guint8* tlv = g_malloc(size);

    gutil_tlv_encode(tlv, tag, value);
    return g_bytes_new_take(tlv, size);
}

guint
fwd_decode_socket_id(
    const GUtilData* data)
{
    GUtilRange in;
    guint64 id;

    in.end = (in.ptr = data->bytes) + data->size;
    if (gutil_unsigned_mbn_decode(&in, &id) && in.ptr == in.end &&
        id && id <= FWD_SOCKET_ID_MAX) {
        return (guint) id;
    }
    return 0;
}

gboolean
fwd_decode_uint_mbn(
    const GUtilData* data,
    guint* out)
{
    guint64 value;
    GUtilRange in;

    in.end = (in.ptr = data->bytes) + data->size;
    if (gutil_unsigned_mbn_decode(&in, &value) &&
        in.ptr == in.end && value <= UINT_MAX) {
        *out = (guint) value;
        return TRUE;
    }
    return FALSE;
}

gboolean
fwd_decode_uint64_mbn(
    const GUtilData* data,
    guint64* out)
{
    guint64 value;
    GUtilRange in;

    in.end = (in.ptr = data->bytes) + data->size;
    if (gutil_unsigned_mbn_decode(&in, &value) &&
        in.ptr == in.end) {
        *out = value;
        return TRUE;
    }
    return FALSE;
}

void
fwd_request_complete_with_umbn_tlv(
    GIoRpcRequest* req,
    guint tag,
    guint64 value)
{
    GBytes* data = fwd_tlv_new_umbn(tag, value);

    giorpc_request_complete(req, data);
    g_bytes_unref(data);
}

static
char*
fwd_format_sockaddr_internal(
    const struct sockaddr* sa,
    gsize sa_len)
{
    if (sa && sa_len > sizeof(sa_family_t)) {
        const int af = sa->sa_family;
        const void* addr = NULL;
        const char* prefix = "";
        const char* suffix = "";
        guint16 port = 0;

        if (af == AF_INET) {
            const struct sockaddr_in* in = (void*)sa;

            addr = &in->sin_addr;
            port = ntohs(in->sin_port);
        } else if (af == AF_INET6) {
            const struct sockaddr_in6* in6 = (void*)sa;

            addr = &in6->sin6_addr;
            port = ntohs(in6->sin6_port);
            prefix = "[";
            suffix = "]";
        }

        if (addr) {
            char buf[INET6_ADDRSTRLEN];
            char* str = g_strdup_printf("%s%s%s:%hu", prefix,
                inet_ntop(af, addr, buf, sizeof(buf)), suffix,
                port);

            return str;
        }
    }
    return NULL;
}

GSocket*
fwd_socket_new(
    GSocketType type,
    struct sockaddr* sa,
    gsize sa_len,
    gboolean allow_reuse,
    gboolean retry_any,
    GError** error)
{
    GSocketAddress* address = g_socket_address_new_from_native(sa, sa_len);

    if (G_IS_INET_SOCKET_ADDRESS(address)) {
        GInetSocketAddress* isa = G_INET_SOCKET_ADDRESS(address);
        GInetAddress* ia = g_inet_socket_address_get_address(isa);
        GSocketFamily af = g_inet_address_get_family(ia);
        GSocket* s = g_socket_new(af, type, G_SOCKET_PROTOCOL_DEFAULT, error);

        if (s) {
            gboolean ok = g_socket_bind(s, address, allow_reuse, error);

            if (!ok && retry_any && (!g_inet_address_get_is_any(ia) ||
                 g_inet_socket_address_get_port(isa))) {
                /* Try to bind to any address */
                ia = g_inet_address_new_any(af);
                gutil_object_unref(address);
                address = g_inet_socket_address_new(ia, 0);
                g_object_unref(ia); /* Drop ref added by the above */
                isa = G_INET_SOCKET_ADDRESS(address);
                g_clear_error(error);
                ok = g_socket_bind(s, address, allow_reuse, error);
            }
            if (ok) {
                /* Query the real address */
                GSocketAddress* la = g_socket_get_local_address(s, error);

                if (la) {
                    const gboolean ok = g_socket_address_to_native(la,
                        sa, sa_len, error);

                    g_object_unref(la);
                    if (ok) {
                        g_socket_set_blocking(s, FALSE);
                        g_object_unref(address);
                        return s;
                    }
                }
            } else if (error) {
                GWARN("%s", GERRMSG(*error));
            }
            g_object_unref(s);
        }
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid socket address");
    }
    gutil_object_unref(address);
    return NULL;
}

GSocket*
fwd_socket_new_from_data(
    GSocketType type,
    GUtilData* sa,
    gboolean allow_reuse,
    gboolean retry_any,
    GError** error)
{
    if (sa) {
        return fwd_socket_new(type, (struct sockaddr*) sa->bytes, sa->size,
            allow_reuse, retry_any, error);
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Missing socket address");
        return NULL;
    }
}

GInetSocketAddress*
fwd_inet_socket_address_new(
    GSocketFamily af,
    gushort port,
    gboolean any)
{
    GInetAddress* ia = any ? g_inet_address_new_any(af) :
        g_inet_address_new_loopback(af);
    GSocketAddress* sa = g_inet_socket_address_new(ia, port);

    g_object_unref(ia);
    return G_INET_SOCKET_ADDRESS(sa);
}

GUtilData*
fwd_socket_address_to_native(
    GSocketAddress* sa,
    GError** error)
{
    if (sa) {
        const gsize len = g_socket_address_get_native_size(sa);
        GUtilData* data = g_malloc(len + sizeof(GUtilData));
        void* buf = (void*)(data + 1);

        data->size = len;
        data->bytes = buf;
        if (g_socket_address_to_native(sa, buf, len, error)) {
            return data;
        }
        g_free(data);
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid socket address");
    }
    return NULL;
}

GUtilData*
fwd_inet_socket_address_to_native(
    GInetSocketAddress* isa,
    GError** error)
{
    return fwd_socket_address_to_native(isa ? G_SOCKET_ADDRESS(isa) : NULL,
        error);
}

GInetSocketAddress*
fwd_inet_socket_address_from_data(
    const GUtilData* native)
{
    if (native && native->size) {
        GSocketAddress* sa = g_socket_address_new_from_native
            ((struct sockaddr*) native->bytes, native->size);

        if (sa) {
            if (G_IS_INET_SOCKET_ADDRESS(sa)) {
                return G_INET_SOCKET_ADDRESS(sa);
            }
            g_object_unref(sa);
        }
    }
    return NULL;
}

guint
fwd_inet_socket_address_hash(
    gconstpointer p)
{
    GInetSocketAddress* sa = G_INET_SOCKET_ADDRESS(p);

    if (sa) {
        /*
         * Not using g_socket_address_to_native(), to avoid unnecessarily
         * allocating and/or copying the memory.
         */
        guint16 p = g_htons(g_inet_socket_address_get_port(sa));
        GInetAddress* ia = g_inet_socket_address_get_address(sa);
        const guint8* ptr = g_inet_address_to_bytes(ia);
        const gsize n = g_inet_address_get_native_size(ia);
        guint h = 0;
        gsize i;

        for (i = 0; i < n; i++) {
            h += (h << 3) + *ptr++;
        }
        for (ptr = (guint8*) &p, i = 0; i < sizeof(p); i++) {
            h += (h << 3) + *ptr++;
        }
        return h;
    } else {
        return 0;
    }
}

gboolean
fwd_inet_socket_address_equal(
    gconstpointer p1,
    gconstpointer p2)
{
    GInetSocketAddress* sa1 = G_INET_SOCKET_ADDRESS(p1);
    GInetSocketAddress* sa2 = G_INET_SOCKET_ADDRESS(p2);

    if (sa1 == sa2) {
        return TRUE;
    } else {
        return sa1 && sa2 &&
            g_inet_socket_address_get_port(sa1) ==
            g_inet_socket_address_get_port(sa2) &&
            g_inet_address_equal(
                g_inet_socket_address_get_address(sa1),
                g_inet_socket_address_get_address(sa2));
    }
}

const char*
fwd_format_sockaddr(
    const struct sockaddr* sa,
    gsize sa_len)
{
    char* str = fwd_format_sockaddr_internal(sa, sa_len);

    if (str) {
        gutil_idle_pool_add(NULL, str, g_free);
    }
    return str;
}

const char*
fwd_format_sockaddr_data(
    const GUtilData* data)
{
    return data ?
        fwd_format_sockaddr((struct sockaddr*) data->bytes, data->size) :
        NULL;
}

const char*
fwd_format_socket_address(
    GSocketAddress* address)
{
    const char* str = NULL;

    if (address) {
        const gsize sa_len = g_socket_address_get_native_size(address);
        struct sockaddr* sa = g_malloc(sa_len);

        if (g_socket_address_to_native(address, sa, sa_len, NULL)) {
            str = fwd_format_sockaddr(sa, sa_len);
        }
        g_free(sa);
    }
    return str;
}

const char*
fwd_format_inet_socket_address(
    GInetSocketAddress* isa)
{
    return isa ? fwd_format_socket_address(G_SOCKET_ADDRESS(isa)) : NULL;
}

const char*
fwd_format_socket_remote_address(
    GSocket* socket)
{
    const char* str = NULL;

    if (socket) {
        GSocketAddress* sa = g_socket_get_remote_address(socket, NULL);

        str = fwd_format_socket_address(sa);
        g_object_unref(sa);
    }
    return str;
}

const char*
fwd_format_socket_local_address(
    GSocket* socket)
{
    const char* str = NULL;

    if (socket) {
        GSocketAddress* sa = g_socket_get_local_address(socket, NULL);

        str = fwd_format_socket_address(sa);
        g_object_unref(sa);
    }
    return str;
}

const char*
fwd_format_connection_remote_address(
    GSocketConnection* sc)
{
    const char* str = NULL;

    if (sc) {
        GSocketAddress* sa = g_socket_connection_get_remote_address(sc, NULL);

        str = fwd_format_socket_address(sa);
        g_object_unref(sa);
    }
    return str;
}

int
fwd_error_code(
    const GError* error,
    int default_error_code)
{
    if (error) {
        if (error->domain == G_IO_ERROR) {
            switch (error->code) {
            case G_IO_ERROR_NO_SPACE: return ENOMEM;
            case G_IO_ERROR_NOT_SUPPORTED: return ENOTSUP;
            case G_IO_ERROR_ADDRESS_IN_USE: return EADDRINUSE;
            case G_IO_ERROR_INVALID_ARGUMENT: return EINVAL;
            case G_IO_ERROR_PERMISSION_DENIED: return EACCES;
            case G_IO_ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
            case G_IO_ERROR_TIMED_OUT: return ETIMEDOUT;
            case G_IO_ERROR_NOT_CONNECTED: return ENOTCONN;
            case G_IO_ERROR_CONNECTION_REFUSED: return ECONNREFUSED;
            case G_IO_ERROR_CANCELLED: return ECANCELED;
            }
        }
        return EFAULT;
    } else {
        return default_error_code;
    }
}

int
fwd_error_to_code(
    GError* error,
    int default_error_code)
{
    const guint code = fwd_error_code(error, default_error_code);

    if (error) {
        g_error_free(error);
    }
    return code;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
