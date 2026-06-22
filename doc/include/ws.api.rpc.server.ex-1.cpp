// Define RPC handler functions
vrtql_msg* session_login(vrtql_rpc_env* e, vrtql_msg* req)
{
    vrtql_msg* reply = vrtql_rpc_reply(req);
    vrtql_msg_set_header(reply, "rc", "0");
    vrtql_msg_set_header(reply, "msg", "Login successful");
    return reply;
}

vrtql_msg* session_logout(vrtql_rpc_env* e, vrtql_msg* req)
{
    vrtql_msg* reply = vrtql_rpc_reply(req);
    vrtql_msg_set_header(reply, "rc", "0");
    return reply;
}

// Create and populate a module
vrtql_rpc_module* module = vrtql_rpc_module_new("session");
vrtql_rpc_module_set(module, "login",  session_login);
vrtql_rpc_module_set(module, "logout", session_logout);

// Create the system and register the module
vrtql_rpc_system* system = vrtql_rpc_system_new();
vrtql_rpc_system_set(system, module);

// Service an incoming request (e.g. from a message server handler)
vrtql_rpc_env env;
env.data = NULL;

vrtql_msg* req = vrtql_msg_new();
vrtql_msg_set_header(req, "id", "session.login");

vrtql_msg* reply = vrtql_rpc_service(system, &env, req);

// reply now contains the handler's response
// req has been freed by vrtql_rpc_service()

if (reply != NULL)
{
    vrtql_msg_free(reply);
}

// Cleanup
vrtql_rpc_system_free(system);
