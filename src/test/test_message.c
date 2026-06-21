// test_message.c — message.c coverage (item-4 BATCH-1, module 3).
//
// vrtql_msg MessagePack/JSON serialize + deserialize. #includes message.c
// (reaches the static mpack parsers + isolates coverage) + links clean libvws
// for the mpack/yyjson/buffer/kvs/vws deps (the #included message symbols shadow
// message.o, never pulled -> no dup, libs non-instrumented). Error arms are
// driven by hand-crafted MessagePack/JSON bytes where the input can force them,
// and by one-shot ld --wrap (mpack_writer_destroy / mpack_reader_destroy /
// yyjson_mut_write / vws_frame_send_binary / vws_msg_recv) where it cannot.
//
// SUBJECT, not modified: a real bug a cell surfaces is ESCALATED, not patched.
// SELF-BOUNDING: a wall-clock _Exit watchdog.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "message.h"
#include "mpack-writer.h"
#include "mpack-reader.h"
#include "util/yyjson.h"

#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

//------------------------------------------------------------------------------
// One-shot fault injection
//------------------------------------------------------------------------------

extern mpack_error_t __real_mpack_writer_destroy(mpack_writer_t* w);
extern mpack_error_t __real_mpack_reader_destroy(mpack_reader_t* r);
extern uint32_t __real_mpack_expect_array(mpack_reader_t* reader);

static int g_writer_destroy_fail = 0;
static int g_expect_array_fake   = 0;
static int g_reader_destroy_fail = 0;
static vws_msg* g_recv_msg       = NULL;

mpack_error_t __wrap_mpack_writer_destroy(mpack_writer_t* w)
{
    mpack_error_t r = __real_mpack_writer_destroy(w);
    if (g_writer_destroy_fail)
    {
        g_writer_destroy_fail = 0;
        return mpack_error_data;
    }
    return r;
}

mpack_error_t __wrap_mpack_reader_destroy(mpack_reader_t* r)
{
    mpack_error_t rc = __real_mpack_reader_destroy(r);
    if (g_reader_destroy_fail)
    {
        g_reader_destroy_fail = 0;
        return mpack_error_data;
    }
    return rc;
}

ssize_t __wrap_vws_frame_send_binary(vws_cnx* c, ucstr data, size_t size)
{
    (void)c; (void)data;
    return (ssize_t)size;   // pretend the frame was sent
}

vws_msg* __wrap_vws_msg_recv(vws_cnx* c)
{
    (void)c;
    vws_msg* m = g_recv_msg;
    g_recv_msg = NULL;
    return m;
}

uint32_t __wrap_mpack_expect_array(mpack_reader_t* reader)
{
    if (g_expect_array_fake)
    {
        g_expect_array_fake = 0;
        return 2;
    }
    return __real_mpack_expect_array(reader);
}

// message.c is the SUBJECT (statics + isolated coverage).
#include "../message.c"

static const int WATCHDOG_SECONDS = 60;

// vrtql_msg content is a binary buffer (NOT null-terminated); compare with the
// known length rather than ASSERT_STR (which would strcmp past the buffer).
#define ASSERT_CONTENT(m, lit) do {                                          \
    ASSERT_EQUAL((int)strlen(lit), (int)vrtql_msg_get_content_size(m));      \
    ASSERT_TRUE(memcmp(vrtql_msg_get_content(m), lit, strlen(lit)) == 0);    \
} while (0)

//------------------------------------------------------------------------------
// new / free / clear / copy
//------------------------------------------------------------------------------

CTEST(message, new_free)
{
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_NOT_NULL(m);
    ASSERT_TRUE(vws_is_flag(&m->flags, VM_MSG_VALID));
    vrtql_msg_free(m);
    vrtql_msg_free(NULL);   // NULL guard (104-107)
}

CTEST(message, copy)
{
    ASSERT_NULL(vrtql_msg_copy(NULL));   // NULL guard (64-67)

    vrtql_msg* m = vrtql_msg_new();
    vrtql_msg_set_routing(m, "to", "svc");
    vrtql_msg_set_header(m, "k", "v");
    vrtql_msg_set_content(m, "body");

    vrtql_msg* c = vrtql_msg_copy(m);
    ASSERT_NOT_NULL(c);
    ASSERT_STR("svc", vrtql_msg_get_routing(c, "to"));
    ASSERT_STR("v", vrtql_msg_get_header(c, "k"));
    ASSERT_CONTENT(c, "body");

    vrtql_msg_free(m);
    vrtql_msg_free(c);
}

CTEST(message, clear)
{
    vrtql_msg* m = vrtql_msg_new();
    vrtql_msg_set_routing(m, "to", "svc");
    vrtql_msg_set_header(m, "k", "v");
    vrtql_msg_set_content(m, "body");
    vrtql_msg_clear(m);
    ASSERT_TRUE(vrtql_msg_is_empty(m));
    vrtql_msg_free(m);
}

//------------------------------------------------------------------------------
// serialize / deserialize round-trips
//------------------------------------------------------------------------------

CTEST(message, roundtrip_mpack)
{
    vrtql_msg* m = vrtql_msg_new();   // default VM_MPACK_FORMAT
    vrtql_msg_set_routing(m, "to", "svc");
    vrtql_msg_set_header(m, "type", "req");
    vrtql_msg_set_content(m, "payload");

    vws_buffer* b = vrtql_msg_serialize(m);
    ASSERT_NOT_NULL(b);

    vrtql_msg* d = vrtql_msg_new();
    ASSERT_TRUE(vrtql_msg_deserialize(d, b->data, b->size));
    ASSERT_STR("svc", vrtql_msg_get_routing(d, "to"));
    ASSERT_STR("req", vrtql_msg_get_header(d, "type"));
    ASSERT_CONTENT(d, "payload");
    ASSERT_EQUAL(VM_MPACK_FORMAT, d->format);

    vws_buffer_free(b);
    vrtql_msg_free(m);
    vrtql_msg_free(d);
}

CTEST(message, roundtrip_json)
{
    vrtql_msg* m = vrtql_msg_new();
    m->format = VM_JSON_FORMAT;
    // C-MSG-2: the JSON headers loop iterates routing->used + writes to the
    // routing object. Keep routing->used == headers->used (1 each) so the loop
    // does NOT read past headers->array (no OOB); the serialized headers are
    // still mis-placed (escalated), so only routing + content round-trip.
    vrtql_msg_set_routing(m, "to", "svc");
    vrtql_msg_set_header(m, "type", "req");
    vrtql_msg_set_content(m, "payload");

    vws_buffer* b = vrtql_msg_serialize(m);
    ASSERT_NOT_NULL(b);
    ASSERT_TRUE(b->size > 0);

    vrtql_msg* d = vrtql_msg_new();
    ASSERT_TRUE(vrtql_msg_deserialize(d, b->data, b->size));
    ASSERT_STR("svc", vrtql_msg_get_routing(d, "to"));
    ASSERT_CONTENT(d, "payload");
    ASSERT_EQUAL(VM_JSON_FORMAT, d->format);

    vws_buffer_free(b);
    vrtql_msg_free(m);
    vrtql_msg_free(d);
}

//------------------------------------------------------------------------------
// serialize error / edge arms
//------------------------------------------------------------------------------

CTEST(message, serialize_null)
{
    // C-MSG-1 FIXED: now returns NULL (was `false`) from the vws_buffer*-returning
    // function. Behavior-identical (false == NULL == 0); type-hygiene. Contract pinned.
    ASSERT_NULL(vrtql_msg_serialize(NULL));   // lines 120-122
}

CTEST(message, serialize_bad_format)
{
    vrtql_msg* m = vrtql_msg_new();
    m->format = 99;   // neither MPACK nor JSON -> fallthrough NULL (line 253)
    ASSERT_NULL(vrtql_msg_serialize(m));
    vrtql_msg_free(m);
}

CTEST(message, serialize_writer_destroy_fail)
{
    vrtql_msg* m = vrtql_msg_new();
    vrtql_msg_set_content(m, "x");
    g_writer_destroy_fail = 1;
    vws_buffer* b = vrtql_msg_serialize(m);   // lines 175-184 -> NULL
    g_writer_destroy_fail = 0;
    ASSERT_NULL(b);
    vrtql_msg_free(m);
}

//------------------------------------------------------------------------------
// deserialize error arms — hand-crafted MessagePack
//------------------------------------------------------------------------------

CTEST(message, deserialize_null)
{
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, NULL, 5));        // data NULL
    ASSERT_FALSE(vrtql_msg_deserialize(m, (ucstr)"x", 0));  // length 0
    vrtql_msg_free(m);
}

CTEST(message, mpack_routing_not_map)
{
    // 0x93 array3 ; 0xa1 'x' = routing is a str (not a map) -> msg_parse_map
    // returns false (644-646) -> deserialize false (303-306).
    unsigned char d[] = { 0x93, 0xa1, 'x', 0x80, 0xc4, 0x00 };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));
    vrtql_msg_free(m);
}

CTEST(message, mpack_headers_not_map)
{
    // routing empty map, headers a str -> 310-313.
    unsigned char d[] = { 0x93, 0x80, 0xa1, 'x', 0xc4, 0x00 };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));
    vrtql_msg_free(m);
}

CTEST(message, mpack_content_not_bin)
{
    // routing + headers empty maps, content an int -> msg_parse_content -1
    // (723-726) -> deserialize false (319-322).
    unsigned char d[] = { 0x93, 0x80, 0x80, 0x01 };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));
    vrtql_msg_free(m);
}

CTEST(message, mpack_key_not_str)
{
    // routing fixmap(1) with an int key -> msg_parse_map key-not-str (664-668).
    unsigned char d[] = { 0x93, 0x81, 0x01, 0xa1, 'v', 0x80, 0xc4, 0x00 };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));
    vrtql_msg_free(m);
}

// NOTE: the value-not-str branch (msg_parse_map :682-686) is NOT covered --
// taking it LEAKS the key malloc'd at :672 (the early return does not free it),
// candidate C-MSG-5; a clean cell cannot exercise it. Left as residual.
CTEST(message, mpack_content_len0)
{
    // content bin8 length 0 -> msg_parse_content returns 0 (730-733).
    unsigned char d[] = { 0x93, 0x80, 0x80, 0xc4, 0x00 };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_TRUE(vrtql_msg_deserialize(m, d, sizeof(d)));
    ASSERT_EQUAL(0, (int)vrtql_msg_get_content_size(m));
    vrtql_msg_free(m);
}

CTEST(message, mpack_reader_destroy_fail)
{
    unsigned char d[] = { 0x93, 0x80, 0x80, 0xc4, 0x00 };
    vrtql_msg* m = vrtql_msg_new();
    g_reader_destroy_fail = 1;
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));  // 335-343
    g_reader_destroy_fail = 0;
    vrtql_msg_free(m);
}

CTEST(message, json_deserialize_populated)
{
    // Hand-crafted JSON with NON-empty routing + headers objects exercises both
    // iteration bodies (the C-MSG-2 serialize bug leaves headers empty, so a
    // serialize round-trip can't reach the headers loop at :394-401).
    vrtql_msg* m = vrtql_msg_new();
    cstr j = "[{\"r\":\"1\"},{\"h\":\"2\"},\"content\"]";
    ASSERT_TRUE(vrtql_msg_deserialize(m, (ucstr)j, strlen(j)));
    ASSERT_STR("1", vrtql_msg_get_routing(m, "r"));
    ASSERT_STR("2", vrtql_msg_get_header(m, "h"));
    ASSERT_CONTENT(m, "content");
    vrtql_msg_free(m);
}

CTEST(message, mpack_routing_reader_error)
{
    // routing tag = a truncated map16 marker (needs 2 count bytes, EOF) ->
    // mpack_read_tag sets a reader error (msg_parse_map :639-641).
    unsigned char d[] = { 0x93, 0xde };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));
    vrtql_msg_free(m);
}

CTEST(message, mpack_content_reader_error)
{
    // empty routing + headers maps then EOF (no content tag) -> the content
    // read_tag sets a reader error (msg_parse_content :716-718).
    unsigned char d[] = { 0x93, 0x80, 0x80 };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));
    vrtql_msg_free(m);
}

//------------------------------------------------------------------------------
// deserialize error arms — JSON
//------------------------------------------------------------------------------

CTEST(message, json_garbage)
{
    // Not 0x93 + not valid JSON -> yyjson_read returns NULL -> root NULL ->
    // root-not-array error (356-362).
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, (ucstr)"not json {", 10));
    vrtql_msg_free(m);
}

CTEST(message, json_root_not_array)
{
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, (ucstr)"{}", 2));   // 356-362
    vrtql_msg_free(m);
}

CTEST(message, json_routing_not_obj)
{
    vrtql_msg* m = vrtql_msg_new();
    cstr j = "[\"x\",{},\"c\"]";   // routing is a string -> 384-390
    ASSERT_FALSE(vrtql_msg_deserialize(m, (ucstr)j, strlen(j)));
    vrtql_msg_free(m);
}

CTEST(message, json_headers_not_obj)
{
    vrtql_msg* m = vrtql_msg_new();
    cstr j = "[{},\"x\",\"c\"]";   // headers is a string -> 403-409
    ASSERT_FALSE(vrtql_msg_deserialize(m, (ucstr)j, strlen(j)));
    vrtql_msg_free(m);
}

CTEST(message, json_content_not_str)
{
    vrtql_msg* m = vrtql_msg_new();
    cstr j = "[{},{},5]";   // content is an int -> 411-421
    ASSERT_FALSE(vrtql_msg_deserialize(m, (ucstr)j, strlen(j)));
    vrtql_msg_free(m);
}

CTEST(message, f_c_msg_4_truncated_mpack)
{
    // C-MSG-4 (Mike-authorized HIGH, F-S2 model): a truncated MPACK str makes
    // mpack_read_bytes_inplace return NULL; msg_parse_map/_content then copy
    // from NULL -> SEGV/DoS on malformed input. Fork-repro over the key, value,
    // and content truncations: RED on current (child SIGSEGV), GREEN after the
    // NULL guards (each returns false/-1).
    unsigned char tkey[] = { 0x93, 0x81, 0xa5, 'k' };            // key str(5)
    unsigned char tval[] = { 0x93, 0x81, 0xa1, 'k', 0xa5, 'v' }; // value str(5)
    unsigned char tcon[] = { 0x93, 0x80, 0x80, 0xc4, 0x05, 'x' };
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
        vrtql_msg* a = vrtql_msg_new();
        vrtql_msg_deserialize(a, tkey, sizeof(tkey));
        vrtql_msg* b = vrtql_msg_new();
        vrtql_msg_deserialize(b, tval, sizeof(tval));
        vrtql_msg* c = vrtql_msg_new();
        vrtql_msg_deserialize(c, tcon, sizeof(tcon));
        _exit(0);   // reached only without a crash (all returned false/-1)
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));   // RED: SIGSEGV on current message.c
}

CTEST(message, f_c_msg_2_json_headers)
{
    // C-MSG-2 (wire-reachable): the JSON-serialize headers loop iterates
    // routing->used (:219) + writes the routing object (:222), so with more
    // routing than headers it reads past headers->array (OOB) + misplaces them.
    // Fork: RED on current (OOB crash / header lost); GREEN after.
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        vrtql_msg* m = vrtql_msg_new();
        m->format = VM_JSON_FORMAT;
        vrtql_msg_set_routing(m, "a", "1");
        vrtql_msg_set_routing(m, "b", "2");   // routing->used = 2
        vrtql_msg_set_header(m, "h", "v");     // headers->used = 1
        vrtql_msg_set_content(m, "body");      // 3-element array for round-trip
        vws_buffer* buf = vrtql_msg_serialize(m);
        vrtql_msg* d = vrtql_msg_new();
        vrtql_msg_deserialize(d, buf->data, buf->size);
        // Structural (RED on a NORMAL build, no ASan): the buggy routing->used
        // loop writes the header into the ROUTING object, so on the bug routing
        // gains the "h" key (used==3) and headers stays empty (used==0).
        if (d->routing->used != 2) _exit(3);   // buggy: 3 (h leaked to routing)
        if (d->headers->used != 1) _exit(4);   // buggy: 0 (headers empty)
        cstr h = vrtql_msg_get_header(d, "h");
        if (h == NULL || strcmp(h, "v") != 0) _exit(1);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));         // RED: OOB crash on current
    ASSERT_EQUAL(0, WEXITSTATUS(status));   // header recovered after the fix
}

CTEST(message, mpack_value_not_str)
{
    // routing fixmap(1) key "k" then int value -> value-not-str (682-686).
    // C-MSG-5: leaks the key on current; the fix frees it (valgrind-clean).
    unsigned char d[] = { 0x93, 0x81, 0xa1, 'k', 0x01, 0x80, 0xc4, 0x00 };
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));
    vrtql_msg_free(m);
}

CTEST(message, mpack_array_not_3)
{
    // :297 the array!=3 arm. The 0x93 magic byte IS a 3-array header, so
    // mpack_expect_array always returns 3 on the magic path; force a non-3
    // count via the one-shot wrap to reach the malformed-array error.
    unsigned char d[] = { 0x93, 0x80, 0x80, 0xc4, 0x00 };
    vrtql_msg* m = vrtql_msg_new();
    g_expect_array_fake = 1;
    ASSERT_FALSE(vrtql_msg_deserialize(m, d, sizeof(d)));   // 295-299
    g_expect_array_fake = 0;
    vrtql_msg_free(m);
}

//------------------------------------------------------------------------------
// is_empty / repr / dump / getters / send / recv
//------------------------------------------------------------------------------

CTEST(message, is_empty)
{
    vrtql_msg* m = vrtql_msg_new();
    ASSERT_TRUE(vrtql_msg_is_empty(m));        // all empty -> true
    vrtql_msg_set_content(m, "x");
    ASSERT_FALSE(vrtql_msg_is_empty(m));       // content>0 -> false (449)
    vrtql_msg_clear_content(m);
    vrtql_msg_set_routing(m, "a", "b");
    ASSERT_FALSE(vrtql_msg_is_empty(m));       // routing>0 -> false (434)
    vrtql_msg_clear_routings(m);
    vrtql_msg_set_header(m, "a", "b");
    // C-MSG-3: the surviving headers->used arm (the duplicate that sat behind it
    // was deleted in the fix). Behavior-identical; this pins no-regression.
    ASSERT_FALSE(vrtql_msg_is_empty(m));       // headers>0 -> false
    vrtql_msg_free(m);
}

CTEST(message, repr_dump)
{
    vrtql_msg* m = vrtql_msg_new();
    vrtql_msg_set_routing(m, "to", "svc");
    vrtql_msg_set_header(m, "k", "v");
    vrtql_msg_set_content(m, "body");   // content>0 branch (507-510)
    vws_buffer* r = vrtql_msg_repr(m);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(r->size > 0);
    vws_buffer_free(r);
    vrtql_msg_dump(m);                   // writes repr to stdout

    vrtql_msg* e = vrtql_msg_new();      // empty -> content<=0 skip
    vws_buffer* r2 = vrtql_msg_repr(e);
    ASSERT_NOT_NULL(r2);
    vws_buffer_free(r2);
    vrtql_msg_free(e);
    vrtql_msg_free(m);
}

CTEST(message, getters_setters)
{
    vrtql_msg* m = vrtql_msg_new();

    vrtql_msg_set_header(m, "h", "1");
    ASSERT_STR("1", vrtql_msg_get_header(m, "h"));
    vrtql_msg_clear_header(m, "h");
    ASSERT_NULL(vrtql_msg_get_header(m, "h"));
    vrtql_msg_set_header(m, "h2", "2");
    vrtql_msg_clear_headers(m);
    ASSERT_NULL(vrtql_msg_get_header(m, "h2"));

    vrtql_msg_set_routing(m, "r", "1");
    ASSERT_STR("1", vrtql_msg_get_routing(m, "r"));
    vrtql_msg_clear_routing(m, "r");
    ASSERT_NULL(vrtql_msg_get_routing(m, "r"));

    vrtql_msg_set_content_binary(m, "abc", 3);
    ASSERT_EQUAL(3, (int)vrtql_msg_get_content_size(m));

    vrtql_msg_free(m);
}

CTEST(message, send)
{
    // vrtql_msg_send serializes + vws_frame_send_binary (wrapped to echo size).
    vrtql_msg* m = vrtql_msg_new();
    vrtql_msg_set_content(m, "hello");
    ssize_t n = vrtql_msg_send(NULL, m);
    ASSERT_TRUE(n > 0);
    vrtql_msg_free(m);
}

CTEST(message, recv)
{
    // recv with no message -> NULL (606-608).
    g_recv_msg = NULL;
    ASSERT_NULL(vrtql_msg_recv(NULL));

    // recv a serialized message via the wrapped vws_msg_recv -> deserialized.
    vrtql_msg* src = vrtql_msg_new();
    vrtql_msg_set_routing(src, "to", "svc");
    vrtql_msg_set_content(src, "data");
    vws_buffer* b = vrtql_msg_serialize(src);

    vws_msg* wsm = vws_msg_new();
    vws_buffer_append(wsm->data, b->data, b->size);
    g_recv_msg = wsm;
    vrtql_msg* got = vrtql_msg_recv(NULL);
    ASSERT_NOT_NULL(got);
    ASSERT_STR("svc", vrtql_msg_get_routing(got, "to"));
    vrtql_msg_free(got);

    // recv with undecodable data -> deserialize fails -> NULL (615-621).
    vws_msg* bad = vws_msg_new();
    vws_buffer_append(bad->data, (ucstr)"not json {", 10);
    g_recv_msg = bad;
    ASSERT_NULL(vrtql_msg_recv(NULL));

    vws_buffer_free(b);
    vrtql_msg_free(src);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_message: watchdog deadline exceeded — abort\n");
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
