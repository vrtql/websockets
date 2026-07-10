// test_svr_http_cov.c — live coverage cell for server.c's HTTP-upgrade +
// header-cap arms (lane-1 coverage campaign; TEST ONLY, no product change).
//
// Targets the live (d) branches in ws_svr_client_data_in's HTTP half:
//
//   - n<0 oversize replies: 414 (request-line tripped the 128 KiB
//     max_request_size) vs 431 (headers tripped it) vs neither (garbage
//     parse error, no reply) — the status==414/431 arms + reason ternary.
//   - headers_complete false arm (split request, second write completes it).
//   - upgrade with missing Sec-WebSocket-Key (close, no 101).
//   - upgrade WITH Sec-WebSocket-Protocol (101 echoes the client protocol).
//   - on_upgrade callback registered (fires post-101).
//   - pipelined WS frame appended to the upgrade request in one write
//     (post-upgrade buffer non-empty arm).
//   - on_http_read rc arms: 0 (keep reading), 1 (new request message),
//     -1 (server closes).
//
// CONTROL (non-vacuity): a plain non-upgrade GET must produce NO 101 and
// must NOT fire on_upgrade — the upgrade arms are reached by upgrade
// requests specifically, not by any traffic.
//
// One live server, fixed port; ALL hooks are registered BEFORE the server
// thread spawns (the onStarting/pre-spawn discipline — no post-spawn field
// writes). The on_http_read handler picks its rc from the request URL, so
// no server state is mutated between cells. Bounded reads + alarm watchdog.

#include "server.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>

static cstr server_host = "127.0.0.1";
static int  server_port = 10195;

static const int WATCHDOG_SECONDS = 90;

static atomic_int upgrade_count;    // on_upgrade firings
static atomic_int http_read_count;  // on_http_read firings

// rc from the URL so the hook is configured once, pre-spawn, and never
// mutated: /keep -> 0 (keep reading), /again -> 1 (fresh request message),
// anything else -> -1 (not handling HTTP; server closes).
static int on_http_read_cb(vws_svr_cnx* cnx)
{
    atomic_fetch_add(&http_read_count, 1);

    cstr url = (cstr)cnx->http->url->data;

    if (url != NULL && strncmp(url, "/keep", 5) == 0)
    {
        return 0;
    }

    if (url != NULL && strncmp(url, "/again", 6) == 0)
    {
        // rc=1 contract (server.h vws_svr_http_read): the handler TAKES
        // OWNERSHIP of the request object; the server allocates a new one.
        vws_http_msg_free(cnx->http);

        return 1;
    }

    return -1;
}

static void on_upgrade_cb(vws_svr_cnx* cnx)
{
    (void)cnx;
    atomic_fetch_add(&upgrade_count, 1);
}

static void server_thread(void* arg)
{
    vws_svr* server = (vws_svr*)arg;
    vws_tcp_svr_run((vws_tcp_svr*)server, server_host, server_port);
    vws_cleanup();
}

// Fresh client connection.
static vws_socket* cnx_open(void)
{
    vws_socket* s = vws_socket_new();

    while (vws_socket_connect(s, server_host, server_port, false) == false)
    {
        vws_msleep(50);
    }

    return s;
}

// Portable substring search over a byte buffer (no GNU memmem).
static bool buf_contains(vws_buffer* b, cstr needle)
{
    size_t nlen = strlen(needle);

    if (b->data == NULL || b->size < nlen || nlen == 0)
    {
        return false;
    }

    for (size_t i = 0; i + nlen <= b->size; i++)
    {
        if (memcmp(b->data + i, needle, nlen) == 0)
        {
            return true;
        }
    }

    return false;
}

// Read until the buffer contains needle, the peer closes, or ~2s passes.
// Returns true if needle was seen.
static bool read_until(vws_socket* s, cstr needle)
{
    for (int i = 0; i < 40; i++)
    {
        if (buf_contains(s->buffer, needle))
        {
            return true;
        }

        if (vws_socket_is_connected(s) == false)
        {
            break;
        }

        vws_socket_read(s);
        vws_msleep(50);
    }

    return buf_contains(s->buffer, needle);
}

// Wait (bounded) for the server to close the connection; true if it did.
static bool wait_for_close(vws_socket* s)
{
    for (int i = 0; i < 40; i++)
    {
        vws_socket_read(s);

        if (vws_socket_is_connected(s) == false)
        {
            return true;
        }

        vws_msleep(50);
    }

    return false;
}

static void send_str(vws_socket* s, cstr data)
{
    vws_socket_write(s, (ucstr)data, strlen(data));
}

// The shared live server, started once (ctest cells run in-order in one
// process; the server outlives all cells and stops in the last one).
static vws_svr*   g_server = NULL;
static uv_thread_t g_server_tid;

static void ensure_server(void)
{
    if (g_server != NULL)
    {
        return;
    }

    alarm(WATCHDOG_SECONDS);

    g_server = vws_svr_new(2, 0, 0);

    // ALL hooks pre-spawn (loop not yet running).
    g_server->on_http_read = on_http_read_cb;
    g_server->on_upgrade   = on_upgrade_cb;

    uv_thread_create(&g_server_tid, server_thread, g_server);

    while (vws_tcp_svr_state((vws_tcp_svr*)g_server) != VS_RUNNING)
    {
        vws_msleep(50);
    }
}

// --- oversize: request line > 128 KiB -> 414 + close -------------------

CTEST(svr_http_cov, cap_414_uri_too_long)
{
    ensure_server();

    size_t pad = 132 * 1024;
    char*  url = (char*)vws.malloc(pad + 1);
    memset(url, 'a', pad);
    url[pad] = '\0';

    vws_socket* s = cnx_open();
    send_str(s, "GET /");
    vws_socket_write(s, (ucstr)url, pad);
    send_str(s, " HTTP/1.1\r\nHost: x\r\n\r\n");

    ASSERT_TRUE(read_until(s, "414 URI Too Long"));
    ASSERT_TRUE(wait_for_close(s));

    vws.free(url);
    vws_socket_free(s);
}

// --- oversize: headers > 128 KiB -> 431 + close -------------------------

CTEST(svr_http_cov, cap_431_headers_too_large)
{
    ensure_server();

    size_t pad = 132 * 1024;
    char*  big = (char*)vws.malloc(pad + 1);
    memset(big, 'b', pad);
    big[pad] = '\0';

    vws_socket* s = cnx_open();
    send_str(s, "GET /close HTTP/1.1\r\nHost: x\r\nX-Big: ");
    vws_socket_write(s, (ucstr)big, pad);
    send_str(s, "\r\n\r\n");

    ASSERT_TRUE(read_until(s, "431 Request Header Fields Too Large"));
    ASSERT_TRUE(wait_for_close(s));

    vws.free(big);
    vws_socket_free(s);
}

// --- garbage: fatal parse error, no oversize status -> close, no reply --

CTEST(svr_http_cov, parse_error_no_status)
{
    ensure_server();

    vws_socket* s = cnx_open();
    send_str(s, "\x01\x02\x03 garbage \r\n\r\n");

    ASSERT_TRUE(wait_for_close(s));
    // No HTTP status line was sent for a non-oversize parse error.
    ASSERT_TRUE(s->buffer->size == 0 ||
                !buf_contains(s->buffer, "HTTP/1.1"));

    vws_socket_free(s);
}

// --- upgrade with missing Sec-WebSocket-Key -> close, no 101 ------------

CTEST(svr_http_cov, upgrade_missing_key)
{
    ensure_server();

    vws_socket* s = cnx_open();
    send_str(s,
        "GET /chat HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n");

    ASSERT_TRUE(wait_for_close(s));
    ASSERT_TRUE(s->buffer->size == 0 ||
                !buf_contains(s->buffer, "101"));

    vws_socket_free(s);
}

// --- upgrade WITH subprotocol -> 101 echoes it; on_upgrade fires --------

CTEST(svr_http_cov, upgrade_with_protocol)
{
    ensure_server();

    int before = atomic_load(&upgrade_count);

    vws_socket* s = cnx_open();
    send_str(s,
        "GET /chat HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "\r\n");

    ASSERT_TRUE(read_until(s, "101 Switching Protocols"));
    ASSERT_TRUE(read_until(s, "Sec-WebSocket-Protocol: chat"));

    // on_upgrade ran on the loop thread; bounded wait for the counter.
    bool fired = false;
    for (int i = 0; i < 40 && !fired; i++)
    {
        fired = atomic_load(&upgrade_count) > before;
        vws_msleep(50);
    }
    ASSERT_TRUE(fired);

    vws_socket_free(s);
}

// --- upgrade + pipelined WS frame in ONE write ---------------------------
// Covers the post-upgrade "socket buffer NOT empty" arm: the client appends
// a masked text frame ("hi") to the upgrade request so WebSocket bytes are
// already buffered when the 101 goes out.

CTEST(svr_http_cov, upgrade_pipelined_frame)
{
    ensure_server();

    const char req[] =
        "GET /chat HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    // Masked client text frame, payload "hi", mask key 0x11 0x22 0x33 0x44.
    const unsigned char frame[] =
    {
        0x81, 0x82, 0x11, 0x22, 0x33, 0x44,
        (unsigned char)('h' ^ 0x11), (unsigned char)('i' ^ 0x22)
    };

    size_t total = (sizeof(req) - 1) + sizeof(frame);
    unsigned char* wire = (unsigned char*)vws.malloc(total);
    memcpy(wire, req, sizeof(req) - 1);
    memcpy(wire + sizeof(req) - 1, frame, sizeof(frame));

    vws_socket* s = cnx_open();
    vws_socket_write(s, wire, total);          // one write: request + frame

    ASSERT_TRUE(read_until(s, "101 Switching Protocols"));
    // No subprotocol was offered: server advertises its default.
    ASSERT_TRUE(read_until(s, "Sec-WebSocket-Protocol: vrtql"));

    vws.free(wire);
    vws_msleep(200);                           // let the frame route
    vws_socket_free(s);
}

// --- split request: headers_complete false arm, then completion ---------

CTEST(svr_http_cov, split_request_completes)
{
    ensure_server();

    int before = atomic_load(&http_read_count);

    vws_socket* s = cnx_open();
    send_str(s, "GET /close HTTP/1.1\r\nHos");   // partial: no complete headers
    vws_msleep(300);                             // parsed, incomplete arm taken
    send_str(s, "t: x\r\n\r\n");                 // completes -> on_http_read -1

    ASSERT_TRUE(wait_for_close(s));

    bool handled = false;
    for (int i = 0; i < 40 && !handled; i++)
    {
        handled = atomic_load(&http_read_count) > before;
        vws_msleep(50);
    }
    ASSERT_TRUE(handled);

    vws_socket_free(s);
}

// --- on_http_read rc=0 (keep reading) and rc=1 (fresh request) ----------

CTEST(svr_http_cov, http_read_keep_and_again)
{
    ensure_server();

    // rc=0: server keeps the connection open, no reply.
    vws_socket* s0 = cnx_open();
    send_str(s0, "GET /keep HTTP/1.1\r\nHost: x\r\n\r\n");
    vws_msleep(300);
    ASSERT_TRUE(vws_socket_is_connected(s0));    // NOT closed by rc=0
    vws_socket_free(s0);

    // rc=1: server allocates a fresh request message and keeps reading;
    // a SECOND request on the same connection parses into the new message.
    vws_socket* s1 = cnx_open();
    send_str(s1, "GET /again HTTP/1.1\r\nHost: x\r\n\r\n");
    vws_msleep(300);
    ASSERT_TRUE(vws_socket_is_connected(s1));
    send_str(s1, "GET /close HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(wait_for_close(s1));             // second request -> rc=-1
    vws_socket_free(s1);
}

// --- CONTROL: plain non-upgrade GET must not touch the upgrade arms -----

CTEST(svr_http_cov, control_plain_get_no_upgrade)
{
    ensure_server();

    int before = atomic_load(&upgrade_count);

    vws_socket* s = cnx_open();
    send_str(s, "GET /close HTTP/1.1\r\nHost: x\r\n\r\n");

    ASSERT_TRUE(wait_for_close(s));              // rc=-1 close, no upgrade
    ASSERT_TRUE(s->buffer->size == 0 ||
                !buf_contains(s->buffer, "101"));
    ASSERT_TRUE(atomic_load(&upgrade_count) == before);

    vws_socket_free(s);

    // Last cell: stop the shared server.
    vws_msleep(200);
    vws_tcp_svr_stop((vws_tcp_svr*)g_server);
    uv_thread_join(&g_server_tid);
    vws_svr_free(g_server);
    g_server = NULL;

    alarm(0);
}

int main(int argc, const char* argv[])
{
    // Debug aid: COV_TRACE=1 turns on the full vws trace firehose.
    if (getenv("COV_TRACE") != NULL)
    {
        vws.tracelevel = VT_ALL;
    }

    return ctest_main(argc, argv);
}
