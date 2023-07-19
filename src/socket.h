#ifndef VRTQL_SOCKET_DECLARE
#define VRTQL_SOCKET_DECLARE

#include <stdint.h>
#include <stddef.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>

#include <stdbool.h>

#include "vrtql.h"

struct vws_socket;

/**
 * @brief Callback for handshake on connect. This is called after socket
 * connection but before set to non-blocking. This provides a way to do basic
 * socket handshake before going into poll() mode.

 * @param s The socket
 * @return Returns true if handshake succeeded, false otherwise
 */
typedef bool (*vws_socket_hs)(struct vws_socket* s);

/**
 * @brief A socket
 */
typedef struct vws_socket
{
    /**< The socket file descriptor. */
    int sockfd;

    /**< The SSL context for the connection. */
    SSL_CTX* ssl_ctx;

    /**< The SSL connection instance. */
    SSL* ssl;

    /**< Socket receive buffer. */
    vrtql_buffer* buffer;

    /**< Socket timeout in milliseconds. */
    int timeout;

    /**< Enable tracing. */
    uint8_t trace;

    /**< User-defined data associated with the connection */
    char* data;

    /**< User-defined handshake function to be called on connect */
    vws_socket_hs hs;

} vws_socket;

/**
 * @defgroup SocketFunctions
 *
 * @brief Functions that manage sockets
 *
 */

/**
 * @brief Allocates a new socket connection.
 *
 * @return A pointer to the new connection instance.
 *
 * @ingroup SocketFunctions
 */
vws_socket* vws_socket_new();

/**
 * @brief Deallocates a socket connection.
 *
 * @param c The socket connection.
 */
void vws_socket_free(vws_socket* s);

/**
 * @brief Server instance constructor
 *
 * Constructs a new server instance. This takes a new, empty vws_socket instance
 * and initializes all of its members.
 *
 * @param s The socket instance to be initialized
 * @return The initialized socket instance
 *
 * @ingroup SocketFunctions
 */
vws_socket* vws_socket_ctor(vws_socket* s);

/**
 * @brief Socket instance destructor
 *
 * Destructs an initialized socket instance.
 *
 * @param s The socket instance to be destructed
 *
 * @ingroup SocketFunctions
 */
void vws_socket_dtor(vws_socket* s);

/**
 * @brief Connects to a specified host URL.
 *
 * @param s The socket instance to be destructed
 * @param host The host to connect to
 * @param port The port to connect to
 * @param ssl Flag to enable/disable SSL
 * @return Returns true if the connection is successful, false otherwise.
 *
 * @ingroup SocketFunctions
 */
bool vws_socket_connect(vws_socket* s, cstr host, int port, bool ssl);

/**
 * @brief Sets a timeout on a socket read/write operations. The default
 *        timeout is 10 seconds.
 *
 * @param s The socket instance to be destructed
 * @param sec The timeout value in seconds.
 * @return True if successful, false otherwise.
 *
 * @ingroup SocketFunctions
 */
bool vws_socket_set_timeout(vws_socket* s, int sec);

/**
 * @brief Closes the connection to the host.
 *
 * @param c The socket connection.
 *
 * @ingroup SocketFunctions
 */
void vws_socket_disconnect(vws_socket* s);

/**
 * @brief Checks if a connection is established.
 *
 * @param c The socket connection.
 * @return Returns true if the connection is established, false otherwise.
 *
 * @ingroup SocketFunctions
 */
bool vws_socket_is_connected(vws_socket* s);

/**
 * @brief Closes a socket
 *
 * @param c The vws_socket
 *
 * @ingroup SocketFunctions
 */
void vws_socket_close(vws_socket* c);

/**
 * @brief Reads data from a socket connection into a buffer
 *
 * @param c The vws_cnx representing the Socket connection.
 * @param data The buffer to read data into.
 * @param size The size of the buffer.
 * @return The number of bytes read, or an error code if an error occurred.
 *
 * @ingroup SocketFunctions
 */
ssize_t vws_socket_read(vws_socket* s);

/**
 * @brief Writes data from a buffer to a socket
 *
 * @param c The vws_cnx representing the socket
 * @param data The buffer containing the data to write.
 * @param size The size of the data to write.
 * @return The number of bytes written, or an error code if an error occurred.
 *
 * @ingroup SocketFunctions
 */
ssize_t vws_socket_write(vws_socket* s, ucstr data, size_t size);

#endif /* VRTQL_SOCKET_DECLARE */
