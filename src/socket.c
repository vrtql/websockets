#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

#if defined(__windows__)
#define _WIN32_WINNT 0x0601  // Windows 7 or later
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <assert.h>
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
static int connect_to_host(const char* host, const char* port);

/**
 * @brief  Sets a timeout on a socket read/write operations.
 *
 * @param fd The socket file descriptor.
 * @param sec The timeout value in seconds.
 * @return True if successful, false otherwise.
 *
 * @ingroup SocketFunctions
 */
static bool socket_set_timeout(int fd, int sec);

/**
 * @brief Sets a socket to non-blocking mode.
 *
 * @param sockfd The socket file descriptor.
 * @return True if successful, false otherwise.
 *
 * @ingroup SocketFunctions
 */
static bool socket_set_nonblocking(int sockfd);

//------------------------------------------------------------------------------
//> Socket API
//------------------------------------------------------------------------------

vws_socket* vws_socket_new()
{
    vws_socket* c = (vws_socket*)vrtql.malloc(sizeof(vws_socket));
    memset(c, 0, sizeof(vws_socket));

    return vws_socket_ctor(c);
}

vws_socket* vws_socket_ctor(vws_socket* s)
{
    s->sockfd  = -1;
    s->buffer  = vrtql_buffer_new();
    s->ssl     = NULL;
    s->timeout = 10000;
    s->data    = NULL;
    s->hs      = NULL;
    s->flush   = true;

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
    vrtql_buffer_free(s->buffer);

    if (s->sockfd >= 0)
    {
        close(s->sockfd);
    }

    // Free connection
    free(s);
}

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

bool vws_socket_set_timeout(vws_socket* s, int sec)
{
    if (socket_set_timeout(s->sockfd, sec) == false)
    {
        return false;
    }

    // Set socket attribute, this will apply to poll().
    s->timeout = sec;

    return true;
}

bool socket_set_timeout(int fd, int sec)
{
#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    if (fd < 0)
    {
        vrtql.error(VE_RT, "Not connected");
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
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

#elif defined(__windows__)

    if (fd == INVALID_SOCKET)
    {
        vrtql.error(VE_RT, "Not connected");
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
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (cstr)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return true;
}

bool socket_set_nonblocking(int sockfd)
{
#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    int flags = fcntl(sockfd, F_GETFL, 0);

    if (flags == -1)
    {
        vrtql.error(VE_SYS, "fcntl(sockfd, F_GETFL, 0) failed");

        return false;
    }

    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        vrtql.error(VE_SYS, "fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) failed");

        return false;
    }

#elif defined(__windows__)

    unsigned long arg = 1;
    if (ioctlsocket(sockfd, FIONBIO, &arg) == SOCKET_ERROR)
    {
        vrtql.error(VE_SYS, "ioctlsocket(sockfd, FIONBIO, &arg)");

        return false;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return true;
}

bool vws_socket_is_connected(vws_socket* c)
{
    if (c == NULL)
    {
        return false;
    }

    return c->sockfd > -1;
}

bool vws_socket_connect(vws_socket* c, cstr host, int port, bool ssl)
{
    if (c == NULL)
    {
        // Return early if failed to create a connection.
        vrtql.error(VE_RT, "Invalid connection pointer()");
        return false;
    }

    // Clear socket buffer in case it was previously used in other connection.
    vrtql_buffer_clear(c->buffer);

    if (ssl == true)
    {
        if (vrtql_is_flag(&vrtql.state, CNX_SSL_INIT) == false)
        {
            SSL_library_init();
            RAND_poll();
            SSL_load_error_strings();

            vrtql_ssl_ctx = SSL_CTX_new(TLS_method());

            if (vrtql_ssl_ctx == NULL)
            {
                vrtql.error(VE_SYS, "Failed to create new SSL context");
                return false;
            }

            SSL_CTX_set_options(vrtql_ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        }

        c->ssl = SSL_new(vrtql_ssl_ctx);

        if (c->ssl == NULL)
        {
            vrtql.error(VE_SYS, "Failed to create new SSL object");
            vws_socket_close(c);
            return false;
        }

        vrtql_set_flag(&vrtql.state, CNX_SSL_INIT);
    }

    char port_str[20];
    sprintf(port_str, "%d", port);
    c->sockfd = connect_to_host(host, port_str);

    if (c->sockfd < 0)
    {
        vrtql.error(VE_SYS, "Connection failed");
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

        if (SSL_connect(c->ssl) <= 0)
        {
            vrtql.error(VE_SYS, "SSL connection failed");
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
    if (socket_set_nonblocking(c->sockfd) == false)
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
            vrtql.error(VE_SYS, "Handshake failed");
            vws_socket_close(c);
            return false;
        }
    }

    vrtql.success();

    return true;
}

void vws_socket_disconnect(vws_socket* c)
{
    if (vws_socket_is_connected(c) == false)
    {
        return;
    }

    vws_socket_close(c);

    vrtql.success();
}

ssize_t vws_socket_read(vws_socket* c)
{
    // Default success unless error
    vrtql.success();

    if (vws_socket_is_connected(c) == false)
    {
        vrtql.error(VE_DISCONNECT, "Not connected");
        return -1;
    }

    // Validate input parameters
    if (c == NULL)
    {
        vrtql.error(VE_WARN, "Invalid parameters");
        return -1;
    }

    struct pollfd fds;
    int poll_events = POLLIN;

openssl_reread:

    #if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    fds.fd     = c->sockfd;
    fds.events = poll_events;

    int rc = poll(&fds, 1, c->timeout);

    if (fds.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
        vrtql.error(VE_DISCONNECT, "Socket error during poll()");
        vws_socket_close(c);
        return -1;
    }

    #elif defined(__windows__)

    WSAPOLLFD fds;
    fds.fd     = c->sockfd;
    fds.events = POLLIN;

    int rc = WSAPoll(&fds, 1, c->timeout);

    if (rc == SOCKET_ERROR)
    {
        vrtql.error(VE_DISCONNECT, "Socket error during WSAPoll()");
        vws_socket_close(c);
        return -1;
    }

    #else
        #error Platform not supported
    #endif

    if (rc == -1)
    {
        vrtql.error(VE_RT, "poll() failed");
        return -1;
    }

    if (rc == 0)
    {
        vrtql.error(VE_WARN, "timeout");
        return 0;
    }

    ssize_t      n = 0;
    ucstr data   = &vrtql.sslbuf[0];
    ssize_t size = sizeof(vrtql.sslbuf);

    if (fds.revents & poll_events)
    {
        // We need running total bc we may make multiple SSL_read() calls.
        int total = 0;

        if (c->ssl != NULL)
        {
            // Drain all data from SSL buffer
            while ((n = SSL_read(c->ssl, data, size)) > 0)
            {
                // Process received data stored in buf
                total += n;
                vrtql_buffer_append(c->buffer, data, n);

                if (n < size)
                {
                    // All available data has been read, break the loop.
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

                // Get the latest OpenSSL error
                char buf[256];
                unsigned long ssl_err = ERR_get_error();
                ERR_error_string_n(ssl_err, buf, sizeof(buf));
                vrtql.error(VE_DISCONNECT, "SSL_read() failed: %s", buf);

                // Close socket
                vws_socket_close(c);

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
                vrtql.error(VE_DISCONNECT, "disconnect");

                // Close socket
                vws_socket_close(c);

                return -1;
            }

            if (n <= -1)
            {
                #if defined(__windows__)
                int err = WSAGetLastError();

                if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)
                {
                    // No data.
                    return 0;
                }

                #else

                if ((errno == EWOULDBLOCK) || (errno == EAGAIN))
                {
                    // No data.
                    return 0;
                }
                else
                {
                    // Error
                    char buf[256];
                    strerror_r(errno, buf, sizeof(buf));
                    vrtql.error(VE_WARN, "recv() failed: %s", buf);

                    // Close socket
                    vws_socket_close(c);

                    return -1;
                }
                #endif
            }

            // Should always be true if we get here
            if (n > 0)
            {
                vrtql_buffer_append(c->buffer, data, n);
            }
        }
    }

    return n;
}

ssize_t vws_socket_write(vws_socket* c, const ucstr data, size_t size)
{
    // Default success unless error
    vrtql.success();

    if (vws_socket_is_connected(c) == false)
    {
        vrtql.error(VE_DISCONNECT, "Not connected");
        return -1;
    }

    // Validate input parameters
    if (c == NULL || data == NULL || size == 0)
    {
        vrtql.error(VE_WARN, "Invalid parameters");
        return -1;
    }

    // But default we will keep looping until we have sent all the data
    size_t sent     = 0;
    int poll_events = POLLOUT;
    int iterations  = 0;
    while (sent < size)
    {
        // If we attempted at east one poll()/send()
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

        int rc = poll(&fds, 1, c->timeout);

        if (fds.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            vrtql.error(VE_DISCONNECT, "Socket error during poll()");
            vws_socket_close(c);
            return -1;
        }

        #elif defined(__windows__)

        WSAPOLLFD fds;
        fds.fd     = c->sockfd;
        fds.events = poll_events;

        int rc = WSAPoll(&fds, 1, c->timeout);

        if (rc == SOCKET_ERROR)
        {
            vrtql.error(VE_DISCONNECT, "Socket error during WSAPoll()");
            vws_socket_close(c);
            return -1;
        }

        #else
            #error Platform not supported
        #endif

        if (rc == -1)
        {
            vrtql.error(VE_SYS, "poll() failed");
            return -1;
        }

        // There was a timeout. Restart loop.
        if (rc == 0)
        {
            continue;
        }

        ssize_t n = 0;
        if (fds.revents & poll_events)
        {
            if (c->ssl != NULL)
            {
                // SSL socket is writable, perform SSL_write() operation
                n = SSL_write(c->ssl, data + sent, size - sent);

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

                        continue;
                    }

                    // Get the latest OpenSSL error
                    char buf[256];
                    unsigned long ssl_err = ERR_get_error();
                    ERR_error_string_n(ssl_err, buf, sizeof(buf));
                    vrtql.error(VE_DISCONNECT, "SSL_write() failed: %s", buf);

                    // Close socket
                    vws_socket_close(c);

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
                    vrtql.error(VE_SYS, "send() error");

                    // Close socket
                    vws_socket_close(c);

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
            vrtql.error(VE_WARN, "SSL_shutdown failed: %s", buf);
        }

        SSL_free(c->ssl);
        c->ssl = NULL;
    }

    /*
    if (vrtql_ssl_ctx != NULL)
    {
        SSL_CTX_free(vrtql_ssl_ctx);
        vrtql_ssl_ctx = NULL;
    }
    */

    if (c->sockfd >= 0)
    {
#if defined(__windows__)
        if (closesocket(c->sockfd) == SOCKET_ERROR
            )
#else
        if (close(c->sockfd) == -1)
#endif
        {
            char buf[256];
            strerror_r(errno, buf, sizeof(buf));
            vrtql.error(VE_WARN, "Socket close failed: %s", buf);
        }

#if defined(__windows__)
        WSACleanup();
#endif

        c->sockfd = -1;
    }
}

int connect_to_host(const char* host, const char* port)
{
    int sockfd = -1;

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
        if (vrtql.tracelevel > 0)
        {
            cstr msg = gai_strerror(error);
            vrtql.trace(VL_ERROR, "getaddrinfo failed: %s: %s", host, msg);
        }

        vrtql.error(VE_SYS, "getaddrinfo() failed");

        return -1;
    }

    for (res = res0; res; res = res->ai_next)
    {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        if (sockfd == -1)
        {
            vrtql.error(VE_SYS, "Failed to create socket");
            continue;
        }

        if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1)
        {
            close(sockfd);
            sockfd = -1;

            vrtql.error(VE_SYS, "Failed to connect");
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
    sockfd = INVALID_SOCKET;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
    {
        vrtql.error(VE_SYS, "WSAStartup failed");
        return -1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve the server address and port
    if (getaddrinfo(host, port, &hints, &result) != 0)
    {
        vrtql.error(VE_SYS, "getaddrinfo failed\n");
        return -1;
    }

    // Attempt to connect to an address until one succeeds
    for (ptr = result; ptr != NULL; ptr =ptr->ai_next)
    {
        // Create a SOCKET for connecting to server
        sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);

        if (sockfd == INVALID_SOCKET)
        {
            char buf[256];
            int e = WSAGetLastError();
            snprintf(buf, sizeof(buf), "socket failed with error: %ld", e);
            vrtql.error(VE_RT, buf);

            WSACleanup();
            return -1;
        }

        // Connect to server.
        if (connect(sockfd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR)
        {
            closesocket(sockfd);
            sockfd = INVALID_SOCKET;
            continue;
        }

        break;
    }

    freeaddrinfo(result);

    if (sockfd == INVALID_SOCKET)
    {
        vrtql.error(VE_SYS, "Unable to connect to host");
        WSACleanup();
        return -1;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return sockfd;
}
