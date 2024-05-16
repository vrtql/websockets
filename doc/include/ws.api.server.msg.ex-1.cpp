#include <vws/server.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

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

int main(int argc, const char* argv[])
{
    // Setup
    vrtql_msg_svr* server = vrtql_msg_svr_new(10, 0, 0);
    server->process       = process;

    // Run
    vrtql_svr_run((vrtql_svr*)server, server_host, server_port);

    // Shutdown
    vrtql_svr_stop((vrtql_svr*)server);
    uv_thread_join(&server_tid);
    vrtql_msg_svr_free(server);
    vws_cleanup();
}

