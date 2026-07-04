// test_svr_bindfail.c — server.c bind/listen-failure exposes NO failure
// state.
//
// BUG: vws_tcp_svr_run sets server->state = VS_RUNNING only at server.c:1298,
// AFTER a successful bind+listen. On uv_tcp_bind failure it `return -1`
// (server.c:1275) and on uv_listen failure `return -1` (server.c:1285) WITHOUT
// setting any failure state. The enum (server.h) has only VS_RUNNING(0) /
// VS_HALTING(1) / VS_HALTED(2) — no VS_FAILED — and the ctor initializes
// state = VS_HALTED. So after a failed run() the state is still VS_HALTED,
// INDISTINGUISHABLE from a server that simply hasn't started. vws_tcp_svr_state
// gives a caller no way to tell "bind failed" from "not started yet".
//
// Consumer manifestation: vsa Broker::start polls
//   while (!vws_tcp_svr_is_running(svr)) millisleep;   (broker.cpp:830)
// is_running is true ONLY for VS_RUNNING, so on bind failure it spins forever
// (signals masked -> only SIGKILL). This RED is the vws half: prove run()
// returns -1 synchronously yet exposes no failure signal.
//
// RED SHAPE (pure-vws, PG-INDEPENDENT, bounded): hold a port with a raw
// listening socket, then vws_tcp_svr_run(server, host, sameport) fails the bind
// and returns -1 BEFORE the uv_run loop. Assert the post-fail state is a
// distinct failure value (VS_FAILED == 3). On the buggy build state stays
// VS_HALTED(2) == the ctor value the test captures up front -> assertion fires
// (RED). On the fixed build (VS_FAILED set before both return -1) it is 3 (GREEN)
// and a bounded poller can break.
//
// SELF-BOUNDING: run() spawns the worker pool BEFORE the bind (server.c:1242),
// so the failed run leaves workers blocked; the cell release/joins them
// (mirrors SP5). Watchdog alarm. No unbounded/backgrounded server. (State-only
// assertion test — NOT run under LSan; the bind-fail listen-socket/ci resource
// leak is a separate out-of-scope observation.)

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"

#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// server.c is the SUBJECT.
#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

// Enum values are literals here so the ONE test compiles against BOTH the buggy
// source (no VS_FAILED symbol yet) and the fixed source. Post-fix VS_FAILED==3.
#define ST_HALTED 2
#define ST_FAILED 3

// Hold a port: raw socket bound + listening on 127.0.0.1:<ephemeral>. An active
// LISTEN blocks a later uv_tcp_bind (even with libuv's default SO_REUSEADDR) with
// EADDRINUSE. Returns the fd; writes the chosen port to *port.
static int hold_port(int* port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;                        // ephemeral
    bind(fd, (struct sockaddr*)&a, sizeof(a));
    listen(fd, 1);
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr*)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

// run() spawned the worker pool before the failed bind. Release + join them —
// bounded teardown, no orphan threads (mirrors SP5's release_and_join_workers).
static void release_and_join_workers(vws_tcp_svr* s)
{
    s->state = VS_HALTING;
    uv_mutex_lock(&s->requests.mutex);
    s->requests.state = VS_HALTING;
    uv_cond_broadcast(&s->requests.cond);
    uv_mutex_unlock(&s->requests.mutex);
    uv_mutex_lock(&s->responses.mutex);
    s->responses.state = VS_HALTING;
    uv_cond_broadcast(&s->responses.cond);
    uv_mutex_unlock(&s->responses.mutex);
    for (int i = 0; i < s->pool_size; i++)
    {
        uv_thread_join(&s->threads[i]);
    }
}

CTEST(svr_bindfail, no_failure_state)
{
    alarm(WATCHDOG_SECONDS);

    int port = 0;
    int hold  = hold_port(&port);
    ASSERT_TRUE(hold >= 0);
    ASSERT_TRUE(port > 0);

    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    ASSERT_TRUE(s != NULL);

    // Confirm the ctor initial state the bug leaves untouched on failure.
    uint8_t s0 = vws_tcp_svr_state(s);
    ASSERT_EQUAL(ST_HALTED, s0);

    // Synchronous failure: uv_tcp_bind hits EADDRINUSE -> return -1 (server.c
    // :1275), BEFORE the uv_run loop. No blocking on the caller.
    int rc = vws_tcp_svr_run(s, "127.0.0.1", port);
    ASSERT_EQUAL(-1, rc);

    // THE ASSERTION. Buggy: state still ST_HALTED(2) == s0 -> no failure signal
    // -> 2 != 3 fires (RED). Fixed: ST_FAILED(3) -> passes (GREEN), and a bounded
    // is_running/state poller can break instead of spinning forever.
    uint8_t s1 = vws_tcp_svr_state(s);
    ASSERT_EQUAL(ST_FAILED, s1);

    // Bounded teardown of the worker pool the failed run left running.
    release_and_join_workers(s);
    close(hold);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
