#include <stdlib.h>
#include <vws/server.h>

int main(int argc, const char* argv[])
{
    // Read configuration from environment or defaults
    cstr host    = getenv("VWS_HOST") ? getenv("VWS_HOST") : "0.0.0.0";
    int  port    = getenv("VWS_PORT") ? atoi(getenv("VWS_PORT")) : 8181;
    int  threads = getenv("VWS_THREADS") ? atoi(getenv("VWS_THREADS")) : 10;

    // Create and configure server
    vrtql_msg_svr* server = vrtql_msg_svr_new(threads, 0, 0);
    server->process       = my_process_handler;

    // Run (blocks until server is stopped)
    vrtql_msg_svr_run(server, host, port);

    // Cleanup
    vrtql_msg_svr_free(server);
    vws_cleanup();

    return 0;
}
