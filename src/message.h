#ifndef VRTQL_MSG_DECLARE
#define VRTQL_MSG_DECLARE

#include "websocket.h"

typedef enum
{
    VM_MPACK_FORMAT,
    VM_JSON_FORMAT
} vrtql_msg_format;

typedef enum
{
    VM_MSG_VALID       = (1 << 1),
    VM_MSG_PRIORITY    = (1 << 2),
    VM_MSG_OUT_OF_BAND = (1 << 3)
} vrtql_msg_state;

/**
 * @brief Represents a message with routing, headers, and content.
 */
typedef struct vrtql_msg
{
    struct sc_map_str routing; /**< A map storing routing information. */
    struct sc_map_str headers; /**< A map storing header fields.       */
    vrtql_buffer* content;     /**< Buffer for the message content.    */
    uint64_t flags;            /**< Message state flags                */
    vrtql_msg_format format;   /**< Message format                     */
} vrtql_msg;

/**
 * @brief Creates a new vrtql_msg instance.
 * @return A pointer to the new vrtql_msg instance.
 */
vrtql_msg* vrtql_msg_new();

/**
 * @brief Frees a vrtql_msg instance.
 * @param msg The vrtql_msg instance to be freed.
 */
void vrtql_msg_free(vrtql_msg* msg);

/**
 * @brief Gets a header value by key.
 * @param msg The vrtql_msg instance.
 * @param key The header key.
 * @return The header value. Returns NULL if the key doesn't exist.
 */
cstr vrtql_msg_get_header(vrtql_msg* msg, cstr key);

/**
 * @brief Sets a header key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The header key.
 * @param value The header value.
 */
void vrtql_msg_set_header(vrtql_msg* msg, cstr key, cstr value);

/**
 * @brief Removes a header key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The header key to be removed.
 */
void vrtql_msg_clear_header(vrtql_msg* msg, cstr key);

/**
 * @brief Gets a routing value by key.
 * @param msg The vrtql_msg instance.
 * @param key The routing key.
 * @return The routing value. Returns NULL if the key doesn't exist.
 */
cstr vrtql_msg_get_routing(vrtql_msg* msg, cstr key);

/**
 * @brief Sets a routing key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The routing key.
 * @param value The routing value.
 */
void vrtql_msg_set_routing(vrtql_msg* msg, cstr key, cstr value);

/**
 * @brief Removes a routing key-value pair.
 * @param msg The vrtql_msg instance.
 * @param key The routing key to be removed.
 */
void vrtql_msg_clear_routing(vrtql_msg* msg, cstr key);

/**
 * @brief Gets the content of the message.
 * @param msg The vrtql_msg instance.
 * @return The content of the message. Returns NULL if no content is set.
 */
cstr vrtql_msg_get_content(vrtql_msg* msg);

/**
 * @brief Gets the size of the content.
 * @param msg The vrtql_msg instance.
 * @return The size of the content in bytes.
 */
size_t vrtql_msg_get_content_size(vrtql_msg* msg);

/**
 * @brief Sets the content of the message from a string.
 * @param msg The vrtql_msg instance.
 * @param value The content to be set.
 */
void vrtql_msg_set_content(vrtql_msg* msg, cstr value);

/**
 * @brief Sets the content of the message from a binary data.
 * @param msg The vrtql_msg instance.
 * @param value The content to be set.
 * @param size The size of the content in bytes.
 */
void vrtql_msg_set_content_binary(vrtql_msg* msg, cstr value, size_t size);

/**
 * @brief Clears the content of the message.
 * @param msg The vrtql_msg instance.
 */
void vrtql_msg_clear_content(vrtql_msg* msg);

/**
 * @brief Serializes a vrtql_msg instance to a buffer.
 * @param msg The vrtql_msg instance.
 * @return A buffer containing the serialized message.
 */
vrtql_buffer* vrtql_msg_serialize(vrtql_msg* msg);

/**
 * @brief Deserializes a buffer to a vrtql_msg instance.
 * @param msg The vrtql_msg instance.
 * @param data The buffer containing the serialized message.
 * @param length The length of the buffer in bytes.
 * @return true on success, false on failure.
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
 * @return Returns the number of bytes sent.
 */
ssize_t vrtql_msg_send(vws_cnx* c, vrtql_msg* msg);

/**
 * @brief Receives a message from the connection.
 *
 * @param c The connection.
 * @return Returns the most recent message or NULL if the socket timed out
 *   without receiving a full message.
 */
vrtql_msg* vrtql_msg_receive(vws_cnx* c);


#endif /* VRTQL_MSG_DECLARE */
