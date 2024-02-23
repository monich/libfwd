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

#include "fwd_control_client.h"

#include "fwd_log_p.h"
#include "fwd_protocol.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_datapack.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

typedef struct fwd_control_call {
    union {
        FwdControlCallInfoFunc info;
        FwdControlCallSocketFunc socket;
        GCallback callback;
    } complete;
    gpointer user_data;
    GDestroyNotify destroy;
} FwdControlCall;

static
FwdControlCall*
fwd_control_call_new(
    GCallback complete,
    gpointer user_data,
    GDestroyNotify destroy)
{
    FwdControlCall* call = g_slice_new0(FwdControlCall);

    call->complete.callback = complete;
    call->user_data = user_data;
    call->destroy = destroy;
    return call;
}

static
void
fwd_control_call_destroy(
    gpointer data)
{
    FwdControlCall* call = data;

    if (call->destroy) {
        call->destroy(call->user_data);
    }
    gutil_slice_free(call);
}

/*==========================================================================*
 * Completion callbacks
 *==========================================================================*/

#define INFO_OUT_TAGS(t) t(VERSIONS) t(BYTE_ORDER)
enum fwd_control_info_out_tag_index {
    #define TagIndex(TAG) INFO_OUT_##TAG##_INDEX,
    INFO_OUT_TAGS(TagIndex)
    INFO_OUT_TAG_COUNT
    #undef TagIndex
};
enum fwd_control_info_out_tag_bit {
    #define TagBit(TAG) INFO_OUT_##TAG##_BIT = BIT_(INFO_OUT_##TAG##_INDEX),
    INFO_OUT_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_control_call_info_complete(
    GIoRpcPeer* rpc,
    GBytes* resp,           /* NULL on failure */
    const GError* error,    /* NULL on success */
    gpointer user_data)
{
    FwdControlCall* call = user_data;
    FwdControlCallInfoFunc fn = call->complete.info;
    static const guint info_out_tags[] = {
        #define Tag(TAG) FWD_CONTROL_INFO_OUT_TAG_##TAG,
        INFO_OUT_TAGS(Tag) /* and zero terminator */ 0
        #define INFO_OUT_REQUIRED_BITS \
            (INFO_OUT_VERSIONS_BIT | INFO_OUT_BYTE_ORDER_BIT)
        #undef Tag
    };

    if (!error) {
        GUtilData data, values[INFO_OUT_TAG_COUNT];
        const int tlvs = gutil_tlvs_decode(gutil_data_from_bytes(&data, resp),
            info_out_tags, values, GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

        /* All known tags are required */
        if (tlvs == INFO_OUT_REQUIRED_BITS) {
            const GUtilData* data = values + INFO_OUT_VERSIONS_INDEX;
            const GUtilData* byteorder = values + INFO_OUT_BYTE_ORDER_INDEX;
            guint64 v1, v2;
            GUtilRange in;

            in.end = (in.ptr = data->bytes) + data->size;
            if (gutil_unsigned_mbn_decode(&in, &v1) &&
                gutil_unsigned_mbn_decode(&in, &v2) &&
                v1 && v2 >= v1 && v1 <= FWD_PROTOCOL_VERSION &&
                in.ptr == in.end &&
                byteorder->size == 4) {
                FwdControlInfo info;

                /* Success */
                memset(&info, 0, sizeof(info));
                info.v1 = (guint) v1;
                info.v2 = (guint) v2;
                memcpy(&info.byteorder, byteorder->bytes, byteorder->size);
                fn(&info, call->user_data);
                return;
            }
        }
        GDEBUG("Failed to parse INFO response");
    }

    /* Any kind of error */
    fn(NULL, call->user_data);
}

#define SOCKET_OUT_TAGS(t) t(ERROR) t(ID) t(ADDRESS)
enum fwd_control_socket_out_tag_index {
    #define TagIndex(TAG) SOCKET_OUT_##TAG##_INDEX,
    SOCKET_OUT_TAGS(TagIndex)
    SOCKET_OUT_TAG_COUNT
    #undef TagIndex
};
enum fwd_control_socket_out_tag_bit {
    #define TagBit(TAG) SOCKET_OUT_##TAG##_BIT = BIT_(SOCKET_OUT_##TAG##_INDEX),
    SOCKET_OUT_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_control_call_socket_complete(
    GIoRpcPeer* rpc,
    GBytes* response,       /* NULL on failure */
    const GError* error,    /* NULL on success */
    gpointer user_data)
{
    FwdControlCall* call = user_data;
    FwdControlCallSocketFunc fn = call->complete.socket;

    if (!error) {
        static const guint socket_out_tags[] = {
            #define Tag(TAG) FWD_CONTROL_SOCKET_OUT_TAG_##TAG,
            SOCKET_OUT_TAGS(Tag) /* and zero terminator */ 0
            #undef Tag
            /* Two allowed combinations of TLVs */
            #define SOCKET_OUT_SUCCESS_BITS (\
                SOCKET_OUT_ID_BIT | \
                SOCKET_OUT_ADDRESS_BIT)
            #define SOCKET_OUT_ERROR_BITS SOCKET_OUT_ERROR_BIT
            #define SOCKET_OUT_ALL_BITS (\
                SOCKET_OUT_SUCCESS_BITS | \
                SOCKET_OUT_ERROR_BITS)
        };
        GUtilData data, values[SOCKET_OUT_TAG_COUNT];
        int tlvs = gutil_tlvs_decode(gutil_data_from_bytes(&data, response),
            socket_out_tags, values, GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

        if (tlvs > 0) {
            guint value;

            /* Only two combination of TLVs are allowed */
            switch (tlvs & SOCKET_OUT_ALL_BITS) {
            case SOCKET_OUT_SUCCESS_BITS:
                /* Make sure that socket ID is within the allowed range */
                value = fwd_decode_socket_id(values + SOCKET_OUT_ID_INDEX);
                if (value) {
                    /* Parse the address */
                    const GUtilData* addr = values + SOCKET_OUT_ADDRESS_INDEX;
                    GSocketAddress* sa = g_socket_address_new_from_native
                        ((gpointer) addr->bytes, addr->size);

                    if (sa) {
                        if (G_IS_INET_SOCKET_ADDRESS(sa)) {
                            /* Success */
                            fn(value, G_INET_SOCKET_ADDRESS(sa),
                                call->user_data);
                            g_object_unref(sa);
                            return;
                        }
                        g_object_unref(sa);
                    }
                }
                break;

            case SOCKET_OUT_ERROR_BITS:
                /* Socket creation error */
                if (fwd_decode_uint_mbn(values + SOCKET_OUT_ERROR_INDEX,
                    &value) && value < INT_MAX) {
                    fn(-(int)value, NULL, call->user_data);
                    return;
                }
                break;
            }
        }
        /* Parsing error (could be considered fatal) */
    }
    fn(0, NULL, call->user_data);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

#define ASSERT_SOCKET_FLAG(NAME) \
    G_STATIC_ASSERT((int) FWD_SOCKET_FLAG_##NAME == \
                    (int) FWD_CONTROL_SOCKET_FLAG_##NAME)

ASSERT_SOCKET_FLAG(REUSADDR);
ASSERT_SOCKET_FLAG(LISTEN);
ASSERT_SOCKET_FLAG(RETRY_ANY);

guint
fwd_control_call_info(
    GIoRpcPeer* rpc,
    FwdControlCallInfoFunc fn,
    gpointer data,
    GDestroyNotify destroy)
{
    GDEBUG("< INFO");
    return giorpc_peer_call(rpc, FWD_IID_CONTROL, FWD_CONTROL_INFO,
        NULL, fn ? fwd_control_call_info_complete : NULL,
        fwd_control_call_new(G_CALLBACK(fn), data, destroy),
        fwd_control_call_destroy);
}

guint
fwd_control_call_socket(
    GIoRpcPeer* rpc,
    guint id,
    GSocketFamily af,
    GSocketType type,
    GInetSocketAddress* isa,
    FWD_SOCKET_FLAGS flg,
    guint backlog,
    guint timeout,
    guint maxconn,
    FwdControlCallSocketFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
{
    guint cid;
    GBytes* tlvs;
    GUtilData* sa = isa ? fwd_inet_socket_address_to_native(isa, NULL) : NULL;
    GByteArray* buf = fwd_tlv_append_umbn(
        fwd_tlv_append_umbn(
        fwd_tlv_append_umbn(g_byte_array_new(),
            FWD_CONTROL_SOCKET_IN_TAG_ID, id),
            FWD_CONTROL_SOCKET_IN_TAG_FAMILY, af),
            FWD_CONTROL_SOCKET_IN_TAG_TYPE, type);

    if (sa) {
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("< SOCKET [%u] %s flg=0x%02x", id,
                fwd_format_sockaddr_data(sa), (guint) flg);
        }
#endif /* GUTIL_LOG_DEBUG */
        fwd_tlv_append(buf, FWD_CONTROL_SOCKET_IN_TAG_ADDRESS, sa);
        g_free(sa);
    } else {
        GDEBUG("< SOCKET [%u] af=%d flg=0x%02x", id, af, (guint) flg);
    }

    if (flg) {
        fwd_tlv_append_umbn(buf, FWD_CONTROL_SOCKET_IN_TAG_FLAGS, flg);
    }

    if (backlog) {
        fwd_tlv_append_umbn(buf, FWD_CONTROL_SOCKET_IN_TAG_BACKLOG, backlog);
    }

    if (timeout) {
        fwd_tlv_append_umbn(buf, FWD_CONTROL_SOCKET_IN_TAG_TIMEOUT, timeout);
    }

    if (maxconn) {
        fwd_tlv_append_umbn(buf, FWD_CONTROL_SOCKET_IN_TAG_MAX_CONN, maxconn);
    }

    tlvs = g_byte_array_free_to_bytes(buf);
    cid = giorpc_peer_call(rpc, FWD_IID_CONTROL, FWD_CONTROL_SOCKET, tlvs,
        fn ? fwd_control_call_socket_complete : NULL,
        fwd_control_call_new(G_CALLBACK(fn), user_data, destroy),
        fwd_control_call_destroy);
    g_bytes_unref(tlvs);
    return cid;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
