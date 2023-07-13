#ifndef VRTQL_WEBSOCKET_DECLARE
#define VRTQL_WEBSOCKET_DECLARE

#include <stdint.h>
#include <stddef.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>

#include <stdbool.h>

#include "vrtql.h"
#include "util/sc_queue.h"

//------------------------------------------------------------------------------
// Frame
//------------------------------------------------------------------------------

/**
 * @brief Represents a Websocket frame.
 */
typedef struct vws_frame
{
    unsigned char fin;       /**< Final frame in the message (1) or not (0). */
    unsigned char opcode;    /**< Defines the interpretation of the payload data. */
    unsigned char mask;      /**< Defines whether the payload is masked. */
    unsigned int offset;     /**< Position of the data in the frame. */
    unsigned long long size; /**< The size of the payload data. */
    unsigned char* data;     /**< The payload data for the frame. */
} vws_frame;

/**
 * @brief Instantiates a new websocket frame given some data, size, and opcode.
 *
 * @param data The data to be contained in the frame.
 * @param s The size of the data.
 * @param oc The opcode for the frame.
 * @return Returns a pointer to the new frame.
 */
vws_frame* vws_frame_new(ucstr data, size_t s, unsigned char oc);

/**
 * @brief Deallocates a websocket frame.
 *
 * @param frame The frame to be deallocated.
 */
void vws_frame_free(vws_frame* frame);

//------------------------------------------------------------------------------
// Message
//------------------------------------------------------------------------------

/**
 * @brief Represents a websocket message, which contains opcode and data.
 */
typedef struct
{
    unsigned char opcode; /**< Defines the interpretation of the payload data. */
    vrtql_buffer* data;   /**< The payload data for the message. */
} vws_msg;

/**
 * @brief Instantiates a new websocket message.
 *
 * @return Returns a pointer to the new message.
 */
vws_msg* vws_msg_new();

/**
 * @brief Deallocates a websocket message.
 *
 * @param msg The message to be deallocated.
 */
void vws_msg_free(vws_msg* msg);

//------------------------------------------------------------------------------
// Connection
//------------------------------------------------------------------------------

/**
 * @brief Represents a websocket connection. Includes various connection details
 * and state flags.
 */
typedef struct
{
    uint64_t flags;            /**< Connection state flags.             */
    vrtql_url url;             /**< The URL of the websocket server.    */
    int sockfd;                /**< The socket file descriptor.         */
    SSL_CTX* ssl_ctx;          /**< The SSL context for the connection. */
    SSL* ssl;                  /**< The SSL connection instance.        */
    char* key;                 /**< The websocket key.                  */
    vrtql_buffer* buffer;      /**< Socket receive buffer.              */
    struct sc_queue_ptr queue; /**< Queue for incoming frames.          */
    int timeout;               /**< Socket timeout in milliseconds.     */
    uint8_t trace;             /**< Enable tracing.                     */
} vws_cnx;

/**
 * @brief Connects to a specified host URL.
 *
 * @param uri The URL of the host to connect to.
 * @return Returns true if the connection is successful, false otherwise.
 */
bool vws_connect(vws_cnx* c, cstr uri);

/**
 * @brief Closes the connection to the host.
 *
 * @param c The websocket connection.
 */
void vws_disconnect(vws_cnx* c);

/**
 * @brief Allocates a new websocket connection.
 *
 * @return A pointer to the new connection instance.
 */
vws_cnx* vws_cnx_new();

/**
 * @brief Deallocates a websocket connection.
 *
 * @param c The websocket connection.
 */
void vws_cnx_free(vws_cnx* c);

/**
 * @brief Checks if a connection is established.
 *
 * @param c The websocket connection.
 * @return Returns true if the connection is established, false otherwise.
 */
bool vws_cnx_is_connected(vws_cnx* c);

/**
 * @brief Sets a timeout for the connection.
 *
 * @param c The websocket connection.
 * @param t The timeout value in seconds.
 * @return Returns true if successful, false otherwise.
 */
bool vws_cnx_set_timeout(vws_cnx* c, int t);

/**
 * @brief Sets the connection to server mode
 *
 * @param c The websocket connection.
 * @return Returns void.
 */
void vws_cnx_set_server_mode(vws_cnx* c);

//------------------------------------------------------------------------------
// Messaging API
//------------------------------------------------------------------------------

/**
 * @brief Sends a TEXT message via a websocket connection.
 *
 * @param c The connection.
 * @param string The text to send.
 * @return Returns the number of bytes sent.
 */
int vws_send_text(vws_cnx* c, cstr string);

/**
 * @brief Sends a BINARY message via a websocket connection.
 *
 * @param c The connection.
 * @param string The data to send.
 * @param size The size of the data in bytes.
 * @return Returns the number of bytes sent.
 */
int vws_send_binary(vws_cnx* c, ucstr string, size_t size);

/**
 * @brief Sends custom frame data via a websocket connection.
 *
 * @param c The connection.
 * @param dataThe data to send.
 * @param size The size of the data in bytes.
 * @param oc The websocket opcode defining the frame type.
 * @return Returns the number of bytes sent out on wire.
 */
ssize_t vws_send_data(vws_cnx* c, ucstr data, size_t size, int oc);

/**
 * @brief Sends a prebuilt websocket frame. This function will take ownership of
 * the frame and deallocate it for the caller.
 *
 * @param c The connection.
 * @param frame The frame to send. This function will take ownership of the
 *   frame and deallocate it for the caller.
 * @return Returns the number of bytes sent out on wire.
 */
ssize_t vws_send_frame(vws_cnx* c, vws_frame* frame);

/**
 * @brief Receives a websocket message from the connection.
 *
 * @param c The connection.
 * @return Returns the most recent websocket message or NULL if the socket
 *   timed out without receiving a full message.
 */
vws_msg* vws_recv_msg(vws_cnx* c);

/**
 * @brief Receives a websocket frame from the connection.
 *
 * @param c The connection.
 * @return Returns the most recent websocket frame or NULL if the socket timed
 *   out without receiving a full frame.
 */
vws_frame* vws_recv_frame(vws_cnx* c);

#endif /* VRTQL_WEBSOCKET_DECLARE */
