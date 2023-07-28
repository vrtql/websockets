#include "common.h"
#include "vws.h"
#include "util/yyjson.h"

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
    data->buffer = (unsigned char*)malloc(1024);
}

CTEST_TEARDOWN(test)
{
    if (data->buffer)
    {
        free(data->buffer);
    }
}

CTEST2(test, read)
{
    const char *json = "{\"name\":\"Mash\",\"star\":4,\"hits\":[2,2,1,3]}";

    // Read JSON and get root
    yyjson_doc* doc  = yyjson_read(json, strlen(json), 0);
    yyjson_val* root = yyjson_doc_get_root(doc);

    // Get root["name"]
    yyjson_val* name = yyjson_obj_get(root, "name");

    ASSERT_TRUE(yyjson_get_type(root) == YYJSON_TYPE_OBJ);

    printf("name: %s\n", yyjson_get_str(name));
    printf("name length:%d\n", (int)yyjson_get_len(name));

    // Get root["star"]
    yyjson_val *star = yyjson_obj_get(root, "star");
    printf("star: %d\n", (int)yyjson_get_int(star));

    // Get root["hits"], iterate over the array
    yyjson_val *hits = yyjson_obj_get(root, "hits");
    size_t idx, max;
    yyjson_val *hit;
    yyjson_arr_foreach(hits, idx, max, hit)
    {
        printf("hit%d: %d\n", (int)idx, (int)yyjson_get_int(hit));
    }

    // Free the doc
    yyjson_doc_free(doc);
}

CTEST2(test, write)
{
    // Create a mutable doc
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    // Set root["name"] and root["star"]
    yyjson_mut_obj_add_str(doc, root, "name", "Mash");
    yyjson_mut_obj_add_int(doc, root, "star", 4);

    // Set root["hits"] with an array
    int hits_arr[] = {2, 2, 1, 3};
    yyjson_mut_val* hits = yyjson_mut_arr_with_sint32(doc, hits_arr, 4);
    yyjson_mut_obj_add_val(doc, root, "hits", hits);

    // To string, minified
    cstr json = yyjson_mut_write(doc, 0, NULL);

    if (json)
    {
        printf("json: %s\n", json); // {"name":"Mash","star":4,"hits":[2,2,1,3]}
        free((void *)json);
    }

    // Free the doc
    yyjson_mut_doc_free(doc);
}

int main(int argc, cstr argv[])
{
    int result = ctest_main(argc, argv);

    return 0;
}

#pragma GCC diagnostic pop
#pragma clang diagnostic pop
