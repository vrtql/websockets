#include <vws/server.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

// Server function to process messages. Runs in context of worker thread.
void process(vws_svr* s, vws_cid_t cid, vws_msg* m, void* ctx)
{
    vws.trace(VL_INFO, "process_message (%ul) %p", cid, m);

    // Echo back. Note: You should always set reply messages format to the
    // format of the connection.

    // Create reply message
    vws_msg* reply = vws_msg_new();

    // Use same format
    reply->opcode  = m->opcode;

    // Copy content
    vws_buffer_append(reply->data, m->data->data, m->data->size);

    // Send. We don't free message as send() does it for us.
    s->send(s, cid, reply, NULL);

    // Clean up request
    vws_msg_free(m);
}

int main(int argc, const char* argv[])
{
    // Setup
    vws_svr* server = vws_svr_new(10, 0, 0);
    server->process = process;

    // Run
    vrtql_tcp_svr_run((vrtql_svr*)server, server_host, server_port);

    // Shutdown
    vrtql_svr_stop((vrtql_svr*)server);
    uv_thread_join(&server_tid);
    vws_svr_free(server);
    vws_cleanup();
}
