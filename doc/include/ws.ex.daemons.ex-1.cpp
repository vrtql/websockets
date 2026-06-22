#include <stdlib.h>
#include <string.h>
#include <vws/server.h>
#include <vws/message.h>

// Per-thread worker context
typedef struct
{
    int request_count;
} worker_ctx;

// Worker thread constructor
void* worker_ctor(void* data)
{
    worker_ctx* ctx = (worker_ctx*)malloc(sizeof(worker_ctx));
    ctx->request_count = 0;
    return ctx;
}

// Worker thread destructor
void worker_dtor(void* data)
{
    worker_ctx* ctx = (worker_ctx*)data;
    free(ctx);
}

// Message processing function
void process(vws_svr* s, vws_cid_t cid, vrtql_msg* m, void* ctx)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)s;
    worker_ctx* wctx = (worker_ctx*)ctx;
    wctx->request_count++;

    // Create reply: echo content back with a count header
    vrtql_msg* reply = vrtql_msg_new();
    reply->format    = m->format;

    // Copy content
    if (m->content->size > 0)
    {
        vws_buffer_append(reply->content, m->content->data, m->content->size);
    }

    // Add request count header
    char count[32];
    snprintf(count, sizeof(count), "%d", wctx->request_count);
    vrtql_msg_set_header(reply, "request-count", count);

    // Send reply
    server->send(s, cid, reply, NULL);

    // Clean up request
    vrtql_msg_free(m);
}

int main(int argc, const char* argv[])
{
    // Read configuration from environment with defaults
    cstr host    = getenv("VWS_HOST")    ? getenv("VWS_HOST")              : "127.0.0.1";
    int  port    = getenv("VWS_PORT")    ? atoi(getenv("VWS_PORT"))        : 8181;
    int  threads = getenv("VWS_THREADS") ? atoi(getenv("VWS_THREADS"))     : 10;
    int  trace   = getenv("VWS_TRACE")   ? atoi(getenv("VWS_TRACE"))       : 0;

    // Set trace level
    vws.tracelevel = trace;

    // Create server
    vrtql_msg_svr* server = vrtql_msg_svr_new(threads, 0, 0);
    server->process       = process;

    // Set up worker thread context
    vws_tcp_svr* base     = (vws_tcp_svr*)server;
    base->worker_ctor     = worker_ctor;
    base->worker_dtor     = worker_dtor;

    // Run (blocks until server is stopped)
    vrtql_msg_svr_run(server, host, port);

    // Cleanup
    vrtql_msg_svr_free(server);
    vws_cleanup();

    return 0;
}
