#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/ssl.h>

#include "websocket.h"
#include "message.h"
#include "async.h"
#include "ssl_test_util.h"
#include "util/mongoose.h"

#define CTEST_MAIN
#include "ctest.h"

//------------------------------------------------------------------------------
// Reactor unit coverage (async.c) — the central cell of the vws SSL coverage
// matrix (vws-ssl-coverage-matrix.md / -spec-ga1.md, the REACTOR column), full
// READ + WRITE machine, zero ctest_skip.
//
// Two real-peer harnesses (ssl_test_util):
//   - a mongoose ws/wss echo server (real WebSocket peer) for steady-state
//     framing + read/write round trips, multi-record drain, and close;
//   - a real loopback-fd TLS control server (tls_server_*) the test fully
//     drives — renegotiate / clean SSL_shutdown / corrupt-record — for the SSL
//     state machine cells the reactor only reaches through a pollable fd.
//
// CONSTRUCTION: the WS cells use the real vws_acnx embedding (vws_cnx base +
// vws_async) constructed in place via vws_cnx_ctor + vws_loop_attach_cnx (F1
// resolved: additive in-place vws_cnx ctor/dtor). The raw SSL-machine cells use
// an asocket-style vws_socket (cnx == NULL) + vws_loop_attach.
//
// WRITE PATH: the write handler drains its out-buffer through the reactor's
// NON-BLOCKING vws_socket_async_write (F2 resolved: async_write live), so the
// write-side SSL machine — pending=WRITE, inverted WANT_READ-on-write, the §6c
// partial/flush re-arm — runs for real; #4 partial-write + #5 reneg-write
// are real cells, not skips.
//
// SELF-BOUNDING (no-unbounded-harness HARD rule): fixed iteration caps, every
// loop/cnx/server torn down per test, a detached wall-clock watchdog _Exit()s
// after a deadline so a regressed reactor that deadlocks cannot pin a core.
//------------------------------------------------------------------------------

static const int K_TEARDOWN       = 15;
static const int WATCHDOG_SECONDS = 120;

//------------------------------------------------------------------------------
// Fault injection via ld --wrap (test binary only; async.c stays production-
// clean — no test seams). Each arm is one-shot + default pass-through, armed
// immediately before the targeted call so no other call consumes it.
//------------------------------------------------------------------------------

#include <errno.h>

static volatile int g_calloc_fail    = 0;
static volatile int g_socketpair_fail = 0;
static volatile int g_recv_inj       = 0;   // 1=EAGAIN, 2=ECONNRESET
static volatile int g_send_inj       = 0;   // 1=EAGAIN, 2=EPIPE
static volatile int g_poll_inj       = 0;   // 1=EINTR, 2=EFAULT, 3=wake POLLERR

extern void* __real_calloc(size_t n, size_t s);
void* __wrap_calloc(size_t n, size_t s)
{
    if (g_calloc_fail) { g_calloc_fail = 0; return NULL; }
    return __real_calloc(n, s);
}

extern int __real_socketpair(int d, int t, int p, int sv[2]);
int __wrap_socketpair(int d, int t, int p, int sv[2])
{
    if (g_socketpair_fail) { g_socketpair_fail = 0; errno = EMFILE; return -1; }
    return __real_socketpair(d, t, p, sv);
}

extern ssize_t __real_recv(int fd, void* b, size_t n, int f);
ssize_t __wrap_recv(int fd, void* b, size_t n, int f)
{
    if (g_recv_inj == 1) { g_recv_inj = 0; errno = EAGAIN;      return -1; }
    if (g_recv_inj == 2) { g_recv_inj = 0; errno = ECONNRESET;  return -1; }
    return __real_recv(fd, b, n, f);
}

extern ssize_t __real_send(int fd, const void* b, size_t n, int f);
ssize_t __wrap_send(int fd, const void* b, size_t n, int f)
{
    if (g_send_inj == 1) { g_send_inj = 0; errno = EAGAIN; return -1; }
    if (g_send_inj == 2) { g_send_inj = 0; errno = EPIPE;  return -1; }
    return __real_send(fd, b, n, f);
}

extern int __real_poll(struct pollfd* fds, nfds_t n, int to);
int __wrap_poll(struct pollfd* fds, nfds_t n, int to)
{
    if (g_poll_inj == 1) { g_poll_inj = 0; errno = EINTR;  return -1; }
    if (g_poll_inj == 2) { g_poll_inj = 0; errno = EFAULT; return -1; }
    if (g_poll_inj == 3)
    {
        g_poll_inj = 0;
        if (n > 0) fds[0].revents = POLLERR;   // wake fd error condition
        return 1;
    }
    return __real_poll(fds, n, to);
}

//------------------------------------------------------------------------------
// Reusable mongoose ws/wss echo server. Runs on its own thread until the stop
// flag is set; mg_tls_init on SSL accept; WS frames are echoed.
//------------------------------------------------------------------------------

typedef struct
{
    const char*    url;        /**< "ws://127.0.0.1:PORT" or "wss://..." */
    atomic_int     stop;       /**< set by the test thread to stop       */
    atomic_int     running;    /**< the server is listening              */
    pthread_t      tid;        /**< the server thread                    */
} test_server;

static void server_fn(struct mg_connection* c, int ev, void* ev_data,
                      void* udata)
{
    test_server* s = (test_server*)udata;

    if (ev == MG_EV_ACCEPT)
    {
        if (mg_url_is_ssl(s->url))
        {
            struct mg_tls_opts opts =
            {
                .cert    = "files/cert.pem",
                .certkey = "files/key.pem",
            };

            mg_tls_init(c, &opts);
        }
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message* hm = (struct mg_http_message*)ev_data;

        if (mg_http_match_uri(hm, "/websocket"))
        {
            mg_ws_upgrade(c, hm, NULL);
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;
        mg_ws_send(c, wm->data.ptr, wm->data.len, WEBSOCKET_OP_BINARY);
    }
}

static void* server_thread(void* arg)
{
    test_server* s = (test_server*)arg;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, s->url, server_fn, s);

    s->running = 1;

    while (s->stop == 0)
    {
        mg_mgr_poll(&mgr, 100);
    }

    mg_mgr_free(&mgr);

    return NULL;
}

static void server_start(test_server* s, const char* url)
{
    s->url     = url;
    s->stop    = 0;
    s->running = 0;

    pthread_create(&s->tid, NULL, server_thread, s);

    while (s->running == 0)
    {
        vws_msleep(20);
    }
}

static void server_stop(test_server* s)
{
    s->stop = 1;
    pthread_join(s->tid, NULL);
}

//------------------------------------------------------------------------------
// WS-level reactor driver — the real vws_acnx embedding on its own loop thread,
// with a frame counter (read path) and a one-shot out-buffer (write path). The
// wh drains via the reactor's non-blocking vws_socket_async_write.
//------------------------------------------------------------------------------

typedef struct
{
    vws_acnx          acnx;    /**< embedded: vws_cnx base + vws_async      */
    struct vws_loop*  loop;
    pthread_t         tid;
    atomic_int        frames;  /**< inbound frames delivered via ingress    */
    atomic_int        wh_calls;/**< wh invocations (write-armed oracle)     */
    atomic_int        disconnected;
    vws_buffer*       outbuf;  /**< user out-buffer (serialized frames)     */
} driver;

static void driver_on_frame(struct vws_cnx* c, vws_frame* frame)
{
    driver* d = (driver*)c->data;

    d->frames = d->frames + 1;

    vws_frame_free(frame);
}

static void driver_wh(struct vws_socket* s)
{
    driver* d = (driver*)s->data;

    d->wh_calls = d->wh_calls + 1;

    if (d->outbuf != NULL && d->outbuf->size > 0)
    {
        size_t sent = vws_socket_async_write(&d->acnx.async,
                                             d->outbuf->data, d->outbuf->size);

        if (sent > 0)
        {
            vws_buffer_drain(d->outbuf, sent);
        }

        // flush=true (default): stay armed until drained; flush=false:
        // one chunk then await an explicit re-arm (the §6c contract).
        if (d->outbuf->size == 0 || s->flush == false)
        {
            d->acnx.async.write_armed = false;
        }
    }
    else
    {
        d->acnx.async.write_armed = false;
    }
}

static void driver_disconnect(struct vws_socket* s)
{
    driver* d = (driver*)s->data;
    d->disconnected = 1;
}

static void* driver_loop(void* arg)
{
    driver* d = (driver*)arg;
    vws_loop_run(d->loop);
    return NULL;
}

// Connect + attach a WS driver to a fresh loop, running on its own thread.
static bool driver_start(driver* d, const char* url)
{
    memset(d, 0, sizeof(*d));

    d->outbuf = vws_buffer_new();

    // Construct the embedded cnx in place (no separate heap object), then the
    // synchronous connect, then attach the reactor over the embedding.
    vws_cnx_ctor(&d->acnx.base);

    if (vws_connect(&d->acnx.base, url) == false)
    {
        return false;
    }

    d->acnx.base.data            = (char*)d;   // frame-cb back-ptr (cnx data)
    d->acnx.base.base.data       = (char*)d;   // socket-cb back-ptr (wh/disc)
    d->acnx.base.process         = driver_on_frame;
    d->acnx.base.base.disconnect = driver_disconnect;

    d->loop = vws_loop_new();

    if (d->loop == NULL)
    {
        return false;
    }

    if (vws_loop_attach_cnx(d->loop, &d->acnx, NULL, driver_wh) == false)
    {
        return false;
    }

    pthread_create(&d->tid, NULL, driver_loop, d);

    return true;
}

static void driver_stop(driver* d)
{
    vws_loop_stop(d->loop);
    pthread_join(d->tid, NULL);
    vws_loop_free(d->loop);
    vws_cnx_dtor(&d->acnx.base);   // in-place teardown (the embedding owns mem)
    vws_buffer_free(d->outbuf);
}

// Queue a message: serialize a (masked, client) WS frame into the out-buffer,
// then arm the write (the producer path: enqueue -> want_write). The wh sends
// the raw framed bytes through vws_socket_async_write.
static void driver_send(driver* d, const char* text)
{
    vws_frame*  f = vws_frame_new((ucstr)text, strlen(text), 0x2);
    vws_buffer* b = vws_serialize(f);   // consumes (frees) f

    if (b != NULL)
    {
        vws_buffer_append(d->outbuf, b->data, b->size);
        vws_buffer_free(b);
    }

    vws_socket_want_write(&d->acnx.async);
}

//------------------------------------------------------------------------------
// Raw-SSL reactor driver — a vws_socket (no WS framing, cnx == NULL) over the
// real loopback TLS control server, for the SSL state machine cells. The rh
// drains the post-decrypt buffer; the wh writes via vws_socket_async_write.
//------------------------------------------------------------------------------

typedef struct
{
    vws_socket        sock;    /**< populated by asocket_tls_connect        */
    vws_async         async;   /**< standalone reactor handle (cnx == NULL) */
    struct vws_loop*  loop;
    pthread_t         tid;
    atomic_int        reads;   /**< rh invocations                          */
    atomic_int        bytes_in;/**< total decrypted bytes consumed          */
    atomic_int        wh_calls;/**< wh invocations                          */
    atomic_int        disconnected;
    vws_buffer*       outbuf;
} raw_driver;

static void raw_rh(struct vws_socket* s)
{
    raw_driver* d = (raw_driver*)s->data;

    d->reads = d->reads + 1;

    if (s->buffer != NULL && s->buffer->size > 0)
    {
        d->bytes_in = d->bytes_in + (int)s->buffer->size;
        vws_buffer_drain(s->buffer, s->buffer->size);
    }
}

static void raw_wh(struct vws_socket* s)
{
    raw_driver* d = (raw_driver*)s->data;

    d->wh_calls = d->wh_calls + 1;

    if (d->outbuf != NULL && d->outbuf->size > 0)
    {
        size_t sent = vws_socket_async_write(&d->async,
                                             d->outbuf->data, d->outbuf->size);

        if (sent > 0)
        {
            vws_buffer_drain(d->outbuf, sent);
        }

        if (d->outbuf->size == 0)
        {
            d->async.write_armed = false;
        }
    }
    else
    {
        d->async.write_armed = false;
    }
}

static void raw_disconnect(struct vws_socket* s)
{
    raw_driver* d = (raw_driver*)s->data;
    d->disconnected = 1;
}

static void* raw_loop(void* arg)
{
    raw_driver* d = (raw_driver*)arg;
    vws_loop_run(d->loop);
    return NULL;
}

static bool raw_start(raw_driver* d, int port)
{
    memset(d, 0, sizeof(*d));

    d->outbuf = vws_buffer_new();

    if (asocket_tls_connect(&d->sock, port) == false)
    {
        return false;
    }

    d->sock.data       = (char*)d;
    d->sock.disconnect = raw_disconnect;

    d->async.sock = &d->sock;   // raw: cnx stays NULL (no WS ingress)
    d->async.cnx  = NULL;

    d->loop = vws_loop_new();

    if (d->loop == NULL)
    {
        return false;
    }

    if (vws_loop_attach(d->loop, &d->async, raw_rh, raw_wh) == false)
    {
        return false;
    }

    pthread_create(&d->tid, NULL, raw_loop, d);

    return true;
}

static void raw_stop(raw_driver* d)
{
    vws_loop_stop(d->loop);
    pthread_join(d->tid, NULL);
    vws_loop_free(d->loop);

    if (d->sock.ssl != NULL)
    {
        SSL_shutdown(d->sock.ssl);
        SSL_free(d->sock.ssl);
    }

    if (d->sock.sockfd >= 0)
    {
        close(d->sock.sockfd);
    }

    if (d->sock.buffer != NULL)
    {
        vws_buffer_free(d->sock.buffer);
    }

    vws_buffer_free(d->outbuf);
}

static void raw_send(raw_driver* d, const char* text)
{
    vws_buffer_append(d->outbuf, (ucstr)text, strlen(text));
    vws_socket_want_write(&d->async);
}

static void raw_send_n(raw_driver* d, char fill, size_t n)
{
    char* big = (char*)malloc(n);
    memset(big, fill, n);
    vws_buffer_append(d->outbuf, (ucstr)big, n);
    free(big);
    vws_socket_want_write(&d->async);
}

//------------------------------------------------------------------------------
// Tests — WS-level read + write round trips (mongoose real peer)
//------------------------------------------------------------------------------

// Cross-thread wake unblocks a blocked poll() (the uv_async_send equivalent).
CTEST(async, wake_unblocks_poll)
{
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);

    // No connection attached: run_once blocks on the wake fd only. Wake from
    // this thread first so the (single-threaded) run_once returns promptly.
    vws_loop_wake(loop);

    int rc = vws_loop_run_once(loop, 1000);
    ASSERT_TRUE(rc == 0);

    vws_loop_free(loop);
}

// Non-SSL round trip: send -> server echoes -> the read dispatch ingests it.
// Also asserts the SSL machine degenerates cleanly (pending never set).
CTEST(async, nonssl_roundtrip)
{
    test_server srv;
    server_start(&srv, "ws://127.0.0.1:8231");

    driver d;
    ASSERT_TRUE(driver_start(&d, "ws://127.0.0.1:8231/websocket"));

    driver_send(&d, "hello");

    for (int i = 0; i < 100 && d.frames < 1; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.frames >= 1);
    ASSERT_TRUE(d.acnx.async.pending == VWS_SSL_NONE);   // non-SSL degeneration

    driver_stop(&d);
    server_stop(&srv);
}

// Cross-thread producer: the loop is already spinning on its own thread when a
// DIFFERENT thread (this one) arms the write via vws_socket_want_write. The
// must surface write-armed into the loop's poll mask so the wh fires — the
// uplink route()->enqueue+want_write path under the single-writer contract.
CTEST(async, cross_thread_want_write)
{
    test_server srv;
    server_start(&srv, "ws://127.0.0.1:8236");

    driver d;
    ASSERT_TRUE(driver_start(&d, "ws://127.0.0.1:8236/websocket"));

    // Let the loop thread settle into a blocked poll() with NO write armed, so
    // the want_write below is the event that must cross the thread boundary.
    vws_msleep(60);

    driver_send(&d, "from-producer-thread");

    for (int i = 0; i < 100 && d.frames < 1; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.frames >= 1);
    ASSERT_TRUE(d.wh_calls >= 1);

    driver_stop(&d);
    server_stop(&srv);
}

// Peer close on the non-SSL path: the server goes away; recv()==0 (EOF, NOT a
// would-block) must fire disconnect. The plaintext analogue of the SSL
// ZERO_RETURN arm; together they prove the reactor distinguishes a real close
// from WANT/idle on BOTH transports.
CTEST(async, nonssl_peer_close)
{
    test_server srv;
    server_start(&srv, "ws://127.0.0.1:8237");

    driver d;
    ASSERT_TRUE(driver_start(&d, "ws://127.0.0.1:8237/websocket"));

    server_stop(&srv);

    for (int i = 0; i < 200 && d.disconnected == 0; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.disconnected == 1);

    driver_stop(&d);
}

// SSL handshake + attach + round trip (the synchronous wss connect, then the
// reactor drives steady-state SSL read + write).
CTEST(async, ssl_roundtrip)
{
    test_server srv;
    server_start(&srv, "wss://127.0.0.1:8232");

    driver d;
    ASSERT_TRUE(driver_start(&d, "wss://127.0.0.1:8232/websocket"));

    driver_send(&d, "secure");

    for (int i = 0; i < 100 && d.frames < 1; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.frames >= 1);

    driver_stop(&d);
    server_stop(&srv);
}

// SSL_pending multi-record: a large echo spans multiple TLS records; the read
// path must drain to exhaustion (one POLLIN -> the whole message). The large
// outbound also drives the SSL write across multiple POLLOUT re-arms (the write
// remainder is never lost).
CTEST(async, ssl_multirecord)
{
    test_server srv;
    server_start(&srv, "wss://127.0.0.1:8233");

    driver d;
    ASSERT_TRUE(driver_start(&d, "wss://127.0.0.1:8233/websocket"));

    // A payload comfortably larger than one TLS record (~16 KB).
    size_t n = 64 * 1024;
    char*  big = (char*)malloc(n + 1);
    memset(big, 'x', n);
    big[n] = '\0';

    driver_send(&d, big);

    for (int i = 0; i < 200 && d.frames < 1; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.frames >= 1);   // the whole message surfaced as a frame

    free(big);

    driver_stop(&d);
    server_stop(&srv);
}

// Clean close (mongoose): the server goes away; the reactor read path sees the
// close and fires the disconnect handler — NOT on WANT/idle.
CTEST(async, ssl_clean_close)
{
    test_server srv;
    server_start(&srv, "wss://127.0.0.1:8234");

    driver d;
    ASSERT_TRUE(driver_start(&d, "wss://127.0.0.1:8234/websocket"));

    // Stop the server -> the connection closes -> disconnect fires.
    server_stop(&srv);

    for (int i = 0; i < 200 && d.disconnected == 0; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.disconnected == 1);

    driver_stop(&d);
}

// Teardown join-before-disconnect, K iterations: stop() while the loop thread
// is blocked in poll()/receive must JOIN before tearing down the cnx (no UAF).
// The [A] control; wa3 re-runs it under TSan/valgrind for the RED/GREEN.
CTEST(async, teardown_overlap)
{
    for (int i = 0; i < K_TEARDOWN; i++)
    {
        test_server srv;
        server_start(&srv, "ws://127.0.0.1:8235");

        driver d;
        ASSERT_TRUE(driver_start(&d, "ws://127.0.0.1:8235/websocket"));

        // Let the loop thread settle into a blocked poll(), then tear down so
        // stop() overlaps the blocked reader.
        vws_msleep(40);

        driver_stop(&d);
        server_stop(&srv);
    }

    ASSERT_TRUE(true);   // the control is the clean TSan/valgrind run, not this
}

//------------------------------------------------------------------------------
// Tests — SSL state machine over the real loopback TLS control server
//------------------------------------------------------------------------------

// Positive handshake + verify: asocket_tls_connect runs SSL_connect with
// VERIFY_PEER + hostname "localhost" against the D1 localhost cert. A live
// raw_start proves the verified handshake, attach, and steady SSL read+write.
CTEST(async, ssl_verify_and_attach)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    raw_driver d;
    ASSERT_TRUE(raw_start(&d, srv.port));   // fails if verify fails

    raw_send(&d, "verified");

    for (int i = 0; i < 100 && d.bytes_in < 8; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.bytes_in >= 8);            // the echo came back, decrypted
    ASSERT_TRUE(d.disconnected == 0);

    raw_stop(&d);
    tls_server_stop(&srv);
}

// Clean SSL close: server does SSL_shutdown (close_notify); the reactor read
// path sees SSL_ERROR_ZERO_RETURN -> ASYNC_CLOSE -> disconnect (NOT WANT/idle).
CTEST(async, ssl_zero_return_close)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    raw_driver d;
    ASSERT_TRUE(raw_start(&d, srv.port));

    tls_server_command(&srv, TLS_SRV_SHUTDOWN);

    for (int i = 0; i < 200 && d.disconnected == 0; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.disconnected == 1);

    raw_stop(&d);
    tls_server_stop(&srv);
}

// FATAL mid-stream: the server injects raw garbage into the TLS stream; the
// client SSL_read fails decrypt -> SSL_ERROR_SSL -> ASYNC_FATAL -> disconnect.
// Distinct from the clean ZERO_RETURN arm.
CTEST(async, ssl_fatal_error_disconnect)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    raw_driver d;
    ASSERT_TRUE(raw_start(&d, srv.port));

    tls_server_command(&srv, TLS_SRV_CORRUPT);

    for (int i = 0; i < 200 && d.disconnected == 0; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.disconnected == 1);

    raw_stop(&d);
    tls_server_stop(&srv);
}

// Partial SSL_write + flush (#4): a payload far larger than the socket send
// buffer forces SSL_write to surface WANT (sent < requested across attempts);
// the wh re-arms POLLOUT and resumes at the unsent offset; the server echoes
// the WHOLE payload back. The data-integrity oracle (all bytes round-trip, no
// loss/dup, no disconnect) holds whether or not a given run physically splits;
// a hard split is forced at slot-time via a small SO_SNDBUF if needed.
CTEST(async, ssl_partial_write_flush)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    raw_driver d;
    ASSERT_TRUE(raw_start(&d, srv.port));

    // Sink mode: the server drains + counts but does NOT echo, so there is no
    // echo backpressure to deadlock the constrained client send buffer; the
    // partial-write integrity (all bytes sent, none lost on re-arm) is read off
    // srv.rx.
    tls_server_command(&srv, TLS_SRV_DRAIN);

    // Shrink the client send buffer so SSL_write cannot flush it in one
    // shot: it returns WANT_WRITE repeatedly and the wh re-arms POLLOUT,
    // the SAME send at the unsent offset (the real partial-write path). The
    // payload (many * SO_SNDBUF) guarantees wh_calls >> 1.
    int sndbuf = 4096;
    setsockopt(d.sock.sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    size_t n = 64 * 1024;
    raw_send_n(&d, 'p', n);

    for (int i = 0; i < 500 && (size_t)srv.rx < n; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE((size_t)srv.rx >= n);       // all sent, none lost on re-arm
    ASSERT_TRUE(d.wh_calls > 1);            // the write genuinely fragmented
    ASSERT_TRUE(d.disconnected == 0);

    raw_stop(&d);
    tls_server_stop(&srv);
}

// WANT cross-direction READ-HALF (reneg): a real server-initiated TLS1.2
// reneg occurs while the reactor is reading app data; SSL_read may return
// WANT_WRITE (inverted) -> pending=READ, want_dir=POLLOUT, retried on POLLOUT,
// NO user wh while pending. Behavioral oracle (D3): the connection survives the
// reneg and data still flows; no spurious disconnect. (Whether the transient
// WANT_WRITE manifests is timing-dependent per ga1's HARD-case note; the data-
// delivered + no-disconnect oracle holds either way.)
CTEST(async, ssl_reneg_read_half)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    raw_driver d;
    ASSERT_TRUE(raw_start(&d, srv.port));

    // Warm up: confirm the steady path before perturbing it.
    raw_send(&d, "pre-reneg");
    for (int i = 0; i < 100 && d.bytes_in < 9; i++)
    {
        vws_msleep(20);
    }
    ASSERT_TRUE(d.bytes_in >= 9);

    int before = d.bytes_in;

    // Drive a real renegotiation, then send across it.
    tls_server_command(&srv, TLS_SRV_RENEG);
    vws_msleep(40);
    raw_send(&d, "post-reneg-data");

    for (int i = 0; i < 200 && d.bytes_in < before + 15; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(d.bytes_in >= before + 15);   // data delivered across the reneg
    ASSERT_TRUE(d.disconnected == 0);         // reneg is not a disconnect

    raw_stop(&d);
    tls_server_stop(&srv);
}

// WANT cross-direction WRITE-HALF (#5): a reneg in flight while the wh is
// sending makes SSL_write return WANT_READ (inverted) -> pending=WRITE,
// want_dir=POLLIN, the SAME send retried on POLLIN before any further wh, the
// remainder never lost. Behavioral oracle: a large payload sent across a reneg
// fully round-trips with no disconnect (the inverted-want transient is timing-
// dependent per ga1; the integrity + no-disconnect oracle holds either way).
CTEST(async, ssl_reneg_write_half)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    raw_driver d;
    ASSERT_TRUE(raw_start(&d, srv.port));

    // Trigger the reneg, then immediately drive a large send across it.
    tls_server_command(&srv, TLS_SRV_RENEG);

    size_t n = 128 * 1024;
    raw_send_n(&d, 'w', n);

    for (int i = 0; i < 400 && (size_t)d.bytes_in < n; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE((size_t)d.bytes_in >= n);   // the whole send crossed the reneg
    ASSERT_TRUE(d.disconnected == 0);

    raw_stop(&d);
    tls_server_stop(&srv);
}

//------------------------------------------------------------------------------
// Cross-WANT (reneg) branch forcing — the D2 BIO-pair dual-SSL injector.
//
// A real server/client-initiated TLS1.2 renegotiation, with the INVERTED
// direction deliberately blocked, makes SSL_read surface SSL_ERROR_WANT_WRITE
// and SSL_write surface SSL_ERROR_WANT_READ — the cross-direction transients
// that real-network reneg timing does not reliably fire (ga1's HARD case). The
// reactor is driven SYNCHRONOUSLY (vws_loop_run_once) over a socketpair poll-fd
// that is DECOUPLED from the in-proc SSL transport (the BIO pair), so the
// inverted-want state machine (async_read/_write WANT arms, apply_result
// pending=READ/WRITE, the pending-precedence on_ready retry) is hit
// deterministically — no thread, no timing.
//------------------------------------------------------------------------------

// Write a small payload through the reactor's non-blocking send (the wh seam).
static void reneg_wh(struct vws_socket* s)
{
    vws_async* a = (vws_async*)s->data;

    unsigned char buf[64];
    memset(buf, 'z', sizeof(buf));

    vws_socket_async_write(a, buf, sizeof(buf));

    a->write_armed = false;   // one-shot
}

// READ-half: a server-initiated reneg while the client->server pipe is full ->
// the client's SSL_read must WRITE its ClientHello, which blocks -> WANT_WRITE
// (inverted) -> pending=READ / want_dir=POLLOUT.
CTEST(async, ssl_reneg_read_half_inverted)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));

    // Server emits the HelloRequest into the client's read side FIRST (before
    // any client->server bytes exist, so its handshake-read drains nothing).
    ASSERT_TRUE(ssl_pair_renegotiate(&p));

    // Now fill the client's write BIO DIRECTLY (raw, bypassing SSL so it does
    // not itself drive the reneg) so the reneg ClientHello write — emitted from
    // inside the client's SSL_read below — has no room and blocks (WANT_WRITE).
    unsigned char junk[4096];
    memset(junk, 0, sizeof(junk));
    for (int i = 0; i < 64; i++)
    {
        int w = BIO_write(p.client_wbio, junk, (int)sizeof(junk));
        if (w <= 0)
        {
            break;
        }
    }

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = p.client;       // SSL transport is the BIO pair, not this fd
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(vws_loop_attach(loop, &a, NULL, NULL));

    // Make the poll fd readable so the reactor dispatches a read.
    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 200);

    // SSL_read hit the reneg + blocked write -> the inverted arm.
    ASSERT_TRUE(a.pending == VWS_SSL_READ);
    ASSERT_TRUE(a.want_dir == POLLOUT);

    // The socketpair end is writable -> the retry runs the on_ready pending
    // (op==READ) precedence path.
    vws_loop_run_once(loop, 200);

    vws_loop_free(loop);
    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
    ssl_pair_free(&p);            // frees p.client (== sock.ssl); no dbl free
}

// WRITE-half: a client-initiated reneg makes the next SSL_write do the
// handshake -> it writes ClientHello then must READ ServerHello (absent) ->
// WANT_READ (inverted) -> pending=WRITE / want_dir=POLLIN.
CTEST(async, ssl_reneg_write_half_inverted)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));

    ASSERT_TRUE(SSL_renegotiate(p.client) == 1);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = p.client;
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(vws_loop_attach(loop, &a, NULL, reneg_wh));

    sock.data      = (char*)&a;   // the wh finds the reactor here
    a.write_armed  = true;        // POLLOUT (socketpair end is writable)

    vws_loop_run_once(loop, 200);

    // SSL_write hit the reneg + absent ServerHello -> the inverted arm.
    ASSERT_TRUE(a.pending == VWS_SSL_WRITE);
    ASSERT_TRUE(a.want_dir == POLLIN);

    // Make the poll fd readable (want_dir == POLLIN now) so the retry runs the
    // on_ready pending (op==WRITE) precedence path: it re-drives the wh.
    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 200);

    vws_loop_free(loop);
    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
    ssl_pair_free(&p);
}

// A poll error condition on the connection fd (here POLLNVAL from a closed fd)
// must fire the reactor disconnect (on_ready POLLERR|POLLNVAL arm), NOT a read
// or a re-arm.
CTEST(async, pollerr_disconnect)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(vws_loop_attach(loop, &a, NULL, NULL));

    close(sp[0]);   // invalidate the polled connection fd -> POLLNVAL

    vws_loop_run_once(loop, 100);

    ASSERT_TRUE(a.live == false);   // the error arm disconnected the reactor

    vws_loop_free(loop);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
}

// Non-SSL send back-pressure: with the kernel send buffer full, the reactor's
// non-blocking send returns EAGAIN -> ASYNC_WANT_WRITE (same-direction; no
// pending). Driven by a direct vws_socket_async_write (the public wh seam).
CTEST(async, nonssl_send_want_write)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    int sb = 2048;
    setsockopt(sp[0], SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));

    int fl = fcntl(sp[0], F_GETFL, 0);
    fcntl(sp[0], F_SETFL, fl | O_NONBLOCK);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = NULL;            // non-SSL path
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;
    a.live = true;

    // Fill the kernel send buffer (peer never reads) so the next send blocks.
    unsigned char big[65536];
    memset(big, 'q', sizeof(big));
    while (write(sp[0], big, sizeof(big)) > 0)
    {
        /* fill to EAGAIN */
    }

    size_t sent = vws_socket_async_write(&a, big, 128);

    ASSERT_TRUE(sent == 0);                  // would-block -> nothing accepted
    ASSERT_TRUE(a.pending == VWS_SSL_NONE);  // non-SSL: same-dir, no pending

    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
}

// A fatal SSL_write (writing after our own SSL_shutdown) -> ASYNC_FATAL ->
// the reactor disconnects; no re-arm. Via a direct vws_socket_async_write.
CTEST(async, ssl_write_error_disconnect)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));

    SSL_shutdown(p.client);   // our side is shut for write -> SSL_write fails

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = p.client;
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;
    a.live = true;

    unsigned char buf[64];
    memset(buf, 'z', sizeof(buf));

    vws_socket_async_write(&a, buf, sizeof(buf));

    ASSERT_TRUE(a.live == false);   // the fatal write disconnected the reactor

    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
    ssl_pair_free(&p);
}

//------------------------------------------------------------------------------
// Literal-100% fault-injection cells — errno-edge SSL arms (mock-BIO), non-SSL
// errno arms (--wrap recv/send), reneg-completion (c), and the defensive guards
// (--wrap calloc/socketpair/poll + NULL-param calls).
//------------------------------------------------------------------------------

static atomic_int g_rh_hits = 0;
static void counting_rh(struct vws_socket* s)
{
    (void)s;
    g_rh_hits = g_rh_hits + 1;
}

// Attach a raw reactor over a handshake-complete SSL + a socketpair poll fd.
static void inj_attach(struct vws_loop** loop, vws_async* a, vws_socket* sock,
                       SSL* ssl, int fd, vws_socket_wh wh)
{
    memset(sock, 0, sizeof(*sock));
    sock->sockfd = fd;
    sock->ssl    = ssl;
    sock->buffer = vws_buffer_new();
    sock->flush  = true;

    memset(a, 0, sizeof(*a));
    a->sock = sock;

    *loop = vws_loop_new();
    vws_loop_attach(*loop, a, NULL, wh);
    sock->data = (char*)a;
}

static void inj_teardown(struct vws_loop* loop, vws_socket* sock, int sp[2])
{
    vws_loop_free(loop);
    sock->ssl = NULL;            // owned by the ssl_pair
    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock->buffer);
}

// (b) SSL_ERROR_SYSCALL + errno==EAGAIN on read -> ASYNC_WANT_READ (same-dir).
CTEST(async, ssl_syscall_eagain_read)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install(p.client, SSL_INJ_SYSCALL_EAGAIN);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, p.client, sp[0], NULL);

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == true);              // WANT is not a disconnect
    ASSERT_TRUE(a.pending == VWS_SSL_NONE);   // same-direction want

    inj_teardown(loop, &sock, sp);
    ssl_pair_free(&p);
}

// (b) SSL_ERROR_SYSCALL + non-EAGAIN on read -> ASYNC_FATAL -> disconnect.
CTEST(async, ssl_syscall_fatal_read)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install(p.client, SSL_INJ_SYSCALL_FATAL);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, p.client, sp[0], NULL);

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == false);   // non-EAGAIN syscall error -> fatal

    inj_teardown(loop, &sock, sp);
    ssl_pair_free(&p);
}

// (b) SSL_ERROR_SYSCALL + EAGAIN on write -> ASYNC_WANT_WRITE (same-dir).
CTEST(async, ssl_syscall_eagain_write)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install(p.client, SSL_INJ_SYSCALL_EAGAIN);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, p.client, sp[0], reneg_wh);
    a.write_armed = true;

    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == true);
    ASSERT_TRUE(a.pending == VWS_SSL_NONE);

    inj_teardown(loop, &sock, sp);
    ssl_pair_free(&p);
}

// (b) SSL_ERROR_SYSCALL + non-EAGAIN on write -> ASYNC_FATAL -> disconnect.
CTEST(async, ssl_syscall_fatal_write)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install(p.client, SSL_INJ_SYSCALL_FATAL);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, p.client, sp[0], reneg_wh);
    a.write_armed = true;

    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == false);   // non-EAGAIN syscall error -> fatal

    inj_teardown(loop, &sock, sp);
    ssl_pair_free(&p);
}

// (b) SSL_write that processes the peer's close_notify mid-reneg -> ZERO_RETURN
// -> ASYNC_CLOSE -> disconnect.
CTEST(async, ssl_zero_return_write)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));

    SSL_renegotiate(p.client);   // next write drives the handshake
    SSL_shutdown(p.server);      // close_notify -> the client's read side

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, p.client, sp[0], reneg_wh);
    a.write_armed = true;

    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == false);   // CLOSE (or fatal) -> disconnect

    inj_teardown(loop, &sock, sp);
    ssl_pair_free(&p);
}

// (b) non-SSL recv EAGAIN -> ASYNC_WANT_READ (--wrap=recv).
CTEST(async, nonssl_recv_eagain)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, NULL, sp[0], NULL);   // non-SSL

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    g_recv_inj = 1;   // EAGAIN
    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == true);   // would-block is not a disconnect

    inj_teardown(loop, &sock, sp);
}

// (b) non-SSL recv hard error -> ASYNC_FATAL -> disconnect (--wrap=recv).
CTEST(async, nonssl_recv_fatal)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, NULL, sp[0], NULL);

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    g_recv_inj = 2;   // ECONNRESET
    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == false);   // fatal -> disconnect

    inj_teardown(loop, &sock, sp);
}

// (b) non-SSL send hard error -> ASYNC_FATAL -> disconnect (--wrap=send).
CTEST(async, nonssl_send_fatal)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = NULL;
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;
    a.live = true;

    unsigned char buf[64];
    memset(buf, 'z', sizeof(buf));

    g_send_inj = 2;   // EPIPE
    vws_socket_async_write(&a, buf, sizeof(buf));

    ASSERT_TRUE(a.live == false);

    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
}

// (c) reneg-read COMPLETION: pending=READ retry clears -> ingress + rh run.
CTEST(async, ssl_reneg_read_complete)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ASSERT_TRUE(ssl_pair_renegotiate(&p));

    // Fill the client->server BIO so the reneg ClientHello write blocks first.
    unsigned char junk[4096];
    memset(junk, 0, sizeof(junk));
    for (int i = 0; i < 64; i++)
    {
        if (BIO_write(p.client_wbio, junk, (int)sizeof(junk)) <= 0)
        {
            break;
        }
    }

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    g_rh_hits = 0;

    // ws-level reactor (cnx != NULL) over the BIO-pair SSL + a counting rh.
    vws_acnx acnx;
    memset(&acnx, 0, sizeof(acnx));
    vws_cnx_ctor(&acnx.base);
    acnx.base.base.ssl    = p.client;
    acnx.base.base.sockfd = sp[0];
    acnx.async.sock       = &acnx.base.base;
    acnx.async.cnx        = &acnx.base;

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(vws_loop_attach(loop, &acnx.async, counting_rh, NULL));

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 200);
    ASSERT_TRUE(acnx.async.pending == VWS_SSL_READ);

    // Drain the client->server buffer so the retried ClientHello sends;
    // the retry then clears pending -> the completion path runs ingress + rh.
    unsigned char drain[8192];
    while (BIO_read(p.server_wbio, drain, (int)sizeof(drain)) > 0)
    {
        /* free the buffer */
    }

    vws_loop_run_once(loop, 200);
    ASSERT_TRUE(acnx.async.pending == VWS_SSL_NONE);
    ASSERT_TRUE(g_rh_hits >= 1);

    vws_loop_free(loop);
    acnx.base.base.ssl    = NULL;   // owned by the pair
    acnx.base.base.sockfd = -1;     // owned here
    vws_cnx_dtor(&acnx.base);
    close(sp[0]);
    close(sp[1]);
    ssl_pair_free(&p);
}

// (c) reneg-read completion on a RAW reactor (cnx == NULL, rh == NULL): the
// completion block skips ingress + rh (their false sides).
CTEST(async, ssl_reneg_read_complete_raw)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ASSERT_TRUE(ssl_pair_renegotiate(&p));

    unsigned char junk[4096];
    memset(junk, 0, sizeof(junk));
    for (int i = 0; i < 64; i++)
    {
        if (BIO_write(p.client_wbio, junk, (int)sizeof(junk)) <= 0) break;
    }

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, p.client, sp[0], NULL);   // raw: cnx+rh NULL

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 200);
    ASSERT_TRUE(a.pending == VWS_SSL_READ);

    unsigned char drain[8192];
    while (BIO_read(p.server_wbio, drain, (int)sizeof(drain)) > 0) { }

    vws_loop_run_once(loop, 200);
    ASSERT_TRUE(a.pending == VWS_SSL_NONE);

    inj_teardown(loop, &sock, sp);
    ssl_pair_free(&p);
}

// (c) reneg-read retry that STAYS pending (the buffer is not drained, so the
// ClientHello write blocks again) -> the completion's pending==NONE is false.
CTEST(async, ssl_reneg_read_retry_pending)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ASSERT_TRUE(ssl_pair_renegotiate(&p));

    unsigned char junk[4096];
    memset(junk, 0, sizeof(junk));
    for (int i = 0; i < 64; i++)
    {
        if (BIO_write(p.client_wbio, junk, (int)sizeof(junk)) <= 0) break;
    }

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    struct vws_loop* loop;
    vws_socket sock;
    vws_async a;
    inj_attach(&loop, &a, &sock, p.client, sp[0], NULL);

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 200);
    ASSERT_TRUE(a.pending == VWS_SSL_READ);

    // Do NOT drain: the retried ClientHello write blocks again -> pending.
    vws_loop_run_once(loop, 200);
    ASSERT_TRUE(a.pending == VWS_SSL_READ);

    inj_teardown(loop, &sock, sp);
    ssl_pair_free(&p);
}

// (B3) read-backpressure: pause clears read_armed so the loop stops polling
// POLLIN (the readable byte is NOT consumed); want_read re-arms and the read
// resumes. Covers the compute_mask read_armed==false arm (now live).
CTEST(async, read_backpressure)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = NULL;
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;

    g_rh_hits = 0;

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(vws_loop_attach(loop, &a, counting_rh, NULL));
    ASSERT_TRUE(a.read_armed == true);

    vws_socket_pause_read(&a);
    ASSERT_TRUE(a.read_armed == false);

    // Readable, but paused: compute_mask drops POLLIN -> no read, no rh.
    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    vws_loop_run_once(loop, 50);
    ASSERT_TRUE(g_rh_hits == 0);     // the readable byte is NOT consumed
    ASSERT_TRUE(a.live == true);

    // Re-arm: resume -> POLLIN polled -> the byte is read -> rh fires.
    vws_socket_want_read(&a);
    ASSERT_TRUE(a.read_armed == true);

    vws_loop_run_once(loop, 200);
    ASSERT_TRUE(g_rh_hits >= 1);

    vws_loop_free(loop);
    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
}

static void* bp_loop_thread(void* arg)
{
    vws_loop_run((struct vws_loop*)arg);
    return NULL;
}

// (B3) cross-thread want_read: the loop runs on its own thread, reads PAUSED;
// a DIFFERENT (producer) thread re-arms via vws_socket_want_read concurrently.
// The set-before-wake ordering must surface read_armed to the loop after it
// drains the wake -> the pending readable byte is read (no lost wake). Mirrors
// cross_thread_want_write; wa3 runs this under TSan.
CTEST(async, cross_thread_want_read)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = NULL;
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;

    g_rh_hits = 0;

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(vws_loop_attach(loop, &a, counting_rh, NULL));

    // Start paused (setup, before any loop thread exists).
    a.read_armed = false;

    pthread_t tid;
    pthread_create(&tid, NULL, bp_loop_thread, loop);

    // Let the loop settle into a blocked poll() with POLLIN NOT armed.
    vws_msleep(60);

    // Make data available, then re-arm from THIS (producer) thread.
    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;
    vws_socket_want_read(&a);

    for (int i = 0; i < 100 && g_rh_hits < 1; i++)
    {
        vws_msleep(20);
    }

    ASSERT_TRUE(g_rh_hits >= 1);   // the cross-thread re-arm woke + read

    vws_loop_stop(loop);
    pthread_join(tid, NULL);
    vws_loop_free(loop);
    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
}

// (a) vws_loop_new calloc failure -> NULL.
CTEST(async, loop_new_calloc_fail)
{
    g_calloc_fail = 1;
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop == NULL);
    g_calloc_fail = 0;
}

// (a) vws_loop_new wake-pair socketpair failure -> NULL.
CTEST(async, loop_new_socketpair_fail)
{
    g_socketpair_fail = 1;
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop == NULL);
    g_socketpair_fail = 0;
}

// (a) run_once poll() EINTR -> idle tick (rc 0).
CTEST(async, runonce_poll_eintr)
{
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    g_poll_inj = 1;
    int rc = vws_loop_run_once(loop, 100);
    ASSERT_TRUE(rc == 0);
    vws_loop_free(loop);
}

// (a) run_once poll() hard error -> negative VE_SOCKET.
CTEST(async, runonce_poll_error)
{
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    g_poll_inj = 2;
    int rc = vws_loop_run_once(loop, 100);
    ASSERT_TRUE(rc < 0);
    vws_loop_free(loop);
}

// (a) a POLLERR/HUP condition on the wake fd -> drained + ignored.
CTEST(async, runonce_wake_pollerr)
{
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    g_poll_inj = 3;
    int rc = vws_loop_run_once(loop, 100);
    ASSERT_TRUE(rc == 0);
    vws_loop_free(loop);
}

// (a) vws_loop_run propagates a run_once error and stops.
CTEST(async, run_poll_error)
{
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    g_poll_inj = 2;
    int rc = vws_loop_run(loop);
    ASSERT_TRUE(rc < 0);
    vws_loop_free(loop);
}

// (a) run_once with no events + a 0 timeout -> idle tick.
CTEST(async, runonce_timeout_tick)
{
    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    int rc = vws_loop_run_once(loop, 0);
    ASSERT_TRUE(rc == 0);
    vws_loop_free(loop);
}

// (a) every NULL-param / invalid-arg guard.
CTEST(async, null_param_guards)
{
    vws_loop_free(NULL);
    ASSERT_TRUE(vws_loop_run_once(NULL, 0) < 0);
    ASSERT_TRUE(vws_loop_run(NULL) < 0);
    vws_loop_wake(NULL);
    vws_loop_stop(NULL);
    vws_socket_want_write(NULL);
    vws_socket_pause_read(NULL);
    vws_socket_want_read(NULL);
    ASSERT_TRUE(vws_socket_async_write(NULL, NULL, 0) == 0);

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = NULL;
    ASSERT_TRUE(vws_loop_attach(NULL, &a, NULL, NULL) == false);   // loop==NULL
    ASSERT_TRUE(vws_loop_attach(loop, NULL, NULL, NULL) == false);  // a==NULL
    ASSERT_TRUE(vws_loop_attach(loop, &a, NULL, NULL) == false);   // sock==NULL
    ASSERT_TRUE(vws_loop_attach_cnx(loop, NULL, NULL, NULL) == false);

    // Second-operand guards: a != NULL but loop/live invalid (the first ||
    // operand is false, so the second is the one that fires).
    vws_async a2;
    memset(&a2, 0, sizeof(a2));   // a2.loop == NULL, a2.live == false
    vws_socket_want_write(&a2);                              // L748 second op
    vws_socket_want_read(&a2);                               // want_read 2nd op
    ASSERT_TRUE(vws_socket_async_write(&a2, NULL, 0) == 0);  // L761 second op

    vws_loop_free(loop);
}

// Non-pending POLLOUT dispatch with NO write handler (wh == NULL): the
// reactor must not crash -- it simply skips the (absent) handler.
CTEST(async, write_dispatch_no_handler)
{
    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket sock;
    memset(&sock, 0, sizeof(sock));
    sock.sockfd = sp[0];
    sock.ssl    = NULL;
    sock.buffer = vws_buffer_new();
    sock.flush  = true;

    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &sock;

    struct vws_loop* loop = vws_loop_new();
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(vws_loop_attach(loop, &a, NULL, NULL));   // wh == NULL

    a.write_armed = true;   // arm POLLOUT; the socketpair end is writable

    vws_loop_run_once(loop, 200);

    ASSERT_TRUE(a.live == true);

    vws_loop_free(loop);
    close(sp[0]);
    close(sp[1]);
    vws_buffer_free(sock.buffer);
}

//------------------------------------------------------------------------------
// Watchdog + main
//------------------------------------------------------------------------------

// Wall-clock self-kill backstop (the no-unbounded-harness HARD rule): if a
// regressed reactor deadlocks a poll()/join, this thread _Exit(99)s the whole
// process after WATCHDOG_SECONDS so nothing pins a core. Detached so it never
// blocks teardown.
static void* watchdog_thread(void* arg)
{
    (void)arg;

    vws_msleep((unsigned int)WATCHDOG_SECONDS * 1000);

    fprintf(stderr, "test_async: watchdog deadline (%ds) exceeded — aborting\n",
            WATCHDOG_SECONDS);
    _Exit(99);

    return NULL;
}

// One-time setup for the WS-over-SSL cells: they connect to a local mongoose
// server whose cert is not in the system trust store, so vws_connect's default
// VERIFY_PEER rejects it. Prime the lazily-created global vws_ssl_ctx (a
// throwaway connect triggers its pthread_once init) then disable verify on it —
// TEST-ONLY; the POSITIVE verify path is covered by ssl_verify_and_attach (the
// raw asocket_tls_connect cell, D1 cert + VERIFY_PEER).
static void prime_vws_ssl_no_verify(void)
{
    test_server tmp;
    server_start(&tmp, "wss://127.0.0.1:8299");

    vws_cnx* c = vws_cnx_new();
    vws_connect(c, "wss://127.0.0.1:8299/websocket");   // makes the ctx
    vws_cnx_free(c);

    server_stop(&tmp);

    if (vws_ssl_ctx != NULL)
    {
        SSL_CTX_set_verify(vws_ssl_ctx, SSL_VERIFY_NONE, NULL);
    }
}

int main(int argc, const char* argv[])
{
    pthread_t watch;

    pthread_create(&watch, NULL, watchdog_thread, NULL);
    pthread_detach(watch);

    prime_vws_ssl_no_verify();

    return ctest_main(argc, argv);
}
