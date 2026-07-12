// test_svr_failstop.c — a failed-start server's stop() must be prompt and
// must join its workers.
//
// DEFECT (pristine): vws_tcp_svr_run spawns the worker pool BEFORE bind. On
// a bind/listen failure the server goes VS_FAILED and run() returns; the uv
// loop never ran, so uv_thread's halting arm — the only setter of VS_HALTED
// on the stop path and the only worker-join site in the tree — can never
// fire. vws_tcp_svr_stop then burns its FULL 15s drain budget waiting for a
// state no live thread can set, logs the "drain did not reach VS_HALTED"
// error, and returns WITHOUT ever joining the workers: the 15s burn is the
// only thing that de-facto keeps a worker from still being inside
// queue_pop when the owner destroys the queue mutex/cond right after.
//
// FIX under test: a VS_FAILED-at-entry fast path in vws_tcp_svr_stop —
// broadcast as today, then uv_thread_join over the pool (deterministic),
// set VS_HALTED, return. This cell asserts the FIXED contract, so it is
// the deterministic RED on pristine (fails at the elapsed bound; the log
// shows the ~15s burn + the drain error) and GREEN on the fixed source
// (sub-second, VS_HALTED, free-after-stop safe because the workers are
// joined). No timing race: the pristine burn is a hardcoded budget.
//
// The port conflict is self-contained: the cell binds an ephemeral port
// with a plain listener and starts the server on that same port — no
// external port coordination needed.
//
// Build: standalone against the built libvws (same recipe as the other
// server cells). A TSan build of this same TU certifies the join leg
// (workers joined before vws_svr_free destroys the queues).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "server.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

static cstr host = "127.0.0.1";

// Occupy an ephemeral port with a plain listener; returns the fd and writes
// the port number.
static int occupy_port(int* port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    ASSERT_TRUE(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    ASSERT_TRUE(listen(fd, 1) == 0);

    socklen_t len = sizeof(addr);
    ASSERT_TRUE(getsockname(fd, (struct sockaddr*)&addr, &len) == 0);

    *port_out = ntohs(addr.sin_port);
    return fd;
}

CTEST(svr_failstop, failed_start_stop_prompt_and_joined)
{
    alarm(60);

    int port    = 0;
    int occupier = occupy_port(&port);

    // Two workers so the join leg exercises a multi-thread pool.
    vws_svr* s = vws_svr_new(2, 0, 0);

    // Bind conflict -> VS_FAILED after the pool was spawned; run() returns.
    uint64_t t_run = vws_now_ms();
    int rc         = vws_svr_run(s, host, port);
    ASSERT_TRUE(rc != 0);
    ASSERT_TRUE(vws_tcp_svr_state((vws_tcp_svr*)s) == VS_FAILED);
    ASSERT_TRUE((vws_now_ms() - t_run) < 5000);

    //> The contract under test: stop() on a failed-start server returns
    // promptly (deterministic worker join, not the 15s drain burn) and
    // reaches VS_HALTED. Pristine fails HERE (elapsed ~15000ms and state
    // never HALTED); fixed passes sub-second.
    uint64_t t0 = vws_now_ms();
    vws_tcp_svr_stop((vws_tcp_svr*)s);
    uint64_t elapsed = vws_now_ms() - t0;

    printf("failstop: stop() elapsed=%llums\n", (unsigned long long)elapsed);

    ASSERT_TRUE(elapsed < 2000);
    ASSERT_TRUE(vws_tcp_svr_state((vws_tcp_svr*)s) == VS_HALTED);

    //> Safe only because the workers are joined: free destroys the queue
    // mutex/cond the workers were woken through. Under the TSan build this
    // certifies the join leg.
    vws_svr_free(s);

    close(occupier);
    alarm(0);
}

int main(int argc, const char* argv[])
{
    vws.tracelevel = VT_THREAD;

    return ctest_main(argc, argv);
}
