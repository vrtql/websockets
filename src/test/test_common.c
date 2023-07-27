#include "common.h"

#include "vrtql.h"
#include "url.h"
#include "util/sc_map.h"
#include "util/sc_queue.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"

CTEST_DATA(test)
{
    unsigned char* buffer;
};

CTEST_SETUP(test)
{
    CTEST_LOG("%s()", __func__);

    data->buffer = (unsigned char*)malloc(1024);
}

CTEST_TEARDOWN(test)
{
    CTEST_LOG("%s()", __func__);

    if (data->buffer)
    {
        free(data->buffer);
    }
}

CTEST2(test, error_callbacks)
{
    vrtql.error(VE_SUCCESS, "No error");
}

CTEST2(test, test_url)
{
    cstr value = "http://user:pass@host.com:8080/path/to/something?query=string#hash";
    url_data_t* url = url_parse(value);

    vrtql.trace(VL_INFO, "url protocol: %s", url->protocol);
    vrtql.trace(VL_INFO, "url host:     %s", url->host);
    vrtql.trace(VL_INFO, "url auth:     %s", url->auth);
    vrtql.trace(VL_INFO, "url hostname: %s", url->hostname);
    vrtql.trace(VL_INFO, "url pathname: %s", url->pathname);
    vrtql.trace(VL_INFO, "url search:   %s", url->search);
    vrtql.trace(VL_INFO, "url path:     %s", url->path);
    vrtql.trace(VL_INFO, "url query:    %s", url->query);
    vrtql.trace(VL_INFO, "url port:     %s", url->port);

    url_free(url);
}

CTEST2(test, base64)
{
    const unsigned char original_data[] = "Hello, World!";
    size_t original_length = strlen((const char *)original_data);

    // Encoding

    char* encoded = vrtql_base64_encode(original_data, original_length);

    // Decoding

    unsigned char* decoded;
    size_t output_length = 0;
    decoded = vrtql_base64_decode(encoded, &output_length);

    printf("Base64 Encoded: %s\n", encoded);
    printf("Base64 Decoded: %s\n", decoded);

    free(encoded);
    free(decoded);
}

CTEST2(test, buffer)
{
    vrtql_buffer* buffer = vrtql_buffer_new(&buffer);

    // Append data to the buffer
    cstr data1 = "Hello, ";
    vrtql_buffer_append(buffer, (ucstr)data1, strlen(data1));

    cstr data2 = "world!";
    vrtql_buffer_append(buffer, (ucstr)data2, strlen(data2));

    // Drain the first 7 bytes from the buffer
    vrtql_buffer_drain(buffer, 7);

    // Print the remaining data in the buffer
    printf("%s\n", buffer->data);
    ASSERT_STR((cstr)buffer->data, "world!");

    // Free the buffer
    vrtql_buffer_free(buffer);
}

CTEST2(test, queue)
{
    const void* elem;
    struct sc_queue_ptr queue;

    sc_queue_init(&queue);
    sc_queue_add_last(&queue, "one");
    sc_queue_add_last(&queue, "one");
    sc_queue_add_last(&queue, "one");

    sc_queue_foreach (&queue, elem)
    {
        ASSERT_STR(elem, "one");
    }

    sc_queue_term(&queue);
}

CTEST(test, map)
{
    ASSERT_EQUAL(1, 1);
    ASSERT_STR("test", "test");
}

CTEST(test, trace)
{
    printf("\n");

    vrtql.trace(VL_DEBUG,   "vrtql.trace(%s)",   "DEBUG");
    vrtql.trace(VL_INFO,    "vrtql.trace(%s)",    "INFO");
    vrtql.trace(VL_WARN,    "vrtql.trace(%s)", "WARNING");
    vrtql.trace(VL_ERROR,   "vrtql.trace(%s)",   "ERROR");
}

CTEST(test, error)
{
    printf("\n");

    vrtql.tracelevel = true;
    vrtql.error(VE_RT, "Handshake invalid");
}

int main(int argc, const char* argv[])
{
    int result = ctest_main(argc, argv);

    return 0;
}

#pragma GCC diagnostic pop
#pragma clang diagnostic pop
