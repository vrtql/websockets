// test_svr_splitbody.c — a bodied HTTP request whose body arrives in a
// later read event than its headers must be served, not closed.
//
// DEFECT (pristine): ws_svr_client_read gated its complete-request handling
// on cnx->http->headers_complete. on_headers_complete fires BEFORE any body
// byte is parsed; only on_message_complete sets `done` and pauses the
// parser (HPE_PAUSED). A read event that completed the HEADERS but not the
// BODY left llhttp at HPE_OK with done==false, the branch was entered
// anyway, and the errno check treated HPE_OK as a parse error: log
// "Error: HPE_OK", close, no reply. Every POST/PUT whose body arrived in a
// later read event than the headers was dropped. GET/upgrade (no body) set
// done in the same event as the headers, which is why the websocket
// handshake never exposed this.
//
// FIX under test: the gate is `done` (the sibling ws_svr_on_http_read gate
// already used it); the headers-done/body-pending event falls through to
// the incomplete-parse drain arm.
//
// CELLS (each asserts the FIXED contract — deterministic RED on pristine):
//   split_body_post — write the POST head (Content-Length: 4), flush, wait
//                     50ms, write the 4-byte body. Fixed: 200 reply, the
//                     http hook fired once. Pristine: connection closed
//                     right after the head ("Error: HPE_OK"), hook never
//                     fires, no reply.
//   control_oneshot — the same POST in ONE write. Serves on pristine AND
//                     fixed: proves the fixture, the hook, and the reply
//                     path are sound, isolating the RED to the split.
//
// One live server, fixed port 10197 (http_cov precedent 10195 + 2); the
// hook is registered pre-spawn and replies 200 itself (rc=1 contract:
// handler takes ownership of the request message).

#include "server.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

#include <unistd.h>
#include <stdatomic.h>
#include <string.h>

static cstr server_host = "127.0.0.1";
static int  server_port = 10197;

static atomic_int http_read_count;

// Reply 200 to any complete HTTP request; rc=1 contract (take ownership,
// server allocates a fresh request message).
static int on_http_read_cb(vws_svr_cnx* cnx)
{
    atomic_fetch_add(&http_read_count, 1);

    vws_buffer* http = vws_buffer_new();
    vws_buffer_printf(http, "HTTP/1.1 200 OK\r\n");
    vws_buffer_printf(http, "Content-Length: 0\r\n\r\n");

    vws_svr_data* reply = vws_svr_data_new(cnx->server, cnx->cid, &http);
    cnx->server->on_data_out(reply, NULL);
    vws_buffer_free(http);

    vws_http_msg_free(cnx->http);

    return 1;
}

static void server_thread(void* arg)
{
    vws_svr* server = (vws_svr*)arg;
    vws_tcp_svr_run((vws_tcp_svr*)server, server_host, server_port);
    vws_cleanup();
}

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

static void send_str(vws_socket* s, cstr data)
{
    vws_socket_write(s, (ucstr)data, strlen(data));
}

static const char* POST_HEAD =
    "POST /post HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Content-Length: 4\r\n"
    "\r\n";

static const char* POST_BODY = "wxyz";

CTEST(svr_splitbody, split_body_post)
{
    alarm(60);

    int before = atomic_load(&http_read_count);

    vws_socket* s = cnx_open();

    //> Headers first; body 50ms later in a separate write. Pristine closes
    // the connection on the head ("Error: HPE_OK", no reply); fixed waits
    // for the body and serves the request.
    send_str(s, POST_HEAD);
    vws_msleep(50);
    send_str(s, POST_BODY);

    ASSERT_TRUE(read_until(s, "200 OK") == true);
    ASSERT_TRUE(atomic_load(&http_read_count) == before + 1);

    vws_socket_free(s);
    alarm(0);
}

// Non-vacuity sibling: the same POST in one write is served on pristine
// AND fixed — the fixture, hook, and reply path are sound; the RED above
// is the split specifically.
CTEST(svr_splitbody, control_oneshot)
{
    alarm(60);

    int before = atomic_load(&http_read_count);

    vws_socket* s = cnx_open();

    vws_buffer* req = vws_buffer_new();
    vws_buffer_printf(req, "%s%s", POST_HEAD, POST_BODY);
    vws_socket_write(s, req->data, req->size);
    vws_buffer_free(req);

    ASSERT_TRUE(read_until(s, "200 OK") == true);
    ASSERT_TRUE(atomic_load(&http_read_count) == before + 1);

    vws_socket_free(s);
    alarm(0);
}

int main(int argc, const char* argv[])
{
    vws.tracelevel = 0;

    atomic_store(&http_read_count, 0);

    vws_svr* server     = vws_svr_new(2, 0, 0);
    server->on_http_read = on_http_read_cb;

    uv_thread_t tid;
    uv_thread_create(&tid, server_thread, server);

    while (vws_tcp_svr_state((vws_tcp_svr*)server) != VS_RUNNING)
    {
        vws_msleep(50);
    }

    int rc = ctest_main(argc, argv);

    vws_tcp_svr_stop((vws_tcp_svr*)server);
    uv_thread_join(&tid);
    vws_svr_free(server);

    return rc;
}
