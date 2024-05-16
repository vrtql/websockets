#include "server.h"
#include "socket.h"

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"

cstr server_host = "127.0.0.1";
int  server_port = 8181;
cstr content     = "Lorem ipsum dolor sit amet";

void process(vws_svr_data* req, void* ctx)
{
    vws_tcp_svr* server = req->server;

    vws.trace(VL_INFO, "process (%p)", req);

    //> Prepare the response: echo the data back

    // Allocate memory for the data to be sent in response
    char* data = (char*)vws.malloc(req->size);

    // Copy the request's data to the response data
    strncpy(data, req->data, req->size);

    // Create response
    vws_svr_data* reply;

    reply = vws_svr_data_own(req->server, req->cid, (ucstr)data, req->size);

    // Free request
    vws_svr_data_free(req);

    if (vws.tracelevel >= VT_APPLICATION)
    {
        vws.trace(VL_INFO, "process(%lu): %i bytes", reply->cid, reply->size);
    }

    // Send reply. This will wakeup network thread.
    vws_tcp_svr_send(reply);
}

void server_thread(void* arg)
{
    vws_tcp_svr* server = (vws_tcp_svr*)arg;
    vws.tracelevel      = VT_THREAD;
    server->trace       = vws.tracelevel;

    vws_tcp_svr_run(server, server_host, server_port);
}

void client_thread(void* arg)
{
    // Connect
    vws.trace(VL_INFO, "[CLIENT] Connecting");
    vws_socket* s = vws_socket_new();
    ASSERT_TRUE(vws_socket_connect(s, server_host, server_port, false));
    vws.trace(VL_INFO, "[CLIENT] Connected");

    // Send request
    vws.trace(VL_INFO, "[CLIENT] Send: %s", content);
    vws_socket_write(s, (ucstr)content, strlen(content));

    // Get reply
    ssize_t n = vws_socket_read(s);
    ASSERT_TRUE(n > 0);
    vws.trace(VL_INFO, "[CLIENT] Receive: %s", s->buffer->data);

    // Disconnect and cleanup.
    vws_socket_free(s);
}

CTEST(test_server, echo)
{
    vws_tcp_svr* server = vws_tcp_svr_new(10, 0, 0);
    vws.tracelevel      = VT_THREAD;
    server->on_data_in  = process;

    vws.trace(VL_INFO, "[CLIENT] Starting server");

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Wait for server to start up
    while (server->state != VS_RUNNING)
    {
        vws_msleep(100);
    }

    int nc = 10;
    uv_thread_t* threads = vws.malloc(sizeof(uv_thread_t) * nc);

    for (int i = 0; i < nc; i++)
    {
        uv_thread_create(&threads[i], client_thread, NULL);
        vws.trace(VL_INFO, "started client thread %p", threads[i]);
    }

    for (int i = 0; i < nc; i++)
    {
        uv_thread_join(&threads[i]);
        vws.trace(VL_INFO, "stopped client thread %p", threads[i]);
    }

    free(threads);

    // Shutdown server
    vws.trace(VL_INFO, "[CLIENT] Stopping server");
    vws_tcp_svr_stop(server);
    uv_thread_join(&server_tid);
    vws_tcp_svr_free(server);

    vws.trace(VL_INFO, "[CLIENT] Done");
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
