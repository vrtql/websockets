#include <vrtql/message.h>

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

    // Create
    vrtql_msg* m1 = vrtql_msg_new();

    vrtql_msg_set_routing(m1, "key", "value");
    vrtql_msg_set_header(m1, "key", "value");
    vrtql_msg_set_content(m1, "payload");

    // Serialize and send
    vrtql_buffer* binary = vrtql_msg_serialize(m1, VM_MPACK_FORMAT);
    vws_send_binary(cnx, binary->data, binary->size);
    vrtql_buffer_free(binary);

    // Receive websocket message
    vws_msg* reply = vws_recv_msg(cnx);

    // Deserialize to VRTQL message
    vrtql_msg* m2 = vrtql_msg_new();
    vrtql_msg_deserialize(m2, reply->data->data, reply->data->size);
    vws_msg_free(reply);

    // Cleanup
    vrtql_msg_free(m1);
    vrtql_msg_free(m2);

    return 0;
}
