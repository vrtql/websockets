#include "common.h"

#include "vws.h"
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
    vws.error(VE_SUCCESS, "No error");
}

CTEST2(test, test_url)
{
    cstr value = "http://user:pass@host.com:8080/path/to/something?query=string#hash";
    url_data_t* url = url_parse(value);

    vws.trace(VL_INFO, "url protocol: %s", url->protocol);
    vws.trace(VL_INFO, "url host:     %s", url->host);
    vws.trace(VL_INFO, "url auth:     %s", url->auth);
    vws.trace(VL_INFO, "url hostname: %s", url->hostname);
    vws.trace(VL_INFO, "url pathname: %s", url->pathname);
    vws.trace(VL_INFO, "url search:   %s", url->search);
    vws.trace(VL_INFO, "url path:     %s", url->path);
    vws.trace(VL_INFO, "url query:    %s", url->query);
    vws.trace(VL_INFO, "url port:     %s", url->port);

    url_free(url);
}

CTEST2(test, base64)
{
    const unsigned char original_data[] = "Hello, World!";
    size_t original_length = strlen((const char *)original_data);

    // Encoding

    char* encoded = vws_base64_encode(original_data, original_length);

    // Decoding

    unsigned char* decoded;
    size_t output_length = 0;
    decoded = vws_base64_decode(encoded, &output_length);

    printf("Base64 Encoded: %s\n", encoded);
    printf("Base64 Decoded: %s\n", decoded);

    free(encoded);
    free(decoded);
}

CTEST2(test, buffer)
{
    vws_buffer* buffer = vws_buffer_new(&buffer);

    // Append data to the buffer
    cstr data1 = "Hello, ";
    vws_buffer_append(buffer, (ucstr)data1, strlen(data1));

    cstr data2 = "world!";
    vws_buffer_append(buffer, (ucstr)data2, strlen(data2));

    // Drain the first 7 bytes from the buffer
    vws_buffer_drain(buffer, 7);

    // Print the remaining data in the buffer
    printf("%s\n", buffer->data);
    ASSERT_STR((cstr)buffer->data, "world!");

    // Free the buffer
    vws_buffer_free(buffer);
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

    vws.trace(VL_DEBUG,   "vws.trace(%s)",   "DEBUG");
    vws.trace(VL_INFO,    "vws.trace(%s)",    "INFO");
    vws.trace(VL_WARN,    "vws.trace(%s)", "WARNING");
    vws.trace(VL_ERROR,   "vws.trace(%s)",   "ERROR");
}

CTEST(test, error)
{
    printf("\n");

    vws.tracelevel = true;
    vws.error(VE_RT, "Handshake invalid");
}

int main(int argc, const char* argv[])
{
    int result = ctest_main(argc, argv);

    return 0;
}

#pragma GCC diagnostic pop
#pragma clang diagnostic pop
