// test_http_reqcap.c — bound the total HTTP request size (url + headers + body)
// on the upgrade ingress (pre-auth memory-exhaustion DoS + no false-positive).
//
// A single running total (request_bytes) is accumulated across on_url,
// on_header_field, on_header_value and on_body; when it exceeds
// max_request_size (default VWS_MAX_HTTP_REQUEST_SIZE, 128 KiB, settable) the
// callback fails the parse, so vws_http_msg_parse returns -1 and the server
// replies 414 (URI Too Long) if the request-line tripped it or 431 (Request
// Header Fields Too Large) if a header/body did, then closes before the
// handshake completes. This consolidates the earlier header-only cap into one
// total-request cap.
//
// Drives the real server ingress ws_svr_client_read, capturing the HTTP
// response via a stubbed on_data_out.
//   CELL 1 (url_cap):     oversized request line   -> 414 + close, no upgrade.
//   CELL 2 (header_cap):  oversized header block    -> 431 + close (the former
//                         header-flood, now rejected via the unified cap).
//   CELL 3 (body_cap):    oversized body            -> 431 + close.
//   CELL 4 (normal):      legit request under cap    -> 101 upgrade, no reject.
//
// SUBJECT, NOT MODIFIED. #includes server.c. Bounded (fixed sizes, watchdog).

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "http_message.h"

#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

static char g_out[512];
static size_t g_out_len = 0;

static void capture_data_out(vws_svr_data* data, void* x)
{
    (void)x;
    g_out_len = data->size < sizeof(g_out) - 1 ? data->size : sizeof(g_out) - 1;
    memcpy(g_out, data->data, g_out_len);
    g_out[g_out_len] = '\0';
    vws_svr_data_free(data);
}

static void server_read(vws_svr_cnx* cnx, const char* bytes, size_t n)
{
    uv_buf_t buf;
    buf.base = (char*)vws.malloc(n);
    memcpy(buf.base, bytes, n);
    buf.len  = n;
    ws_svr_client_read(cnx, (ssize_t)n, &buf);
}

static vws_svr* make_server(vws_svr_cnx* cnx, vws_cnx** cref)
{
    vws_svr* s = vws_svr_new(2, 0, 0);
    s->base.on_data_out = capture_data_out;

    vws_cnx* c = vws_cnx_new();
    memset(cnx, 0, sizeof(*cnx));
    cnx->server = (vws_tcp_svr*)s;
    vws_cid_clear(&cnx->cid);
    cnx->upgraded = false;
    cnx->http     = vws_http_msg_new(HTTP_REQUEST);
    cnx->data     = (char*)c;

    g_out_len = 0; g_out[0] = '\0';
    *cref = c;
    return s;
}

static void teardown(vws_svr* s, vws_cnx* c, vws_http_msg* keep_http)
{
    vws_svr_data* d;
    while (s->base.responses.size > 0)
    {
        d = queue_pop(&s->base.responses);
        vws_svr_data_free(d);
    }
    if (keep_http != NULL) { vws_http_msg_free(keep_http); }
    vws_cnx_free(c);
    vws_tcp_svr_free((vws_tcp_svr*)s);
}

CTEST(http_reqcap, url_cap)
{
    alarm(WATCHDOG_SECONDS);
    vws_svr_cnx cnx; vws_cnx* c; vws_svr* s = make_server(&cnx, &c);

    ASSERT_TRUE(cnx.http->max_request_size == VWS_MAX_HTTP_REQUEST_SIZE);

    // "GET /" + ~192 KiB of 'a' + " HTTP/1.1\r\n\r\n" -> URL alone exceeds 128 KiB.
    size_t url = 192u * 1024u;
    size_t cap = url + 64;
    char* req  = (char*)vws.malloc(cap);
    size_t off = 0;
    off += snprintf(req + off, cap - off, "GET /");
    memset(req + off, 'a', url); off += url;
    off += snprintf(req + off, cap - off, " HTTP/1.1\r\n\r\n");
    server_read(&cnx, req, off);
    vws.free(req);

    ASSERT_TRUE(cnx.http->request_bytes > cnx.http->max_request_size);
    ASSERT_TRUE(strstr(g_out, "414") != NULL);
    ASSERT_TRUE(strstr(g_out, "URI Too Long") != NULL);
    ASSERT_TRUE(cnx.upgraded == false);
    ASSERT_TRUE(s->base.responses.size >= 1);

    teardown(s, c, cnx.http);
    alarm(0);
}

CTEST(http_reqcap, header_cap)
{
    alarm(WATCHDOG_SECONDS);
    vws_svr_cnx cnx; vws_cnx* c; vws_svr* s = make_server(&cnx, &c);

    // Header block exceeding 128 KiB (the former header-flood; still rejected).
    size_t cap = 256u * 1024u;
    char*  blob = (char*)vws.malloc(cap);
    size_t off  = 0;
    off += snprintf(blob + off, cap - off, "GET / HTTP/1.1\r\n");
    for (int i = 0; i < 9000; i++)
    {
        off += snprintf(blob + off, cap - off, "H%06d: vvvvvvvvvv\r\n", i);
    }
    server_read(&cnx, blob, off);
    vws.free(blob);

    ASSERT_TRUE(cnx.http->request_bytes > cnx.http->max_request_size);
    ASSERT_TRUE(strstr(g_out, "431") != NULL);
    ASSERT_TRUE(strstr(g_out, "Request Header Fields Too Large") != NULL);
    ASSERT_TRUE(cnx.upgraded == false);

    teardown(s, c, cnx.http);
    alarm(0);
}

CTEST(http_reqcap, body_cap)
{
    alarm(WATCHDOG_SECONDS);
    vws_svr_cnx cnx; vws_cnx* c; vws_svr* s = make_server(&cnx, &c);

    // A POST whose body exceeds 128 KiB.
    size_t body = 192u * 1024u;
    size_t cap  = body + 128;
    char*  req  = (char*)vws.malloc(cap);
    size_t off  = snprintf(req, cap,
        "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body);
    memset(req + off, 'b', body); off += body;
    server_read(&cnx, req, off);
    vws.free(req);

    ASSERT_TRUE(cnx.http->request_bytes > cnx.http->max_request_size);
    ASSERT_TRUE(strstr(g_out, "431") != NULL);
    ASSERT_TRUE(cnx.upgraded == false);

    teardown(s, c, cnx.http);
    alarm(0);
}

CTEST(http_reqcap, normal_upgrade)
{
    alarm(WATCHDOG_SECONDS);
    vws_svr_cnx cnx; vws_cnx* c; vws_svr* s = make_server(&cnx, &c);

    const char* req =
        "GET /chat HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    server_read(&cnx, req, strlen(req));

    ASSERT_TRUE(cnx.upgraded == true);
    ASSERT_TRUE(strstr(g_out, "101") != NULL);
    ASSERT_TRUE(strstr(g_out, "Switching Protocols") != NULL);
    ASSERT_TRUE(strstr(g_out, "414") == NULL);
    ASSERT_TRUE(strstr(g_out, "431") == NULL);

    // http freed on upgrade.
    teardown(s, c, NULL);
    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
