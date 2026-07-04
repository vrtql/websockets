// Regression guard: vrtql_rpc_invoke must not treat the peer-supplied "m" reply
// header as a printf format string.
//
// The error path formats the reply's "m" header via vws.error(), which is
// printf-style. Passing the header as the FORMAT argument let a peer reply with
// "m":"%s%s..." crash the client (absent-vararg read) and "m":"%n" write memory.
// The fix passes it as a %s argument. This test drives the real invoke path with
// a hostile "m" header: buggy source crashes, fixed source treats it as literal
// data (error text equals the header verbatim).

#include <stdio.h>
#include <string.h>
#include "message.h"
#include "rpc.c"

static char g_tag[64] = {0};
static const char* HOSTILE = "%s%s%s%s%s%s%s%s%s%s%s%s";

ssize_t __wrap_vrtql_msg_send(vws_cnx* c, vrtql_msg* msg)
{
    (void)c;
    cstr t = vrtql_msg_get_routing(msg, "t");
    if (t != NULL) { strncpy(g_tag, t, sizeof(g_tag) - 1); }
    return 1;
}

vrtql_msg* __wrap_vrtql_msg_recv(vws_cnx* c)
{
    (void)c;
    vrtql_msg* reply = vrtql_msg_new();
    vrtql_msg_set_routing(reply, "t", g_tag);
    vrtql_msg_set_header(reply, "c", "0");
    vrtql_msg_set_header(reply, "m", HOSTILE);
    return reply;
}

int main(void)
{
    vrtql_rpc rpc;
    rpc.cnx = (vws_cnx*)0x1;
    rpc.retries = 5;
    rpc.out_of_band = out_of_band_default;
    rpc.reconnect = NULL;
    rpc.data = NULL;
    rpc.val = vws_buffer_new();

    vrtql_msg* req = vrtql_msg_new();
    vrtql_rpc_invoke(&rpc, req);   // buggy: crashes here; fixed: returns cleanly

    // Fixed source: the hostile header was formatted as data, so the recorded
    // error text equals it verbatim (no expansion, no crash).
    const char* got = vws.e.text;
    vws_buffer_free(rpc.val);

    if (got == NULL || strcmp(got, HOSTILE) != 0)
    {
        printf("FAIL: error text = \"%s\" (expected the literal header)\n",
               got ? got : "(null)");
        return 1;
    }
    printf("PASS: hostile \"m\" header treated as data, no format expansion\n");
    return 0;
}
