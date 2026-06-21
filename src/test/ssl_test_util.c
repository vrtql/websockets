#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509v3.h>

#include "vws.h"
#include "socket.h"
#include "ssl_test_util.h"

//------------------------------------------------------------------------------
// D1 — trust the test-only localhost CA on a client ctx.
//------------------------------------------------------------------------------

bool ssl_test_trust_localhost(SSL_CTX* ctx)
{
    if (ctx == NULL)
    {
        return false;
    }

    if (SSL_CTX_load_verify_locations(ctx, SSL_TEST_LOCALHOST_CERT, NULL) != 1)
    {
        return false;
    }

    return true;
}

void ssl_test_reset_trust(SSL_CTX* ctx)
{
    // Install a fresh empty verify store so the caller controls trust
    // deterministically regardless of what other cells did to a shared ctx.
    // SSL_CTX_set_cert_store frees the previously-set store.
    SSL_CTX_set_cert_store(ctx, X509_STORE_new());
}

//------------------------------------------------------------------------------
// Shared ctx builders (TLS1.2 pinned so the renegotiation cases are valid;
// TLS1.3 removed renegotiation, RFC 8446).
//------------------------------------------------------------------------------

static SSL_CTX* client_ctx_tls12(bool verify)
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());

    if (ctx == NULL)
    {
        return NULL;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);

    if (verify == true)
    {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

        if (ssl_test_trust_localhost(ctx) == false)
        {
            SSL_CTX_free(ctx);
            return NULL;
        }
    }
    else
    {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    return ctx;
}

static SSL_CTX* server_ctx_tls12(void)
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());

    if (ctx == NULL)
    {
        return NULL;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(ctx, SSL_TEST_LOCALHOST_CERT,
                                     SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, SSL_TEST_LOCALHOST_KEY,
                                    SSL_FILETYPE_PEM) != 1)
    {
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

//------------------------------------------------------------------------------
// D2a — BIO-pair in-process dual-SSL (no fd, no network).
//------------------------------------------------------------------------------

bool ssl_pair_init(ssl_pair* p)
{
    memset(p, 0, sizeof(*p));

    p->client_ctx = client_ctx_tls12(false);   // the pair tests the machine,
    p->server_ctx = server_ctx_tls12();         // not the verify path

    if (p->client_ctx == NULL || p->server_ctx == NULL)
    {
        ssl_pair_free(p);
        return false;
    }

    p->client = SSL_new(p->client_ctx);
    p->server = SSL_new(p->server_ctx);

    if (p->client == NULL || p->server == NULL)
    {
        ssl_pair_free(p);
        return false;
    }

    // A BIO pair: bytes written to one half are readable on the other. Each
    // SSL takes ownership of its half (freed by SSL_free).
    if (BIO_new_bio_pair(&p->client_wbio, 0, &p->server_wbio, 0) != 1)
    {
        ssl_pair_free(p);
        return false;
    }

    SSL_set_bio(p->client, p->client_wbio, p->client_wbio);
    SSL_set_bio(p->server, p->server_wbio, p->server_wbio);

    SSL_set_connect_state(p->client);
    SSL_set_accept_state(p->server);

    return true;
}

bool ssl_pair_handshake(ssl_pair* p)
{
    for (int i = 0; i < 100; i++)
    {
        if (SSL_is_init_finished(p->client) && SSL_is_init_finished(p->server))
        {
            return true;
        }

        SSL_do_handshake(p->client);
        SSL_do_handshake(p->server);
    }

    return SSL_is_init_finished(p->client) && SSL_is_init_finished(p->server);
}

void ssl_pair_pump(ssl_pair* p)
{
    // The BIO pair shuttles bytes directly; nudge both state machines so queued
    // handshake/app records cross.
    SSL_do_handshake(p->client);
    SSL_do_handshake(p->server);
}

bool ssl_pair_renegotiate(ssl_pair* p)
{
    if (SSL_renegotiate(p->server) != 1)
    {
        return false;
    }

    // Emit the HelloRequest; the client observes the cross-direction on its
    // next SSL_read/SSL_write of application data.
    SSL_do_handshake(p->server);

    return true;
}

void ssl_pair_free(ssl_pair* p)
{
    if (p->client != NULL) SSL_free(p->client);   // frees its BIO-pair half
    if (p->server != NULL) SSL_free(p->server);

    if (p->client_ctx != NULL) SSL_CTX_free(p->client_ctx);
    if (p->server_ctx != NULL) SSL_CTX_free(p->server_ctx);

    memset(p, 0, sizeof(*p));
}

//------------------------------------------------------------------------------
// D2c — mock-BIO transport-error injector.
//------------------------------------------------------------------------------

typedef struct
{
    int mode;     /**< ssl_inject_mode */
    int once;     /**< 1 = inject only the FIRST write, then succeed */
    int fired;    /**< the one-shot has fired */
} mock_data;

static int mock_inj_errno(BIO* b)
{
    mock_data* d = (mock_data*)BIO_get_data(b);

    BIO_clear_retry_flags(b);   // no retry -> SSL maps to SSL_ERROR_SYSCALL

    errno = (d->mode == SSL_INJ_SYSCALL_FATAL) ? ECONNRESET : EAGAIN;

    return -1;
}

static int mock_bread(BIO* b, char* buf, int len)
{
    (void)buf;
    (void)len;
    return mock_inj_errno(b);
}

static int mock_bwrite(BIO* b, const char* buf, int len)
{
    mock_data* d = (mock_data*)BIO_get_data(b);

    // One-shot: error the first write, then "succeed" so the SSL op completes
    // (lets a blocking socket.c write loop exit after the errno arm fires).
    if (d->once && d->fired)
    {
        return len;
    }
    d->fired = 1;

    return mock_inj_errno(b);
}

static long mock_ctrl(BIO* b, int cmd, long n, void* p)
{
    (void)b;
    (void)n;
    (void)p;
    return (cmd == BIO_CTRL_FLUSH) ? 1 : 0;
}

static int mock_create(BIO* b)
{
    BIO_set_init(b, 1);
    return 1;
}

static int mock_destroy(BIO* b)
{
    mock_data* d = (mock_data*)BIO_get_data(b);
    free(d);
    return 1;
}

static BIO_METHOD* mock_method(void)
{
    static BIO_METHOD* m = NULL;

    if (m == NULL)
    {
        m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK,
                         "ssl-inject");
        BIO_meth_set_read(m, mock_bread);
        BIO_meth_set_write(m, mock_bwrite);
        BIO_meth_set_ctrl(m, mock_ctrl);
        BIO_meth_set_create(m, mock_create);
        BIO_meth_set_destroy(m, mock_destroy);
    }

    return m;
}

static void inject_install(SSL* ssl, ssl_inject_mode mode, int once)
{
    BIO* b = BIO_new(mock_method());

    mock_data* d = (mock_data*)calloc(1, sizeof(mock_data));
    d->mode = (int)mode;
    d->once = once;
    BIO_set_data(b, d);

    // One BIO for both directions; SSL_set_bio frees the (shared) old pair BIO.
    SSL_set_bio(ssl, b, b);
}

void ssl_inject_install(SSL* ssl, ssl_inject_mode mode)
{
    inject_install(ssl, mode, 0);
}

void ssl_inject_install_once(SSL* ssl, ssl_inject_mode mode)
{
    inject_install(ssl, mode, 1);
}

//------------------------------------------------------------------------------
// D2b — real loopback-fd TLS echo server (POSIX). The reactor's SSL machine
// only reaches its static read/write/apply path through a pollable fd, so the
// renegotiation / clean-close / corrupt-record cells need a real fd peer the
// test fully controls.
//------------------------------------------------------------------------------

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

static void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// One accepted connection's service loop. Blocking SSL over a non-blocking fd:
// poll() gates each step so stop/cmd stay responsive; WANT_* loops.
static void serve_one(tls_server* s, int conn)
{
    SSL* ssl = SSL_new(s->ctx);

    if (ssl == NULL)
    {
        close(conn);
        return;
    }

    SSL_set_fd(ssl, conn);

    // Accept the handshake (blocking fd for the handshake keeps it simple).
    if (SSL_accept(ssl) != 1)
    {
        SSL_free(ssl);
        close(conn);
        return;
    }

    set_nonblocking(conn);

    char buf[4096];

    while (s->stop == 0)
    {
        int cmd = s->cmd;

        if (cmd == TLS_SRV_SHUTDOWN)
        {
            SSL_shutdown(ssl);     // clean close_notify -> client ZERO_RETURN
            break;
        }

        if (cmd == TLS_SRV_CORRUPT)
        {
            // Inject raw non-record garbage into the live TLS stream -> the
            // client's next SSL_read fails decrypt -> SSL_ERROR_SSL (FATAL).
            unsigned char junk[32];
            memset(junk, 0xff, sizeof(junk));
            (void)send(conn, junk, sizeof(junk), MSG_NOSIGNAL);
            break;
        }

        if (cmd == TLS_SRV_RENEG)
        {
            // Real server-initiated TLS1.2 renegotiation. Stop echoing while it
            // in flight so the client reneg writes can back up the pipe and
            // surface the cross-direction WANT on its SSL_read.
            SSL_renegotiate(ssl);
            SSL_do_handshake(ssl);
            s->cmd = TLS_SRV_ECHO;
        }

        // Only poll the socket when OpenSSL has no buffered plaintext: after a
        // partial record read the remainder lives in SSL_pending, invisible to
        // poll(POLLIN) — polling there would stall mid-record. Drain it first.
        if (SSL_pending(ssl) == 0)
        {
            struct pollfd pfd;
            pfd.fd      = conn;
            pfd.events  = POLLIN;
            pfd.revents = 0;

            if (poll(&pfd, 1, 50) <= 0)
            {
                continue;
            }
        }

        int n = SSL_read(ssl, buf, (int)sizeof(buf));

        if (n > 0)
        {
            s->rx += n;

            if (cmd == TLS_SRV_DRAIN)
            {
                // Sink mode: count, never echo. A small throttle keeps the
                // client's (deliberately small) send buffer full so SSL_write
                // fragments -> the reactor exercises the partial-write re-arm,
                // while still draining steadily (no permanent backpressure).
                vws_msleep(2);
                continue;
            }

            int off = 0;

            while (off < n && s->stop == 0)
            {
                int w = SSL_write(ssl, buf + off, n - off);

                if (w > 0)
                {
                    off += w;
                    s->tx += w;
                    continue;
                }

                int e = SSL_get_error(ssl, w);

                // Honor backpressure via poll() (NOT a busy spin): the echo
                // waits for the client to drain its receive buffer.
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    struct pollfd p = { conn, POLLOUT, 0 };
                    poll(&p, 1, 50);
                    continue;
                }

                if (e == SSL_ERROR_WANT_READ)
                {
                    struct pollfd p = { conn, POLLIN, 0 };
                    poll(&p, 1, 50);
                    continue;
                }

                break;
            }

            continue;
        }

        int e = SSL_get_error(ssl, n);

        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
        {
            continue;
        }

        (void)e;
        break;   // ZERO_RETURN / SYSCALL / SSL -> the peer is gone
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(conn);
}

static void* tls_server_thread(void* arg)
{
    tls_server* s = (tls_server*)arg;

    s->running = 1;

    while (s->stop == 0)
    {
        struct pollfd pfd;
        pfd.fd      = s->listenfd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 50) <= 0)
        {
            continue;
        }

        int conn = accept(s->listenfd, NULL, NULL);

        if (conn < 0)
        {
            continue;
        }

        s->cmd = TLS_SRV_ECHO;
        serve_one(s, conn);
    }

    return NULL;
}

bool tls_server_start(tls_server* s)
{
    memset(s, 0, sizeof(*s));
    s->listenfd = -1;

    s->ctx = server_ctx_tls12();

    if (s->ctx == NULL)
    {
        return false;
    }

    s->listenfd = socket(AF_INET, SOCK_STREAM, 0);

    if (s->listenfd < 0)
    {
        SSL_CTX_free(s->ctx);
        s->ctx = NULL;
        return false;
    }

    int one = 1;
    setsockopt(s->listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;                       // ephemeral port

    if (bind(s->listenfd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(s->listenfd, 4) != 0)
    {
        close(s->listenfd);
        s->listenfd = -1;
        SSL_CTX_free(s->ctx);
        s->ctx = NULL;
        return false;
    }

    struct sockaddr_in bound;
    socklen_t          blen = sizeof(bound);

    if (getsockname(s->listenfd, (struct sockaddr*)&bound, &blen) != 0)
    {
        close(s->listenfd);
        s->listenfd = -1;
        SSL_CTX_free(s->ctx);
        s->ctx = NULL;
        return false;
    }

    s->port = (int)ntohs(bound.sin_port);

    pthread_create(&s->tid, NULL, tls_server_thread, s);

    while (s->running == 0)
    {
        vws_msleep(20);
    }

    return true;
}

void tls_server_command(tls_server* s, tls_srv_cmd cmd)
{
    s->cmd = (int)cmd;
}

void tls_server_stop(tls_server* s)
{
    s->stop = 1;
    pthread_join(s->tid, NULL);

    if (s->listenfd >= 0)
    {
        close(s->listenfd);
        s->listenfd = -1;
    }

    if (s->ctx != NULL)
    {
        SSL_CTX_free(s->ctx);
        s->ctx = NULL;
    }
}

bool asocket_tls_connect(vws_socket* sock, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((unsigned short)port);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return false;
    }

    // VERIFY_PEER + hostname "localhost" -> this connect also exercises the
    // positive verify path (D1 cert SAN covers localhost / 127.0.0.1).
    SSL_CTX* ctx = client_ctx_tls12(true);

    if (ctx == NULL)
    {
        close(fd);
        return false;
    }

    SSL* ssl = SSL_new(ctx);

    // SSL holds its own ctx reference; drop ours so SSL_free reclaims the ctx
    // (no dangling ctx, valgrind-clean).
    SSL_CTX_free(ctx);

    if (ssl == NULL)
    {
        close(fd);
        return false;
    }

    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, "localhost");

    X509_VERIFY_PARAM* vp = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    X509_VERIFY_PARAM_set1_host(vp, "localhost", 0);

    if (SSL_connect(ssl) != 1)
    {
        SSL_free(ssl);
        close(fd);
        return false;
    }

    sock->sockfd     = fd;
    sock->ssl        = ssl;
    sock->buffer     = vws_buffer_new();
    sock->timeout    = 1000;
    sock->flush      = true;
    sock->hs         = NULL;
    sock->disconnect = NULL;
    sock->data       = NULL;

    return true;
}

#else  /* non-POSIX: the fd server is unavailable; the cells using it skip. */

bool tls_server_start(tls_server* s)            { (void)s; return false; }
void tls_server_command(tls_server* s, tls_srv_cmd c) { (void)s; (void)c; }
void tls_server_stop(tls_server* s)             { (void)s; }
bool asocket_tls_connect(vws_socket* sk, int p)
{ (void)sk; (void)p; return false; }

#endif
