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

#include "test_common.h"

#include "libfwd.h"
#include "fwd_protocol.h"
#include "fwd_util_p.h"

#include <gutil_misc.h>
#include <gutil_idlepool.h>

#include <arpa/inet.h>
#include <errno.h>
#include <sys/un.h>

static TestOpt test_opt;

#define TEST_(name) "/fwd/util/" name

/*==========================================================================*
 * version
 *==========================================================================*/

static
void
test_version()
{
    g_assert_cmpuint(fwd_version(), == ,FWD_VERSION);
}

/*==========================================================================*
 * tlv_misc
 *==========================================================================*/

static
void
test_tlv_misc(
    void)
{
    static const guint8 out[] = { 0x01, 0x00 };
    GByteArray* buf = fwd_tlv_append(g_byte_array_new(), 1, NULL);

    g_assert_cmpmem(buf->data, buf->len, out, sizeof(out));
    g_byte_array_free(buf, TRUE);
}

/*==========================================================================*
 * encode_tlv
 *==========================================================================*/

typedef struct test_encode_data {
    const char* name;
    guint tag;
    guint value;
    guint value_offset;
    GUtilData out;
} TestEncodeUnsignedMbnTlv;

static const guint8 test_encode_tlv_1_2_out[] = { 0x01, 0x01, 0x02 };
static const guint8 test_encode_tlv_1_128_out[] = { 0x01, 0x02, 0x81, 0x00 };
static const guint8 test_encode_tlv_128_1_out[] = { 0x81, 0x00, 0x01, 0x01 };

static const TestEncodeUnsignedMbnTlv test_encode_tlv_all[] = {
#define TEST_CASE(x) TEST_("encode_tlv/") x
    {
        TEST_CASE("1_2"), 1, 2, 2,
        { TEST_ARRAY_AND_SIZE(test_encode_tlv_1_2_out) }
    },{
        TEST_CASE("1_128"), 1, 128, 2,
        { TEST_ARRAY_AND_SIZE(test_encode_tlv_1_128_out) }
    },{
        TEST_CASE("128_1"), 128, 1, 3,
        { TEST_ARRAY_AND_SIZE(test_encode_tlv_128_1_out) }
    }
#undef TEST_CASE
};

static
void
test_encode_tlv(
    gconstpointer test_data)
{
    const TestEncodeUnsignedMbnTlv* test = test_data;
    GByteArray* buf = g_byte_array_new();
    GUtilData in, out;
    GBytes* bytes1;
    GBytes* bytes2;
    guint8* data;

    /* Encode TLV into GBytes */
    bytes1 = fwd_tlv_new_umbn(test->tag, test->value);
    gutil_data_from_bytes(&out, bytes1);
    g_assert(gutil_data_equal(&out, &test->out));

    /* The same thing but with pre-encoded value */
    in.bytes = out.bytes + test->value_offset;
    in.size = out.size - test->value_offset;
    bytes2 = fwd_build_tlv(test->tag, &in);
    g_assert(g_bytes_equal(bytes1, bytes2));

    /* The same thing but into a plain buffer */
    data = g_malloc(out.size);
    g_assert(fwd_tlv_encode_umbn(data, test->tag, test->value) ==
        data + out.size);
    g_assert(!memcmp(data, out.bytes, out.size));

    /* The same thing but into a byte array */
    fwd_tlv_append_umbn(buf, test->tag, test->value);
    g_assert_cmpmem(buf->data, buf->len, out.bytes, out.size);

    g_bytes_unref(bytes1);
    g_bytes_unref(bytes2);
    g_byte_array_free(buf, TRUE);
    g_free(data);
}

/*==========================================================================*
 * format_socket_address
 *==========================================================================*/

typedef struct test_format_socket_address_data {
    const char* name;
    int af;
    const char* addr;
    guint16 port;
    const char* out;
} TestFormatSocketAddress;

static const TestFormatSocketAddress test_format_socket_address_all[] = {
#define TEST_CASE(x) TEST_("format_socket_address/") x
    { TEST_CASE("localhost"), AF_INET, "127.0.0.1", 23456, "127.0.0.1:23456" },
    { TEST_CASE("localhost6"), AF_INET6, "::1", 34567, "[::1]:34567" }
#undef TEST_CASE
};

static
void
test_format_socket_address(
    gconstpointer data)
{
    const TestFormatSocketAddress* test = data;
    struct sockaddr_storage ss;
    struct sockaddr* sa = (void*) &ss;
    GSocketAddress* address;
    GInetSocketAddress* isa;
    gsize sa_len = 0;
    void* addr = NULL;
    guint16* port = NULL;

    memset(&ss, 0, sizeof(ss));
    if (test->af == AF_INET) {
        struct sockaddr_in* in = (void*) sa;

        addr = &in->sin_addr;
        port = &in->sin_port;
        sa_len = sizeof(*in);
    } else if (test->af == AF_INET6) {
        struct sockaddr_in6* in6 = (void*) sa;

        addr = &in6->sin6_addr;
        port = &in6->sin6_port;
        sa_len = sizeof(*in6);
    } else {
        g_assert_not_reached();
    }

    sa->sa_family = test->af;
    g_assert_cmpint(inet_pton(sa->sa_family, test->addr, addr), == ,1);
    *port = htons(test->port);

    address = g_socket_address_new_from_native(sa, sa_len);
    isa = G_INET_SOCKET_ADDRESS(address);
    g_assert(address);
    g_assert(isa);

    g_assert_cmpstr(fwd_format_inet_socket_address(isa), == ,test->out);
    gutil_idle_pool_drain(gutil_idle_pool_get_default());
    g_object_unref(address);
}

/*==========================================================================*
 * format_socket_address/fail
 *==========================================================================*/

static
void
test_format_socket_address_fail(
    void)
{
    struct sockaddr sa;

    memset(&sa, 0, sizeof(sa));
    g_assert(!fwd_format_sockaddr(NULL, 0));
    g_assert(!fwd_format_sockaddr_data(NULL));
    g_assert(!fwd_format_sockaddr(&sa, sizeof(sa.sa_family)));
    g_assert(!fwd_format_sockaddr(&sa, sizeof(sa.sa_family) + 1));
    g_assert(!fwd_format_socket_address(NULL));
    g_assert(!fwd_format_socket_local_address(NULL));
    g_assert(!fwd_format_socket_remote_address(NULL));
    g_assert(!fwd_format_inet_socket_address(NULL));
    g_assert(!fwd_format_connection_remote_address(NULL));
}

/*==========================================================================*
 * socket_address_hash
 *==========================================================================*/

typedef struct test_socket_address_hash_data {
    const char* addr;
    guint16 port;
    guint hash;
} TestSocketAddressHash;

static const TestSocketAddressHash test_socket_address_hash_all[] = {
    { "127.0.0.1", 1234, 7499550 },
    { "127.0.0.1", 4321, 7499673 },
    { "192.168.1.1", 1234, 12440712 },
    { "192.168.1.1", 4321, 12440835 },
    { "2001:4860:4860::8888", 1234, 2701210423 },
    { "2001:4860:4860::8888", 4321, 2701210546 },
    { "2001:4860:4860::8844", 1234, 2701204915 },
    { "2001:4860:4860::8844", 4321, 2701205038 }
};

static
void
test_socket_address_hash_null(
    void)
{
    g_assert_cmpuint(fwd_inet_socket_address_hash(NULL), == ,0);
}

static
void
test_socket_address_hash(
    gconstpointer data)
{
    const TestSocketAddressHash* test = data;
    GInetAddress* ia = g_inet_address_new_from_string(test->addr);
    GSocketAddress* sa = g_inet_socket_address_new(ia, test->port);

    g_assert_cmpuint(fwd_inet_socket_address_hash(sa), == ,test->hash);
    g_object_unref(ia);
    g_object_unref(sa);
}

/*==========================================================================*
 * socket_address_equal
 *==========================================================================*/

static
void
test_socket_address_equal(
    void)
{
    GInetAddress* ia1 = g_inet_address_new_from_string("127.0.0.1");
    GInetAddress* ia2 = g_inet_address_new_from_string("1.2.3.4");
    GInetAddress* ia3 = g_inet_address_new_from_string("::1");
    GSocketAddress* sa1 = g_inet_socket_address_new(ia1, 1234);
    GSocketAddress* sa2 = g_inet_socket_address_new(ia2, 1234);
    GSocketAddress* sa3 = g_inet_socket_address_new(ia1, 4321);
    GSocketAddress* sa4 = g_inet_socket_address_new(ia1, 4321);
    GSocketAddress* sa5 = g_inet_socket_address_new(ia3, 4321);

    g_assert(fwd_inet_socket_address_equal(NULL, NULL));
    g_assert(fwd_inet_socket_address_equal(sa1, sa1));
    g_assert(fwd_inet_socket_address_equal(sa3, sa4));
    g_assert(!fwd_inet_socket_address_equal(sa1, NULL));
    g_assert(!fwd_inet_socket_address_equal(NULL, sa1));
    g_assert(!fwd_inet_socket_address_equal(sa1, sa2)); /* Different addr */
    g_assert(!fwd_inet_socket_address_equal(sa1, sa3)); /* Different port */
    g_assert(!fwd_inet_socket_address_equal(sa4, sa5)); /* Different af */
    g_object_unref(sa1);
    g_object_unref(sa2);
    g_object_unref(sa3);
    g_object_unref(sa4);
    g_object_unref(sa5);
    g_object_unref(ia1);
    g_object_unref(ia2);
    g_object_unref(ia3);
}

/*==========================================================================*
 * socket_address_to_native
 *==========================================================================*/

static
void
test_socket_address_to_native(
    void)
{
    struct sockaddr_in in;
    struct sockaddr_un un;
    static guint8 addr[4] = {127, 0, 0, 1};
    GInetSocketAddress* sa;
    GUtilData* sa_data;
    GUtilData addr_data;

    /* Failure */
    g_assert(!fwd_inet_socket_address_to_native(NULL, NULL));
    g_assert(!fwd_inet_socket_address_from_data(NULL));
    addr_data.bytes = addr;
    addr_data.size = 1;
    g_assert(!fwd_inet_socket_address_from_data(&addr_data));

    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    addr_data.bytes = (void*) &un;
    addr_data.size = sizeof(un);
    g_assert(!fwd_inet_socket_address_from_data(&addr_data));

    /* Prepare native address */
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = g_ntohs(1234);
    memcpy(&(in.sin_addr.s_addr), addr, 4);

    /* Convert it to GInetSocketAddress */
    addr_data.bytes = (void*) &in;
    addr_data.size = sizeof(in);
    sa = fwd_inet_socket_address_from_data(&addr_data);
    g_assert(sa);

    /* And back */
    sa_data = fwd_inet_socket_address_to_native(sa, NULL);
    g_assert(sa_data);
    g_assert_cmpuint(sa_data->size, == ,sizeof(in));
    g_assert(!memcmp(sa_data->bytes, &in, sa_data->size));

    g_object_unref(sa);
    g_free(sa_data);
}

/*==========================================================================*
 * decode_socket_id/ok
 *==========================================================================*/

typedef struct test_decode_socket_id_ok_data {
    const char* name;
    guint id;
    GUtilData in;
} TestDecodeSocketIdOk;

static const guint8 test_decode_socket_id_ok_1[] = { 0x01 };
static const guint8 test_decode_socket_id_ok_max[] = { 0xff, 0xff, 0xff, 0x7f };

static const TestDecodeSocketIdOk test_decode_socket_id_ok_all[] = {
#define TEST_CASE(x) TEST_("decode_socket_id/ok/") x
    {
        TEST_CASE("1"), 1,
        { TEST_ARRAY_AND_SIZE(test_decode_socket_id_ok_1) }
    },
    {
        TEST_CASE("max"), FWD_SOCKET_ID_MAX,
        { TEST_ARRAY_AND_SIZE(test_decode_socket_id_ok_max) }
    }
#undef TEST_CASE
};

static
void
test_decode_socket_id_ok(
    gconstpointer data)
{
    const TestDecodeSocketIdOk* test = data;
    guint id = fwd_decode_socket_id(&test->in);

    g_assert_cmpuint(id, == ,test->id);
}

/*==========================================================================*
 * decode_socket_id/fail
 *==========================================================================*/

typedef struct test_decode_socket_id_fail_data {
    const char* name;
    GUtilData in;
} TestDecodeSocketIdFail;

static const guint8 test_decode_socket_id_fail_0[] = { 0x00 };
static const guint8 test_decode_socket_id_fail_garbage[] = { 0xff };
static const guint8 test_decode_socket_id_fail_extra_data[] = { 0x00, 0x00 };
static const guint8 test_decode_socket_id_fail_too_big[] = {
    0x81, 0xff, 0xff, 0xff, 0x7f
};

static const TestDecodeSocketIdFail test_decode_socket_id_fail_all[] = {
#define TEST_CASE(x) TEST_("decode_socket_id/fail/") x
    {
        TEST_CASE("0"),
        { TEST_ARRAY_AND_SIZE(test_decode_socket_id_fail_0) }
    },{
        TEST_CASE("garbage"),
        { TEST_ARRAY_AND_SIZE(test_decode_socket_id_fail_garbage) }
    },{
        TEST_CASE("extra_data"),
        { TEST_ARRAY_AND_SIZE(test_decode_socket_id_fail_extra_data) }
    },{
        TEST_CASE("too_big"),
        { TEST_ARRAY_AND_SIZE(test_decode_socket_id_fail_too_big) }
    }
#undef TEST_CASE
};

static
void
test_decode_socket_id_fail(
    gconstpointer data)
{
    const TestDecodeSocketIdFail* test = data;

    g_assert_cmpuint(fwd_decode_socket_id(&test->in), == ,0);
}

/*==========================================================================*
 * decode_uint/ok
 *==========================================================================*/

typedef struct test_decode_uint_ok_data {
    const char* name;
    guint64 value;
    GUtilData in;
} TestDecodeUintOk;

static const guint8 test_decode_uint_ok_0[] = { 0x00 };
static const guint8 test_decode_uint_ok_1[] = { 0x01 };

static const TestDecodeUintOk test_decode_uint_ok_all[] = {
#define TEST_CASE(x) TEST_("decode_uint/ok/") x
    {
        TEST_CASE("0"), 0,
        { TEST_ARRAY_AND_SIZE(test_decode_uint_ok_0) }
    },{
        TEST_CASE("1"), 1,
        { TEST_ARRAY_AND_SIZE(test_decode_uint_ok_1) }
    }
#undef TEST_CASE
};

static
void
test_decode_uint_ok(
    gconstpointer data)
{
    const TestDecodeUintOk* test = data;
    guint value = 42;
    guint64 value64 = 43;

    g_assert(fwd_decode_uint_mbn(&test->in, &value));
    g_assert(fwd_decode_uint64_mbn(&test->in, &value64));
    g_assert_cmpuint(value, == ,test->value);
    g_assert_cmpuint(value64, == ,test->value);
}

/*==========================================================================*
 * decode_uint/fail
 *==========================================================================*/

typedef struct test_decode_uint_fail_data {
    const char* name;
    GUtilData in;
} TestDecodeUintFail;

static const guint8 test_decode_uint_fail_garbage[] = { 0xff };
static const guint8 test_decode_uint_fail_extra_data[] = { 0x00, 0x00 };
#if UINT_MAX == 0xffffffff
static const guint8 test_decode_uint_fail_too_big[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};
#endif

static const TestDecodeUintFail test_decode_uint_fail_all[] = {
#define TEST_CASE(x) TEST_("decode_uint/fail/") x
    {
        TEST_CASE("garbage"),
        { TEST_ARRAY_AND_SIZE(test_decode_uint_fail_garbage) }
    },{
        TEST_CASE("extra_data"),
        { TEST_ARRAY_AND_SIZE(test_decode_uint_fail_extra_data) }
#if UINT_MAX == 0xffffffff
    },{
        TEST_CASE("too_big"),
        { TEST_ARRAY_AND_SIZE(test_decode_uint_fail_too_big) }
#endif
    }
#undef TEST_CASE
};

static
void
test_decode_uint_fail(
    gconstpointer data)
{
    const TestDecodeUintFail* test = data;
    const guint original_value = 42;
    guint value = original_value;

    g_assert(!fwd_decode_uint_mbn(&test->in, &value));
    g_assert_cmpuint(value, == ,original_value);
}

/*==========================================================================*
 * decode_uint64/fail
 *==========================================================================*/

/* Reuse TestDecodeUintFail and its data */

static const TestDecodeUintFail test_decode_uint64_fail_all[] = {
#define TEST_CASE(x) TEST_("decode_uint64/fail/") x
    {
        TEST_CASE("garbage"),
        { TEST_ARRAY_AND_SIZE(test_decode_uint_fail_garbage) }
    },{
        TEST_CASE("extra_data"),
        { TEST_ARRAY_AND_SIZE(test_decode_uint_fail_extra_data) }
    }
#undef TEST_CASE
};

static
void
test_decode_uint64_fail(
    gconstpointer data)
{
    const TestDecodeUintFail* test = data;
    const guint original_value = 42;
    guint64 value = original_value;

    g_assert(!fwd_decode_uint64_mbn(&test->in, &value));
    g_assert_cmpuint(value, == ,original_value);
}

/*==========================================================================*
 * socket
 *==========================================================================*/

static
void
test_socket(
    void)
{
    struct sockaddr_in in;
    struct sockaddr* sa = (void*) &in;
    GUtilData sa_data = { (void*) &in, sizeof(in) };
    const GSocketType type = G_SOCKET_TYPE_DATAGRAM;
    GError* error = NULL;
    GSocketAddress* sa1;
    GSocketAddress* sa2;
    GSocket* s1;
    GSocket* s2;

    g_assert(!fwd_socket_new_from_data(type, NULL, FALSE, FALSE, &error));
    g_assert(error);
    g_clear_error(&error);

    memset(&in, 0, sizeof(in));
    g_assert(!fwd_socket_new(type, sa, sizeof(in), FALSE, FALSE, &error));
    g_assert(error);
    g_clear_error(&error);

    sa->sa_family = AF_INET;
    s1 = fwd_socket_new(type, sa, sizeof(in), TRUE, FALSE, &error);
    g_assert(s1);
    g_assert(!error);
    g_assert(in.sin_port); /* fwd_socket_new fills in the real address */
    g_assert((sa1 = g_socket_address_new_from_native(sa, sizeof(in))));

    /* Can't register the same address twice... */
    g_assert(!fwd_socket_new_from_data(type, &sa_data, FALSE, FALSE, NULL));
    g_assert(!fwd_socket_new_from_data(type, &sa_data, FALSE, FALSE, &error));
    g_assert(error);
    g_clear_error(&error);

    /* unless we allow address reuse */
    s2 = fwd_socket_new(type, sa, sizeof(in), TRUE, FALSE, &error);
    g_assert(s2);
    g_assert(!error);
    g_assert((sa2 = g_socket_address_new_from_native(sa, sizeof(in))));
    g_assert(fwd_inet_socket_address_equal(sa1, sa2));
    g_object_unref(s2);
    g_object_unref(sa2);

    /* or allow to try binding to any address */
    s2 = fwd_socket_new(type, sa, sizeof(in), FALSE, TRUE, &error);
    g_assert(s2);
    g_assert(!error);
    g_assert((sa2 = g_socket_address_new_from_native(sa, sizeof(in))));
    g_assert(!fwd_inet_socket_address_equal(sa1, sa2));
    g_object_unref(s2);
    g_object_unref(sa2);

    g_object_unref(s1);
    g_object_unref(sa1);
}

/*==========================================================================*
 * error_code
 *==========================================================================*/

static
void
test_error_code(
    void)
{
    g_assert_cmpint(fwd_error_to_code(NULL, 0), == ,0);
    g_assert_cmpint(fwd_error_to_code(g_error_new_literal(G_FILE_ERROR,
        G_FILE_ERROR_FAILED, "Test"), EFAULT), == ,EFAULT);
}

/*==========================================================================*
 * io_error_code
 *==========================================================================*/

typedef struct test_io_error_code_data {
    const char* name;
    int code;
    GIOErrorEnum io_error;
} TestIoErrorCode;

static const TestIoErrorCode test_io_error_code_all[] = {
#define TEST_IO_ERROR(x) TEST_("error_code/") #x, x
    { TEST_IO_ERROR(ENOMEM), G_IO_ERROR_NO_SPACE },
    { TEST_IO_ERROR(ENOTSUP), G_IO_ERROR_NOT_SUPPORTED },
    { TEST_IO_ERROR(EADDRINUSE), G_IO_ERROR_ADDRESS_IN_USE },
    { TEST_IO_ERROR(EINVAL), G_IO_ERROR_INVALID_ARGUMENT },
    { TEST_IO_ERROR(EACCES), G_IO_ERROR_PERMISSION_DENIED },
    { TEST_IO_ERROR(EMFILE), G_IO_ERROR_TOO_MANY_OPEN_FILES },
    { TEST_IO_ERROR(ECONNREFUSED), G_IO_ERROR_CONNECTION_REFUSED },
    { TEST_IO_ERROR(ETIMEDOUT), G_IO_ERROR_TIMED_OUT },
    { TEST_IO_ERROR(ENOTCONN), G_IO_ERROR_NOT_CONNECTED },
    { TEST_IO_ERROR(ECANCELED), G_IO_ERROR_CANCELLED },
    { TEST_IO_ERROR(EFAULT), G_IO_ERROR_FAILED }
#undef TEST_IO_ERROR
};

static
void
test_io_error_code(
    gconstpointer data)
{
    const TestIoErrorCode* test = data;
    GError* error = g_error_new_literal(G_IO_ERROR,
        g_io_error_from_errno(test->code), test->name);
    const int code = fwd_error_code(error, 0);

    g_assert_cmpint(code, == ,test->code);
    g_assert_cmpint(fwd_error_to_code(error, 0), == ,code);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

int main(int argc, char* argv[])
{
    guint i;

    g_test_init(&argc, &argv, NULL);
    test_init(&test_opt, argc, argv);

    g_test_add_func(TEST_("version"), test_version);
    g_test_add_func(TEST_("format_socket_address/fail"),
        test_format_socket_address_fail);

    g_test_add_func(TEST_("socket_address_hash/null"),
        test_socket_address_hash_null);
    for (i = 0; i < G_N_ELEMENTS(test_socket_address_hash_all); i++) {
        const TestSocketAddressHash* test = test_socket_address_hash_all + i;
        char* name = g_strdup_printf(TEST_("socket_address_hash/%s/%u"),
            test->addr, test->port);

        g_test_add_data_func(name, test, test_socket_address_hash);
        g_free(name);
    }

    g_test_add_func(TEST_("socket_address_equal"),
        test_socket_address_equal);
    g_test_add_func(TEST_("socket_address_to_native"),
        test_socket_address_to_native);
    g_test_add_func(TEST_("socket"), test_socket);
    g_test_add_func(TEST_("error_code/misc"), test_error_code);

    #define ADD_TEST_SET(test) do { \
        for (i = 0; i < G_N_ELEMENTS(test##_all); i++) { \
            g_test_add_data_func(test##_all[i].name, test##_all + i, test); \
        } } while (FALSE)

    g_test_add_func(TEST_("tlv_misc"), test_tlv_misc);
    ADD_TEST_SET(test_encode_tlv);
    ADD_TEST_SET(test_format_socket_address);
    ADD_TEST_SET(test_decode_socket_id_ok);
    ADD_TEST_SET(test_decode_socket_id_fail);
    ADD_TEST_SET(test_decode_uint_ok);
    ADD_TEST_SET(test_decode_uint_fail);
    ADD_TEST_SET(test_decode_uint64_fail);
    ADD_TEST_SET(test_io_error_code);
    return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
