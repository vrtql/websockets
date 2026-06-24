// ---------------------------------------------------------------------------
// test_stop_wedge.c — pure-vws white-box repro for a57b5e8ad7 (cea2369900 #4).
//
// Tests vws_tcp_svr_stop() at its OWN layer (no libvsa => no #5 UAF).
//
// MECHANISM (verified in server.c): on shutdown, uv_thread() (the loop's async
// callback) sees VS_HALTING and JOINS all worker threads (server.c:912-917)
// BEFORE calling svr_shutdown()->uv_stop(). So if a worker thread never exits,
// that uv_thread_join() hangs => the loop thread is stuck inside uv_thread =>
// uv_run() never returns => vws_tcp_svr_run() never sets VS_HALTED (server.c:
// 1350) => vws_tcp_svr_stop()'s drain `while (state != VS_HALTED)` spins.
//
// SYNTHETIC-DRIFT INJECTION (white-box falsification): install an on_data_in
// handler that BLOCKS forever, then send it one request so a worker enters it
// and never exits — exactly the never-reach-VS_HALTED condition the fix guards.
//   * pre-fix  (current libvws): vws_tcp_svr_stop() spins unbounded -> the
//     wall-clock WATCHDOG fires -> RED (exit 2).
//   * post-fix (rebuilt libvws): the bounded ~15s drain returns + trace-warns
//     -> GREEN (exit 0), deterministically, even though the worker is stuck.
//
// The stuck worker + server threads are reaped by _Exit() at the end; the test
// never hangs (watchdog backstop).
//
// Build (from following/vws/src):
//   cc -g test/test_stop_wedge.c -I. -L. -lvws -luv -lssl -lcrypto -lpthread \
//      -o test/test_stop_wedge
// Run:  ./test/test_stop_wedge
// ---------------------------------------------------------------------------

#include "server.h"
#include "socket.h"

#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char* g_host  = "127.0.0.1";
static int         g_port  = 8231;
static volatile int g_block = 1;   // worker blocks while set (never cleared)

static void on_alarm(int sig)
{
    (void)sig;
    const char* m =
        "[FAIL] a57b5e8ad7: vws_tcp_svr_stop() WEDGED (unbounded drain did not "
        "return within the watchdog budget)\n";
    write(STDERR_FILENO, m, 90);
    _Exit(2);
}

// Worker-thread handler that NEVER returns -> on shutdown its uv_thread_join()
// hangs -> VS_HALTED is never reached -> the drain spins (pre-fix).
static void process_block(vws_svr_data* req, void* ctx)
{
    (void)ctx;
    vws_svr_data_free(req);

    while (g_block)        // never released: this worker is stuck forever
    {
        vws_msleep(100);
    }
}

static void server_thread(void* arg)
{
    vws_tcp_svr* server = (vws_tcp_svr*)arg;
    vws_tcp_svr_run(server, g_host, g_port);
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    // One worker so the single stuck worker deterministically hangs the join.
    vws_tcp_svr* server = vws_tcp_svr_new(1, 0, 0);
    server->on_data_in  = process_block;

    uv_thread_t tid;
    uv_thread_create(&tid, server_thread, server);
    while (server->state != VS_RUNNING)
    {
        vws_msleep(50);
    }

    // Send one request so the (only) worker enters process_block and gets stuck.
    vws_socket* s = vws_socket_new();
    if (vws_socket_connect(s, g_host, g_port, false) == false)
    {
        fprintf(stderr, "connect failed\n");
        return 3;
    }

    const char* req = "stick-the-worker";
    vws_socket_write(s, (ucstr)req, strlen(req));
    vws_msleep(500);          // ensure the worker has entered process_block
    vws_socket_free(s);

    // --- gated call: vws_tcp_svr_stop() must return BOUNDED despite the stuck
    //     worker (the loop thread will hang in uv_thread_join; the FIX makes the
    //     caller's drain return anyway). ----------------------------------------
    signal(SIGALRM, on_alarm);
    alarm(25);                // > the 15s fix budget, < an unbounded spin

    fprintf(stderr, "a57b5e8ad7 white-box repro: stuck worker injected; "
                    "calling vws_tcp_svr_stop() (watchdog=25s)...\n");

    time_t t0 = time(NULL);
    vws_tcp_svr_stop(server); // pre-fix: spins forever -> watchdog RED
    long elapsed = (long)(time(NULL) - t0);

    alarm(0);

    // Do NOT join the server thread: it is stuck in uv_thread_join(worker). The
    // point is that the CALLER (vws_tcp_svr_stop) returned bounded. _Exit reaps
    // the stuck threads.
    fprintf(stderr,
            "[PASS] a57b5e8ad7: vws_tcp_svr_stop() returned BOUNDED in ~%lds "
            "despite a stuck worker\n", elapsed);

    _Exit(0);
}
