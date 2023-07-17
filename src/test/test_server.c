#include "server.h"
#include "socket.h"

#include "common.h"

cstr server_host = "127.0.0.1";
int  server_port = 8181;
cstr content     = "Lorem ipsum dolor sit amet";

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
        vrtql_trace( VL_INFO,
                     "process_data(%p): %i bytes",
                     reply->cnx,
                     reply->size);
    }

    // Send reply. This will wakeup network thread.
    vrtql_svr_send(server, reply);
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

    vrtql_trace(VL_INFO, "[CLIENT] Starting server");

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Wait for server to start up
    while (server->state != VS_RUNNING)
    {
        vrtql_msleep(100);
    }

    // Connect
    vrtql_trace(VL_INFO, "[CLIENT] Connecting");
    vws_socket* s = vws_socket_new();
    ASSERT_TRUE(vws_socket_connect(s, server_host, server_port, false));
    s->trace = true;
    vrtql_trace(VL_INFO, "[CLIENT] Connected");

    // Send request
    vrtql_trace(VL_INFO, "[CLIENT] Send: %s", content);
    vws_socket_write(s, content, strlen(content));

    // Get reply
    ssize_t n = vws_socket_read(s);
    ASSERT_TRUE(n > 0);
    vrtql_trace(VL_INFO, "[CLIENT] Receive: %s", s->buffer->data);

    // Disconnect and cleanup.
    vws_socket_free(s);

    // Shutdown server

    vrtql_trace(VL_INFO, "[CLIENT] Stopping server");

    vrtql_svr_stop(server);
    uv_thread_join(&server_tid);
    vrtql_svr_free(server);

    vrtql_trace(VL_INFO, "[CLIENT] Done");
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
