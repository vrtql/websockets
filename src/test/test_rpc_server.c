#define CTEST_MAIN
#include "ctest.h"

#include "common.h"

#include "rpc.h"

// RPC Call: session.login
vrtql_msg* session_login(vrtql_rpc_env* e, vrtql_msg* m)
{
    vrtql_msg* reply = vrtql_msg_new();
    vrtql_msg_set_header(reply, "rc", "0");

    return reply;
}

// RPC Call: session.logout
vrtql_msg* session_logout(vrtql_rpc_env* e, vrtql_msg* m)
{
    vrtql_msg* reply = vrtql_msg_new();
    vrtql_msg_set_header(reply, "rc", "0");

    return reply;
}

// RPC Call: session.info
vrtql_msg* session_info(vrtql_rpc_env* e, vrtql_msg* m)
{
    vrtql_msg* reply = vrtql_msg_new();
    vrtql_msg_set_header(reply, "rc", "0");

    return reply;
}

CTEST(test_rpc, server_side_module)
{
    // Define module
    vrtql_rpc_module* module = vrtql_rpc_module_new("session");
    vrtql_rpc_module_set(module, "login",  session_login);
    vrtql_rpc_module_set(module, "logout", session_logout);
    vrtql_rpc_module_set(module, "info",   session_info);

    // Cleanup
    vrtql_rpc_module_free(module);
}

CTEST(test_rpc, server_side_service)
{
    // Create RPC system
    vrtql_rpc_system* system = vrtql_rpc_system_new();

    // Create module
    vrtql_rpc_module* module = vrtql_rpc_module_new("session");
    vrtql_rpc_module_set(module, "login",  session_login);
    vrtql_rpc_module_set(module, "logout", session_logout);
    vrtql_rpc_module_set(module, "info",   session_info);

    // Register module in system
    vrtql_rpc_system_set(system, module);

    // Setup RPC request
    vrtql_rpc_env env;
    vrtql_msg* req = vrtql_msg_new();
    vrtql_msg_set_header(req, "id", "session.login");

    // Invoke RPC
    vrtql_msg* reply = vrtql_rpc_service(system, &env, req);
    ASSERT_TRUE(reply != NULL);

    // Verify reply
    cstr rc = vrtql_msg_get_header(reply, "rc");
    ASSERT_TRUE(strncmp(rc, "0", 1) == 0);

    // Cleanup
    vrtql_msg_free(reply);
    vrtql_rpc_system_free(system);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
