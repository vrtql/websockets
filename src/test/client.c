#include "util/mongoose.h"

static const char* s_url = "ws://localhost:8000/websocket";

// Print websocket response and signal that we're done
static void fn(struct mg_connection* c, int ev, void* ev_data, void* fn_data)
{
    if (ev == MG_EV_OPEN)
    {
        c->is_hexdumping = 1;
    }
    else if (ev == MG_EV_CONNECT)
    {
        // Connected to server. Extract host name from URL
        struct mg_str host = mg_url_host(s_url);

        // If s_url is wss://, tell client connection to use TLS
        if (mg_url_is_ssl(s_url))
        {
            struct mg_tls_opts opts = {.srvname = host};
            mg_tls_init(c, &opts);
        }
    }
    else if (ev == MG_EV_TLS_HS)
    {
        MG_INFO(("TLS handshake done!"));
    }
    else if (ev == MG_EV_ERROR)
    {
        // On error, log error message
        MG_ERROR(("%p %s", c->fd, (char*) ev_data));
    }
    else if (ev == MG_EV_WS_OPEN)
    {
        // When websocket handshake is successful, send message
        mg_ws_send(c, "hello", 5, WEBSOCKET_OP_TEXT);
    }
    else if (ev == MG_EV_WS_MSG)
    {
        // When we get echo response, print it
        struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;
        printf("GOT ECHO REPLY: [%.*s]\n", (int)wm->data.len, wm->data.ptr);
    }

    if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE || ev == MG_EV_WS_MSG)
    {
        // Signal that we're done
        *(bool*)fn_data = true;
    }
}

int main(void)
{
    struct mg_mgr mgr;        // Event manager
    bool done = false;        // Event handler flips it to true
    struct mg_connection* c;  // Client connection
    mg_mgr_init(&mgr);        // Initialise event manager
    mg_log_set(MG_LL_DEBUG);  // Set log level

    // Create client
    c = mg_ws_connect(&mgr, s_url, fn, &done, NULL);

    while (c && done == false)
    {
        // Wait for echo
        mg_mgr_poll(&mgr, 1000);
    }

    // Deallocate resources
    mg_mgr_free(&mgr);

    return 0;
}
