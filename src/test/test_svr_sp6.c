// test_svr_sp6.c — server.c teardown-safety regression (FIX-NOW #1 + #3).
//
// Two defects in vws_tcp_svr's connection teardown, surfaced by the wa Channel
// test-peer under a reconnect-with-inflight race and triaged FIX-NOW (ticket
// 7247dcc924):
//
//   #1  DOUBLE uv_close. A connection dropped while it has inflight data can be
//       reached by two cnx-teardown paths on ONE handle (svr_on_read EOF close
//       :2134, the uv_thread CLOSE-response close :938, peer_disconnect :1640,
//       the run() shutdown close :1305/:1329). libuv asserts !uv__is_closing on
//       the second uv_close and ABORTS the process -- a wire-reachable remote
//       abort/DoS. The fix routes every cnx-teardown close through
//       vws_tcp_svr_uv_close(), which now early-returns when uv_is_closing().
//
//   #3  svr_on_connect accept-fail FALL-THROUGH. On uv_accept != 0 the else arm
//       (server.c:2103-2106) uv_close()s the client handle but then falls
//       through to svr_cnx_new() + on_connect() on the just-closing handle --
//       a spurious connect (and a window where the cpool holds a cnx whose
//       handle is already closing). The fix adds the missing `return;`. The
//       symmetric read_start-fail arm already returns.
//
// TEST-FIRST / non-vacuous falsifier:
//   - suite `dblclose`   RED pre-fix = libuv aborts on the redundant close;
//                        GREEN post-fix = the wrapper guard makes it a no-op.
//   - suite `accept_fail` RED pre-fix = the fall-through fires cnx_open_cb once
//                        (spurious); GREEN post-fix = zero connect callbacks.
//   Run a single suite with `./test_svr_sp6 <suite>` (the dblclose RED aborts
//   the process, so isolate accept_fail to see its own RED).
//
// SERVER-PROTECTION (HARD): own-loop unit (no threads/network) for #1; a
// bounded self-killing live server with a wall-clock _Exit watchdog + STOP+JOIN
// before return for #3. No unbounded or backgrounded server EVER.
//
// #includes server.c so the subject's statics + vws_tcp_svr_uv_close are in
// scope and shadow static_lib's server.o (clean libvws/libuv stay
// non-instrumented), mirroring SP5.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "socket.h"

#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

//------------------------------------------------------------------------------
// One-shot libuv uv_accept fault injection (default pass-through).
//
// The arm flag is written on the TEST thread and read on the SERVER thread
// (svr_on_connect runs in the reactor), so it is C11 _Atomic -- a plain flag
// would be a cross-thread data race. "Fail the NEXT uv_accept, then disarm."
//------------------------------------------------------------------------------

extern int __real_uv_accept(uv_stream_t* server, uv_stream_t* client);

static _Atomic int g_uv_accept_fail = 0;

int __wrap_uv_accept(uv_stream_t* server, uv_stream_t* client)
{
    if (atomic_load(&g_uv_accept_fail))
    {
        atomic_store(&g_uv_accept_fail, 0);
        return UV_ECONNABORTED;
    }

    return __real_uv_accept(server, client);
}

// server.c is the SUBJECT (statics + isolated coverage).
#include "../server.c"

static const int WATCHDOG_SECONDS = 60;

static cstr server_host = "127.0.0.1";
static int  g_port = 18801;            // bumped per cell to dodge TIME_WAIT

//------------------------------------------------------------------------------
// #1 -- redundant close on one handle is a guarded no-op (not a libuv abort).
//
// A real loop-registered handle carrying the cinfo svr_on_close expects, closed
// TWICE through vws_tcp_svr_uv_close. This is the deterministic realization of
// the reconnect-with-inflight race: two cnx-teardown paths funnel one handle.
// cid stays invalid (vws_cid_clear) so svr_on_close skips the cnx path and just
// frees handle + cinfo. PRE-FIX the second uv_close aborts the process;
// reaching the end of the cell at all is the GREEN signal.
//------------------------------------------------------------------------------

CTEST(dblclose, redundant_close_is_guarded)
{
    uv_loop_t loop;
    ASSERT_EQUAL(0, uv_loop_init(&loop));

    // A server only so svr_on_close has a non-NULL ->server + a cpool to probe.
    vws_tcp_svr* s = vws_tcp_svr_new(1, 0, 0);
    ASSERT_TRUE(s != NULL);

    // Heap-allocated: svr_on_close frees handle->data (cinfo) + the handle.
    uv_tcp_t* h     = (uv_tcp_t*)vws.malloc(sizeof(uv_tcp_t));
    vws_cinfo* info = (vws_cinfo*)vws.malloc(sizeof(vws_cinfo));
    info->server    = s;
    info->cnx       = NULL;
    vws_cid_clear(&info->cid);          // invalid key -> svr_on_close skips cnx
    h->data         = info;
    ASSERT_EQUAL(0, uv_tcp_init(&loop, (uv_tcp_t*)h));

    // First teardown: schedules the close, marks the handle closing.
    vws_tcp_svr_uv_close(s, (uv_handle_t*)h);

    // Redundant teardown on the SAME already-closing handle.
    // PRE-FIX: uv_close asserts !uv__is_closing -> libuv aborts (RED).
    // POST-FIX: the uv_is_closing guard early-returns -> harmless no-op.
    vws_tcp_svr_uv_close(s, (uv_handle_t*)h);

    // Drain the single pending close (svr_on_close frees h + info).
    uv_run(&loop, UV_RUN_DEFAULT);

    // Reaching here is the GREEN signal -- pre-fix the cell aborted above.
    ASSERT_EQUAL(0, uv_loop_close(&loop));

    vws_tcp_svr_free(s);
}

//------------------------------------------------------------------------------
// #3 -- a failed accept must NOT spuriously open a connection.
//
// Bounded live server; arm uv_accept to fail the next accept; one connect-drop
// drives exactly one svr_on_connect whose uv_accept fails. cnx_open_cb counts
// connection opens. PRE-FIX the fall-through runs svr_cnx_new -> cnx_open_cb
// fires once (spurious); POST-FIX the else arm returns -> zero opens.
//
// We do NOT assert a follow-up echo: the connection failed AT uv_accept, so it
// stays pending in libuv's backlog and would absorb the next accept (inherent
// to this arm -- the same caveat SP5's accept_uv_tcp_init_fail cell notes).
// Server survival + zero spurious open is the proof.
//------------------------------------------------------------------------------

static _Atomic int g_cnx_opens = 0;

static bool count_cnx_open(vws_svr_cnx* cnx)
{
    (void)cnx;
    atomic_fetch_add(&g_cnx_opens, 1);
    return true;
}

static int g_run_port;

static void server_thread_fn(void* arg)
{
    vws_tcp_svr_run((vws_tcp_svr*)arg, server_host, g_run_port);
}

static void client_connect_drop(void)
{
    vws_socket* c = vws_socket_new();
    vws_socket_connect(c, server_host, g_run_port, false);
    vws_msleep(50);          // give the reactor a beat to run svr_on_connect
    vws_socket_free(c);
}

CTEST(accept_fail, no_spurious_connect)
{
    atomic_store(&g_cnx_opens, 0);

    g_run_port     = g_port++;
    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    s->cnx_open_cb = count_cnx_open;     // counts every connection open

    uv_thread_t tid;
    uv_thread_create(&tid, server_thread_fn, s);

    int spins = 0;
    while (s->state != VS_RUNNING && spins++ < 500)   // <= ~5s cap
    {
        vws_msleep(10);
    }
    ASSERT_EQUAL(VS_RUNNING, (int)s->state);

    atomic_store(&g_uv_accept_fail, 1);  // fail the NEXT (accept) uv_accept
    client_connect_drop();               // -> svr_on_connect else arm (:2103)

    // The reactor survived the failed accept.
    ASSERT_TRUE(vws_tcp_svr_is_running(s));

    // PRE-FIX: the fall-through opened a connection on the closing handle.
    // POST-FIX: the else arm returned -> no connection was opened.
    ASSERT_EQUAL(0, atomic_load(&g_cnx_opens));

    vws_tcp_svr_stop(s);
    uv_thread_join(&tid);
    vws_tcp_svr_free(s);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_sp6: watchdog deadline exceeded — abort\n");
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
