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
#include "test_io_stream.h"

#include "libfwd.h"
#include "fwd_entry.h"
#include "fwd_peer_p.h"
#include "fwd_control_client.h"
#include "fwd_op_call.h"
#include "fwd_protocol.h"
#include "fwd_socket_client.h"
#include "fwd_socket_io.h"
#include "fwd_util_p.h"

#include <giorpc.h>

#include <gutil_misc.h>
#include <gutil_intarray.h>

#include <errno.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

static TestOpt test_opt;

typedef struct test_bytes {
    const char* name;
    GUtilData data;
} TestBytes;

#define TEST_(name) "/fwd/api/" name

#define test_socket_address_new_any(af,port) \
    fwd_inet_socket_address_new(af,port,TRUE)
#define test_socket_address_new_loopback(af,port) \
    fwd_inet_socket_address_new(af,port,FALSE)
#define test_socket_address_new_any_ipv4(port) \
    test_socket_address_new_any(G_SOCKET_FAMILY_IPV4, port)

static
GSocket*
test_socket_new(
    GSocketFamily af,
    GSocketType type,
    gboolean allow_reuse)
{
    GInetSocketAddress* sa = fwd_inet_socket_address_new(af, 0, TRUE);
    GSocket* s = g_socket_new(af, type, G_SOCKET_PROTOCOL_DEFAULT, NULL);

    g_assert(s);
    g_assert(g_socket_bind(s, G_SOCKET_ADDRESS(sa), allow_reuse, NULL));
    g_object_unref(sa);
    return s;
}

static
gushort
test_socket_get_local_port(
    GSocket* socket)
{
    GSocketAddress* sa = g_socket_get_local_address(socket, NULL);
    gushort port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(sa));

    g_object_unref(sa);
    return port;
}

static
void
test_io_error_not_reached(
    FwdSocketIoHandler* handler,
    const GError* error)
{
    GDEBUG("%s", GERRMSG(error));
    g_assert_not_reached();
}

static
void
test_socket_added(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    GUtilIntArray* ids = user_data;

    GDEBUG("Socket #%u added, state %d", id, state);
    g_assert(!gutil_int_array_contains(ids, id));
    gutil_int_array_append(ids, id);
}

static
void
test_socket_removed(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    GUtilIntArray* ids = user_data;

    GDEBUG("Socket #%u is removed", id);
    g_assert(gutil_int_array_remove(ids, id));
}

/*==========================================================================*
 * Common setup
 *==========================================================================*/

typedef enum test_data_flags {
    TEST_DATA_NO_FLAGS = 0,
    TEST_DATA_FLAG_PRE_START
} TEST_DATA_FLAGS;

typedef struct test_data {
    GIoRpcPeer* rpc[2];
    FwdPeer* fp[2];
    GMainLoop* loop;
} TestData;

typedef struct test_error_socket_data {
    TestData common;
    int sid;
} TestErrorSocketData;

static
void
test_data_init_start(
    GIoRpcPeer* rpc,
    void* loop)
{
    if (rpc->state == GIORPC_PEER_STATE_STARTED) {
        GDEBUG("Peer started");
        g_main_loop_quit(loop);
    }
}

static
TestData*
test_data_init_with_flags(
    TestData* setup,
    TEST_DATA_FLAGS flags)
{
    int i, fd[2];

    g_assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    setup->loop = g_main_loop_new(NULL, FALSE);
    for (i = 0; i < 2; i++) {
        GIOStream* stream = test_io_stream_new_fd(fd[i], TRUE);

        setup->rpc[i] = giorpc_peer_new(stream, NULL);
        g_object_unref(stream);
    }

    if (flags & TEST_DATA_FLAG_PRE_START) {
         for (i = 0; i < 2; i++) {
            giorpc_peer_start(setup->rpc[i]);
         }
         for (i = 0; i < 2; i++) {
            GIoRpcPeer* rpc = setup->rpc[i];

            if (rpc->state < GIORPC_PEER_STATE_STARTED) {
                gulong start_id = giorpc_peer_add_state_handler(rpc,
                    test_data_init_start, setup->loop);

                test_run(&test_opt, setup->loop);
                giorpc_peer_remove_handler(rpc, start_id);
                g_assert_cmpint(rpc->state, == ,GIORPC_PEER_STATE_STARTED);
            }
        }
    }

    for (i = 0; i < 2; i++) {
        GIoRpcPeer* rpc = setup->rpc[i];

        setup->fp[i] = fwd_peer_new_with_rpc(rpc);
        g_assert_cmpint(setup->fp[i]->state, == ,FWD_STATE_STARTING);
        if (!(flags & TEST_DATA_FLAG_PRE_START)) {
            giorpc_peer_start(rpc);
        }
    }

    return setup;
}

static
TestData*
test_data_init(
    TestData* setup)
{
    return test_data_init_with_flags(setup, TEST_DATA_NO_FLAGS);
}

static
void
test_data_cleanup(
    TestData* test)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(test->fp); i++) {
        giorpc_peer_unref(test->rpc[i]);
        fwd_peer_unref(test->fp[i]);
    }
    g_main_loop_unref(test->loop);
}

static
void
test_wait_remote_listen_proc(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* loop)
{
    GDEBUG("Remote socket #%u added, state %d", id, state);
    g_assert_cmpint(state, == ,FWD_SOCKET_STATE_LISTEN_LOCAL);
    test_quit_later(loop);
}

static
void
test_wait_remote_listen(
    TestData* test)
{
    FwdPeer* remote = test->fp[1];
    gulong wait_id = fwd_peer_add_socket_added_handler(remote,
        test_wait_remote_listen_proc, test->loop);

    /* Wait for the remote socket to start listening */
    test_run(&test_opt, test->loop);
    fwd_peer_remove_handler(remote, wait_id);
}

/*==========================================================================*
 * Forwarding test
 *
 * 1. The server side start listening
 * 2. The client connects
 * 3. The client sends 'in'
 * 4. The server receives 'in' and sends back 'out'
 * 5. The client receives 'out' and closes the connection
 *==========================================================================*/

typedef struct test_forward_data {
    TestData common;
    GSocket* client;
    GSource* client_read;
    GSocket* server;
    GSource* server_read;
    GUtilData in;
    GUtilData out;
} TestForwardData;

static
FwdPeer*
test_forward_init(
    TestForwardData* test)
{
    static const guchar data_in[] = { 'i', 'n' };
    static const guchar data_out[] = { 'o', 'u', 't' };

    memset(test, 0, sizeof(*test));
    test->in.bytes = data_in;
    test->in.size = sizeof(data_in);
    test->out.bytes = data_out;
    test->out.size = sizeof(data_out);
    return test_data_init(&test->common)->fp[0];
}

static
void
test_forward_cleanup(
    TestForwardData* test)
{
    g_assert(test->client);
    g_assert(test->server);
    if (test->client_read) {
        g_source_destroy(test->client_read);
        g_source_unref(test->client_read);
    }
    if (test->server_read) {
        g_source_destroy(test->server_read);
        g_source_unref(test->server_read);
    }

    g_object_unref(test->client);
    g_object_unref(test->server);
    test_data_cleanup(&test->common);
}

static
void
test_forward_socket_state_changed(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    TestForwardData* test = user_data;

    GDEBUG("Socket #%u state %d", id, state);
    g_assert_cmpint(state, == ,FWD_SOCKET_STATE_LISTEN_REMOTE);
    g_main_loop_quit(test->common.loop);
}

static
gboolean
test_forward_client_read_cb(
    GSocket* socket,
    GIOCondition condition,
    gpointer user_data)
{
    TestForwardData* test = user_data;
    GSocketAddress* from = NULL;
    GError* error = NULL;
    const GUtilData* expected = &test->out;
    gsize size = expected->size + 1; /* Allocate one extra byte */
    void* buf = g_malloc0(size);
    gssize received;

    g_assert(condition & G_IO_IN);
    received = g_socket_receive_from(socket, &from, buf, size, NULL, &error);
    if (!from) {
        from = g_socket_get_remote_address(socket, NULL);
    }
    GDEBUG("Client received %u bytes from %s", (guint) received,
        fwd_format_socket_address(from));
    g_assert_cmpint(received, == ,expected->size);
    g_assert(!memcmp(buf, expected->bytes, expected->size));
    g_free(buf);
    gutil_object_unref(from);
    g_source_unref(test->client_read);
    test->client_read = NULL;

    /* Close the client socket and wait for the other side to get notified */
    GDEBUG("Shutting down the client socket");
    g_assert(g_socket_shutdown(socket, TRUE, TRUE, &error));
    return G_SOURCE_REMOVE;
}

static
gboolean
test_forward_server_read_cb(
    GSocket* socket,
    GIOCondition condition,
    gpointer user_data)
{
    TestForwardData* test = user_data;
    GError* error = NULL;
    GSocketAddress* from = NULL;
    const GUtilData* expected = &test->in;
    gsize size = expected->size + 1; /* Allocate one extra byte */
    void* buf = g_malloc0(size);
    const gssize received = g_socket_receive_from(socket, &from, buf,
            size, NULL, &error);

    g_assert (condition & G_IO_IN);
    if (received > 0) {
        const GUtilData* back = &test->out;

        g_assert_cmpint(received, == ,expected->size);
        GDEBUG("Server received %u bytes from %s", (guint) received,
            fwd_format_socket_address(from));
        g_assert(!memcmp(buf, expected->bytes, expected->size));

        /* Wait for the data to come back */
        g_assert(!test->client_read);
        test->client_read = g_socket_create_source(test->client, G_IO_IN, NULL);
        g_source_set_callback(test->client_read, (GSourceFunc)
            test_forward_client_read_cb, test, NULL);
        g_source_attach(test->client_read, NULL);

        /* Send some data back */
        GDEBUG("Sending %u bytes back to the client", (guint) back->size);
        g_assert_cmpint(g_socket_send(test->server, (void*) back->bytes,
            back->size, NULL, NULL), == ,back->size);

        /* Wait for the data to be delivered */
        gutil_object_unref(from);
        g_free(buf);
        return G_SOURCE_CONTINUE;
    } else {
        g_assert_cmpint(received, == ,0);
        GDEBUG("Server EOF (expected)");
        gutil_object_unref(from);
        g_free(buf);

        g_source_unref(test->server_read);
        test->server_read = NULL;

        /* Close the socket */
        g_assert(g_socket_close(socket, &error));
        return G_SOURCE_REMOVE;
    }
}

static
void
test_forward_accept(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    TestForwardData* test = user_data;
    GSocketListener* listener = G_SOCKET_LISTENER(object);
    GError* error = NULL;
    GSocketConnection* connection = g_socket_listener_accept_finish(listener,
        result, NULL, &error);

    g_assert(connection);
    g_assert(!test->server);
    g_object_ref(test->server = g_socket_connection_get_socket(connection));

    GDEBUG("Server received connection from %s",
        fwd_format_connection_remote_address(connection));

    /* Wait for the data to arrive */
    g_assert(!test->server_read);
    test->server_read = g_socket_create_source(test->server, G_IO_IN, NULL);
    g_source_set_callback(test->server_read, (GSourceFunc)
        test_forward_server_read_cb, test, NULL);
    g_source_attach(test->server_read, NULL);
    g_object_unref(connection);
}

static
void
test_forward_connected(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    TestForwardData* test = user_data;
    const GUtilData* data = &test->in;
    GSocketClient* client = G_SOCKET_CLIENT(object);
    GError* error = NULL;
    GSocketConnection* connection = g_socket_client_connect_finish(client,
        result, &error);

    g_assert(connection);
    g_assert(!test->client);
    g_object_ref(test->client = g_socket_connection_get_socket(connection));

    GDEBUG("Client connected to %s",
        fwd_format_connection_remote_address(connection));
    g_object_unref(connection);

    /* Send the data */
    GDEBUG("Client sending %u bytes", (guint) data->size);
    g_assert_cmpint(g_socket_send(test->client, (void*) data->bytes,
        data->size, NULL, &error), == ,data->size);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

static
void
test_exit_loop_when_started(
    FwdPeer* fp,
    gpointer loop)
{
    if (fp->state == FWD_STATE_STARTED) {
        GDEBUG("Done");
        g_main_loop_quit(loop);
    }
}

static
void
test_exit_loop_when_stopped(
    FwdPeer* fp,
    gpointer loop)
{
    if (fp->state == FWD_STATE_STOPPED) {
        GDEBUG("Done");
        g_main_loop_quit(loop);
    }
}

static
void
test_socket_call_ok(
    int id,
    GInetSocketAddress* address,
    gpointer user_data)
{
    TestErrorSocketData* test = user_data;

    g_assert_cmpint(id, > ,0);
    g_assert(address);
    GDEBUG("SOCKET id %d", id);
    test->sid = id;
    g_main_loop_quit(test->common.loop);
}

static
void
test_socket_call(
    const TestBytes* data,
    FWD_SOCKET_CODE code,
    GIoRpcPeerResponseFunc fn)
{
    TestData test;
    GIoRpcPeer* rpc;
    GBytes* in = g_bytes_new_static(data->data.bytes, data->data.size);

    test_data_init_with_flags(&test, TEST_DATA_FLAG_PRE_START);
    rpc = test.rpc[0];

    /* Send a broken request */
    g_assert(giorpc_peer_call(rpc, FWD_IID_SOCKET, code, in, fn, &test,
        NULL));
    g_bytes_unref(in);
    test_run(&test_opt, test.loop);
    test_data_cleanup(&test);
}

/*==========================================================================*
 * basic
 *==========================================================================*/

static
void
test_basic()
{
    TestData test;
    gulong state_id;
    GInetSocketAddress* sa = test_socket_address_new_any_ipv4(0);

    /* Public APIs are NULL-resistant */
    g_assert(!fwd_peer_new(NULL, NULL));
    g_assert(!fwd_peer_new_with_rpc(NULL));
    g_assert(!fwd_peer_ref(NULL));
    g_assert(!fwd_peer_add_state_handler(NULL, NULL, NULL));
    g_assert(!fwd_peer_add_local_datagram_forwarder(NULL, G_SOCKET_FAMILY_IPV4,
        0, NULL, FWD_TIMEOUT_DEFAULT, 0, FWD_FLAGS_NONE, NULL));
    g_assert(!fwd_peer_add_local_stream_forwarder(NULL, G_SOCKET_FAMILY_IPV4,
        0, NULL, FWD_BACKLOG_DEFAULT, FWD_FLAGS_NONE, NULL));
    g_assert(!fwd_peer_add_remote_datagram_forwarder(NULL, G_SOCKET_FAMILY_IPV4,
        0, NULL, FWD_BACKLOG_DEFAULT, FWD_TIMEOUT_DEFAULT, 0,
        FWD_FLAGS_NONE, NULL));
    g_assert(!fwd_peer_add_remote_stream_forwarder(NULL, G_SOCKET_FAMILY_IPV4,
        0, NULL, FWD_BACKLOG_DEFAULT, FWD_FLAGS_NONE, NULL));
    g_assert(!fwd_peer_sync(NULL, 0, NULL));
    fwd_peer_remove_forwarder(NULL, 0);
    fwd_peer_remove_handler(NULL, 0);
    fwd_peer_remove_handler(NULL, 1);
    fwd_peer_unref(NULL);

    /* Some internal APIs too */
    g_assert(!fwd_entry_ref(NULL));
    g_assert(!fwd_entry_rpc(NULL));
    g_assert(!fwd_op_call_start(NULL, 0));
    g_assert(!fwd_peer_rpc(NULL));
    g_assert(!fwd_entry_insert(NULL));
    fwd_entry_unref(NULL);

    test_data_init(&test);
    g_assert(!fwd_peer_add_state_handler(test.fp[0], NULL, NULL));
    g_assert(fwd_peer_ref(test.fp[0]) == test.fp[0]);
    g_assert(fwd_peer_ref(test.fp[1]) == test.fp[1]);
    fwd_peer_unref(test.fp[0]);
    fwd_peer_unref(test.fp[1]);

    state_id = fwd_peer_add_state_handler(test.fp[0],
        test_exit_loop_when_started, test.loop);
    g_assert(state_id);
    g_assert(fwd_peer_add_local_stream_forwarder(test.fp[0],
        G_SOCKET_FAMILY_IPV4, 0, sa, 1, FWD_FLAGS_NONE, NULL));
    g_object_unref(sa);

    GDEBUG("Waiting for the tunnel to start");
    test_run(&test_opt, test.loop);
    fwd_peer_clear_handler(test.fp[0], &state_id);
    g_assert(!state_id);

    test_data_cleanup(&test);
}

/*==========================================================================*
 * stop
 *==========================================================================*/

static
gboolean
test_stop_cb(
    gpointer rpc)
{
    GDEBUG("Stopping");
    giorpc_peer_stop((GIoRpcPeer*)rpc);
    return G_SOURCE_REMOVE;
}

static
void
test_stop(
    void)
{
    TestData test;
    FwdPeer* fp;
    gulong id;

    memset(&test, 0, sizeof(test));
    fp = test_data_init_with_flags(&test, TEST_DATA_FLAG_PRE_START)->fp[0];
    id = fwd_peer_add_state_handler(fp, test_exit_loop_when_stopped, test.loop);
    g_idle_add(test_stop_cb, test.rpc[0]);
    test_run(&test_opt, test.loop);
    fwd_peer_clear_handler(fp, &id);
    test_data_cleanup(&test);
}

/*==========================================================================*
 * echo
 *==========================================================================*/

typedef struct test_echo {
    TestData setup;
    GBytes* data;
} TestEcho;

static
void
test_echo_done(
    GIoRpcPeer* rpc,
    GBytes* resp,
    const GError* error,
    gpointer user_data)
{
    TestEcho* test = user_data;

    g_assert(g_bytes_equal(resp, test->data));
    GDEBUG("Echo OK");
    g_main_loop_quit(test->setup.loop);
}

static
void
test_echo(
    void)
{
    static const guint8 echo_bytes[] = { 0x01, 0x02, 0x03 };
    GIoRpcPeer* rpc;
    TestEcho test;

    memset(&test, 0, sizeof(test));
    test.data = g_bytes_new_static(TEST_ARRAY_AND_SIZE(echo_bytes));
    test_data_init_with_flags(&test.setup, TEST_DATA_FLAG_PRE_START);

    rpc = test.setup.rpc[0];
    g_assert(giorpc_peer_call(rpc, FWD_IID_CONTROL, FWD_CONTROL_ECHO,
        test.data, test_echo_done, &test, NULL));
    test_run(&test_opt, test.setup.loop);

    test_data_cleanup(&test.setup);
    g_bytes_unref(test.data);
}

/*==========================================================================*
 * info/ok
 *==========================================================================*/

static
void
test_info_ok_done(
    const FwdControlInfo* info,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert(info);
    GDEBUG("INFO response %u:%u 0x%08x", info->v1, info->v2, info->byteorder);
    g_assert_cmpuint(info->v1, > ,0);
    g_assert_cmpuint(info->v2, >= ,info->v1);
    g_assert_cmpuint(info->byteorder, == ,FWD_BYTE_ORDER_MAGIC);
    g_main_loop_quit(test->loop);
}

static
void
test_info_ok(
    void)
{
    TestData test;

    test_data_init(&test);
    g_assert(fwd_control_call_info(test.rpc[0],
        test_info_ok_done, &test, NULL));
    test_run(&test_opt, test.loop);
    test_data_cleanup(&test);
}

/*==========================================================================*
 * info/error
 *==========================================================================*/

#define TEST_BYTE_ORDER_BLOCK \
    FWD_CONTROL_INFO_OUT_TAG_BYTE_ORDER, 0x04, \
    TEST_INT32_BYTES(FWD_BYTE_ORDER_MAGIC)

static const guint8 test_info_error_junk[] = { 0xff };
static const guint8 test_info_error_missing[] = { 0x7f, 0x00 };
static const guint8 test_info_error_v1[] =
    { 0x01, 0x00, TEST_BYTE_ORDER_BLOCK };
static const guint8 test_info_error_v2[] =
    { 0x01, 0x01, 0x01, TEST_BYTE_ORDER_BLOCK };
static const guint8 test_info_error_zero[] =
    { 0x01, 0x02, 0x00, 0x01, TEST_BYTE_ORDER_BLOCK };
static const guint8 test_info_error_order[] =
    { 0x01, 0x02, 0x02, 0x01, TEST_BYTE_ORDER_BLOCK };
static const guint8 test_info_error_extra[] =
    { 0x01, 0x03, 0x01, 0x02, 0x03, TEST_BYTE_ORDER_BLOCK };
static const guint8 test_info_error_compat[] = { 0x01, 0x02,
    FWD_PROTOCOL_VERSION + 1,
    FWD_PROTOCOL_VERSION + 2,
    TEST_BYTE_ORDER_BLOCK
};
static const guint8 test_info_error_byteorder[] = { 0x01, 0x02,
    FWD_PROTOCOL_VERSION, FWD_PROTOCOL_VERSION,
    FWD_CONTROL_INFO_OUT_TAG_BYTE_ORDER, 0x00
};

static const TestBytes test_info_error_all[] = {
    #define TEST_CASE(x) TEST_("info/error/") x
    {TEST_CASE("empty_response"), {NULL, 0}},
    {TEST_CASE("junk"), {TEST_ARRAY_AND_SIZE(test_info_error_junk)}},
    {TEST_CASE("missing"), {TEST_ARRAY_AND_SIZE(test_info_error_missing)}},
    {TEST_CASE("v1"), {TEST_ARRAY_AND_SIZE(test_info_error_v1)}},
    {TEST_CASE("v2"), {TEST_ARRAY_AND_SIZE(test_info_error_v2)}},
    {TEST_CASE("zero"), {TEST_ARRAY_AND_SIZE(test_info_error_zero)}},
    {TEST_CASE("order"), {TEST_ARRAY_AND_SIZE(test_info_error_order)}},
    {TEST_CASE("extra"), {TEST_ARRAY_AND_SIZE(test_info_error_extra)}},
    {TEST_CASE("compat"), {TEST_ARRAY_AND_SIZE(test_info_error_compat)}},
    {TEST_CASE("byteorder"), {TEST_ARRAY_AND_SIZE(test_info_error_byteorder)}}
    #undef TEST_CASE
};

static
void
test_info_error_done(
    FwdPeer* fp,
    void* loop)
{
    g_assert_cmpint(fp->state, == ,FWD_STATE_STOPPED);
    g_main_loop_quit(loop);
}

static
void
test_info_error_handler(
    GIoRpcPeer* rpc,
    guint iid,
    guint code,
    GBytes* data,
    GIoRpcRequest* req,
    gpointer user_data)
{
    const GUtilData* out = user_data;
    GBytes* resp = g_bytes_new_static(out->bytes, out->size);

    g_assert_cmpuint(iid, == ,FWD_IID_CONTROL);
    g_assert_cmpuint(code, == ,FWD_CONTROL_INFO);
    g_assert_cmpuint(g_bytes_get_size(data), == ,0);
    giorpc_request_complete(req, resp);
    g_bytes_unref(resp);
}

static
void
test_info_error(
    gconstpointer test_data)
{
    const TestBytes* test = test_data;
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    FwdPeer* fp;
    GIoRpcPeer* rpc;
    gulong handler_id, state_id;
    GIOStream* io;
    int fd[2];

    g_assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));

    io = test_io_stream_new_fd(fd[0], TRUE);
    giorpc_peer_start(rpc = giorpc_peer_new(io, NULL));
    g_object_unref(io);

    io = test_io_stream_new_fd(fd[1], TRUE);
    fp = fwd_peer_new(io, NULL);
    g_object_unref(io);

    handler_id = giorpc_peer_add_request_handler(rpc, FWD_IID_CONTROL,
        FWD_CONTROL_INFO, test_info_error_handler, (void*) &test->data);
    state_id = fwd_peer_add_state_handler(fp, test_info_error_done, loop);

    test_run(&test_opt, loop);

    g_assert_cmpint(fp->state, == ,FWD_STATE_STOPPED);
    giorpc_peer_remove_handler(rpc, handler_id);
    fwd_peer_remove_handler(fp, state_id);

    fwd_peer_unref(fp);
    giorpc_peer_unref(rpc);
    g_main_loop_unref(loop);
}

/*==========================================================================*
 * socket/inval
 *==========================================================================*/

static const guint8 test_socket_inval_junk[] = { 0xff };
static const guint8 test_socket_inval_missing[] = { 0x7f, 0x00 };
static const guint8 test_socket_inval_id[] = { 0x01, 0x01, 0xff };
static const guint8 test_socket_inval_id_none[] = { 0x02, 0x00 };
static const guint8 test_socket_inval_id_zero[] = { 0x01, 0x01, 0x00 };
static const guint8 test_socket_inval_af_junk[] =
    { 0x01, 0x01, 0x01, 0x02, 0x00 };
static const guint8 test_socket_inval_af[] =
    { 0x01, 0x01, 0x01, 0x02, 0x01, AF_MAX + 1 };
static const guint8 test_socket_inval_prot[] =
    { 0x01, 0x01, 0x01, 0x03, 0x00 };
static const guint8 test_socket_inval_prot0[] =
    { 0x01, 0x01, 0x01, 0x03, 0x01, 0x00 };
static const guint8 test_socket_inval_flags[] =
    { 0x01, 0x01, 0x01, 0x05, 0x00 };

G_STATIC_ASSERT(AF_MAX < 0x80);

static const TestBytes test_socket_inval_all[] = {
    #define TEST_CASE(x) TEST_("socket/inval/") x
    { TEST_CASE("junk"), {TEST_ARRAY_AND_SIZE(test_socket_inval_junk)} },
    { TEST_CASE("missing"), {TEST_ARRAY_AND_SIZE(test_socket_inval_missing)} },
    { TEST_CASE("id"), {TEST_ARRAY_AND_SIZE(test_socket_inval_id)} },
    { TEST_CASE("id_none"), {TEST_ARRAY_AND_SIZE(test_socket_inval_id_none)} },
    { TEST_CASE("id_zero"), {TEST_ARRAY_AND_SIZE(test_socket_inval_id_zero)} },
    { TEST_CASE("af_junk"), {TEST_ARRAY_AND_SIZE(test_socket_inval_af_junk)} },
    { TEST_CASE("af"), {TEST_ARRAY_AND_SIZE(test_socket_inval_af)} },
    { TEST_CASE("prot"), {TEST_ARRAY_AND_SIZE(test_socket_inval_prot)} },
    { TEST_CASE("prot0"), {TEST_ARRAY_AND_SIZE(test_socket_inval_prot0)} },
    { TEST_CASE("flags"), {TEST_ARRAY_AND_SIZE(test_socket_inval_flags)} }
    #undef TEST_CASE
};

static
void
test_socket_inval_done(
    GIoRpcPeer* rpc,
    GBytes* resp,
    const GError* error,
    gpointer user_data)
{
    static const guint8 expected_bytes[] = {
        FWD_CONTROL_SOCKET_OUT_TAG_ERROR, 0x01, EINVAL
    };
    static const GUtilData expected = { TEST_ARRAY_AND_SIZE(expected_bytes) };
    TestData* test = user_data;
    GUtilData tlv;

    g_assert(!error);
    g_assert(gutil_data_equal(gutil_data_from_bytes(&tlv, resp), &expected));
    GDEBUG("SOCKET failed (as expected)");
    g_main_loop_quit(test->loop);
}

static
void
test_socket_inval(
    gconstpointer test_data)
{
    const TestBytes* data = test_data;
    TestData test;
    GIoRpcPeer* rpc;
    GBytes* in = g_bytes_new_static(data->data.bytes, data->data.size);

    test_data_init_with_flags(&test, TEST_DATA_FLAG_PRE_START);
    rpc = test.rpc[0];

    g_assert(giorpc_peer_call(rpc, FWD_IID_CONTROL, FWD_CONTROL_SOCKET,
        in, test_socket_inval_done, &test, NULL));
    g_bytes_unref(in);
    test_run(&test_opt, test.loop);
    test_data_cleanup(&test);
}

/*==========================================================================*
 * socket/ok
 *==========================================================================*/

static
void
test_socket_ok_done(
    int id,
    GInetSocketAddress* address,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert_cmpint(id, > ,0);
    g_assert(address);
    GDEBUG("SOCKET id %d", id);
    g_main_loop_quit(test->loop);
}

static
void
test_socket_ok(
    void)
{
    TestData test;

    test_data_init(&test);
    g_assert(fwd_control_call_socket(test.rpc[0], 1234, AF_INET,
        SOCK_STREAM, NULL, FWD_SOCKET_NO_FLAGS, 0, FWD_TIMEOUT_DEFAULT,
        FWD_LIMIT_DEFAULT, test_socket_ok_done, &test, NULL));
    test_run(&test_opt, test.loop);
    test_data_cleanup(&test);
}

/*==========================================================================*
 * socket/error/family
 *==========================================================================*/

static
void
test_socket_error_family_done(
    int id,
    GInetSocketAddress* address,
    void* user_data)
{
    TestData* test = user_data;

    g_assert_cmpint(id, < ,0);
    g_assert(!address);
    GDEBUG("SOCKET failed (as expected)");
    g_main_loop_quit(test->loop);
}

static
void
test_socket_error_family(
    void)
{
    TestData test;
    GInetSocketAddress* address = test_socket_address_new_any_ipv4(0);

    test_data_init(&test);
    /* Socket family mismatch */
    g_assert(fwd_control_call_socket(test.rpc[0], 1234,
        G_SOCKET_FAMILY_IPV6, G_SOCKET_TYPE_DATAGRAM, address,
        FWD_SOCKET_NO_FLAGS, 0, FWD_TIMEOUT_DEFAULT, FWD_LIMIT_DEFAULT,
        test_socket_error_family_done, &test, NULL));
    test_run(&test_opt, test.loop);
    test_data_cleanup(&test);
    g_object_unref(address);
}

/*==========================================================================*
 * socket/error/bind
 *==========================================================================*/

typedef struct test_error_bind_data {
    TestData common;
    GInetSocketAddress* address;
} TestErrorBindData;

static
void
test_socket_error_bind_ok(
    int id,
    GInetSocketAddress* address,
    gpointer user_data)
{
    TestErrorBindData* test = user_data;

    g_assert_cmpint(id, > ,0);
    g_assert(address);
    GDEBUG("SOCKET id %d", id);
    g_object_ref(test->address = address);
    g_main_loop_quit(test->common.loop);
}

static
void
test_socket_error_bind_fail(
    int id,
    GInetSocketAddress* address,
    gpointer user_data)
{
    TestErrorBindData* test = user_data;

    g_assert_cmpint(id, < ,0);
    g_assert(!address);
    GDEBUG("SOCKET call error %d (as expected)", id);
    g_main_loop_quit(test->common.loop);
}

static
void
test_socket_error_bind(
    void)
{
    TestErrorBindData test;
    GIoRpcPeer* rpc;

    memset(&test, 0, sizeof(test));
    rpc = test_data_init(&test.common)->rpc[0];

    /* Bind a socket */
    g_assert(fwd_control_call_socket(rpc, 1234, AF_INET, SOCK_STREAM, NULL,
        FWD_SOCKET_NO_FLAGS, 0, FWD_TIMEOUT_DEFAULT, FWD_LIMIT_DEFAULT,
        test_socket_error_bind_ok, &test, NULL));
    test_run(&test_opt, test.common.loop);
    g_assert(test.address);

    /* Try to bind to the same address */
    g_assert(fwd_control_call_socket(rpc, 1234, AF_INET, SOCK_STREAM,
        test.address, FWD_SOCKET_NO_FLAGS, 0, FWD_TIMEOUT_DEFAULT,
        FWD_LIMIT_DEFAULT, test_socket_error_bind_fail, &test, NULL));
    test_run(&test_opt, test.common.loop);

    g_object_unref(test.address);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * connect/inval
 *==========================================================================*/

static const guint8 test_connect_inval_junk[] = { 0xff };
static const guint8 test_connect_inval_id[] = { 0x01, 0x01, 0x00, 0x02, 0x00};
static const guint8 test_connect_inval_addr[] =
    { 0x01, 0x01, 0x01, 0x02, 0x00 };

static const TestBytes test_connect_inval_all[] = {
    #define TEST_CASE(x) TEST_("connect/inval/") x
    { TEST_CASE("junk"), {TEST_ARRAY_AND_SIZE(test_connect_inval_junk)} },
    { TEST_CASE("id"), {TEST_ARRAY_AND_SIZE(test_connect_inval_id)} },
    { TEST_CASE("addr"), {TEST_ARRAY_AND_SIZE(test_connect_inval_addr)} }
    #undef TEST_CASE
};

static
void
test_connect_inval_done(
    GIoRpcPeer* rpc,
    GBytes* resp,
    const GError* error,
    gpointer user_data)
{
    static const guint8 expected_bytes[] = {
        FWD_CONTROL_SOCKET_OUT_TAG_ERROR, 0x01, EINVAL
    };
    static const GUtilData expected = { TEST_ARRAY_AND_SIZE(expected_bytes) };
    TestData* test = user_data;
    GUtilData tlv;

    g_assert(!error);
    g_assert(gutil_data_equal(gutil_data_from_bytes(&tlv, resp), &expected));
    GDEBUG("CONNECT failed (as expected)");
    g_main_loop_quit(test->loop);
}

static
void
test_connect_inval(
    gconstpointer data)
{
    test_socket_call(data, FWD_SOCKET_CONNECT, test_connect_inval_done);
}

/*==========================================================================*
 * connect/error
 *==========================================================================*/

static
void
test_connect_error_done(
    int result,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert_cmpint(result, > ,0);
    GDEBUG("CONNECT error %d (expected)", result);
    g_main_loop_quit(test->loop);
}

static
void
test_connect_error(
    void)
{
    TestErrorSocketData test;
    GIoRpcPeer* rpc;
    GSocket* socket = test_socket_new(G_SOCKET_FAMILY_IPV4,
        G_SOCKET_TYPE_STREAM, FALSE);
    GSocketAddress* sa = g_socket_get_local_address(socket, NULL);

    memset(&test, 0, sizeof(test));
    rpc = test_data_init(&test.common)->rpc[0];

    /* Bind a socket */
    g_assert(fwd_control_call_socket(rpc, 1234, AF_INET, SOCK_STREAM, NULL,
        FWD_SOCKET_NO_FLAGS, 0, FWD_TIMEOUT_DEFAULT, FWD_LIMIT_DEFAULT,
        test_socket_call_ok, &test, NULL));
    test_run(&test_opt, test.common.loop);
    g_assert_cmpint(test.sid, > ,0);

    /* Try to connect (and fail because the socket isn't listening) */
    g_assert(G_IS_INET_SOCKET_ADDRESS(sa));
    g_assert(fwd_socket_call_connect(rpc, test.sid,
        G_INET_SOCKET_ADDRESS(sa), test_connect_error_done,
        &test.common, NULL));
    test_run(&test_opt, test.common.loop);

    g_object_unref(socket);
    g_object_unref(sa);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * accept/inval
 *==========================================================================*/

static const guint8 test_accept_inval_junk[] = { 0xff };
static const guint8 test_accept_inval_id[] = { 0x01, 0x01, 0x00 };

static const TestBytes test_accept_inval_all[] = {
    #define TEST_CASE(x) TEST_("accept/inval/") x
    { TEST_CASE("junk"), {TEST_ARRAY_AND_SIZE(test_accept_inval_junk)} },
    { TEST_CASE("id"), {TEST_ARRAY_AND_SIZE(test_accept_inval_id)} }
    #undef TEST_CASE
};

static
void
test_accept_inval_done(
    GIoRpcPeer* rpc,
    GBytes* resp,
    const GError* error,
    gpointer user_data)
{
    static const guint8 expected_bytes[] = {
        FWD_CONTROL_SOCKET_OUT_TAG_ERROR, 0x01, EINVAL
    };
    static const GUtilData expected = { TEST_ARRAY_AND_SIZE(expected_bytes) };
    TestData* test = user_data;
    GUtilData tlv;

    g_assert(!error);
    g_assert(gutil_data_equal(gutil_data_from_bytes(&tlv, resp), &expected));
    GDEBUG("ACCEPT failed (as expected)");
    g_main_loop_quit(test->loop);
}

static
void
test_accept_inval(
    gconstpointer data)
{
    test_socket_call(data, FWD_SOCKET_ACCEPT, test_accept_inval_done);
}

/*==========================================================================*
 * accept/error
 *==========================================================================*/

static
void
test_accept_error_done(
    int result,
    guint rid,
    const GUtilData* src,
    const GUtilData* dest,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert_cmpint(result, > ,0);
    GDEBUG("ACCEPT error %d (expected)", result);
    g_main_loop_quit(test->loop);
}

static
void
test_accept_error(
    void)
{
    TestErrorSocketData test;
    GIoRpcPeer* rpc;
    GSocket* socket = test_socket_new(G_SOCKET_FAMILY_IPV4,
        G_SOCKET_TYPE_STREAM, FALSE);

    memset(&test, 0, sizeof(test));
    rpc = test_data_init(&test.common)->rpc[0];

    /* Bind a socket */
    g_assert(fwd_control_call_socket(rpc, 1234, AF_INET, SOCK_STREAM, NULL,
        FWD_SOCKET_NO_FLAGS, 0, FWD_TIMEOUT_DEFAULT, FWD_LIMIT_DEFAULT,
        test_socket_call_ok, &test, NULL));
    test_run(&test_opt, test.common.loop);
    g_assert_cmpint(test.sid, > ,0);

    /* This socket won't accept anything */
    g_assert(fwd_socket_call_accept(rpc, test.sid,
        test_accept_error_done, &test.common, NULL));
    test_run(&test_opt, test.common.loop);

    g_object_unref(socket);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * accepted/inval
 *==========================================================================*/

static const guint8 test_accepted_inval_junk[] = { 0xff };
static const guint8 test_accepted_inval_miss[] = { 0x01, 0x01, 0x01 };
static const guint8 test_accepted_inval_id[] =
    { 0x01, 0x01, 0x00, 0x02, 0x01, 0x02 };
static const guint8 test_accepted_inval_cid[] =
    { 0x01, 0x01, 0x01, 0x02, 0x01, 0x00 };

static const TestBytes test_accepted_inval_all[] = {
    #define TEST_CASE(x) TEST_("accepted/inval/") x
    { TEST_CASE("junk"), {TEST_ARRAY_AND_SIZE(test_accepted_inval_junk)} },
    { TEST_CASE("miss"), {TEST_ARRAY_AND_SIZE(test_accepted_inval_miss)} },
    { TEST_CASE("id"), {TEST_ARRAY_AND_SIZE(test_accepted_inval_id)} },
    { TEST_CASE("cid"), {TEST_ARRAY_AND_SIZE(test_accepted_inval_cid)} }
    #undef TEST_CASE
};

static
void
test_accepted_inval_done(
    GIoRpcPeer* rpc,
    GBytes* resp,
    const GError* error,
    gpointer user_data)
{
    TestData* test = user_data;

    /* ACCEPT doesn't return anything */
    g_assert(!error);
    g_assert_cmpuint(g_bytes_get_size(resp), == ,0);
    GDEBUG("ACCEPTED done");
    g_main_loop_quit(test->loop);
}

static
void
test_accepted_inval(
    gconstpointer data)
{
    test_socket_call(data, FWD_SOCKET_ACCEPTED, test_accepted_inval_done);
}

/*==========================================================================*
 * data/inval
 *==========================================================================*/

static const guint8 test_data_inval_junk[] = { 0xff };
static const guint8 test_data_inval_miss[] = { 0x01, 0x01, 0x01 };
static const guint8 test_data_inval_id[] = { 0x01, 0x01, 0x00, 0x02, 0x00 };

static const TestBytes test_data_inval_all[] = {
    #define TEST_CASE(x) TEST_("data/inval/") x
    { TEST_CASE("junk"), {TEST_ARRAY_AND_SIZE(test_data_inval_junk)} },
    { TEST_CASE("miss"), {TEST_ARRAY_AND_SIZE(test_data_inval_miss)} },
    { TEST_CASE("id"), {TEST_ARRAY_AND_SIZE(test_data_inval_id)} }
    #undef TEST_CASE
};

static
void
test_data_inval_done(
    GIoRpcPeer* rpc,
    GBytes* resp,
    const GError* error,
    gpointer user_data)
{
    static const guint8 expected_bytes[] = {
        FWD_CONTROL_SOCKET_OUT_TAG_ERROR, 0x01, EINVAL
    };
    static const GUtilData expected = { TEST_ARRAY_AND_SIZE(expected_bytes) };
    TestData* test = user_data;
    GUtilData tlv;

    g_assert(!error);
    g_assert(gutil_data_equal(gutil_data_from_bytes(&tlv, resp), &expected));
    GDEBUG("DATA failed (as expected)");
    g_main_loop_quit(test->loop);
}

static
void
test_data_inval(
    gconstpointer data)
{
    test_socket_call(data, FWD_SOCKET_DATA, test_data_inval_done);
}

/*==========================================================================*
 * data/error
 *==========================================================================*/

static
void
test_data_error_connect_ok(
    int result,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert_cmpint(result, == ,0);
    GDEBUG("Connected");
    g_main_loop_quit(test->loop);
}

static
void
test_data_error_fail_1(
    int result,
    guint nbytes,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert_cmpint(result, < ,0);
    g_assert_cmpuint(nbytes, == ,0);
    GDEBUG("DATA error %d (expected)", result);
    g_main_loop_quit(test->loop);
}

static
void
test_data_error_fail_2(
    int result,
    guint nbytes,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert_cmpint(result, != ,0);
    g_assert_cmpuint(nbytes, == ,0);
    GDEBUG("DATA error %d (expected)", result);
    test_quit_later(test->loop);
}

static
void
test_data_error(
    void)
{
    TestErrorSocketData test;
    GIoRpcPeer* rpc;
    static const guint8 data_bytes[] = { 0xff, 0xfe, 0xfd, 0xfc };
    static const GUtilData data =  { TEST_ARRAY_AND_SIZE(data_bytes) };
    GSocket* socket = test_socket_new(G_SOCKET_FAMILY_IPV4,
        G_SOCKET_TYPE_STREAM, FALSE);
    GSocketAddress* sa = g_socket_get_local_address(socket, NULL);

    memset(&test, 0, sizeof(test));
    rpc = test_data_init(&test.common)->rpc[0];

    /* Bind a socket */
    GDEBUG("Creating socket");
    g_assert(fwd_control_call_socket(rpc, 1234, AF_INET, SOCK_STREAM, NULL,
        FWD_SOCKET_NO_FLAGS, 0, FWD_TIMEOUT_DEFAULT, FWD_LIMIT_DEFAULT,
        test_socket_call_ok, &test, NULL));
    test_run(&test_opt, test.common.loop);
    g_assert_cmpint(test.sid, > ,0);

    /* Won't be able to send anything because the socket isn't connected */
    GDEBUG("Trying to send data to unconnected socket");
    g_assert(fwd_socket_call_data(rpc, test.sid, &data, NULL,
        test_data_error_fail_1, &test.common, NULL));
    test_run(&test_opt, test.common.loop);

    /* Connect it */
    GDEBUG("Connecting socket");
    g_assert(g_socket_listen(socket, NULL));
    g_assert(G_IS_INET_SOCKET_ADDRESS(sa));
    g_assert(fwd_socket_call_connect(rpc, test.sid,
        G_INET_SOCKET_ADDRESS(sa), test_data_error_connect_ok,
        &test.common, NULL));
    test_run(&test_opt, test.common.loop);

    /* Close the connection and try to send the data */
    GDEBUG("Trying to send data to closed socket");
    fwd_socket_call_close(rpc, test.sid);
    g_assert(fwd_socket_call_data(rpc, test.sid, &data, NULL,
        test_data_error_fail_2, &test.common, NULL));
    test_run(&test_opt, test.common.loop);

    g_assert(g_socket_close(socket, NULL));
    g_object_unref(socket);
    g_object_unref(sa);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * close/inval
 *==========================================================================*/

static const guint8 test_close_inval_junk[] = { 0xff };
static const guint8 test_close_inval_id[] = { 0x01, 0x01, 0x00 };

static const TestBytes test_close_inval_all[] = {
    #define TEST_CASE(x) TEST_("close/inval/") x
    { TEST_CASE("junk"), {TEST_ARRAY_AND_SIZE(test_close_inval_junk)} },
    { TEST_CASE("id"), {TEST_ARRAY_AND_SIZE(test_close_inval_id)} }
    #undef TEST_CASE
};

static
void
test_close_inval_done(
    GIoRpcPeer* rpc,
    GBytes* resp,
    const GError* error,
    gpointer user_data)
{
    TestData* test = user_data;

    g_assert(!error);
    g_assert(resp);
    g_assert_cmpuint(g_bytes_get_size(resp), == ,0);
    GDEBUG("CLOSE ok (never fails)");
    g_main_loop_quit(test->loop);
}

static
void
test_close_inval(
    gconstpointer data)
{
    test_socket_call(data, FWD_SOCKET_CLOSE, test_close_inval_done);
}

/*==========================================================================*
 * forward/datagram/../max_conn
 *==========================================================================*/

typedef struct test_forward_datagram_max_conn_data {
    TestData common;
    GUtilIntArray* ids;
} TestForwardlDatagramMaxConnData;

static
FwdPeer*
test_forward_datagram_max_conn_data_init(
    TestForwardlDatagramMaxConnData* test)
{
    memset(test, 0, sizeof(*test));

    test->ids = gutil_int_array_new();
    return test_data_init(&test->common)->fp[0];
}

static
void
test_forward_datagram_max_conn_data_cleanup(
    TestForwardlDatagramMaxConnData* test)
{
    gutil_int_array_free(test->ids, TRUE);
    test_data_cleanup(&test->common);
}

static
void
test_forward_datagram_max_conn_internal_socket_added(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    TestForwardlDatagramMaxConnData* test = user_data;

    GDEBUG("Socket #%u added, state %d", id, state);
    g_assert(!gutil_int_array_contains(test->ids, id));
    gutil_int_array_append(test->ids, id);
}

static
void
test_forward_datagram_max_conn_internal_socket_removed(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    TestForwardlDatagramMaxConnData* test = user_data;

    GDEBUG("Socket #%u is removed", id);
    g_assert(gutil_int_array_remove(test->ids, id));
    g_main_loop_quit(test->common.loop);
}

/*==========================================================================*
 * forward/datagram/local/max_conn
 *==========================================================================*/

static
void
test_forward_datagram_local_max_conn()
{
    TestForwardlDatagramMaxConnData test;
    const GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    const GSocketType type = G_SOCKET_TYPE_DATAGRAM;
    static const guchar data[] = { 'd', 'a', 't', 'a' };
    FwdPeer* fp = test_forward_datagram_max_conn_data_init(&test);
    GSocket* remote_socket = test_socket_new(af, type, FALSE);
    GSocket* send_sockets[3];
    GSocket* tmp = test_socket_new(af, type, TRUE);
    const gushort remote_port = test_socket_get_local_port(remote_socket);
    const gushort local_port = test_socket_get_local_port(tmp);
    GSocketAddress* sa_local = G_SOCKET_ADDRESS
        (test_socket_address_new_loopback(af, local_port));
    GInetSocketAddress* isa_remote = test_socket_address_new_loopback(af,
        remote_port);
    gulong id[2];
    guint i, fw_socket_id;

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        send_sockets[i] = test_socket_new(af, type, FALSE);
    }

    /* Expect notification when sockets are being added/removed */
    id[0] = fwd_peer_add_socket_added_handler(fp,
        test_forward_datagram_max_conn_internal_socket_added, &test);
    id[1] = fwd_peer_add_socket_removed_handler(fp, 0,
        test_forward_datagram_max_conn_internal_socket_removed, &test);
    g_assert(id[0]);
    g_assert(id[1]);

    /* Create the forwarder, 2 connection max */
    fw_socket_id = fwd_peer_add_local_datagram_forwarder(fp, af, local_port,
        isa_remote, FWD_TIMEOUT_DEFAULT, 2, FWD_FLAG_REUSE_ADDRESS, NULL);
    g_assert(fw_socket_id);

    /* Close the temporary socket to leave the forwarding socket alone */
    g_assert(g_socket_close(tmp, NULL));
    g_object_unref(tmp);

    /* Forward the data */
    GDEBUG("Forwarding %hu => %hu", local_port, remote_port);
    GDEBUG("Sending %u bytes to %s %u times", (guint) sizeof(data) ,
        fwd_format_socket_address(sa_local), (guint)
        G_N_ELEMENTS(send_sockets));
    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        g_assert_cmpint(g_socket_send_to(send_sockets[i], sa_local,
            (void*) data, sizeof(data), NULL, NULL), == ,sizeof(data));
    }
    test_run(&test_opt, test.common.loop);

    /* The loop exits when the first remote socket gets deleted. */
    g_assert_cmpuint(test.ids->count, == ,3); /* 1 + 2 must be left */
    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);

    GDEBUG("Removing the forwarder");
    fwd_peer_clear_all_handlers(fp, id);
    fwd_peer_remove_forwarder(fp, fw_socket_id);

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        g_object_unref(send_sockets[i]);
    }
    g_object_unref(remote_socket);
    g_object_unref(isa_remote);
    g_object_unref(sa_local);

    test_forward_datagram_max_conn_data_cleanup(&test);
}

/*==========================================================================*
 * forward/datagram/remote/max_conn
 *==========================================================================*/

static
void
test_forward_datagram_remote_max_conn()
{
    TestForwardlDatagramMaxConnData test;
    const GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    const GSocketType type = G_SOCKET_TYPE_DATAGRAM;
    static const guchar data[] = { 'd', 'a', 't', 'a' };
    FwdPeer* fp = test_forward_datagram_max_conn_data_init(&test);
    FwdPeer* remote = test.common.fp[1];
    GSocket* s_local = test_socket_new(af, type, FALSE);
    GInetSocketAddress* isa_local = G_INET_SOCKET_ADDRESS
        (g_socket_get_local_address(s_local, NULL));
    const gushort local_port = g_inet_socket_address_get_port(isa_local);
    GSocket* tmp = test_socket_new(af, type, TRUE);
    const gushort remote_port = test_socket_get_local_port(tmp);
    GSocketAddress* sa_remote = G_SOCKET_ADDRESS
        (test_socket_address_new_loopback(af, remote_port));
    GSocket* send_sockets[3];
    gulong id[2];
    guint i, fw_socket_id;

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        send_sockets[i] = test_socket_new(af, type, FALSE);
    }

    /* Create the forwarder, 2 connection max */
    fw_socket_id = fwd_peer_add_remote_datagram_forwarder(fp, af, remote_port,
        isa_local, FWD_BACKLOG_DEFAULT, FWD_TIMEOUT_DEFAULT, 2,
        FWD_FLAG_REUSE_ADDRESS, NULL);
    g_assert(fw_socket_id);

    /* Wait for the remote socket to start listening */
    test_wait_remote_listen(&test.common);

    /* Close the temporary socket to leave the forwarding socket alone */
    g_assert(g_socket_close(tmp, NULL));
    g_object_unref(tmp);

    /* Watch sockets (connections) being added/removed */
    id[0] = fwd_peer_add_socket_added_handler(remote,
        test_forward_datagram_max_conn_internal_socket_added, &test);
    id[1] = fwd_peer_add_socket_removed_handler(remote, 0,
        test_forward_datagram_max_conn_internal_socket_removed, &test);
    g_assert(id[0]);
    g_assert(id[1]);

    /* Forward the data */
    GDEBUG("Forwarding %hu => %hu", local_port, remote_port);
    GDEBUG("Sending %u bytes to %s %u times", (guint) sizeof(data) ,
        fwd_format_inet_socket_address(isa_local), (guint)
        G_N_ELEMENTS(send_sockets));
    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        g_assert_cmpint(g_socket_send_to(send_sockets[i], sa_remote,
            (void*) data, sizeof(data), NULL, NULL), == ,sizeof(data));
    }
    test_run(&test_opt, test.common.loop);

    /* The loop exits when the first remote socket gets deleted. */
    g_assert_cmpuint(test.ids->count, == ,2); /* 2 must remain there */
    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);

    GDEBUG("Removing the forwarder");
    fwd_peer_clear_all_handlers(remote, id);
    fwd_peer_remove_forwarder(fp, fw_socket_id);

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        g_object_unref(send_sockets[i]);
    }
    g_object_unref(s_local);
    g_object_unref(isa_local);
    g_object_unref(sa_remote);

    test_forward_datagram_max_conn_data_cleanup(&test);
}

/*==========================================================================*
 * forward/timeout
 *==========================================================================*/

typedef struct test_forward_timeout_data {
    TestData common;
    GUtilIntArray* internal_ids;
} TestForwardTimeoutData;

static
FwdPeer*
test_forward_timeout_data_init(
    TestForwardTimeoutData* test)
{
    memset(test, 0, sizeof(*test));

    test->internal_ids = gutil_int_array_new();
    return test_data_init(&test->common)->fp[0];
}

static
void
test_forward_timeout_data_cleanup(
    TestForwardTimeoutData* test)
{
    gutil_int_array_free(test->internal_ids, TRUE);
    test_data_cleanup(&test->common);
}

static
void
test_forward_timeout_internal_socket_added(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    TestForwardTimeoutData* test = user_data;

    GDEBUG("Internal socket #%u added, state %d", id, state);
    g_assert(!gutil_int_array_contains(test->internal_ids, id));
    gutil_int_array_append(test->internal_ids, id);
}

static
void
test_forward_timeout_internal_socket_removed(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    TestForwardTimeoutData* test = user_data;

    GDEBUG("Internal socket #%u is removed", id);
    g_assert(gutil_int_array_remove(test->internal_ids, id));
    if (!test->internal_ids->count) {
        test_quit_later(test->common.loop);
    }
}

/*==========================================================================*
 * forward/datagram/local/timeout
 *==========================================================================*/

static
void
test_forward_datagram_local_timeout()
{
    TestForwardTimeoutData test;
    const GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    const GSocketType type = G_SOCKET_TYPE_DATAGRAM;
    static const guchar data[] = { 'd', 'a', 't', 'a' };
    FwdPeer* fp = test_forward_timeout_data_init(&test);
    FwdPeer* remote = test.common.fp[1];
    GSocket* remote_socket = test_socket_new(af, type, FALSE);
    GSocket* send_sockets[2];
    GSocket* tmp = test_socket_new(af, type, TRUE);
    const gushort remote_port = test_socket_get_local_port(remote_socket);
    const gushort local_port = test_socket_get_local_port(tmp);
    GSocketAddress* sa_local = G_SOCKET_ADDRESS
        (test_socket_address_new_loopback(af,local_port));
    GInetSocketAddress* isa_remote = test_socket_address_new_loopback(af,
        remote_port);
    gulong id[2];
    guint i, fw_socket_id;

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        send_sockets[i] = test_socket_new(af, type, FALSE);
    }

    /* Expect notification when sockets are being added/removed */
    id[0] = fwd_peer_add_socket_added_handler(remote,
        test_forward_timeout_internal_socket_added, &test);
    id[1] = fwd_peer_add_socket_removed_handler(remote, 0,
        test_forward_timeout_internal_socket_removed, &test);
    g_assert(id[0]);
    g_assert(id[1]);

    /* Create the forwarder */
    fw_socket_id = fwd_peer_add_local_datagram_forwarder(fp, af, local_port,
        isa_remote, 500 /* ms */, FWD_LIMIT_DEFAULT, FWD_FLAG_REUSE_ADDRESS,
        NULL);
    g_assert(fw_socket_id);

    /* Close the temporary socket to leave the forwarding socket alone */
    g_assert(g_socket_close(tmp, NULL));
    g_object_unref(tmp);

    /* Forward the data */
    GDEBUG("Forwarding %hu => %hu", local_port, remote_port);
    GDEBUG("Sending %u bytes to %s 2*%u times", (guint) sizeof(data) ,
        fwd_format_socket_address(sa_local), (guint)
        G_N_ELEMENTS(send_sockets));
    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        g_assert_cmpint(g_socket_send_to(send_sockets[i], sa_local,
            (void*) data, sizeof(data), NULL, NULL), == ,sizeof(data));
        g_assert_cmpint(g_socket_send_to(send_sockets[i], sa_local,
            (void*) data, sizeof(data), NULL, NULL), == ,sizeof(data));
    }
    test_run(&test_opt, test.common.loop);

    /* The loop exits when inactivity timeout expires and the remote socket
     * gets deleted. */
    g_assert_cmpuint(test.internal_ids->count, == ,0);
    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);

    GDEBUG("Removing the forwarder");
    fwd_peer_remove_forwarder(fp, fw_socket_id);

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        g_object_unref(send_sockets[i]);
    }
    g_object_unref(remote_socket);
    g_object_unref(isa_remote);
    g_object_unref(sa_local);

    test_forward_timeout_data_cleanup(&test);
}

/*==========================================================================*
 * forward/datagram/remote/timeout
 *==========================================================================*/

static
void
test_forward_datagram_remote_timeout()
{
    TestForwardTimeoutData test;
    const GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    const GSocketType type = G_SOCKET_TYPE_DATAGRAM;
    static const guchar data[] = { 'd', 'a', 't', 'a' };
    FwdPeer* fp = test_forward_timeout_data_init(&test);
    FwdPeer* remote = test.common.fp[1];
    GSocket* tmp = test_socket_new(af, type, TRUE);
    GSocket* local_socket = test_socket_new(G_SOCKET_FAMILY_IPV4,
        G_SOCKET_TYPE_DATAGRAM, FALSE);
    GInetSocketAddress* local_addr = G_INET_SOCKET_ADDRESS
        (g_socket_get_local_address(local_socket, NULL));
    GSocket* remote_socket = test_socket_new(af, type, FALSE);
    GSocket* send_sockets[8];
    const gushort local_port = g_inet_socket_address_get_port(local_addr);
    const gushort remote_port = test_socket_get_local_port(tmp);
    GSocketAddress* sa_remote = G_SOCKET_ADDRESS
        (test_socket_address_new_loopback(af, remote_port));
    gulong id[2];
    guint i, j, fw_socket_id;

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        send_sockets[i] = test_socket_new(af, type, FALSE);
    }

    /*
     * Create the forwarder. Backlog is low enough to overflow the incoming
     * connection queue, and yet large enough to keep it non-empty (which
     * improves the coverage)
     */
    fw_socket_id = fwd_peer_add_remote_datagram_forwarder(fp, af, remote_port,
        local_addr, 3, 500 /* ms */, 4, FWD_FLAG_REUSE_ADDRESS,
        NULL);

    /* Wait for the remote socket to start listening */
    test_wait_remote_listen(&test.common);

    /* Close the temporary socket to leave the forwarding socket alone */
    g_assert(g_socket_close(tmp, NULL));
    g_object_unref(tmp);

    /* Expect notification when sockets are being added/removed */
    id[0] = fwd_peer_add_socket_added_handler(remote,
        test_forward_timeout_internal_socket_added, &test);
    id[1] = fwd_peer_add_socket_removed_handler(remote, 0,
        test_forward_timeout_internal_socket_removed, &test);
    g_assert(id[0]);
    g_assert(id[1]);

    /* Forward the data */
    GDEBUG("Forwarding %hu => %hu", remote_port, local_port);
    GDEBUG("Sending %u bytes to %s 2*%u times", (guint) sizeof(data) ,
        fwd_format_socket_address(sa_remote), (guint)
        G_N_ELEMENTS(send_sockets));
    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        for (j = 0; j < 2; j++) {
            g_assert_cmpint(g_socket_send_to(send_sockets[i], sa_remote,
                (void*) data, sizeof(data), NULL, NULL), == ,sizeof(data));
        }
    }
    test_run(&test_opt, test.common.loop);

    /* The loop exits when inactivity timeout expires and the remote socket
     * gets deleted. */
    g_assert_cmpuint(test.internal_ids->count, == ,0);
    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);

    GDEBUG("Removing the forwarder");
    fwd_peer_remove_forwarder(fp, fw_socket_id);

    for (i = 0; i < G_N_ELEMENTS(send_sockets); i++) {
        g_object_unref(send_sockets[i]);
    }

    g_object_unref(local_socket);
    g_object_unref(local_addr);
    g_object_unref(remote_socket);
    g_object_unref(sa_remote);

    test_forward_timeout_data_cleanup(&test);
}

/*==========================================================================*
 * forward/datagram/local
 *==========================================================================*/

typedef struct test_forward_datagram_local_data {
    TestData common;
    GSocket* remote_socket;
    GUtilData in, out;
    FwdSocketIoHandler remote;
    FwdSocketIoHandler local;
    guint remote_socket_id;
} TestForwardDatagramLocalData;

static
void
test_forward_datagram_local_io_receive_remote(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    TestForwardDatagramLocalData* test = G_CAST(handler,
        TestForwardDatagramLocalData, remote);

    GDEBUG("Received %u bytes from %s", (guint) data->size,
        fwd_format_inet_socket_address(from));
    g_assert(gutil_data_equal(data, &test->in));

    /* Send some data in the opposite direction */
    GDEBUG("Sending %u bytes back", (guint) test->out.size);
    g_assert_cmpint(g_socket_send_to(test->remote_socket,
        G_SOCKET_ADDRESS(from), (void*) test->out.bytes, test->out.size,
        NULL, NULL), == , test->out.size);
}

static
void
test_forward_datagram_local_io_receive_local(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    TestForwardDatagramLocalData* test = G_CAST(handler,
        TestForwardDatagramLocalData, local);

    GDEBUG("Received %u bytes from %s", (guint) data->size,
        fwd_format_inet_socket_address(from));
    g_assert(gutil_data_equal(data, &test->out));
    test_quit_later(test->common.loop);
}

static
void
test_forward_datagram_local_socket_added_remote(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    TestForwardDatagramLocalData* test = user_data;

    GDEBUG("Remote socket #%u added, state %d", id, state);
    g_assert_cmpuint(test->remote_socket_id, == ,0);
    test->remote_socket_id = id;
}

static
void
test_forward_datagram_local_socket_removed_remote(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    TestForwardDatagramLocalData* test = user_data;

    GDEBUG("Remote socket #%u is removed", id);
    g_assert_cmpuint(test->remote_socket_id, == ,id);
    test->remote_socket_id = 0;
    test_quit_later(test->common.loop);
}

static
FwdPeer*
test_forward_datagram_local_init(
    TestForwardDatagramLocalData* test)
{
    static FwdSocketIoCallbacks remote_io_cb = {
        test_io_error_not_reached,
        test_forward_datagram_local_io_receive_remote
    };
    static FwdSocketIoCallbacks local_io_cb = {
        test_io_error_not_reached,
        test_forward_datagram_local_io_receive_local
    };
    static const guchar data_in[] = { 'i', 'n' };
    static const guchar data_out[] = { 'o', 'u', 't' };

    memset(test, 0, sizeof(*test));
    test->in.bytes = data_in;
    test->in.size = sizeof(data_in);
    test->out.bytes = data_out;
    test->out.size = sizeof(data_out);
    test->remote_socket = test_socket_new(G_SOCKET_FAMILY_IPV4,
        G_SOCKET_TYPE_DATAGRAM, FALSE);
    test->remote.cb = &remote_io_cb;
    test->local.cb = &local_io_cb;
    return test_data_init(&test->common)->fp[0];
}

static
void
test_forward_datagram_local()
{
    TestForwardDatagramLocalData test;
    const GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    const GSocketType type = G_SOCKET_TYPE_DATAGRAM;
    FwdPeer* fp = test_forward_datagram_local_init(&test);
    FwdPeer* remote = test.common.fp[1];
    GSocket* local_socket = test_socket_new(af, type, FALSE);
    GSocket* tmp = test_socket_new(af, type, TRUE);
    const gushort remote_port = test_socket_get_local_port(test.remote_socket);
    const gushort local_port = test_socket_get_local_port(tmp);
    GInetSocketAddress* sa_remote = test_socket_address_new_loopback(af,
        remote_port);
    GInetSocketAddress* sa_local = test_socket_address_new_loopback(af,
        local_port);
    guint fw_socket_id;
    GUtilIntArray* local_ids = gutil_int_array_new();
    GMainContext* context = g_main_context_default();
    FwdSocketIo* remote_io;
    FwdSocketIo* local_io;
    gulong id[2], remote_id[2];

    /* Start the listeners */
    remote_io = fwd_socket_io_new(test.remote_socket, context, &test.remote);
    local_io = fwd_socket_io_new(local_socket, context, &test.local);
    fwd_socket_io_start_receive(remote_io);
    fwd_socket_io_start_receive(local_io);

    /* Expect notification when sockets are being added/removed */
    id[0] = fwd_peer_add_socket_added_handler(fp,
        test_socket_added, local_ids);
    id[1] = fwd_peer_add_socket_removed_handler(fp, 0,
        test_socket_removed, local_ids);
    g_assert(id[0]);
    g_assert(id[1]);

    remote_id[0] = fwd_peer_add_socket_added_handler(remote,
        test_forward_datagram_local_socket_added_remote, &test);
    remote_id[1] = fwd_peer_add_socket_removed_handler(remote, 0,
        test_forward_datagram_local_socket_removed_remote, &test);
    g_assert(remote_id[0]);
    g_assert(remote_id[1]);

    /* Create the forwarder */
    fw_socket_id = fwd_peer_add_local_datagram_forwarder(fp, af, local_port,
        sa_remote, TEST_TIMEOUT_MS, FWD_LIMIT_DEFAULT, FWD_FLAG_REUSE_ADDRESS,
        NULL);
    g_assert(fw_socket_id);
    g_assert_cmpuint(local_ids->count, == ,1);
    g_assert_cmpint(local_ids->data[0], == ,fw_socket_id);

    /* Close the temporary socket to leave the forwarding socket alone */
    g_assert(g_socket_close(tmp, NULL));
    g_object_unref(tmp);

    /* Forward the data */
    GDEBUG("Forwarding %hu => %hu", local_port, remote_port);
    GDEBUG("Sending %u bytes to %s", (guint) test.in.size ,
        fwd_format_inet_socket_address(sa_local));
    g_assert_cmpint(g_socket_send_to(local_socket, G_SOCKET_ADDRESS(sa_local),
        (void*) test.in.bytes, test.in.size, NULL, NULL), == ,test.in.size);
    test_run(&test_opt, test.common.loop);

    /* The second (connection) internal socket must be created by now */
    g_assert_cmpuint(local_ids->count, == ,2);
    g_assert_cmpuint(test.remote_socket_id, != ,0);
    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);

    /* Do it again (this time datagram connection already exists) */
    GDEBUG("Sending another %u bytes to %s", (guint) test.in.size ,
        fwd_format_inet_socket_address(sa_local));
    g_assert_cmpint(g_socket_send_to(local_socket, G_SOCKET_ADDRESS(sa_local),
        (void*) test.in.bytes, test.in.size, NULL, NULL), == ,test.in.size);
    test_run(&test_opt, test.common.loop);

    GDEBUG("Removing the forwarder");
    fwd_peer_remove_forwarder(fp, fw_socket_id);

    /* All local sockets must have been removed by the above call */
    g_assert_cmpuint(local_ids->count, == ,0);
    fwd_peer_clear_all_handlers(fp, id);

    /* But not the remote socket (yet) */
    g_assert_cmpuint(test.remote_socket_id, != ,0);
    GDEBUG("Waiting for the remote socket to disappear");
    test_run(&test_opt, test.common.loop);
    g_assert_cmpuint(test.remote_socket_id, == ,0);
    fwd_peer_clear_all_handlers(remote, remote_id);

    gutil_int_array_free(local_ids, TRUE);
    fwd_socket_io_free(remote_io);
    fwd_socket_io_free(local_io);
    g_object_unref(local_socket);
    g_object_unref(sa_remote);
    g_object_unref(sa_local);

    g_object_unref(test.remote_socket);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * forward/datagram/remote
 *==========================================================================*/

typedef struct test_forward_datagram_remote_data {
    TestData common;
    GSocket* local_socket;
    GUtilData in, out;
    FwdSocketIoHandler local;
    FwdSocketIoHandler remote;
} TestForwardDatagramRemoteData;

static
void
test_forward_datagram_remote_io_receive_local(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    TestForwardDatagramRemoteData* test = G_CAST(handler,
        TestForwardDatagramRemoteData, local);

    GDEBUG("Received %u bytes from %s", (guint) data->size,
        fwd_format_inet_socket_address(from));
    g_assert(gutil_data_equal(data, &test->in));

    /* Send some data in the opposite direction */
    GDEBUG("Sending %u bytes back", (guint) test->out.size);
    g_assert_cmpint(g_socket_send_to(test->local_socket,
        G_SOCKET_ADDRESS(from), (void*) test->out.bytes, test->out.size,
        NULL, NULL), == , test->out.size);
}

static
void
test_forward_datagram_remote_io_receive_remote(
    FwdSocketIoHandler* handler,
    GInetSocketAddress* from,
    const GUtilData* data)
{
    TestForwardDatagramRemoteData* test = G_CAST(handler,
        TestForwardDatagramRemoteData, remote);

    GDEBUG("Received %u bytes from %s", (guint) data->size,
        fwd_format_inet_socket_address(from));
    g_assert(gutil_data_equal(data, &test->out));
    test_quit_later(test->common.loop);
}

static
void
test_forward_datagram_remote_socket_wait_remove(
    FwdPeer* fp,
    guint id,
    void* loop)
{
    GDEBUG("Local socket #%u is removed", id);
    test_quit_later(loop);
}

static
FwdPeer*
test_forward_datagram_remote_init(
    TestForwardDatagramRemoteData* test)
{
    static FwdSocketIoCallbacks local_io_cb = {
        test_io_error_not_reached,
        test_forward_datagram_remote_io_receive_local
    };
    static FwdSocketIoCallbacks remote_io_cb = {
        test_io_error_not_reached,
        test_forward_datagram_remote_io_receive_remote
    };
    static const guchar data_in[] = { 'i', 'n' };
    static const guchar data_out[] = { 'o', 'u', 't' };

    memset(test, 0, sizeof(*test));
    test->in.bytes = data_in;
    test->in.size = sizeof(data_in);
    test->out.bytes = data_out;
    test->out.size = sizeof(data_out);
    test->local_socket = test_socket_new(G_SOCKET_FAMILY_IPV4,
        G_SOCKET_TYPE_DATAGRAM, FALSE);
    test->local.cb = &local_io_cb;
    test->remote.cb = &remote_io_cb;
    return test_data_init(&test->common)->fp[0];
}

static
void
test_forward_datagram_remote()
{
    TestForwardDatagramRemoteData test;
    const GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    const GSocketType type = G_SOCKET_TYPE_DATAGRAM;
    FwdPeer* fp = test_forward_datagram_remote_init(&test);
    GSocket* tmp = test_socket_new(af, type, TRUE);
    GSocket* remote_socket = test_socket_new(af, type, FALSE);
    GInetSocketAddress* local_addr = G_INET_SOCKET_ADDRESS
        (g_socket_get_local_address(test.local_socket, NULL));
    const gushort local_port = g_inet_socket_address_get_port(local_addr);
    const gushort remote_port = test_socket_get_local_port(tmp);
    GInetSocketAddress* remote_addr = test_socket_address_new_loopback(af,
        remote_port);
    guint fw_socket_id;
    GUtilIntArray* local_ids = gutil_int_array_new();
    GMainContext* context = g_main_context_default();
    FwdSocketIo* remote_io;
    FwdSocketIo* local_io;
    gulong wait_id, id[2];

    /* Start the listeners */
    remote_io = fwd_socket_io_new(remote_socket, context, &test.remote);
    local_io = fwd_socket_io_new(test.local_socket, context, &test.local);
    fwd_socket_io_start_receive(remote_io);
    fwd_socket_io_start_receive(local_io);

    /* Expect notification when sockets are being added/removed */
    id[0] = fwd_peer_add_socket_added_handler(fp, test_socket_added, local_ids);
    id[1] = fwd_peer_add_socket_removed_handler(fp, 0, test_socket_removed,
        local_ids);
    g_assert(id[0]);
    g_assert(id[1]);

    /* Create the forwarder */
    fw_socket_id = fwd_peer_add_remote_datagram_forwarder(fp, af, remote_port,
        local_addr, 1, TEST_TIMEOUT_MS, FWD_LIMIT_DEFAULT,
        FWD_FLAG_REUSE_ADDRESS, NULL);
    g_assert(fw_socket_id);

    /* Wait for the remote socket to start listening */
    test_wait_remote_listen(&test.common);

    /*
     * Sync with the remote side before sending the data. Otherwise the UDP
     * packet may get delivered before the ACCEPT calls go through.
     */
    GDEBUG("Forwarding %hu => %hu", remote_port, local_port);
    g_assert(fwd_peer_sync(fp, TEST_TIMEOUT_MS, NULL));

    /* Now we can safely send the data */
    GDEBUG("Sending %u bytes to %s", (guint) test.in.size ,
        fwd_format_inet_socket_address(remote_addr));
    g_assert_cmpint(g_socket_send_to(remote_socket,
        G_SOCKET_ADDRESS(remote_addr), (void*) test.in.bytes, test.in.size,
        NULL, NULL), == ,test.in.size);
    test_run(&test_opt, test.common.loop);

    /* The second (connection) internal socket must be created by now */
    g_assert_cmpuint(local_ids->count, == ,2);
    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);

    GDEBUG("Removing the forwarder");
    fwd_peer_remove_forwarder(fp, fw_socket_id);

    /* Only one must be left */
    g_assert_cmpuint(local_ids->count, == ,1);

    /* Wait for the second (last) local socket to disappear */
    wait_id = fwd_peer_add_socket_removed_handler(fp, 0,
        test_forward_datagram_remote_socket_wait_remove, test.common.loop);
    GDEBUG("Waiting for the last local socket to disappear");
    test_run(&test_opt, test.common.loop);
    fwd_peer_remove_handler(fp, wait_id);

    fwd_peer_clear_all_handlers(fp, id);
    gutil_int_array_free(local_ids, TRUE);
    fwd_socket_io_free(remote_io);
    fwd_socket_io_free(local_io);
    g_object_unref(remote_socket);
    g_object_unref(remote_addr);
    g_object_unref(local_addr);
    g_object_unref(tmp);

    g_object_unref(test.local_socket);
    test_data_cleanup(&test.common);
}

/*==========================================================================*
 * forward/stream/local
 *==========================================================================*/

typedef struct test_forward_stream_local_data {
    TestForwardData forward;
    guint connection_socket_id;
} TestForwardStreamLocalData;

static
void
test_forward_stream_local_connection_added(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    TestForwardStreamLocalData* test = user_data;

    GDEBUG("Remote socket #%u added, state %d", id, state);
    g_assert(!test->connection_socket_id);
    test->connection_socket_id = id;
}

static
void
test_forward_stream_local_connection_removed(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    TestForwardStreamLocalData* test = user_data;

    GDEBUG("Remote socket #%u is removed", id);
    g_assert_cmpuint(test->connection_socket_id, == ,id);
    test_quit_later(test->forward.common.loop);
}

static
void
test_forward_stream_local_socket_added(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    guint* socket_id = user_data;

    GDEBUG("Local socket #%u added, state %d", id, state);
    g_assert_cmpint(state, == ,FWD_SOCKET_STATE_LISTEN_LOCAL);
    g_assert_cmpuint(id, != ,0);
    g_assert_cmpuint(*socket_id, == ,0);
    *socket_id = id;
}

static
void
test_forward_stream_local()
{
    TestForwardStreamLocalData test;
    FwdPeer* fp = test_forward_init(&test.forward);
    FwdPeer* remote = test.forward.common.fp[1];
    GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    GSocketType type = G_SOCKET_TYPE_STREAM;
    GSocket* tmp = test_socket_new(af, type, TRUE);
    GSocketListener* listener = g_socket_listener_new();
    GCancellable* server_cancel = g_cancellable_new();
    const gushort server_port = g_socket_listener_add_any_inet_port
        (listener, NULL, NULL);
    GInetSocketAddress* sa_remote =
        test_socket_address_new_loopback(af, server_port);
    const gushort listen_port = test_socket_get_local_port(tmp);
    GSocketClient* connector = g_socket_client_new();
    GSocketConnectable* connectable = G_SOCKET_CONNECTABLE
        (test_socket_address_new_loopback(af, listen_port));
    guint fw_socket_id, socket_id = 0;
    gulong id[2];

    /* Start the listener that will accept the forwarded connection */
    g_assert(server_port);
    g_socket_listener_accept_async(listener, server_cancel,
        test_forward_accept, &test);

    /* Expect a notification when socket is being added */
    id[0] = fwd_peer_add_socket_added_handler(fp,
        test_forward_stream_local_socket_added, &socket_id);
    g_assert(id[0]);

    /* Create the forwarding socket */
    GDEBUG("Creating the forwarder");
    fw_socket_id = fwd_peer_add_local_stream_forwarder(fp, af, listen_port,
        sa_remote, FWD_BACKLOG_DEFAULT, FWD_FLAG_REUSE_ADDRESS, NULL);
    g_assert_cmpint(fw_socket_id, > ,0);
    g_assert_cmpint(fw_socket_id, == ,socket_id);

    /* Delete it and create it again */
    socket_id = 0;
    GDEBUG("Deleting this forwarder");
    fwd_peer_remove_forwarder(fp, fw_socket_id);
    GDEBUG("Re-creating the forwarder");
    fw_socket_id = fwd_peer_add_local_stream_forwarder(fp, af, listen_port,
        sa_remote, FWD_BACKLOG_DEFAULT, FWD_FLAG_REUSE_ADDRESS, NULL);
    g_assert_cmpint(fw_socket_id, > ,0);
    g_assert_cmpint(fw_socket_id, == ,socket_id);
    fwd_peer_remove_handler(fp, id[0]);

    /* Wait for the connection socket to be created and terminated */
    test.connection_socket_id = 0;
    id[0] = fwd_peer_add_socket_added_handler(remote,
        test_forward_stream_local_connection_added, &test);
    id[1] = fwd_peer_add_socket_removed_handler(remote, 0,
        test_forward_stream_local_connection_removed, &test);

    /* Close the temporary socket to leave the forwarding socket alone */
    g_assert(g_socket_close(tmp, NULL));
    g_object_unref(tmp);
    g_object_unref(sa_remote);

    /* Forward the connection */
    GDEBUG("Forwarding %hu => %hu", listen_port, server_port);
    g_socket_client_connect_async(connector, connectable, NULL,
        test_forward_connected, &test);

    test_run(&test_opt, test.forward.common.loop);

    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);
    fwd_peer_remove_forwarder(fp, fw_socket_id);
    fwd_peer_remove_forwarder(fp, fw_socket_id); /* noop */
    fwd_peer_clear_all_handlers(remote, id);
    g_object_unref(connectable);
    g_object_unref(connector);
    g_cancellable_cancel(server_cancel);
    g_object_unref(server_cancel);
    g_socket_listener_close(listener);
    g_object_unref(listener);
    test_forward_cleanup(&test.forward);
}

/*==========================================================================*
 * forward/stream/remote
 *==========================================================================*/

typedef struct test_forward_stream_remote_data {
    TestForwardData forward;
    guint connection_socket_id;
} TestForwardStreamRemoteData;

static
void
test_forward_stream_remote_connection_added(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    TestForwardStreamRemoteData* test = user_data;

    GDEBUG("Local socket #%u added, state %d", id, state);
    g_assert(!test->connection_socket_id);
    test->connection_socket_id = id;
}

static
void
test_forward_stream_remote_connection_removed(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    TestForwardStreamRemoteData* test = user_data;

    GDEBUG("Local socket #%u is removed", id);
    g_assert_cmpuint(test->connection_socket_id, == ,id);
    test_quit_later(test->forward.common.loop);
}

static
void
test_forward_stream_remote_socket_removed(
    FwdPeer* fp,
    guint id,
    void* user_data)
{
    TestForwardStreamRemoteData* test = user_data;

    GDEBUG("Local socket #%u is removed", id);
    test_quit_later(test->forward.common.loop);
}

static
void
test_forward_stream_remote_socket_added(
    FwdPeer* fp,
    guint id,
    FWD_SOCKET_STATE state,
    void* user_data)
{
    guint* socket_id = user_data;

    GDEBUG("Socket #%u added, state %d", id, state);
    g_assert_cmpint(state, == ,FWD_SOCKET_STATE_LISTEN_REMOTE_STARTING);
    g_assert_cmpuint(id, != ,0);
    g_assert_cmpuint(*socket_id, == ,0);
    *socket_id = id;
}

static
void
test_forward_stream_remote()
{
    TestForwardStreamRemoteData test;
    FwdPeer* fp = test_forward_init(&test.forward);
    FwdPeer* local = test.forward.common.fp[1];
    GSocketFamily af = G_SOCKET_FAMILY_IPV4;
    GSocketType type = G_SOCKET_TYPE_STREAM;
    GSocket* tmp = test_socket_new(af, type, TRUE);
    GSocketListener* listener = g_socket_listener_new();
    GCancellable* server_cancel = g_cancellable_new();
    const gushort server_port = g_socket_listener_add_any_inet_port
        (listener, NULL, NULL);
    GInetSocketAddress* sa_local =
        test_socket_address_new_loopback(af, server_port);
    const gushort listen_port = test_socket_get_local_port(tmp);
    GSocketClient* connector = g_socket_client_new();
    GSocketConnectable* connectable = G_SOCKET_CONNECTABLE
        (test_socket_address_new_loopback(af, listen_port));
    guint fw_socket_id, socket_id = 0;
    gulong id[2];

    /* Start the listener that will accept the forwarded connection */
    g_assert(server_port);
    g_socket_listener_accept_async(listener, server_cancel,
        test_forward_accept, &test);

    /* Expect a notification when socket is being added */
    id[0] = fwd_peer_add_socket_added_handler(fp,
        test_forward_stream_remote_socket_added, &socket_id);
    g_assert(id[0]);

    /* Create the forwarding socket */
    GDEBUG("Creating the forwarder");
    fw_socket_id = fwd_peer_add_remote_stream_forwarder(fp, af, listen_port,
        sa_local, 1, FWD_FLAG_REUSE_ADDRESS, NULL);
    g_assert(fw_socket_id);
    g_assert_cmpint(fw_socket_id, == ,socket_id);

    /* Wait until it gets initialized */
    id[1] = fwd_peer_add_socket_state_handler(fp, fw_socket_id,
        test_forward_socket_state_changed, &test);
    test_run(&test_opt, test.forward.common.loop);
    fwd_peer_remove_handler(fp, id[1]);

    /* Delete it and create it again */
    GDEBUG("Deleting this forwarder");
    socket_id = 0;
    fwd_peer_remove_forwarder(fp, fw_socket_id);
    GDEBUG("Re-creating the forwarder");
    fw_socket_id = fwd_peer_add_remote_stream_forwarder(fp, af, listen_port,
        sa_local, 1, FWD_FLAG_REUSE_ADDRESS, NULL);
    g_assert(fw_socket_id);
    g_assert_cmpint(fw_socket_id, == ,socket_id);

    /* Wait until it gets initialized again */
    id[1] = fwd_peer_add_socket_state_handler(fp, fw_socket_id,
        test_forward_socket_state_changed, &test);
    test_run(&test_opt, test.forward.common.loop);
    fwd_peer_clear_all_handlers(fp, id);

    /* Close the temporary socket to leave the forwarding socket alone */
    g_assert(g_socket_close(tmp, NULL));
    g_object_unref(tmp);
    g_object_unref(sa_local);

    /* Wait for the connection socket to be created and terminated */
    test.connection_socket_id = 0;
    id[0] = fwd_peer_add_socket_added_handler(local,
        test_forward_stream_remote_connection_added, &test);
    id[1] = fwd_peer_add_socket_removed_handler(local, 0,
        test_forward_stream_remote_connection_removed, &test);

    /*
     * Sync with the remote side before sending the data. Otherwise the
     * connection may be attempted before the ACCEPT calls go through.
     */
    GDEBUG("Forwarding %hu => %hu", listen_port, server_port);
    g_assert(fwd_peer_sync(fp, TEST_TIMEOUT_MS, NULL));

    /* Now we can safely try to connect */
    GDEBUG("Connecting to %hu", listen_port);
    g_socket_client_connect_async(connector, connectable, NULL,
        test_forward_connected, &test);

    test_run(&test_opt, test.forward.common.loop);

    g_assert_cmpint(fp->state, == ,FWD_STATE_STARTED);
    fwd_peer_remove_forwarder(fp, fw_socket_id);
    fwd_peer_remove_forwarder(fp, fw_socket_id); /* noop */

    /* Removing the forwarer will close the socket on the other side */
    fwd_peer_remove_handler(local, id[1]);
    id[1] = fwd_peer_add_socket_removed_handler(local, 0,
        test_forward_stream_remote_socket_removed, &test);
    test_run(&test_opt, test.forward.common.loop);

    fwd_peer_clear_all_handlers(local, id);
    g_object_unref(connectable);
    g_object_unref(connector);
    g_cancellable_cancel(server_cancel);
    g_object_unref(server_cancel);
    g_socket_listener_close(listener);
    g_object_unref(listener);
    test_forward_cleanup(&test.forward);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    test_init(&test_opt, argc, argv);

    g_test_add_func(TEST_("basic"), test_basic);
    g_test_add_func(TEST_("stop"), test_stop);
    g_test_add_func(TEST_("echo"), test_echo);
    g_test_add_func(TEST_("info/ok"), test_info_ok);
    g_test_add_func(TEST_("socket/ok"), test_socket_ok);
    g_test_add_func(TEST_("socket/error/family"), test_socket_error_family);
    g_test_add_func(TEST_("socket/error/bind"), test_socket_error_bind);
    g_test_add_func(TEST_("accept/error"), test_accept_error);
    g_test_add_func(TEST_("connect/error"), test_connect_error);
    g_test_add_func(TEST_("data/error"), test_data_error);
    g_test_add_func(TEST_("forward/stream/local"), test_forward_stream_local);
    g_test_add_func(TEST_("forward/stream/remote"), test_forward_stream_remote);
    g_test_add_func(TEST_("forward/datagram/local/max_conn"),
        test_forward_datagram_local_max_conn);
    g_test_add_func(TEST_("forward/datagram/remote/max_conn"),
        test_forward_datagram_remote_max_conn);
    g_test_add_func(TEST_("forward/datagram/local/timeout"),
        test_forward_datagram_local_timeout);
    g_test_add_func(TEST_("forward/datagram/remote/timeout"),
        test_forward_datagram_remote_timeout);
    g_test_add_func(TEST_("forward/datagram/local/ok"),
        test_forward_datagram_local);
    g_test_add_func(TEST_("forward/datagram/remote/ok"),
        test_forward_datagram_remote);

    #define ADD_TEST_SET(test) do { guint i; \
        for (i = 0; i < G_N_ELEMENTS(test##_all); i++) { \
            g_test_add_data_func(test##_all[i].name, test##_all + i, test); \
        } } while (FALSE)

    ADD_TEST_SET(test_info_error);
    ADD_TEST_SET(test_socket_inval);
    ADD_TEST_SET(test_connect_inval);
    ADD_TEST_SET(test_accept_inval);
    ADD_TEST_SET(test_accepted_inval);
    ADD_TEST_SET(test_data_inval);
    ADD_TEST_SET(test_close_inval);
    return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
