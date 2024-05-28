#include "common.h"

#define CTEST_MAIN
#include "ctest.h"

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

CTEST2(test, vws_kvs)
{
    // Initialize the dynamic array
    vws_kvs* map = vws_kvs_new(20);

    // Insert items
    int val1    = 100;
    double val2 = 200.5;
    char val3[] = "value";

    vws_kvs_set(map, "key1", &val1, sizeof(val1));
    vws_kvs_set(map, "key2", &val2, sizeof(val2));
    vws_kvs_set(map, "key3", val3,  sizeof(val3));

    ASSERT_TRUE(vws_kvs_size(map) == 3);

    const char* keys[] = { "key1", "key2", "key3" };

    for (int i = 0; i < 3; i++)
    {
        vws_value* val = vws_kvs_get(map, keys[i]);

        ASSERT_TRUE(val != NULL);

        if (strcmp(keys[i], "key1") == 0)
        {
            ASSERT_TRUE(*(int*)(val->data) == val1);
        }

        if (strcmp(keys[i], "key2") == 0)
        {
            ASSERT_TRUE(*(double*)(val->data) == val2);
        }

        if (strcmp(keys[i], "key3") == 0)
        {
            ASSERT_TRUE(strncmp(val->data, val3, val->size) == 0);
        }
    }

    //> Remove

    vws_kvs_remove(map, "key1");
    vws_kvs_remove(map, "key2");
    vws_kvs_remove(map, "key3");

    ASSERT_TRUE(vws_kvs_size(map) == 0);

    // Insert items

    const char* values[] = { "value1", "value2", "value3" };

    vws_kvs_set_cstring(map, "key1", values[0]);
    vws_kvs_set_cstring(map, "key2", values[1]);
    vws_kvs_set_cstring(map, "key3", values[2]);

    // Find and print values
    for (int i = 0; i < 3; i++)
    {
        const char* val = vws_kvs_get_cstring(map, keys[i]);
        ASSERT_TRUE(val != NULL);
        ASSERT_TRUE(strcmp(val, values[i]) == 0);
    }

    vws_kvs_remove(map, "key1");
    vws_kvs_remove(map, "key2");
    vws_kvs_remove(map, "key3");

    vws_kvs_clear(map);
    ASSERT_TRUE(vws_kvs_size(map) == 0);

    // Free the dynamic array
    vws_kvs_free(map);
}

CTEST2(test, map_bench)
{
    // Initialize the dynamic array
    vws_kvs* map = vws_kvs_new(20);

    // Initialize the hashtable
    struct sc_map_str ht;
    sc_map_init_str(&ht, 0, 0);

    // Add key/value pairs

    size_t searches = 1000000;
    size_t size     = 3;
    char kbuf[256];
    char vbuf[256];

    const char** keys   = malloc(sizeof(char*) * size);
    const char** values = malloc(sizeof(char*) * size);

    // Find and print values
    for (size_t i = 0; i < size; i++)
    {
        sprintf(&kbuf[0],"key%zu", i);
        sprintf(&vbuf[0],"value%zu", i);

        keys[i]   = strdup(&kbuf[i]);
        values[i] = strdup(&vbuf[i]);

        vws_kvs_set_cstring(map, keys[i], values[i]);
        vws_map_set(&ht, keys[i], values[i]);
    }

    // Measure map speed

    clock_t start_time = clock();

    for (size_t i = 0; i < searches; i++)
    {
        vws_kvs_get_cstring(map, keys[i % size]);
    }

    clock_t end_time    = clock();
    double kvs_duration = (double)(end_time - start_time) / CLOCKS_PER_SEC;

    // Measure hashtable speed

    start_time = clock();

    for (size_t i = 0; i < searches; i++)
    {
        vws_map_get(&ht, keys[i % size]);
    }

    end_time            = clock();
    double map_duration = (double)(end_time - start_time) / CLOCKS_PER_SEC;

    printf("KVS:       %f seconds %G ref/sec\n", kvs_duration, kvs_duration/searches);
    printf("Hashtable: %f seconds %G ref/sec\n", map_duration, map_duration/searches);
    printf("Factor:    %f seconds\n", map_duration/kvs_duration);

    //> Cleanup

    // Find and print values
    for (size_t i = 0; i < size; i++)
    {
        free(keys[i]);
        free(values[i]);
    }

    free(keys);
    free(values);

    vws_kvs_free(map);
    vws_map_clear(&ht);
    sc_map_term_str(&ht);
}

CTEST2(test, error_callbacks)
{
    vws.error(VE_SUCCESS, "No error");
}

CTEST2(test, test_url)
{
    cstr value = "http://user:pass@host.com:8080"
                 "/path/to/something?query=string#hash";
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

    vws_cleanup();
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}

#pragma GCC diagnostic pop
#pragma clang diagnostic pop
