// In-process WebSocket via uv_socketpair() + vws_pipe_connect().
//
// Demonstrates how an arbitrary connected stream fd can be injected into a
// running vws server. The server side adopts the fd as a uv_pipe_t in its
// libuv loop; the client side wraps the other end as a fully-handshaken
// vws_cnx.

#include "server.h"
#include "message.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

// Echoes any message back to the sender. Runs on a worker thread.
static void process(vws_svr* s, vws_cid_t cid, vws_msg* m, void* ctx)
{
    vws_msg* reply = vws_msg_new();
    reply->opcode  = m->opcode;
    vws_buffer_append(reply->data, m->data->data, m->data->size);

    s->send(s, cid, reply, NULL);
    vws_msg_free(m);
}

static void server_thread(void* arg)
{
    vws_tcp_svr* server = (vws_tcp_svr*)arg;
    vws.tracelevel      = VT_THREAD;
    server->trace       = vws.tracelevel;

    // Bind to an ephemeral port. We won't connect over TCP, but
    // vws_tcp_svr_run() requires a host/port to bind. The pipe path
    // bypasses listen/accept entirely.
    vws_tcp_svr_run(server, "127.0.0.1", 0);

    vws_cleanup();
}

CTEST(test_pipe_server, echo)
{
    vws_svr* server    = vws_svr_new(4, 0, 0);
    server->process_ws = process;

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    while (vws_tcp_svr_state((vws_tcp_svr*)server) != VS_RUNNING)
    {
        vws_msleep(10);
    }

    // Open an in-process connection. No TCP, no DNS, no listen socket --
    // just a connected socket pair handed to the server's loop.
    vws_cnx* cnx = vws_pipe_connect((vws_tcp_svr*)server);
    ASSERT_NOT_NULL(cnx);
    ASSERT_TRUE(vws_cnx_is_connected(cnx));

    cstr payload = "hello inproc";
    for (int i = 0; i < 5; i++)
    {
        ASSERT_TRUE(vws_msg_send_text(cnx, payload) > 0);

        vws_msg* reply = vws_msg_recv(cnx);
        ASSERT_NOT_NULL(reply);
        ASSERT_TRUE(strncmp( payload,
                             (cstr)reply->data->data,
                             reply->data->size ) == 0);
        vws_msg_free(reply);
    }

    vws_disconnect(cnx);
    vws_cnx_free(cnx);

    // Give the server time to finish flushing CLOSE frames.
    sleep(1);

    vws_tcp_svr_stop((vws_tcp_svr*)server);
    uv_thread_join(&server_tid);
    vws_svr_free(server);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
