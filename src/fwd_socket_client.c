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

#include "fwd_socket_client.h"

#include "fwd_log_p.h"
#include "fwd_protocol.h"
#include "fwd_util_p.h"

#include <giorpc.h>
#include <gutil_datapack.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

typedef struct fwd_socket_call {
    union {
        FwdSocketCallStatusFunc status;
        FwdSocketCallAcceptFunc accept;
        FwdSocketCallDataFunc data;
        GCallback callback;
    } complete;
    gpointer user_data;
    GDestroyNotify destroy;
} FwdSocketCall;

static
FwdSocketCall*
fwd_socket_call_new(
    GCallback complete,
    gpointer user_data,
    GDestroyNotify destroy)
{
    FwdSocketCall* call = g_slice_new0(FwdSocketCall);

    call->complete.callback = complete;
    call->user_data = user_data;
    call->destroy = destroy;
    return call;
}

static
void
fwd_socket_call_destroy(
    gpointer data)
{
    FwdSocketCall* call = data;

    if (call->destroy) {
        call->destroy(call->user_data);
    }
    gutil_slice_free(call);
}

static
guint
fwd_socket_call_start(
    GIoRpcPeer* rpc,
    FWD_SOCKET_CODE code,
    GBytes* data, /* Takes ownership */
    GIoRpcPeerResponseFunc response,
    GCallback complete,
    void* user_data,
    GDestroyNotify destroy)
{
    guint call_id = giorpc_peer_call(rpc, FWD_IID_SOCKET,
        code,  data, complete ? response : NULL,
        fwd_socket_call_new(complete, user_data, destroy),
        fwd_socket_call_destroy);

    g_bytes_unref(data);
    return call_id;
}

/*==========================================================================*
 * Completion callbacks
 *==========================================================================*/

static
void
fwd_socket_call_generic_status_complete(
    FwdSocketCall* call,
    guint status_tag,
    GBytes* response)
{
    FwdSocketCallStatusFunc fn = call->complete.status;

    if (response) {
        guint status, tags[2];
        GUtilData values[G_N_ELEMENTS(tags)-1];
        GUtilData data;

        tags[0] = status_tag;
        tags[1] = 0;
        if (gutil_tlvs_decode(gutil_data_from_bytes(&data, response),
            tags, values, GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS) == 1 &&
            fwd_decode_uint_mbn(values, &status) && status < INT_MAX) {
            fn(status, call->user_data);
            return;
        }
    }

    /* RPC error or parsing failure */
    fn(-1, call->user_data);
}

#define ACCEPT_OUT_TAGS(t) t(RESULT) t(CID) t(SRC_ADDRESS) t(DEST_ADDRESS)
enum fwd_socket_accept_out_tag_index {
    #define TagIndex(TAG) ACCEPT_##TAG##_INDEX,
    ACCEPT_OUT_TAGS(TagIndex)
    ACCEPT_OUT_TAG_COUNT
    #undef TagIndex
};
enum fwd_socket_accept_out_tag_bit {
    #define TagBit(TAG) ACCEPT_##TAG##_BIT = BIT_(ACCEPT_##TAG##_INDEX),
    ACCEPT_OUT_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_socket_call_accept_complete(
    GIoRpcPeer* rpc,
    GBytes* resp,           /* NULL on failure */
    const GError* error,    /* NULL on success */
    gpointer user_data)
{
    FwdSocketCall* call = user_data;
    FwdSocketCallAcceptFunc fn = call->complete.accept;

    if (!error) {
        static const guint accept_out_tags[] = {
            #define Tag(TAG) FWD_SOCKET_ACCEPT_OUT_TAG_##TAG,
            ACCEPT_OUT_TAGS(Tag) /* and zero terminator */ 0
            #undef Tag
        };
        guint status;
        GUtilData data, values[ACCEPT_OUT_TAG_COUNT];
        const int tlvs = gutil_tlvs_decode(gutil_data_from_bytes(&data, resp),
            accept_out_tags, values, GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

        if (tlvs > 0 && (tlvs & ACCEPT_RESULT_BIT) &&
            fwd_decode_uint_mbn(values + ACCEPT_RESULT_INDEX, &status) &&
            status < INT_MAX) {
            if (status == FWD_SOCKET_RESULT_OK) {
                guint cid = fwd_decode_socket_id(values + ACCEPT_CID_INDEX);
                const GUtilData* src_sa = (tlvs & ACCEPT_SRC_ADDRESS_BIT) ?
                    (values + ACCEPT_SRC_ADDRESS_INDEX) : NULL;
                const GUtilData* dest_sa = (tlvs & ACCEPT_DEST_ADDRESS_BIT) ?
                    (values + ACCEPT_DEST_ADDRESS_INDEX) : NULL;

                if (cid && src_sa) {
                    fn(status, cid, src_sa, dest_sa, call->user_data);
                    return;
                }
            } else {
                fn(status, 0, NULL, NULL, call->user_data);
                return;
            }
        }
    }

    /* RPC error or parsing failure */
    fn(-1, 0, NULL, NULL, call->user_data);
}

static
void
fwd_socket_call_connect_complete(
    GIoRpcPeer* rpc,
    GBytes* response,       /* NULL on failure */
    const GError* error,    /* NULL on success */
    gpointer user_data)
{
    fwd_socket_call_generic_status_complete(user_data,
        FWD_SOCKET_CONNECT_OUT_TAG_RESULT, response);
}

#define DATA_OUT_TAGS(t) t(RESULT) t(BYTES_SENT)
enum fwd_socket_data_out_tag_index {
    #define TagIndex(TAG) DATA_##TAG##_INDEX,
    DATA_OUT_TAGS(TagIndex)
    DATA_OUT_TAG_COUNT
    #undef TagIndex
};
enum fwd_socket_data_out_tag_bit {
    #define TagBit(TAG) DATA_##TAG##_BIT = BIT_(DATA_##TAG##_INDEX),
    DATA_OUT_TAGS(TagBit)
    #undef TagBit
};

static
void
fwd_socket_call_data_complete(
    GIoRpcPeer* rpc,
    GBytes* resp,           /* NULL on failure */
    const GError* error,    /* NULL on success */
    gpointer user_data)
{
    FwdSocketCall* call = user_data;
    FwdSocketCallDataFunc fn = call->complete.data;

    if (resp) {
        static const guint data_out_tags[] = {
            #define Tag(TAG) FWD_SOCKET_DATA_OUT_TAG_##TAG,
            DATA_OUT_TAGS(Tag) /* and zero terminator */ 0
            #undef Tag
        };
        guint status;
        GUtilData data, values[DATA_OUT_TAG_COUNT];
        const int tlvs = gutil_tlvs_decode(gutil_data_from_bytes(&data, resp),
            data_out_tags, values, GUTIL_TLVS_DECODE_FLAG_SKIP_UNKNOWN_TAGS);

        if (tlvs > 0 && (tlvs & DATA_RESULT_BIT) &&
            fwd_decode_uint_mbn(values + DATA_RESULT_INDEX, &status) &&
            status < INT_MAX) {
            if (status == FWD_SOCKET_RESULT_OK) {
                guint bytes_sent;

                if (fwd_decode_uint_mbn(values + DATA_BYTES_SENT_INDEX,
                    &bytes_sent)) {
                    fn(status, bytes_sent, call->user_data);
                    return;
                }
            }
        }
    }
    /* RPC error or parsing failure */
    fn(-1, 0, call->user_data);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

guint
fwd_socket_call_accept(
    GIoRpcPeer* rpc,
    guint id,
    FwdSocketCallAcceptFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
{
    GDEBUG("[%u] < ACCEPT", id);
    return fwd_socket_call_start(rpc, FWD_SOCKET_ACCEPT,
        fwd_tlv_new_umbn(FWD_SOCKET_ACCEPT_IN_TAG_ID, id),
        fwd_socket_call_accept_complete, G_CALLBACK(fn), user_data,
        destroy);
}

guint
fwd_socket_call_accepted(
    GIoRpcPeer* rpc,
    guint id,
    guint cid,
    gpointer user_data,
    GDestroyNotify destroy)
{
    GDEBUG("[%u] < ACCEPTED %u", id, cid);
    return fwd_socket_call_start(rpc,
        FWD_SOCKET_ACCEPTED, g_byte_array_free_to_bytes(
        fwd_tlv_append_umbn(fwd_tlv_append_umbn(g_byte_array_new(),
            FWD_SOCKET_ACCEPTED_IN_TAG_ID, id),
            FWD_SOCKET_ACCEPTED_IN_TAG_CID, cid)),
        NULL, NULL, user_data, destroy);
}

guint
fwd_socket_call_connect(
    GIoRpcPeer* rpc,
    guint id,
    GInetSocketAddress* isa,
    FwdSocketCallStatusFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
{
    GSocketAddress* sa = G_SOCKET_ADDRESS(isa);
    GUtilData* addr = fwd_socket_address_to_native(sa, NULL);

    if (addr) {
        GBytes* tlvs = g_byte_array_free_to_bytes(
            fwd_tlv_append(fwd_tlv_append_umbn(g_byte_array_new(),
                FWD_SOCKET_CONNECT_IN_TAG_ID, id),
                FWD_SOCKET_CONNECT_IN_TAG_ADDRESS, addr));

#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] < CONNECT %s", id, fwd_format_socket_address(sa));
        }
#endif /* GUTIL_LOG_DEBUG */

        g_free(addr);
        return fwd_socket_call_start(rpc, FWD_SOCKET_CONNECT,
           tlvs, fwd_socket_call_connect_complete,
            G_CALLBACK(fn), user_data, destroy);
    }
    return 0;
}

guint
fwd_socket_call_data(
    GIoRpcPeer* rpc,
    guint id,
    const GUtilData* data,
    GInetSocketAddress* sa,
    FwdSocketCallDataFunc fn,
    gpointer user_data,
    GDestroyNotify destroy)
{
    GUtilData* addr = fwd_inet_socket_address_to_native(sa, NULL);
    GByteArray* buf = fwd_tlv_append_umbn(g_byte_array_new(),
        FWD_SOCKET_DATA_IN_TAG_ID, id);

    if (addr) {
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("[%u] < DATA %u byte(s) from %s", id, (guint) data->size,
                   fwd_format_sockaddr_data(addr));
        }
#endif /* GUTIL_LOG_DEBUG */
        fwd_tlv_append(buf, FWD_SOCKET_DATA_IN_TAG_ADDRESS, addr);
        g_free(addr);
    } else {
        GDEBUG("[%u] < DATA %u byte(s)", id, (guint) data->size);
    }
    fwd_tlv_append(buf, FWD_SOCKET_DATA_IN_TAG_BYTES, data);

    return fwd_socket_call_start(rpc, FWD_SOCKET_DATA,
        g_byte_array_free_to_bytes(buf), fwd_socket_call_data_complete,
        G_CALLBACK(fn), user_data, destroy);
}

guint
fwd_socket_call_close(
    GIoRpcPeer* rpc,
    guint id)
{
    guint call_id = 0;

    if (id) {
        GBytes* data = fwd_tlv_new_umbn(FWD_SOCKET_CLOSE_IN_TAG_ID, id);

        GDEBUG("[%u] < CLOSE", id);
        call_id = giorpc_peer_call(rpc, FWD_IID_SOCKET, FWD_SOCKET_CLOSE,
            data, NULL, NULL, NULL);
        g_bytes_unref(data);
    }
    return call_id;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
