#ifndef VWS_HTTP_REQUEST_DECLARE
#define VWS_HTTP_REQUEST_DECLARE

#include "vws.h"
#include "llhttp/llhttp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct vws_http_msg
 * @brief Structure representing an HTTP request
 */
typedef struct vws_http_msg
{
    /**< The parser */
    llhttp_t* parser;

    /**< The parser settings */
    llhttp_settings_t* settings;

    /**< A map storing header fields. */
    vws_kvs* headers;

    /**< The url */
    vws_buffer* url;

    /**< The body */
    vws_buffer* body;

    /**< Placeholder for header field */
    vws_buffer* field;

    /**< Placeholder for header value */
    vws_buffer* value;

    /** Flag indicates headers have been parsed. */
    bool headers_complete;

    /** Flag indicates a complete message has been parsed. */
    bool done;
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

/**
 * @brief Get the content length from the HTTP message.
 *
 * This function retrieves the content length of the HTTP message.
 *
 * @param m Pointer to the HTTP message.
 * @return The content length as a 64-bit unsigned integer.
 */
uint64_t vws_http_msg_content_length(vws_http_msg* m);

/**
 * @brief Get the major version number of the HTTP message.
 *
 * This function retrieves the major version number of the HTTP message.
 *
 * @param m Pointer to the HTTP message.
 * @return The major version number as a 64-bit unsigned integer.
 */
uint64_t vws_http_msg_version_major(vws_http_msg* m);

/**
 * @brief Get the minor version number of the HTTP message.
 *
 * This function retrieves the minor version number of the HTTP message.
 *
 * @param m Pointer to the HTTP message.
 * @return The minor version number as a 64-bit unsigned integer.
 */
uint64_t vws_http_msg_version_minor(vws_http_msg* m);

/**
 * @brief Get the error number of the HTTP message.
 *
 * This function retrieves the error number of the HTTP message.
 *
 * @param m Pointer to the HTTP message.
 * @return The error number as a 64-bit unsigned integer.
 */
uint64_t vws_http_msg_errno(vws_http_msg* m);

/**
 * @brief Get the status code of the HTTP message.
 *
 * This function retrieves the status code of the HTTP message.
 *
 * @param m Pointer to the HTTP message.
 * @return The status code as an 8-bit unsigned integer.
 */
uint8_t vws_http_msg_status_code(vws_http_msg* m);

/**
 * @brief Get the status string of the HTTP message.
 *
 * This function retrieves the status string of the HTTP message.
 *
 * @param m Pointer to the HTTP message.
 * @return The status string as a cstr.
 */
cstr vws_http_msg_status_string(vws_http_msg* m);

/**
 * @brief Get the method string of the HTTP message.
 *
 * This function retrieves the method string of the HTTP message.
 *
 * @param m Pointer to the HTTP message.
 * @return The method string as a cstr.
 */
cstr vws_http_msg_method_string(vws_http_msg* m);

#ifdef __cplusplus
}
#endif

#endif /* VWS_HTTP_MSG_DECLARE */
