#include "server.h"
#include "message.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

cstr server_host = "127.0.0.1";
int  server_port = 8181;
cstr uri         = "ws://localhost:8181/websocket";
cstr content     = "Lorem ipsum dolor sit amet";

// Server function to process messages. Runs in context of worker thread.
void process(vws_svr* s, vws_cid_t cid, vrtql_msg* m, void* ctx)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)s;

    vws.trace(VL_INFO, "process (%lu) %p", cid, m);

    // Echo back. Note: You should always set reply messages format to the
    // format of the connection.

    // Create reply message
    vrtql_msg* reply = vrtql_msg_new();
    reply->format    = m->format;

    // Copy content
    ucstr data  = m->content->data;
    size_t size = m->content->size;
    vws_buffer_append(reply->content, data, size);

    // Send. We don't free message as send() does it for us.
    server->send(s, cid, reply, NULL);

    // Clean up request
    vrtql_msg_free(m);
}

void server_thread(void* arg)
{
    vws_tcp_svr* server = (vws_tcp_svr*)arg;
    vws.tracelevel      = VT_THREAD;
    server->trace       = vws.tracelevel;

    vws_tcp_svr_run(server, server_host, server_port);

    vws_cleanup();
}

void client_thread(void* arg)
{
    vws_cnx* cnx = vws_cnx_new();

    while (vws_connect(cnx, uri) == false)
    {
        vws.trace(VL_ERROR, "[client]: connecting %s", uri);
    }

    cstr payload = "payload";

    int i = 0;
    while (true)
    {
        if (i++ > 10)
        {
            break;
        }

        // Create
        vrtql_msg* request = vrtql_msg_new();
        vrtql_msg_set_content(request, payload);

        // Send
        while (vrtql_msg_send(cnx, request) < 0)
        {
            if (vws_socket_is_connected((vws_socket*)cnx) == false)
            {
                goto restart;
            }
        }

        // Receive
        vrtql_msg* reply = NULL;

        while (reply == NULL)
        {
            printf("vrtql_msg_recv(cnx)\n");
            reply = vrtql_msg_recv(cnx);
            if (vws_socket_is_connected((vws_socket*)cnx) == false)
            {
                goto restart;
            }
        }

        // Check
        ucstr content = reply->content->data;
        size_t size   = reply->content->size;
        ASSERT_TRUE(strncmp(payload, (cstr)content, size) == 0);
        vrtql_msg_free(reply);

restart:

        // Cleanup
        vrtql_msg_free(request);
    }

    // Disconnect
    vws_disconnect(cnx);
    vws_cnx_free(cnx);

    vws_cleanup();
}

void client_test(int iterations, int nt)
{
    for (int i = 0; i < iterations; i++)
    {
        uv_thread_t* threads = vws.malloc(sizeof(uv_thread_t) * nt);

        for (int i = 0; i < nt; i++)
        {
            uv_thread_create(&threads[i], client_thread, NULL);
            vws.trace(VL_INFO, "started client thread %p", threads[i]);
        }

        for (int i = 0; i < nt; i++)
        {
            uv_thread_join(&threads[i]);
            vws.trace(VL_INFO, "stopped client thread %p", threads[i]);
        }

        free(threads);
    }
}

CTEST(test_msg_server, echo)
{
    vrtql_msg_svr* server = vrtql_msg_svr_new(10, 0, 0);
    server->process       = process;

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Wait for server to start up
    while (vws_tcp_svr_state((vws_tcp_svr*)server) != VS_RUNNING)
    {
        vws_msleep(100);
    }

    client_test(5, 50);

    // Shutdown

    // Need to give the server time to properly send out CLOSE frames, etc. If
    // we don't give it time, then it will it may fail to complete sending
    // CLOSE_FRAME ws_svr_process_frame() and look like a memory leak in
    // valgrind.
    sleep(1);

    // Shutdown server
    vws_tcp_svr_stop((vws_tcp_svr*)server);
    uv_thread_join(&server_tid);
    vrtql_msg_svr_free(server);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
