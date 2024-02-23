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

#include "fwd_socket_service.h"

#include "fwd_log_p.h"
#include "fwd_protocol.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_datapack.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <errno.h>

struct fwd_socket_service {
    FwdSocketServiceHandler* handler;
    GIoRpcPeer* rpc;
    gulong handler_id;
};

#define OUT_TAG_RESULT (1)

static
void
fwd_socket_service_response_ok_mbn(
    GIoRpcRequest* req,
    guint result_tag,
    guint value_tag,
    guint64 value)
{
    GBytes* data = g_byte_array_free_to_bytes(
        fwd_tlv_append_umbn(fwd_tlv_append_umbn(g_byte_array_new(),
        result_tag, FWD_SOCKET_RESULT_OK), value_tag, value));

    giorpc_request_complete(req, data);
    g_bytes_unref(data);
}

static
void
fwd_socket_service_response_status(
    GIoRpcRequest* req,
    guint status)
{
    fwd_request_complete_with_umbn_tlv(req, OUT_TAG_RESULT, status);
}

static
void
fwd_socket_service_response_ok(
    GIoRpcRequest* req)
{
    fwd_socket_service_response_status(req, FWD_SOCKET_RESULT_OK);
}

/*==========================================================================*
 * Request handlers
 *==========================================================================*/

#define ACCEPT_IN_TAGS(t) t(ID)
enum fwd_socket_protocol_accept_in_tag_index {
    #define TagIndex(TAG) ACCEPT_##TAG##_INDEX,
    ACCEPT_IN_TAGS(TagIndex)
    ACCEPT_IN_TAG_COUNT
    #undef TagIndex
};
enum fwd_socket_protocol_accept_in_tag_bit {
    #define TagBit(TAG) ACCEPT_##TAG##_BIT = BIT_(ACCEPT_##TAG##_INDEX),
    ACCEPT_IN_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_socket_service_handle_accept(
    FwdSocketService* self,
    GIoRpcRequest* req,
    const GUtilData* in)
{
    static const guint accept_in_tags[] = {
        #define Tag(TAG) FWD_SOCKET_ACCEPT_IN_TAG_##TAG,
        ACCEPT_IN_TAGS(Tag) /* and zero terminator */ 0
        #undef Tag
    };
    GUtilData values[ACCEPT_IN_TAG_COUNT];
    const int tlvs = gutil_tlvs_decode(in, accept_in_tags, values,
        GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

    /* There's only one tag and it's required */
    if (tlvs == ACCEPT_ID_BIT) {
        const guint id = fwd_decode_socket_id(values + ACCEPT_ID_INDEX);

        if (id) {
            FwdSocketServiceHandler* handler = self->handler;

            GDEBUG("[%u] > ACCEPT", id);
            handler->cb->accept(handler, req, id);
            return;
        }
    }

    /* Generic failure */
    fwd_socket_service_accept_error(req, EINVAL);
}

#define ACCEPTED_IN_TAGS(t) t(ID) t(CID)
enum fwd_socket_protocol_accepted_in_tag_index {
    #define TagIndex(TAG) ACCEPTED_##TAG##_INDEX,
    ACCEPTED_IN_TAGS(TagIndex)
    ACCEPTED_IN_TAG_COUNT
    #undef TagIndex
};
enum fwd_socket_protocol_accepted_in_tag_bit {
    #define TagBit(TAG) ACCEPTED_##TAG##_BIT = BIT_(ACCEPTED_##TAG##_INDEX),
    ACCEPTED_IN_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_socket_service_handle_accepted(
    FwdSocketService* self,
    GIoRpcRequest* req,
    const GUtilData* in)
{
    static const guint accepted_in_tags[] = {
        #define Tag(TAG) FWD_SOCKET_ACCEPTED_IN_TAG_##TAG,
        ACCEPTED_IN_TAGS(Tag) /* and zero terminator */ 0
        #define ACCEPTED_IN_REQUIRED_BITS (ACCEPTED_ID_BIT | ACCEPTED_CID_BIT)
        #undef Tag
    };
    GUtilData values[ACCEPTED_IN_TAG_COUNT];
    const int tlvs = gutil_tlvs_decode(in, accepted_in_tags, values,
        GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

    if (tlvs > 0 &&
       (tlvs & ACCEPTED_IN_REQUIRED_BITS) == ACCEPTED_IN_REQUIRED_BITS) {
        const guint id = fwd_decode_socket_id(values + ACCEPTED_ID_INDEX);
        const guint cid = fwd_decode_socket_id(values + ACCEPTED_CID_INDEX);

        if (id && cid) {
            FwdSocketServiceHandler* handler = self->handler;

            GDEBUG("[%u] > ACCEPTED %u", id, cid);
            handler->cb->accepted(handler, id, cid);
        }
    }
    /* There's nothing to return */
    giorpc_request_complete(req, NULL);
}

#define CONNECT_IN_TAGS(t) t(ID) t(ADDRESS)
enum fwd_socket_protocol_connect_in_tag_index {
    #define TagIndex(TAG) CONNECT_##TAG##_INDEX,
    CONNECT_IN_TAGS(TagIndex)
    CONNECT_IN_TAG_COUNT
    #undef TagIndex
};
enum fwd_socket_protocol_connect_in_tag_bit {
    #define TagBit(TAG) CONNECT_##TAG##_BIT = BIT_(CONNECT_##TAG##_INDEX),
    CONNECT_IN_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_socket_service_handle_connect(
    FwdSocketService* self,
    GIoRpcRequest* req,
    const GUtilData* in)
{
    static const guint connect_in_tags[] = {
        #define Tag(TAG) FWD_SOCKET_CONNECT_IN_TAG_##TAG,
        CONNECT_IN_TAGS(Tag) /* and zero terminator */ 0
        #undef Tag
        #define CONNECT_IN_REQUIRED_BITS (CONNECT_ID_BIT | CONNECT_ADDRESS_BIT)
    };
    GUtilData values[CONNECT_IN_TAG_COUNT];
    const int tlvs = gutil_tlvs_decode(in, connect_in_tags, values,
        GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

    if (tlvs == CONNECT_IN_REQUIRED_BITS) {
        const guint id = fwd_decode_socket_id(values + CONNECT_ID_INDEX);

        if (id) {
            const GUtilData* addr = values + CONNECT_ADDRESS_INDEX;
            GInetSocketAddress* to = fwd_inet_socket_address_from_data(addr);

            if (to) {
                FwdSocketServiceHandler* handler = self->handler;

#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    GDEBUG("[%u] > CONNECT %s", id,
                        fwd_format_sockaddr_data(addr));
                }
#endif /* GUTIL_LOG_DEBUG */

                handler->cb->connect(handler, req, id, to);
                g_object_unref(to);
                return;
            }
        }
    }

    /* Generic failure */
    fwd_socket_service_connect_error(req, EINVAL);
}

#define DATA_IN_TAGS(t) t(ID) t(BYTES) t(ADDRESS)
enum fwd_socket_protocol_data_in_tag_index {
    #define TagIndex(TAG) DATA_##TAG##_INDEX,
    DATA_IN_TAGS(TagIndex)
    DATA_IN_TAG_COUNT
    #undef TagIndex
};
enum fwd_socket_protocol_data_in_tag_bit {
    #define TagBit(TAG) DATA_##TAG##_BIT = BIT_(DATA_##TAG##_INDEX),
    DATA_IN_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_socket_service_handle_data(
    FwdSocketService* self,
    GIoRpcRequest* req,
    const GUtilData* in)
{
    static const guint data_in_tags[] = {
        #define Tag(TAG) FWD_SOCKET_DATA_IN_TAG_##TAG,
        DATA_IN_TAGS(Tag) /* and zero terminator */ 0
        #undef Tag
        #define DATA_IN_REQUIRED_BITS (DATA_ID_BIT | DATA_BYTES_BIT)
    };
    GUtilData values[DATA_IN_TAG_COUNT];
    const int tlvs = gutil_tlvs_decode(in, data_in_tags, values,
        GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

    if (tlvs > 0 &&
       (tlvs & DATA_IN_REQUIRED_BITS) == DATA_IN_REQUIRED_BITS) {
        const guint id = fwd_decode_socket_id(values + DATA_ID_INDEX);

        if (id) {
            FwdSocketServiceHandler* handler = self->handler;
            GInetSocketAddress* from = NULL;
            const GUtilData* data = values + DATA_BYTES_INDEX;

            if (tlvs & DATA_ADDRESS_BIT) {
                from = fwd_inet_socket_address_from_data(values +
                    DATA_ADDRESS_INDEX);
            }

#if GUTIL_LOG_DEBUG
            if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                if (from) {
                    GDEBUG("[%u] > DATA %u byte(s) from %s", id, (guint)
                        data->size, fwd_format_inet_socket_address(from));
                } else {
                    GDEBUG("[%u] > DATA %u byte(s)", id, (guint) data->size);
                }
            }
#endif /* GUTIL_LOG_DEBUG */

            handler->cb->data(handler, req, id, data, from);
            gutil_object_unref(from);
            return;
        }
    }
    /* Generic failure */
    fwd_socket_service_connect_error(req, EINVAL);
}

#define CLOSE_IN_TAGS(t) t(ID)
enum fwd_socket_service_close_in_tag_index {
    #define TagIndex(TAG) CLOSE_##TAG##_INDEX,
    CLOSE_IN_TAGS(TagIndex)
    CLOSE_IN_TAG_COUNT
    #undef TagIndex
};
enum fwd_socket_service_close_in_tag_bit {
    #define TagBit(TAG) CLOSE_##TAG##_BIT = BIT_(CLOSE_##TAG##_INDEX),
    CLOSE_IN_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_socket_service_handle_close(
    FwdSocketService* self,
    GIoRpcRequest* req,
    const GUtilData* in)
{
    static const guint close_in_tags[] = {
        #define Tag(TAG) FWD_SOCKET_CONNECT_IN_TAG_##TAG,
        CLOSE_IN_TAGS(Tag) /* and zero terminator */ 0
        #undef Tag
    };
    GUtilData values[CLOSE_IN_TAG_COUNT];
    const int tlvs = gutil_tlvs_decode(in, close_in_tags, values,
        GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

    /* There's only one tag and it's required */
    if (tlvs == CLOSE_ID_BIT) {
        const guint id = fwd_decode_socket_id(values + CLOSE_ID_INDEX);

        if (id) {
            FwdSocketServiceHandler* handler = self->handler;

            GDEBUG("[%u] > CLOSE", id);
            handler->cb->close(handler, id);
        }
    }
    /* This is a one-way request, just complete it */
    giorpc_request_complete(req, NULL);
}

static
void
fwd_socket_service_handle_request(
    GIoRpcPeer* rpc,
    guint iid,
    guint code,
    GBytes* bytes,
    GIoRpcRequest* req,
    gpointer user_data)
{
    FwdSocketService* self = user_data;
    GUtilData in;

    gutil_data_from_bytes(&in, bytes);
    switch ((FWD_SOCKET_CODE)code) {
    case FWD_SOCKET_ACCEPT:
        fwd_socket_service_handle_accept(self, req, &in);
        break;
    case FWD_SOCKET_ACCEPTED:
        fwd_socket_service_handle_accepted(self, req, &in);
        break;
    case FWD_SOCKET_CONNECT:
        fwd_socket_service_handle_connect(self, req, &in);
        break;
    case FWD_SOCKET_DATA:
        fwd_socket_service_handle_data(self, req, &in);
        break;
    case FWD_SOCKET_CLOSE:
        fwd_socket_service_handle_close(self, req, &in);
        break;
    }
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdSocketService*
fwd_socket_service_new(
    GIoRpcPeer* rpc,
    FwdSocketServiceHandler* handler)
{
    FwdSocketService* self = g_slice_new(FwdSocketService);

    self->handler = handler;
    self->rpc = giorpc_peer_ref(rpc);
    self->handler_id = giorpc_peer_add_request_handler(rpc,
        FWD_IID_SOCKET, GIORPC_CODE_ANY,
        fwd_socket_service_handle_request, self);
    return self;
}

void
fwd_socket_service_free(
    FwdSocketService* self)
{
    giorpc_peer_remove_handler(self->rpc, self->handler_id);
    giorpc_peer_unref(self->rpc);
    gutil_slice_free(self);
}

void
fwd_socket_service_accept_ok(
    GIoRpcRequest* req,
    guint cid,
    GInetSocketAddress* src_addr,
    GInetSocketAddress* dest_addr)
{
    GBytes* tlvs;
    GUtilData* src = fwd_inet_socket_address_to_native(src_addr, NULL);
    GByteArray* buf = fwd_tlv_append(fwd_tlv_append_umbn(
        fwd_tlv_append_umbn(g_byte_array_new(),
            FWD_SOCKET_ACCEPT_OUT_TAG_RESULT, FWD_SOCKET_RESULT_OK),
            FWD_SOCKET_ACCEPT_OUT_TAG_CID, cid),
            FWD_SOCKET_ACCEPT_OUT_TAG_SRC_ADDRESS, src);

    /* Destination address is optional (redundant for UDP) */
    if (dest_addr) {
        GUtilData* dest = fwd_inet_socket_address_to_native(dest_addr, NULL);

        fwd_tlv_append(buf, FWD_SOCKET_ACCEPT_OUT_TAG_DEST_ADDRESS, dest);
        g_free(dest);
    }

    tlvs = g_byte_array_free_to_bytes(buf);
    giorpc_request_complete(req, tlvs);
    g_bytes_unref(tlvs);
    g_free(src);
}

void
fwd_socket_service_accept_error(
    GIoRpcRequest* req,
    guint error)
{
    G_STATIC_ASSERT(OUT_TAG_RESULT == FWD_SOCKET_ACCEPT_OUT_TAG_RESULT);
    fwd_socket_service_response_status(req, error);
}

void
fwd_socket_service_connect_ok(
    GIoRpcRequest* req)
{
    G_STATIC_ASSERT(OUT_TAG_RESULT == FWD_SOCKET_CONNECT_OUT_TAG_RESULT);
    fwd_socket_service_response_ok(req);
}

void
fwd_socket_service_connect_error(
    GIoRpcRequest* req,
    guint error)
{
    G_STATIC_ASSERT(OUT_TAG_RESULT == FWD_SOCKET_CONNECT_OUT_TAG_RESULT);
    fwd_socket_service_response_status(req, error);
}

void
fwd_socket_service_data_ok(
    GIoRpcRequest* req,
    gsize nbytes)
{
    fwd_socket_service_response_ok_mbn(req,
        FWD_SOCKET_DATA_OUT_TAG_RESULT,
        FWD_SOCKET_DATA_OUT_TAG_BYTES_SENT, nbytes);
}

void
fwd_socket_service_data_error(
    GIoRpcRequest* req,
    guint error)
{
    G_STATIC_ASSERT(OUT_TAG_RESULT == FWD_SOCKET_DATA_OUT_TAG_RESULT);
    fwd_socket_service_response_status(req, error);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
