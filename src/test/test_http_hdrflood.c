// test_http_hdrflood.c — bound total header size during the HTTP upgrade
// handshake (pre-auth memory-exhaustion DoS + no-false-positive).
//
// BUG (pre-fix): the WS-upgrade HTTP parser accumulated header field/value
// bytes with no limit; a client streaming many headers -- or one enormous
// header -- grew the buffers without bound before the request completed,
// exhausting memory pre-handshake (pre-auth).
//
// FIX: track total header bytes (field-name + value) per request; when the
// aggregate exceeds max_header_size (default VWS_MAX_HTTP_HEADER_SIZE = 64 KiB,
// settable) the header callback fails the parse (llhttp error) so
// vws_http_msg_parse returns -1, and the server replies 431 (Request Header
// Fields Too Large) and closes the connection before the handshake completes.
//
// This drives the REAL server ingress ws_svr_client_read end-to-end (parser cap
// + server 431/close/upgrade), capturing the HTTP response via a stubbed
// on_data_out.
//   CELL 1 (cap_enforced): an over-cap header stream -> bytes tracked past the
//   cap, parse fails, a 431 is emitted, the close is queued, no upgrade.
//   CELL 2 (under_cap_upgrade): a legit upgrade request under the cap -> 101
//   Switching Protocols, connection upgraded, no 431 (no false-positive).
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

// Capture the HTTP response the server emits (101 or 431) instead of writing to
// a socket.
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

// Feed a raw byte blob to the server read path (mirrors a libuv read callback).
static void server_read(vws_svr_cnx* cnx, const char* bytes, size_t n)
{
    uv_buf_t buf;
    buf.base = (char*)vws.malloc(n);      // ws_svr_client_read frees this
    memcpy(buf.base, bytes, n);
    buf.len  = n;
    ws_svr_client_read(cnx, (ssize_t)n, &buf);
}

// Wire a server-side connection to the real ws ingress with a capturing
// data-out. Returns the cnx by out-param; caller frees.
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

    g_out_len = 0;
    g_out[0]  = '\0';
    *cref = c;
    return s;
}

CTEST(http_hdrcap, cap_enforced)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr_cnx cnx;
    vws_cnx* c;
    vws_svr* s = make_server(&cnx, &c);

    // Fresh request defaults to the 64 KiB header cap.
    ASSERT_TRUE(cnx.http->max_header_size == VWS_MAX_HTTP_HEADER_SIZE);

    // Build a request line + a header block that exceeds 64 KiB with NO
    // terminating blank line, and feed it in ONE read. ~6000 "H......: v...\r\n"
    // lines (~18 B each) -> > 64 KiB. (One read mirrors a single large recv;
    // the cap trips mid-parse.)
    size_t cap    = 128u * 1024u;
    char*  blob   = (char*)vws.malloc(cap);
    size_t off    = 0;
    off += snprintf(blob + off, cap - off, "GET / HTTP/1.1\r\n");
    for (int i = 0; i < 6000; i++)
    {
        off += snprintf(blob + off, cap - off, "H%06d: vvvvvvvv\r\n", i);
    }
    server_read(&cnx, blob, off);
    vws.free(blob);

    // GREEN: bytes tracked past the cap; a 431 was emitted; the close was
    // queued; the connection did NOT upgrade.
    ASSERT_TRUE(cnx.http->header_bytes > cnx.http->max_header_size);
    ASSERT_TRUE(strstr(g_out, "431") != NULL);
    ASSERT_TRUE(strstr(g_out, "Request Header Fields Too Large") != NULL);
    ASSERT_TRUE(cnx.upgraded == false);
    ASSERT_TRUE(s->base.responses.size >= 1);   // svr_cnx_close queued a close

    // Teardown.
    vws_svr_data* d;
    while (s->base.responses.size > 0)
    {
        d = queue_pop(&s->base.responses);
        vws_svr_data_free(d);
    }
    vws_http_msg_free(cnx.http);
    vws_cnx_free(c);
    vws_tcp_svr_free((vws_tcp_svr*)s);
    alarm(0);
}

CTEST(http_hdrcap, under_cap_upgrade)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr_cnx cnx;
    vws_cnx* c;
    vws_svr* s = make_server(&cnx, &c);

    // A legitimate, complete WS upgrade request well under 64 KiB.
    const char* req =
        "GET /chat HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    server_read(&cnx, req, strlen(req));

    // No false-positive: upgraded cleanly with a 101, no 431. (ws_svr_client_read
    // frees cnx.http on upgrade and sets cnx.http = NULL.)
    ASSERT_TRUE(cnx.upgraded == true);
    ASSERT_TRUE(strstr(g_out, "101") != NULL);
    ASSERT_TRUE(strstr(g_out, "Switching Protocols") != NULL);
    ASSERT_TRUE(strstr(g_out, "431") == NULL);

    // Teardown.
    vws_svr_data* d;
    while (s->base.responses.size > 0)
    {
        d = queue_pop(&s->base.responses);
        vws_svr_data_free(d);
    }
    vws_cnx_free(c);
    vws_tcp_svr_free((vws_tcp_svr*)s);
    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
