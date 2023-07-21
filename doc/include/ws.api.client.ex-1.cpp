#include <vrtql/websocket.h>

int main()
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

    //
    // { Do stuff: send and receive messages/frames }
    //

    // Disconnect
    vws_disconnect(cnx);

    // Free the connection
    vws_cnx_free(cnx);

    return 0;
}

