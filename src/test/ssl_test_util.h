#ifndef VWS_SSL_TEST_UTIL_H
#define VWS_SSL_TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <pthread.h>
#endif

#include <openssl/ssl.h>

#include "socket.h"

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// vws SSL test harness (TEST-ONLY — never linked into production)
//
// Two facilities, both network-free, for the COMPLETE-SSL coverage bar
// (vws-ssl-coverage-matrix.md / -spec-ga1.md):
//
//   D1  A test-only self-signed LOCALHOST cert/key (files/localhost-*.pem, SAN
//       localhost + 127.0.0.1) plus ssl_test_trust_localhost(), so the positive
//       hostname/peer-verify path runs against wss://localhost with NO network
//       and NO production verify init.
//
//   D2  Two in-process SSL drivers with FULL control of byte delivery:
//         - ssl_pair_*  : a BIO-pair dual-SSL harness (no fd) that runs a REAL
//                         TLS1.2 handshake + a REAL SSL_renegotiate, for the
//                         sync socket.c / cnx SSL cases.
//         - tls_server_* + asocket_tls_connect : a real loopback-fd TLS echo
//                         server the test fully controls (renegotiate / clean
//                         SSL_shutdown / corrupt-record on command), for the
//                         REACTOR (async.c) state machine, which only reaches
//                         its static read/write/apply path through an fd.
//
// Self-bounding: every facility tears down its threads/sockets/SSL on free; the
// callers add the iteration caps + wall-clock _Exit watchdog.
//------------------------------------------------------------------------------

#define SSL_TEST_LOCALHOST_CERT "files/localhost-cert.pem"
#define SSL_TEST_LOCALHOST_KEY  "files/localhost-key.pem"

/**
 * @brief Trust the D1 test-only localhost CA on a CLIENT ctx so a positive
 *        peer/hostname verify succeeds against the test server. TEST-ONLY:
 *        never call from production init (it widens the trust store).
 *
 * @param ctx The client SSL_CTX
 * @return true on success
 */
bool ssl_test_trust_localhost(SSL_CTX* ctx);

/**
 * @brief Reset a client ctx's verify trust to an EMPTY store (a fresh
 *        X509_STORE) so a verify cell can deterministically exercise the
 *        untrusted-negative path regardless of cell order on the shared
 *        process-global ctx. TEST-ONLY.
 *
 * @param ctx The client SSL_CTX
 */
void ssl_test_reset_trust(SSL_CTX* ctx);

//------------------------------------------------------------------------------
// D2a — BIO-pair in-process dual-SSL (no fd, no network). A client SSL and a
// server SSL wired back-to-back through a BIO pair; pumps a REAL TLS1.2
// handshake and can drive a REAL server-initiated renegotiation so the client
// genuinely observes a cross-direction WANT during application I/O.
//------------------------------------------------------------------------------

typedef struct
{
    SSL_CTX* client_ctx;       /**< client side ctx (TLS1.2 pinned)    */
    SSL_CTX* server_ctx;       /**< server side ctx (localhost cert)   */
    SSL*     client;           /**< the client SSL                     */
    SSL*     server;           /**< the in-proc server SSL             */
    BIO*     client_wbio;      /**< client write end of the pair       */
    BIO*     server_wbio;      /**< server write end of the pair       */
} ssl_pair;

/**
 * @brief Build the two ctxs + SSLs + BIO pair (TLS1.2 pinned both sides). The
 *        client trusts the D1 localhost cert; the server presents it.
 *
 * @param p The pair (zeroed + populated)
 * @return true on success
 */
bool ssl_pair_init(ssl_pair* p);

/**
 * @brief Pump the handshake to completion on both sides (drives the BIO pair
 *        until both SSL_is_init_finished).
 *
 * @param p The pair
 * @return true once both sides are established
 */
bool ssl_pair_handshake(ssl_pair* p);

/**
 * @brief Flush any queued bytes across the pair in BOTH directions once.
 *
 * @param p The pair
 */
void ssl_pair_pump(ssl_pair* p);

/**
 * @brief Begin a REAL server-initiated TLS1.2 renegotiation (SSL_renegotiate +
 *        SSL_do_handshake on the server). The client then observes the
 *        cross-direction WANT on its next SSL_read/SSL_write.
 *
 * @param p The pair
 * @return true on success
 */
bool ssl_pair_renegotiate(ssl_pair* p);

/**
 * @brief Tear the pair down (SSLs, ctxs, BIOs).
 *
 * @param p The pair
 */
void ssl_pair_free(ssl_pair* p);

//------------------------------------------------------------------------------
// D2c — mock-BIO transport-error injector. Replaces a handshake-complete SSL's
// BIO with a mock that returns a chosen transport error on the next read/write,
// for forcing the reactor's SSL error-classification arms (SSL_ERROR_SYSCALL +
// errno) that a real transport does not produce on demand.
//------------------------------------------------------------------------------

typedef enum
{
    SSL_INJ_SYSCALL_EAGAIN = 1,   /**< -1, no retry, errno=EAGAIN -> SYSCALL  */
    SSL_INJ_SYSCALL_FATAL         /**< -1, no retry, errno=ECONNRESET */
} ssl_inject_mode;

/**
 * @brief Replace ssl's BIO (both read + write) with a mock that injects
 *        `mode` on every read/write op. The SSL must be handshake-complete; the
 *        old BIO is freed by SSL_set_bio. The mock is owned by the SSL.
 *
 * @param ssl  A handshake-complete SSL (e.g. ssl_pair.client)
 * @param mode The error to inject
 */
void ssl_inject_install(SSL* ssl, ssl_inject_mode mode);

/**
 * @brief Like ssl_inject_install, but injects the error only on the 1st write,
 *        then lets writes succeed — so a blocking caller (socket.c write) exits
 *        its retry loop after the errno arm is exercised once.
 *
 * @param ssl  A handshake-complete SSL
 * @param mode The error to inject once
 */
void ssl_inject_install_once(SSL* ssl, ssl_inject_mode mode);

//------------------------------------------------------------------------------
// D2b — real loopback-fd TLS echo server (the reactor's SSL machine needs a
// pollable fd). Runs on its own thread; echoes WS-agnostic raw bytes; the test
// flips a control flag to drive a renegotiation, a clean SSL_shutdown, or a
// corrupt-record fault on the live connection.
//------------------------------------------------------------------------------

typedef enum
{
    TLS_SRV_ECHO = 0,    /**< steady echo                                 */
    TLS_SRV_RENEG,       /**< SSL_renegotiate + stop reading (force WANT) */
    TLS_SRV_SHUTDOWN,    /**< SSL_shutdown (clean close_notify/ZERO_RETURN)*/
    TLS_SRV_CORRUPT,     /**< raw garbage on the fd (FATAL protocol error) */
    TLS_SRV_DRAIN        /**< sink: read + count rx, do NOT echo (no
                              backpressure -- for the partial-write cell) */
} tls_srv_cmd;

typedef struct
{
    int              port;       /**< the bound loopback port (out)       */
    atomic_int       running;    /**< the server is accepting             */
    atomic_int       stop;       /**< the test asks the server to exit    */
    atomic_int       cmd;        /**< a tls_srv_cmd the server applies     */
    SSL_CTX*         ctx;        /**< server ctx (localhost cert)         */
    int              listenfd;   /**< the listening socket                */
    pthread_t        tid;        /**< the server thread                   */
    atomic_long      rx;         /**< bytes read from the client          */
    atomic_long      tx;         /**< bytes echoed back to the client     */
} tls_server;

/**
 * @brief Start the loopback TLS echo server on an ephemeral port; blocks until
 *        it is accepting. The bound port lands in s->port.
 *
 * @param s The server (zeroed + populated)
 * @return true on success
 */
bool tls_server_start(tls_server* s);

/**
 * @brief Ask the live connection to apply a command (reneg/shutdown/corrupt)
 *        on its next service iteration.
 *
 * @param s   The server
 * @param cmd The command
 */
void tls_server_command(tls_server* s, tls_srv_cmd cmd);

/**
 * @brief Stop the server thread + close its sockets.
 *
 * @param s The server
 */
void tls_server_stop(tls_server* s);

/**
 * @brief Build a CLIENT vws_socket connected to the running TLS server with a
 *        completed (blocking) handshake: TCP connect + SSL_connect + trust the
 *        D1 localhost cert. The reactor then drives steady-state I/O over the
 *        returned non-NULL fd/SSL. The caller owns teardown (closes fd, frees
 *        SSL via the socket's normal path).
 *
 * @param sock The socket to populate (zeroed first by the caller)
 * @param port The server port
 * @return true on a completed handshake
 */
bool asocket_tls_connect(vws_socket* sock, int port);

#ifdef __cplusplus
}
#endif

#endif /* VWS_SSL_TEST_UTIL_H */
