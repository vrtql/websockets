#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "websocket.h"
#include "message.h"

#include "common.h"

cstr content = "content";
cstr uri     = "ws://localhost:8181/websocket";

#define check_reply(c, value)                                             \
{                                                                         \
    vws_msg* m = vws_msg_recv(c);                                         \
    ASSERT_TRUE(m != NULL);                                               \
    ASSERT_TRUE(strncmp((cstr)m->data->data, value, m->data->size) == 0); \
    vws_msg_free(m);\
}

CTEST_DATA(test)
{
    unsigned char* buffer;
    vws_cnx* c;
};

CTEST_SETUP(test)
{
    data->c = vws_cnx_new();
    ASSERT_TRUE(data->c != NULL);
    ASSERT_TRUE(vws_connect(data->c, uri));

    // Enable tracing
    vrtql.trace = VT_PROTOCOL;
}

CTEST_TEARDOWN(test)
{
    vws_cnx_free(data->c);
}

CTEST2(test, send_receive)
{
    vws_frame_send_text(data->c, content);
    check_reply(data->c, content);
}

CTEST2(test, send_data)
{
    vws_frame_send_data(data->c, (ucstr)content, strlen(content), 0x1);
    check_reply(data->c, content);
}

CTEST2(test, send_frame)
{
    vws_frame* frame = vws_frame_new((ucstr)content, strlen(content), 0x1);
    vws_frame_send(data->c, frame);
    check_reply(data->c, content);
}

CTEST2(test, message)
{
    cstr payload = "payload";

    // Create
    vrtql_msg* request = vrtql_msg_new();
    vrtql_msg_set_content(request, payload);

    // Send
    ASSERT_TRUE(vrtql_msg_send(data->c, request) > 0);

    // Receive
    vrtql_msg* reply = vrtql_msg_recv(data->c);
    ASSERT_TRUE(reply != NULL);

    // Check
    cstr content = reply->content->data;
    size_t size  = reply->content->size;
    ASSERT_TRUE(strncmp(payload, content, size) == 0);

    // Cleanup
    vrtql_msg_free(request);
    vrtql_msg_free(reply);
}

int main(int argc, const char* argv[])
{
    int result = ctest_main(argc, argv);

    return 0;
}
