// Create a WebSocket connection and connect
vws_cnx* cnx = vws_cnx_new();
vws_connect(cnx, "ws://localhost:8181/websocket");

// Create an RPC instance using the connection
vrtql_rpc* rpc = vrtql_rpc_new(cnx);

// Create a request message
vrtql_msg* req = vrtql_msg_new();
vrtql_msg_set_header(req, "id", "session.login");
vrtql_msg_set_header(req, "username", "admin");
vrtql_msg_set_header(req, "password", "secret");

// Make the RPC call (low-level)
vrtql_msg* reply = vrtql_rpc_exec(rpc, req);

if (reply != NULL)
{
    cstr rc = vrtql_msg_get_header(reply, "rc");
    printf("Return code: %s\n", rc);
    vrtql_msg_free(reply);
}

// Cleanup
vrtql_msg_free(req);
vrtql_rpc_free(rpc);
vws_disconnect(cnx);
vws_cnx_free(cnx);
