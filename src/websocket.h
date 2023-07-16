#ifndef VRTQL_WEBSOCKET_DECLARE
#define VRTQL_WEBSOCKET_DECLARE

#include <stdint.h>
#include <stddef.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>

#include <stdbool.h>

#include "vrtql.h"
#include "socket.h"
#include "util/sc_queue.h"

//------------------------------------------------------------------------------
// Frame
//------------------------------------------------------------------------------

/** @brief Defines the types of WebSocket frames */
typedef enum
{
    /** A continuation frame, data message split across multiple frames. */
    CONTINUATION_FRAME = 0x0,

    /** A text data frame. */
    TEXT_FRAME = 0x1,

    /** A binary data frame. */
    BINARY_FRAME = 0x2,

    /** A close control frame. */
    CLOSE_FRAME = 0x8,

    /** A ping control frame. */
    PING_FRAME = 0x9,

    /** A pong control frame, in response to a ping. */
    PONG_FRAME = 0xA

} frame_type_t;

/**
 * @brief Represents a Websocket frame.
 */
typedef struct vws_frame
{
    /**< Final frame in the message (1) or not (0). */
    unsigned char fin;

    /**< Defines the interpretation of the payload data. */
    unsigned char opcode;

    /**< Defines whether the payload is masked. */
    unsigned char mask;

    /**< Position of the data in the frame. */
    unsigned int offset;

    /**< The size of the payload data. */
    unsigned long long size;

    /**< The payload data for the frame. */
    unsigned char* data;

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

/**
 * @brief Generates a close frame for a WebSocket connection.
 *
 * @return A pointer to the generated close frame.
 *
 * @ingroup FrameFunctions
 */
vrtql_buffer* vws_generate_close_frame();

/**
 * @brief Generates a pong frame in response to a received ping frame.
 *
 * @param ping_data The data from the received ping frame.
 * @param s The size of the ping data.
 * @return A pointer to the generated pong frame.
 *
 * @ingroup FrameFunctions
 */
vrtql_buffer* vws_generate_pong_frame(ucstr ping_data, size_t s);

//------------------------------------------------------------------------------
// Message
//------------------------------------------------------------------------------

/**
 * @brief Represents a websocket message, which contains opcode and data.
 */
typedef struct
{
    /**< Defines the interpretation of the payload data. */
    unsigned char opcode;

    /**< The payload data for the message. */
    vrtql_buffer* data;
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

struct vws_cnx;

/**
 * @brief Callback for frame processing
 * @param s The server instance
 * @param t The incoming request to process
 */
typedef void (*vws_process_frame)(struct vws_cnx* cnx, vws_frame* frame);

/**
 * @brief A WebSocket connection.
 */
typedef struct vws_cnx
{
    /**< Base class */
    struct vws_socket base;

    /**< Connection state flags. */
    uint64_t flags;

    /**< The URL of the websocket server. */
    vrtql_url url;

    /**< The WebSocket origin. */
    char* origin;

    /**< The websocket key. */
    char* key;

    /**< Queue for incoming frames. */
    struct sc_queue_ptr queue;

    /**< Frame processing callback. */
    vws_process_frame process;

    /**< User-defined data associated with the connection */
    char* data;

} vws_cnx;

/**
 * @brief Generates a WebSocket accept key from input.
 *
 * @param key The input key
 * @return Returns the accept key on the heap. Caller MUST call free() on return
 * value
 */
cstr vws_accept_key(cstr key);

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
 * @brief Sets the connection to server mode
 *
 * @param c The websocket connection.
 * @return Returns void.
 */
void vws_cnx_set_server_mode(vws_cnx* c);

/**
 * @brief Processes incoming data from a WebSocket connection.
 *
 * This function parses the data in the socket buffer and attempts to extract
 * complete WebSocket frames. It processes as many frames as possible and
 * calls the appropriate processing function for each frame. The consumed
 * data is then drained from the buffer.
 *
 * @param c The WebSocket connection.
 * @return The total number of bytes consumed from the socket buffer.
 *         If no complete frame is available or an error occurs, it returns 0.
 */
ssize_t vws_socket_ingress(vws_cnx* c);

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
 * @brief Removes and returns the first complete message from the connection's
 *         message queue.
 *
 * @param c The vws_cnx representing the WebSocket connection.
 * @return A pointer to the popped vws_msg object, or NULL if the queue is
 * empty.
 *
 * @ingroup MessageFunctions
 */
vws_msg* vws_pop_message(vws_cnx* c);

/**
 * @brief Receives a websocket frame from the connection.
 *
 * @param c The connection.
 * @return Returns the most recent websocket frame or NULL if the socket timed
 *   out without receiving a full frame.
 */
vws_frame* vws_recv_frame(vws_cnx* c);

#endif /* VRTQL_WEBSOCKET_DECLARE */
