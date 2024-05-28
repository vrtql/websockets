#include "server.h"
#include "message.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

cstr server_host = "127.0.0.1";
int  server_port = 10181;

// Server function to process HTTP messages. Runs in context of worker thread.
bool process(vws_svr* s, vws_cid_t cid, vws_http_msg* msg, void* ctx)
{
    vws_tcp_svr* server = (vws_tcp_svr*)s;

    if (vws.tracelevel >= VT_APPLICATION)
    {
        vws.trace(VL_INFO, "server: process (%ul) %p", cid, msg);
    }

    cstr response = "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "Hello world";

    // Allocate memory for the data to be sent in response
    char* data = (char*)vws.strdup(response);

    // Create response
    vws_svr_data* reply = vws_svr_data_own(server, cid, (ucstr)data, strlen(data));

    // Send reply. This will wakeup network thread.
    vws_tcp_svr_send(reply);

    return true;
}

void server_thread(void* arg)
{
    vws_svr* server      = (vws_svr*)arg;
    vws.tracelevel       = VT_THREAD;
    server->base.trace   = vws.tracelevel;
    server->process_http = process;

    vws_tcp_svr_run((vws_tcp_svr*)server, server_host, server_port);

    vws.trace(VL_INFO, "server_thread: exit");

    vws_cleanup();
}

void client_thread(void* arg)
{
    vws_socket* s = vws_socket_new();
    cstr content  = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";

restart:

    // Connect
    vws.trace(VL_INFO, "[CLIENT] connecting");
    while (vws_socket_connect(s, server_host, server_port, false) == false)
    {
        vws.trace(VL_ERROR, "[client]: faile to connect");
    }

    cstr payload = "payload";

    int i = 0;
    while (true)
    {
        if (i++ > 10)
        {
            break;
        }

        // Send
        while (vws_socket_write(s, (ucstr)content, strlen(content)) < 0)
        {
            if (vws_socket_is_connected(s) == false)
            {
                goto restart;
            }
        }

        // Receive
        while (vws_socket_read(s) <= 0)
        {
            vws.trace(VL_INFO, "[CLIENT] Waiting");
            if (vws_socket_is_connected(s) == false)
            {
                goto restart;
            }
        }

        //vws.trace(VL_INFO, "[CLIENT] Receive: %s", s->buffer->data);

        // Parse response
        vws_http_msg* reply = vws_http_msg_new(HTTP_RESPONSE);

        while (reply->done == false)
        {
            vws_http_msg_parse(reply, s->buffer->data, s->buffer->size);
        }

        ASSERT_TRUE(reply->done == true);
        ASSERT_TRUE(vws_http_msg_status_code(reply) == 200);

        vws_http_msg_free(reply);
    }

    // Disconnect
    vws_socket_free(s);

    vws_cleanup();

    vws.trace(VL_INFO, "[CLIENT] Done");
}

void client_test(int iterations, int nt)
{
    for (int i = 0; i < iterations; i++)
    {
        uv_thread_t* threads = vws.malloc(sizeof(uv_thread_t) * nt);

        for (int i = 0; i < nt; i++)
        {
            uv_thread_create(&threads[i], client_thread, NULL);
            vws.trace(VL_INFO, "client_test: started client thread %p", threads[i]);
        }

        for (int i = 0; i < nt; i++)
        {
            uv_thread_join(&threads[i]);
            vws.trace(VL_INFO, "client_test: stopped client thread %p", threads[i]);
        }

        free(threads);
    }
}

CTEST(test_http_server, http)
{
    vws_svr* server      = vws_svr_new(10, 0, 0);
    server->process_http = process;

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Wait for server to start up
    while (vws_tcp_svr_state((vws_tcp_svr*)server) != VS_RUNNING)
    {
        vws_msleep(100);
    }

    client_test(10, 10);

    // Shutdown

    // Need to give the server time to properly send out CLOSE frames, etc. If
    // we don't give it time, then it will it may fail to complete sending
    // CLOSE_FRAME ws_svr_process_frame() and look like a memory leak in
    // valgrind.
    sleep(1);

    // Shutdown server
    vws_tcp_svr_stop((vws_tcp_svr*)server);
    uv_thread_join(&server_tid);
    vws_svr_free(server);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
