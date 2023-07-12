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
    vws_msg* m = vws_recv_msg(c);                                         \
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
    data->c->trace = true;
}

CTEST_TEARDOWN(test)
{
    vws_cnx_free(data->c);
}

CTEST2(test, send_receive)
{
    vws_send_text(data->c, content);
    check_reply(data->c, content);
}

CTEST2(test, send_data)
{
    vws_send_data(data->c, (ucstr)content, strlen(content), 0x1);
    check_reply(data->c, content);
}

CTEST2(test, send_frame)
{
    vws_frame* frame = vws_frame_new((ucstr)content, strlen(content), 0x1);
    vws_send_frame(data->c, frame);
    check_reply(data->c, content);
}

CTEST2(test, message)
{
    cstr payload = "payload";

    // Create
    vrtql_msg* m1 = vrtql_msg_new();
    vrtql_msg_set_content(m1, payload);

    // Serialize and send
    vrtql_buffer* binary = vrtql_msg_serialize(m1, VM_MPACK_FORMAT);
    vws_send_data(data->c, binary->data, binary->size, 0x2);
    vrtql_buffer_free(binary);

    // Receive websocket message
    vws_msg* wsm = vws_recv_msg(data->c);
    ASSERT_TRUE(wsm != NULL);

    // Deserialize VRTQL message
    vrtql_msg* m2 = vrtql_msg_new();

    bool rc = vrtql_msg_deserialize(m2, wsm->data->data, wsm->data->size);
    ASSERT_TRUE(rc == true);
    vws_msg_free(wsm);

    // Check
    ASSERT_TRUE(strncmp((cstr)m2->content->data, payload, m2->content->size) == 0);

    // Cleanup
    vrtql_msg_free(m1);
    vrtql_msg_free(m2);
}

int main(int argc, const char* argv[])
{
    int result = ctest_main(argc, argv);

    return 0;
}
