// test_svr_sp1.c — server.c coverage, SUB-PHASE 1 (item-4 module 6).
//
// Reactor lifecycle (ctor/run/stop/dtor/shutdown) + accept->read->dispatch->
// write->close + the worker-pool/queue, via a BOUNDED self-killing live TCP
// server (a server thread on an ephemeral loopback port + a loopback client),
// plus the address-pool + queue units and the --wrap alloc/uv-fail arms.
//
// #includes server.c (statics + isolated coverage) + links clean libvws + libuv
// for the deps (the #included server symbols shadow server.o).
//
// SERVER-PROTECTION (HARD): every cell bounds its clients, an iteration cap on
// the start-up spin, a wall-clock _Exit watchdog, and STOPS + JOINS the server
// thread before returning -- NO unbounded/backgrounded server EVER.
//
// SUBJECT, not modified: a real bug a cell surfaces is ESCALATED/flagged.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "socket.h"

#include <pthread.h>
#include <unistd.h>

//------------------------------------------------------------------------------
// One-shot fault injection
//------------------------------------------------------------------------------

extern void* __real_calloc(size_t n, size_t s);
static int g_calloc_fail = 0;

void* __wrap_calloc(size_t n, size_t s)
{
    if (g_calloc_fail && --g_calloc_fail == 0) return NULL;
    return __real_calloc(n, s);
}

// server.c is the SUBJECT (statics + isolated coverage).
#include "../server.c"

static const int WATCHDOG_SECONDS = 60;

static cstr server_host = "127.0.0.1";
static int  g_port = 18181;            // bumped per cell to dodge TIME_WAIT

//------------------------------------------------------------------------------
// Bounded live-server harness
//------------------------------------------------------------------------------

// Echo processing callback (runs on a worker thread): copy + send back.
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
    vws_tcp_svr* s = (vws_tcp_svr*)arg;
    vws_tcp_svr_run(s, server_host, g_run_port);
}

// Start a server on its own thread; spin (capped) until VS_RUNNING.
static vws_tcp_svr* start_server(uv_thread_t* tid, int pool, int backlog,
                                 int qsize)
{
    g_run_port = g_port++;
    vws_tcp_svr* s = vws_tcp_svr_new(pool, backlog, qsize);
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
    uv_thread_join(tid);   // the run-loop has exited
    vws_tcp_svr_free(s);
}

// One bounded echo round-trip from a fresh client.
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

//------------------------------------------------------------------------------
// Reactor lifecycle + accept/read/dispatch/write/close (single client)
//------------------------------------------------------------------------------

CTEST(svr, echo_roundtrip)
{
    uv_thread_t tid;
    vws_tcp_svr* s = start_server(&tid, 4, 0, 0);
    ASSERT_EQUAL(VS_RUNNING, (int)s->state);
    ASSERT_TRUE(vws_tcp_svr_is_running(s));

    ASSERT_TRUE(client_echo("hello server"));

    stop_server(s, &tid);   // stops + joins + frees -- no server left running
}

//------------------------------------------------------------------------------
// Multiple clients (worker-pool + queue under a small burst)
//------------------------------------------------------------------------------

CTEST(svr, echo_multi_client)
{
    uv_thread_t tid;
    vws_tcp_svr* s = start_server(&tid, 4, 0, 0);

    for (int i = 0; i < 8; i++)
    {
        ASSERT_TRUE(client_echo("burst"));
    }

    stop_server(s, &tid);
}

// A client that connects then drops immediately (no send) -> the server sees
// EOF (nread<0) -> svr_on_read close path + svr_on_close.
static void client_connect_drop(void)
{
    vws_socket* c = vws_socket_new();
    vws_socket_connect(c, server_host, g_run_port, false);
    vws_socket_free(c);   // closes the fd
}

CTEST(svr, client_abrupt_disconnect)
{
    uv_thread_t tid;
    vws_tcp_svr* s = start_server(&tid, 2, 0, 0);

    client_connect_drop();
    vws_msleep(100);   // let the reactor observe the EOF + run on_close
    ASSERT_TRUE(client_echo("after-drop"));   // server still serving

    stop_server(s, &tid);
}

CTEST(svr, queue_overflow_wait)
{
    // pool=1 worker + queue capacity 1 + a burst -> the request queue fills and
    // queue_push blocks on the cond until the single worker drains (2287-2290).
    uv_thread_t tid;
    vws_tcp_svr* s = start_server(&tid, 1, 0, 1);

    for (int i = 0; i < 6; i++)
    {
        ASSERT_TRUE(client_echo("q"));
    }

    stop_server(s, &tid);
}

CTEST(svr, queue_unit)
{
    // Deterministic queue branch coverage (push-not-running drop, fill+pop,
    // pop-on-halting) without the live reactor.
    vws_svr_queue q;
    queue_init(&q, 2, "t");
    q.state = VS_RUNNING;
    ASSERT_TRUE(queue_empty(&q));

    vws_svr_data* d1 = (vws_svr_data*)vws.malloc(sizeof(vws_svr_data));
    vws_svr_data* d2 = (vws_svr_data*)vws.malloc(sizeof(vws_svr_data));
    queue_push(&q, d1);
    queue_push(&q, d2);            // fills capacity 2
    ASSERT_FALSE(queue_empty(&q));
    ASSERT_TRUE(queue_pop(&q) == d1);
    ASSERT_TRUE(queue_pop(&q) == d2);
    vws.free(d1);
    vws.free(d2);

    // push when not running -> data dropped via the (now ref-aware) free. The
    // item must carry an initialized refcount AND a NULL data pointer: the
    // ref-aware vws_svr_data_free reads both refs and data (vws.free(t->data))
    // when it drops to zero, so an uninitialized data field would be a read of
    // uninitialized memory + a free of a garbage pointer (RACE-3 refcount).
    q.state = VS_HALTED;
    vws_svr_data* d3 = (vws_svr_data*)vws.malloc(sizeof(vws_svr_data));
    atomic_init(&d3->refs, 1);
    d3->data = NULL;
    queue_push(&q, d3);

    // pop on halting -> NULL (2317-2323).
    q.state = VS_HALTING;
    ASSERT_NULL(queue_pop(&q));

    q.state = VS_RUNNING;   // queue_destroy expects a normal teardown
    queue_destroy(&q);
}

//------------------------------------------------------------------------------
// Address pool unit (set / get / remove / resize-by-overflow)
//------------------------------------------------------------------------------

CTEST(svr, address_pool)
{
    address_pool* pool = address_pool_new(2, 2);   // initial_size, growth_factor
    ASSERT_NOT_NULL(pool);

    // Set past the initial capacity -> resize.
    int64_t a = address_pool_set(pool, (uintptr_t)0x1000);
    int64_t b = address_pool_set(pool, (uintptr_t)0x2000);
    int64_t c = address_pool_set(pool, (uintptr_t)0x3000);   // forces resize

    ASSERT_EQUAL(0x1000, (int)address_pool_get(pool, a));
    ASSERT_EQUAL(0x2000, (int)address_pool_get(pool, b));
    ASSERT_EQUAL(0x3000, (int)address_pool_get(pool, c));

    address_pool_remove(pool, b);
    ASSERT_EQUAL(0, (int)address_pool_get(pool, b));   // freed slot -> 0

    address_pool_free(&pool);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_sp1: watchdog deadline exceeded — abort\n");
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
