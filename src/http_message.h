#ifndef VRTQL_HTTP_REQUEST_DECLARE
#define VRTQL_HTTP_REQUEST_DECLARE

#include "vrtql.h"
#include "http_parser.h"

/**
 * @struct vrtql_http_msg
 * @brief Structure representing an HTTP request
 */
typedef struct vrtql_http_msg
{
    /**< The parser */
    http_parser* parser;

    /**< The parser settings */
    http_parser_settings* settings;

    /**< A map storing header fields. */
    struct sc_map_str headers;

    /**< The url */
    vrtql_buffer* url;

    /**< The body */
    vrtql_buffer* body;

    /**< Placeholder for header field */
    vrtql_buffer* field;

    /**< Placeholder for header value */
    vrtql_buffer* value;

    /** Flag indicates a complete message has been parsed. */
    bool complete;

} vrtql_http_msg;

/**
 * @brief Creates a new instance of vrtql_http_msg.
 * @param mode Must be one of HTTP_REQUEST or HTTP_RESPONSE depending on the
 *        message to parse.
 * @return The newly created vrtql_http_msg instance.
 */
vrtql_http_msg* vrtql_http_msg_new(int mode);

/**
 * @brief Parses the provided data as an HTTP request.
 * @param req The vrtql_http_msg instance.
 * @param data The data to parse.
 * @param size The size of the data.
 * @return The number of bytes parsed, or a negative value on error.
 */
int vrtql_http_msg_parse(vrtql_http_msg* req, cstr data, size_t size);

/**
 * @brief Frees the resources associated with the vrtql_http_msg instance.
 * @param req The vrtql_http_msg instance to free.
 */
void vrtql_http_msg_free(vrtql_http_msg* req);

#endif /* VRTQL_HTTP_MSG_DECLARE */
