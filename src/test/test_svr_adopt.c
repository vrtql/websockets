// test_svr_adopt.c — fd-adoption seam (vws_tcp_svr_run_adopt +
// vws_tcp_svr_adopt + the FD_ADOPT drain arm).
//
// The seam under test: a server started with vws_tcp_svr_run_adopt() owns NO
// listening socket; an application-owned acceptor hands each connected fd to
// a target server with vws_tcp_svr_adopt() (callable from any thread). The
// loop thread drains the FD_ADOPT block and runs the inetd-pattern adoption
// body there. Any drain path that discards an FD_ADOPT block without adopting
// must CLOSE the carried fd (an open descriptor is not memory — freeing the
// block alone leaks the socket).
//
// Cells:
//   1 echo_roundtrip        — no-listen loop liveness + two adopted
//                             connections echo on one server (cross-thread
//                             hand-off) + clean stop/join.
//   2 not_running_false     — adopt() on a non-running server refuses and the
//                             CALLER retains the (still-open) descriptor.
//   3 shutdown_drain_closes — white-box (reqleak pattern): a block still
//                             queued when svr_shutdown drains must have its
//                             fd CLOSED (EBADF), not just freed.
//   4 push_after_halt_closes— white-box: the queue_push not-running arm (the
//                             stop()-races-adopt window) must close the fd.
//   5 concurrent_adopters   — 4 threads x 4 fds adopt + echo concurrently
//                             (the valgrind/TSan concurrency subject).
//
// SERVER-PROTECTION (HARD): bounded cells + wall-clock _Exit watchdog; every
// live server is stop()'d and its run thread JOINED — no runaway server.
//
// SUBJECT, NOT MODIFIED: server.c is #included (statics + isolated coverage).

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"
#include "server.h"
#include "socket.h"
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 60;

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

// A loopback TCP pair: return the server end (for adoption), set *client.
static int make_pair(int* client)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
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

static void run_adopt_thread_fn(void* arg)
{
    vws_tcp_svr_run_adopt((vws_tcp_svr*)arg);
}

// Block until an echo of `msg` comes back on the raw client fd (bounded by
// the cell watchdog; the fd is blocking so read() parks until the reply).
static bool echo_ok(int client, const char* msg)
{
    size_t n = strlen(msg);
    if (write(client, msg, n) != (ssize_t)n)
    {
        return false;
    }

    char   buf[64];
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = read(client, buf + got, sizeof(buf) - got);
        if (r <= 0)
        {
            return false;
        }
        got += (size_t)r;
    }

    return memcmp(buf, msg, n) == 0;
}

// Cell 1: no-listen loop liveness + adoption + echo on TWO connections.
// The echoes are the liveness proof: uv_run() must be parked on the wakeup
// handle (not returned) for the FD_ADOPT drain to run at all.
CTEST(adopt, echo_roundtrip)
{
    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    s->on_data_in  = process_echo;

    uv_thread_t tid;
    uv_thread_create(&tid, run_adopt_thread_fn, s);

    int spins = 0;
    while (vws_tcp_svr_is_running(s) == false && spins++ < 300) vws_msleep(10);
    ASSERT_TRUE(vws_tcp_svr_is_running(s));

    int c1, c2;
    int a1 = make_pair(&c1);
    int a2 = make_pair(&c2);

    // Cross-thread hand-off: this (test) thread plays the acceptor.
    ASSERT_TRUE(vws_tcp_svr_adopt(s, a1));
    ASSERT_TRUE(vws_tcp_svr_adopt(s, a2));

    // Both adopted connections serve on the one loop.
    ASSERT_TRUE(echo_ok(c1, "hello-a1"));
    ASSERT_TRUE(echo_ok(c2, "hello-a2"));

    // Interleave a second round (connections persist after first serve).
    ASSERT_TRUE(echo_ok(c2, "again-a2"));
    ASSERT_TRUE(echo_ok(c1, "again-a1"));

    close(c1);
    close(c2);

    vws_tcp_svr_stop(s);
    uv_thread_join(&tid);
    ASSERT_TRUE(vws_tcp_svr_is_halted(s));
    vws_tcp_svr_free(s);
}

// Cell 2: a non-running server refuses the hand-off and the caller RETAINS
// the open descriptor (the acceptor's dead-reactor skip depends on this).
CTEST(adopt, not_running_returns_false)
{
    vws_tcp_svr* s = vws_tcp_svr_new(1, 0, 0);   // ctor state = VS_HALTED

    int client;
    int afd = make_pair(&client);

    ASSERT_FALSE(vws_tcp_svr_adopt(s, afd));

    // Caller still owns a LIVE descriptor.
    ASSERT_TRUE(fcntl(afd, F_GETFD) != -1);

    // Invalid-arg arms.
    ASSERT_FALSE(vws_tcp_svr_adopt(NULL, afd));
    ASSERT_FALSE(vws_tcp_svr_adopt(s, VWS_INVALID_SOCKET));

    close(afd);
    close(client);
    vws_tcp_svr_free(s);
}

// Cell 3 (white-box, reqleak pattern): an FD_ADOPT block still resident in
// the responses queue when svr_shutdown drains must have its fd CLOSED. The
// loop never runs, so the block is guaranteed resident at shutdown.
CTEST(adopt, shutdown_drain_closes_fd)
{
    vws_tcp_svr* s = vws_tcp_svr_new(1, 0, 0);

    int client;
    int afd = make_pair(&client);

    // White-box: mark RUNNING so adopt() accepts; the loop is never run, so
    // the pushed block sits in responses (uv_async_send is pended, unserved).
    s->state = VS_RUNNING;
    ASSERT_TRUE(vws_tcp_svr_adopt(s, afd));
    ASSERT_TRUE(s->responses.size == 1);

    // Drive the ACTUAL drain. svr_shutdown requires a non-HALTED state.
    s->state = VS_HALTING;
    svr_shutdown(s);

    // The descriptor must be CLOSED by the drain, not merely freed with the
    // block. F_GETFD on a closed fd fails with EBADF.
    errno = 0;
    ASSERT_TRUE(fcntl(afd, F_GETFD) == -1);
    ASSERT_EQUAL(EBADF, errno);

    close(client);
    vws_tcp_svr_free(s);
}

// Cell 4 (white-box): the stop()-races-adopt window. adopt()'s state check
// passes, but the responses queue has already flipped out of VS_RUNNING --
// queue_push's not-running arm consumes the block and must close the fd.
CTEST(adopt, push_after_halt_closes_fd)
{
    vws_tcp_svr* s = vws_tcp_svr_new(1, 0, 0);

    int client;
    int afd = make_pair(&client);

    // White-box: server looks RUNNING to adopt(), queue does not.
    s->state           = VS_RUNNING;
    s->responses.state = VS_HALTING;

    // adopt() returns true (hand-off accepted) but the block is consumed by
    // the queue's not-running arm; the fd must be closed there.
    ASSERT_TRUE(vws_tcp_svr_adopt(s, afd));
    ASSERT_TRUE(s->responses.size == 0);

    errno = 0;
    ASSERT_TRUE(fcntl(afd, F_GETFD) == -1);
    ASSERT_EQUAL(EBADF, errno);

    close(client);
    s->state           = VS_HALTED;    // restore ctor state for the dtor
    s->responses.state = VS_RUNNING;   // queue_destroy in dtor is idempotent
    vws_tcp_svr_free(s);
}

// Cell 5: concurrent adopters (the TSan/valgrind concurrency subject).
// 4 threads each adopt 4 connections into ONE running server and verify the
// echo on each -- exercises the mutex-protected queue + uv_async_send
// hand-off from multiple non-loop threads at once.
#define ADOPTERS         4
#define FDS_PER_ADOPTER  4

static vws_tcp_svr* g_conc_svr = NULL;
static int          g_conc_ok[ADOPTERS];

static void* adopter_thread_fn(void* arg)
{
    int idx = (int)(intptr_t)arg;
    int ok  = 1;

    for (int i = 0; i < FDS_PER_ADOPTER; i++)
    {
        int client;
        int afd = make_pair(&client);

        if (vws_tcp_svr_adopt(g_conc_svr, afd) == false)
        {
            close(afd);
            close(client);
            ok = 0;
            continue;
        }

        char msg[32];
        snprintf(msg, sizeof(msg), "t%d-c%d", idx, i);

        if (echo_ok(client, msg) == false)
        {
            ok = 0;
        }

        close(client);
    }

    g_conc_ok[idx] = ok;
    return NULL;
}

CTEST(adopt, concurrent_adopters)
{
    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    s->on_data_in  = process_echo;
    g_conc_svr     = s;

    uv_thread_t tid;
    uv_thread_create(&tid, run_adopt_thread_fn, s);

    int spins = 0;
    while (vws_tcp_svr_is_running(s) == false && spins++ < 300) vws_msleep(10);
    ASSERT_TRUE(vws_tcp_svr_is_running(s));

    pthread_t adopters[ADOPTERS];
    for (int i = 0; i < ADOPTERS; i++)
    {
        pthread_create(&adopters[i], NULL, adopter_thread_fn,
                       (void*)(intptr_t)i);
    }

    for (int i = 0; i < ADOPTERS; i++)
    {
        pthread_join(adopters[i], NULL);
    }

    for (int i = 0; i < ADOPTERS; i++)
    {
        ASSERT_EQUAL(1, g_conc_ok[i]);
    }

    vws_tcp_svr_stop(s);
    uv_thread_join(&tid);
    ASSERT_TRUE(vws_tcp_svr_is_halted(s));
    vws_tcp_svr_free(s);
}

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_adopt: watchdog deadline exceeded — abort\n");
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
