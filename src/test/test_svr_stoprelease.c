// test_svr_stoprelease.c -- a request block that never reaches a worker must
// release the message object it carries (server.c svr_data_dispose).
//
// DEFECT: the loop thread hands a worker a vws_svr_data whose data is a
// POINTER to a message object: a vws_msg* (ws_svr_client_read) or a
// vws_http_msg* (ws_svr_on_http_read). The worker-side consumer
// (ws_svr_client_data_in) takes the object and clears data before it frees
// the block. When the server stops first, the block is released by the server
// itself, in one of two places:
//
//   - svr_shutdown() drains what is still resident in the requests queue
//     (queue_pop answers NULL as soon as the queue is halting, even with items
//     queued, so workers leave items behind);
//   - queue_push() on a queue that is no longer running drops the block.
//
// Both ended in vws_svr_data_free(), which does free(data): that releases the
// outer struct of the message only. A vws_msg lost its vws_buffer and bytes
// (valgrind: "24 direct" + the data); a vws_http_msg lost its parser, header
// table and buffers.
//
// These cells make the block with the PRODUCTION producers (bytes go in
// through ws_svr_client_read, as from a libuv read callback) and release it
// with the PRODUCTION stop code. There is nothing to assert about lost memory
// from inside the process: the observer is valgrind. The program is registered
// twice in CMakeLists.txt -- plain, for the functional asserts, and as
// test_svr_stoprelease_memcheck under
//   valgrind --leak-check=full --error-exitcode=9
// which FAILS on any lost block. The control cells take the normal worker
// path and lose nothing with or without the fix.
//
// Out of scope here, on purpose: IRQ blocks and PEER_CONNECT blocks.
//
// SUBJECT, NOT MODIFIED: server.c is #included so the statics are reachable.
// No sockets, no threads, no reactor run. A watchdog alarm bounds every cell.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "websocket.h"

#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

// Counters for the control cells' application callbacks.
static int ws_delivered   = 0;
static int http_delivered = 0;

static void app_process_ws(vws_svr* s, vws_cid_t c, vws_msg* m, void* x)
{
    (void)s; (void)c; (void)x;
    ws_delivered++;

    // The application owns the message (see ws_svr_client_process).
    vws_msg_free(m);
}

static bool app_process_http(vws_svr* s, vws_cid_t c, vws_http_msg* m, void* x)
{
    (void)s; (void)c; (void)m; (void)x;
    http_delivered++;

    // The caller (ws_svr_client_data_in) frees the request.
    return true;
}

// Replies the read path may write go nowhere: there is no connection.
static void discard_data_out(vws_svr_data* data, void* x)
{
    (void)x;
    vws_svr_data_free(data);
}

// A server-side connection as the accept path would leave it, without a
// socket: enough for ws_svr_client_read to parse bytes and queue a request.
typedef struct
{
    vws_svr_cnx cnx;
    vws_cnx*    c;
} test_cnx;

static void test_cnx_open(test_cnx* t, vws_svr* s, bool upgraded)
{
    memset(&t->cnx, 0, sizeof(t->cnx));

    t->c          = vws_cnx_new();
    t->c->process = ws_svr_process_frame;
    t->c->data    = (char*)&t->cnx;

    t->cnx.server   = (vws_tcp_svr*)s;
    t->cnx.data     = (char*)t->c;
    t->cnx.upgraded = upgraded;
    vws_cid_clear(&t->cnx.cid);

    if (upgraded == false)
    {
        t->cnx.http = vws_http_msg_new(HTTP_REQUEST);
    }
}

static void test_cnx_close(test_cnx* t)
{
    if (t->cnx.http != NULL)
    {
        vws_http_msg_free(t->cnx.http);
    }

    vws_cnx_free(t->c);
}

// Hand bytes to the server exactly as svr_on_read does: a heap buffer the
// read function takes ownership of.
static void feed_bytes(test_cnx* t, const unsigned char* bytes, size_t size)
{
    uv_buf_t buf;
    buf.base = (char*)vws.malloc(size);
    buf.len  = size;
    memcpy(buf.base, bytes, size);

    ws_svr_client_read(&t->cnx, (ssize_t)size, &buf);
}

// One complete, masked, 115-byte binary message as a client would send it.
static void feed_ws_message(test_cnx* t)
{
    unsigned char payload[115];
    memset(payload, 0x5A, sizeof(payload));

    vws_frame* f     = vws_frame_new(payload, sizeof(payload), BINARY_FRAME);
    vws_buffer* wire = vws_serialize(f);

    feed_bytes(t, wire->data, wire->size);
    vws_buffer_free(wire);
}

// One complete HTTP request that is not an upgrade.
static void feed_http_request(test_cnx* t)
{
    cstr text = "GET /status HTTP/1.1\r\nHost: test\r\nX-One: two\r\n\r\n";
    feed_bytes(t, (const unsigned char*)text, strlen(text));
}

static vws_svr* server_open(void)
{
    vws_svr* s = vws_svr_new(1, 0, 0);

    s->process_ws       = app_process_ws;
    s->process_http     = app_process_http;
    s->base.on_data_out = discard_data_out;

    // A constructed server is VS_HALTED until vws_tcp_svr_run() marks it
    // running, and svr_shutdown() returns at once on a halted server. Mark it
    // as run() would, so the stop code under test does its work.
    s->base.state = VS_RUNNING;

    return s;
}

// What vws_tcp_svr_stop() writes before it wakes anything. The function
// itself is not called: it waits on worker threads this cell does not have.
static void server_mark_halting(vws_svr* s)
{
    s->base.state           = VS_HALTING;
    s->base.requests.state  = VS_HALTING;
    s->base.responses.state = VS_HALTING;
}

static void server_close(vws_svr* s)
{
    // The stop path under test: drain, then destroy.
    svr_shutdown((vws_tcp_svr*)s);
    vws_svr_free(s);
}

//------------------------------------------------------------------------------
// WebSocket message block
//------------------------------------------------------------------------------

CTEST(svr_stoprelease, ws_msg_resident_at_shutdown)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr* s = server_open();
    test_cnx t;
    test_cnx_open(&t, s, true);

    feed_ws_message(&t);

    // The message is queued for a worker that will never take it.
    ASSERT_EQUAL(1, (int)s->base.requests.size);

    server_close(s);
    test_cnx_close(&t);

    alarm(0);
}

CTEST(svr_stoprelease, ws_msg_pushed_on_halting_queue)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr* s = server_open();
    test_cnx t;
    test_cnx_open(&t, s, true);

    // Stop was called; the loop thread still reads one more message.
    server_mark_halting(s);
    feed_ws_message(&t);

    // queue_push() dropped it: nothing is resident.
    ASSERT_EQUAL(0, (int)s->base.requests.size);

    server_close(s);
    test_cnx_close(&t);

    alarm(0);
}

//------------------------------------------------------------------------------
// HTTP request block
//------------------------------------------------------------------------------

CTEST(svr_stoprelease, http_resident_at_shutdown)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr* s = server_open();
    test_cnx t;
    test_cnx_open(&t, s, false);

    feed_http_request(&t);

    ASSERT_EQUAL(1, (int)s->base.requests.size);

    // It is the HTTP kind of block.
    vws_svr_data* head = s->base.requests.buffer[s->base.requests.head];
    ASSERT_TRUE(vws_is_flag(&head->flags, VWS_SVR_STATE_HTTP));

    server_close(s);
    test_cnx_close(&t);

    alarm(0);
}

CTEST(svr_stoprelease, http_pushed_on_halting_queue)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr* s = server_open();
    test_cnx t;
    test_cnx_open(&t, s, false);

    server_mark_halting(s);
    feed_http_request(&t);

    ASSERT_EQUAL(0, (int)s->base.requests.size);

    server_close(s);
    test_cnx_close(&t);

    alarm(0);
}

//------------------------------------------------------------------------------
// Controls: the block reaches the worker-side consumer. Nothing is lost with
// or without the fix, and the application gets its message exactly once.
//------------------------------------------------------------------------------

CTEST(svr_stoprelease, control_ws_msg_consumed)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr* s = server_open();
    test_cnx t;
    test_cnx_open(&t, s, true);

    ws_delivered = 0;
    feed_ws_message(&t);
    ASSERT_EQUAL(1, (int)s->base.requests.size);

    // What worker_thread() does with a request.
    vws_svr_data* request = queue_pop(&s->base.requests);
    ASSERT_NOT_NULL(request);
    s->base.on_data_in(request, NULL);

    ASSERT_EQUAL(1, ws_delivered);
    ASSERT_EQUAL(0, (int)s->base.requests.size);

    server_close(s);
    test_cnx_close(&t);

    alarm(0);
}

CTEST(svr_stoprelease, control_http_consumed)
{
    alarm(WATCHDOG_SECONDS);

    vws_svr* s = server_open();
    test_cnx t;
    test_cnx_open(&t, s, false);

    http_delivered = 0;
    feed_http_request(&t);
    ASSERT_EQUAL(1, (int)s->base.requests.size);

    vws_svr_data* request = queue_pop(&s->base.requests);
    ASSERT_NOT_NULL(request);
    s->base.on_data_in(request, NULL);

    ASSERT_EQUAL(1, http_delivered);
    ASSERT_EQUAL(0, (int)s->base.requests.size);

    server_close(s);
    test_cnx_close(&t);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
