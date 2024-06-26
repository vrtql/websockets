#define CTEST_MAIN
#include "ctest.h"

#include "common.h"

#include "http_message.h"

CTEST(test_rpc, call)
{
    char* data = "GET /hello HTTP/1.1\r\n"
                 "Host: example.com\r\n"
                 "User-Agent: curl/7.68.0\r\n"
                 "Accept: */*\r\n"
                 "\r\n";

    vws_http_msg* req = vws_http_msg_new(HTTP_REQUEST);

    vws_http_msg_parse(req, data, strlen(data));
    ASSERT_TRUE(req->headers_complete == true);

    printf("\n");

    for(uint32_t i = 0; i < req->headers->used; i++)
    {
        cstr key   = req->headers->array[i].key;
        cstr value = req->headers->array[i].value.data;

        printf("Header: %s: %s\n", key, value);
    }

    vws_http_msg_free(req);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
