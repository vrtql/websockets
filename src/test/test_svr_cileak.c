// test_svr_cileak.c — candidate: does the listen-socket cinfo (ci)
// block leak on the vws_tcp_svr_run bind-fail arm?
//
// vws_tcp_svr_run (server.c) allocates the listening uv_tcp_t `socket`
// (server.c:1249) and a vws_cinfo `ci` (server.c:1252), sets socket->data = ci,
// then binds. On bind failure it `return -1` (server.c:1275) without freeing
// either. This harness drives that exact arm (held port -> EADDRINUSE) and then
// runs the REAL teardown vws_tcp_svr_free -> tcp_svr_dtor, so the loop / worker
// pool / queues / cpool are all reclaimed and whatever LSan still reports is the
// genuinely-orphaned residue of the fail arm — a CLEAN attribution.
//
// Empirical (LSan) is ground truth for this candidate: report exactly what
// LSan attributes to server.c:1249 / :1252, not a static guess.
//
// SELF-BOUNDING: workers spawned before the bind are release/joined; watchdog
// alarm; state-only otherwise. Run under ASan/LSan (detect_leaks=1).

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"

#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_cileak: watchdog deadline — abort\n");
    _Exit(99);
    return NULL;
}

static int hold_port(int* port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    bind(fd, (struct sockaddr*)&a, sizeof(a));
    listen(fd, 1);
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr*)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

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

CTEST(svr_cileak, bindfail_ci_block)
{
    pthread_t watch;
    pthread_create(&watch, NULL, watchdog_thread, NULL);
    pthread_detach(watch);

    int port = 0;
    int hold = hold_port(&port);
    ASSERT_TRUE(hold >= 0);

    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    ASSERT_TRUE(s != NULL);

    int rc = vws_tcp_svr_run(s, "127.0.0.1", port);
    ASSERT_EQUAL(-1, rc);

    // Bounded worker teardown, then REAL dtor so loop/pool/queues/cpool are
    // reclaimed — leaving only the fail arm's genuine orphans for LSan.
    release_and_join_workers(s);
    vws_tcp_svr_free(s);

    close(hold);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
