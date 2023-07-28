#include <vrtql/server.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

// Server function to process messages. Runs in context of worker thread.
void process_message(vws_svr_cnx* cnx, vrtql_msg* m)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)cnx->server;

    vws.trace(VL_INFO, "process_message (%p) %p", cnx, m);

    // Echo back. Note: You should always set reply messages format to the
    // format of the connection.

    // Create reply message
    vrtql_msg* reply = vrtql_msg_new();
    reply->format    = cnx->format;

    // Copy content
    cstr data   = m->content->data;
    size_t size = m->content->size;
    vrtql_buffer_append(reply->content, data, size);

    // Send. We don't free message as send() does it for us.
    server->send(cnx, reply);

    // Clean up request
    vrtql_msg_free(m);
}

int main(int argc, const char* argv[])
{
    vrtql_msg_svr* server = vrtql_msg_svr_new(10, 0, 0);
    server->process       = process_message;

    vrtql_svr_run((vrtql_svr*)server, server_host, server_port);

    // Shutdown
    vrtql_svr_stop((vrtql_svr*)server);
    uv_thread_join(&server_tid);
    vrtql_msg_svr_free(server);
}

