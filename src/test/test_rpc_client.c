#include "rpc.h"

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"

cstr content = "content";
cstr uri     = "wss://gem.vrtql.com:2020";

CTEST_DATA(test)
{
    vws_cnx* c;
    vrtql_rpc* rpc;
};

CTEST_SETUP(test)
{
    data->c = vws_cnx_new();
    ASSERT_TRUE(data->c != NULL);
    ASSERT_TRUE(vws_connect(data->c, uri));

    data->rpc = vrtql_rpc_new(data->c);

    // Enable tracing
    vws.tracelevel = VT_PROTOCOL;
}

CTEST_TEARDOWN(test)
{
    vws_cnx_free(data->c);
    vrtql_rpc_free(data->rpc);
}

CTEST2(test, basic)
{
    // Create request message
    vrtql_msg* req = vrtql_msg_new();
    vrtql_msg_set_header(req, "id", "vql");

    // Headers for authentication
    vrtql_msg_set_header(req, "username", "vrtql");
    vrtql_msg_set_header(req, "password", "vrtql");
    vrtql_msg_set_header(req, "domain", "global");

    // VQL command to execute
    vrtql_msg_set_content(req, "sql select * from solari");

    vrtql_msg* reply = vrtql_rpc_exec(data->rpc, req);
    ASSERT_TRUE(reply != NULL);
    vrtql_msg_dump(reply);

    vrtql_msg_free(req);
    vrtql_msg_free(reply);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
