#include <string.h>
#include <vws/server.h>
#include <vws/rpc.h>

// Worker thread context holds the RPC system and environment
typedef struct
{
    vrtql_rpc_system* system;
    vrtql_rpc_env env;
} rpc_ctx;

//----------------------------------------------------------------------
// RPC Handlers: "user" module
//----------------------------------------------------------------------

vrtql_msg* user_login(vrtql_rpc_env* e, vrtql_msg* req)
{
    vrtql_msg* reply = vrtql_rpc_reply(req);

    cstr username = vrtql_msg_get_header(req, "username");
    cstr password = vrtql_msg_get_header(req, "password");

    if (username != NULL && password != NULL)
    {
        // Perform authentication (simplified)
        vrtql_msg_set_header(reply, "rc", "0");
        vrtql_msg_set_header(reply, "msg", "Authenticated");
        vrtql_msg_set_content(reply, "{\"token\":\"abc123\"}");
    }
    else
    {
        vrtql_msg_set_header(reply, "rc", "1");
        vrtql_msg_set_header(reply, "msg", "Missing credentials");
    }

    return reply;
}

vrtql_msg* user_info(vrtql_rpc_env* e, vrtql_msg* req)
{
    vrtql_msg* reply = vrtql_rpc_reply(req);
    vrtql_msg_set_header(reply, "rc", "0");
    vrtql_msg_set_content(reply, "{\"name\":\"admin\",\"role\":\"superuser\"}");
    return reply;
}

//----------------------------------------------------------------------
// RPC Handlers: "system" module
//----------------------------------------------------------------------

vrtql_msg* system_status(vrtql_rpc_env* e, vrtql_msg* req)
{
    vrtql_msg* reply = vrtql_rpc_reply(req);
    vrtql_msg_set_header(reply, "rc", "0");
    vrtql_msg_set_content(reply, "{\"status\":\"running\"}");
    return reply;
}

//----------------------------------------------------------------------
// Server setup
//----------------------------------------------------------------------

// Build the RPC system with all modules
vrtql_rpc_system* build_rpc_system()
{
    vrtql_rpc_system* system = vrtql_rpc_system_new();

    // User module
    vrtql_rpc_module* user = vrtql_rpc_module_new("user");
    vrtql_rpc_module_set(user, "login", user_login);
    vrtql_rpc_module_set(user, "info",  user_info);
    vrtql_rpc_system_set(system, user);

    // System module
    vrtql_rpc_module* sys = vrtql_rpc_module_new("system");
    vrtql_rpc_module_set(sys, "status", system_status);
    vrtql_rpc_system_set(system, sys);

    return system;
}

// Worker thread constructor: each thread gets its own RPC environment
void* worker_ctor(void* data)
{
    rpc_ctx* ctx = (rpc_ctx*)malloc(sizeof(rpc_ctx));
    ctx->system  = (vrtql_rpc_system*)data;
    ctx->env.data   = NULL;
    ctx->env.module = NULL;
    return ctx;
}

// Worker thread destructor
void worker_dtor(void* data)
{
    rpc_ctx* ctx = (rpc_ctx*)data;
    free(ctx);
}

// Message processing: dispatch to RPC system
void process(vws_svr* s, vws_cid_t cid, vrtql_msg* m, void* ctx)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)s;
    rpc_ctx* rctx = (rpc_ctx*)ctx;

    // Dispatch to RPC system
    vrtql_msg* reply = vrtql_rpc_service(rctx->system, &rctx->env, m);

    if (reply != NULL)
    {
        server->send(s, cid, reply, NULL);
    }

    // Note: m has been freed by vrtql_rpc_service()
}

int main(int argc, const char* argv[])
{
    // Build the RPC system
    vrtql_rpc_system* system = build_rpc_system();

    // Create server
    vrtql_msg_svr* server = vrtql_msg_svr_new(10, 0, 0);
    server->process       = process;

    // Worker threads share the RPC system (read-only) but get their
    // own environment via worker_ctor
    vws_tcp_svr* base        = (vws_tcp_svr*)server;
    base->worker_ctor        = worker_ctor;
    base->worker_ctor_data   = system;
    base->worker_dtor        = worker_dtor;

    // Run
    vrtql_msg_svr_run(server, "127.0.0.1", 8181);

    // Cleanup
    vrtql_msg_svr_free(server);
    vrtql_rpc_system_free(system);
    vws_cleanup();

    return 0;
}
