// test_svr_msgflood.c — Bound the reassembled-message size on the
// broker ingress (fragmentation memory-exhaustion DoS + no-false-positive).
//
// BUG (pre-fix): ws_svr_process_frame queued every TEXT/BINARY/CONTINUATION
// frame into the per-connection reassembly queue with no cap on aggregate bytes;
// a peer streaming CONTINUATION frames (fin=0) grew it without bound -> OOM.
//
// FIX: accumulate f->size per in-progress message; when the aggregate exceeds
// cnx->max_message_size (default VWS_MAX_MESSAGE_SIZE = 64 MiB, settable), send a
// 1009 (Message Too Big) close, flag the connection closing, and stop queuing.
// Reset the aggregate when a message completes (fin=1).
//
// CELL 1 (cap_enforced): flood the ingress with continuation frames whose
// AGGREGATE exceeds 64 MiB (a byte cap -- many tiny frames would stay UNDER it
// and correctly NOT trip). GREEN: bytes were tracked, the cap fired (queue did
// NOT retain the whole flood), the connection is flagged closing, and a 1009
// CLOSE frame was emitted to the client.
// CELL 2 (control): a legit multi-frame message UNDER the cap still assembles
// and delivers, and does not trip the cap -- proves no false-positive.
//
// SUBJECT, NOT MODIFIED. #includes server.c so ws_svr_process_frame is the
// isolated copy. Bounded (fixed sizes, watchdog); drains + frees everything.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"
#include "websocket.h"

#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>

#include "../server.c"

static const int WATCHDOG_SECONDS = 60;

// Wire a vws_cnx to the server-side frame processor with a minimal server so the
// breach path's close-send (vws_svr_data_new + vws_tcp_svr_send) has a real
// responses queue to land in.
static vws_cnx* make_server_cnx(vws_tcp_svr* s, vws_svr_cnx* svr_cnx)
{
    memset(svr_cnx, 0, sizeof(*svr_cnx));
    svr_cnx->server = s;
    vws_cid_clear(&svr_cnx->cid);

    vws_cnx* c   = vws_cnx_new();
    c->process   = ws_svr_process_frame;
    c->data      = (char*)svr_cnx;
    return c;
}

static void feed(vws_cnx* c, const unsigned char* payload, size_t size,
                 unsigned char opcode, int fin)
{
    vws_frame* f = vws_frame_new(payload, size, opcode);
    f->fin       = fin;
    ws_svr_process_frame(c, f);
}

// [vws SV2] The breach path now sends its 1009 TOO_BIG close reply through
// cnx->server->on_data_out (the loop-thread direct-write seam) instead of the
// blocking responses queue_push it used before. This test drives
// ws_svr_process_frame with a SYNTHETIC cnx that has no cid in the address pool,
// so the real on_data_out (svr_client_data_out) would just free the reply on its
// "connection no longer exists" arm and the reply would be unobservable. Install
// a capture hook in its place: it takes ownership of the data exactly as the real
// on_data_out does (copy the bytes we assert on, then free the block).
static ucstr  captured_data  = NULL;
static size_t captured_size  = 0;
static int    captured_count = 0;

static void capture_data_out(vws_svr_data* data, void* x)
{
    (void)x;
    captured_count++;
    if (captured_data == NULL && data->size > 0)
    {
        captured_data = (ucstr)vws.malloc(data->size);
        memcpy(captured_data, data->data, data->size);
        captured_size = data->size;
    }
    vws_svr_data_free(data);
}

CTEST(svr_msgflood, cap_enforced)
{
    alarm(WATCHDOG_SECONDS);

    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    ASSERT_TRUE(s != NULL);
    vws_svr_cnx svr_cnx;
    vws_cnx* c = make_server_cnx(s, &svr_cnx);

    // Fresh connection defaults to the 64 MiB message cap (proves the default).
    ASSERT_TRUE(c->max_message_size == VWS_MAX_MESSAGE_SIZE);

    // [vws SV2] Observe the breach path's 1009 reply at the on_data_out seam.
    captured_data  = NULL;
    captured_size  = 0;
    captured_count = 0;
    s->on_data_out = capture_data_out;

    // Flood: 65 x 1 MiB continuation frames (fin=0) -> aggregate 65 MiB > 64 MiB.
    // A byte cap, so this is what trips it -- not frame count.
    const size_t chunk = 1024u * 1024u;   // 1 MiB
    unsigned char* buf = (unsigned char*)vws.malloc(chunk);
    memset(buf, 0xCD, chunk);
    for (int i = 0; i < 65; i++)
    {
        feed(c, buf, chunk, CONTINUATION_FRAME, 0);
    }
    vws.free(buf);

    // GREEN: bytes tracked + cap fired + connection closing + queue bounded.
    ASSERT_TRUE((c->flags & CNX_CLOSING) != 0);        // connection closed
    ASSERT_EQUAL(0, (int)c->msg_bytes);                // reset on breach
    ASSERT_TRUE((int)sc_queue_size(&c->queue) < 65);   // did NOT retain the flood

    // 1009 CLOSE emitted to the client via on_data_out (SV2: direct-write seam,
    // not the responses queue). The capture hook holds its serialized bytes.
    ASSERT_TRUE(captured_count >= 1);
    ASSERT_TRUE(captured_data != NULL);
    vws_frame close_f;
    size_t consumed = 0;
    fs_t st = vws_deserialize(captured_data, captured_size, &close_f, &consumed);
    ASSERT_EQUAL(FRAME_COMPLETE, st);
    ASSERT_EQUAL(CLOSE_FRAME, close_f.opcode);
    ASSERT_TRUE(close_f.size >= 2);
    int16_t code = (int16_t)ntohs(*(uint16_t*)close_f.data);
    ASSERT_EQUAL(WS_CLOSE_TOO_BIG, code);              // 1009 Message Too Big
    vws.free(close_f.data);
    vws.free(captured_data);
    captured_data = NULL;

    // Teardown: drain retained frames + free cnx + server.
    vws_frame* f;
    while (sc_queue_size(&c->queue) > 0)
    {
        f = sc_queue_del_last(&c->queue);
        vws_frame_free(f);
    }
    vws_cnx_free(c);
    vws_tcp_svr_free(s);

    alarm(0);
}

CTEST(svr_msgflood, under_cap_control)
{
    alarm(WATCHDOG_SECONDS);

    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    ASSERT_TRUE(s != NULL);
    vws_svr_cnx svr_cnx;
    vws_cnx* c = make_server_cnx(s, &svr_cnx);

    // A legit fragmented message well under the cap: 3 small frames, final fin=1.
    feed(c, (const unsigned char*)"AAA", 3, TEXT_FRAME,         0);
    feed(c, (const unsigned char*)"BBB", 3, CONTINUATION_FRAME, 0);
    feed(c, (const unsigned char*)"CCC", 3, CONTINUATION_FRAME, 1);

    // No false-positive: connection NOT closed, aggregate reset after the fin=1
    // completion, no close emitted.
    ASSERT_TRUE((c->flags & CNX_CLOSING) == 0);
    ASSERT_EQUAL(0, (int)c->msg_bytes);
    ASSERT_EQUAL(0, (int)s->responses.size);

    // Delivers: the complete message assembles (9 bytes across 3 frames).
    vws_msg* m = vws_msg_pop(c);
    ASSERT_TRUE(m != NULL);
    ASSERT_EQUAL(9, (int)m->data->size);
    vws_msg_free(m);

    vws_cnx_free(c);
    vws_tcp_svr_free(s);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
