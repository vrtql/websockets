// test_svr_heartbeat_monotonic.c — the heartbeat/liveness deadlines are measured
// with a MONOTONIC clock (vws_now_ms, uv_hrtime based), not wall-clock time(),
// so an NTP/wall-clock step cannot spuriously fire a peer-unrecoverable report
// (a false maintenance alarm + disconnect of a live peer) or defer detection of
// a real dead peer.
//
// Cells:
//   (1) now_ms_is_monotonic — vws_now_ms() advances forward at MILLISECOND
//       resolution and never backward. A wall-clock time() in seconds would read
//       identically across a 25ms sleep (delta 0); the ms delta proves the
//       production source is a monotonic ms clock, not second-granular wall time.
//   (2) wallclock_jump_false_fires (RED / defect) — via the vws_now_ms_fn seam,
//       inject a clock that STEPS FORWARD like an NTP correction shortly after
//       the down-anchor is stamped. A peer unreachable for only a moment is then
//       FALSELY reported unrecoverable. This is exactly what wall-clock time()
//       did: a forward step makes a young peer look old.
//   (3) monotonic_no_false_fire (GREEN / fix) — with the real monotonic clock
//       and a large threshold, a briefly-down peer is NOT reported: monotonic
//       elapsed is the real (small) elapsed, immune to the step in cell (2).
//   (4) genuine_unreachable_fires (CONTROL, non-tautology) — real monotonic
//       clock, a genuinely unreachable peer past a 300ms threshold IS reported.
//       300ms is sub-second: the fix's ms resolution can express it, the former
//       time() seconds clock could not. Proves the fix preserves real detection.
//
// SERVER-PROTECTION (HARD): bounded spins (capped) + wall-clock _Exit watchdog +
// STOP+JOIN the server thread each cell. server.c is the SUBJECT (#included).

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "socket.h"
#include "websocket.h"

#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <time.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 60;
static cstr server_host = "127.0.0.1";
static int  g_port      = 18701;
static int  g_run_port;

// Unrecoverable-peer report capture.
static _Atomic int g_unrecoverable_count = 0;

static void capture_unrecoverable(vws_tcp_svr* server, vws_cid_t cid)
{
    (void)server; (void)cid;
    atomic_fetch_add(&g_unrecoverable_count, 1);
}

// Injectable fake clock (the vws_now_ms_fn seam). g_fake_ms is the value the
// heartbeat sees as "now"; the test steps it to simulate a wall-clock jump.
static _Atomic uint64_t g_fake_ms = 0;
static uint64_t fake_now_ms(void) { return atomic_load(&g_fake_ms); }

//------------------------------------------------------------------------------
// Bounded live-server harness (config set BEFORE the reactor thread starts).
//------------------------------------------------------------------------------

static void server_thread_fn(void* arg)
{
    vws_tcp_svr* s = (vws_tcp_svr*)arg;
    vws_tcp_svr_run(s, server_host, g_run_port);
}

static vws_svr* make_server(void)
{
    atomic_store(&g_unrecoverable_count, 0);
    return vws_svr_new(2, 0, 0);
}

static void launch(vws_svr* ws, uv_thread_t* tid)
{
    g_run_port = g_port++;
    uv_thread_create(tid, server_thread_fn, (vws_tcp_svr*)ws);
    int spins = 0;
    while (((vws_tcp_svr*)ws)->state != VS_RUNNING && spins++ < 500)
    {
        vws_msleep(10);
    }
}

static void stop_server(vws_svr* ws, uv_thread_t* tid)
{
    vws_tcp_svr_stop((vws_tcp_svr*)ws);
    uv_thread_join(tid);
    vws_svr_free(ws);
}

// peer_add requires a non-NULL connect fn, but svr_resolve() connect()s to the
// peer address FIRST; a closed port refuses fast, so the fn never runs and the
// peer stays in the CLOSED down-retry branch (never reachable).
static int never_connects(vws_peer* p, void* x) { (void)p; (void)x; return -1; }

// Reserve then release a loopback port so nothing listens: connect() is refused
// fast and deterministically.
static int reserve_closed_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    socklen_t alen = sizeof(a);
    getsockname(fd, (struct sockaddr*)&a, &alen);
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

//------------------------------------------------------------------------------
// (1) The production clock is monotonic milliseconds.
//------------------------------------------------------------------------------

CTEST(monotonic, now_ms_is_monotonic)
{
    vws_now_ms_fn = NULL;   // real clock

    uint64_t a = vws_now_ms();
    vws_msleep(25);
    uint64_t b = vws_now_ms();

    ASSERT_TRUE(b >= a);                 // never moves backward
    ASSERT_TRUE((b - a) >= 10);          // ms resolution: seconds time() -> 0 here
    ASSERT_TRUE((b - a) < 5000);         // sane upper bound

    // The source is a monotonic (uptime) origin, NOT wall-clock: uptime ms is
    // far smaller than epoch ms. Wall-clock time()*1000 would be ~1.7e12; a
    // monotonic clock is orders of magnitude below that. This directly rejects
    // a wall-clock (NTP-steppable) source, not just a coarse one.
    ASSERT_TRUE(b < ((uint64_t)time(NULL) * 1000) / 2);

    // Never-decreasing across a tight loop.
    uint64_t prev = vws_now_ms();
    for (int i = 0; i < 1000; i++)
    {
        uint64_t cur = vws_now_ms();
        ASSERT_TRUE(cur >= prev);
        prev = cur;
    }
}

//------------------------------------------------------------------------------
// (2) RED: a stepping (wall-clock-like) clock causes a FALSE unrecoverable fire.
//------------------------------------------------------------------------------

CTEST(monotonic, wallclock_jump_false_fires)
{
    int dead_port = reserve_closed_port();
    ASSERT_TRUE(dead_port > 0);

    atomic_store(&g_fake_ms, 1000);
    vws_now_ms_fn = fake_now_ms;         // heartbeat now reads the fake clock

    vws_svr* ws = make_server();
    ws->base.ping_interval_ms      = 0;
    ws->base.pong_deadline_ms      = 0;
    ws->base.peer_unrecoverable_ms = 5000;   // large: only a JUMP exceeds it soon
    ws->base.peer_unrecoverable_cb = capture_unrecoverable;

    vws_peer* p = vws_tcp_svr_peer_add((vws_tcp_svr*)ws, "127.0.0.1", dead_port,
                                       never_connects, NULL);
    ASSERT_NOT_NULL(p);
    uv_thread_t tid;
    launch(ws, &tid);

    // Wait until the down-anchor is stamped (at fake now == 1000), so the jump
    // lands AFTER the anchor rather than becoming the anchor.
    int spins = 0;
    while (p->first_unreachable_ts == 0 && spins++ < 300) vws_msleep(20);
    ASSERT_TRUE(p->first_unreachable_ts != 0);

    // NTP forward step: wall-clock leaps 100s though barely any real time passed.
    atomic_store(&g_fake_ms, 1000 + 100000);

    // The stepped elapsed (100000 > 5000) trips the deadline -> a FALSE report.
    spins = 0;
    while (atomic_load(&g_unrecoverable_count) < 1 && spins++ < 300) vws_msleep(20);
    ASSERT_TRUE(atomic_load(&g_unrecoverable_count) == 1);

    vws_tcp_svr_peer_remove((vws_tcp_svr*)ws, "127.0.0.1", dead_port);
    stop_server(ws, &tid);
    vws_now_ms_fn = NULL;
}

//------------------------------------------------------------------------------
// (3) GREEN: the real monotonic clock does NOT false-fire a briefly-down peer.
//------------------------------------------------------------------------------

CTEST(monotonic, monotonic_no_false_fire)
{
    int dead_port = reserve_closed_port();
    ASSERT_TRUE(dead_port > 0);

    vws_now_ms_fn = NULL;                 // real monotonic clock

    vws_svr* ws = make_server();
    ws->base.ping_interval_ms      = 0;
    ws->base.pong_deadline_ms      = 0;
    ws->base.peer_unrecoverable_ms = 60000;  // 60s: no real test wait approaches it
    ws->base.peer_unrecoverable_cb = capture_unrecoverable;

    ASSERT_NOT_NULL(vws_tcp_svr_peer_add((vws_tcp_svr*)ws, "127.0.0.1", dead_port,
                                         never_connects, NULL));
    uv_thread_t tid;
    launch(ws, &tid);

    // The peer is down the whole time, but only for ~0.8s of REAL (monotonic)
    // time, far under 60s -> no report. (Under wall-clock, an NTP step could
    // still have tripped it; monotonic cannot be stepped.)
    vws_msleep(800);
    ASSERT_TRUE(atomic_load(&g_unrecoverable_count) == 0);

    vws_tcp_svr_peer_remove((vws_tcp_svr*)ws, "127.0.0.1", dead_port);
    stop_server(ws, &tid);
}

//------------------------------------------------------------------------------
// (4) CONTROL: a genuinely-unreachable peer past a sub-second threshold fires.
//------------------------------------------------------------------------------

CTEST(monotonic, genuine_unreachable_fires)
{
    int dead_port = reserve_closed_port();
    ASSERT_TRUE(dead_port > 0);

    vws_now_ms_fn = NULL;                 // real monotonic clock

    vws_svr* ws = make_server();
    ws->base.ping_interval_ms      = 0;
    ws->base.pong_deadline_ms      = 0;
    ws->base.peer_unrecoverable_ms = 300;    // 300ms: sub-second, ms-only precision
    ws->base.peer_unrecoverable_cb = capture_unrecoverable;

    ASSERT_NOT_NULL(vws_tcp_svr_peer_add((vws_tcp_svr*)ws, "127.0.0.1", dead_port,
                                         never_connects, NULL));
    uv_thread_t tid;
    launch(ws, &tid);

    // Genuinely down > 300ms of REAL time -> reported exactly once.
    int spins = 0;
    while (atomic_load(&g_unrecoverable_count) < 1 && spins++ < 300) vws_msleep(20);
    ASSERT_TRUE(atomic_load(&g_unrecoverable_count) == 1);

    // Fires ONCE per span.
    vws_msleep(400);
    ASSERT_TRUE(atomic_load(&g_unrecoverable_count) == 1);

    vws_tcp_svr_peer_remove((vws_tcp_svr*)ws, "127.0.0.1", dead_port);
    stop_server(ws, &tid);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_heartbeat_monotonic: watchdog deadline exceeded — abort\n");
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
