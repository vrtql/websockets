// test_rpc_cov.c — rpc.c coverage (item-4 BATCH-1, module 5).
//
// The RPC client (tag/exec/invoke/reconnect) + server (module/system maps,
// parse_rpc_string, reply, service dispatch). #includes rpc.c (reaches the
// static helpers + isolates coverage) + links clean libvws for the
// vrtql_msg/vws/sc_map deps (the #included rpc symbols shadow rpc.o). The
// network is removed by one-shot ld --wrap on the message layer: vrtql_msg_send
// / vrtql_msg_recv / vws_reconnect drive exec deterministically (the recv
// reply echoes the request tag so dispatch matches); RAND_bytes for the tag arm.
//
// SUBJECT, not modified: a real bug a cell surfaces is ESCALATED, not patched.
// SELF-BOUNDING: a wall-clock _Exit watchdog.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "rpc.h"

#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>

//------------------------------------------------------------------------------
// Wrap-the-message-layer harness
//------------------------------------------------------------------------------

extern int __real_RAND_bytes(unsigned char* buf, int num);

static int  g_rand_fail   = 0;
static int  g_send_fails  = 0;     // fail N sends with VE_SOCKET, then succeed
static int  g_reconnect_ok = 1;
static char g_sent_tag[64];

// recv "script": one action consumed per vrtql_msg_recv call.
enum { RV_MATCH = 0, RV_MISMATCH, RV_TIMEOUT, RV_SOCKET, RV_GENERIC };
static int  g_recv_script[8];
static int  g_recv_n   = 0;
static int  g_recv_idx = 0;
static cstr g_reply_c  = NULL;     // "c" header on a matched reply (or NULL)
static cstr g_reply_m  = NULL;     // "m" header on a matched reply (or NULL)
static int  g_reply_content = 0;   // attach content to the matched reply

int __wrap_RAND_bytes(unsigned char* buf, int num)
{
    if (g_rand_fail)
    {
        g_rand_fail = 0;
        return 0;
    }
    return __real_RAND_bytes(buf, num);
}

ssize_t __wrap_vrtql_msg_send(vws_cnx* c, vrtql_msg* m)
{
    (void)c;
    // Routing key aligned to "t" : exec now tags the request with
    // "t", matching the reply builder and the broker wire contract.
    cstr t = vrtql_msg_get_routing(m, "t");
    if (t != NULL)
    {
        strncpy(g_sent_tag, t, sizeof(g_sent_tag) - 1);
        g_sent_tag[sizeof(g_sent_tag) - 1] = '\0';
    }
    if (g_send_fails > 0)
    {
        g_send_fails--;
        vws.e.code = VE_SOCKET;
        return -1;
    }
    return 10;
}

vrtql_msg* __wrap_vrtql_msg_recv(vws_cnx* c)
{
    (void)c;
    int action = (g_recv_idx < g_recv_n) ? g_recv_script[g_recv_idx] : RV_MATCH;
    g_recv_idx++;

    if (action == RV_TIMEOUT) { vws.e.code = VE_TIMEOUT; return NULL; }
    if (action == RV_SOCKET)  { vws.e.code = VE_SOCKET;  return NULL; }
    if (action == RV_GENERIC) { vws.e.code = VE_RT;      return NULL; }

    vrtql_msg* r = vrtql_msg_new();
    // Echo the tag under key "t"  so exec's aligned read matches.
    vrtql_msg_set_routing(r, "t",
                          action == RV_MISMATCH ? "WRONGTAG" : g_sent_tag);
    if (g_reply_c != NULL) vrtql_msg_set_header(r, "c", g_reply_c);
    if (g_reply_m != NULL) vrtql_msg_set_header(r, "m", g_reply_m);
    if (g_reply_content)   vrtql_msg_set_content(r, "result");
    return r;
}

extern bool __real_vws_reconnect(vws_cnx* c);
bool __wrap_vws_reconnect(vws_cnx* c)
{
    (void)c;
    return g_reconnect_ok ? true : false;
}

// rpc.c is the SUBJECT (statics + isolated coverage).
#include "../rpc.c"

static const int WATCHDOG_SECONDS = 60;

static void recv_script(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    g_recv_n = n;
    g_recv_idx = 0;
    for (int i = 0; i < n && i < 8; i++) g_recv_script[i] = va_arg(ap, int);
    va_end(ap);
}

static void reset_harness(void)
{
    g_rand_fail = 0; g_send_fails = 0; g_reconnect_ok = 1;
    g_recv_n = 0; g_recv_idx = 0;
    g_reply_c = NULL; g_reply_m = NULL; g_reply_content = 0;
}

//------------------------------------------------------------------------------
// tag
//------------------------------------------------------------------------------

CTEST(rpc, tag)
{
    reset_harness();
    // C-RPC-6 witness: on current the tag is not NUL-terminated -> strlen here
    // (and at exec:174) over-reads (valgrind RED); clean after the fix.
    char* t = vrtql_rpc_tag(7);
    ASSERT_NOT_NULL(t);
    ASSERT_EQUAL(7, (int)strlen(t));
    // [vws R4] vws.free pairs vrtql_rpc_tag's vws.malloc (allocator symmetry).
    vws.free(t);

    g_rand_fail = 1;
    char* f = vrtql_rpc_tag(7);   // RAND_bytes fails -> NULL
    g_rand_fail = 0;
    ASSERT_NULL(f);
}

//------------------------------------------------------------------------------
// module / system maps
//------------------------------------------------------------------------------

static vrtql_msg* dummy_call(vrtql_rpc_env* e, vrtql_msg* m)
{
    (void)e;
    return vrtql_rpc_reply(m);
}

static vrtql_msg* null_call(vrtql_rpc_env* e, vrtql_msg* m)
{
    (void)e; (void)m;
    return NULL;
}

CTEST(rpc, module_system_maps)
{
    vrtql_rpc_module* mod = vrtql_rpc_module_new("session");
    ASSERT_NOT_NULL(mod);
    vrtql_rpc_module_set(mod, "echo", dummy_call);
    ASSERT_TRUE(vrtql_rpc_module_get(mod, "echo") == dummy_call);
    ASSERT_NULL(vrtql_rpc_module_get(mod, "missing"));   // sys_map_get miss
    // C-RPC-4 FIXED: the sys_map_set UPDATE path now always strdup's + frees the
    // old key, so updating an existing key is safe. Exercise it here (was the
    // candidate that aborted module_free); the dedicated abort/leak falsifier is
    // f_c_rpc_4_update_nonheap_key (fork). The "echo" literal below is the exact
    // non-heap key that the pre-fix code stored + later vws.free()'d.
    vrtql_rpc_module_set(mod, "echo", null_call);        // UPDATE (same key)
    ASSERT_TRUE(vrtql_rpc_module_get(mod, "echo") == null_call);

    vrtql_rpc_system* sys = vrtql_rpc_system_new();
    ASSERT_NOT_NULL(sys);
    vrtql_rpc_system_set(sys, mod);
    ASSERT_TRUE(vrtql_rpc_system_get(sys, "session") == mod);
    ASSERT_NULL(vrtql_rpc_system_get(sys, "nope"));

    vrtql_rpc_system_free(sys);   // frees the module too
}

// C-RPC-4 (sys_map_set UPDATE stores caller's non-heap key): updating an
// existing key with a string LITERAL ("echo") stored that literal pointer; on
// the pre-fix code module_free then vws.free()'d it -> glibc invalid-pointer
// abort. Fork-repro: RED = child aborts/crashes pre-fix, GREEN = the strdup+
// free-old fix makes module_free clean and the child survives.
CTEST(rpc, f_c_rpc_4_update_nonheap_key)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGABRT, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        vrtql_rpc_module* mod = vrtql_rpc_module_new("m");
        vrtql_rpc_module_set(mod, "echo", dummy_call);   // insert -> strdup "echo"
        vrtql_rpc_module_set(mod, "echo", null_call);    // UPDATE -> literal pre-fix
        vrtql_rpc_module_free(mod);                       // pre-fix: free(literal) abort
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));         // RED: SIGABRT/SIGSEGV on pre-fix
    ASSERT_EQUAL(0, WEXITSTATUS(status));
}

// C-RPC-5 (module_new(NULL) error-set-but-falls-through): the NULL guard set
// the error but did not return, so strdup(NULL) (strlen(NULL)) crashed.
// Fork-repro: RED = child SIGSEGV pre-fix, GREEN = returns NULL, child survives.
CTEST(rpc, f_c_rpc_5_module_new_null)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        vrtql_rpc_module* m = vrtql_rpc_module_new(NULL);
        _exit(m == NULL ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQUAL(0, WEXITSTATUS(status));
}

//------------------------------------------------------------------------------
// parse_rpc_string
//------------------------------------------------------------------------------

CTEST(rpc, parse_rpc_string)
{
    char* m; char* f;

    ASSERT_TRUE(parse_rpc_string("session.echo", &m, &f));
    ASSERT_STR("session", m);
    ASSERT_STR("echo", f);
    vws.free(m); vws.free(f);

    ASSERT_FALSE(parse_rpc_string("nodelimiter", &m, &f));   // no '.'

    // Empty segments are now REJECTED (was accepted -> lookup-miss). A malformed
    // id with no module or no function is invalid. (revert the m_len/f_len==0
    // guard -> these return true -> RED.)
    ASSERT_FALSE(parse_rpc_string(".echo", &m, &f));     // empty module
    ASSERT_FALSE(parse_rpc_string("session.", &m, &f));  // empty function
    ASSERT_FALSE(parse_rpc_string(".", &m, &f));         // both empty
}

//------------------------------------------------------------------------------
// reply
//------------------------------------------------------------------------------

CTEST(rpc, reply)
{
    vrtql_msg* req = vrtql_msg_new();
    req->format = VM_JSON_FORMAT;
    vrtql_msg_set_routing(req, "t", "tag123");
    vrtql_msg_set_routing(req, "from", "peerA");

    vrtql_msg* r = vrtql_rpc_reply(req);
    ASSERT_STR("tag123", vrtql_msg_get_routing(r, "t"));
    ASSERT_STR("peerA", vrtql_msg_get_routing(r, "to"));
    ASSERT_EQUAL(VM_JSON_FORMAT, r->format);
    vrtql_msg_free(req);
    vrtql_msg_free(r);

    // No tag / no from -> those routings absent.
    vrtql_msg* req2 = vrtql_msg_new();
    vrtql_msg* r2 = vrtql_rpc_reply(req2);
    ASSERT_NULL(vrtql_msg_get_routing(r2, "t"));
    ASSERT_NULL(vrtql_msg_get_routing(r2, "to"));
    vrtql_msg_free(req2);
    vrtql_msg_free(r2);
}

//------------------------------------------------------------------------------
// service dispatch + error arms
//------------------------------------------------------------------------------

CTEST(rpc, service)
{
    vrtql_rpc_system* sys = vrtql_rpc_system_new();
    vrtql_rpc_module* mod = vrtql_rpc_module_new("session");
    vrtql_rpc_module_set(mod, "echo", dummy_call);
    vrtql_rpc_module_set(mod, "bad", null_call);
    vrtql_rpc_system_set(sys, mod);

    vrtql_rpc_env env;
    memset(&env, 0, sizeof(env));

    // Happy dispatch.
    vrtql_msg* req = vrtql_msg_new();
    vrtql_msg_set_header(req, "id", "session.echo");
    vrtql_msg* reply = vrtql_rpc_service(sys, &env, req);
    ASSERT_NOT_NULL(reply);
    ASSERT_TRUE(env.module == mod);
    vrtql_msg_free(reply);

    // id missing -> NULL.
    vrtql_msg* r2 = vrtql_msg_new();
    ASSERT_NULL(vrtql_rpc_service(sys, &env, r2));

    // parse fail (no '.') -> NULL.
    vrtql_msg* r3 = vrtql_msg_new();
    vrtql_msg_set_header(r3, "id", "nodelim");
    ASSERT_NULL(vrtql_rpc_service(sys, &env, r3));

    // module miss -> NULL.
    vrtql_msg* r4 = vrtql_msg_new();
    vrtql_msg_set_header(r4, "id", "nomod.fn");
    ASSERT_NULL(vrtql_rpc_service(sys, &env, r4));

    // call miss -> NULL.
    vrtql_msg* r5 = vrtql_msg_new();
    vrtql_msg_set_header(r5, "id", "session.nofn");
    ASSERT_NULL(vrtql_rpc_service(sys, &env, r5));

    // handler returns NULL.
    vrtql_msg* r6 = vrtql_msg_new();
    vrtql_msg_set_header(r6, "id", "session.bad");
    ASSERT_NULL(vrtql_rpc_service(sys, &env, r6));

    vrtql_rpc_system_free(sys);
}

//------------------------------------------------------------------------------
// reconnect + new/free
//------------------------------------------------------------------------------

static bool reconnect_cb_ok(vrtql_rpc* rpc)   { (void)rpc; return true;  }
static bool reconnect_cb_fail(vrtql_rpc* rpc) { (void)rpc; return false; }

CTEST(rpc, reconnect)
{
    reset_harness();
    vrtql_rpc* rpc = vrtql_rpc_new(NULL);

    g_reconnect_ok = 0;
    ASSERT_FALSE(reconnect(rpc));            // vws_reconnect fails

    g_reconnect_ok = 1;
    rpc->reconnect = NULL;
    ASSERT_TRUE(reconnect(rpc));             // no user handler

    rpc->reconnect = reconnect_cb_fail;
    ASSERT_FALSE(reconnect(rpc));            // user handler fails

    rpc->reconnect = reconnect_cb_ok;
    ASSERT_TRUE(reconnect(rpc));             // all ok

    vrtql_rpc_free(rpc);
}

//------------------------------------------------------------------------------
// exec — the send/recv/retry/out-of-band machinery
//------------------------------------------------------------------------------

CTEST(rpc, exec_paths)
{
    reset_harness();
    vrtql_rpc* rpc = vrtql_rpc_new(NULL);

    // Send ok, first recv matches.
    recv_script(1, RV_MATCH);
    vrtql_msg* req = vrtql_msg_new();
    vrtql_msg_set_header(req, "id", "a.b");
    vrtql_msg* reply = vrtql_rpc_exec(rpc, req);
    ASSERT_NOT_NULL(reply);
    vrtql_msg_free(req);
    vrtql_msg_free(reply);

    // Send fails once (VE_SOCKET) -> reconnect ok -> retry -> match.
    reset_harness();
    g_send_fails = 1; g_reconnect_ok = 1;
    recv_script(1, RV_MATCH);
    req = vrtql_msg_new();
    reply = vrtql_rpc_exec(rpc, req);
    ASSERT_NOT_NULL(reply);
    vrtql_msg_free(req);
    vrtql_msg_free(reply);

    // Send fails -> reconnect fails -> NULL (VE_SEND set).
    reset_harness();
    g_send_fails = 1; g_reconnect_ok = 0;
    req = vrtql_msg_new();
    ASSERT_NULL(vrtql_rpc_exec(rpc, req));
    ASSERT_TRUE(vws_is_flag(&vws.e.code, VE_SEND));
    vrtql_msg_free(req);

    // Tag mismatch -> out_of_band -> then a match.
    reset_harness();
    recv_script(2, RV_MISMATCH, RV_MATCH);
    req = vrtql_msg_new();
    reply = vrtql_rpc_exec(rpc, req);
    ASSERT_NOT_NULL(reply);
    vrtql_msg_free(req);
    vrtql_msg_free(reply);

    // Timeouts exhaust the retries -> NULL.
    reset_harness();
    rpc->retries = 2;
    recv_script(2, RV_TIMEOUT, RV_TIMEOUT);
    req = vrtql_msg_new();
    ASSERT_NULL(vrtql_rpc_exec(rpc, req));
    vrtql_msg_free(req);

    // recv socket error -> NULL (VE_RECV set).
    reset_harness();
    rpc->retries = 5;
    recv_script(1, RV_SOCKET);
    req = vrtql_msg_new();
    ASSERT_NULL(vrtql_rpc_exec(rpc, req));
    ASSERT_TRUE(vws_is_flag(&vws.e.code, VE_RECV));
    vrtql_msg_free(req);

    vrtql_rpc_free(rpc);
}

//------------------------------------------------------------------------------
// invoke + out_of_band_default
//------------------------------------------------------------------------------

CTEST(rpc, invoke)
{
    reset_harness();
    vrtql_rpc* rpc = vrtql_rpc_new(NULL);

    // Happy invoke: reply carries "c" + "m" headers + content.
    recv_script(1, RV_MATCH);
    g_reply_c = "0"; g_reply_m = "ok"; g_reply_content = 1;
    vrtql_msg* req = vrtql_msg_new();
    vrtql_msg_set_content(req, "args");   // req->content->size > 0 path
    ASSERT_TRUE(vrtql_rpc_invoke(rpc, req));   // frees req

    // reply with "c" but no "m" -> the else arm (:93-95) atoi(rc), rc non-NULL.
    reset_harness();
    recv_script(1, RV_MATCH);
    g_reply_c = "7"; g_reply_m = NULL;
    vrtql_msg* reqE = vrtql_msg_new();
    ASSERT_TRUE(vrtql_rpc_invoke(rpc, reqE));

    // reply NULL (recv socket error) -> false.
    reset_harness();
    recv_script(1, RV_SOCKET);
    vrtql_msg* req2 = vrtql_msg_new();
    ASSERT_FALSE(vrtql_rpc_invoke(rpc, req2));
    // [vws R1] Do NOT free req2 here. vrtql_rpc_invoke documents taking ownership
    // of req ("automatically freed. Caller should NOT use this message again")
    // and now honors that on the FAILURE path too (R1 -- it previously leaked req
    // on failure). Freeing it here double-frees (crash in vws_kvs_clear). Real
    // callers (vcs_login/vcs_exec) correctly do not free on failure.

    vrtql_rpc_free(rpc);
}

CTEST(rpc, trace_paths)
{
    reset_harness();
    int saved = vws.tracelevel;
    vws.tracelevel = VT_SERVICE;

    // exec with trace -> the send + recv dump blocks (:115-122, :213-220).
    vrtql_rpc* rpc = vrtql_rpc_new(NULL);
    recv_script(1, RV_MATCH);
    vrtql_msg* req = vrtql_msg_new();
    vrtql_msg* reply = vrtql_rpc_exec(rpc, req);
    vrtql_msg_free(req);
    if (reply) vrtql_msg_free(reply);
    vrtql_rpc_free(rpc);

    // service with trace -> received + sent dump blocks (:457-464, :531-538).
    vrtql_rpc_system* sys = vrtql_rpc_system_new();
    vrtql_rpc_module* mod = vrtql_rpc_module_new("m");
    vrtql_rpc_module_set(mod, "f", dummy_call);
    vrtql_rpc_system_set(sys, mod);
    vrtql_rpc_env env;
    memset(&env, 0, sizeof(env));
    vrtql_msg* sreq = vrtql_msg_new();
    vrtql_msg_set_header(sreq, "id", "m.f");
    vrtql_msg* sreply = vrtql_rpc_service(sys, &env, sreq);
    if (sreply) vrtql_msg_free(sreply);
    vrtql_rpc_system_free(sys);

    vws.tracelevel = saved;
}

CTEST(rpc, out_of_band_default)
{
    vrtql_rpc* rpc = vrtql_rpc_new(NULL);
    vrtql_msg* m = vrtql_msg_new();
    out_of_band_default(rpc, m);   // frees m
    vrtql_rpc_free(rpc);
}

CTEST(rpc, f_c_rpc_1_no_c_header)
{
    // C-RPC-1 (wire-reachable HIGH): a peer reply with NO "c" header -> rc=NULL
    // -> the else arm atoi(NULL) -> SEGV (remote DoS). Fork: RED on current
    // (child SIGSEGV), GREEN after the rc NULL-guard.
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        reset_harness();
        vrtql_rpc* rpc = vrtql_rpc_new(NULL);
        recv_script(1, RV_MATCH);   // matched reply, no "c"/"m" headers
        vrtql_msg* req = vrtql_msg_new();
        vrtql_rpc_invoke(rpc, req);   // atoi(NULL) on current
        vrtql_rpc_free(rpc);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));   // RED: SIGSEGV on current rpc.c
}

CTEST(rpc, c_rpc_3_reply_content)
{
    // C-RPC-3 (wire-reachable): the reply-content copy is gated on the REQUEST
    // size (:79), so a peer reply WITH content is dropped when the request had
    // none. RED on current (rpc->val empty), GREEN after gating on
    // reply->content->size. ("c" set so the C-RPC-1 atoi path is benign.)
    reset_harness();
    vrtql_rpc* rpc = vrtql_rpc_new(NULL);
    recv_script(1, RV_MATCH);
    g_reply_c = "0"; g_reply_content = 1;   // reply carries content
    vrtql_msg* req = vrtql_msg_new();        // request has NO content
    ASSERT_TRUE(vrtql_rpc_invoke(rpc, req)); // frees req
    ASSERT_TRUE(rpc->val->size > 0);         // reply content copied
    vrtql_rpc_free(rpc);
}

CTEST(rpc, c_rpc_2_free_no_leak)
{
    // C-RPC-2: vrtql_rpc_free calls vws_buffer_new(rpc->val) instead of
    // vws_buffer_free -> leaks rpc->val EVERY free. Valgrind RED on current
    // (definitely-lost), clean after the fix.
    vrtql_rpc* rpc = vrtql_rpc_new(NULL);
    vrtql_rpc_free(rpc);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_rpc_cov: watchdog deadline exceeded — abort\n");
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
