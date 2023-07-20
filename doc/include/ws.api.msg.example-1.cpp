#include <vrtql/message.h>

int main()
{
    vws_cnx* cnx = vws_cnx_new();

    cstr uri = "ws://localhost:8181/websocket";
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Create a message
    vrtql_msg* request = vrtql_msg_new();

    vrtql_msg_set_routing(request, "key", "value");
    vrtql_msg_set_header(request, "key", "value");
    vrtql_msg_set_content(request, "payload");

    // Send
    if (vrtql_msg_send(cnx, request) < 0)
    {
        printf("Failed to send: %s\n", vrtql.e.text);
        vrtql_msg_free(request);
        vws_cnx_free(cnx);
        return 1;
    }

    // Receive
    vrtql_msg* reply = vrtql_msg_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vrtql_msg_free(reply);
    }

    // Cleanup
    vrtql_msg_free(request);

    vws_disconnect(cnx);
    vws_cnx_free(cnx);

    return 0;
}
