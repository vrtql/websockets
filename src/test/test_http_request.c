#include "common.h"

#include "http_message.h"

CTEST(test_rpc, call)
{
    char* data = "GET /hello HTTP/1.1\r\n"
                 "Host: example.com\r\n"
                 "User-Agent: curl/7.68.0\r\n"
                 "Accept: */*\r\n"
                 "\r\n";

    vrtql_http_msg* req = vrtql_http_msg_new(HTTP_REQUEST);

    vrtql_http_msg_parse(req, data, strlen(data));
    ASSERT_TRUE(req->complete == true);

    printf("\n");

    cstr key; cstr value;
    sc_map_foreach(&req->headers, key, value)
    {
        printf("Header: %s: %s\n", key, value);
    }

    vrtql_http_msg_free(req);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
