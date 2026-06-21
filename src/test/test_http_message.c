// test_http_message.c — http_message.c coverage (item-4 BATCH-1, module 2).
//
// http_message.c wraps llhttp + vws_buffer/vws_kvs, so it is NOT fully
// self-contained: this TU #includes it (to reach the static callbacks +
// process_header + lcase, and to ISOLATE coverage to http_message.c) but links
// the clean libvws.a for the buffer/kvs/llhttp/vws deps. The #included
// http_message symbols shadow libvws.a's http_message.o (the archive member is
// never pulled), so there is no duplicate definition and libvws stays
// non-instrumented (clean by construction — nothing to restore).
//
// SUBJECT, not modified: a real bug a cell surfaces is ESCALATED, not patched.
// SELF-BOUNDING: a wall-clock _Exit watchdog.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "http_message.h"

#include <pthread.h>
#include <unistd.h>

// http_message.c is the SUBJECT (statics + isolated coverage).
#include "../http_message.c"

// One-shot ld --wrap on llhttp_execute: force the (HPE_PAUSED_UPGRADE, !done)
// condition deterministically -- a real WS-upgrade request does NOT reliably
// produce it (llhttp reports the upgrade as HPE_OK + an upgrade flag), so the
// PAUSED + !done arm (http_message.c:225-228) can only be forced here.
extern enum llhttp_errno
__real_llhttp_execute(llhttp_t* p, const char* data, size_t len);
static int g_force_pause_upgrade = 0;

enum llhttp_errno
__wrap_llhttp_execute(llhttp_t* p, const char* data, size_t len)
{
    if (g_force_pause_upgrade)
    {
        g_force_pause_upgrade = 0;
        return HPE_PAUSED_UPGRADE;   // no callbacks run -> req->done stays false
    }
    return __real_llhttp_execute(p, data, len);
}

static const int WATCHDOG_SECONDS = 60;

//------------------------------------------------------------------------------
// lcase + the NULL free guard
//------------------------------------------------------------------------------

CTEST(http_msg, lcase)
{
    char s[] = "HeLLo-World";
    ucstr r = lcase(s);
    ASSERT_STR("hello-world", (cstr)r);
}

CTEST(http_msg, free_null)
{
    vws_http_msg_free(NULL);   // lines 137-140
}

//------------------------------------------------------------------------------
// Complete request: callbacks + process_header (both branches) + the done path
//------------------------------------------------------------------------------

CTEST(http_msg, parse_get_request)
{
    char* data = "GET /hello HTTP/1.1\r\n"
                 "Host: example.com\r\n"
                 "User-Agent: curl/7.68.0\r\n"
                 "Accept: */*\r\n"
                 "\r\n";

    vws_http_msg* req = vws_http_msg_new(HTTP_REQUEST);

    // Single complete message -> done; the byte count equals the input length.
    int n = vws_http_msg_parse(req, data, strlen(data));
    ASSERT_EQUAL((int)strlen(data), n);
    ASSERT_TRUE(req->headers_complete == true);
    ASSERT_TRUE(req->done == true);

    // on_url accumulated the request target.
    ASSERT_TRUE(req->url->size > 0);

    // Headers were lower-cased + stored (process_header value!=0 branch; the
    // first on_header_field hit the value==0 branch).
    cstr host = vws_kvs_get_cstring(req->headers, "host");
    ASSERT_NOT_NULL(host);
    ASSERT_STR("example.com", host);

    // Request-line getters.
    ASSERT_EQUAL(1, (int)vws_http_msg_version_major(req));
    ASSERT_EQUAL(1, (int)vws_http_msg_version_minor(req));
    ASSERT_STR("GET", vws_http_msg_method_string(req));

    vws_http_msg_free(req);
}

//------------------------------------------------------------------------------
// POST with a body: on_body + content_length
//------------------------------------------------------------------------------

CTEST(http_msg, parse_post_body)
{
    char* data = "POST /submit HTTP/1.1\r\n"
                 "Host: example.com\r\n"
                 "Content-Length: 11\r\n"
                 "\r\n"
                 "hello world";

    vws_http_msg* req = vws_http_msg_new(HTTP_REQUEST);
    vws_http_msg_parse(req, data, strlen(data));

    ASSERT_TRUE(req->done == true);
    ASSERT_TRUE(req->body->size >= 11);   // on_body appended the payload
    // llhttp decrements content_length as the body is consumed; 0 once the
    // message is complete (the getter at http_message.c:261 is exercised).
    ASSERT_EQUAL(0, (int)vws_http_msg_content_length(req));

    vws_http_msg_free(req);
}

//------------------------------------------------------------------------------
// vws_http_msg_parse branch matrix: partial / malformed / upgrade
//------------------------------------------------------------------------------

CTEST(http_msg, parse_partial)
{
    // Incomplete request (no terminating CRLFCRLF): rc == HPE_OK, not done ->
    // line 256 returns the full size (all bytes consumed by parsing).
    char* data = "GET /slow HTTP/1.1\r\n"
                 "Host: example.com\r\n";

    vws_http_msg* req = vws_http_msg_new(HTTP_REQUEST);
    int n = vws_http_msg_parse(req, data, strlen(data));
    ASSERT_EQUAL((int)strlen(data), n);
    ASSERT_TRUE(req->done == false);
    vws_http_msg_free(req);
}

CTEST(http_msg, parse_malformed)
{
    // A bad request line -> llhttp error (not OK, not paused) -> the else arm
    // (line 231-234) returns -1; the errno getter is then non-OK.
    char* data = "GET / NOTHTTP\r\n\r\n";

    vws_http_msg* req = vws_http_msg_new(HTTP_REQUEST);
    int n = vws_http_msg_parse(req, data, strlen(data));
    ASSERT_EQUAL(-1, n);
    ASSERT_TRUE(vws_http_msg_errno(req) != HPE_OK);
    vws_http_msg_free(req);
}

CTEST(http_msg, parse_paused_not_done)
{
    // HPE_PAUSED_UPGRADE with done==false -> the abnormal-pause arm returns -1
    // (http_message.c:220 true, :225 true, :228). Forced via __wrap_llhttp_execute
    // (no callbacks run, so done stays false). Documents that a pause for any
    // reason other than on_message_complete is treated as a fatal parse.
    vws_http_msg* req = vws_http_msg_new(HTTP_REQUEST);
    g_force_pause_upgrade = 1;
    int n = vws_http_msg_parse(req, "GET / HTTP/1.1\r\n", 16);
    ASSERT_EQUAL(-1, n);
    ASSERT_TRUE(req->done == false);
    vws_http_msg_free(req);
}

//------------------------------------------------------------------------------
// Response mode: status_code + status_string
//------------------------------------------------------------------------------

CTEST(http_msg, parse_response)
{
    // C-HM-1: a 3-digit status code must survive (was truncated to uint8_t ->
    // 404 & 0xFF == 148). RED on current (148), GREEN after widening to int.
    char* data = "HTTP/1.1 404 Not Found\r\n"
                 "Content-Length: 0\r\n"
                 "\r\n";

    vws_http_msg* res = vws_http_msg_new(HTTP_RESPONSE);
    vws_http_msg_parse(res, data, strlen(data));

    ASSERT_EQUAL(404, (int)vws_http_msg_status_code(res));
    ASSERT_STR("NOT_FOUND", vws_http_msg_status_string(res));

    vws_http_msg_free(res);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_http_message: watchdog deadline exceeded — abort\n");
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
