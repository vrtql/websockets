// test_svr_uafopen.c — server.c use-after-free: inetd adoption frees a
// loop-registered uv handle WITHOUT uv_close on the uv_tcp_open-fail arm.
//
// BUG (candidate): vws_tcp_svr_inetd_run (server.c ~1477) does
//   uv_tcp_init(loop, c)   -> registers c in loop->handle_queue
//   uv_tcp_open(c, sockfd) -> on FAILURE: vws.free(c); return 1;   // no uv_close
// Once uv_tcp_init succeeds the handle is linked into the loop's handle_queue.
// Freeing it bare leaves a DANGLING node in that queue. The next loop op that
// walks the queue — tcp_svr_dtor's uv_walk (server.c ~1826) / uv_loop_close —
// dereferences the freed handle => use-after-free. (The peer-RECONNECTED arm in
// uv_thread, server.c ~1015-1021 + ~1030-1036, has the identical shape.)
//
// SP5 marked these arms "audited-only ... cannot be driven into a clean bounded
// cell without first fixing the SUBJECT" (test_svr_sp5.c:211). This cell
// FALSIFIES that: with a one-shot --wrap on uv_tcp_open it drives inetd_run into
// the fail arm, then walks the loop (what the dtor does) and ASan reports the
// UAF on the freed, still-registered handle. Hunt-to-ground: the RED fires.
//
// SUBJECT, NOT MODIFIED: server.c is #included so inetd_run/on_uv_walk are the
// isolated copies. SELF-BOUNDING: watchdog thread + release/join of the worker
// pool inetd_run left blocked on its early return. No unbounded/backgrounded
// server.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"

#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>

// One-shot fault injection on uv_tcp_open (armed AFTER inetd_run's uv_tcp_init
// so only the adopt call fails). Flag is written on the test thread, read on the
// same thread (inetd_run runs synchronously here) — atomic for discipline.
extern int __real_uv_tcp_open(uv_tcp_t* handle, uv_os_sock_t sock);

static _Atomic int g_uv_tcp_open_fail = 0;

int __wrap_uv_tcp_open(uv_tcp_t* handle, uv_os_sock_t sock)
{
    if (atomic_load(&g_uv_tcp_open_fail))
    {
        atomic_store(&g_uv_tcp_open_fail, 0);
        return UV_EBUSY;
    }
    return __real_uv_tcp_open(handle, sock);
}

// server.c is the SUBJECT.
#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_uafopen: watchdog deadline — abort\n");
    _Exit(99);
    return NULL;
}

// A real connected client fd inetd_run can adopt (bind/listen/connect/accept on
// loopback; return the accepted server-side fd).
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

// Release the worker pool inetd_run left blocked on the early return, then JOIN —
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

CTEST(svr_uaf, inetd_open_fail_frees_registered_handle)
{
    pthread_t watch;
    pthread_create(&watch, NULL, watchdog_thread, NULL);
    pthread_detach(watch);

    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    ASSERT_TRUE(s != NULL);

    int client = -1;
    int afd    = make_inet_pair(&client);
    ASSERT_TRUE(afd >= 0);

    // Arm the fault: the NEXT uv_tcp_open (inetd_run's adopt) fails, AFTER
    // uv_tcp_init has already registered the handle in the loop.
    atomic_store(&g_uv_tcp_open_fail, 1);

    // Drives server.c:1477 -> uv_tcp_init OK -> uv_tcp_open FAILS -> vws.free(c)
    // WITHOUT uv_close. inetd_run returns 1; the freed handle stays linked in
    // s->loop->handle_queue.
    int rc = vws_tcp_svr_inetd_run(s, afd);
    ASSERT_EQUAL(1, rc);

    // Bounded teardown of the worker pool the early return left running.
    release_and_join_workers(s);

    close(client);

    // Real teardown: vws_tcp_svr_free -> tcp_svr_dtor, which uv_walk's the
    // loop's handle_queue + runs it to flush closes + uv_loop_close's it (the
    // EXACT path SP5:211 named as where the UAF manifests). Also frees the whole
    // server, so a fixed subject is leak-clean here.
    //
    //  - BUGGY subject: inetd_run bare-freed the still-registered handle, so it
    //    is a DANGLING node in handle_queue. tcp_svr_dtor's uv_walk uv_close's it
    //    -> valgrind "Invalid write" into the freed block (the RED).
    //  - FIXED subject: the open-fail arm uv_close'd the handle via
    //    svr_on_close_discard; uv_walk skips the already-closing handle, the
    //    dtor's uv_run flushes the close cb -> clean, no UAF, no leak (GREEN).
    vws_tcp_svr_free(s);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
