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
static int on_message_begin(http_parser* p);

/**
 * @brief Callback for HTTP parser's on_headers_complete event.
 * @param p The HTTP parser instance.
 * @return 0 to continue parsing.
 */
static int on_headers_complete(http_parser* p);

/**
 * @brief Callback for HTTP parser's on_message_complete event.
 * @param p The HTTP parser instance.
 * @return 0 to continue parsing.
 */
static int on_message_complete(http_parser* p);

/**
 * @brief Callback for HTTP parser's on_url event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the URL data.
 * @param l Length of the URL data.
 * @param intr Flag indicating if the URL data is incomplete.
 * @return 0 to continue parsing.
 */
static int on_url(http_parser* p, cstr at, size_t l, int intr);

/**
 * @brief Callback for HTTP parser's on_header_field event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the header field data.
 * @param l Length of the header field data.
 * @param intr Flag indicating if the header field data is incomplete.
 * @return 0 to continue parsing.
 */
static int on_header_field(http_parser* p, cstr at, size_t l, int intr);

/**
 * @brief Callback for HTTP parser's on_header_value event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the header value data.
 * @param l Length of the header value data.
 * @param intr Flag indicating if the header value data is incomplete.
 * @return 0 to continue parsing.
 */
static int on_header_value(http_parser* p, cstr at, size_t l, int intr);

/**
 * @brief Callback for HTTP parser's on_body event.
 * @param p The HTTP parser instance.
 * @param at Pointer to the body data.
 * @param l Length of the body data.
 * @param intr Flag indicating if the body data is incomplete.
 * @return 0 to continue parsing.
 */
static int on_body(http_parser* p, cstr at, size_t l, int intr);

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
    vws_http_msg* req   = vws.malloc(sizeof(vws_http_msg));
    req->parser         = vws.malloc(sizeof(http_parser));
    req->settings       = vws.malloc(sizeof(http_parser_settings));
    req->parser->data   = req;
    req->url            = vws_buffer_new();
    req->body           = vws_buffer_new();
    req->field          = vws_buffer_new();
    req->value          = vws_buffer_new();
    req->complete       = false;

    http_parser_init(req->parser, mode);
    http_parser_settings_init(req->settings);

    http_parser_settings* s = req->settings;
    s->on_message_begin     = on_message_begin;
    s->on_headers_complete  = on_headers_complete;
    s->on_message_complete  = on_message_complete;
    s->on_url               = on_url;
    s->on_header_field      = on_header_field;
    s->on_header_value      = on_header_value;
    s->on_body              = on_body;

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

int on_message_begin(http_parser* p)
{
    return 0;
}

int on_headers_complete(http_parser* p)
{
    vws_http_msg* req = p->data;
    req->complete     = true;

    // Process final header, if any.
    process_header(req);

    return 0;
}

int on_message_complete(http_parser* p)
{
    return 0;
}

int on_url(http_parser* p, cstr at, size_t l, int intr)
{
    vws_http_msg* req = p->data;
    vws_buffer_append(req->url, (ucstr)at, l);

    return 0;
}

int on_header_field(http_parser* p, cstr at, size_t l, int intr)
{
    vws_http_msg* req = p->data;

    process_header(req);

    // Start new field
    vws_buffer_append(req->field, (ucstr)at, l);

    return 0;
}

int on_header_value(http_parser* p, cstr at, size_t l, int intr)
{
    vws_http_msg* req = p->data;

    vws_buffer_append(req->value, (ucstr)at, l);

    return 0;
}

int on_body(http_parser* p, cstr at, size_t l, int intr)
{
    vws_http_msg* req = p->data;
    vws_buffer_append(req->url, (ucstr)at, l);

    return 0;
}

int vws_http_msg_parse(vws_http_msg* req, cstr data, size_t size)
{
    http_parser* p          = req->parser;
    http_parser_settings* s = req->settings;

    return http_parser_execute(p, s, data, size);
}
