#include "common.h"
#include "ws.h"

CTEST_DATA(test)
{
    unsigned char* buffer;
};

CTEST_SETUP(test)
{
    data->buffer = (unsigned char*)malloc(1024);
}

CTEST_TEARDOWN(test)
{
    if (data->buffer)
    {
        free(data->buffer);
    }
}

CTEST2(test, connect)
{
    vrtql_ws_msg* reply;
    int sent     = 0;
    cstr content = "hello";
    cstr uri     = "ws://localhost:8000/websocket";

    //> Connect

    vrtql_ws_cnx* c = vrtql_ws_cnx_new(uri);
    ASSERT_TRUE(vrtql_ws_connect(c) == true);

    //> Text message

    sent = vrtql_ws_send_text(c, content);
    ASSERT_TRUE(sent = strlen(content));
    reply = vrtql_ws_recv(c);
    ASSERT_TRUE(reply != NULL);
    ASSERT_TRUE(strncmp(content, reply->data, reply->size) == 0);
    vrtql_ws_msg_free(reply);

    //> Binary message

    sent = vrtql_ws_send_binary(c, content, strlen(content));
    ASSERT_TRUE(sent = strlen(content));
    reply = vrtql_ws_recv(c);
    ASSERT_TRUE(reply != NULL);
    ASSERT_TRUE(strncmp(content, reply->data, reply->size) == 0);
    vrtql_ws_msg_free(reply);

    //> Cleanup

    vrtql_ws_close(c);
    vrtql_ws_cnx_free(c);
}

int main(int argc, const char* argv[])
{
    int result = ctest_main(argc, argv);

    return 0;
}
