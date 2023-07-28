#include "common.h"

#include "rpc.h"

// RPC Call: session.login
vrtql_msg* session_login(vws_rpc_env* e, vrtql_msg* m)
{
    vrtql_msg* reply = vrtql_msg_new();
    vrtql_msg_set_header(reply, "rc", "0");

    return reply;
}

// RPC Call: session.logout
vrtql_msg* session_logout(vws_rpc_env* e, vrtql_msg* m)
{
    vrtql_msg* reply = vrtql_msg_new();
    vrtql_msg_set_header(reply, "rc", "0");

    return reply;
}

// RPC Call: session.info
vrtql_msg* session_info(vws_rpc_env* e, vrtql_msg* m)
{
    vrtql_msg* reply = vrtql_msg_new();
    vrtql_msg_set_header(reply, "rc", "0");

    return reply;
}

CTEST(test_rpc, module)
{
    // Define module
    vws_rpc_module* module = vws_rpc_module_new("session");
    vws_rpc_module_set(module, "login",  session_login);
    vws_rpc_module_set(module, "logout", session_logout);
    vws_rpc_module_set(module, "info",   session_info);

    // Cleanup
    vws_rpc_module_free(module);
}

CTEST(test_rpc, call)
{
    // Create RPC system
    vws_rpc_system* system = vws_rpc_system_new();

    // Create module
    vws_rpc_module* module = vws_rpc_module_new("session");
    vws_rpc_module_set(module, "login",  session_login);
    vws_rpc_module_set(module, "logout", session_logout);
    vws_rpc_module_set(module, "info",   session_info);

    // Register module in system
    vws_rpc_system_set(system, module);

    // Setup RPC request
    vws_rpc_env env;
    vrtql_msg* req = vrtql_msg_new();
    vrtql_msg_set_header(req, "id", "session.login");

    // Invoke RPC
    vrtql_msg* reply = vws_rpc(system, &env, req);
    ASSERT_TRUE(reply != NULL);

    // Verify reply
    cstr rc = vrtql_msg_get_header(reply, "rc");
    ASSERT_TRUE(strncmp(rc, "0", 1) == 0);

    // Cleanup
    vrtql_msg_free(reply);
    vws_rpc_system_free(system);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
