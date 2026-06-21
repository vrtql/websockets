// test_svr_sp5.c — server.c coverage, SUB-PHASE 5 (deferred error/leak arms).
//
// The SP4-leftover CONTROLLED-CONCURRENCY --wrap coverage arms that SP1 left
// deferred (its header promised "the --wrap alloc/uv-fail arms" but only the
// calloc scaffold shipped). These drive the libuv-failure error paths in
// svr_on_connect (accept) + vws_tcp_svr_inetd_run (adoption) + the queue
// shutdown-drain path, via one-shot ld --wrap on the libuv primitives over a
// BOUNDED self-killing live server (+ a pure queue unit for the drain arm).
//
// SUBJECT, NOT MODIFIED: every cell drives an UNMODIFIED server.c error path.
// Several of these paths LEAK or mishandle teardown on the SUBJECT side -- those
// are CANDIDATES, surfaced here for the audit + reachability-tag, NOT fixed. The
// cells assert the OBSERVABLE functional outcome (server survives / inetd_run
// returns 1 / the blocked pusher unblocks); the leaks they surface are reported
// to the planner as candidates (see the cell comments for the audit-why).
//
// SERVER-PROTECTION (HARD): bounded clients, a capped start-up spin, a
// wall-clock _Exit watchdog that fires even on a thread hang, and STOP+JOIN of
// every server/worker thread before each cell returns -- NO unbounded or
// backgrounded server EVER.
//
// #includes server.c (statics + isolated coverage) so the #included symbols
// shadow static_lib's server.o; clean libvws/libuv stay non-instrumented.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "socket.h"

#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>

//------------------------------------------------------------------------------
// One-shot libuv fault injection (default pass-through).
//
// The arm flags are written on the TEST thread and read on the SERVER thread
// (svr_on_connect runs in the reactor), so they are C11 _Atomic -- a plain
// flag here would be a cross-thread data race (and this harness is a candidate
// to run under TSan alongside SP4). "Fail the NEXT call, then disarm" -- armed
// AFTER the listen/adopt path so only the targeted accept/adopt call fails.
//------------------------------------------------------------------------------

extern int __real_uv_tcp_init(uv_loop_t* loop, uv_tcp_t* handle);
extern int __real_uv_read_start(uv_stream_t* stream, uv_alloc_cb alloc_cb,
                                uv_read_cb read_cb);

static _Atomic int g_uv_tcp_init_fail   = 0;
static _Atomic int g_uv_read_start_fail = 0;

int __wrap_uv_tcp_init(uv_loop_t* loop, uv_tcp_t* handle)
{
    if (atomic_load(&g_uv_tcp_init_fail))
    {
        atomic_store(&g_uv_tcp_init_fail, 0);
        return UV_ENOMEM;
    }
    return __real_uv_tcp_init(loop, handle);
}

int __wrap_uv_read_start(uv_stream_t* stream, uv_alloc_cb alloc_cb,
                         uv_read_cb read_cb)
{
    if (atomic_load(&g_uv_read_start_fail))
    {
        atomic_store(&g_uv_read_start_fail, 0);
        return UV_EINVAL;
    }
    return __real_uv_read_start(stream, alloc_cb, read_cb);
}

// server.c is the SUBJECT (statics + isolated coverage).
#include "../server.c"

static const int WATCHDOG_SECONDS = 60;

static cstr server_host = "127.0.0.1";
static int  g_port = 18701;            // bumped per cell to dodge TIME_WAIT

//------------------------------------------------------------------------------
// Bounded live-server harness (mirrors SP1)
//------------------------------------------------------------------------------

static void process_echo(vws_svr_data* req, void* ctx)
{
    (void)ctx;
    char* data = (char*)vws.malloc(req->size);
    memcpy(data, req->data, req->size);
    vws_svr_data* reply = vws_svr_data_own(req->server, req->cid,
                                           (ucstr)data, req->size);
    vws_svr_data_free(req);
    vws_tcp_svr_send(reply);
}

static int g_run_port;
static void server_thread_fn(void* arg)
{
    vws_tcp_svr_run((vws_tcp_svr*)arg, server_host, g_run_port);
}

static vws_tcp_svr* start_server(uv_thread_t* tid, int pool)
{
    g_run_port = g_port++;
    vws_tcp_svr* s = vws_tcp_svr_new(pool, 0, 0);
    s->on_data_in = process_echo;
    uv_thread_create(tid, server_thread_fn, s);
    int spins = 0;
    while (s->state != VS_RUNNING && spins++ < 500)   // <= ~5s cap
    {
        vws_msleep(10);
    }
    return s;
}

static void stop_server(vws_tcp_svr* s, uv_thread_t* tid)
{
    vws_tcp_svr_stop(s);
    uv_thread_join(tid);
    vws_tcp_svr_free(s);
}

static bool client_echo(cstr msg)
{
    vws_socket* c = vws_socket_new();
    if (vws_socket_connect(c, server_host, g_run_port, false) == false)
    {
        vws_socket_free(c);
        return false;
    }
    vws_socket_write(c, (ucstr)msg, strlen(msg));
    ssize_t n = vws_socket_read(c);
    vws_socket_free(c);
    return n > 0;
}

// A bare connect+drop: drives one accept (and one svr_on_connect) without
// expecting an echo -- used when the accept path is armed to fail.
static void client_connect_drop(void)
{
    vws_socket* c = vws_socket_new();
    vws_socket_connect(c, server_host, g_run_port, false);
    vws_msleep(50);          // give the reactor a beat to run svr_on_connect
    vws_socket_free(c);
}

//------------------------------------------------------------------------------
// C-SVR-2: svr_on_connect accept path -- uv_tcp_init failure (server.c:2127).
//
// On uv_tcp_init!=0 svr_on_connect returns early WITHOUT freeing the just-
// allocated client handle `c` (:2114) or `cinfo` (:2122) -> both LEAK. The
// reactor stays alive and keeps serving. COVERAGE of the :2127-2131 arm; the
// leak is the candidate (accept path = untrusted-connection-reachable under fd
// pressure -> flagged for the planner's fix-now/batch call).
//------------------------------------------------------------------------------

CTEST(svr_err, accept_uv_tcp_init_fail)
{
    uv_thread_t tid;
    vws_tcp_svr* s = start_server(&tid, 2);
    ASSERT_EQUAL(VS_RUNNING, (int)s->state);

    atomic_store(&g_uv_tcp_init_fail, 1);   // fail the NEXT (accept) uv_tcp_init
    client_connect_drop();                  // -> svr_on_connect :2127 early-out

    // The reactor survived the failed accept (did not crash/exit). NOTE we do
    // NOT assert a follow-up echo here: :2127 fails BEFORE uv_accept, so the
    // connection stays pending in libuv's backlog and absorbs the next accept
    // -- inherent to this arm, not a server fault. Survival is the proof.
    ASSERT_TRUE(vws_tcp_svr_is_running(s));

    stop_server(s, &tid);
}

//------------------------------------------------------------------------------
// C-SVR-2: svr_on_connect accept path -- uv_read_start failure (server.c:2135).
//
// uv_tcp_init + uv_accept succeed (the handle is registered in the loop + the
// fd adopted), then uv_read_start!=0 returns early -- again WITHOUT freeing `c`
// / `cinfo`. The handle is later closed by tcp_svr_dtor's uv_walk (no UAF) but
// the c+cinfo MEMORY leaks. COVERAGE of :2135-2139; leak = candidate.
//------------------------------------------------------------------------------

CTEST(svr_err, accept_uv_read_start_fail)
{
    uv_thread_t tid;
    vws_tcp_svr* s = start_server(&tid, 2);
    ASSERT_EQUAL(VS_RUNNING, (int)s->state);

    atomic_store(&g_uv_read_start_fail, 1);   // fail the accept uv_read_start
    client_connect_drop();                    // -> svr_on_connect :2135 early-out

    ASSERT_TRUE(client_echo("after-uv-read-start-fail"));

    stop_server(s, &tid);
}

//------------------------------------------------------------------------------
// inetd adoption -- uv_tcp_init failure (server.c:1450).
//
// inetd_run spawns the worker pool (:1431) BEFORE adopting the socket. On
// uv_tcp_init!=0 it frees `c` and returns 1 -- but the pool_size workers it
// already spawned are NOT torn down (they block on the request-queue cond). The
// SUBJECT leaks those threads to the caller; here we MIRROR inetd_stop's
// worker-release so the harness can JOIN them and stay bounded. The candidate
// (early-return leaves the worker pool running) is reported, not fixed.
//
// NOTE the deeper :1458 (uv_tcp_open) / :1473 (uv_read_start) inetd arms are
// audited-only (documented in the wa2 report): on those, the handle is already
// registered in the loop and the path frees it WITHOUT uv_close, so any
// subsequent teardown is a use-after-free in tcp_svr_dtor's uv_walk -- a
// candidate that cannot be driven into a clean bounded cell without first
// fixing the SUBJECT. We cover the safe :1450 arm (init failed -> `c` never
// entered the loop) and document the rest.
//------------------------------------------------------------------------------

static int make_inet_pair(int* client)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    bind(lfd, (struct sockaddr*)&a, sizeof(a));
    listen(lfd, 1);
    socklen_t l = sizeof(a);
    getsockname(lfd, (struct sockaddr*)&a, &l);
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    connect(cfd, (struct sockaddr*)&a, l);
    int afd = accept(lfd, NULL, NULL);
    close(lfd);
    *client = cfd;
    return afd;
}

// Release the worker pool the SUBJECT left blocked on its early-return, then
// JOIN every worker -- bounded teardown of the half-constructed inetd server.
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

// C-SVR-4 (FIXED): tcp_svr_dtor now queue_destroy()s a never-run server's
// ctor-allocated queues, so vws_tcp_svr_free alone is leak-clean. Kept as a
// named helper so the inetd never-run cells read as a single teardown call and
// remain C-SVR-4 falsifiers: revert the tcp_svr_dtor hunk -> these cells leak
// the ~16 KB ctor queues -> valgrind RED.
static void free_unrun_server(vws_tcp_svr* s)
{
    vws_tcp_svr_free(s);
}

CTEST(inetd_err, inetd_uv_tcp_init_fail)
{
    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    s->on_data_in = process_echo;

    int client;
    int sockfd = make_inet_pair(&client);

    atomic_store(&g_uv_tcp_init_fail, 1);   // fail adoption's uv_tcp_init :1450
    int rc = vws_tcp_svr_inetd_run(s, sockfd);
    ASSERT_EQUAL(1, rc);                     // early-return arm taken
    ASSERT_EQUAL(1, s->inetd_mode);          // :1445 ran before the failure

    release_and_join_workers(s);             // bound the orphaned worker pool
    free_unrun_server(s);
    close(client);
    close(sockfd);
}

//------------------------------------------------------------------------------
// inetd adoption -- invalid descriptor (server.c:1417-1421), the C-SVR-4
// residual SP3 left uncovered.
//
// inetd_run(sockfd<0) returns 1 immediately, BEFORE spawning workers or
// touching libuv -- a clean early-out. The only loose end is that
// tcp_svr_dtor never queue_destroy()s a never-run server (the ~16 KB ctor-queue
// leak SP3 noted); free_unrun_server destroys them explicitly so this cell is
// leak-clean. The dtor-does-not-destroy-queues gap is the C-SVR-4 candidate.
//------------------------------------------------------------------------------

CTEST(inetd_err, inetd_sockfd_negative)
{
    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);

    ASSERT_EQUAL(1, vws_tcp_svr_inetd_run(s, -1));   // :1417-1421
    ASSERT_EQUAL(0, s->inetd_mode);                  // never reached :1445

    free_unrun_server(s);                            // no workers were spawned
}

//------------------------------------------------------------------------------
// queue shutdown-drain leak (drain-leak, FIXED) — queue_push blocked-pusher
// halt-wake path.
//
// queue_push takes ownership of `data`. The pre-lock not-running path frees it
// (vws_svr_data_free). But the path where a pusher BLOCKED on a full queue then
// wakes to find the queue no longer RUNNING used to return WITHOUT freeing
// `data` -- an ownership leak inconsistent with the pre-lock path. FIXED: that
// path now drops the hand-off's reference too.
//
// This cell reproduces it deterministically as a pure unit (no reactor): fill
// the queue, block a pusher, drain one slot, flip to HALTING + broadcast -> the
// pusher takes the halt-wake path. d1/d2 are refcount-correct vws_svr_data
// (vws_svr_data_own, refs=1) so the fixed free path is exercised honestly.
// FALSIFIER: revert the queue_push hunk -> d2 leaks -> valgrind RED on this cell.
//------------------------------------------------------------------------------

static vws_svr_queue g_drain_q;

static void drain_pusher_fn(void* arg)
{
    // Blocks on the full queue, then wakes into the HALTING path.
    queue_push(&g_drain_q, (vws_svr_data*)arg);
}

CTEST(queue_err, shutdown_drain_leak)
{
    vws_cid_t cid;
    memset(&cid, 0, sizeof(cid));

    queue_init(&g_drain_q, 1, "drain");      // capacity 1, state RUNNING
    ASSERT_TRUE(g_drain_q.state == VS_RUNNING);

    // Fill the single slot. Refcount-correct object (refs=1, data=NULL).
    vws_svr_data* d1 = vws_svr_data_own(NULL, cid, NULL, 0);
    queue_push(&g_drain_q, d1);

    // A pusher that blocks on the full queue.
    vws_svr_data* d2 = vws_svr_data_own(NULL, cid, NULL, 0);
    uv_thread_t pusher;
    uv_thread_create(&pusher, drain_pusher_fn, d2);

    // Let the pusher reach cond_wait, then drain a slot so size<capacity.
    vws_msleep(100);
    vws_svr_data* popped = queue_pop(&g_drain_q);
    ASSERT_TRUE(popped == d1);
    vws_svr_data_free(d1);

    // Flip to shutdown + wake the pusher -> it exits via the halt-wake path,
    // which (FIXED) frees d2 instead of leaking it.
    uv_mutex_lock(&g_drain_q.mutex);
    g_drain_q.state = VS_HALTING;
    uv_cond_broadcast(&g_drain_q.cond);
    uv_mutex_unlock(&g_drain_q.mutex);

    uv_thread_join(&pusher);                 // bounded: pusher returns post-fix

    // d2 was freed by queue_push (the fix). The queue is now empty.
    ASSERT_TRUE(g_drain_q.size == 0);

    g_drain_q.state = VS_RUNNING;            // queue_destroy expects a normal end
    queue_destroy(&g_drain_q);
}

//------------------------------------------------------------------------------
// C-SVR-4 (FIXED) — tcp_svr_dtor frees a never-run server's ctor queues.
//
// A server created with vws_tcp_svr_new but never run skips svr_shutdown (the
// only other queue_destroy site), so before the fix tcp_svr_dtor leaked the
// ctor-allocated request/response queue buffers, names, mutex + cond (~16 KB).
// FIXED: tcp_svr_dtor now queue_destroy()s both (idempotent -- a no-op on the
// run path where svr_shutdown already freed them). Dedicated cell, no inetd/wrap
// machinery. FALSIFIER: revert the tcp_svr_dtor hunk -> ~16 KB leak here ->
// valgrind RED.
//------------------------------------------------------------------------------

CTEST(svr_lifecycle, new_free_unrun)
{
    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQUAL(VS_HALTED, (int)s->state);  // never run

    vws_tcp_svr_free(s);                     // dtor must destroy the ctor queues
}

//------------------------------------------------------------------------------
// C-SVR-6 / vws_cleanup null-after-free (FIXED) — no dangling double-free.
//
// vws_set_error frees vws.e.text before each strdup. vws_cleanup() frees it at
// thread teardown; before the fix it left vws.e.text dangling (freed, non-null),
// so a subsequent vws_set_error on the SAME thread would free() that dangling
// pointer -> double-free. FIXED: vws_cleanup nulls vws.e.text after free, so it
// is safe to call on a thread that keeps running. FALSIFIER: revert the
// `vws.e.text = NULL;` hunk -> the second set_error double-frees -> valgrind
// "Invalid free" RED (or glibc abort).
//------------------------------------------------------------------------------

CTEST(vws_err, cleanup_no_dangling)
{
    vws.error(VE_RT, "first");               // strdups vws.e.text
    vws_cleanup();                           // frees + (FIX) nulls vws.e.text
    ASSERT_TRUE(vws.e.text == NULL);

    // Without the null-after-free this would free() a dangling pointer (the prior
    // vws.e.text) on its frees-then-strdups path -> double-free.
    vws.error(VE_RT, "second");              // must NOT double-free
    ASSERT_TRUE(vws.e.text != NULL);

    vws_cleanup();                           // release "second"
    ASSERT_TRUE(vws.e.text == NULL);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_sp5: watchdog deadline exceeded — abort\n");
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
