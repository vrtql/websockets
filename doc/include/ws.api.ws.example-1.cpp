#include <vrtql/websocket.h>

int main()
{
    cstr uri = "ws://localhost:8000/websocket";

    // vws_connect() will detect "wss" scheme and automatically use SSL
    vws_cnx* cnx = vws_cnx_new();

    // Check if the connection was successful
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Check connection state. This should always be true here.
    assert(vws_cnx_is_connected(cnx) == true);

    // Set timeout to 60 seconds (default is 10)
    vws_cnx_set_timeout(cnx, 60);

    // Enable tracing. This will dump frames to the console in human-readable
    // format as they are sent and received.
    cnx->trace = true;

    // Send a text message
    vws_send_text(cnx, "Hello, world!");

    // Receive websocket message
    vws_msg* reply = vws_recv_msg(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    // Send a binary message
    vws_send_binary(cnx, "Hello, world!", 14);

    // Receive websocket message
    reply = vws_recv_msg(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    vws_disconnect(cnx);

    return 0;
}

