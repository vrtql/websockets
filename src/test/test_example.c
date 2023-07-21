#include "message.h"

int main(int argc, const char* argv[])
{
    // Create connection object
    vws_cnx* cnx = vws_cnx_new();

    // Set connection timeout to 2 seconds (the default is 10). This applies
    // both to connect() and to read operations (i.e. poll()).
    vws_socket_set_timeout((vws_socket*)cnx, 2);

    // Connect. This will automatically use SSL if "wss" scheme is used.
    cstr uri = "ws://localhost:8181/websocket";
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Can check connection state this way. Should always be true here as we
    // just successfully connected.
    assert(vws_socket_is_connected((vws_socket*)cnx) == true);

    // Enable tracing. This will dump frames to the console in human-readable
    // format as they are sent and received.
    vrtql.tracelevel = VT_PROTOCOL;

    // Send a TEXT frame
    vws_frame_send_text(cnx, "Hello, world!");

    // Receive websocket message
    vws_msg* reply = vws_msg_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    // Send a BINARY message
    vws_msg_send_binary(cnx, (ucstr)"Hello, world!", 14);

    // Receive websocket message
    reply = vws_msg_recv(cnx);

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
