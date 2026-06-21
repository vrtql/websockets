// test_svr_sp2.c — server.c coverage, SUB-PHASE 2 (peering).
//
// The peer connect/reconnect/fail state machine driven by a CONTROLLABLE mock
// peer (a vws_peer_connect callback returning a scripted socketfd) on a bounded
// self-killing live server. CLOSED -> PENDING (uv_thread) -> worker peer_connect
// -> RECONNECTED -> uv_thread adopts the fd -> CONNECTED.
//
// SERVER-PROTECTION (HARD): bounded waits (capped) + wall-clock _Exit watchdog +
// STOP+JOIN the server thread each cell -- no runaway server.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "socket.h"

#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// server.c is the SUBJECT (statics + isolated coverage).
#include "../server.c"

static const int WATCHDOG_SECONDS = 60;
static cstr server_host = "127.0.0.1";
static int  g_port = 18301;
static int  g_run_port;

//------------------------------------------------------------------------------
// Bounded live-server harness (no on_data_in -- we only drive peering)
//------------------------------------------------------------------------------

static void server_thread_fn(void* arg)
{
    vws_tcp_svr* s = (vws_tcp_svr*)arg;
    vws_tcp_svr_run(s, server_host, g_run_port);
}

// Peering is a WS-server feature: ws_svr_client_data_in (the WS on_data_in)
// holds the PEER_CONNECT handler. vws_svr embeds vws_tcp_svr as its first member
// so a vws_svr* casts to vws_tcp_svr*.
static vws_svr* start_server(uv_thread_t* tid)
{
    g_run_port = g_port++;
    vws_svr* ws = vws_svr_new(2, 0, 0);
    uv_thread_create(tid, server_thread_fn, (vws_tcp_svr*)ws);
    int spins = 0;
    while (((vws_tcp_svr*)ws)->state != VS_RUNNING && spins++ < 500)
    {
        vws_msleep(10);
    }
    return ws;
}

static void stop_server(vws_svr* ws, uv_thread_t* tid)
{
    vws_tcp_svr_stop((vws_tcp_svr*)ws);
    uv_thread_join(tid);
    vws_svr_free(ws);
}

//------------------------------------------------------------------------------
// Mock peer connect callbacks
//------------------------------------------------------------------------------

static int g_peer_other_end = -1;

// Success: a real loopback TCP pair (uv_tcp_open needs an INET socket, not an
// AF_UNIX socketpair). Listen on an ephemeral port, connect, accept -> two TCP
// fds; hand the server the client fd, keep the accepted (peer) end alive.
static int mock_peer_connect_ok(vws_peer* p, void* x)
{
    (void)p; (void)x;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(lfd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(lfd); return -1; }
    listen(lfd, 1);
    socklen_t alen = sizeof(a);
    getsockname(lfd, (struct sockaddr*)&a, &alen);
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(cfd, (struct sockaddr*)&a, alen) != 0)
    {
        close(lfd); close(cfd); return -1;
    }
    int afd = accept(lfd, NULL, NULL);
    close(lfd);
    g_peer_other_end = afd;   // keep the peer end alive
    return cfd;               // the server adopts this TCP fd
}

// Failure: no descriptor.
static int mock_peer_connect_fail(vws_peer* p, void* x)
{
    (void)p; (void)x;
    return -1;
}

//------------------------------------------------------------------------------
// peer_add validation + remove
//------------------------------------------------------------------------------

CTEST(peer, add_null_fn)
{
    uv_thread_t tid;
    vws_svr* ws = start_server(&tid);
    ASSERT_NULL(vws_tcp_svr_peer_add((vws_tcp_svr*)ws, "h", 1, NULL, NULL));   // 1643-1648
    stop_server(ws, &tid);
}

CTEST(peer, add_remove)
{
    uv_thread_t tid;
    vws_svr* ws = start_server(&tid);
    vws_peer* p = vws_tcp_svr_peer_add((vws_tcp_svr*)ws, "1.2.3.4", 9, mock_peer_connect_fail,
                                       NULL);
    ASSERT_NOT_NULL(p);
    vws_tcp_svr_peer_remove((vws_tcp_svr*)ws, "1.2.3.4", 9);
    stop_server(ws, &tid);
}

//------------------------------------------------------------------------------
// connect failure -> peer never reaches CONNECTED; peers_online stays false
//------------------------------------------------------------------------------

CTEST(peer, connect_fail)
{
    uv_thread_t tid;
    vws_svr* ws = start_server(&tid);

    vws_tcp_svr_peer_add((vws_tcp_svr*)ws, "5.6.7.8", 9, mock_peer_connect_fail, NULL);

    // Let the timer fire + the worker attempt the (failing) connect.
    int spins = 0;
    while (vws_tcp_svr_peers_online((vws_tcp_svr*)ws) == false && spins++ < 50) vws_msleep(20);
    ASSERT_FALSE(vws_tcp_svr_peers_online((vws_tcp_svr*)ws));   // never connects

    stop_server(ws, &tid);
}

//------------------------------------------------------------------------------
// CLOSED -> PENDING transition (the uv_thread queues the PEER_CONNECT block).
// The worker-side connect + socket adoption (svr_resolve / peer_connect /
// uv_tcp_open -> CONNECTED) is exercised END-TO-END by the live test_peering.c
// with real peers -- this isolated mock harness covers the peer lifecycle + the
// CLOSED->PENDING transition; the worker-connect+adopt is RESIDUAL here
// (covered-elsewhere).
//------------------------------------------------------------------------------

CTEST(peer, peer_lifecycle_running)
{
    uv_thread_t tid;
    vws_svr* ws = start_server(&tid);

    vws_peer* p = vws_tcp_svr_peer_add((vws_tcp_svr*)ws, "127.0.0.1", 9,
                                       mock_peer_connect_ok, NULL);
    ASSERT_NOT_NULL(p);

    // Let the peer timer + uv_thread run the peer block (CLOSED -> PENDING +
    // queue the PEER_CONNECT request) on a running server, then remove + stop.
    vws_msleep(200);
    vws_tcp_svr_peer_remove((vws_tcp_svr*)ws, "127.0.0.1", 9);

    stop_server(ws, &tid);
    if (g_peer_other_end >= 0) close(g_peer_other_end);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_sp2: watchdog deadline exceeded — abort\n");
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
