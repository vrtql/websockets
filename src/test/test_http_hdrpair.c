// test_http_hdrpair.c — HTTP header field/value pairing across empty values and
// split callbacks.
//
// BUG (pre-fix): process_header emitted+cleared a header only when the value was
// non-empty. An empty-valued header ("X-Empty:\r\n") left the field buffer
// uncleared, so the next field name concatenated onto it -- the real header
// (e.g. Host, Origin, Sec-WebSocket-Key) was lost and a bogus "x-emptyhost"
// appeared. A remote unauthenticated client could drop/hide any header during
// the WS upgrade by prefixing an empty-valued one.
//
// FIX: track whether a value callback has completed the current pair (in_value,
// set in on_header_value, which llhttp fires even for empty values). The next
// on_header_field / on_headers_complete emits + clears the prior pair only when
// in_value is set. This stores an empty-valued header as "" and always clears
// the buffers, while NOT emitting a field name that llhttp splits across
// multiple on_header_field callbacks.
//
// Cells: empty-value preservation + no smuggling; split field name; split value;
// a normal multi-header request. Drives vws_http_msg_parse directly.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "http_message.h"

#include <string.h>

static cstr hdr(vws_http_msg* m, cstr k)
{
    return vws_kvs_get_cstring(m->headers, k);
}

CTEST(hdrpair, empty_value_no_smuggle)
{
    vws_http_msg* m = vws_http_msg_new(HTTP_REQUEST);
    const char* req =
        "GET /chat HTTP/1.1\r\n"
        "X-Empty:\r\n"
        "Host: example.com\r\n"
        "\r\n";
    ASSERT_TRUE(vws_http_msg_parse(m, (cstr)req, strlen(req)) >= 0);

    // Host survives (not lost, not mangled); empty header stored as "".
    ASSERT_TRUE(hdr(m, "host") != NULL);
    ASSERT_TRUE(strcmp(hdr(m, "host"), "example.com") == 0);
    ASSERT_TRUE(hdr(m, "x-empty") != NULL);
    ASSERT_TRUE(strcmp(hdr(m, "x-empty"), "") == 0);
    // No bogus concatenated key.
    ASSERT_TRUE(hdr(m, "x-emptyhost") == NULL);

    vws_http_msg_free(m);
}

CTEST(hdrpair, split_field_name)
{
    // Field NAME split across two parse calls (llhttp streams it). Must NOT emit
    // the partial name as an empty-valued header.
    vws_http_msg* m = vws_http_msg_new(HTTP_REQUEST);
    const char* a = "GET / HTTP/1.1\r\nCont";
    const char* b = "ent-Type: text/html\r\nHost: x\r\n\r\n";
    ASSERT_TRUE(vws_http_msg_parse(m, (cstr)a, strlen(a)) >= 0);
    ASSERT_TRUE(vws_http_msg_parse(m, (cstr)b, strlen(b)) >= 0);

    ASSERT_TRUE(hdr(m, "content-type") != NULL);
    ASSERT_TRUE(strcmp(hdr(m, "content-type"), "text/html") == 0);
    ASSERT_TRUE(hdr(m, "cont") == NULL);            // no premature partial emit
    ASSERT_TRUE(hdr(m, "host") != NULL);

    vws_http_msg_free(m);
}

CTEST(hdrpair, split_value)
{
    // Value split across two parse calls -> concatenated.
    vws_http_msg* m = vws_http_msg_new(HTTP_REQUEST);
    const char* a = "GET / HTTP/1.1\r\nX-Long: aaa";
    const char* b = "bbb\r\nHost: x\r\n\r\n";
    ASSERT_TRUE(vws_http_msg_parse(m, (cstr)a, strlen(a)) >= 0);
    ASSERT_TRUE(vws_http_msg_parse(m, (cstr)b, strlen(b)) >= 0);

    ASSERT_TRUE(hdr(m, "x-long") != NULL);
    ASSERT_TRUE(strcmp(hdr(m, "x-long"), "aaabbb") == 0);

    vws_http_msg_free(m);
}

CTEST(hdrpair, normal_multi_header)
{
    vws_http_msg* m = vws_http_msg_new(HTTP_REQUEST);
    const char* req =
        "GET /chat HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    ASSERT_TRUE(vws_http_msg_parse(m, (cstr)req, strlen(req)) >= 0);

    ASSERT_TRUE(strcmp(hdr(m, "host"), "localhost") == 0);
    ASSERT_TRUE(strcmp(hdr(m, "upgrade"), "websocket") == 0);
    ASSERT_TRUE(strcmp(hdr(m, "sec-websocket-key"), "dGhlIHNhbXBsZSBub25jZQ==") == 0);

    vws_http_msg_free(m);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
