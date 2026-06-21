// test_svr_sp3.c — server.c coverage, SUB-PHASE 3 (inetd mode).
//
// vws_tcp_svr_inetd_run adopts a pre-opened TCP fd + runs the loop; it
// auto-stops (inetd_stop) when the single socket closes. Driven over a loopback
// TCP pair on a bounded self-killing thread.
//
// SERVER-PROTECTION (HARD): bounded + wall-clock _Exit watchdog + the inetd
// thread auto-stops on client close and is JOINED -- no runaway server.

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"
#include "server.h"
#include "socket.h"
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

static void noop_inetd_connect(vws_svr_cnx* c) { (void)c; }
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

// A loopback TCP pair: return the server end (for inetd_run), set *client.
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

static int g_inetd_fd;
static void inetd_thread_fn(void* arg)
{
    vws_tcp_svr_inetd_run((vws_tcp_svr*)arg, g_inetd_fd);
}

// NOTE: the inetd_run(sockfd<0) arm (:1417-1421) is NOT covered -- exercising
// it needs vws_tcp_svr_new+free WITHOUT a run, which leaks the ctor's queue
// buffers (~16 KB): vws_tcp_svr_free does not queue_destroy a never-run server
// (candidate C-SVR-4, edge/local). Left as residual.
CTEST(inetd, echo_and_autostop)
{
    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    s->on_data_in        = process_echo;
    s->on_inetd_connect  = noop_inetd_connect;

    int client;
    g_inetd_fd = make_pair(&client);

    uv_thread_t tid;
    uv_thread_create(&tid, inetd_thread_fn, s);

    // Spin until inetd mode is up.
    int spins = 0;
    while (s->inetd_mode != 1 && spins++ < 300) vws_msleep(10);
    ASSERT_EQUAL(1, s->inetd_mode);   // adoption succeeded (1448-1495)

    // C-SVR-3 cell: NO workaround -- rely on inetd_run setting VS_RUNNING so the
    // close -> inetd_stop auto-stop fires. RED on current (hang -> watchdog),
    // GREEN after the inetd_run fix.

    // Echo round-trip over the raw client fd.
    write(client, "hi", 2);
    char buf[16];
    vws_msleep(100);
    ssize_t n = read(client, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);

    // Close the client -> the inetd server sees EOF -> auto-stop -> run returns.
    close(client);   // EOF -> svr_on_read nread<0 -> on_close -> inetd_stop
    uv_thread_join(&tid);   // bounded by the auto-stop + the watchdog
    vws_tcp_svr_free(s);
}

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_svr_sp3: watchdog deadline exceeded — abort\n");
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
