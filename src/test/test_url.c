// test_url.c — url.c coverage (item-4 BATCH-1, module 1).
//
// url.c is self-contained (pure libc: malloc/calloc/strdup/sscanf/sprintf), so
// this TU #includes it directly. Two payoffs: the static helpers (get_part,
// strff) become reachable, and coverage is ISOLATED to url.c — the shared
// libvws/libvai are never instrumented (clean by construction, nothing to
// restore). One-shot ld --wrap on malloc/calloc forces the allocation-failure
// arms; url.c's own strdup() routes through the wrapped malloc.
//
// SUBJECT, not modified: a real bug a cell surfaces is ESCALATED, not patched.
// url.c has several use-before-NULL-check crash paths (see the report's
// candidate-findings list); this suite covers the REACHABLE surface and leaves
// those crash-guarded branches as the proven-dead/candidate residual.
//
// SELF-BOUNDING: a wall-clock _Exit watchdog (no live servers/loops here).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

// One-shot libc fault injection. Default pass-through; arm the Nth call to fail
// (1 = next call), then auto-disarm.
extern void* __real_malloc(size_t);
extern void* __real_calloc(size_t, size_t);
static int g_malloc_fail = 0;
static int g_calloc_fail = 0;

void* __wrap_malloc(size_t n)
{
    if (g_malloc_fail && --g_malloc_fail == 0)
    {
        return NULL;
    }
    return __real_malloc(n);
}

void* __wrap_calloc(size_t a, size_t b)
{
    if (g_calloc_fail && --g_calloc_fail == 0)
    {
        return NULL;
    }
    return __real_calloc(a, b);
}

// url.c is the SUBJECT (statics + isolated coverage).
#include "url.c"

#define CTEST_MAIN
#include "ctest.h"

static const int WATCHDOG_SECONDS = 60;

// The canonical fully-populated URL (every component present) — the safe
// happy-path input for the accessors (no NULL-component crash path).
#define FULL_URL "http://user:pass@host.com:8080/path/to/something" \
                 "?query=string#hash"

//------------------------------------------------------------------------------
// Scheme predicates
//------------------------------------------------------------------------------

CTEST(url, is_protocol)
{
    ASSERT_TRUE(url_is_protocol("http"));
    ASSERT_TRUE(url_is_protocol("https"));
    ASSERT_TRUE(url_is_protocol("ws"));
    ASSERT_TRUE(url_is_protocol("wss"));
    ASSERT_TRUE(url_is_protocol("ssh"));
    ASSERT_TRUE(url_is_protocol("git"));
    ASSERT_TRUE(url_is_protocol("doi"));     // last (unofficial) entry
    ASSERT_FALSE(url_is_protocol("zzz"));    // table miss -> false
    ASSERT_FALSE(url_is_protocol(""));
}

CTEST(url, is_ssh)
{
    ASSERT_TRUE(url_is_ssh("ssh"));
    ASSERT_TRUE(url_is_ssh("git"));
    ASSERT_FALSE(url_is_ssh("http"));
}

//------------------------------------------------------------------------------
// Static helpers (reachable via the direct #include)
//------------------------------------------------------------------------------

CTEST(url, strff)
{
    char* r = strff("abcdef", 3);
    ASSERT_NOT_NULL(r);
    ASSERT_STR("def", r);
    free(r);
}

CTEST(url, get_part)
{
    // Skip "http://" (7) then read up to '@' -> "user:pass".
    char* r = get_part("http://user:pass@host", "%[^@]", 7);
    ASSERT_NOT_NULL(r);
    ASSERT_STR("user:pass", r);
    free(r);
    // NOTE: the get_part strdup-failure early-return (url.c:63-64) is NOT
    // covered here -- taking it LEAKS the sibling strdups (candidate C-URL-6),
    // so a clean cell can't exercise it; left as residual pending the ruling.
}

//------------------------------------------------------------------------------
// url_get_protocol — valid / table-miss / malloc-failure
//------------------------------------------------------------------------------

CTEST(url, get_protocol)
{
    char* p = url_get_protocol("http://host");
    ASSERT_NOT_NULL(p);
    ASSERT_STR("http", p);
    free(p);

    // Scheme not in the table -> NULL.
    char* bad = url_get_protocol("zzz://host");
    ASSERT_NULL(bad);

    // malloc-failure arm (line 224-225): the protocol buffer alloc fails.
    g_malloc_fail = 1;
    char* nomem = url_get_protocol("http://host");
    g_malloc_fail = 0;
    ASSERT_NULL(nomem);
}

//------------------------------------------------------------------------------
// url_parse — full, ssh, calloc-failure, invalid scheme
//------------------------------------------------------------------------------

CTEST(url, parse_full)
{
    char href[] = FULL_URL;
    url_data_t* u = url_parse(href);
    ASSERT_NOT_NULL(u);

    ASSERT_STR("http", u->protocol);
    ASSERT_STR("user:pass", u->auth);
    ASSERT_STR("host.com", u->host);
    ASSERT_STR("8080", u->port);
    ASSERT_STR("host.com:8080", u->hostname);
    ASSERT_NOT_NULL(u->path);
    ASSERT_NOT_NULL(u->pathname);
    ASSERT_NOT_NULL(u->search);
    ASSERT_NOT_NULL(u->query);
    ASSERT_NOT_NULL(u->hash);

    url_free(u);
}

CTEST(url, parse_ssh)
{
    // is_ssh path: ':' host/path split + "%s" (no leading '/') path format.
    char href[] = "ssh://git@host.com:repo/path";
    url_data_t* u = url_parse(href);
    ASSERT_NOT_NULL(u);
    ASSERT_STR("ssh", u->protocol);
    ASSERT_STR("git", u->auth);
    ASSERT_STR("host.com", u->host);
    url_free(u);
}

CTEST(url, parse_calloc_fail)
{
    // Line 93-94: the url_data_t calloc fails -> NULL (the one calloc in parse).
    char href[] = FULL_URL;
    g_calloc_fail = 1;
    url_data_t* u = url_parse(href);
    g_calloc_fail = 0;
    ASSERT_NULL(u);
}

CTEST(url, parse_invalid_scheme)
{
    // Line 99-104: url_get_protocol returns NULL -> parse frees + returns NULL.
    char href[] = "zzz://host/path";
    url_data_t* u = url_parse(href);
    ASSERT_NULL(u);
}

CTEST(url, get_hostname_ssh)
{
    // url_get_hostname is_ssh true-branch (line 261-262): ':' host delimiter.
    char* h = url_get_hostname("ssh://git@host.com:repo");
    ASSERT_NOT_NULL(h);
    ASSERT_STR("host.com", h);
    free(h);
}

CTEST(url, get_path_ssh)
{
    // url_get_path is_ssh true-branch (line 313-314 + the "%s" path format).
    char* p = url_get_path("ssh://git@host.com:repo/x");
    ASSERT_NOT_NULL(p);
    free(p);
}

// Allocation-failure cleanup arms in url_parse. The malloc ordinals are the
// deterministic sequence for FULL_URL (probed): #12 = the hostname get_part's
// final strdup (so get_part returns NULL -> the !hostname cleanup), #22 = the
// pathname malloc. url.c is the SUBJECT (fixed) so the sequence is stable; the
// pinned URL keeps the ordinals valid. (The !path cleanup, url.c:153-156, is
// NOT armed: taking it leaks tmp_path -- candidate C-URL-7 -- so it is left as
// residual pending the ruling.)
CTEST(url, parse_hostname_alloc_fail)
{
    char href[] = FULL_URL;
    g_malloc_fail = 12;
    url_data_t* u = url_parse(href);
    g_malloc_fail = 0;
    ASSERT_NULL(u);   // lines 122-125 (!hostname cleanup)
}

CTEST(url, parse_pathname_alloc_fail)
{
    char href[] = FULL_URL;
    g_malloc_fail = 22;
    url_data_t* u = url_parse(href);
    g_malloc_fail = 0;
    ASSERT_NULL(u);   // lines 164-167 (!pathname cleanup)
}

// C-URL-8 (host use-before-check, OOM): the host malloc is the 14th malloc in
// url_parse(FULL_URL) (probed; stable for the pinned URL). The fix reorders the
// !host check BEFORE the sscanf. NOTE: this is a behavior-IDENTICAL hardening on
// glibc -- sscanf("...","%[^:]",NULL) returns 0 WITHOUT writing/crashing (the
// pointer-deref UB the standard warns of is tolerated here), so the pre-fix code
// already fell through to the !host cleanup with no crash and no leak. So there
// is no runtime falsifier; this cell is a no-regression/coverage pin of the
// host-OOM cleanup arm (the fix removes reliance on sscanf's NULL-tolerance).
CTEST(url, parse_host_alloc_fail)
{
    char href[] = FULL_URL;
    g_malloc_fail = 14;
    url_data_t* u = url_parse(href);
    g_malloc_fail = 0;
    ASSERT_NULL(u);   // host-malloc-fail -> !host cleanup -> NULL (valgrind-clean)
}

// C-URL-10 (url_parse !path cleanup leak, OOM): the path malloc is the 21st
// malloc (probed). The !path cleanup freed tmp_url + data but not tmp_path. In
// the PARENT (so valgrind sees it): RED = tmp_path leaks on the pre-fix code,
// GREEN = freed. No crash either way, so this is parent-safe.
CTEST(url, parse_path_alloc_fail)
{
    char href[] = FULL_URL;
    g_malloc_fail = 21;
    url_data_t* u = url_parse(href);
    g_malloc_fail = 0;
    ASSERT_NULL(u);   // !path cleanup; valgrind-clean only after the tmp_path free
}

// C-URL-1 (url_get_hash strlen(NULL)): url_get_search returns NULL when a URL
// has no '?search' component; strlen(NULL) crashed. Fork-repro on a no-search
// URL: RED = child SIGSEGV pre-fix, GREEN = survives (search_len guarded to 0).
CTEST(url, f_c_url_1_no_search_hash)
{
    // Same auth/host/path structure as FULL_URL (so the hostname-parse chain is
    // valgrind-clean, as accessors_full proves) but with NO "?query" -> so
    // url_get_search returns NULL and strlen(search) is reached. Fork-repro:
    // RED = child SIGSEGV pre-fix (strlen(NULL)), GREEN = survives (guarded 0).
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        char* h = url_get_hash("http://user:pass@host.com:8080/path/to/something");
        free(h);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQUAL(0, WEXITSTATUS(status));
}

// C-URL-9 (get_part strdup-fail leak): the 3-way strdup-failure early-return
// freed none of the strdups that succeeded. Arm the 3rd alloc (fmt_url's
// strdup) to fail; run in the PARENT so valgrind sees the tmp+tmp_url leak.
// RED = leak pre-fix, GREEN = all three freed.
CTEST(url, f_c_url_9_get_part_leak)
{
    g_malloc_fail = 3;
    char* r = get_part("http://user@host", "%[^@]", 7);
    g_malloc_fail = 0;
    ASSERT_NULL(r);
}

// Concrete follow-on (Mike policy: FIX concrete local bugs). get_part/strff
// over-read 1 byte past the buffer when the offset exceeded strlen(url) (short
// hosts). Parent valgrind cell: RED ("Invalid read of size 1") if the get_part
// clamp is reverted, GREEN with it. (The strdup'd-then-NUL'd fmt_url buffer is
// only strlen(url)+1 bytes; advancing past that walked off the end.)
CTEST(url, f_get_part_no_overread)
{
    char* h = url_get_hostname("http://host.com/path");
    free(h);
    char* p = url_get_path("http://h/p");
    free(p);
    char* s = url_get_search("http://host.com/path");
    free(s);
}

// Concrete follow-on. url_get_port on an empty-host URL: url_get_hostname
// returns ":8080" (non-NULL) but url_get_host returns NULL (nothing before the
// ':'), so the old strlen(host) dereferenced NULL. Fork-repro: RED = child
// SIGSEGV pre-fix, GREEN = the !host guard returns NULL (and frees hostname).
CTEST(url, f_url_get_port_empty_host)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        char* p = url_get_port("http://:8080/x");
        free(p);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQUAL(0, WEXITSTATUS(status));
}

//------------------------------------------------------------------------------
// Accessors on the fully-populated URL (every getter returns non-NULL safely)
//------------------------------------------------------------------------------

CTEST(url, accessors_full)
{
    char* protocol = url_get_protocol(FULL_URL);
    char* auth     = url_get_auth(FULL_URL);
    char* hostname = url_get_hostname(FULL_URL);
    char* host     = url_get_host(FULL_URL);
    char* pathname = url_get_pathname(FULL_URL);
    char* path     = url_get_path(FULL_URL);
    char* search   = url_get_search(FULL_URL);
    char* query    = url_get_query(FULL_URL);
    char* hash     = url_get_hash(FULL_URL);
    char* port     = url_get_port(FULL_URL);

    ASSERT_STR("http", protocol);
    ASSERT_STR("user:pass", auth);
    ASSERT_STR("host.com:8080", hostname);
    ASSERT_STR("host.com", host);
    ASSERT_NOT_NULL(pathname);
    ASSERT_NOT_NULL(path);
    ASSERT_NOT_NULL(search);
    ASSERT_NOT_NULL(query);
    ASSERT_NOT_NULL(hash);
    ASSERT_STR("8080", port);

    free(protocol);
    free(auth);
    free(hostname);
    free(host);
    free(pathname);
    free(path);
    free(search);
    free(query);
    free(hash);
    free(port);
}

// Safe NULL-return paths: url_get_protocol/url_get_auth on a table-miss scheme
// return NULL with their result-checked early returns (no use-before-check).
CTEST(url, accessor_null_safe)
{
    char* p = url_get_protocol("zzz://host");
    ASSERT_NULL(p);

    char* a = url_get_auth("zzz://host");   // line 237-238: protocol NULL -> NULL
    ASSERT_NULL(a);
}

// CANDIDATE C-URL-2 (Mike-authorized HIGH fix, F-S2 model): url_get_hostname
// did strdup(protocol) BEFORE the !protocol guard, so an unknown scheme ->
// strdup(NULL) crash, cascading to EVERY accessor (they all call
// url_get_hostname). Fork-repro: on CURRENT the child SIGSEGVs (RED); after the
// guard-before-strdup move every accessor returns NULL and the child survives.
CTEST(url, f_c_url_2_unknown_scheme)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        const char* u = "zzz://host/p?q#h";
        if (url_get_hostname(u) != NULL) _exit(1);
        if (url_get_host(u)     != NULL) _exit(2);
        if (url_get_path(u)     != NULL) _exit(3);
        if (url_get_pathname(u) != NULL) _exit(4);
        if (url_get_search(u)   != NULL) _exit(5);
        if (url_get_query(u)    != NULL) _exit(6);
        if (url_get_hash(u)     != NULL) _exit(7);
        if (url_get_port(u)     != NULL) _exit(8);
        _exit(0);   // reached only without a crash + all accessors NULL
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));         // RED: crash on current url.c
    ASSERT_EQUAL(0, WEXITSTATUS(status));   // all accessors returned NULL
}

CTEST(url, get_path_unknown_scheme)
{
    // Post-C-URL-2 the accessors are SAFE on an unknown scheme: url_get_path
    // returns NULL via the !protocol||!hostname guard (url.c:304-305) -- now
    // live (was dead behind the C-URL-2 crash). Direct (non-fork) call so gcov
    // records the line (the fork-repro's child _exit skips the gcov flush).
    char* p = url_get_path("zzz://host/path");
    ASSERT_NULL(p);
}

//------------------------------------------------------------------------------
// url_free + the inspect/print helpers
//------------------------------------------------------------------------------

CTEST(url, free_null)
{
    url_free(NULL);   // line 428: NULL guard
}

CTEST(url, free_partial)
{
    // A partially-populated struct (some members NULL) frees cleanly.
    url_data_t* u = (url_data_t*)calloc(1, sizeof(url_data_t));
    u->protocol = strdup("http");
    u->host = strdup("host");
    // the rest stay NULL
    url_free(u);
}

CTEST(url, inspect)
{
    // url_data_inspect prints every member (populated + NULL branches of the
    // PRINT_MEMBER macro).
    char href[] = FULL_URL;
    url_data_t* u = url_parse(href);
    ASSERT_NOT_NULL(u);
    url_data_inspect(u);   // populated members
    url_free(u);

    url_data_t* partial = (url_data_t*)calloc(1, sizeof(url_data_t));
    url_data_inspect(partial);   // the (NULL) branch of PRINT_MEMBER
    free(partial);

    // url_inspect: now frees what it parses (was a by-construction leak). Under
    // valgrind this is RED pre-fix (parsed struct leaked), GREEN after.
    char href2[] = FULL_URL;
    url_inspect(href2);
}

//------------------------------------------------------------------------------

static void* watchdog_thread(void* arg)
{
    (void)arg;
    sleep((unsigned int)WATCHDOG_SECONDS);
    fprintf(stderr, "test_url: watchdog deadline exceeded — aborting\n");
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
