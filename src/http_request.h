#ifndef VRTQL_HTTP_REQUEST_DECLARE
#define VRTQL_HTTP_REQUEST_DECLARE

#include "vrtql.h"
#include "http_parser.h"

/**
 * @struct vrtql_http_req
 * @brief Structure representing an HTTP request
 */
typedef struct vrtql_http_req
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

} vrtql_http_req;

/**
 * @brief Creates a new instance of vrtql_http_req.
 * @return The newly created vrtql_http_req instance.
 */
vrtql_http_req* vrtql_http_req_new();

/**
 * @brief Parses the provided data as an HTTP request.
 * @param req The vrtql_http_req instance.
 * @param data The data to parse.
 * @param size The size of the data.
 * @return The number of bytes parsed, or a negative value on error.
 */
int vrtql_http_req_parse(vrtql_http_req* req, cstr data, size_t size);

/**
 * @brief Frees the resources associated with the vrtql_http_req instance.
 * @param req The vrtql_http_req instance to free.
 */
void vrtql_http_req_free(vrtql_http_req* req);

#endif /* VRTQL_HTTP_REQUEST_DECLARE */
