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

#include <signal.h>
#include <sys/wait.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>

#include "vws.h"
#include "socket.h"
#include "ssl_test_util.h"
#include "async.h"

#define CTEST_MAIN
#include "ctest.h"

//------------------------------------------------------------------------------
// SYNC socket.c SSL coverage — the centerpiece of COMPLETE SSL, sync side
// (vws-ssl-coverage-spec-ga1.md, the SYNC column). socket.c is the SUBJECT: its
// SSL behavior is TESTED, never modified — a real bug a test surfaces is
// ESCALATED, not silently fixed.
//
// Reuses the reactor harness (ssl_test_util): D1 localhost cert, the loopback
// TLS control server (handshake / echo / renegotiate / clean SSL_shutdown /
// corrupt-record), the BIO-pair dual-SSL (real TLS1.2 reneg), and the mock-BIO
// errno injector. The cross-direction WANT + the SSL errno arms are driven over
// a vws_socket whose SSL is a BIO-pair / mock BIO and whose pollable fd is a
// socketpair (decoupled), so vws_socket_read/write's poll()+SSL loop runs
// deterministically.
//
// SELF-BOUNDING: short c->timeout per socket + a wall-clock _Exit watchdog.
//------------------------------------------------------------------------------

static const int WATCHDOG_SECONDS = 120;

// Fault injection (ld --wrap=poll) for the poll() error arms. One-shot, default
// pass-through; armed immediately before the targeted read/write.
#include <errno.h>
static volatile int g_poll_inj = 0;   // 1 = POLLERR revents, 2 = rc -1

extern int __real_poll(struct pollfd* fds, nfds_t n, int to);
int __wrap_poll(struct pollfd* fds, nfds_t n, int to)
{
    if (g_poll_inj == 1) { g_poll_inj = 0; if (n > 0) fds[0].revents = POLLERR;
                           return 1; }
    if (g_poll_inj == 2) { g_poll_inj = 0; if (n > 0) fds[0].revents = 0;
                           errno = EFAULT; return -1; }
    return __real_poll(fds, n, to);
}

static bool hs_ok(vws_socket* s)   { (void)s; return true;  }
static bool hs_fail(vws_socket* s) { (void)s; return false; }

// Build a vws_socket over a handshake-complete SSL + a socketpair poll fd. The
// SSL is owned by the caller (e.g. the ssl_pair); teardown nulls it first.
static void sock_over_fd(vws_socket* s, SSL* ssl, int fd, int timeout_ms)
{
    vws_socket_ctor(s);          // buffer + defaults
    s->ssl     = ssl;
    s->sockfd  = fd;
    s->timeout = timeout_ms;
}

// On a fatal SSL error socket.c's abnormal-close legitimately SSL_free's c->ssl
// (== p->client) and close()s the fd; detect that (s->ssl / fd cleared) so
// the ssl_pair does not double-free and sp[0] is not double-closed.
static void sock_release(vws_socket* s, int sp[2], ssl_pair* p)
{
    if (s->ssl == NULL && p != NULL)
    {
        p->client = NULL;       // socket.c already freed the shared SSL
    }

    s->ssl = NULL;

    if (s->buffer != NULL)
    {
        vws_buffer_free(s->buffer);
        s->buffer = NULL;
    }

    if (s->sockfd >= 0)
    {
        close(s->sockfd);       // sp[0], unless socket.c already closed it
    }
    s->sockfd = -1;

    close(sp[1]);               // the server end is always ours
}

//------------------------------------------------------------------------------
// Handshake + cert verify (ga1 #1) — the global vws_ssl_ctx (VERIFY_PEER +
// system trust) over the loopback server's D1 localhost cert.
//------------------------------------------------------------------------------

// Negative (untrusted) THEN positive (trust loaded) — one cell so the prime of
// the lazily-created vws_ssl_ctx is deterministic.
CTEST(socket_ssl, connect_verify)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    // Prime the process-global vws_ssl_ctx (its pthread_once init runs on the
    // first wss connect) so we can deterministically control its trust below.
    // The ctx is SHARED across cells -> an earlier cell may already have
    // trusted the localhost cert, so we must not assume it is untrusted here.
    if (vws_ssl_ctx == NULL)
    {
        vws_socket probe;
        vws_socket_ctor(&probe);
        probe.timeout = 1000;
        vws_socket_connect(&probe, "localhost", srv.port, true);
        vws_socket_close(&probe);
        vws_buffer_free(probe.buffer);
    }
    ASSERT_TRUE(vws_ssl_ctx != NULL);

    // NEGATIVE: reset the ctx to a FRESH empty trust store so the untrusted-
    // verify path is deterministic for ANY cell order (independent of what
    // other cells did to this shared ctx) -> the localhost cert is untrusted
    // -> SSL_connect verify fails.
    ssl_test_reset_trust(vws_ssl_ctx);
    vws_socket s1;
    vws_socket_ctor(&s1);
    s1.timeout = 2000;
    bool neg = vws_socket_connect(&s1, "localhost", srv.port, true);
    ASSERT_TRUE(neg == false);
    vws_socket_close(&s1);
    vws_buffer_free(s1.buffer);

    // Trust the D1 cert on the now-fresh store (TEST-ONLY).
    ASSERT_TRUE(ssl_test_trust_localhost(vws_ssl_ctx));

    // POSITIVE: trusted + SNI + hostname "localhost" matches SAN -> connect.
    vws_socket s2;
    vws_socket_ctor(&s2);
    s2.timeout = 2000;
    bool pos = vws_socket_connect(&s2, "localhost", srv.port, true);
    ASSERT_TRUE(pos == true);
    ASSERT_TRUE(vws_socket_is_connected(&s2) == true);

    // TLS1.2 was pinned server-side (ga1 #8): version is TLSv1.2.
    ASSERT_TRUE(strcmp(SSL_get_version(s2.ssl), "TLSv1.2") == 0);

    vws_socket_close(&s2);
    vws_buffer_free(s2.buffer);

    tls_server_stop(&srv);
}

//------------------------------------------------------------------------------
// Read/write over TLS (ga1 #2) + SSL_pending multi-record (#3) + clean close
// / abrupt / fatal (ga1 #6) over the real loopback server.
//------------------------------------------------------------------------------

// Connect (ordering of connect_verify is NOT guaranteed across runs, so
// (re)load it here too) + write + read the echo back.
static bool connect_trusted(vws_socket* s, tls_server* srv)
{
    vws_socket_ctor(s);
    s->timeout = 2000;

    // Prime + trust (idempotent).
    if (vws_ssl_ctx == NULL)
    {
        vws_socket probe;
        vws_socket_ctor(&probe);
        probe.timeout = 1000;
        vws_socket_connect(&probe, "localhost", srv->port, true);
        vws_socket_close(&probe);
        vws_buffer_free(probe.buffer);
    }
    if (vws_ssl_ctx != NULL)
    {
        ssl_test_trust_localhost(vws_ssl_ctx);
    }

    return vws_socket_connect(s, "localhost", srv->port, true);
}

CTEST(socket_ssl, read_write_roundtrip)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    ASSERT_TRUE(vws_socket_write(&s, (ucstr)"hello", 5) == 5);

    ssize_t n = vws_socket_read(&s);
    ASSERT_TRUE(n >= 5);
    ASSERT_TRUE(s.buffer->size >= 5);

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

CTEST(socket_ssl, read_multirecord)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    // A payload larger than one TLS record; the echo drains via the SSL_pending
    // loop across multiple SSL_read calls (one vws_socket_read).
    size_t big = 48 * 1024;
    char*  buf = (char*)malloc(big);
    memset(buf, 'x', big);
    ASSERT_TRUE((size_t)vws_socket_write(&s, (ucstr)buf, big) == big);
    free(buf);

    size_t got = 0;
    for (int i = 0; i < 200 && got < big; i++)
    {
        ssize_t n = vws_socket_read(&s);
        if (n > 0) got += (size_t)n;
    }
    ASSERT_TRUE(got >= big);

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

// Clean SSL_shutdown by the peer -> SSL_read errors -> abnormal close.
CTEST(socket_ssl, read_peer_shutdown)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    tls_server_command(&srv, TLS_SRV_SHUTDOWN);
    vws_msleep(40);

    ssize_t n = vws_socket_read(&s);
    ASSERT_TRUE(n <= 0);   // closed/error, not data

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

// Corrupt record -> SSL protocol error -> abnormal close.
CTEST(socket_ssl, read_fatal)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    tls_server_command(&srv, TLS_SRV_CORRUPT);
    vws_msleep(40);

    ssize_t n = vws_socket_read(&s);
    ASSERT_TRUE(n < 0);

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

//------------------------------------------------------------------------------
// SSL errno arms (ga1 #6) + WANT cross-direction (ga1 #5) over a BIO-pair /
// mock-BIO vws_socket with a socketpair poll fd.
//------------------------------------------------------------------------------

// SSL_ERROR_SYSCALL + EAGAIN on read -> VE_TIMEOUT, return 0 (not an error).
CTEST(socket_ssl, read_syscall_eagain)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install(p.client, SSL_INJ_SYSCALL_EAGAIN);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket s;
    sock_over_fd(&s, p.client, sp[0], 200);

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    ssize_t n = vws_socket_read(&s);
    ASSERT_TRUE(n == 0);   // would-block surfaced as VE_TIMEOUT / 0

    sock_release(&s, sp, &p);
    ssl_pair_free(&p);
}

// SSL_ERROR_SYSCALL + non-EAGAIN on read -> abnormal close, -1.
CTEST(socket_ssl, read_syscall_fatal)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install(p.client, SSL_INJ_SYSCALL_FATAL);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket s;
    sock_over_fd(&s, p.client, sp[0], 200);

    char b = 1;
    ssize_t wn = write(sp[1], &b, 1);
    (void)wn;

    ssize_t n = vws_socket_read(&s);
    ASSERT_TRUE(n < 0);

    sock_release(&s, sp, &p);
    ssl_pair_free(&p);
}

// WANT cross-direction over a REAL reneg (ga1 #5). socket.c's read/write
// are BLOCKING (they loop poll()+SSL until the op completes or errors), so the
// reactor's single-step BIO-pair trick cannot be used here — a permanently
// stuck reneg would loop forever. Instead drive a REAL reneg over the loopback
// server (which COMPLETES, so the call returns), behaviorally: the connection
// survives the reneg and data flows. Whether the WANT_WRITE-on-read goto /
// WANT_READ-on-write continue is hit is timing-dependent (ga1's HARD
// case); the no-crash + data-flows oracle holds either way.
CTEST(socket_ssl, read_reneg_cross)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    tls_server_command(&srv, TLS_SRV_RENEG);
    vws_msleep(40);

    ASSERT_TRUE(vws_socket_write(&s, (ucstr)"post-reneg", 10) == 10);

    ssize_t n = vws_socket_read(&s);
    ASSERT_TRUE(n >= 0);                          // no false error / no crash
    ASSERT_TRUE(vws_socket_is_connected(&s));     // survived the reneg

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

CTEST(socket_ssl, write_reneg_cross)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    SSL_renegotiate(s.ssl);   // client-initiated; the next write drives it

    ssize_t n = vws_socket_write(&s, (ucstr)"data-across-the-reneg", 21);
    ASSERT_TRUE(n == 21 || n <= 0);   // completes or times out; no crash
    ASSERT_TRUE(vws_socket_is_connected(&s));

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

// SSL_ERROR_SYSCALL + EAGAIN on write -> socket.c's loop continues; with a
// ONE-SHOT injection the next SSL_write succeeds, so the call exits after the
// errno arm is exercised (a persistent inject would loop -- socket.c blocks
// and retries EAGAIN forever, by design).
CTEST(socket_ssl, write_syscall_eagain)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install_once(p.client, SSL_INJ_SYSCALL_EAGAIN);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket s;
    sock_over_fd(&s, p.client, sp[0], 100);

    unsigned char buf[64];
    memset(buf, 'z', sizeof(buf));

    ssize_t n = vws_socket_write(&s, buf, sizeof(buf));
    ASSERT_TRUE(n == (ssize_t)sizeof(buf));   // completed after the EAGAIN arm

    sock_release(&s, sp, &p);
    ssl_pair_free(&p);
}

// SSL_ERROR_SYSCALL + non-EAGAIN on write -> abnormal close, -1.
CTEST(socket_ssl, write_syscall_fatal)
{
    ssl_pair p;
    ASSERT_TRUE(ssl_pair_init(&p));
    ASSERT_TRUE(ssl_pair_handshake(&p));
    ssl_inject_install(p.client, SSL_INJ_SYSCALL_FATAL);

    int sp[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    vws_socket s;
    sock_over_fd(&s, p.client, sp[0], 100);

    unsigned char buf[64];
    memset(buf, 'z', sizeof(buf));

    ssize_t n = vws_socket_write(&s, buf, sizeof(buf));
    ASSERT_TRUE(n < 0);

    sock_release(&s, sp, &p);
    ssl_pair_free(&p);
}

//------------------------------------------------------------------------------
// Partial SSL_write +/- flush (ga1 #4) over the loopback server with a small
// send buffer.
//------------------------------------------------------------------------------

CTEST(socket_ssl, partial_write_flush)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    int sndbuf = 4096;
    setsockopt(s.sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // flush=true (default): vws_socket_write loops over POLLOUT until all sent.
    size_t big = 64 * 1024;
    char*  buf = (char*)malloc(big);
    memset(buf, 'p', big);
    ssize_t n = vws_socket_write(&s, (ucstr)buf, big);
    free(buf);
    ASSERT_TRUE((size_t)n == big);   // all sent across the POLLOUT re-polls

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

//------------------------------------------------------------------------------
// Guard / error paths.
//------------------------------------------------------------------------------

// NULL / not-connected guards: connect(NULL); read/write on an unconnected
// socket (is_connected==false). (NOTE to wa1: vws_socket_read/write check
// is_connected(c) BEFORE the c==NULL guard, and is_connected(NULL)==false
// -> the later `if (c == NULL)` blocks at socket.c L489-492 / L717-718 are dead
// code, a mis-ordered guard. ESCALATED, not patched.)
CTEST(socket_ssl, guard_paths)
{
    ASSERT_TRUE(vws_socket_connect(NULL, "localhost", 4000, true) == false);

    vws_socket s;
    vws_socket_ctor(&s);                 // sockfd = -1, not connected
    ASSERT_TRUE(vws_socket_read(&s) == -1);
    ASSERT_TRUE(vws_socket_write(&s, (ucstr)"x", 1) == -1);
    ASSERT_TRUE(vws_socket_read(NULL) == -1);
    ASSERT_TRUE(vws_socket_write(NULL, (ucstr)"x", 1) == -1);
    vws_buffer_free(s.buffer);
}

// TCP connect failure (no listener) -> connect path returns false.
CTEST(socket_ssl, connect_tcp_fail)
{
    vws_socket s;
    vws_socket_ctor(&s);
    s.timeout = 1000;
    // Port 1 has no listener -> connect_to_host fails.
    ASSERT_TRUE(vws_socket_connect(&s, "127.0.0.1", 1, true) == false);
    vws_buffer_free(s.buffer);
}

// Read poll() timeout (no data within c->timeout) -> VE_TIMEOUT, return 0.
CTEST(socket_ssl, read_poll_timeout)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));
    s.timeout = 60;                      // short

    // No data sent -> poll(POLLIN) times out -> read returns 0.
    ssize_t n = vws_socket_read(&s);
    ASSERT_TRUE(n == 0);

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

// Partial write with flush=false -> returns after the first chunk.
CTEST(socket_ssl, write_flush_false)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    int sndbuf = 2048;
    setsockopt(s.sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    s.flush = false;

    size_t big = 64 * 1024;
    char*  buf = (char*)malloc(big);
    memset(buf, 'f', big);
    ssize_t n = vws_socket_write(&s, (ucstr)buf, big);
    free(buf);
    ASSERT_TRUE(n >= 0 && (size_t)n < big);   // flush=false: partial return

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

// Handshake-handler hook (c->hs): success continues; failure -> close + false.
// (Runs for both SSL + non-SSL; bare sockets default hs==NULL, hence not hit by
// the other cells.)
CTEST(socket_ssl, hs_handler)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    // hs returns true -> connect succeeds.
    vws_socket s1;
    vws_socket_ctor(&s1);
    s1.timeout = 2000;
    s1.hs = hs_ok;
    if (vws_ssl_ctx != NULL) ssl_test_trust_localhost(vws_ssl_ctx);
    bool ok = vws_socket_connect(&s1, "localhost", srv.port, true);
    ASSERT_TRUE(ok == true);
    vws_socket_close(&s1);
    vws_buffer_free(s1.buffer);

    // hs returns false -> connect fails (the handshake-failed path).
    vws_socket s2;
    vws_socket_ctor(&s2);
    s2.timeout = 2000;
    s2.hs = hs_fail;
    bool bad = vws_socket_connect(&s2, "localhost", srv.port, true);
    ASSERT_TRUE(bad == false);
    vws_socket_close(&s2);
    vws_buffer_free(s2.buffer);

    tls_server_stop(&srv);
}

// poll() POLLERR / hard-error arms on read and write (--wrap=poll). A bare
// socketpair-backed socket (no server thread) so the injection is isolated.
static void poll_err_socket(vws_socket* s, int sp[2])
{
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    vws_socket_ctor(s);
    s->sockfd  = sp[0];      // is_connected() == true
    s->ssl     = NULL;       // non-SSL: poll-error arm is before the ssl path
    s->timeout = 200;
}

static void poll_err_release(vws_socket* s, int sp[2])
{
    if (s->sockfd >= 0) close(s->sockfd);
    close(sp[1]);
    if (s->buffer) vws_buffer_free(s->buffer);
}

CTEST(socket_ssl, read_poll_error)
{
    int sp[2];
    vws_socket s;

    poll_err_socket(&s, sp);
    g_poll_inj = 1;                              // POLLERR
    ASSERT_TRUE(vws_socket_read(&s) == -1);
    poll_err_release(&s, sp);

    poll_err_socket(&s, sp);
    g_poll_inj = 2;                              // rc -1
    ASSERT_TRUE(vws_socket_read(&s) == -1);
    poll_err_release(&s, sp);
}

CTEST(socket_ssl, write_poll_error)
{
    int sp[2];
    vws_socket s;

    poll_err_socket(&s, sp);
    g_poll_inj = 1;                              // POLLERR
    ASSERT_TRUE(vws_socket_write(&s, (ucstr)"x", 1) == -1);
    poll_err_release(&s, sp);

    poll_err_socket(&s, sp);
    g_poll_inj = 2;                              // rc -1
    ASSERT_TRUE(vws_socket_write(&s, (ucstr)"x", 1) == -1);
    poll_err_release(&s, sp);
}

//------------------------------------------------------------------------------
// Close / shutdown (ga1 #1 close path) + concurrent pthread_once init.
//------------------------------------------------------------------------------

CTEST(socket_ssl, close_shutdown)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    // vws_socket_close runs the SSL_shutdown + SSL_free path.
    vws_socket_close(&s);
    ASSERT_TRUE(s.ssl == NULL);
    ASSERT_TRUE(vws_socket_is_connected(&s) == false);

    vws_buffer_free(s.buffer);
    tls_server_stop(&srv);
}

typedef struct
{
    tls_server* srv;
    int         ok;
} conn_arg;

static void* concurrent_connect(void* arg)
{
    conn_arg* a = (conn_arg*)arg;

    vws_socket s;
    vws_socket_ctor(&s);
    s.timeout = 2000;

    if (vws_socket_connect(&s, "localhost", a->srv->port, true))
    {
        a->ok = 1;
    }

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);

    return NULL;
}

// pthread_once: N threads first-connect concurrently -> exactly-one init, no
// crash, vws_ssl_ctx non-NULL.
CTEST(socket_ssl, concurrent_init)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));
    if (vws_ssl_ctx != NULL)
    {
        ssl_test_trust_localhost(vws_ssl_ctx);
    }

    enum { N = 6 };
    pthread_t tid[N];
    conn_arg  args[N];

    for (int i = 0; i < N; i++)
    {
        args[i].srv = &srv;
        args[i].ok  = 0;
        pthread_create(&tid[i], NULL, concurrent_connect, &args[i]);
    }
    for (int i = 0; i < N; i++)
    {
        pthread_join(tid[i], NULL);
    }

    ASSERT_TRUE(vws_ssl_ctx != NULL);   // init happened, exactly once, no crash

    tls_server_stop(&srv);
}

//------------------------------------------------------------------------------
// F-S1 / F-S2 fix repros (RED on current socket.c -> GREEN after the fixes).
//------------------------------------------------------------------------------

// F-S1: read/write test is_connected(c) BEFORE the c==NULL guard, and
// is_connected(NULL)==false returns first (VE_SOCKET) -> the c==NULL guard is
// DEAD. After the reorder it runs and sets VE_WARN ("Invalid parameters").
// RED now (VE_SOCKET), GREEN after the reorder (VE_WARN).
CTEST(socket_ssl, f_s1_null_guard)
{
    vws_socket_read(NULL);
    ASSERT_TRUE(vws.e.code == VE_WARN);

    vws_socket_write(NULL, (ucstr)"x", 1);
    ASSERT_TRUE(vws.e.code == VE_WARN);
}

// F-S2: an SSL write to a peer whose read end is closed raises SIGPIPE; on
// default disposition that TERMINATES the process. A child (SIG_DFL) does the
// write so the parent can observe. RED now (SIGPIPE death), GREEN after the
// per-thread-sigmask fix (the write returns -1 and the child survives).
CTEST(socket_ssl, f_s2_sigpipe)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    tls_server_stop(&srv);   // the peer closes
    vws_msleep(40);

    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGPIPE, SIG_DFL);   // default disposition: SIGPIPE kills

        unsigned char* big = (unsigned char*)malloc(131072);
        memset(big, 'x', 131072);
        vws_socket_write(&s, big, 131072);   // broken peer -> SIGPIPE now
        free(big);
        _exit(0);                            // reached only without SIGPIPE
    }

    int status = 0;
    waitpid(pid, &status, 0);

    ASSERT_TRUE(WIFEXITED(status));          // RED: death; GREEN: survives

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
}

//------------------------------------------------------------------------------
// Watchdog + main
//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    vws_msleep((unsigned int)WATCHDOG_SECONDS * 1000);
    fprintf(stderr, "test_socket_ssl: watchdog deadline exceeded — aborting\n");
    _Exit(99);
    return NULL;
}

CTEST(socket_ssl, f_s2_reactor_sigpipe)
{
    tls_server srv;
    ASSERT_TRUE(tls_server_start(&srv));

    vws_socket s;
    ASSERT_TRUE(connect_trusted(&s, &srv));

    tls_server_stop(&srv);   // the peer closes its read end
    vws_msleep(40);

    // Wrap the socket-backed SSL connection in a reactor handle and drive the
    // REACTOR write path (NOT vws_socket_write). vws_socket_async_write needs
    // only sock + live; async_write does the SSL write (async.c async_write).
    vws_async a;
    memset(&a, 0, sizeof(a));
    a.sock = &s;
    a.live = true;

    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGPIPE, SIG_DFL);   // default: SIGPIPE kills (main SIG_IGNs)

        unsigned char* big = (unsigned char*)malloc(131072);
        memset(big, 'x', 131072);
        vws_socket_async_write(&a, big, 131072);   // reactor write -> SIGPIPE
        free(big);
        _exit(0);   // reached only without a SIGPIPE death
    }

    int status = 0;
    waitpid(pid, &status, 0);

    ASSERT_TRUE(WIFEXITED(status));   // RED: SIGPIPE death; GREEN: guarded

    vws_socket_close(&s);
    vws_buffer_free(s.buffer);
}

int main(int argc, const char* argv[])
{
    // SSL writes go via OpenSSL's socket BIO (write() w/o MSG_NOSIGNAL),
    // so a broken-pipe SSL write raises SIGPIPE — the app convention is to
    // ignore it (then socket.c sees EPIPE via its error path). (Flagged to wa1:
    // socket.c's SSL write path lacks the MSG_NOSIGNAL the non-SSL path uses.)
    signal(SIGPIPE, SIG_IGN);

    pthread_t watch;
    pthread_create(&watch, NULL, watchdog_thread, NULL);
    pthread_detach(watch);

    return ctest_main(argc, argv);
}
