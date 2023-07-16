#include <ctype.h>
#include <stdio.h>

#include "http_request.h"

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

vrtql_http_req* vrtql_http_req_new()
{
    vrtql_http_req* req = vrtql.malloc(sizeof(vrtql_http_req));
    req->parser         = vrtql.malloc(sizeof(http_parser));
    req->settings       = vrtql.malloc(sizeof(http_parser_settings));
    req->parser->data   = req;
    req->url            = vrtql_buffer_new();
    req->body           = vrtql_buffer_new();
    req->field          = vrtql_buffer_new();
    req->value          = vrtql_buffer_new();
    req->complete       = false;

    http_parser_init(req->parser, HTTP_REQUEST);
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

void vrtql_http_req_free(vrtql_http_req* req)
{
    if (req == NULL)
    {
        return;
    }

    cstr key; cstr value;
    sc_map_foreach(&req->headers, key, value)
    {
        free(key);
        free(value);
    }

    sc_map_term_str(&req->headers);
    vrtql_buffer_free(req->url);
    vrtql_buffer_free(req->body);
    vrtql_buffer_free(req->field);
    vrtql_buffer_free(req->value);
    free(req->parser);
    free(req->settings);
    free(req);
}

int on_message_begin(http_parser* p)
{
    return 0;
}

int on_headers_complete(http_parser* p)
{
    vrtql_http_req* req = p->data;
    req->complete       = true;

    return 0;
}

int on_message_complete(http_parser* p)
{
    return 0;
}

int on_url(http_parser* p, cstr at, size_t l, int intr)
{
    vrtql_http_req* req = p->data;
    vrtql_buffer_append(req->url, at, l);

    return 0;
}

/**
 * @brief Converts a string to lowercase.
 * @param s The string to convert.
 * @return The converted lowercase string.
 */
char* lcase(char* s)
{
    for(int i = 0; s[i]; i++)
    {
        s[i] = tolower(s[i]);
    }

    return s;
}

int on_header_field(http_parser* p, cstr at, size_t l, int intr)
{
    vrtql_http_req* req = p->data;

    if (req->value->size != 0)
    {
        // We've got a complete field-value pair.

        // Null-terminate
        vrtql_buffer_append(req->field, "\0", 1);
        vrtql_buffer_append(req->value, "\0", 1);

        cstr field = lcase(req->field->data);
        cstr data  = req->value->data;
        vrtql_map_set(&req->headers, field, data);

        // Reset for the next field-value pair.
        vrtql_buffer_clear(req->field);
        vrtql_buffer_clear(req->value);
    }

    // Start new field
    vrtql_buffer_append(req->field, at, l);

    return 0;
}

int on_header_value(http_parser* p, cstr at, size_t l, int intr)
{
    vrtql_http_req* req = p->data;

    vrtql_buffer_append(req->value, at, l);

    return 0;
}

int on_body(http_parser* p, cstr at, size_t l, int intr)
{
    vrtql_http_req* req = p->data;
    vrtql_buffer_append(req->url, at, l);

    return 0;
}

int vrtql_http_req_parse(vrtql_http_req* req, cstr data, size_t size)
{
    http_parser* p          = req->parser;
    http_parser_settings* s = req->settings;

    return http_parser_execute(p, s, data, size);
}
