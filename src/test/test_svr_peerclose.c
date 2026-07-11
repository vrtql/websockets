// test_svr_peerclose.c — vsa 693392dc1d: peer stays CONNECTED with a
// dangling info.cnx after any non-EOF close of its connection.
//
// DEFECT (vws server.c, pristine): only the read-EOF arm (~:2611) and the
// pong-deadline arm (~:1267) mark a peer VWS_PEER_CLOSED before closing its
// cnx. Every other close of a peer link — the CLOSE-response arm (~:1064,
// reached by vws_tcp_svr_close), the write-queue-cap shed, and
// peer_disconnect — frees the cnx and leaves the peer record CONNECTED with
// info.cnx dangling. The wd sweep then reads through the freed cnx on its
// next tick (`peer->info.cnx->data`, ~:1248), can write and double-close
// through it, the reconnect branch (~:1090) never re-fires (silent
// partition), and the unrecoverable report never triggers.
//
// WIRE-REACHABLE TRIGGER (this harness, wa2 repro shape WA-A-1): one
// garbage (non-vrtql_msg) websocket payload on an established peer link.
// The msg layer's deserialize fails (msg_svr_client_ws_msg_in, ~:3553) ->
// vws_tcp_svr_close -> CLOSE arm -> uv_close with NO peer feedback.
//
// CELLS (select via WF2_CELL env; ports via WF2_PORT_A / WF2_PORT_B — both
// REQUIRED, no defaults: runs only inside a wf1-announced port slot):
//
//   garbage_close_feedback
//     Carrier A = vrtql_msg_svr owning a peer link dialed to carrier B
//     (plain ws-level vws_svr). Once CONNECTED, A sends one message; B
//     replies with garbage bytes. Pristine lib (RED): ASan
//     heap-use-after-free at the sweep's freed read within ~1 tick of the
//     close — the log-witnessed RED. Patched lib (GREEN): the funneled
//     feedback (svr_peer_close_feedback) marks the peer CLOSED, the
//     reconnect branch re-dials, and the cell asserts recovery: peer
//     CONNECTED again under a NEW cid.key (proves a real re-dial, not a
//     stale record).
//
//   control_eof_redial (non-vacuity sibling, must pass on pristine AND
//     patched): same fixture, but B abruptly closes the link from its side
//     (vws_tcp_svr_close on B = bare uv_close, no WS CLOSE frame) -> A sees
//     read EOF -> the EOF arm's pre-existing feedback marks the peer CLOSED
//     -> re-dial -> recovery under a new cid.key. Proves the fixture, the
//     dial loop, and the recovery observation are all sound where feedback
//     exists, isolating the RED to the missing feedback on the CLOSE arm.
//
// SELF-BOUNDING: every cell arms a watchdog alarm; every poll has a
// deadline. The harness cannot hang a battery.
//
// Build: standalone against the iso-built libvws (pristine => RED binary,
// patched => GREEN binary). Scratchpad-only; never lands in shared
// src/test.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"
#include "message.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

static cstr host = "127.0.0.1";

static int  port_a = 0;              // carrier A (msg svr owning the peer)
static int  port_b = 0;              // carrier B (remote ws svr, dialed)
static char uri_b[128];              // ws://host:port_b/websocket

// Carrier B behavior for the current cell.
typedef enum
{
    B_REPLY_GARBAGE,   // reply one non-vrtql_msg payload (deserialize fail)
    B_CLOSE_ABRUPT     // close the cid (bare uv_close => EOF at A)
} b_mode_t;

static b_mode_t b_mode = B_REPLY_GARBAGE;

//------------------------------------------------------------------------
// Carrier B: plain ws-level server
//------------------------------------------------------------------------

static void b_process_ws(vws_svr* s, vws_cid_t cid, vws_msg* m, void* ctx)
{
    (void)ctx;

    vws_msg_free(m);

    if (b_mode == B_REPLY_GARBAGE)
    {
        // One payload that can never vrtql_msg_deserialize().
        vws_msg* reply = vws_msg_new();
        reply->opcode  = TEXT_FRAME;

        cstr junk = "GARBAGE-NOT-A-VRTQL-MSG";
        vws_buffer_append(reply->data, (ucstr)junk, strlen(junk));

        s->send(s, cid, reply, NULL);
    }
    else
    {
        // Abrupt close: queues a CLOSE block; B's wd loop bare-uv_closes
        // the handle (no WS CLOSE frame) => carrier A reads EOF.
        vws_tcp_svr_close((vws_tcp_svr*)s, cid);
    }
}

static void b_thread(void* arg)
{
    vws_svr* server = (vws_svr*)arg;

    ((vws_tcp_svr*)server)->trace = vws.tracelevel;

    vws_svr_run(server, host, port_b);
    vws_cleanup();
}

//------------------------------------------------------------------------
// Carrier A: msg-level server owning the peer link
//------------------------------------------------------------------------

static void a_process_msg(vws_svr* s, vws_cid_t cid, vrtql_msg* m, void* ctx)
{
    (void)s;
    (void)cid;
    (void)ctx;

    // A's peer link never receives a valid vrtql_msg in these cells; the
    // garbage payload dies in deserialize before reaching here.
    vrtql_msg_free(m);
}

static vws_sockfd_t a_peer_connect(vws_peer* p, void* x)
{
    (void)p;
    (void)x;

    vws_cnx* cnx = vws_cnx_new();

    if (vws_connect(cnx, uri_b) == false)
    {
        vws_cnx_free(cnx);
        return -1;
    }

    vws_sockfd_t sockfd = cnx->base.sockfd;
    cnx->base.sockfd    = -1;
    vws_cnx_free(cnx);

    return sockfd;
}

static void a_thread(void* arg)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)arg;

    ((vws_tcp_svr*)server)->trace = vws.tracelevel;

    vws_tcp_svr_peer_add( (vws_tcp_svr*)server,
                          host,
                          port_b,
                          a_peer_connect,
                          NULL );

    vws_tcp_svr_run((vws_tcp_svr*)server, host, port_a);
    vws_cleanup();
}

//------------------------------------------------------------------------
// Fixture helpers
//------------------------------------------------------------------------

// Bounded poll: wait until carrier A's (single) peer is CONNECTED with a
// cid.key different from not_key. Pool keys are 0-based slot indexes (the
// FIRST connection gets key 0); the invalid sentinel is -1 (vws_cid_clear).
// Returns the peer's current cid.key (>= 0), or -1 on deadline. Pass
// not_key = -1 to accept any CONNECTED state (initial link-up).
static int64_t wait_peer_connected( vws_tcp_svr* s,
                                    int64_t      not_key,
                                    int          deadline_ms )
{
    uint64_t start = vws_now_ms();

    while ((vws_now_ms() - start) < (uint64_t)deadline_ms)
    {
        if (s->peers->used > 0)
        {
            vws_peer* peer = (vws_peer*)s->peers->array[0].value.data;

            if (peer != NULL && peer->state == VWS_PEER_CONNECTED)
            {
                int64_t key = peer->info.cid.key;

                if (key >= 0 && key != not_key)
                {
                    return key;
                }
            }
        }

        vws_msleep(50);
    }

    return -1;
}

static bool wait_state(vws_tcp_svr* s, uint8_t st, int deadline_ms)
{
    uint64_t start = vws_now_ms();

    while (vws_tcp_svr_state(s) != st)
    {
        if ((vws_now_ms() - start) >= (uint64_t)deadline_ms)
        {
            return false;
        }

        vws_msleep(50);
    }

    return true;
}

// Send one (well-formed) trigger message from A to its peer cid. B's
// b_process_ws reacts per b_mode.
static void send_trigger(vrtql_msg_svr* a, vws_cid_t cid)
{
    vrtql_msg* m = vrtql_msg_new();
    vrtql_msg_set_content(m, "trigger");

    // send() takes ownership of m.
    a->send((vws_svr*)a, cid, m, NULL);
}

static bool cell_enabled(cstr name)
{
    cstr sel = getenv("WF2_CELL");

    if (sel == NULL || strcmp(sel, name) == 0)
    {
        return true;
    }

    printf("SKIP %s (WF2_CELL=%s)\n", name, sel);
    return false;
}

//------------------------------------------------------------------------
// Cells
//------------------------------------------------------------------------

// RED on pristine lib / GREEN on patched lib. See file header.
CTEST(svr_peerclose, garbage_close_feedback)
{
    if (cell_enabled("garbage_close_feedback") == false)
    {
        return;
    }

    alarm(60);

    b_mode = B_REPLY_GARBAGE;

    vws_svr* b       = vws_svr_new(1, 0, 0);
    b->process_ws    = b_process_ws;

    vrtql_msg_svr* a = vrtql_msg_svr_new(1, 0, 0);
    a->process       = a_process_msg;

    uv_thread_t b_tid;
    uv_thread_create(&b_tid, b_thread, b);
    ASSERT_TRUE(wait_state((vws_tcp_svr*)b, VS_RUNNING, 10000));

    uv_thread_t a_tid;
    uv_thread_create(&a_tid, a_thread, a);
    ASSERT_TRUE(wait_state((vws_tcp_svr*)a, VS_RUNNING, 10000));

    // Initial link-up.
    int64_t key0 = wait_peer_connected((vws_tcp_svr*)a, -1, 10000);
    ASSERT_TRUE(key0 >= 0);

    // One message out; B replies garbage; A's deserialize fails and the
    // CLOSE arm closes the peer's cnx.
    vws_peer* peer = (vws_peer*)((vws_tcp_svr*)a)->peers->array[0].value.data;
    send_trigger(a, peer->info.cid);

    // PRISTINE: ASan aborts here (sweep's freed read on the next wd tick,
    // server.c ~:1248) — the RED. The peer also never re-dials; that leg
    // is the mechanism's second face and is proven GREEN-side by the
    // key-change assert below.
    //
    // PATCHED: svr_peer_close_feedback marks the peer CLOSED before the
    // close; the reconnect branch re-dials; recovery = CONNECTED again
    // under a NEW cid.key.
    int64_t key1 = wait_peer_connected((vws_tcp_svr*)a, key0, 15000);
    ASSERT_TRUE(key1 >= 0);
    ASSERT_TRUE(key1 != key0);

    // Teardown (reached only when the lib is patched). Give CLOSE frames
    // time to flush (test_peering precedent).
    sleep(1);

    vws_tcp_svr_stop((vws_tcp_svr*)a);
    uv_thread_join(&a_tid);
    vrtql_msg_svr_free(a);

    vws_tcp_svr_stop((vws_tcp_svr*)b);
    uv_thread_join(&b_tid);
    vws_svr_free(b);

    alarm(0);
}

// CONTROL — non-vacuity sibling. Must pass on pristine AND patched: the
// EOF arm already feeds back, so the same fixture recovers. Isolates the
// RED to the CLOSE arm's missing feedback.
CTEST(svr_peerclose, control_eof_redial)
{
    if (cell_enabled("control_eof_redial") == false)
    {
        return;
    }

    alarm(60);

    b_mode = B_CLOSE_ABRUPT;

    vws_svr* b       = vws_svr_new(1, 0, 0);
    b->process_ws    = b_process_ws;

    vrtql_msg_svr* a = vrtql_msg_svr_new(1, 0, 0);
    a->process       = a_process_msg;

    uv_thread_t b_tid;
    uv_thread_create(&b_tid, b_thread, b);
    ASSERT_TRUE(wait_state((vws_tcp_svr*)b, VS_RUNNING, 10000));

    uv_thread_t a_tid;
    uv_thread_create(&a_tid, a_thread, a);
    ASSERT_TRUE(wait_state((vws_tcp_svr*)a, VS_RUNNING, 10000));

    int64_t key0 = wait_peer_connected((vws_tcp_svr*)a, -1, 10000);
    ASSERT_TRUE(key0 >= 0);

    // Trigger: B abruptly closes its side => A reads EOF => EOF arm's
    // feedback (present in pristine) => re-dial => recovery.
    vws_peer* peer = (vws_peer*)((vws_tcp_svr*)a)->peers->array[0].value.data;
    send_trigger(a, peer->info.cid);

    int64_t key1 = wait_peer_connected((vws_tcp_svr*)a, key0, 15000);
    ASSERT_TRUE(key1 >= 0);
    ASSERT_TRUE(key1 != key0);

    sleep(1);

    vws_tcp_svr_stop((vws_tcp_svr*)a);
    uv_thread_join(&a_tid);
    vrtql_msg_svr_free(a);

    vws_tcp_svr_stop((vws_tcp_svr*)b);
    uv_thread_join(&b_tid);
    vws_svr_free(b);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    cstr pa = getenv("WF2_PORT_A");
    cstr pb = getenv("WF2_PORT_B");

    if (pa == NULL || pb == NULL)
    {
        fprintf( stderr,
                 "WF2_PORT_A / WF2_PORT_B required (announced-slots-only; "
                 "no default ports). Refusing to run.\n" );
        return 2;
    }

    port_a = atoi(pa);
    port_b = atoi(pb);

    if (port_a <= 0 || port_b <= 0 || port_a == port_b)
    {
        fprintf(stderr, "Invalid ports A=%s B=%s\n", pa, pb);
        return 2;
    }

    snprintf(uri_b, sizeof(uri_b), "ws://%s:%d/websocket", host, port_b);

    vws.tracelevel = VT_THREAD;

    return ctest_main(argc, argv);
}
