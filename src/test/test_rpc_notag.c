// regression guard — vrtql_rpc_exec must not crash on a reply that
// carries no tag.
//
// rpc.c reads the reply tag then compares it:
//   cstr t = vrtql_msg_get_routing(reply, "t");
//   if (t == NULL || strncmp(tag, t, strlen(tag)) != 0) { out_of_band; continue; }
//
// vrtql_msg_get_routing returns NULL when the received reply has no tag routing
// entry (a malformed or unrelated peer message). The pre-fix code read key
// "tag" (which no reply ever carried, since the request set "tag" but the reply
// builder echoes "t") and passed the NULL straight to strncmp() -> crash. The
// fix aligns the key to "t" on both the set and the read, and guards t == NULL.
//
// This test #includes rpc.c and wraps the send/recv seam. The recv wrap returns
// ONE reply with no tag routing, then NULL/timeout so the fixed retry loop
// drains and returns. On buggy source the first reply crashes exec (strncmp on
// NULL). On fixed source exec routes it out-of-band and returns NULL cleanly.

#include <stdio.h>
#include <unistd.h>
#include "message.h"
#include "rpc.c"

static const unsigned WATCHDOG_SECONDS = 30;

// Force the send loop to succeed.
ssize_t __wrap_vrtql_msg_send(vws_cnx* c, vrtql_msg* msg)
{
    (void)c; (void)msg;
    return 1;
}

// First call: a reply with routing but NO tag entry. Later calls: timeout/NULL
// so the fixed retry loop terminates instead of spinning on out-of-band.
static int g_recv_calls = 0;
vrtql_msg* __wrap_vrtql_msg_recv(vws_cnx* c)
{
    (void)c;

    if (g_recv_calls++ == 0)
    {
        vrtql_msg* reply = vrtql_msg_new();
        vrtql_msg_set_routing(reply, "from", "peer");   // present, but no tag
        return reply;
    }

    vws.e.code = VE_TIMEOUT;
    return NULL;
}

int main(void)
{
    alarm(WATCHDOG_SECONDS);

    vrtql_rpc rpc;
    rpc.cnx         = (vws_cnx*)0x1;   // never dereferenced (send/recv wrapped)
    rpc.retries     = 5;
    rpc.out_of_band = out_of_band_default;
    rpc.reconnect   = NULL;
    rpc.data        = NULL;
    rpc.val         = vws_buffer_new();

    vrtql_msg* req   = vrtql_msg_new();

    // Buggy source crashes here (strncmp on a NULL tag). Fixed source routes the
    // tagless reply out-of-band and, finding no matching response, returns NULL.
    vrtql_msg* reply = vrtql_rpc_exec(&rpc, req);

    vws_buffer_free(rpc.val);
    alarm(0);

    if (reply != NULL)
    {
        printf("FAIL: expected NULL (no matching response), got %p\n",
               (void*)reply);
        vrtql_msg_free(reply);
        return 1;
    }

    printf("PASS: tagless reply routed out-of-band, no crash\n");
    return 0;
}
