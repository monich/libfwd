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

#include "fwd_socket_io.h"
#include "fwd_util_p.h"

#include <gutil_log.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

static TestOpt test_opt;

#define TEST_(name) "/fwd/socket_io/" name

/*==========================================================================*
 * Common setup
 *==========================================================================*/

typedef struct test_data {
    FwdSocketIoHandler handler;
    GSocket* socket[2]; /* First connected to second */
    GInetSocketAddress* isa[2];
    GMainLoop* loop;
} TestData;

static
TestData*
test_data_from_handler(
    FwdSocketIoHandler* handler)
{
    g_assert(handler);
    return G_CAST(handler, TestData, handler);
}

static
TestData*
test_data_init(
    TestData* test,
    GSocketType type,
    const FwdSocketIoCallbacks* cb)
{
    int i;
    const GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    GError* error = NULL;

    test->loop = g_main_loop_new(NULL, FALSE);
    test->handler.cb = cb;
    for (i = 0; i < 2; i++) {
        GInetAddress* ia = g_inet_address_new_loopback(af);
        GSocketAddress* sa = g_inet_socket_address_new(ia, 0);
        GSocket* socket = g_socket_new(af, type, G_SOCKET_PROTOCOL_DEFAULT,
            NULL);

        g_assert(socket);
        g_assert(g_socket_bind(socket, sa, FALSE, &error));
        test->socket[i] = socket;
        g_object_unref(ia);
        g_object_unref(sa);

        sa = g_socket_get_local_address(socket, &error);
        g_assert(sa);
        test->isa[i] = G_INET_SOCKET_ADDRESS(sa);
        GDEBUG("Socket #%d %s", i, fwd_format_socket_address(sa));
    }

    return test;
}

static
void
test_data_cleanup(
    TestData* test)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(test->socket); i++) {
        g_object_unref(test->isa[i]);
        g_object_unref(test->socket[i]);
    }
    g_main_loop_unref(test->loop);
}


/*==========================================================================*
 * Common callbacks
 *==========================================================================*/

static
void
test_io_error_not_reached(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    g_assert_not_reached();
}

static
void
test_io_receive_not_reached(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    g_assert_not_reached();
}

static
void
test_io_sent_not_reached(
    gsize sent,
    const GError* error,
    void* user_data)
{
    g_assert_not_reached();
}

static
void
test_io_destroy_count(
    gpointer user_data)
{
    (*((int*)user_data))++;
}

/*==========================================================================*
 * Basic
 *==========================================================================*/

static
void
test_basic(
    void)
{
    TestData test;
    FwdSocketIo* io;
    static const guchar data[] = { 'd', 'a', 't', 'a' };
    static const FwdSocketIoCallbacks io_cb = {
        test_io_error_not_reached,
        test_io_receive_not_reached
    };
    GBytes* bytes = g_bytes_new_static(TEST_ARRAY_AND_SIZE(data));
    int destroyed = FALSE;

    test_data_init(&test, G_SOCKET_TYPE_DATAGRAM, &io_cb);
    io = fwd_socket_io_new(test.socket[0], g_main_context_default(),
        &test.handler);

    g_assert(io);
    fwd_socket_io_start_receive(io);
    fwd_socket_io_stop_receive(io);
    fwd_socket_io_stop_receive(io);  /* Second time has no effect */
    fwd_socket_io_send(io, test.isa[1], bytes,
        test_io_sent_not_reached, &destroyed, test_io_destroy_count);
    fwd_socket_io_send(io, test.isa[1], bytes,
        test_io_sent_not_reached, &destroyed, test_io_destroy_count);
    g_bytes_unref(bytes);
    fwd_socket_io_clear(&io);
    g_assert_null(io);
    fwd_socket_io_clear(&io); /* noop, it's already null */
    fwd_socket_io_free(io); /* noop, it's already null */
    g_assert_cmpint(destroyed, == ,2);
    test_data_cleanup(&test);
}

/*==========================================================================*
 * Error
 *==========================================================================*/

static
void
test_error_cb(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    TestData* test = test_data_from_handler(handler);

    GDEBUG("Error (expected) %s", GERRMSG(error));
    g_main_loop_quit(test->loop);
}

static
void
test_error(
    void)
{
    TestData test;
    FwdSocketIo* io;
    GError* error = NULL;
    static const FwdSocketIoCallbacks io_cb = {
        test_error_cb,
        test_io_receive_not_reached
    };

    test_data_init(&test, G_SOCKET_TYPE_STREAM, &io_cb);
    g_assert(g_socket_listen(test.socket[1], &error));
    g_assert(g_socket_connect(test.socket[0], G_SOCKET_ADDRESS(test.isa[1]),
        NULL, &error));
    io = fwd_socket_io_new(test.socket[0], g_main_context_default(),
        &test.handler);

    g_assert(g_socket_close(test.socket[1], &error));
    test_run(&test_opt, test.loop);

    fwd_socket_io_free(io);
    test_data_cleanup(&test);
}

/*==========================================================================*
 * SendOk
 *==========================================================================*/

typedef struct test_send_ok_data {
    TestData common;
    GUtilData data[2];
    guint sent;
    guint send_finished;
    guint received;
} TestSendData;

static
TestSendData*
test_send_ok_data_from_handler(
    FwdSocketIoHandler* handler)
{
    g_assert(handler);
    return G_CAST(handler, TestSendData, common.handler);
}

static
void
test_send_ok_receive_cb(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    TestSendData* test = test_send_ok_data_from_handler(handler);
    GError* error = NULL;
    GUtilData* sa_from = fwd_inet_socket_address_to_native
        (from, &error);
    GUtilData* sa_expected = fwd_inet_socket_address_to_native
        (test->common.isa[1], &error);

    g_assert_cmpuint(test->received, < ,G_N_ELEMENTS(test->data));
    g_assert(sa_from);
    g_assert(sa_expected);
    GDEBUG("Received %u bytes from %s", (guint) data->size,
        fwd_format_sockaddr_data(sa_from));
    g_assert(gutil_data_equal(sa_expected, sa_from));
    g_assert(gutil_data_equal(test->data + test->received, data));
    g_free(sa_expected);
    g_free(sa_from);

    test->received++;
    if (test->received == G_N_ELEMENTS(test->data)) {
        g_main_loop_quit(test->common.loop);
    }
}

static
void
test_send_ok_complete(
    gsize sent,
    const GError* error,
    void* user_data)
{
    TestSendData* test = user_data;

    GDEBUG("%u bytes sent", (guint) sent);
    g_assert(!error);
    g_assert_cmpuint(test->sent, < ,G_N_ELEMENTS(test->data));
    g_assert_cmpuint(test->data[test->sent].size, == ,sent);
    test->sent++;
}

static
void
test_send_ok_destroy(
    void* user_data)
{
    TestSendData* test = user_data;

    GDEBUG("send finished");
    g_assert_cmpuint(test->send_finished, < ,G_N_ELEMENTS(test->data));
    test->send_finished++;
}

static
void
test_send_ok(
    void)
{
    TestSendData test;
    FwdSocketIo* io[2];
    GMainContext* context = g_main_context_default();
    FwdSocketIoHandler* handler = &test.common.handler;
    static const guchar test_bytes0[] = { 'f', 'i', 'r', 's', 't' };
    static const guchar test_bytes1[] = { 'l', 'a', 's', 't' };
    GBytes* bytes0 = g_bytes_new_static(TEST_ARRAY_AND_SIZE(test_bytes0));
    GBytes* bytes1 = g_bytes_new_static(TEST_ARRAY_AND_SIZE(test_bytes1));
    static const FwdSocketIoCallbacks io_cb = {
        test_io_error_not_reached,
        test_send_ok_receive_cb
    };

    memset(&test, 0, sizeof(test));
    test.data[0].bytes = test_bytes0;
    test.data[0].size = sizeof(test_bytes0);
    test.data[1].bytes = test_bytes1;
    test.data[1].size = sizeof(test_bytes1);
    test_data_init(&test.common, G_SOCKET_TYPE_DATAGRAM, &io_cb);
    io[0] = fwd_socket_io_new(test.common.socket[0], context, handler);
    io[1] = fwd_socket_io_new(test.common.socket[1], context, handler);

    fwd_socket_io_start_receive(io[0]);
    fwd_socket_io_send(io[1], test.common.isa[0], bytes0,
        test_send_ok_complete, &test, test_send_ok_destroy);
    fwd_socket_io_send(io[1], test.common.isa[0], bytes1,
        test_send_ok_complete, &test, NULL);
    g_bytes_unref(bytes0);
    g_bytes_unref(bytes1);

    test_run(&test_opt, test.common.loop);
    g_assert_cmpuint(test.sent, == ,2);
    g_assert_cmpuint(test.send_finished, == ,1);
    g_assert_cmpuint(test.received, == ,2);

    fwd_socket_io_free(io[0]);
    fwd_socket_io_free(io[1]);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * SendError
 *==========================================================================*/

typedef struct test_send_error_data {
    TestData common;
    guint errors;
    guint send_finished;
} TestSendErrorData;

static
TestSendErrorData*
test_send_error_data_from_handler(
    FwdSocketIoHandler* handler)
{
    g_assert(handler);
    return G_CAST(handler, TestSendErrorData, common.handler);
}

static
void
test_sent_error_cb(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    TestSendErrorData* test = test_send_error_data_from_handler(handler);

    test->errors++;
    GDEBUG("Error (expected) %s", GERRMSG(error));
}

static
void
test_send_error_complete(
    gsize sent,
    const GError* error,
    void* user_data)
{
    TestSendErrorData* test = user_data;

    GDEBUG("%s", GERRMSG(error));
    g_assert(error);
    g_assert_cmpuint(sent, == ,0);
    g_assert_cmpuint(test->send_finished, == ,0);
    g_main_loop_quit(test->common.loop);
}

static
void
test_send_error_destroy(
    void* user_data)
{
    TestSendErrorData* test = user_data;

    GDEBUG("Send finished");
    test->send_finished++;
}

static
void
test_send_error(
    void)
{
    TestSendErrorData test;
    FwdSocketIo* io[2];
    GMainContext* context = g_main_context_default();
    FwdSocketIoHandler* handler = &test.common.handler;
    static const guchar test_bytes[] = { 'e', 'r', 'r', 'o', 'r' };
    GBytes* bytes = g_bytes_new_static(TEST_ARRAY_AND_SIZE(test_bytes));
    GError* error = NULL;
    static const FwdSocketIoCallbacks io_cb = {
        test_sent_error_cb,
        test_io_receive_not_reached
    };

    memset(&test, 0, sizeof(test));
    test_data_init(&test.common, G_SOCKET_TYPE_STREAM, &io_cb);
    g_assert(g_socket_listen(test.common.socket[0], &error));
    g_assert(g_socket_connect(test.common.socket[1],
        G_SOCKET_ADDRESS(test.common.isa[0]), NULL, &error));
    io[0] = fwd_socket_io_new(test.common.socket[0], context, handler);
    io[1] = fwd_socket_io_new(test.common.socket[1], context, handler);

    fwd_socket_io_start_receive(io[0]);
    fwd_socket_io_send(io[1], test.common.isa[0], bytes,
        test_send_error_complete, &test, test_send_error_destroy);
    fwd_socket_io_send(io[1], test.common.isa[0], bytes,
        test_send_error_complete, &test, test_send_error_destroy);
    g_assert(g_socket_shutdown(test.common.socket[0], TRUE, FALSE, &error));
    g_bytes_unref(bytes);

    test_run(&test_opt, test.common.loop);
    g_assert_cmpint(test.errors, == ,2);
    g_assert_cmpint(test.send_finished, == ,1);

    fwd_socket_io_free(io[0]);
    fwd_socket_io_free(io[1]);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * Receive
 *==========================================================================*/

typedef struct test_receive_data {
    TestData common;
    GUtilData data;
} TestReceiveData;

static
TestReceiveData*
test_receive_data_from_handler(
    FwdSocketIoHandler* handler)
{
    g_assert(handler);
    return G_CAST(handler, TestReceiveData, common.handler);
}

static
void
test_receive_cb(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    TestReceiveData* test = test_receive_data_from_handler(handler);
    GError* error = NULL;
    GUtilData* sa_from = fwd_inet_socket_address_to_native
        (from, &error);
    GUtilData* sa_expected = fwd_inet_socket_address_to_native
        (test->common.isa[1], &error);

    g_assert(sa_from);
    g_assert(sa_expected);
    GDEBUG("Received %u bytes from %s", (guint) data->size,
        fwd_format_sockaddr_data(sa_from));
    g_assert(gutil_data_equal(sa_expected, sa_from));
    g_assert(gutil_data_equal(&test->data, data));
    g_free(sa_expected);
    g_free(sa_from);

    g_main_loop_quit(test->common.loop);
}

static
void
test_receive(
    void)
{
    TestReceiveData test;
    FwdSocketIo* io;
    GError* error = NULL;
    static const guchar test_bytes[] = { 'd', 'a', 't', 'a' };
    static const FwdSocketIoCallbacks io_cb = {
        test_io_error_not_reached,
        test_receive_cb
    };

    memset(&test, 0, sizeof(test));
    test.data.bytes = test_bytes;
    test.data.size = sizeof(test_bytes);
    test_data_init(&test.common, G_SOCKET_TYPE_DATAGRAM, &io_cb);
    io = fwd_socket_io_new(test.common.socket[0], g_main_context_default(),
        &test.common.handler);

    fwd_socket_io_start_receive(io);
    fwd_socket_io_start_receive(io); /* Second time has no effect */
    g_assert_cmpint(g_socket_send_to(test.common.socket[1],
        G_SOCKET_ADDRESS(test.common.isa[0]), (void*) test.data.bytes,
        test.data.size, NULL, &error), == ,test.data.size);

    test_run(&test_opt, test.common.loop);

    fwd_socket_io_free(io);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    test_init(&test_opt, argc, argv);
    g_test_add_func(TEST_("basic"), test_basic);
    g_test_add_func(TEST_("error"), test_error);
    g_test_add_func(TEST_("send/ok"), test_send_ok);
    g_test_add_func(TEST_("send/error"), test_send_error);
    g_test_add_func(TEST_("receive"), test_receive);
    return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
