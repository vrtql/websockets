// test_svr_pipeline.c — a pipelined second HTTP request buffered in the SAME
// read event must be served in that event.
//
// ws_svr_client_read, on a complete request, drains that request's bytes,
// hands it to on_http_read, and returns. It is the only server-side parse
// entry and runs only on a NEW uv read event — so a second complete request
// already sitting in the connection buffer (a pipelining client wrote both
// requests in one segment) is not parsed until the client sends MORE bytes.
// A client that writes N requests and then blocks reading N responses gets
// one response and stalls. No data loss; a liveness gap on the (rare)
// pipelining pattern. GET/upgrade single-request flows are unaffected.
//
// Drives the real server ingress ws_svr_client_read directly with a stubbed
// on_data_out and a counting on_http_read (the test_http_reqcap pattern —
// one server_read call == one uv read event, so "both requests in one send"
// is exact and deterministic, no sockets).
//
//   CELL 1 (pipelined_one_event): two complete GETs in ONE read event.
//     Buggy: one request handled, one response; the second sits buffered.
//     Fixed: both handled in that event, two responses.
//   CELL 2 (two_events): the same two GETs in two read events — two
//     responses on buggy and fixed alike (the control).
//   CELL 3 (partial_tail): request-1 + a PARTIAL request-2 in event one,
//     the rest of request-2 in event two. Two responses on buggy and fixed
//     alike — pins that the re-parse loop composes with the done-gate (a
//     partial tail falls to the drain arm; no spin, no double-serve).
//
// SUBJECT, NOT MODIFIED. #includes server.c. Bounded (watchdog).

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

static int g_handled   = 0;   // requests reaching on_http_read
static int g_responses = 0;   // replies leaving via on_data_out

static void count_data_out(vws_svr_data* data, void* x)
{
    (void)x;
    g_responses++;
    vws_svr_data_free(data);
}

// Handle one HTTP request: emit a minimal response and report it handled.
// Returning 1 takes ownership of the completed request (the reference
// handler ws_svr_on_http_read frees it the same way); the server then
// allocates a fresh parser for the next request.
static int count_http_read(vws_svr_cnx* cnx)
{
    g_handled++;

    vws_buffer* http = vws_buffer_new();
    vws_buffer_printf(http, "HTTP/1.1 200 OK\r\n");
    vws_buffer_printf(http, "Content-Length: 0\r\n\r\n");

    vws_svr_data* reply = vws_svr_data_new(cnx->server, cnx->cid, &http);
    cnx->server->on_data_out(reply, NULL);
    vws_buffer_free(http);

    vws_http_msg_free(cnx->http);

    return 1;
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
    s->base.on_data_out = count_data_out;
    s->on_http_read     = count_http_read;

    vws_cnx* c = vws_cnx_new();
    memset(cnx, 0, sizeof(*cnx));
    cnx->server = (vws_tcp_svr*)s;
    vws_cid_clear(&cnx->cid);
    cnx->upgraded = false;
    cnx->http     = vws_http_msg_new(HTTP_REQUEST);
    cnx->data     = (char*)c;

    g_handled   = 0;
    g_responses = 0;
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

static const char* REQ1 = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
static const char* REQ2 = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";

// SUBJECT — both complete requests arrive in one read event; both must be
// served in that event. Buggy source: handled == 1 (the second request waits
// for bytes that never come).
CTEST(svr_pipeline, pipelined_one_event)
{
    alarm(WATCHDOG_SECONDS);
    vws_svr_cnx cnx; vws_cnx* c; vws_svr* s = make_server(&cnx, &c);

    char both[128];
    size_t n = (size_t)snprintf(both, sizeof(both), "%s%s", REQ1, REQ2);
    server_read(&cnx, both, n);

    ASSERT_TRUE(g_handled == 2);
    ASSERT_TRUE(g_responses == 2);

    vws_cnx* cc = (vws_cnx*)cnx.data;
    ASSERT_TRUE(cc->base.buffer->size == 0);

    teardown(s, c, cnx.http);
    alarm(0);
}

// CONTROL — the same two requests in two read events: two responses on
// buggy and fixed source alike.
CTEST(svr_pipeline, two_events)
{
    alarm(WATCHDOG_SECONDS);
    vws_svr_cnx cnx; vws_cnx* c; vws_svr* s = make_server(&cnx, &c);

    server_read(&cnx, REQ1, strlen(REQ1));
    server_read(&cnx, REQ2, strlen(REQ2));

    ASSERT_TRUE(g_handled == 2);
    ASSERT_TRUE(g_responses == 2);

    teardown(s, c, cnx.http);
    alarm(0);
}

// COMPOSITION — request-1 plus a PARTIAL request-2 in one event, the rest in
// a second event. Pins that the re-parse composes with the done-gate: the
// partial tail falls to the incomplete-parse drain arm (no spin), is not
// double-served, and completes on the next event. Two on both sources.
CTEST(svr_pipeline, partial_tail)
{
    alarm(WATCHDOG_SECONDS);
    vws_svr_cnx cnx; vws_cnx* c; vws_svr* s = make_server(&cnx, &c);

    size_t split = 9;   // "GET /b HT" — mid-request-line

    char first[128];
    size_t n = (size_t)snprintf(first, sizeof(first), "%s%.*s",
                                REQ1, (int)split, REQ2);
    server_read(&cnx, first, n);

    ASSERT_TRUE(g_handled == 1);

    server_read(&cnx, REQ2 + split, strlen(REQ2) - split);

    ASSERT_TRUE(g_handled == 2);
    ASSERT_TRUE(g_responses == 2);

    teardown(s, c, cnx.http);
    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
