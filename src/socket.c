#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

#if defined(__windows__)
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601  // Windows 7 or later
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <openssl/rand.h>

#include "socket.h"

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

/** @brief Defines the various states of a WebSocket connection */
typedef enum
{
    /** The connection with the client is in the initial SSL handshake phase. */
    CNX_SSL_INIT = (1 << 3),

} socket_flags_t;

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
/* pthread_once guards the one-time SSL_library_init / SSL_CTX_new path
 * in vws_socket_connect. Without this, concurrent first-time connects
 * race on the global vws_ssl_ctx pointer (multiple SSL_CTX_new calls
 * each overwriting the global; concurrent SSL_new readers may see a
 * half-constructed or freed context -> segfault).
 *
 * Note: the existing vws_is_flag(&vws.state, CNX_SSL_INIT) check is
 * useless for this purpose because `vws` is __thread (thread-local) —
 * every thread observes the flag as unset on its first call. The flag
 * is left in place for any code that reads it, but the actual init
 * gate is pthread_once on a process-global state. */
static pthread_once_t vws_ssl_init_once = PTHREAD_ONCE_INIT;

static void vws_ssl_init_do(void)
{
    SSL_library_init();
    RAND_poll();
    SSL_load_error_strings();

    vws_ssl_ctx = SSL_CTX_new(TLS_method());

    if (vws_ssl_ctx == NULL)
    {
        return;
    }

    /* Require TLS 1.2+ */
    SSL_CTX_set_min_proto_version(vws_ssl_ctx, TLS1_2_VERSION);

    /* Load system trust store */
    if (SSL_CTX_set_default_verify_paths(vws_ssl_ctx) != 1)
    {
        SSL_CTX_free(vws_ssl_ctx);
        vws_ssl_ctx = NULL;
        return;
    }

    /* Verify peer certs */
    SSL_CTX_set_verify(vws_ssl_ctx, SSL_VERIFY_PEER, NULL);
}
#endif

/**
 * @brief Connects to a host at a specific port and returns the connection
 *        status.
 *
 * @param host The host to connect to.
 * @param port The port to connect to.
 * @return The connection status, 0 if successful, an error code otherwise.
 *
 * @ingroup ConnectionFunctions
 */
static vws_sockfd_t connect_to_host(const char* host, const char* port);

/**
 * @brief  Sets a timeout on a socket read/write operations.
 *
 * @param fd The socket file descriptor.
 * @param sec The timeout value in seconds.
 * @return True if successful, false otherwise.
 *
 * @ingroup SocketFunctions
 */
static bool socket_set_timeout(vws_sockfd_t fd, int sec);

/**
 * @brief Calls handler for unexpected socket closure.
 */
static void socket_abnormal_close(vws_socket* c);

//------------------------------------------------------------------------------
//> Socket API
//------------------------------------------------------------------------------

vws_socket* vws_socket_new()
{
    vws_socket* c = (vws_socket*)vws.malloc(sizeof(vws_socket));
    memset(c, 0, sizeof(vws_socket));

    return vws_socket_ctor(c);
}

vws_socket* vws_socket_ctor(vws_socket* s)
{
    s->sockfd     = VWS_INVALID_SOCKET;
    s->buffer     = vws_buffer_new();
    s->ssl        = NULL;
    s->timeout    = 10000;
    s->data       = NULL;
    s->hs         = NULL;
    s->disconnect = NULL;
    s->flush      = true;

    return s;
}

void vws_socket_free(vws_socket* c)
{
    if (c == NULL)
    {
        return;
    }

    vws_socket_dtor(c);
}

void vws_socket_dtor(vws_socket* s)
{
    vws_socket_disconnect(s);

    // Free receive buffer
    vws_buffer_free(s->buffer);

    if (s->sockfd != VWS_INVALID_SOCKET)
    {
        #if defined(__windows__)
        closesocket(s->sockfd);
        #else
        close(s->sockfd);
        #endif
    }

    // Free connection
    vws.free(s);
}

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

void socket_abnormal_close(vws_socket* c)
{
    // If disconnect callback is registered
    if (c->disconnect != NULL)
    {
        // Call it
        c->disconnect(c);
    }

    vws_socket_close(c);
}

bool vws_socket_set_timeout(vws_socket* s, int sec)
{
    if (socket_set_timeout(s->sockfd, sec) == false)
    {
        return false;
    }

    // Set socket attribute, this will apply to poll().
    s->timeout = sec * 1000;

    return true;
}

bool socket_set_timeout(vws_sockfd_t fd, int sec)
{
    #if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    if (fd == VWS_INVALID_SOCKET)
    {
        vws.error(VE_RT, "Invalid socket descriptor");
        return false;
    }

    if (sec == -1)
    {
        sec = 0;
    }

    struct timeval tm;
    tm.tv_sec  = sec;
    tm.tv_usec = 0;

    // Set the send timeout
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tm, sizeof(tm)) < 0)
    {
        vws.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tm, sizeof(tm)) < 0)
    {
        vws.error(VE_SYS, "setsockopt failed");

        return false;
    }

    #elif defined(__windows__)

    if (fd == VWS_INVALID_SOCKET)
    {
        vws.error(VE_RT, "Invalid socket descriptor");
        return false;
    }

    // Convert from sec to ms for Windows
    DWORD tm = sec * 1000;

    if (sec == -1)
    {
        // Maximum value (136.17 years)
        sec = 4294967295;
    }

    // Set the send timeout
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (cstr)&tm, sizeof(tm)) < 0)
    {
        vws.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (cstr)&tm, sizeof(tm)) < 0)
    {
        vws.error(VE_SYS, "setsockopt failed");

        return false;
    }

    #else
    #error Platform not supported
    #endif

    vws.success();

    return true;
}

bool vws_socket_set_nonblocking(vws_sockfd_t sockfd)
{
    #if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    int flags = fcntl(sockfd, F_GETFL, 0);

    if (flags == -1)
    {
        vws.error(VE_SYS, "fcntl(sockfd, F_GETFL, 0) failed");

        return false;
    }

    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        vws.error(VE_SYS, "fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) failed");

        return false;
    }

    #elif defined(__windows__)

    unsigned long arg = 1;
    if (ioctlsocket(sockfd, FIONBIO, &arg) == SOCKET_ERROR)
    {
        vws.error(VE_SYS, "ioctlsocket(sockfd, FIONBIO, &arg)");

        return false;
    }

    #else
    #error Platform not supported
    #endif

    vws.success();

    return true;
}

bool vws_socket_is_connected(vws_socket* c)
{
    if (c == NULL)
    {
        return false;
    }

    return c->sockfd != VWS_INVALID_SOCKET;
}

bool vws_socket_connect(vws_socket* c, cstr host, int port, bool ssl)
{
    if (c == NULL)
    {
        // Return early if failed to create a connection.
        vws.error(VE_RT, "Invalid connection pointer()");
        return false;
    }

    // Clear socket buffer in case it was previously used in other connection.
    vws_buffer_clear(c->buffer);

    if (ssl == true)
    {
#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
        /* pthread_once: exactly one thread runs vws_ssl_init_do across
         * the entire process; all other concurrent first callers block
         * until it completes; subsequent calls return immediately. */
        pthread_once(&vws_ssl_init_once, vws_ssl_init_do);

        if (vws_ssl_ctx == NULL)
        {
            vws.error(VE_SYS, "SSL init failed (vws_ssl_ctx is NULL)");
            return false;
        }
#else
        /* Non-POSIX path: keep the original (racy under threads) init.
         * Windows/etc. callers should not concurrently first-init. */
        if (vws_is_flag(&vws.state, CNX_SSL_INIT) == false)
        {
            SSL_library_init();
            RAND_poll();
            SSL_load_error_strings();

            vws_ssl_ctx = SSL_CTX_new(TLS_method());

            if (vws_ssl_ctx == NULL)
            {
                vws.error(VE_SYS, "Failed to create new SSL context");
                return false;
            }

            SSL_CTX_set_min_proto_version(vws_ssl_ctx, TLS1_2_VERSION);

            if (SSL_CTX_set_default_verify_paths(vws_ssl_ctx) != 1)
            {
                vws.error(VE_SYS, "SSL_CTX_set_default_verify_paths failed");
                return false;
            }

            SSL_CTX_set_verify(vws_ssl_ctx, SSL_VERIFY_PEER, NULL);
        }
#endif

        c->ssl = SSL_new(vws_ssl_ctx);

        if (c->ssl == NULL)
        {
            vws.error(VE_SYS, "Failed to create new SSL object");
            vws_socket_close(c);
            return false;
        }

        vws_set_flag(&vws.state, CNX_SSL_INIT);
    }

    char port_str[20];
    sprintf(port_str, "%d", port);
    c->sockfd = connect_to_host(host, port_str);

    if (c->sockfd == VWS_INVALID_SOCKET)
    {
        vws.error(VE_SYS, "Connection failed");
        vws_socket_close(c);
        return false;
    }

    // Set default timeout
    if (socket_set_timeout(c->sockfd, c->timeout/1000) == false)
    {
        // Error already set
        vws_socket_close(c);
        return false;
    }

    if (c->ssl != NULL)
    {
        SSL_set_fd(c->ssl, c->sockfd);

        /* SNI */
        SSL_set_tlsext_host_name(c->ssl, host);

        /* Hostname verification (OpenSSL 1.1.0+) */
        X509_VERIFY_PARAM *param = SSL_get0_param(c->ssl);
        #ifdef X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        #endif
        X509_VERIFY_PARAM_set1_host(param, host, 0);

        int ret = SSL_connect(c->ssl);

        if (ret <= 0)
        {
            char err_msg[128];
            int ssl_err = SSL_get_error(c->ssl, ret);
            snprintf( err_msg,
                      sizeof(err_msg),
                      "SSL_connect failed: %s (error %d)",
                      ssl_err == SSL_ERROR_SYSCALL ? "System call" :
                      ssl_err == SSL_ERROR_SSL ? "SSL protocol" : "Other",
                      ssl_err );
            vws.error(VE_SYS, "%s", err_msg);

            vws_socket_close(c);
            return false;
        }
    }

    #if defined(__bsd__)

    // Disable SIGPIPE
    int val = 1;
    setsockopt(c->sockfd, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val));

    #endif

    // Go into non-blocking mode as we are using poll() for socket_read() and
    // socket_write().
    if (vws_socket_set_nonblocking(c->sockfd) == false)
    {
        // Error already set
        vws_socket_close(c);
        return false;
    }

    // Check if handshake handler is registered
    if (c->hs != NULL)
    {
        if (c->hs(c) == false)
        {
            vws.error(VE_SYS, "Handshake failed");
            vws_socket_close(c);
            return false;
        }
    }

    vws.success();

    return true;
}

void vws_socket_disconnect(vws_socket* c)
{
    if (vws_socket_is_connected(c) == false)
    {
        return;
    }

    vws_socket_close(c);

    vws.success();
}

ssize_t vws_socket_read(vws_socket* c)
{
    // Default success unless error
    vws.success();

    // Validate input parameters (F-S1: the NULL guard MUST precede the
    // is_connected() check, which dereferences nothing but returns false for
    // NULL — leaving this block unreachable if it came second).
    if (c == NULL)
    {
        vws.error(VE_WARN, "Invalid parameters");
        return -1;
    }

    if (vws_socket_is_connected(c) == false)
    {
        vws.error(VE_SOCKET, "vws_socket_read()");
        return -1;
    }

    #if defined(__windows__)
    WSAPOLLFD fds;
    #else
    struct pollfd fds;
    #endif
    int poll_events = POLLIN;

openssl_reread:

    #if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    fds.fd     = c->sockfd;
    fds.events = poll_events;

    // [vws V-12] Retry poll() on EINTR (a delivered signal) instead of treating
    // the interrupt as a hard failure that tears the connection down.
    int rc;
    do
    {
        rc = poll(&fds, 1, c->timeout);
    }
    while (rc == -1 && errno == EINTR);

    // [vws V-12] poll() failure (rc < 0) leaves revents UNDEFINED
    // (POSIX) -- inspecting it read stack garbage that, with a stray POLLERR
    // bit, abnormal-closed a HEALTHY connection on any interrupting signal.
    // Only inspect revents when poll actually reported events (rc > 0); the
    // rc <= 0 cases fall to the rc checks below.
    if ((rc > 0) && (fds.revents & (POLLERR | POLLHUP | POLLNVAL)))
    {
        vws.error(VE_SOCKET, "Socket error during poll()");
        socket_abnormal_close(c);
        return -1;
    }

    #elif defined(__windows__)

    fds.fd     = c->sockfd;
    fds.events = POLLIN;

    int rc = WSAPoll(&fds, 1, c->timeout);

    if (rc == SOCKET_ERROR)
    {
        vws.error(VE_SOCKET, "Socket error during WSAPoll()");
        socket_abnormal_close(c);

        return -1;
    }

    #else
    #error Platform not supported
    #endif

    if (rc == -1)
    {
        vws.error(VE_RT, "poll() failed");
        return -1;
    }

    if (rc == 0)
    {
        vws.error(VE_TIMEOUT, "poll()");
        return 0;
    }

    ssize_t      n = 0;
    ucstr data   = &vws.sslbuf[0];
    ssize_t size = sizeof(vws.sslbuf);

    if (fds.revents & poll_events)
    {
        if (c->ssl != NULL)
        {
            // We need running total bc we may make multiple SSL_read() calls.
            int total = 0;

            // Drain all data from SSL buffer
            while ((n = ssl_read_nosigpipe(c->ssl, data, (int)size)) > 0)
            {
                // Process received data stored in buf
                total += n;
                vws_buffer_append(c->buffer, data, n);

                if (n < size && SSL_pending(c->ssl) == 0)
                {
                    // A short read alone does NOT mean the socket is drained:
                    // OpenSSL can still hold decrypted application data from an
                    // already-received TLS record (SSL_pending() > 0), and that
                    // data is invisible to poll() on the raw fd. Stopping here
                    // would leave it unread until a subsequent clean close
                    // (SSL_ERROR_ZERO_RETURN) surfaces as a read error and the
                    // pending bytes are discarded — truncating a large response
                    // body. Only break once OpenSSL has nothing buffered.
                    break;
                }
            }

            // Check for error conditions
            if (n <= 0)
            {
                int err = SSL_get_error(c->ssl, n);

                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
                {
                    // SSL needs to do something on socket in order to continue.

                    // If it wants to read data
                    if (err == SSL_ERROR_WANT_READ)
                    {
                        // We are done. We have emptied the read buffer.
                        return total;
                    }

                    // If it wants to write data
                    if (err == SSL_ERROR_WANT_WRITE)
                    {
                        // It's doing some internal negotiation and we need to
                        // help it along by running poll() for writes. Then we
                        // will return to SSL_read() in which SSL will send out
                        // the data it needs to.
                        poll_events = POLLOUT;
                        goto openssl_reread;
                    }
                }
                else if (err == SSL_ERROR_SYSCALL)
                {
                    #if defined(__windows__)

                    int err = WSAGetLastError();

                    if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)
                    {
                        vws.error(VE_TIMEOUT, "SSL_read()");
                        return 0;
                    }

                    #else

                    if ((errno == EWOULDBLOCK) || (errno == EAGAIN))
                    {
                        vws.error(VE_TIMEOUT, "SSL_read()");
                        return 0;
                    }

                    #endif
                }

                // Get the latest OpenSSL error
                char buf[256];
                unsigned long ssl_err = ERR_get_error();
                ERR_error_string_n(ssl_err, buf, sizeof(buf));
                vws.error(VE_SOCKET, "SSL_read() failed: %s", buf);

                // Close socket
                socket_abnormal_close(c);

                return -1;
            }
        }
        else
        {
            // Non-SSL socket is readable, perform recv() operation
            #if defined(__linux__) || defined(__sunos__)
            n = recv(c->sockfd, data, size, MSG_NOSIGNAL);
            #else
            n = recv(c->sockfd, data, size, 0);
            #endif

            if (n == 0)
            {
                vws.error(VE_SOCKET, "disconnect");

                // Close socket
                socket_abnormal_close(c);

                return -1;
            }

            if (n <= -1)
            {
                #if defined(__windows__)
                int err = WSAGetLastError();

                if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)
                {
                    vws.error(VE_TIMEOUT, "recv()");
                    return 0;
                }

                #else

                if ((errno == EWOULDBLOCK) || (errno == EAGAIN))
                {
                    vws.error(VE_TIMEOUT, "recv()");
                    return 0;
                }
                else
                {
                    // Error
                    char buf[256];
                    strerror_r(errno, buf, sizeof(buf));
                    vws.error(VE_WARN, "recv() failed: %s", buf);

                    // Close socket
                    socket_abnormal_close(c);

                    return -1;
                }
                #endif
            }

            // Should always be true if we get here
            if (n > 0)
            {
                vws_buffer_append(c->buffer, data, n);
            }
        }
    }

    return n;
}

// F-S2 / ga-fix (A) UNIFY: SSL_read and SSL_write both go via OpenSSL's socket
// BIO, which calls write() WITHOUT MSG_NOSIGNAL (SSL_read writes during a
// renegotiation), so a broken-pipe op raises SIGPIPE and -- under the default
// disposition -- terminates the process. libvai is EMBEDDED, so we must NOT
// change the process-wide signal disposition. Instead block SIGPIPE on THIS
// thread for the duration of the op and drain a SIGPIPE we generated before
// restoring the mask. Windows has no SIGPIPE, so the guards compile to a bare
// SSL op. These are the SHARED guards: the sync Client (vws_socket_read/write)
// AND the async reactor (async_read/async_write) route every SSL op through
// them, so there are NO raw SSL_read/SSL_write on the SSL path (enforced by the
// ssl_sigpipe_guard regression gate).

ssize_t ssl_read_nosigpipe(SSL* ssl, ucstr data, int len)
{
#if !defined(__windows__)

    sigset_t block, oldmask, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);

    // Was a SIGPIPE already pending before our op? If the query fails, assume
    // pending (conservative: never drain a signal we may not have generated).
    int was_pending = 1;
    if (sigpending(&pending) == 0)
    {
        was_pending = sigismember(&pending, SIGPIPE);
    }

    // Block SIGPIPE on THIS thread. If the mask call fails we did not block,
    // so we must neither drain nor restore below.
    int blocked = (pthread_sigmask(SIG_BLOCK, &block, &oldmask) == 0);

#endif

    ssize_t n = SSL_read(ssl, data, len);

#if !defined(__windows__)

    // Preserve the SSL op's errno across the sigmask/drain calls so the
    // caller's SSL_get_error + errno (SYSCALL arm) classification holds.
    int saved_errno = errno;

    if (blocked)
    {
        if (n <= 0 && was_pending == 0)
        {
            if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE))
            {
                // Drain the SIGPIPE our op generated so it does not fire on
                // restore (non-blocking: it is already pending).
                struct timespec zero = { 0, 0 };
                sigtimedwait(&block, NULL, &zero);
            }
        }

        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    }

    errno = saved_errno;

#endif

    return n;
}

ssize_t ssl_write_nosigpipe(SSL* ssl, const ucstr data, int len)
{
#if !defined(__windows__)

    sigset_t block, oldmask, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);

    // Was a SIGPIPE already pending before our op? If the query fails, assume
    // pending (conservative: never drain a signal we may not have generated).
    int was_pending = 1;
    if (sigpending(&pending) == 0)
    {
        was_pending = sigismember(&pending, SIGPIPE);
    }

    // Block SIGPIPE on THIS thread. If the mask call fails we did not block,
    // so we must neither drain nor restore below.
    int blocked = (pthread_sigmask(SIG_BLOCK, &block, &oldmask) == 0);

#endif

    ssize_t n = SSL_write(ssl, data, len);

#if !defined(__windows__)

    // Preserve the SSL op's errno across the sigmask/drain calls so the
    // caller's SSL_get_error + errno (SYSCALL arm) classification holds.
    int saved_errno = errno;

    if (blocked)
    {
        if (n <= 0 && was_pending == 0)
        {
            if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE))
            {
                // Drain the SIGPIPE our op generated so it does not fire on
                // restore (non-blocking: it is already pending).
                struct timespec zero = { 0, 0 };
                sigtimedwait(&block, NULL, &zero);
            }
        }

        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    }

    errno = saved_errno;

#endif

    return n;
}

ssize_t vws_socket_write(vws_socket* c, const ucstr data, size_t size)
{
    // Default success unless error
    vws.success();

    // Validate input parameters (F-S1: the NULL guard MUST precede the
    // is_connected() check, which returns false for NULL — leaving this block
    // unreachable if it came second).
    if (c == NULL || data == NULL || size == 0)
    {
        vws.error(VE_WARN, "Invalid parameters");
        return -1;
    }

    if (vws_socket_is_connected(c) == false)
    {
        vws.error(VE_SOCKET, "vws_socket_write()");
        return -1;
    }

    // But default we will keep looping until we have sent all the data
    size_t sent     = 0;
    int poll_events = POLLOUT;
    int iterations  = 0;
    while (sent < size)
    {
        // If we attempted at least one poll()/send()
        if (iterations++ > 0)
        {
            // And we are not set to flush mode, then we will return here,
            // sending back how much data we sent. The caller will need to
            // adjust the buffer accordingly.
            if (c->flush == false)
            {
                break;
            }
        }

        #if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

        struct pollfd fds;
        fds.fd     = c->sockfd;
        fds.events = poll_events;

        // [vws V-12] retry on EINTR (see the read path).
        int rc;
        do
        {
            rc = poll(&fds, 1, c->timeout);
        }
        while (rc == -1 && errno == EINTR);

        // [vws V-12] see the read path: revents is UNDEFINED on rc < 0,
        // so only inspect it when poll reported events (rc > 0).
        if ((rc > 0) && (fds.revents & (POLLERR | POLLHUP | POLLNVAL)))
        {
            vws.error(VE_SOCKET, "Socket error during poll()");
            socket_abnormal_close(c);
            return -1;
        }

        #elif defined(__windows__)

        WSAPOLLFD fds;
        fds.fd     = c->sockfd;
        fds.events = poll_events;

        int rc = WSAPoll(&fds, 1, c->timeout);

        if (rc == SOCKET_ERROR)
        {
            vws.error(VE_SOCKET, "Socket error during WSAPoll()");
            socket_abnormal_close(c);
            return -1;
        }

        #else
        #error Platform not supported
        #endif

        if (rc == -1)
        {
            vws.error(VE_SYS, "poll() failed");
            return -1;
        }

        // There was a timeout. Restart loop. Sends are all or nothing: we keep
        // pushing until either all the data goes or the connection
        // drops. Anything else is inconsistent state.
        if (rc == 0)
        {
            // Keep going.
            continue;
        }

        ssize_t n = 0;
        if (fds.revents & poll_events)
        {
            if (c->ssl != NULL)
            {
                // SSL socket is writable, perform SSL_write() operation
                // (F-S2: SIGPIPE-guarded; see ssl_write_nosigpipe).
                n = ssl_write_nosigpipe(c->ssl, data + sent,
                                        (int)(size - sent));

                if (n <= 0)
                {
                    int err = SSL_get_error(c->ssl, n);

                    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
                    {
                        // The SSL socket is still open but would block on write

                        if (err == SSL_ERROR_WANT_READ)
                        {
                            poll_events = POLLIN;
                        }
                        else
                        {
                            poll_events = POLLOUT;
                        }

                        // Keep going
                        continue;
                    }
                    else if (err == SSL_ERROR_SYSCALL)
                    {
                        #if defined(__windows__)

                        int err = WSAGetLastError();

                        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)
                        {
                            // Timeout. Keep going.
                            continue;
                        }

                        #else

                        if ((errno == EWOULDBLOCK) || (errno == EAGAIN))
                        {
                            // Timeout. Keep going.
                            continue;
                        }

                        #endif
                    }

                    // Get the latest OpenSSL error
                    char buf[256];
                    unsigned long ssl_err = ERR_get_error();
                    ERR_error_string_n(ssl_err, buf, sizeof(buf));
                    vws.error(VE_SOCKET, "SSL_write() failed: %s", buf);

                    // Close socket
                    socket_abnormal_close(c);

                    return -1;
                }

                // Reset
                poll_events = POLLOUT;
            }
            else
            {
                // Non-SSL socket is writable, perform send() operation
                #if defined(__linux__) || defined(__sunos__)

                n = send(c->sockfd, data + sent, size - sent, MSG_NOSIGNAL);

                #else

                n = send(c->sockfd, data + sent, size - sent, 0);

                #endif

                if (n <= -1)
                {
                    #if defined(__windows__)
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)
                    {
                        // The socket is still open but would block on send
                        continue;
                    }
                    #else
                    if (errno == EWOULDBLOCK || errno == EAGAIN)
                    {
                        // The socket is still open but would block on send
                        continue;
                    }
                    #endif

                    // An error occurred, and the socket might be closed
                    vws.error(VE_SYS, "send() error");

                    // Close socket
                    socket_abnormal_close(c);

                    return -1;
                }
            }

            if (n > 0)
            {
                sent += n;
            }
        }
    }

    return sent;
}

void vws_socket_close(vws_socket* c)
{
    if (c->ssl != NULL)
    {
        // Unidirectional shutdown
        int rc = SSL_shutdown(c->ssl);

        if (rc < 0)
        {
            // Get the latest OpenSSL error
            char buf[256];
            unsigned long ssl_err = ERR_get_error();
            ERR_error_string_n(ssl_err, buf, sizeof(buf));
            vws.error(VE_WARN, "SSL_shutdown failed: %s", buf);
        }

        SSL_free(c->ssl);
        c->ssl = NULL;
    }

    if (c->sockfd != VWS_INVALID_SOCKET)
    {
        #if defined(__windows__)
        if (closesocket(c->sockfd) == SOCKET_ERROR)
        #else
        if (close(c->sockfd) == -1)
        #endif
        {
            char buf[256];
            #if defined(__windows__)
            strerror_s(buf, sizeof(buf), errno);
            #else
            strerror_r(errno, buf, sizeof(buf));
            #endif
            vws.error(VE_WARN, "Socket close failed: %s", buf);
        }
        #if defined(__windows__)
        WSACleanup();
        #endif

        c->sockfd = VWS_INVALID_SOCKET;
    }
}

vws_sockfd_t connect_to_host(const char* host, const char* port)
{
    vws_sockfd_t sockfd = VWS_INVALID_SOCKET;

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "connect_to_host(): enter");
    }

    #if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    // Resolve the host
    struct addrinfo hints, *res, *res0;
    int error;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = PF_UNSPEC; // Accept any family (IPv4 or IPv6)
    hints.ai_socktype = SOCK_STREAM;

    error = getaddrinfo(host, port, &hints, &res0);

    if (error)
    {
        if (vws.tracelevel > 0)
        {
            cstr msg = gai_strerror(error);
            vws.trace(VL_ERROR, "getaddrinfo failed: %s: %s", host, msg);
        }

        vws.error(VE_SYS, "getaddrinfo() failed");

        return VWS_INVALID_SOCKET;
    }

    for (res = res0; res; res = res->ai_next)
    {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        if (sockfd == VWS_INVALID_SOCKET)
        {
            vws.error(VE_SYS, "Failed to create socket");
            continue;
        }

        if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1)
        {
            close(sockfd);
            sockfd = VWS_INVALID_SOCKET;

            vws.error(VE_SYS, "Failed to connect");
            continue;
        }

        break; // If we get here, we must have connected successfully
    }

    freeaddrinfo(res0); // Free the addrinfo structure for this host

    #elif defined(__windows__)

    // Windows specific implementation
    // Please refer to Windows Socket programming guide

    WSADATA wsaData;
    struct addrinfo *result = NULL, *ptr = NULL, hints;
    sockfd = VWS_INVALID_SOCKET;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
    {
        vws.error(VE_SYS, "WSAStartup failed");
        return VWS_INVALID_SOCKET;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve the server address and port
    if (getaddrinfo(host, port, &hints, &result) != 0)
    {
        vws.error(VE_SYS, "getaddrinfo failed\n");
        return VWS_INVALID_SOCKET;
    }

    // Attempt to connect to an address until one succeeds
    for (ptr = result; ptr != NULL; ptr =ptr->ai_next)
    {
        // Create a SOCKET for connecting to server
        sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);

        if (sockfd == VWS_INVALID_SOCKET)
        {
            char buf[256];
            int e = WSAGetLastError();
            snprintf(buf, sizeof(buf), "socket failed with error: %ld", e);
            vws.error(VE_RT, "%s", buf);

            WSACleanup();
            return VWS_INVALID_SOCKET;
        }

        // Connect to server.
        if (connect(sockfd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR)
        {
            closesocket(sockfd);
            sockfd = VWS_INVALID_SOCKET;
            continue;
        }

        break;
    }

    freeaddrinfo(result);

    if (sockfd == VWS_INVALID_SOCKET)
    {
        vws.error(VE_SYS, "Unable to connect to host");
        WSACleanup();
        return VWS_INVALID_SOCKET;
    }

    #else
    #error Platform not supported
    #endif

    vws.success();

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "connect_to_host(): leave");
    }

    return sockfd;
}

bool vws_socket_addr_info(const struct sockaddr* addr, cstr* host, int* port)
{
    char hoststr[512];
    char portstr[24];
    int addrlen = 0;

    *host = NULL;
    *port = -1;

    switch (addr->sa_family)
    {
        case AF_INET:
        {
            addrlen = sizeof (struct sockaddr_in);
            break;
        }
        case AF_INET6:
        {
            addrlen = sizeof (struct sockaddr_in6);
            break;
        }
        default:
        {
            return false;
        }
    }

    int rc = getnameinfo( addr, addrlen,
                          hoststr, sizeof(hoststr),
                          portstr, sizeof(portstr),
                          NI_NUMERICHOST | NI_NUMERICSERV );

    if (rc == 0)
    {
        *host = strdup(hoststr);
        *port = atoi(portstr);
    }

    return true;
}

