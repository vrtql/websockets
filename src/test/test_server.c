#include "server.h"
#include "socket.h"

#include "common.h"

cstr server_host = "127.0.0.1";
int  server_port = 8181;
cstr content     = "Lorem ipsum dolor sit amet";

void process_data(vrtql_svr_data* req)
{
    vrtql_svr* server = req->cnx->server;

    vrtql.trace(VL_INFO, "process_data (%p)", req);

    //> Prepare the response: echo the data back

    // Allocate memory for the data to be sent in response
    char* data = (char*)vrtql.malloc(req->size);

    // Copy the request's data to the response data
    strncpy(data, req->data, req->size);

    // Create response
    vrtql_svr_data* reply = vrtql_svr_data_own(req->cnx, data, req->size);

    // Free request
    vrtql_svr_data_free(req);

    if (vrtql.tracelevel >= VT_APPLICATION)
    {
        vrtql.trace( VL_INFO,
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

    vrtql.tracelevel = VT_THREAD;
    server->trace    = vrtql.tracelevel;

    vrtql_svr_run(server, server_host, server_port);
}

void client_thread(void* arg)
{
    // Connect
    vrtql.trace(VL_INFO, "[CLIENT] Connecting");
    vws_socket* s = vws_socket_new();
    ASSERT_TRUE(vws_socket_connect(s, server_host, server_port, false));
    vrtql.trace(VL_INFO, "[CLIENT] Connected");

    // Send request
    vrtql.trace(VL_INFO, "[CLIENT] Send: %s", content);
    vws_socket_write(s, content, strlen(content));

    // Get reply
    ssize_t n = vws_socket_read(s);
    ASSERT_TRUE(n > 0);
    vrtql.trace(VL_INFO, "[CLIENT] Receive: %s", s->buffer->data);

    // Disconnect and cleanup.
    vws_socket_free(s);
}

CTEST(test_server, echo)
{
    vrtql_svr* server  = vrtql_svr_new(10, 0, 0);
    vrtql.tracelevel   = VT_THREAD;
    server->on_data_in = process_data;

    vrtql.trace(VL_INFO, "[CLIENT] Starting server");

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Wait for server to start up
    while (server->state != VS_RUNNING)
    {
        vrtql_msleep(100);
    }

    int nc = 10;
    uv_thread_t* threads = vrtql.malloc(sizeof(uv_thread_t) * nc);

    for (int i = 0; i < nc; i++)
    {
        uv_thread_create(&threads[i], client_thread, NULL);
        vrtql.trace(VL_INFO, "started client thread %p", threads[i]);
    }

    for (int i = 0; i < nc; i++)
    {
        uv_thread_join(&threads[i]);
        vrtql.trace(VL_INFO, "stopped client thread %p", threads[i]);
    }

    free(threads);

    // Shutdown server
    vrtql.trace(VL_INFO, "[CLIENT] Stopping server");
    vrtql_svr_stop(server);
    uv_thread_join(&server_tid);
    vrtql_svr_free(server);

    vrtql.trace(VL_INFO, "[CLIENT] Done");
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
