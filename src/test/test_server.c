#include <uv.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <stdlib.h>
#endif

#include "server.h"

cstr server_host = "127.0.0.1";
int  server_port = 8181;
#define TEST_DATA "Hello, Server!"

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    buf->base = (char*) malloc(suggested_size);
    buf->len  = suggested_size;
}

static void on_client_write(uv_write_t *req, int status)
{
    if (status < 0)
    {
        fprintf(stderr, "uv_write error: %s\n", uv_strerror(status));
        return;
    }
    printf("Message sent!\n");
    free(req);
}

static void on_client_close(uv_handle_t* handle)
{
    printf("Closed connection.\n");
}

static void on_client_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    if (nread < 0)
    {
        if (nread == UV_EOF)
        {
            // The other side closed the connection. Let's do the same.
            uv_close((uv_handle_t*) stream, NULL);
        }
    }
    else if (nread > 0)
    {
        printf("Received data: %.*s\n", (int)nread, buf->base);

        // Close the connection after receiving the data
        uv_close((uv_handle_t*) stream, on_client_close);
    }

    // OK to free buffer as write_data copies it.
    if (buf->base)
    {
        free(buf->base);
    }
}

static void on_client_connect(uv_connect_t *req, int status)
{
    if (status < 0) {
        fprintf(stderr, "Connection error: %s\n", uv_strerror(status));
        return;
    }

    printf("Connected to the server!\n");

    // Now that we're connected, let's send some test data.
    uv_buf_t buffer = uv_buf_init(TEST_DATA, strlen(TEST_DATA));
    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    uv_write(write_req, req->handle, &buffer, 1, on_client_write);
    free(req);

    // Start reading from the stream
    uv_read_start(req->handle, alloc_buffer, on_client_read);
}

void server_thread(void* arg)
{
    vrtql_svr* server = (vrtql_svr*)arg;
    vrtql_svr_run(server, server_host, server_port);
}

int main()
{
    vrtql_svr* server = vrtql_svr_new(10, 0, 0);

    server->trace = 1;

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    // Give the server some time to start up.
    sleep(2);

    uv_loop_t* loop = uv_default_loop();

    uv_tcp_t client;
    uv_tcp_init(loop, &client);

    struct sockaddr_in dest;
    uv_ip4_addr(server_host, server_port, &dest);

    uv_connect_t* connect_req = (uv_connect_t*)malloc(sizeof(uv_connect_t));
    uv_tcp_connect( connect_req,
                    &client,
                    (const struct sockaddr*)&dest,
                    on_client_connect );

    uv_run(loop, UV_RUN_DEFAULT);

    // Make sure to close the client loop
    while(uv_loop_close(loop) != UV_EBUSY)
    {

    }

    vrtql_svr_stop(server);

    uv_thread_join(&server_tid);

    vrtql_svr_free(server);

    return 0;
}
