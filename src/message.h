#ifndef VRTQL_MSG_DECLARE
#define VRTQL_MSG_DECLARE

#include "websocket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    VM_MPACK_FORMAT,
    VM_JSON_FORMAT
} vrtql_msg_format_t;

/** Values 1-10 are reserved. Apps can use 11-63 */
typedef enum
{
    VM_MSG_VALID    = (1 << 1),
    VM_MSG_PRIORITY = (1 << 2),
    VM_MSG_IRQ      = (1 << 3)
} vrtql_msg_state_t;

/**
 * @defgroup MessageFunctions
 *
 * @brief Functions that manage VRTQL messages
 *
 */

/**
 * @brief Represents a message with routing, headers, and content.
 *
 * @ingroup MessageFunctions
 */
typedef struct vrtql_msg
{
    vws_kvs* routing;          /**< A map storing routing information. */
    vws_kvs* headers;          /**< A map storing header fields.       */

    vws_buffer* content;       /**< Buffer for the message content.    */
    uint64_t flags;            /**< Message state flags                */
    vrtql_msg_format_t format; /**< Message format                     */
    void* data;                /**< User-defined data                  */

} vrtql_msg;

/**
 * @brief Creates a new vrtql_msg instance.
 * @return A pointer to the new vrtql_msg instance.
 *
 * @ingroup MessageFunctions
 */
vrtql_msg* vrtql_msg_new();

/**
 * @brief Frees a vrtql_msg instance.
 * @param msg The vrtql_msg instance to be freed.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_free(vrtql_msg* msg);

/**
 * @brief Creates a deep copy of a vrtql_msg instance.
 *
 * This function creates a new vrtql_msg instance and copies the routing table,
 * headers table, and content from the original message to the new message.
 *
 * @param original The vrtql_msg instance to be copied.
 * @return A pointer to the new vrtql_msg instance. Returns NULL if the original
 *         message is NULL or if memory allocation fails.
 */
vrtql_msg* vrtql_msg_copy(vrtql_msg* original);

/**
 * @brief Dumps/traces a message to JSON format to stdout
 * @param msg The message
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_dump(vrtql_msg* m);

/**
 * @brief Dumps/traces a message to buffer
 * @param msg The message
 * @return buffer containing text representation, caller must free with
 *   vws_buffer_free()
 *
 * @ingroup MessageFunctions
 */
vws_buffer* vrtql_msg_repr(vrtql_msg* msg);

/**
 * @brief Gets a header value by key.
 * @param msg The vrtql_msg instance.
 * @param key The header key.
 * @return The header value. Returns NULL if the key doesn't exist.
 *
 * @ingroup MessageFunctions
 */
cstr vrtql_msg_get_header(vrtql_msg* msg, cstr key);

/**
 * @brief Sets a header key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The header key.
 * @param value The header value.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_set_header(vrtql_msg* msg, cstr key, cstr value);

/**
 * @brief Removes a header key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The header key to be removed.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_clear_header(vrtql_msg* msg, cstr key);

/**
 * @brief Removes all header key-value pairs.
 * @param msg The vrtql_msg instance.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_clear_headers(vrtql_msg* msg);

/**
 * @brief Gets a routing value by key.
 * @param msg The vrtql_msg instance.
 * @param key The routing key.
 * @return The routing value. Returns NULL if the key doesn't exist.
 *
 * @ingroup MessageFunctions
 */
cstr vrtql_msg_get_routing(vrtql_msg* msg, cstr key);

/**
 * @brief Sets a routing key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The routing key.
 * @param value The routing value.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_set_routing(vrtql_msg* msg, cstr key, cstr value);

/**
 * @brief Removes a routing key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The routing key to be removed.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_clear_routing(vrtql_msg* msg, cstr key);

/**
 * @brief Removes all routing key-value pairs.
 * @param msg The vrtql_msg instance.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_clear_routings(vrtql_msg* msg);

/**
 * @brief Gets the content of the message.
 * @param msg The vrtql_msg instance.
 * @return The content of the message. Returns NULL if no content is set.
 *
 * @ingroup MessageFunctions
 */
cstr vrtql_msg_get_content(vrtql_msg* msg);

/**
 * @brief Gets the size of the content.
 * @param msg The vrtql_msg instance.
 * @return The size of the content in bytes.
 *
 * @ingroup MessageFunctions
 */
size_t vrtql_msg_get_content_size(vrtql_msg* msg);

/**
 * @brief Sets the content of the message from a string.
 * @param msg The vrtql_msg instance.
 * @param value The content to be set.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_set_content(vrtql_msg* msg, cstr value);

/**
 * @brief Sets the content of the message from a binary data.
 * @param msg The vrtql_msg instance.
 * @param value The content to be set.
 * @param size The size of the content in bytes.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_set_content_binary(vrtql_msg* msg, cstr value, size_t size);

/**
 * @brief Clears the content of the message.
 * @param msg The vrtql_msg instance.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_clear_content(vrtql_msg* msg);

/**
 * @brief Clears message: headers, routing, content
 * @param msg The vrtql_msg instance.
 *
 * @ingroup MessageFunctions
 */
void vrtql_msg_clear(vrtql_msg* msg);

/**
 * @brief Indicates emptry message: no content, no headers, no routing.
 * @param msg The vrtql_msg instance.
 * @return True if message is empty, false otherwise
 *
 * @ingroup MessageFunctions
 */
bool vrtql_msg_is_empty(vrtql_msg* msg);

/**
 * @brief Serializes a vrtql_msg instance to a buffer.
 * @param msg The vrtql_msg instance.
 * @return A buffer containing the serialized message.
 *
 * @ingroup MessageFunctions
 */
vws_buffer* vrtql_msg_serialize(vrtql_msg* msg);

/**
 * @brief Deserializes a buffer to a vrtql_msg instance.
 * @param msg The vrtql_msg instance.
 * @param data The buffer containing the serialized message.
 * @param length The length of the buffer in bytes.
 * @return true on success, false on failure.
 *
 * @ingroup MessageFunctions
 */
bool vrtql_msg_deserialize(vrtql_msg* msg, ucstr data, size_t length);

/**
 * @brief Sends a message via a websocket connection. Does not take ownership of
 * message. Caller is still responsible for freeing message. This is to allow
 * reuse of messages.
 *
 * @param c The connection.
 * @param string The data to send.
 * @param size The size of the data in bytes.
 * @return Returns the number of bytes sent or -1 on error. In the case of
 *         error, check vws.e for details, especially for VE_SOCKET.
 *
 * @ingroup MessageFunctions
 */
ssize_t vrtql_msg_send(vws_cnx* c, vrtql_msg* msg);

/**
 * @brief Receives a message from the connection.
 *
 * @param c The connection.
 * @return Returns the most recent message or NULL if the socket timed out
 *   without receiving a full message. In the case of NULL, You should also
 *   check for socket error (vws.e.code == VE_SOCKET or
 *   vws_cnx_is_connected()).
 *
 * @ingroup MessageFunctions
 */
vrtql_msg* vrtql_msg_recv(vws_cnx* c);

#ifdef __cplusplus
}
#endif

#endif /* VRTQL_MSG_DECLARE */
