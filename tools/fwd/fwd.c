/*
 * Copyright (C) 2024-2025 Slava Monich <slava@monich.com>
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

#include <libfwd.h>
#include <giorpc.h>

#include <gutil_idlepool.h>
#include <gutil_log.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include <arpa/inet.h>

#define DEFAULT_PORT 2024

typedef enum retval {
    RET_OK,
    RET_ERR,
    RET_CMDLINE
} RETVAL;

typedef struct fwd_opt FwdOpt;

typedef struct fwd_opts_common {
    int udp_timeout_sec;
} FwdOptsCommon;

typedef struct fwd_opts {
    GSList* list;
    FwdOptsCommon common;
    gboolean stdio;
} FwdOpts;

typedef
guint
(*FwdForwardFunc)(
    FwdPeer* fp,
    const FwdOptsCommon* common,
    const FwdOpt* opt,
    GError** error);

struct fwd_opt {
    FwdForwardFunc fn;
    GSocketFamily af;
    gushort port;
    GInetSocketAddress* to;
    int backlog;
    FWD_FLAGS flags;
};

typedef struct fwd_run {
    const FwdOpts* opts;
    GMainLoop* loop;
    GCancellable* cancel;
    GError* error;
    RETVAL ret;
} FwdRun;

static
GIOStream*
fwd_stdio_stream_new()
{
    GInputStream* in = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    GOutputStream* out = g_unix_output_stream_new(STDOUT_FILENO, FALSE);
    GIOStream* io = g_simple_io_stream_new(in, out);

    g_object_unref(in);
    g_object_unref(out);
    return io;
}

static
const char*
fwd_format_sa_native(
    const struct sockaddr* sa,
    gsize sa_len)
{
    if (sa && sa_len > sizeof(sa_family_t)) {
        const int af = sa->sa_family;
        const void* addr = NULL;
        const char* prefix = "";
        const char* suffix = "";
        gushort port = 0;

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

            return gutil_idle_pool_add(NULL, str, g_free);
        }
    }
    return NULL;
}

static
const char*
fwd_format_sa(
    GSocketAddress* sa)
{
    const char* str = NULL;

    if (sa) {
        const gsize sa_len = g_socket_address_get_native_size(sa);
        struct sockaddr* sa_buf = g_malloc(sa_len);

        if (g_socket_address_to_native(sa, sa_buf, sa_len, NULL)) {
            str = fwd_format_sa_native(sa_buf, sa_len);
        }
        g_free(sa_buf);
    }
    return str;
}

static
GInetSocketAddress*
fwd_address_parse(
    const char* str,
    gushort default_port,
    GSocketFamily prefer_af,
    GError** error)
{
    GSocketConnectable* sc = g_network_address_parse(str, default_port, error);
    GInetSocketAddress* fallback_isa = NULL;

    if (sc) {
        GSocketAddressEnumerator* sae = g_socket_connectable_enumerate(sc);
        GSocketAddress* sa = g_socket_address_enumerator_next(sae, NULL, error);

        g_object_unref(sc);
        while (sa) {
            if (G_IS_INET_SOCKET_ADDRESS(sa)) {
                GInetSocketAddress* isa = G_INET_SOCKET_ADDRESS(sa);
                GInetAddress* ia = g_inet_socket_address_get_address(isa);
                GSocketFamily af = g_inet_address_get_family(ia);

                if (af == prefer_af) {
                    /* This is exactly what we were looking for */
                    gutil_object_unref(fallback_isa);
                    g_object_unref(sae);
                    return isa;
                } else if (!fallback_isa) {
                    /* Kind of what we're looking for, but not quite */
                    fallback_isa = isa;
                    sa = NULL;
                }
            }
            /* Check the next one */
            gutil_object_unref(sa);
            sa = g_socket_address_enumerator_next(sae, NULL, error);
        }
        gutil_object_unref(sae);
    }
    return fallback_isa;
}

static
gboolean
fwd_run_apply_opts(
    FwdRun* run,
    FwdPeer* fp)
{
    const FwdOpts* opts = run->opts;
    const GSList* l;

    for (l = opts->list; l; l = l->next) {
        FwdOpt* opt = l->data;

        if (!opt->fn(fp, &opts->common, opt, &run->error)) {
            return FALSE;
        }
    }

    return TRUE;
}

static
void
fwd_run_state_handler(
    FwdPeer* fp,
    void* user_data)
{
    FwdRun* run = user_data;
    const FwdOpts* opts = run->opts;

    GDEBUG("Client state %d", fp->state);
    if (opts->list || opts->stdio) {
        if (fp->state == FWD_STATE_STOPPED) {
            run->ret = RET_ERR;
            fwd_peer_unref(fp);
            g_main_loop_quit(run->loop);
        }
    } else {
        /* No options, it's a connection check */
        run->ret = (fp->state == FWD_STATE_STARTED) ? RET_OK : RET_ERR;
        g_main_loop_quit(run->loop);
        fwd_peer_unref(fp);
    }
}

static
void
fwd_client_connect_done(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    FwdRun* run = user_data;
    GSocketClient* connector = G_SOCKET_CLIENT(object);
    GSocketConnection* connection = g_socket_client_connect_finish(connector,
        result, &run->error);

    if (connection) {
        FwdPeer* fp;

        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("Connected to %s",
                fwd_format_sa(gutil_idle_pool_add_object(NULL,
                g_socket_connection_get_remote_address(connection, NULL))));
        }

        fp = fwd_peer_new(G_IO_STREAM(connection), NULL);
        g_object_unref(connection);

        if (fwd_run_apply_opts(run, fp)) {
            /* State change handler will unref FwdPeer */
            fwd_peer_add_state_handler(fp, fwd_run_state_handler, run);
            return;
        }
        fwd_peer_unref(fp);
    }
    g_main_loop_quit(run->loop);
}

static
void
fwd_client_run(
    FwdRun* run,
    GSocketConnectable* connectable)
{
    GSocketClient* connector = g_socket_client_new();

    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
         GDEBUG("Connecting to %s", (char*) gutil_idle_pool_add(NULL,
             g_socket_connectable_to_string(connectable), g_free));
    }

    g_socket_client_connect_async(connector, connectable, run->cancel,
        fwd_client_connect_done, run);
    g_main_loop_run(run->loop);
    g_object_unref(connector);
}

static
void
fwd_server_state_handler(
    FwdPeer* fp,
    void* user_data)
{
    GDEBUG("Server state %d", fp->state);
    if (fp->state == FWD_STATE_STOPPED) {
        fwd_peer_unref(fp);
    }
}

static
void
fwd_server_accept_done(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    FwdRun* run = user_data;
    GSocketListener* listener = G_SOCKET_LISTENER(object);
    GSocketConnection* connection = g_socket_listener_accept_finish(listener,
        result, NULL, &run->error);

    if (connection) {
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GDEBUG("Accepted connection from %s",
                fwd_format_sa(gutil_idle_pool_add_object(NULL,
                g_socket_connection_get_remote_address(connection, NULL))));
        }

        /* State change handler will unref FwdPeer */
        fwd_peer_add_state_handler(fwd_peer_new(G_IO_STREAM(connection), NULL),
            fwd_server_state_handler, run);
        g_object_unref(connection);

        /* Accept the next connection */
        g_socket_listener_accept_async(listener, run->cancel,
            fwd_server_accept_done, run);
    }
}

static
void
fwd_server_run(
    FwdRun* run,
    gushort port)
{
    GSocketListener* listener = g_socket_listener_new();

    if (g_socket_listener_add_inet_port(listener, port, NULL, &run->error)) {
        GDEBUG("Listening on port %hu", port);
        g_socket_listener_accept_async(listener, run->cancel,
            fwd_server_accept_done, run);
        g_main_loop_run(run->loop);
    }
    g_object_unref(listener);
}

static
guint
fwd_add_local_tcp_forwarder(
    FwdPeer* fp,
    const FwdOptsCommon* common,
    const FwdOpt* opt,
    GError** error)
{
    return fwd_peer_add_local_stream_forwarder(fp, opt->af, opt->port,
        opt->to, opt->backlog, opt->flags, error);
}

static
guint
fwd_add_remote_tcp_forwarder(
    FwdPeer* fp,
    const FwdOptsCommon* common,
    const FwdOpt* opt,
    GError** error)
{
    return fwd_peer_add_remote_stream_forwarder(fp, opt->af, opt->port,
        opt->to, opt->backlog, opt->flags, error);
}

static
guint
fwd_add_local_udp_forwarder(
    FwdPeer* fp,
    const FwdOptsCommon* common,
    const FwdOpt* opt,
    GError** error)
{
    return fwd_peer_add_local_datagram_forwarder(fp, opt->af, opt->port,
        opt->to, common->udp_timeout_sec * 1000, FWD_LIMIT_DEFAULT,
        opt->flags, error);
}

static
guint
fwd_add_remote_udp_forwarder(
    FwdPeer* fp,
    const FwdOptsCommon* common,
    const FwdOpt* opt,
    GError** error)
{
    return fwd_peer_add_remote_datagram_forwarder(fp, opt->af, opt->port,
        opt->to, opt->backlog, common->udp_timeout_sec * 1000,
        FWD_LIMIT_DEFAULT, opt->flags, error);
}

static
void
fwd_opt_free(
    FwdOpt* opt)
{
    gutil_object_unref(opt->to);
    g_free(opt);
}

static
void
fwd_opts_init(
    FwdOpts* opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->common.udp_timeout_sec = FWD_TIMEOUT_DEFAULT / 1000;
}

static
void
fwd_opts_destroy(
    FwdOpts* opts)
{
    g_slist_free_full(opts->list, (GDestroyNotify) fwd_opt_free);
    opts->list = NULL;
}

static
gboolean
fwd_signal(
    gpointer user_data)
{
    FwdRun* run = user_data;

    if (!g_cancellable_is_cancelled(run->cancel)) {
        GINFO("Caught signal, shutting down...");
        g_cancellable_cancel(run->cancel);
        g_main_loop_quit(run->loop);
    }
    return G_SOURCE_CONTINUE;
}

static
gboolean
fwd_parse_port(
    const char* value,
    gushort* port,
    GError** error)
{
    guint n;

    if (gutil_parse_uint(value, 0, &n) && n > 0 && n <= 0xffff) {
        *port = (gushort) n;
        return TRUE;
    } else {
        g_propagate_error(error, g_error_new(G_OPTION_ERROR,
            G_OPTION_ERROR_BAD_VALUE, "Invalid port '%s'", value));
        return FALSE;
    }
}

static
FwdOpt*
fwd_opt_parse(
    const gchar* spec,
    GError** error)
{
    /* port:host[:hostport] */
    gchar** tokens = g_strsplit(spec, ":", 2);
    const int n = g_strv_length(tokens);
    gushort port;

    if (n == 2 && fwd_parse_port(tokens[0], &port, error)) {
        GInetSocketAddress* isa = fwd_address_parse(tokens[1], port,
            G_SOCKET_FAMILY_IPV4, error);

        if (isa) {
            GInetAddress* ia = g_inet_socket_address_get_address(isa);
            FwdOpt* opt = g_new0(FwdOpt, 1);

            opt->backlog = FWD_BACKLOG_DEFAULT;
            opt->af = g_inet_address_get_family(ia);
            opt->to = isa;
            opt->port = port;
            g_strfreev(tokens);
            return opt;
        }
    } else if (!error || !*error) {
        g_propagate_error(error, g_error_new(G_OPTION_ERROR,
            G_OPTION_ERROR_BAD_VALUE, "Invalid forward spec '%s'", spec));
    }
    g_strfreev(tokens);
    return NULL;
}

static
gboolean
fwd_opt_tcp_local(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    FwdOpt* opt = fwd_opt_parse(value, error);

    if (opt) {
        FwdOpts* opts = data;

        opt->fn = fwd_add_local_tcp_forwarder;
        opts->list = g_slist_append(opts->list, opt);
        GDEBUG("Local forward TCP rule %hu => %s", opt->port,
            fwd_format_sa(G_SOCKET_ADDRESS(opt->to)));
        return TRUE;
    } else {
        return FALSE;
    }
}

static
gboolean
fwd_opt_udp_local(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    FwdOpt* opt = fwd_opt_parse(value, error);

    if (opt) {
        FwdOpts* opts = data;

        opt->fn = fwd_add_local_udp_forwarder;
        opts->list = g_slist_append(opts->list, opt);
        GDEBUG("Local forward UDP rule %hu => %s", opt->port,
            fwd_format_sa(G_SOCKET_ADDRESS(opt->to)));
        return TRUE;
    } else {
        return FALSE;
    }
}

static
gboolean
fwd_opt_tcp_remote(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    FwdOpt* opt = fwd_opt_parse(value, error);

    if (opt) {
        FwdOpts* opts = data;

        opt->fn = fwd_add_remote_tcp_forwarder;
        opts->list = g_slist_append(opts->list, opt);
        GDEBUG("Remote forward TCP rule %hu => %s", opt->port,
            fwd_format_sa(G_SOCKET_ADDRESS(opt->to)));
        return TRUE;
    } else {
        return FALSE;
    }
}

static
gboolean
fwd_opt_udp_remote(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    FwdOpt* opt = fwd_opt_parse(value, error);

    if (opt) {
        FwdOpts* opts = data;

        opt->fn = fwd_add_remote_udp_forwarder;
        opts->list = g_slist_append(opts->list, opt);
        GDEBUG("Remote forward UDP rule %hu => %s", opt->port,
            fwd_format_sa(G_SOCKET_ADDRESS(opt->to)));
        return TRUE;
    } else {
        return FALSE;
    }
}

static
gboolean
fwd_opt_debug(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    gutil_log_default.level = (gutil_log_default.level < GLOG_LEVEL_DEBUG) ?
        GLOG_LEVEL_DEBUG : GLOG_LEVEL_VERBOSE;
    return TRUE;
}

int
main(
    int argc,
    char* argv[])
{
    RETVAL ret = RET_CMDLINE;
    FwdOpts opts;
    GError* error = NULL;
    GOptionGroup* group;
    GOptionContext* context;
    const GOptionEntry entries[] = {
        { "verbose", 'v',
          G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, fwd_opt_debug,
          "Enable verbose output", NULL },
        { "tcp-local", 'L',
          G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, fwd_opt_tcp_local,
          "Listen locally and forward TCP connections to the remote side",
          "SPEC" },
         { "udp-local", 'l',
           G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, fwd_opt_udp_local,
          "Listen locally and forward UDP packets to the remote side",
          "SPEC" },
        { "tcp-remote", 'R',
          G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, fwd_opt_tcp_remote,
          "Listen remotely and forward TCP connections to the local side",
          "SPEC" },
        { "udp-remote", 'r',
          G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, fwd_opt_udp_remote,
          "Listen remotely and forward UDP packets to the local side",
          "SPEC" },
        { "inactivity", 'i',
          G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &opts.common.udp_timeout_sec,
          "Inactivity timeout for UDP sockets", "SEC" },
        { NULL }
    };

    /*
     * g_type_init has been deprecated since version 2.36
     * The type system is initialized automagically since then
     */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    g_type_init();
    G_GNUC_END_IGNORE_DEPRECATIONS;

    /* Set up logging */
    gutil_log_func = gutil_log_stderr;
    gutil_log_default.name = "fwd";

    fwd_opts_init(&opts);
    context = g_option_context_new("[ADDR][:PORT]");
    group = g_option_group_new("", "", "", &opts, NULL);
    g_option_group_add_entries(group, entries);
    g_option_context_set_main_group(context, group);
    g_option_context_set_summary(context,
        "Listens locally or remotely and forwards incoming connections "
        "to the other side at ADDR.\n\n"
        "If no ADDR is provided, acts as a server.");
    g_option_context_set_description(context,
        "The forwarding SPEC format is 'port:host[:hostport]'.\n"
        "\n"
        "If no forwarding option is provided to the client, the client "
        "attempts to connect to the server at ADDR and immediately exits, "
        "either with zero status if the connection succeeds or with a "
        "non-zero status if the connection fails.\n");

    if (g_option_context_parse(context, &argc, &argv, &error) && argc <= 2) {
        char* addr = (argc > 1) ? argv[1] : NULL;
        guint sigterm, sigint;
        FwdRun run;

        memset(&run, 0, sizeof(run));
        run.opts = &opts;
        run.loop = g_main_loop_new(NULL, FALSE);
        run.cancel = g_cancellable_new();

        sigterm = g_unix_signal_add(SIGTERM, fwd_signal, &run);
        sigint = g_unix_signal_add(SIGINT, fwd_signal, &run);

        if (addr) {
            g_strchomp(addr);
            if (!strcmp(addr, "-")) {
                GIOStream* io = fwd_stdio_stream_new();
                FwdPeer* fp;

                gutil_log_set_type(GLOG_TYPE_STDERR, NULL);
                GDEBUG("Using stdio");
                opts.stdio = TRUE;
                fp = fwd_peer_new(io, NULL);
                g_object_unref(io);

                if (fwd_run_apply_opts(&run, fp)) {
                    /* State change handler will unref FwdPeer */
                    fwd_peer_add_state_handler(fp, fwd_run_state_handler, &run);
                    g_main_loop_run(run.loop);
                } else {
                    fwd_peer_unref(fp);
                }
            } else if (addr[0] == ':') {
                gushort port = DEFAULT_PORT;

                if (!addr[1] ||
                    fwd_parse_port(addr + 1, &port, &run.error)) {
                    fwd_server_run(&run, port);
                }
            } else {
                GSocketConnectable* connectable =
                    g_network_address_parse(addr, DEFAULT_PORT, &run.error);

                if (connectable) {
                    fwd_client_run(&run, connectable);
                    g_object_unref(connectable);
                }
            }
        } else {
            fwd_server_run(&run, DEFAULT_PORT);
        }

        if (run.error) {
            GINFO("%s", GERRMSG(run.error));
            g_error_free(run.error);
            ret = RET_ERR;
        } else if (g_cancellable_is_cancelled(run.cancel)) {
            GINFO("Terminated");
            ret = RET_ERR;
        } else {
            ret = run.ret;
        }

        g_source_remove(sigterm);
        g_source_remove(sigint);
        g_main_loop_unref(run.loop);
        g_object_unref(run.cancel);
    } else {
        if (error) {
            GINFO("%s", GERRMSG(error));
            g_error_free(error);
        } else {
            char* help = g_option_context_get_help(context, TRUE, NULL);

            fprintf(stderr, "%s", help);
            g_free(help);
        }
        ret = RET_CMDLINE;
    }
    gutil_idle_pool_drain(NULL);
    fwd_opts_destroy(&opts);
    g_option_context_free(context);
    return ret;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
