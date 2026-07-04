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

CTEST(svr_msgflood, cap_enforced)
{
    alarm(WATCHDOG_SECONDS);

    vws_tcp_svr* s = vws_tcp_svr_new(2, 0, 0);
    ASSERT_TRUE(s != NULL);
    vws_svr_cnx svr_cnx;
    vws_cnx* c = make_server_cnx(s, &svr_cnx);

    // Fresh connection defaults to the 64 MiB message cap (proves the default).
    ASSERT_TRUE(c->max_message_size == VWS_MAX_MESSAGE_SIZE);

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

    // 1009 CLOSE emitted to the client (landed on the responses queue).
    ASSERT_TRUE(s->responses.size >= 1);
    vws_svr_data* resp = queue_pop(&s->responses);
    ASSERT_TRUE(resp != NULL);
    vws_frame close_f;
    size_t consumed = 0;
    fs_t st = vws_deserialize((ucstr)resp->data, resp->size, &close_f, &consumed);
    ASSERT_EQUAL(FRAME_COMPLETE, st);
    ASSERT_EQUAL(CLOSE_FRAME, close_f.opcode);
    ASSERT_TRUE(close_f.size >= 2);
    int16_t code = (int16_t)ntohs(*(uint16_t*)close_f.data);
    ASSERT_EQUAL(WS_CLOSE_TOO_BIG, code);              // 1009 Message Too Big
    vws.free(close_f.data);
    vws_svr_data_free(resp);

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
