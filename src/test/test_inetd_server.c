#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "server.h"
#include "socket.h"

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"

cstr server_host = "127.0.0.1";
int  server_port = 8181;
cstr content     = "Lorem ipsum dolor sit amet";

void process_data(vws_svr_data* req, void* thread_ctx)
{
    vws_tcp_svr* server = req->server;

    vws.trace(VL_INFO, "process_data (%p)", req);

    //> Prepare the response: echo the data back

    // Allocate memory for the data to be sent in response
    char* data = (char*)vws.malloc(req->size);

    // Copy the request's data to the response data
    strncpy(data, req->data, req->size);

    // Create response
    vws_svr_data* reply = vws_svr_data_own( req->server,
                                            req->cid,
                                            (ucstr)data,
                                            req->size );

    // Free request
    vws_svr_data_free(req);

    if (vws.tracelevel >= VT_APPLICATION)
    {
        vws.trace( VL_INFO,
                   "process_data(%lu): %i bytes",
                   reply->cid,
                   reply->size);
    }

    // Send reply. This will wakeup network thread.
    vws_tcp_svr_send(reply);
}

void server_thread(void* arg)
{
    // Create a listening socket
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    ASSERT_TRUE(listenfd >= 0);

    // setsockopt: Handy debugging trick that lets us rerun the server
    // immediately after we kill it; otherwise we have to wait about 20 secs.
    // Eliminates "ERROR on binding: Address already in use" error.
    int optval = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR,
                (const void*)&optval, sizeof(int) );

    struct sockaddr_in serveraddr;
    memset((char*)&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)server_port);

    struct sockaddr* bindaddr = (struct sockaddr*)&serveraddr;
    ASSERT_TRUE(bind(listenfd, bindaddr, sizeof(serveraddr)) >= 0);

    vws_tcp_svr* server = (vws_tcp_svr*)arg;
    server->state       = VS_RUNNING;
    server->on_data_in  = process_data;
    vws.tracelevel      = VT_ALL;
    server->trace       = vws.tracelevel;

    ASSERT_TRUE(listen(listenfd, 10) >= 0);

    struct sockaddr_in clientaddr;
    socklen_t clientlen = sizeof(clientaddr);
    int connfd = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
    ASSERT_TRUE(connfd >= 0);

    vws_tcp_svr_inetd_run(server, connfd);
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
    server->on_data_in  = process_data;

    vws.trace(VL_INFO, "[CLIENT] Starting server");

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Wait for server to start up
    while (server->state != VS_RUNNING)
    {
        vws_msleep(100);
    }

    int nc = 1;
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

    // Server auto-shutdown on client disconnect. Wait for thread.
    uv_thread_join(&server_tid);

    // Cleanup
    vws_tcp_svr_free(server);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
