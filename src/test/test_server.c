#include "server.h"
#include "socket.h"

#include "common.h"

cstr server_host = "127.0.0.1";
int  server_port = 8181;

void process_data(vrtql_svr_data* req)
{
    vrtql_svr* server = req->cnx->server;

    //> Prepare the response: echo the data back

    // Allocate memory for the data to be sent in response
    char* data = (char*)vrtql.malloc(req->size);

    // Copy the request's data to the response data
    strncpy(data, req->data, req->size);

    // Create response
    vrtql_svr_data* reply = vrtql_svr_data_own(req->cnx, data, req->size);

    // Free request
    vrtql_svr_data_free(req);

    if (server->trace)
    {
        vrtql_trace(VL_INFO, "process_data(): %p queing", reply->cnx);
    }

    // Send reply. This will wakeup network thread.
    vrtql_svr_send(server, reply);

    if (server->trace)
    {
        vrtql_trace(VL_INFO, "process_data(): %p done", reply->cnx);
    }
}

void server_thread(void* arg)
{
    vrtql_svr* server = (vrtql_svr*)arg;
    vrtql_svr_run(server, server_host, server_port);
}

CTEST(test_server, echo)
{
    vrtql_svr* server  = vrtql_svr_new(10, 0, 0);
    server->trace      = 1;
    server->on_data_in = process_data;

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Wait for server to start up
    while (server->state != VS_RUNNING)
    {
        vrtql_msleep(100);
    }

    cstr content = "Hello, Server!";

    vws_socket* s = vws_socket_new();
    ASSERT_TRUE(vws_socket_connect(s, server_host, server_port, false));
    s->trace = true;
    vws_socket_write(s, content, strlen(content));

    ssize_t n = vws_socket_read(s);
    ASSERT_TRUE(n > 0);
    printf("Received: %s\n", s->buffer->data);

    vws_socket_disconnect(s);
    vws_socket_free(s);

    vrtql_svr_stop(server);
    uv_thread_join(&server_tid);
    vrtql_svr_free(server);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
