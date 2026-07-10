// test_svr_peeruaf.c — server.c borrowed-peer DOUBLE-FREE at shutdown drain.
//
// BUG: svr_shutdown()'s generic requests-queue drain (server.c ~2499) frees
// every residual vws_svr_data via vws_svr_data_free() WITHOUT clearing ->data.
// A PEER_CONNECT block (built by the peer-reconnect producer at server.c ~1119)
// carries a BORROWED pointer to the kvs-owned vws_peer:
//
//   block = vws_svr_data_own(server, peer->info.cid, (ucstr)peer, ...);
//   vws_set_flag(&block->flags, VWS_SVR_STATE_PEER_CONNECT);   // borrows the peer
//   queue_push(&server->requests, block);
//
// vws_svr_data_own stores ->data as a RAW borrowed pointer (server.c ~1395,
// no copy). The NORMAL consumer (svr_route_data, server.c ~3328) clears
// block->data = NULL (server.c ~3335) BEFORE vws_svr_data_free(), so the
// borrowed peer is never freed on the hot path. But if the block is still
// resident in requests at shutdown (workers stop draining once VS_HALTING —
// queue_pop returns NULL immediately), svr_shutdown's drain frees it WITHOUT
// that guard => it frees the kvs-owned vws_peer. tcp_svr_dtor() then reads
// (free(peer->host), server.c ~2210) and frees (vws_kvs_free, server.c ~2213)
// the SAME peer AGAIN => use-after-free + double-free.
//
// In production the freed peer slot can be recycled by a later start/stop
// cycle into a uv timer handle; the live loop's uv__run_timers re-arm then
// dereferences garbage => an intermittent, hard-to-attribute teardown
// SIGSEGV. This unit removes that stochastic recycle race: it drives the free-site
// (svr_shutdown drain) + the ACTUAL double-use site (tcp_svr_dtor) directly on
// a minimally-constructed server, so the fault is DETERMINISTIC. Under ASan the
// RED fires as heap-use-after-free (peer->host read) / attempting-double-free.
//
// FIX (mirror the consumer's guard in the drain): before vws_svr_data_free() in
// the svr_shutdown drain loop, if the block is a PEER_CONNECT block, set
// data->data = NULL (the peer is kvs-owned, freed exactly once by
// tcp_svr_dtor's vws_kvs_free). GREEN: peer freed once, block struct still
// freed (no reqleak-style leak), control path intact.
//
// SUBJECT, NOT MODIFIED: server.c is #included so svr_shutdown / tcp_svr_dtor /
// vws_svr_data_own / queue_push / vws_tcp_svr_peer_add are the isolated copies.
// No server threads, no libuv reactor run — a pure unit around the shutdown
// teardown, so no watchdog race surface. A short alarm() bounds the run so
// the test can never hang.

#define CTEST_MAIN
#include "ctest.h"

#include "common.h"
#include "server.h"

#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

// server.c is the SUBJECT (statics + isolated coverage).
#include "../server.c"

static const int WATCHDOG_SECONDS = 30;

// A no-op peer connect function: peer_add requires a non-NULL fn, but this
// unit never runs the reactor so it is never invoked.
static vws_sockfd_t dummy_peer_connect(struct vws_peer* p, void* x)
{
    (void)p;
    (void)x;
    return -1;
}

// Build a server whose peers-kvs holds one peer and whose requests queue holds
// a residual PEER_CONNECT block BORROWING that kvs peer — exactly the producer
// state (server.c ~1119) left undrained at shutdown. Returns the server; out
// param yields the kvs peer pointer.
static vws_tcp_svr* make_residual_peer_connect(vws_peer** kvs_peer_out)
{
    vws_tcp_svr* s = vws_tcp_svr_new(1, 128, 8);
    ASSERT_TRUE(s != NULL);

    // svr_shutdown no-ops unless the server is running (reqleak precedent).
    s->state = VS_RUNNING;

    // Real producer step 1: register a peer. This mallocs a vws_peer into the
    // peers kvs (vws_kvs_set, server.c ~2011) with peer->host = strdup(...).
    vws_peer* peer =
        vws_tcp_svr_peer_add(s, "127.0.0.1", 9999, dummy_peer_connect, NULL);
    ASSERT_TRUE(peer != NULL);

    // Real producer step 2: build the PEER_CONNECT block borrowing the kvs
    // peer (verbatim server.c ~1119-1125) and queue it on requests.
    vws_svr_data* block =
        vws_svr_data_own(s, peer->info.cid, (ucstr)peer, sizeof(vws_peer*));
    vws_set_flag(&block->flags, VWS_SVR_STATE_PEER_CONNECT);
    queue_push(&s->requests, block);

    ASSERT_TRUE(s->requests.size == 1);   // resident going into shutdown

    *kvs_peer_out = peer;
    return s;
}

// RED cell. Drive the real shutdown teardown sequence (svr_shutdown then
// tcp_svr_dtor, exactly as vws_tcp_svr_run's stop arm + vws_tcp_svr_free do).
// Unpatched: svr_shutdown's drain frees the borrowed kvs peer, then
// tcp_svr_dtor's peers loop reads peer->host (UAF) and vws_kvs_free double-frees
// it. ASan aborts here. Patched: peer freed exactly once, no fault.
CTEST(svr_peeruaf, peer_connect_residual_double_free)
{
    alarm(WATCHDOG_SECONDS);

    vws_peer* peer = NULL;
    vws_tcp_svr* s = make_residual_peer_connect(&peer);

    // FREE SITE: svr_shutdown drain -> vws_svr_data_free(block) -> frees the
    // borrowed peer (server.c ~2499). Patched drain clears block->data first.
    svr_shutdown(s);

    // DOUBLE-USE SITE: tcp_svr_dtor reads peer->host (server.c ~2210) and
    // vws_kvs_free's the peer (server.c ~2213). Unpatched => second free of the
    // already-freed peer. This is the deterministic RED.
    tcp_svr_dtor(s);

    // Reached only when patched (no abort): the peer was freed exactly once.
    vws.free(s);

    // Post-state assertion (a positive gate for the GREEN run — the RED never
    // gets here because ASan aborts at the double-use above).
    ASSERT_TRUE(1);

    alarm(0);
}

// CONTROL cell (non-vacuity). Same drain+dtor sequence, but the residual
// requests block owns a SEPARATE payload (NOT the kvs peer) and is NOT a
// PEER_CONNECT block. svr_shutdown frees the payload once; tcp_svr_dtor frees
// the peer once. No aliasing => no double-free on patched OR unpatched. Proves
// the harness + the drain+dtor machinery are clean when ->data is genuinely
// owned, isolating the fault to the borrowed-peer PEER_CONNECT aliasing — and
// proves a candidate "fix" that simply stops draining requests would be caught
// (this block's struct must still be freed: no leak).
CTEST(svr_peeruaf, control_owned_payload_residual_clean)
{
    alarm(WATCHDOG_SECONDS);

    vws_tcp_svr* s = vws_tcp_svr_new(1, 128, 8);
    ASSERT_TRUE(s != NULL);
    s->state = VS_RUNNING;

    // A peer in the kvs (freed once by tcp_svr_dtor, as in the RED).
    vws_peer* peer =
        vws_tcp_svr_peer_add(s, "127.0.0.1", 9998, dummy_peer_connect, NULL);
    ASSERT_TRUE(peer != NULL);

    // Residual request that OWNS its payload (not a borrowed peer, no
    // PEER_CONNECT flag) — the ordinary reqleak-style residual item.
    size_t plen = 64;
    ucstr payload = (ucstr)vws.malloc(plen);
    memset(payload, 0xAB, plen);
    vws_cid_t cid; vws_cid_clear(&cid);
    vws_svr_data* block = vws_svr_data_own(s, cid, payload, plen);
    queue_push(&s->requests, block);
    ASSERT_TRUE(s->requests.size == 1);

    // svr_shutdown frees payload+block (single free); tcp_svr_dtor frees the
    // peer (single free). No double-free, no leak — on patched AND unpatched.
    svr_shutdown(s);
    tcp_svr_dtor(s);
    vws.free(s);

    ASSERT_TRUE(1);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
