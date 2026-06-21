// test_ws_frame.c — websocket.c NON-SSL coverage (item-4 BATCH-1, module 4).
//
// The non-transport surface of websocket.c: frame serialize/deserialize +
// framing length encodings + masking, frame/message new/free, the opcode
// dispatch (process_frame), message assembly (vws_msg_pop /
// has_complete_message), the control-frame generators, and the handshake
// key/accept helpers. The SSL/socket transport paths (cnx_connect,
// socket_handshake, vws_cnx_ingress, the send wrappers) are OUT OF SCOPE
// (covered by test_socket_ssl / the live ws tests).
//
// #includes websocket.c (reaches the statics + isolates coverage) + links clean
// libvws for the socket/sc_queue/base64/SHA1 deps (the #included symbols shadow
// websocket.o -> no dup, libs non-instrumented). One-shot ld --wrap on
// RAND_bytes (masking + key gen) and vws_socket_write (the CLOSE/PING responses
// in process_frame) where a socket is otherwise required.
//
// SUBJECT, not modified: a real bug a cell surfaces is ESCALATED, not patched.
// SELF-BOUNDING: a wall-clock _Exit watchdog.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "websocket.h"

#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

//------------------------------------------------------------------------------
// One-shot fault injection
//------------------------------------------------------------------------------

extern int __real_RAND_bytes(unsigned char* buf, int num);
static int g_rand_fail = 0;

int __wrap_RAND_bytes(unsigned char* buf, int num)
{
    if (g_rand_fail)
    {
        g_rand_fail = 0;
        return 0;   // failure (!= 1)
    }
    return __real_RAND_bytes(buf, num);
}

// process_frame sends CLOSE/PING responses via vws_socket_write -> wrap it so
// no live socket is needed (echo the size).
static int g_disc_after_write = 0;
ssize_t __wrap_vws_socket_write(vws_socket* s, ucstr data, size_t size)
{
    (void)data;
    if (g_disc_after_write) { g_disc_after_write = 0; s->sockfd = -1; }
    return (ssize_t)size;
}

// websocket.c is the SUBJECT (statics + isolated coverage).
#include "../websocket.c"

static const int WATCHDOG_SECONDS = 60;

//------------------------------------------------------------------------------
// frame new / free
//------------------------------------------------------------------------------

CTEST(ws, frame_new_free)
{
    vws_frame* f = vws_frame_new((ucstr)"hello", 5, TEXT_FRAME);
    ASSERT_NOT_NULL(f);
    ASSERT_EQUAL(TEXT_FRAME, f->opcode);
    ASSERT_EQUAL(5, (int)f->size);
    vws_frame_free(f);

    vws_frame* z = vws_frame_new(NULL, 0, TEXT_FRAME);   // size 0 -> no data
    ASSERT_NOT_NULL(z);
    ASSERT_NULL(z->data);
    vws_frame_free(z);

    vws_frame_free(NULL);   // NULL guard
}

CTEST(ws, f_c_ws_1_send_not_connected)
{
    // C-WS-1: vws_frame_send OWNS `frame` (the connected path frees it via
    // vws_serialize). A fresh cnx is not connected, so the early-return path is
    // taken -- which leaked the frame pre-fix. Parent valgrind: RED (definitely
    // lost) pre-fix, GREEN after the vws_frame_free. Also pins the -1 contract.
    vws_cnx* c = vws_cnx_new();
    ASSERT_NOT_NULL(c);
    ASSERT_TRUE(vws_cnx_is_connected(c) == false);

    vws_frame* f = vws_frame_new((ucstr)"hi", 2, TEXT_FRAME);
    ssize_t r = vws_frame_send(c, f);
    ASSERT_TRUE(r == -1);

    vws_cnx_free(c);
}

//------------------------------------------------------------------------------
// serialize / deserialize round-trips at every length encoding + mask mode
//------------------------------------------------------------------------------

// Serialize a frame then deserialize it back, asserting the payload survives.
static void roundtrip(size_t len, int server_mode)
{
    unsigned char* payload = (unsigned char*)malloc(len ? len : 1);
    for (size_t i = 0; i < len; i++) payload[i] = (unsigned char)('a' + (i % 26));

    vws_frame* f = vws_frame_new((ucstr)payload, len, BINARY_FRAME);
    if (server_mode) f->mask = 0;   // server frames are unmasked

    vws_buffer* b = vws_serialize(f);   // frees f
    ASSERT_NOT_NULL(b);

    vws_frame g;
    memset(&g, 0, sizeof(g));
    size_t consumed = 0;
    fs_t rc = vws_deserialize(b->data, b->size, &g, &consumed);
    ASSERT_EQUAL(FRAME_COMPLETE, rc);
    ASSERT_EQUAL((int)len, (int)g.size);
    if (len > 0) ASSERT_TRUE(memcmp(g.data, payload, len) == 0);
    ASSERT_EQUAL((int)b->size, (int)consumed);

    if (g.data) vws.free(g.data);
    vws_buffer_free(b);
    free(payload);
}

CTEST(ws, serialize_roundtrip_small)   { roundtrip(5, 0); }      // <=125, masked
CTEST(ws, serialize_roundtrip_125)     { roundtrip(125, 0); }    // boundary
CTEST(ws, serialize_roundtrip_126)     { roundtrip(200, 0); }    // 2-byte length
CTEST(ws, serialize_roundtrip_65535)   { roundtrip(65535, 0); }  // 2-byte boundary
CTEST(ws, serialize_roundtrip_65536)   { roundtrip(65536, 0); }  // 8-byte length
CTEST(ws, serialize_roundtrip_unmasked){ roundtrip(50, 1); }     // server mode

CTEST(ws, serialize_null)
{
    ASSERT_NULL(vws_serialize(NULL));   // empty frame -> NULL
}

CTEST(ws, serialize_rand_fail)
{
    vws_frame* f = vws_frame_new((ucstr)"x", 1, TEXT_FRAME);   // masked
    g_rand_fail = 1;
    vws_buffer* b = vws_serialize(f);   // RAND_bytes fails -> NULL (frees f)
    g_rand_fail = 0;
    ASSERT_NULL(b);
}

//------------------------------------------------------------------------------
// deserialize incomplete frames
//------------------------------------------------------------------------------

CTEST(ws, deserialize_incomplete)
{
    vws_frame g;
    size_t consumed = 0;

    // < 2 header bytes
    unsigned char a[] = { 0x81 };
    ASSERT_EQUAL(FRAME_INCOMPLETE, vws_deserialize(a, 1, &g, &consumed));

    // length 126 but extended-length bytes truncated
    unsigned char b[] = { 0x82, 0x7e, 0x00 };
    ASSERT_EQUAL(FRAME_INCOMPLETE, vws_deserialize(b, 3, &g, &consumed));

    // header complete, payload truncated (unmasked, says 5, gives 2)
    unsigned char c[] = { 0x82, 0x05, 'a', 'b' };
    ASSERT_EQUAL(FRAME_INCOMPLETE, vws_deserialize(c, 4, &g, &consumed));

    // masked, masking key + payload truncated
    unsigned char d[] = { 0x82, 0x83, 0x00, 0x00 };
    ASSERT_EQUAL(FRAME_INCOMPLETE, vws_deserialize(d, 4, &g, &consumed));
}

CTEST(ws, f_c_ws_2_overflow)
{
    // C-WS-2 (wire-reachable HIGH): a peer 8-byte length of 2^64-1 overflows
    // required_bytes, bypassing the incomplete-guard -> vws.malloc(~2^64) ->
    // OOB/NULL-deref. Fork-repro: RED on current (child SIGSEGV); GREEN after
    // (FRAME_ERROR, no malloc, child exits 0). Masked + unmasked.
    unsigned char um[] = {
        0x82, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    unsigned char mk[] = {
        0x82, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xAA, 0xBB, 0xCC, 0xDD };
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        vws_frame f;
        size_t c;
        memset(&f, 0, sizeof(f));
        if (vws_deserialize(um, sizeof(um), &f, &c) != FRAME_ERROR) _exit(1);
        memset(&f, 0, sizeof(f));
        if (vws_deserialize(mk, sizeof(mk), &f, &c) != FRAME_ERROR) _exit(2);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));         // RED: SIGSEGV on current
    ASSERT_EQUAL(0, WEXITSTATUS(status));   // both returned FRAME_ERROR
}

CTEST(ws, deserialize_length_bounds)
{
    vws_frame f;
    size_t c;
    fs_t rc;

    // Over the 64 MiB cap (0x04000001) -> FRAME_ERROR (malformed). RED on
    // current (INCOMPLETE there, no crash) -> GREEN. No fork needed.
    unsigned char over[] = { 0x82, 0x7F, 0,0,0,0, 0x04,0,0,0x01 };
    memset(&f, 0, sizeof(f));
    rc = vws_deserialize(over, sizeof(over), &f, &c);
    ASSERT_EQUAL(FRAME_ERROR, rc);

    // At the cap (0x04000000 = 64 MiB) with data absent -> FRAME_INCOMPLETE
    // (the cap must NOT reject a legit large frame still being buffered).
    unsigned char atcap[] = { 0x82, 0x7F, 0,0,0,0, 0x04,0,0,0 };
    memset(&f, 0, sizeof(f));
    rc = vws_deserialize(atcap, sizeof(atcap), &f, &c);
    ASSERT_EQUAL(FRAME_INCOMPLETE, rc);

    // A normal small valid frame still parses (regression).
    unsigned char ok[] = { 0x82, 0x02, 'h', 'i' };
    memset(&f, 0, sizeof(f));
    rc = vws_deserialize(ok, sizeof(ok), &f, &c);
    ASSERT_EQUAL(FRAME_COMPLETE, rc);
    if (f.data) vws.free(f.data);
}

//------------------------------------------------------------------------------
// process_frame opcode dispatch (queue ops + wrapped socket responses)
//------------------------------------------------------------------------------

CTEST(ws, process_frame_data)
{
    vws_cnx* c = vws_cnx_new();

    // TEXT/BINARY/CONTINUATION are queued.
    process_frame(c, vws_frame_new((ucstr)"a", 1, TEXT_FRAME));
    ASSERT_EQUAL(1, (int)sc_queue_size(&c->queue));
    process_frame(c, vws_frame_new((ucstr)"b", 1, BINARY_FRAME));
    process_frame(c, vws_frame_new((ucstr)"c", 1, CONTINUATION_FRAME));
    ASSERT_EQUAL(3, (int)sc_queue_size(&c->queue));

    // PONG is discarded (freed, not queued).
    process_frame(c, vws_frame_new((ucstr)"p", 1, PONG_FRAME));
    ASSERT_EQUAL(3, (int)sc_queue_size(&c->queue));

    // An invalid/reserved opcode hits the default (freed).
    process_frame(c, vws_frame_new((ucstr)"x", 1, 0x03));
    ASSERT_EQUAL(3, (int)sc_queue_size(&c->queue));

    vws_cnx_free(c);
}

CTEST(ws, process_frame_control)
{
    vws_cnx* c = vws_cnx_new();
    // CLOSE + PING send a response via the wrapped vws_socket_write.
    process_frame(c, vws_frame_new(NULL, 0, CLOSE_FRAME));
    ASSERT_TRUE(vws_is_flag(&c->flags, CNX_CLOSING));
    process_frame(c, vws_frame_new((ucstr)"pingdata", 8, PING_FRAME));
    vws_cnx_free(c);
}

//------------------------------------------------------------------------------
// message assembly: has_complete_message + vws_msg_pop (single + fragmented)
//------------------------------------------------------------------------------

CTEST(ws, msg_pop_single)
{
    vws_cnx* c = vws_cnx_new();
    ASSERT_FALSE(has_complete_message(c));     // empty
    ASSERT_NULL(vws_msg_pop(c));

    vws_frame* f = vws_frame_new((ucstr)"hello", 5, TEXT_FRAME);  // fin=1
    process_frame(c, f);
    ASSERT_TRUE(has_complete_message(c));
    vws_msg* m = vws_msg_pop(c);
    ASSERT_NOT_NULL(m);
    ASSERT_EQUAL(TEXT_FRAME, m->opcode);
    ASSERT_EQUAL(5, (int)m->data->size);
    vws_msg_free(m);
    vws_cnx_free(c);
}

CTEST(ws, msg_pop_fragmented)
{
    vws_cnx* c = vws_cnx_new();

    // TEXT(fin=0) + CONTINUATION(fin=0) + CONTINUATION(fin=1) -> one message.
    vws_frame* f1 = vws_frame_new((ucstr)"aa", 2, TEXT_FRAME);
    f1->fin = 0;
    vws_frame* f2 = vws_frame_new((ucstr)"bb", 2, CONTINUATION_FRAME);
    f2->fin = 0;
    vws_frame* f3 = vws_frame_new((ucstr)"cc", 2, CONTINUATION_FRAME);
    f3->fin = 1;
    process_frame(c, f1);
    process_frame(c, f2);
    process_frame(c, f3);

    ASSERT_TRUE(has_complete_message(c));
    vws_msg* m = vws_msg_pop(c);
    ASSERT_NOT_NULL(m);
    ASSERT_EQUAL(TEXT_FRAME, m->opcode);    // opcode from the first frame
    ASSERT_EQUAL(6, (int)m->data->size);    // aa+bb+cc
    vws_msg_free(m);
    vws_cnx_free(c);
}

//------------------------------------------------------------------------------
// control-frame generators + handshake key helpers
//------------------------------------------------------------------------------

CTEST(ws, generators)
{
    vws_buffer* close = vws_generate_close_frame();
    ASSERT_NOT_NULL(close);
    ASSERT_TRUE(close->size > 0);
    vws_buffer_free(close);

    vws_buffer* pong = vws_generate_pong_frame((ucstr)"ping", 4);
    ASSERT_NOT_NULL(pong);
    ASSERT_TRUE(pong->size > 0);
    vws_buffer_free(pong);
}

CTEST(ws, accept_key_verify)
{
    // RFC 6455 example: key dGhlIHNhbXBsZSBub25jZQ== -> accept
    // s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
    cstr key = "dGhlIHNhbXBsZSBub25jZQ==";
    cstr expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

    cstr accept = vws_accept_key(key);
    ASSERT_NOT_NULL(accept);
    ASSERT_STR(expected, accept);
    vws.free((void*)accept);

    ASSERT_TRUE(verify_handshake(key, expected));        // match
    ASSERT_FALSE(verify_handshake(key, "wrong"));         // mismatch
}

CTEST(ws, generate_key)
{
    char* k = generate_websocket_key();
    ASSERT_NOT_NULL(k);
    vws.free(k);

    g_rand_fail = 1;
    char* fail = generate_websocket_key();   // RAND_bytes fails -> NULL
    g_rand_fail = 0;
    ASSERT_NULL(fail);
}

CTEST(ws, send_wrappers)
{
    // The send wrappers serialize + write; fake "connected" (sockfd>=0) so the
    // path proceeds, and the wrapped vws_socket_write echoes the size (the real
    // socket write is transport, out of scope here).
    vws_cnx* c = vws_cnx_new();
    ((vws_socket*)c)->sockfd = 3;
    ASSERT_TRUE(vws_frame_send_text(c, "hi") > 0);
    ASSERT_TRUE(vws_frame_send_binary(c, (ucstr)"x", 1) > 0);
    ASSERT_TRUE(vws_msg_send_text(c, "hi") > 0);
    ASSERT_TRUE(vws_msg_send_binary(c, (ucstr)"x", 1) > 0);
    ASSERT_TRUE(vws_msg_send_data(c, (ucstr)"x", 1, TEXT_FRAME) > 0);
    ((vws_socket*)c)->sockfd = -1;   // restore so free() does not close fd 3
    vws_cnx_free(c);
}

CTEST(ws, dump_frame_extended)
{
    // 126 (2-byte) length branch.
    unsigned char* p = (unsigned char*)malloc(300);
    memset(p, 'x', 300);
    vws_frame* f = vws_frame_new((ucstr)p, 300, BINARY_FRAME);
    f->mask = 0;
    vws_buffer* b = vws_serialize(f);
    vws_dump_websocket_frame(b->data, b->size);
    vws_buffer_free(b);
    free(p);

    // 127 (8-byte) length branch.
    unsigned char* q = (unsigned char*)malloc(65536);
    memset(q, 'y', 65536);
    vws_frame* g = vws_frame_new((ucstr)q, 65536, BINARY_FRAME);
    g->mask = 0;
    vws_buffer* b2 = vws_serialize(g);
    vws_dump_websocket_frame(b2->data, b2->size);
    vws_buffer_free(b2);
    free(q);

    // truncated 126 (size<4) + truncated 127 (size<10) "Invalid" branches.
    unsigned char t126[] = { 0x82, 0x7e, 0x00 };
    vws_dump_websocket_frame(t126, 3);
    unsigned char t127[] = { 0x82, 0x7f, 0x00 };
    vws_dump_websocket_frame(t127, 3);

    // a MASKED frame (mask bit set) -> the masking-key dump branch.
    vws_frame* mf = vws_frame_new((ucstr)"hi", 2, TEXT_FRAME);
    vws_buffer* mb = vws_serialize(mf);
    vws_dump_websocket_frame(mb->data, mb->size);
    vws_buffer_free(mb);

    // masked header but truncated masking key (size < header+4) -> Invalid.
    unsigned char tmask[] = { 0x82, 0x82, 0x00 };
    vws_dump_websocket_frame(tmask, 3);
}

// NOTE: the vws_frame_send not-connected early-return (websocket.c:634-636) is
// NOT covered -- taking it LEAKS the frame vws_frame_new just allocated (the
// early return does not free it), candidate C-WS-1 (LOCAL send-side, batch); a
// clean cell cannot exercise it. Left as residual.
CTEST(ws, send_server_mode_and_trace)
{
    vws_cnx* c = vws_cnx_new();
    ((vws_socket*)c)->sockfd = 3;
    vws_cnx_set_server_mode(c);          // :640-643 server unmask
    int saved = vws.tracelevel;
    vws.tracelevel = VT_PROTOCOL;        // :648-658 trace-dump path
    ASSERT_TRUE(vws_frame_send_text(c, "hi") > 0);
    vws.tracelevel = saved;
    ((vws_socket*)c)->sockfd = -1;
    vws_cnx_free(c);
}

CTEST(ws, send_disconnect_after_write)
{
    vws_cnx* c = vws_cnx_new();
    ((vws_socket*)c)->sockfd = 3;
    g_disc_after_write = 1;
    ASSERT_EQUAL(-1, (int)vws_frame_send_text(c, "x"));   // :668-670
    vws_cnx_free(c);
}

//------------------------------------------------------------------------------
// cnx new/free + server mode + dump
//------------------------------------------------------------------------------

CTEST(ws, cnx_lifecycle)
{
    vws_cnx* c = vws_cnx_new();
    ASSERT_NOT_NULL(c);
    ASSERT_FALSE(vws_cnx_is_connected(c));
    vws_cnx_set_server_mode(c);
    ASSERT_TRUE(vws_is_flag(&c->flags, CNX_SERVER));
    vws_cnx_free(c);
}

CTEST(ws, dump_frame)
{
    // Build a real serialized frame + dump it (exercises the parse + print).
    vws_frame* f = vws_frame_new((ucstr)"hi", 2, TEXT_FRAME);
    f->mask = 0;
    vws_buffer* b = vws_serialize(f);
    vws_dump_websocket_frame(b->data, b->size);
    vws_buffer_free(b);

    unsigned char tiny[] = { 0x81 };
    vws_dump_websocket_frame(tiny, 1);   // < 2 -> "Invalid" branch
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_ws_frame: watchdog deadline exceeded — abort\n");
    _Exit(99);
    return NULL;
}

int main(int argc, const char* argv[])
{
    pthread_t watch;
    pthread_create(&watch, NULL, watchdog_thread, NULL);
    pthread_detach(watch);

    return ctest_main(argc, argv);
}
