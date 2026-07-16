// test_svr_heartbeat_pong.c — SV1 regression pin.
//
// BUG (pre-fix): last_active was bumped and ping_outstanding cleared ONLY in
// websocket.c's CLIENT process_frame. Server connections — including PEER links,
// the exact connections the uv_thread heartbeat sweep monitors — are handled by
// ws_svr_process_frame, whose PONG arm just freed the frame and whose other arms
// never touched last_active. On the defaults (ping_interval_ms=10000,
// pong_deadline_ms=20000) the sweep sent an idle-PING at 10s, the peer PONGed,
// ping_outstanding STAYED set, and 20s later the sweep declared the peer frozen
// and closed it — so every HEALTHY peer link churned about every 30s.
//
// FIX (server.c ws_svr_process_frame): bump c->last_active on ANY inbound frame
// and clear c->ping_outstanding in the PONG arm (mirroring the client handler).
//
// This is a WHITE-BOX pin, not a live two-broker soak: the sweep's close DECISION
// (server.c:1338-1340) was always correct; the defect was entirely that the
// server frame handler never updated the two fields it reads. So driving a PONG
// through ws_svr_process_frame and asserting (1) the field updates and (2) that
// the production close predicate then evaluates false is the direct falsifier —
// deterministic, no network/timing flake.
//
//   Cell 1 (fields): a PONG clears ping_outstanding and bumps last_active.
//                    RED pre-fix: neither happens.
//   Cell 2 (behavior): after the PONG the sweep's own close predicate is FALSE,
//                    with a non-vacuous control proving the age term IS exceeded
//                    (so ping_outstanding is the only discriminator).
//
// SUBJECT, not modified: server.c is #included so ws_svr_process_frame is the
// isolated copy. SELF-BOUNDING: wall-clock _Exit watchdog.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "websocket.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

// Minimal server-side cnx. ws_svr_process_frame's PONG path touches only the
// vws_cnx and its ->data (a vws_svr_cnx); it never derefs cnx->server, so a
// zeroed stack svr_cnx suffices (mirrors test_svr_msgflood::make_server_cnx).
static vws_cnx* make_server_cnx(vws_svr_cnx* svr_cnx)
{
    memset(svr_cnx, 0, sizeof(*svr_cnx));
    vws_cnx* c = vws_cnx_new();
    c->process = ws_svr_process_frame;
    c->data    = (char*)svr_cnx;
    return c;
}

//------------------------------------------------------------------------------
// (1) A PONG clears ping_outstanding and bumps last_active.
//------------------------------------------------------------------------------

CTEST(svr_hb_pong, pong_clears_outstanding_and_bumps_active)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr_cnx svr_cnx;
    vws_cnx* c = make_server_cnx(&svr_cnx);

    // The state the sweep leaves after it sends an idle-PING: outstanding set,
    // last_active/ping_sent_ts stamped in the past (1 ms; the monotonic uptime
    // clock reads far above 1).
    c->ping_outstanding = true;
    c->ping_sent_ts     = 1;
    c->last_active      = 1;

    // The peer answers with a PONG. Server-side frames go through
    // ws_svr_process_frame (NOT websocket.c's client process_frame).
    ws_svr_process_frame(c, vws_frame_new(NULL, 0, PONG_FRAME));   // frees the frame

    // [SV1] PONG arm clears the flag; the top of the handler bumps last_active on
    // every inbound frame. Pre-fix neither happened on the server side.
    ASSERT_TRUE(c->ping_outstanding == false);   // RED pre-fix (stayed true)
    ASSERT_TRUE(c->last_active > 1);              // RED pre-fix (never bumped)

    vws_cnx_free(c);

    alarm(0);
}

//------------------------------------------------------------------------------
// (2) After the PONG, the production close predicate leaves the peer CONNECTED.
//------------------------------------------------------------------------------

CTEST(svr_hb_pong, healthy_peer_survives_sweep_after_pong)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr_cnx svr_cnx;
    vws_cnx* c = make_server_cnx(&svr_cnx);

    const uint64_t pong_deadline_ms = 100;

    // The sweep sent an idle-PING whose age ALREADY exceeds the deadline: the
    // only thing that can keep this peer alive is the PONG clearing the flag.
    uint64_t now = vws_now_ms();
    c->ping_outstanding = true;
    c->ping_sent_ts     = now - (pong_deadline_ms + 500);
    c->last_active      = now - (pong_deadline_ms + 500);

    ws_svr_process_frame(c, vws_frame_new(NULL, 0, PONG_FRAME));

    // Mirror of the production close predicate (server.c:1338-1340). With the fix
    // ping_outstanding is now false, so the sweep leaves the healthy peer
    // CONNECTED. Pre-fix it stayed true AND the age exceeds the deadline, so the
    // sweep closed a healthy link (the ~30s flap).
    uint64_t now2 = vws_now_ms();
    bool would_close = (pong_deadline_ms > 0) &&
                       c->ping_outstanding &&
                       (now2 - c->ping_sent_ts) > pong_deadline_ms;

    ASSERT_TRUE(would_close == false);   // RED pre-fix

    // Non-vacuous control: had the PONG NOT cleared the flag, the SAME predicate
    // trips -- proving the assertion above passes because of the cleared flag,
    // not because the age term happened to be under the deadline.
    c->ping_outstanding = true;
    uint64_t now3 = vws_now_ms();
    bool would_close_if_unanswered = (pong_deadline_ms > 0) &&
                                     c->ping_outstanding &&
                                     (now3 - c->ping_sent_ts) > pong_deadline_ms;

    ASSERT_TRUE(would_close_if_unanswered == true);

    vws_cnx_free(c);

    alarm(0);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_heartbeat_pong: watchdog deadline exceeded — abort\n");
    _Exit(99);
    return NULL;
}

int main(int argc, const char* argv[])
{
    pthread_t watch;
    pthread_create(&watch, NULL, watchdog_thread, NULL);
    pthread_detach(watch);

    return ctest_main(argc, argv);
}
