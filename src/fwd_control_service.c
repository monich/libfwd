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

#include "fwd_control_service.h"

#include "fwd_log_p.h"
#include "fwd_protocol.h"
#include "fwd_util_p.h"

#include <giorpc.h>

#include <gutil_datapack.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <errno.h>
#include <netinet/in.h>
#include <sys/types.h>

struct fwd_control_service {
    FwdControlServiceHandler* handler;
    GIoRpcPeer* rpc;
    gulong handler_id;
};

/*==========================================================================*
 * Request handlers
 *==========================================================================*/

static
void
fwd_control_service_handle_info(
    GIoRpcRequest* req)
{
    const guint v1 = FWD_PROTOCOL_VERSION;
    const guint v2 = FWD_PROTOCOL_VERSION;
    const guint32 byteorder = FWD_BYTE_ORDER_MAGIC;
    GUtilData vdata, bdata;
    guint8* vbuf;
    GBytes* tlvs;

    /*
     *  Tag 1: protocol range (mandatory)
     *  Value: 2 unsigned MBNs. The minimum supported version is followed by
     *         the maximum one. The range is inclusive, e.g. [1,1] declares
     *         support for a single protocol version 1. Obviously, the
     *         maximum must be greater or equal to the minimum.
     */
    vdata.size = gutil_unsigned_mbn_size(v1) + gutil_unsigned_mbn_size(v2);
    vdata.bytes = vbuf = g_malloc(vdata.size);
    gutil_unsigned_mbn_encode(vbuf + gutil_unsigned_mbn_encode(vbuf, v1), v2);

    /*
     *  Tag 2: byte order (mandatory)
     *  Value: 4 bytes containing 0x04030201 in the native byte order, e.g.
     *         0x01 0x02 0x03 0x04 - Little Endian
     *         0x04 0x03 0x02 0x01 - Big Endian
     */
    bdata.size = sizeof(byteorder);
    bdata.bytes = (const guint8*) &byteorder;

    tlvs = g_byte_array_free_to_bytes(
        fwd_tlv_append(fwd_tlv_append(g_byte_array_new(),
            FWD_CONTROL_INFO_OUT_TAG_VERSIONS, &vdata),
            FWD_CONTROL_INFO_OUT_TAG_BYTE_ORDER, &bdata));

    GDEBUG("> INFO");
    giorpc_request_complete(req, tlvs);
    g_bytes_unref(tlvs);
    g_free(vbuf);
}

#define SOCKET_IN_TAGS(t) t(ID) t(FAMILY) t(TYPE) t(ADDRESS) t(FLAGS) \
        t(BACKLOG) t(TIMEOUT) t(MAX_CONN)
enum fwd_control_socket_in_tag_index {
    #define TagIndex(TAG) SOCKET_IN_##TAG##_INDEX,
    SOCKET_IN_TAGS(TagIndex)
    SOCKET_IN_TAG_COUNT
    #undef TagIndex
};
enum fwd_control_socket_in_tag_bit {
    #define TagBit(TAG) SOCKET_IN_##TAG##_BIT = BIT_(SOCKET_IN_##TAG##_INDEX),
    SOCKET_IN_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_control_service_handle_socket(
    FwdControlService* self,
    GIoRpcRequest* req,
    GBytes* bytes)
{
    static const guint socket_in_tags[] = {
        #define Tag(TAG) FWD_CONTROL_SOCKET_IN_TAG_##TAG,
        SOCKET_IN_TAGS(Tag) /* and zero terminator */ 0
        #undef Tag
    };
    GUtilData data, values[SOCKET_IN_TAG_COUNT];
    const int tlvs = gutil_tlvs_decode(gutil_data_from_bytes(&data, bytes),
        socket_in_tags, values, GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

    /* Socket id tag is the only one required */
    if (tlvs > 0 && (tlvs & SOCKET_IN_ID_BIT)) {
        const guint rid = fwd_decode_socket_id(values + SOCKET_IN_ID_INDEX);

        if (rid) {
            guint af = AF_INET, type = SOCK_STREAM;
            struct sockaddr_storage ss;
            struct sockaddr* sa = (void*) &ss;
            gsize sa_len = 0;
            guint64 flags = 0;
            uint backlog = 0, timeout = 0, maxconn = 0;
            gboolean ok = TRUE;

            if ((tlvs & SOCKET_IN_FAMILY_BIT) &&
                (!fwd_decode_uint_mbn(values +
                SOCKET_IN_FAMILY_INDEX, &af) || af > AF_MAX)) {
                ok = FALSE;
            }

            if (ok && (tlvs & SOCKET_IN_TYPE_BIT)) {
                ok = FALSE;
                if (fwd_decode_uint_mbn(values +
                    SOCKET_IN_TYPE_INDEX, &type)) {
                    /* g_socket_new blows up on invalid type */
                    switch ((GSocketType)type) {
                    case G_SOCKET_TYPE_INVALID:
                    case G_SOCKET_TYPE_SEQPACKET:
                        break;
                    case G_SOCKET_TYPE_STREAM:
                    case G_SOCKET_TYPE_DATAGRAM:
                        ok = TRUE;
                        break;
                    }
                }
            }

            if (ok && (tlvs & SOCKET_IN_FLAGS_BIT) &&
                !fwd_decode_uint64_mbn(values +
                SOCKET_IN_FLAGS_INDEX, &flags)) {
                ok = FALSE;
            }

            if (ok) {
                if (tlvs & SOCKET_IN_ADDRESS_BIT) {
                    const GUtilData* value = values + SOCKET_IN_ADDRESS_INDEX;

                    if (value->size > sizeof(sa_family_t)) {
                        sa = (void*) value->bytes;
                        sa_len = value->size;
                        if (tlvs & SOCKET_IN_FAMILY_BIT) {
                            /* If both are specified, they must match */
                            if (sa->sa_family != af) {
                                ok = FALSE;
                            }
                        } else if (sa->sa_family <= AF_MAX) {
                            /* Family wasn't specified */
                            af = sa->sa_family;
                        } else {
                            ok = FALSE;
                        }
                    } else {
                        /* Address is definitely too short */
                        ok = FALSE;
                    }
                } else {
                    /* Default address (any) */
                    memset(&ss, 0, sizeof(ss));
                    ss.ss_family = (sa_family_t) af;
                    sa_len = (af == AF_INET) ? sizeof(struct sockaddr_in) :
                        (af == AF_INET6) ? sizeof(struct sockaddr_in6) :
                        sizeof(ss);
                }
            }

            if (ok && (tlvs & SOCKET_IN_BACKLOG_BIT) &&
                /* Backlog requires the Listen flag */
                (!(flags & FWD_CONTROL_SOCKET_FLAG_LISTEN) ||
                 !fwd_decode_uint_mbn(values + SOCKET_IN_BACKLOG_INDEX,
                 &backlog))) {
                ok = FALSE;
            }

            if (ok && (tlvs & SOCKET_IN_TIMEOUT_BIT) &&
                !fwd_decode_uint_mbn(values + SOCKET_IN_TIMEOUT_INDEX,
                &timeout)) {
                ok = FALSE;
            }

            if (ok && (tlvs & SOCKET_IN_MAX_CONN_BIT) &&
                !fwd_decode_uint_mbn(values + SOCKET_IN_MAX_CONN_INDEX,
                &maxconn)) {
                ok = FALSE;
            }

            if (ok) {
                GSocketAddress* address =
                    g_socket_address_new_from_native(sa, sa_len);

#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    if (tlvs & SOCKET_IN_ADDRESS_BIT) {
                        GDEBUG("> SOCKET [%u] %s", rid,
                            fwd_format_sockaddr(sa, sa_len));
                    } else {
                        GDEBUG("> SOCKET [%u] af=%u", rid, af);
                    }
                }
#endif /* GUTIL_LOG_DEBUG */

                if (G_IS_INET_SOCKET_ADDRESS(address)) {
                    FwdControlServiceHandler* handler = self->handler;

                    handler->cb->socket(handler, req, rid, type,
                        G_INET_SOCKET_ADDRESS(address), flags,
                        backlog, timeout, maxconn);
                } else {
                    fwd_request_complete_with_umbn_tlv(req,
                        FWD_CONTROL_SOCKET_OUT_TAG_ERROR, EINVAL);
                }
                gutil_object_unref(address);
                return;
            }
        }
    }

    /* Generic failure */
    fwd_request_complete_with_umbn_tlv(req,
        FWD_CONTROL_SOCKET_OUT_TAG_ERROR, EINVAL);
}

static
void
fwd_control_service_handle_request(
    GIoRpcPeer* rpc,
    guint iid,
    guint code,
    GBytes* data,
    GIoRpcRequest* req,
    gpointer user_data)
{
    FwdControlService* self = user_data;

    switch ((FWD_CONTROL_CODE)code) {
    case FWD_CONTROL_INFO:
        fwd_control_service_handle_info(req);
        break;
    case FWD_CONTROL_ECHO:
        GDEBUG("> ECHO");
        giorpc_request_complete(req, data);
        break;
    case FWD_CONTROL_SOCKET:
        fwd_control_service_handle_socket(self, req, data);
        break;
    }
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdControlService*
fwd_control_service_new(
    GIoRpcPeer* rpc,
    FwdControlServiceHandler* handler)
{
    FwdControlService* self = g_slice_new(FwdControlService);

    self->handler = handler;
    self->rpc = giorpc_peer_ref(rpc);
    self->handler_id = giorpc_peer_add_request_handler(rpc,
        FWD_IID_CONTROL, GIORPC_CODE_ANY,
        fwd_control_service_handle_request, self);
    return self;
}

void
fwd_control_service_free(
    FwdControlService* self)
{
    giorpc_peer_remove_handler(self->rpc, self->handler_id);
    giorpc_peer_unref(self->rpc);
    gutil_slice_free(self);
}

void
fwd_control_service_socket_ok(
    GIoRpcRequest* req,
    guint id,
    const GUtilData* sa)
{
    GBytes* tlvs = g_byte_array_free_to_bytes(
        fwd_tlv_append(fwd_tlv_append_umbn(g_byte_array_new(),
            FWD_CONTROL_SOCKET_OUT_TAG_ID, id),
            FWD_CONTROL_SOCKET_OUT_TAG_ADDRESS, sa));

    giorpc_request_complete(req, tlvs);
    g_bytes_unref(tlvs);
}

void
fwd_control_service_socket_error(
    GIoRpcRequest* req,
    guint error)
{
    fwd_request_complete_with_umbn_tlv(req,
        FWD_CONTROL_SOCKET_OUT_TAG_ERROR, error);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
