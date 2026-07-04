// test_svr_reqleak.c — server.c shutdown-drain LEAK (requests queue).
//
// BUG (candidate): svr_shutdown() (server.c) drains the RESPONSES
// queue item-by-item (freeing each resident vws_svr_data) but then calls
// queue_destroy(&requests) WITHOUT draining the requests queue first.
// queue_destroy frees only the ring buffer array + name/mutex/cond — never the
// vws_svr_data items still resident in the ring. So any request still queued at
// shutdown (plus its owned payload) leaks.
//
// Why residual requests items are reachable at shutdown: worker threads pop the
// requests queue, but once vws_tcp_svr_stop() flips queue->state to VS_HALTING,
// queue_pop() returns NULL IMMEDIATELY (server.c queue_pop VS_HALTING arm) even
// when size>0 — so workers stop draining with items still in the ring. The
// uv_thread stop arm joins the workers and calls svr_shutdown(), which drains
// only responses. The requests remainder is dropped on the floor.
//
// This white-box RED calls the ACTUAL buggy function svr_shutdown() on a
// minimally-constructed server whose requests queue holds one item and whose
// responses queue is empty. Under LeakSanitizer/valgrind the resident request
// item (24-ish bytes struct + its payload) is reported leaked. A fixed
// svr_shutdown that drains requests symmetrically returns zero-leak.
//
// SUBJECT, NOT MODIFIED: server.c is #included so svr_shutdown/queue_* are the
// isolated-coverage copies. No server threads, no libuv reactor run — a pure
// unit around svr_shutdown, so no watchdog race surface. A short alarm() is
// armed anyway per lane discipline (no unbounded harness).

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"

#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

// server.c is the SUBJECT (statics + isolated coverage).
#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

// A request payload we can positively identify in a leak report: a distinct
// malloc'd block owned by the vws_svr_data (vws_svr_data_free frees ->data).
static ucstr make_payload(size_t n)
{
    ucstr p = (ucstr)vws.malloc(n);
    memset(p, 0xAB, n);
    return p;
}

CTEST(svr_shutdown, requests_residual_leak)
{
    alarm(WATCHDOG_SECONDS);

    // Minimal server: only the fields svr_shutdown touches must be live.
    //   - server->state  (read; must not be VS_HALTED or shutdown no-ops)
    //   - server->responses (drained: needs mutex + buffer + size)
    //   - server->requests  (queue_destroy'd WITHOUT drain — the bug)
    //   - server->loop      (uv_stop) — give it a real inited loop
    vws_tcp_svr* s = (vws_tcp_svr*)vws.malloc(sizeof(vws_tcp_svr));
    memset(s, 0, sizeof(vws_tcp_svr));

    s->state = VS_RUNNING;

    s->loop = (uv_loop_t*)vws.malloc(sizeof(uv_loop_t));
    ASSERT_TRUE(uv_loop_init(s->loop) == 0);

    queue_init(&s->requests,  8, "requests");
    queue_init(&s->responses, 8, "responses");

    // Queue ONE request that will still be resident at shutdown. Give it an
    // owned payload so the leak is two blocks (struct + data), unambiguous in
    // the sanitizer report.
    size_t plen = 64;
    ucstr payload = make_payload(plen);
    vws_cid_t cid; vws_cid_clear(&cid);
    vws_svr_data* req = vws_svr_data_own(s, cid, payload, plen);
    queue_push(&s->requests, req);

    ASSERT_TRUE(s->requests.size == 1);   // resident going into shutdown
    ASSERT_TRUE(s->responses.size == 0);

    // Drive the ACTUAL buggy path. svr_shutdown drains responses (empty here),
    // then queue_destroy(&requests) — dropping our resident req + payload.
    svr_shutdown(s);

    // After the bug: requests buffer freed, but the resident item is NOT.
    // The item pointer is now unreachable (buffer gone) => definite leak.
    // We cannot observe the leak from inside the process without a heap
    // accounting hook; LeakSanitizer/valgrind is the observer. The cell's
    // functional post-state is that shutdown completed and state advanced.
    ASSERT_TRUE(s->state == VS_HALTED || s->requests.buffer == NULL);

    // Tear down the loop + server shell we own (NOT the leaked request — that
    // is the bug's residue and must remain leaked for the RED to fire).
    uv_loop_close(s->loop);
    vws.free(s->loop);
    vws.free(s);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
