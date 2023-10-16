#include <ctype.h>
#include <stdio.h>

#include "http_message.h"

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

/**
 * @brief Callback for HTTP parser's on_message_begin event.
 * @param p The HTTP parser instance.
 * @return 0 to continue parsing.
 */
static int on_message_begin(llhttp_t* p);

/**
 * @brief Callback for HTTP parser's on_headers_complete event.
 * @param p The HTTP parser instance.
 * @return 0 to continue parsing.
 */
static int on_headers_complete(llhttp_t* p);

/**
 * @brief Callback for HTTP parser's on_message_complete event.
 * @param p The HTTP parser instance.
 * @return HPE_PAUSED to stop parsing.
 */
static int on_message_complete(llhttp_t* p);

/**
 * @brief Callback for HTTP parser's on_url event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the URL data.
 * @param l Length of the URL data.
 * @return 0 to continue parsing.
 */
static int on_url(llhttp_t* p, cstr at, size_t l);

/**
 * @brief Callback for HTTP parser's on_header_field event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the header field data.
 * @param l Length of the header field data.
 * @return 0 to continue parsing.
 */
static int on_header_field(llhttp_t* p, cstr at, size_t l);

/**
 * @brief Callback for HTTP parser's on_header_value event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the header value data.
 * @param l Length of the header value data.
 * @return 0 to continue parsing.
 */
static int on_header_value(llhttp_t* p, cstr at, size_t l);

/**
 * @brief Callback for HTTP parser's on_body event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the body data.
 * @param l Length of the body data.
 * @return 0 to continue parsing.
 */
static int on_body(llhttp_t* p, cstr at, size_t l);

//------------------------------------------------------------------------------
// HTTP Request
//------------------------------------------------------------------------------

/**
 * @brief Converts a string to lowercase.
 * @param s The string to convert.
 * @return The converted lowercase string.
 */
ucstr lcase(char* s)
{
    for(int i = 0; s[i]; i++)
    {
        s[i] = tolower(s[i]);
    }

    return (ucstr)s;
}

static void process_header(vws_http_msg* req)
{
    if (req->value->size != 0)
    {
        // We've got a complete field-value pair.

        // Null-terminate
        vws_buffer_append(req->field, (ucstr)"\0", 1);
        vws_buffer_append(req->value, (ucstr)"\0", 1);

        ucstr field = lcase((cstr)req->field->data);
        ucstr data  = req->value->data;
        vws_map_set(&req->headers, (cstr)field, (cstr)data);

        // Reset for the next field-value pair.
        vws_buffer_clear(req->field);
        vws_buffer_clear(req->value);
    }
}

vws_http_msg* vws_http_msg_new(int mode)
{
    vws_http_msg* req     = vws.malloc(sizeof(vws_http_msg));
    req->parser           = vws.malloc(sizeof(llhttp_t));
    req->settings         = vws.malloc(sizeof(llhttp_settings_t));
    req->url              = vws_buffer_new();
    req->body             = vws_buffer_new();
    req->field            = vws_buffer_new();
    req->value            = vws_buffer_new();
    req->headers_complete = false;
    req->done             = false;

    llhttp_settings_init(req->settings);

    req->settings->on_message_begin     = on_message_begin;
    req->settings->on_headers_complete  = on_headers_complete;
    req->settings->on_message_complete  = on_message_complete;
    req->settings->on_url               = on_url;
    req->settings->on_header_field      = on_header_field;
    req->settings->on_header_value      = on_header_value;
    req->settings->on_body              = on_body;

    llhttp_init(req->parser, mode, req->settings);
    req->parser->data = req;

    sc_map_init_str(&req->headers, 0, 0);

    return req;
}

void vws_http_msg_free(vws_http_msg* req)
{
    if (req == NULL)
    {
        return;
    }

    cstr key; cstr value;
    sc_map_foreach(&req->headers, key, value)
    {
        vws.free(key);
        vws.free(value);
    }

    sc_map_term_str(&req->headers);
    vws_buffer_free(req->url);
    vws_buffer_free(req->body);
    vws_buffer_free(req->field);
    vws_buffer_free(req->value);
    vws.free(req->parser);
    vws.free(req->settings);
    vws.free(req);
}

int on_message_begin(llhttp_t* p)
{
    return 0;
}

int on_headers_complete(llhttp_t* p)
{
    vws_http_msg* req     = p->data;
    req->headers_complete = true;

    // Process final header, if any.
    process_header(req);

    return 0;
}

int on_message_complete(llhttp_t* p)
{
    vws_http_msg* req = p->data;
    req->done         = true;

    // We have a complete message. Pause the parser.
    return HPE_PAUSED;
}

int on_url(llhttp_t* p, cstr at, size_t l)
{
    vws_http_msg* req = p->data;
    vws_buffer_append(req->url, (ucstr)at, l);

    return 0;
}

int on_header_field(llhttp_t* p, cstr at, size_t l)
{
    vws_http_msg* req = p->data;

    process_header(req);

    // Start new field
    vws_buffer_append(req->field, (ucstr)at, l);

    return 0;
}

int on_header_value(llhttp_t* p, cstr at, size_t l)
{
    vws_http_msg* req = p->data;
    vws_buffer_append(req->value, (ucstr)at, l);

    return 0;
}

int on_body(llhttp_t* p, cstr at, size_t l)
{
    vws_http_msg* req = p->data;
    vws_buffer_append(req->url, (ucstr)at, l);

    return 0;
}

int vws_http_msg_parse(vws_http_msg* req, cstr data, size_t size)
{
    enum llhttp_errno rc = llhttp_execute(req->parser, data, size);

    if (rc != HPE_OK)
    {
        // If paused
        if ((rc == HPE_PAUSED) || (rc == HPE_PAUSED_UPGRADE))
        {
            // Only legit reason to pause is on_message_complete() and therefore
            // done() will be set to true. If we pause for any other reason this
            // is error.
            if (req->done == false)
            {
                // Fatal error. Message is messed up somehow.
                return -1;
            }
        }
        else
        {
            // All other errors. Message is messed up somehow.
            return -1;
        }
    }

    // If the HTTP message was fully parsed
    if (req->done == true)
    {
        // llhttp_execute() internal cursor stops at the end of first http
        // message. We compute the amount of data consumed by using
        // llhttp_get_error_pos() to get the position of the last parsed byte
        // and subtract from that address the start address of "data". This is
        // the number of bytes consumed by the completed message out of the
        // total bytes in "data". Note that if data consists of only a single
        // HTTP message, then there will be zero bytes left. If however there is
        // another request, or data from from UPGRADE, then there can be
        // additional data

        return llhttp_get_error_pos(req->parser) - data;
    }

    // The HTTP message is not completely parsed and therefore all data provided
    // was consumed in message parsing.
    return size;
}
