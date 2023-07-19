#include <vrtql/websocket.h>

int main()
{
    // Create connection object
    vws_cnx* cnx = vws_cnx_new();

    // Set connection timeout to 2 seconds (the default is 10). This applies
    // both to connect() and to read operations (i.e. poll()).
    vws_cnx_set_timeout(cnx, 2);

    // Connect. This will automatically use SSL if "wss" scheme is used.
    cstr uri = "ws://localhost:8000/websocket";
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Can check connection state this way. Should always be true here as we
    // just successfully connected.
    assert(vws_cnx_is_connected(cnx) == true);

    // Enable tracing. This will dump frames to the console in human-readable
    // format as they are sent and received.
    vrtql.trace = VT_PROTOCOL;

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

