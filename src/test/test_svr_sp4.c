// test_svr_sp4.c — server.c coverage, SUB-PHASE 4 (race-sensitive arms / TSan).
//
// Stress the concurrent substrate (server thread + worker pool + client threads
// + repeated teardown) under ThreadSanitizer to surface the data-race class
// that hid 29 races in async.c: queue push/pop vs shutdown, pool-lookup vs
// close, shutdown ordering.
//
// SERVER-PROTECTION (HARD, re-confirmed for the CONCURRENT harness -- NOT
// inherited from the serial cells): an EXPLICIT round cap (no while-true), every
// client thread JOINED, the server STOPPED+JOINED each round, a TSan-sized
// wall-clock _Exit watchdog that fires even on a thread hang/deadlock, and the
// whole binary is additionally run under an external `timeout`. KILL on stuck.

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"
#include "server.h"
#include "socket.h"
#include <pthread.h>
#include <unistd.h>

#include "../server.c"

// TSan slows ~5-10x; size the watchdog for the instrumented stress but keep it
// HARD-bounded so a deadlock cannot run away.
static const int WATCHDOG_SECONDS = 150;
static const int STRESS_ROUNDS    = 15;   // explicit K cap, no while-true
static const int CLIENTS_PER_ROUND = 16;  // oversubscribe -> force preemption at send:1231
static const int MSGS_PER_CLIENT   = 100; // sustained traffic keeps the loop draining

static cstr server_host = "127.0.0.1";
static int  g_port = 18601;
static int  g_run_port;

static void process_echo(vws_svr_data* req, void* ctx)
{
    (void)ctx;
    char* d = (char*)vws.malloc(req->size);
    memcpy(d, req->data, req->size);
    vws_svr_data* reply = vws_svr_data_own(req->server, req->cid, (ucstr)d,
                                           req->size);
    vws_svr_data_free(req);
    vws_tcp_svr_send(reply);
}

static void server_thread_fn(void* arg)
{
    vws_tcp_svr_run((vws_tcp_svr*)arg, server_host, g_run_port);
}

static void client_thread_fn(void* arg)
{
    (void)arg;
    vws_socket* c = vws_socket_new();
    if (vws_socket_connect(c, server_host, g_run_port, false))
    {
        for (int m = 0; m < MSGS_PER_CLIENT; m++)
        {
            vws_socket_write(c, (ucstr)"race", 4);
            vws_socket_read(c);
        }
    }
    vws_socket_free(c);
}

CTEST(svr_tsan, stress)
{
    for (int r = 0; r < STRESS_ROUNDS; r++)   // EXPLICIT bound
    {
        g_run_port = g_port++;
        vws_tcp_svr* s = vws_tcp_svr_new(4, 0, 0);
        s->on_data_in  = process_echo;

        uv_thread_t tid;
        uv_thread_create(&tid, server_thread_fn, s);
        int spins = 0;
        while (s->state != VS_RUNNING && spins++ < 500) vws_msleep(10);

        uv_thread_t clients[CLIENTS_PER_ROUND];
        for (int i = 0; i < CLIENTS_PER_ROUND; i++)
        {
            uv_thread_create(&clients[i], client_thread_fn, NULL);
        }
        for (int i = 0; i < CLIENTS_PER_ROUND; i++)
        {
            uv_thread_join(&clients[i]);   // JOIN every client thread
        }

        // Teardown overlaps with any in-flight worker/network activity.
        vws_tcp_svr_stop(s);
        uv_thread_join(&tid);              // JOIN the server thread
        vws_tcp_svr_free(s);
    }
}

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_sp4: watchdog deadline exceeded — abort\n");
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
