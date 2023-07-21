#include <vrtql/server.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

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

int main(int argc, const char* argv[])
{
    vrtql_svr* server  = vrtql_svr_new(10, 0, 0);
    vrtql.tracelevel   = VT_THREAD;
    server->on_data_in = process_data;
    vrtql_svr_run(server, server_host, server_port);

    // Shutdown
    vrtql_svr_stop(server);
    uv_thread_join(&server_tid);
    vrtql_svr_free(server);
}


