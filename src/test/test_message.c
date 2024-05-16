#include "common.h"

#define CTEST_MAIN
#include "ctest.h"

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

    vws_buffer* binary = vrtql_msg_serialize(send);
    bool rc = vrtql_msg_deserialize(receive, binary->data, binary->size);

    if (rc == false)
    {
        ASSERT_FAIL();
    }

    vws_buffer_free(binary);

    char* key; char* val;

    for (size_t i = 0; i < receive->routing->used; i++)
    {
        key = receive->routing->array[i].key;
        val = receive->routing->array[i].value.data;

        ASSERT_STR(key, "key");
        ASSERT_STR(val, "value");
    }

    for (size_t i = 0; i < receive->headers->used; i++)
    {
        key = receive->headers->array[i].key;
        val = receive->headers->array[i].value.data;

        ASSERT_STR(key, "key");
        ASSERT_STR(val, "value");
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
    vws_buffer* binary = vrtql_msg_serialize(send);
    bool rc = vrtql_msg_deserialize(receive, binary->data, binary->size);

    if (rc == false)
    {
        ASSERT_FAIL();
    }

    vws_buffer_free(binary);

    char* key; char* val;

    for (size_t i = 0; i < receive->routing->used; i++)
    {
        key = receive->routing->array[i].key;
        val = receive->routing->array[i].value.data;

        ASSERT_STR(key, "key");
        ASSERT_STR(val, "value");
    }

    for (size_t i = 0; i < receive->headers->used; i++)
    {
        key = receive->headers->array[i].key;
        val = receive->headers->array[i].value.data;

        ASSERT_STR(key, "key");
        ASSERT_STR(val, "value");
    }

    ASSERT_STR((cstr)receive->content->data, "content");

    vrtql_msg_free(send);
    vrtql_msg_free(receive);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
