#include "common.h"

#include "message.h"
#include "mpack-reader.h"
#include "mpack-writer.h"

CTEST(test_message, mpack_serialization)
{
    vrtql_msg* send    = vrtql_msg_new();
    vrtql_msg* receive = vrtql_msg_new();

    vrtql_msg_set_routing(send, "key", "value");
    vrtql_msg_set_header(send, "key", "value");
    vrtql_msg_set_content(send, "content");

    vrtql_buffer* binary = vrtql_msg_serialize(send);
    bool rc = vrtql_msg_deserialize(receive, binary->data, binary->size);

    if (rc == false)
    {
        ASSERT_FAIL();
    }

    vrtql_buffer_free(binary);

    char* key; char* value;

    sc_map_foreach(&receive->routing, key, value)
    {
        ASSERT_STR(key, "key");
        ASSERT_STR(value, "value");
    }

    sc_map_foreach(&receive->headers, key, value)
    {
        ASSERT_STR(key, "key");
        ASSERT_STR(value, "value");
    }

    ASSERT_STR((cstr)receive->content->data, "content");

    vrtql_msg_free(send);
    vrtql_msg_free(receive);
}

CTEST(test_message, json_serialization)
{
    vrtql_msg* send    = vrtql_msg_new();
    vrtql_msg* receive = vrtql_msg_new();

    vrtql_msg_set_routing(send, "key", "value");
    vrtql_msg_set_header(send, "key", "value");
    vrtql_msg_set_content(send, "content");

    send->format = VM_MPACK_FORMAT;
    vrtql_buffer* binary = vrtql_msg_serialize(send);
    bool rc = vrtql_msg_deserialize(receive, binary->data, binary->size);

    if (rc == false)
    {
        ASSERT_FAIL();
    }

    vrtql_buffer_free(binary);

    char* key; char* value;

    sc_map_foreach(&receive->routing, key, value)
    {
        ASSERT_STR(key, "key");
        ASSERT_STR(value, "value");
    }

    sc_map_foreach(&receive->headers, key, value)
    {
        ASSERT_STR(key, "key");
        ASSERT_STR(value, "value");
    }

    ASSERT_STR((cstr)receive->content->data, "content");

    vrtql_msg_free(send);
    vrtql_msg_free(receive);
}

int main(int argc, const char* argv[])
{
    int result = ctest_main(argc, argv);

    return 0;
}
