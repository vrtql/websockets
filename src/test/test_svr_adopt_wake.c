// test_svr_adopt_wake.c — vsa e433e4a3: a failed adoption of the server's
// own outgoing peer must not cut the wake short.
//
// DEFECT (vws server.c before the fix): uv_thread() — the loop thread's wake
// handler — first writes every queued response, then walks the outgoing
// peers, then resumes paused reads, calls the loop callback (loop_cb) and
// arms the peer tick. The arm that adopts a dialed peer socket
// (peer->state == VWS_PEER_RECONNECTED) left the WHOLE FUNCTION with
// `return;` when the adoption failed, at four places: set-nonblocking,
// uv_tcp_init, uv_tcp_open, uv_read_start. Such a wake had already put its
// responses on the wire and then skipped everything after the peer walk,
// the loop callback included, until the next wake (the 200 ms peer tick).
// A client could therefore hold a response while the loop callback that
// belongs to the same wake had not run; a server above this one that
// applies a login in its loop callback then judged that client's next
// frames with the old state.
//
// FIX: the four arms end THAT PEER's turn only (`continue;`).
//
// HOW THE SCENE IS SET: one ws-level server with ONE outgoing peer whose
// dial function is a stub, a loop callback that counts its calls, and one
// ws client.
//   1. The loop thread is held inside the loop callback (wake N). The call
//      count at that moment is C.
//   2. While it is held, the stub's dial ends (per cell, below) and one
//      response is queued for the client.
//   3. The hold is released. The next wake writes the response and then
//      meets the peer.
//   4. The client sends a frame the instant it has the response. The WORKER
//      that processes that frame records the call count. The frame is read
//      by the loop thread in a later callback than the wake that wrote the
//      response, so: count == C  <=> no loop callback ran in that wake.
//
// COUNT: the cells assert count > C, not count == C + 1. The 200 ms peer
// tick keeps waking the loop thread while the peer is not connected, so on
// a slow run (a loaded machine, valgrind) a second wake can come before the
// client's frame is processed and the count is C + 2. A slow run can
// therefore let the first cell pass on a library without the fix; it can
// never make it fail on a library with the fix.
//
// CELLS (suite svr_adopt_wake):
//
//   adopt_fail_keeps_wake — THE GUARD. The stub returns a descriptor
//     number that is not open (BAD_FD), so vws_socket_set_nonblocking()
//     fails and the adoption arm gives up. Fixed lib: the count is above
//     C (the loop callback ran in the response's own wake) and the peer is
//     CLOSED with no descriptor (the peer is still failed and closed; only
//     the wake goes on). Unfixed lib: the count is C => this cell FAILS.
//
//   control_dial_failed — the stub returns -1: the dial failed, there is
//     no adoption. Count above C on fixed AND unfixed.
//
//   control_dial_pending — the stub does not return: the peer stays
//     PENDING. Count above C on fixed AND unfixed.
//
//   The two controls prove that the scene, the hold and the count are
//   sound where no adoption fails; they isolate a failure of the first
//   cell to the adoption arm.
//
// The peer's address is the server's own port: before it calls the dial
// function, the worker probes the address with a plain connect
// (svr_resolve) and gives up when nothing answers. The probe opens and
// closes one connection on the server; the stub itself dials nothing.
//
// Only the set-nonblocking arm can be reached from outside the library.
// The other three arms are the same statement at the same place in the
// same loop; they are covered by reading, not by a cell.
//
// SELF-BOUNDING: every cell arms a watchdog alarm; every wait has a
// deadline; the stub and the hold leave at the end of the cell.
//
// PORT: 10199 (VWS_TEST_PORT overrides).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <time.h>

#include "server.h"
#include "message.h"

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

// A descriptor number that is never open in this process
#define BAD_FD 999999

static cstr host = "127.0.0.1";
static int  port = 10199;
static char uri[128];

typedef enum
{
    DIAL_BAD_FD,    // the dial "succeeds" with a descriptor that is not open
    DIAL_FAILED,    // the dial fails (-1)
    DIAL_PENDING    // the dial does not end
} dial_mode_t;

static dial_mode_t dial_mode = DIAL_BAD_FD;

static atomic_int loop_calls    = 0;
static atomic_int stall_request = 0;
static atomic_int stalled       = 0;
static atomic_int quit          = 0;
static atomic_int stub_entered  = 0;
static atomic_int hello_seen    = 0;
static atomic_int frame_seen    = 0;
static atomic_int frame_calls   = -1;
static sem_t      stall_release;
static sem_t      dial_go;
static vws_cid_t  client_cid;

// What one scene observed
typedef struct
{
    int  calls_at_stall;      // C
    int  calls_at_frame;      // count when the client's frame was processed
    int  state_at_release;    // peer state when the hold was released
    int  state_after_frame;   // peer state right after the frame
    long fd_after_frame;      // peer descriptor right after the frame
} scene_t;

//------------------------------------------------------------------------
// Fixture
//------------------------------------------------------------------------

static bool sem_wait_ms(sem_t* s, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;

    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    return sem_timedwait(s, &ts) == 0;
}

// Loop thread: once per wake that reaches the loop callback
static void on_loop(void* data)
{
    (void)data;

    atomic_fetch_add(&loop_calls, 1);

    if (atomic_load(&stall_request) == 1)
    {
        atomic_store(&stalled, 1);

        // Bounded: at most 5 s
        sem_wait_ms(&stall_release, 5000);

        atomic_store(&stalled, 0);
    }
}

// Worker thread: the dial of the outgoing peer
static vws_sockfd_t stub_connect(vws_peer* p, void* x)
{
    (void)p;
    (void)x;

    atomic_fetch_add(&stub_entered, 1);

    // Wait for the cell, in bounded slices; leave at the end of the cell
    while (atomic_load(&quit) == 0)
    {
        if (sem_wait_ms(&dial_go, 100) == true)
        {
            if (dial_mode == DIAL_BAD_FD)
            {
                return BAD_FD;
            }

            return -1;
        }
    }

    return -1;
}

// Worker thread: a message from the client
static void on_ws(vws_svr* s, vws_cid_t cid, vws_msg* m, void* ctx)
{
    (void)s;
    (void)ctx;

    if (m->data->size >= 5 && memcmp(m->data->data, "HELLO", 5) == 0)
    {
        client_cid = cid;
        atomic_store(&hello_seen, 1);
    }
    else if (m->data->size >= 5 && memcmp(m->data->data, "FRAME", 5) == 0)
    {
        atomic_store(&frame_calls, atomic_load(&loop_calls));
        atomic_store(&frame_seen, 1);
    }

    vws_msg_free(m);
}

static void server_thread(void* arg)
{
    vws_svr* server = (vws_svr*)arg;

    ((vws_tcp_svr*)server)->trace = vws.tracelevel;

    // See the file header: the peer's address is this server's own port
    vws_tcp_svr_peer_add( (vws_tcp_svr*)server,
                          host,
                          port,
                          stub_connect,
                          NULL );

    vws_svr_run(server, host, port);
    vws_cleanup();
}

// Bounded poll of a flag
static bool wait_flag(atomic_int* f, int want, int deadline_ms)
{
    uint64_t start = vws_now_ms();

    while ((vws_now_ms() - start) < (uint64_t)deadline_ms)
    {
        if (atomic_load(f) == want)
        {
            return true;
        }

        vws_msleep(1);
    }

    return false;
}

// Bounded poll of the peer's state
static bool wait_peer_state(vws_peer* peer, int want, int deadline_ms)
{
    uint64_t start = vws_now_ms();

    while ((vws_now_ms() - start) < (uint64_t)deadline_ms)
    {
        if ((int)peer->state == want)
        {
            return true;
        }

        vws_msleep(1);
    }

    return false;
}

// Runs one scene. Returns NULL when the scene was set and observed, else
// the step that could not be reached. The server is always stopped and
// freed before the return.
static cstr run_scene(dial_mode_t mode, scene_t* out)
{
    cstr failed = NULL;

    dial_mode = mode;

    atomic_store(&loop_calls, 0);
    atomic_store(&stall_request, 0);
    atomic_store(&stalled, 0);
    atomic_store(&quit, 0);
    atomic_store(&stub_entered, 0);
    atomic_store(&hello_seen, 0);
    atomic_store(&frame_seen, 0);
    atomic_store(&frame_calls, -1);

    sem_init(&stall_release, 0, 0);
    sem_init(&dial_go, 0, 0);

    vws_svr* a                 = vws_svr_new(3, 0, 0);
    a->process_ws              = on_ws;
    ((vws_tcp_svr*)a)->loop_cb = on_loop;

    uv_thread_t tid;
    uv_thread_create(&tid, server_thread, a);

    vws_cnx* c = vws_cnx_new();

    //> Server up (bounded 5 s), client in

    bool up = false;

    for (int i = 0; i < 500; i++)
    {
        if (vws_tcp_svr_is_running((vws_tcp_svr*)a) == true)
        {
            up = true;
            break;
        }

        vws_msleep(10);
    }

    if (up == false)
    {
        failed = "server did not start";
        goto done;
    }

    if (vws_connect(c, uri) == false)
    {
        failed = "client could not connect";
        goto done;
    }

    vws_socket_set_timeout((vws_socket*)c, 3);
    vws_msg_send_text(c, "HELLO");

    if (wait_flag(&hello_seen, 1, 3000) == false)
    {
        failed = "server did not see the client's first message";
        goto done;
    }

    //> The peer's dial is in the stub

    if (wait_flag(&stub_entered, 1, 3000) == false)
    {
        failed = "the peer's dial never reached the stub";
        goto done;
    }

    //> Hold the loop thread inside the loop callback (wake N)

    atomic_store(&stall_request, 1);

    if (wait_flag(&stalled, 1, 3000) == false)
    {
        failed = "the loop thread was not held";
        goto done;
    }

    atomic_store(&stall_request, 0);

    out->calls_at_stall = atomic_load(&loop_calls);

    vws_tcp_svr* tcp = (vws_tcp_svr*)a;
    vws_peer* peer   = (vws_peer*)tcp->peers->array[0].value.data;

    //> While it is held: the dial ends, and a response is queued

    if (mode == DIAL_BAD_FD)
    {
        sem_post(&dial_go);

        if (wait_peer_state(peer, VWS_PEER_RECONNECTED, 2000) == false)
        {
            failed = "the peer did not reach RECONNECTED";
            goto done;
        }
    }
    else if (mode == DIAL_FAILED)
    {
        sem_post(&dial_go);

        if (wait_peer_state(peer, VWS_PEER_CLOSED, 2000) == false)
        {
            failed = "the peer did not reach CLOSED";
            goto done;
        }
    }

    out->state_at_release = (int)peer->state;

    {
        vws_msg* reply = vws_msg_new();
        reply->opcode  = TEXT_FRAME;
        vws_buffer_append(reply->data, (ucstr)"REPLY", 5);

        // send() takes ownership of reply
        a->send(a, client_cid, reply, NULL);
    }

    // Time for the response to be queued and the wake requests to be in
    vws_msleep(20);

    //> Release: wake N+1 writes the response, then meets the peer

    sem_post(&stall_release);

    vws_msg* got = vws_msg_recv(c);

    if (got == NULL)
    {
        failed = "the client did not receive the response";
        goto done;
    }

    vws_msg_free(got);

    // The client answers at once
    vws_msg_send_text(c, "FRAME");

    if (wait_flag(&frame_seen, 1, 3000) == false)
    {
        failed = "the client's frame was not processed";
        goto done;
    }

    out->calls_at_frame    = atomic_load(&frame_calls);
    out->state_after_frame = (int)peer->state;
    out->fd_after_frame    = (long)peer->sockfd;

done:

    atomic_store(&quit, 1);
    sem_post(&stall_release);

    vws_disconnect(c);
    vws_cnx_free(c);

    vws_tcp_svr_stop((vws_tcp_svr*)a);
    uv_thread_join(&tid);
    vws_svr_free(a);

    sem_destroy(&stall_release);
    sem_destroy(&dial_go);

    return failed;
}

//------------------------------------------------------------------------
// Cells
//------------------------------------------------------------------------

// THE GUARD: fails on the library without the fix. See file header.
CTEST(svr_adopt_wake, adopt_fail_keeps_wake)
{
    alarm(60);

    scene_t s;
    memset(&s, 0, sizeof(s));

    cstr failed = run_scene(DIAL_BAD_FD, &s);

    if (failed != NULL)
    {
        printf("scene not set: %s\n", failed);
    }

    ASSERT_TRUE(failed == NULL);

    printf( "adopt_fail_keeps_wake: calls at hold=%d, at frame=%d, "
            "peer state at release=%d, after frame=%d, fd=%ld\n",
            s.calls_at_stall, s.calls_at_frame, s.state_at_release,
            s.state_after_frame, s.fd_after_frame );

    // The scene really was the failed adoption: the dial had handed over
    // a descriptor, and the peer is closed again with none
    ASSERT_EQUAL(VWS_PEER_RECONNECTED, s.state_at_release);
    ASSERT_EQUAL(VWS_PEER_CLOSED, s.state_after_frame);
    ASSERT_TRUE(s.fd_after_frame == (long)VWS_INVALID_SOCKET);

    // The wake that wrote the response also ran the loop callback
    // (greater-than, not +1: see COUNT in the file header)
    ASSERT_TRUE(s.calls_at_frame > s.calls_at_stall);

    alarm(0);
}

// CONTROL — must pass with and without the fix: no adoption is attempted.
CTEST(svr_adopt_wake, control_dial_failed)
{
    alarm(60);

    scene_t s;
    memset(&s, 0, sizeof(s));

    cstr failed = run_scene(DIAL_FAILED, &s);

    if (failed != NULL)
    {
        printf("scene not set: %s\n", failed);
    }

    ASSERT_TRUE(failed == NULL);

    printf( "control_dial_failed: calls at hold=%d, at frame=%d, "
            "peer state at release=%d, after frame=%d\n",
            s.calls_at_stall, s.calls_at_frame, s.state_at_release,
            s.state_after_frame );

    ASSERT_EQUAL(VWS_PEER_CLOSED, s.state_at_release);
    ASSERT_TRUE(s.calls_at_frame > s.calls_at_stall);

    alarm(0);
}

// CONTROL — must pass with and without the fix: the dial has not ended.
CTEST(svr_adopt_wake, control_dial_pending)
{
    alarm(60);

    scene_t s;
    memset(&s, 0, sizeof(s));

    cstr failed = run_scene(DIAL_PENDING, &s);

    if (failed != NULL)
    {
        printf("scene not set: %s\n", failed);
    }

    ASSERT_TRUE(failed == NULL);

    printf( "control_dial_pending: calls at hold=%d, at frame=%d, "
            "peer state at release=%d, after frame=%d\n",
            s.calls_at_stall, s.calls_at_frame, s.state_at_release,
            s.state_after_frame );

    ASSERT_EQUAL(VWS_PEER_PENDING, s.state_at_release);
    ASSERT_TRUE(s.calls_at_frame > s.calls_at_stall);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    cstr p = getenv("VWS_TEST_PORT");

    if (p != NULL)
    {
        port = atoi(p);
    }

    if (port <= 0)
    {
        fprintf(stderr, "Invalid port %s\n", p);
        return 2;
    }

    snprintf(uri, sizeof(uri), "ws://%s:%d/websocket", host, port);

    return ctest_main(argc, argv);
}
