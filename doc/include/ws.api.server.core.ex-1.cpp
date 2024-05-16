#include <vws/server.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

void process(vws_svr_data* req, void* ctx)
{
    vws.trace(VL_INFO, "process (%p)", req);

    vws_tcp_svr* server = req->server;

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

int main(int argc, const char* argv[])
{
    // Setup
    vrtql_svr* server  = vrtql_svr_new(10, 0, 0);
    vws.tracelevel     = VT_THREAD;
    server->on_data_in = process;

    // Run
    vrtql_svr_run(server, server_host, server_port);

    // Shutdown
    vrtql_svr_stop(server);
    uv_thread_join(&server_tid);
    vrtql_svr_free(server);
}


