#ifndef VWS_HTTP_REQUEST_DECLARE
#define VWS_HTTP_REQUEST_DECLARE

#include "vws.h"
#include "http_parser.h"

/**
 * @struct vws_http_msg
 * @brief Structure representing an HTTP request
 */
typedef struct vws_http_msg
{
    /**< The parser */
    http_parser* parser;

    /**< The parser settings */
    http_parser_settings* settings;

    /**< A map storing header fields. */
    struct sc_map_str headers;

    /**< The url */
    vws_buffer* url;

    /**< The body */
    vws_buffer* body;

    /**< Placeholder for header field */
    vws_buffer* field;

    /**< Placeholder for header value */
    vws_buffer* value;

    /** Flag indicates a complete message has been parsed. */
    bool complete;

} vws_http_msg;

/**
 * @brief Creates a new instance of vws_http_msg.
 * @param mode Must be one of HTTP_REQUEST or HTTP_RESPONSE depending on the
 *        message to parse.
 * @return The newly created vws_http_msg instance.
 */
vws_http_msg* vws_http_msg_new(int mode);

/**
 * @brief Parses the provided data as an HTTP request.
 * @param req The vws_http_msg instance.
 * @param data The data to parse.
 * @param size The size of the data.
 * @return The number of bytes parsed, or a negative value on error.
 */
int vws_http_msg_parse(vws_http_msg* req, cstr data, size_t size);

/**
 * @brief Frees the resources associated with the vws_http_msg instance.
 * @param req The vws_http_msg instance to free.
 */
void vws_http_msg_free(vws_http_msg* req);

#endif /* VWS_HTTP_MSG_DECLARE */
