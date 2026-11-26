// test_svr_stoprelease_e2e.c -- the stop-time release of queued WebSocket
// messages, end to end: a real server on loopback, its loop thread, its one
// worker and vws_tcp_svr_stop(). Companion of test_svr_stoprelease.c, which
// holds the description of the defect and the socket-free cells.
//
// How the messages are made to be resident at stop, without a timing guess:
//
//   - the server has ONE worker;
//   - the application callback holds the first message on a semaphore, so the
//     worker cannot take another request;
//   - a client sends three messages. The loop thread reads all of them; the
//     second and third stay in the requests queue;
//   - the test waits until it SEES them resident, then calls
//     vws_tcp_svr_stop(). A helper thread releases the worker once the
//     requests queue has left the running state. The worker returns, finds the queue halting
//     and exits; the loop thread joins it and runs svr_shutdown(), which
//     releases the two resident blocks.
//
// The observer of the lost memory is valgrind:
//
//   make test_svr_stoprelease_e2e_memcheck
//
// fails on any lost block. Run plainly, the program checks that exactly one
// message reached the application and that messages were resident at stop.
//
// This program links the library as built (it does not #include server.c).
// Loopback only (127.0.0.1:8238). Bounded by a watchdog alarm.

#include "server.h"
#include "message.h"

#include <unistd.h>
#include <signal.h>

#define CTEST_MAIN
#include "ctest.h"
#include "common.h"

static cstr server_host = "127.0.0.1";
static int  server_port = 8238;
static cstr uri         = "ws://127.0.0.1:8238/websocket";

static const int WATCHDOG_SECONDS = 60;

static uv_sem_t   held;       // posted when the worker holds the first message
static uv_sem_t   release;    // posted to let the worker go on
static atomic_int delivered;  // messages that reached the application

// Runs in the one worker thread.
static void process(vws_svr* s, vws_cid_t cid, vws_msg* m, void* ctx)
{
    (void)s; (void)cid; (void)ctx;

    if (atomic_fetch_add(&delivered, 1) == 0)
    {
        uv_sem_post(&held);
        uv_sem_wait(&release);
    }

    vws_msg_free(m);
}

static void server_thread(void* arg)
{
    vws_svr* server = (vws_svr*)arg;
    vws_svr_run(server, server_host, server_port);
    vws_cleanup();
}

static int requests_state(vws_tcp_svr* server)
{
    uv_mutex_lock(&server->requests.mutex);
    int state = (int)server->requests.state;
    uv_mutex_unlock(&server->requests.mutex);

    return state;
}

// Lets the held worker go once stop has been called. The condition is the
// state of the REQUESTS QUEUE, not of the server: vws_tcp_svr_stop() writes
// the server's state one statement before the queue's, and a worker released
// in between would find the queue still running, take the second message and
// make this test fail for a reason that is not the defect. Once the queue is
// not running, queue_pop() gives the worker nothing more.
static void releaser_thread(void* arg)
{
    vws_tcp_svr* server = (vws_tcp_svr*)arg;

    while (requests_state(server) == VS_RUNNING)
    {
        vws_msleep(10);
    }

    uv_sem_post(&release);
}

static int resident_requests(vws_tcp_svr* server)
{
    uv_mutex_lock(&server->requests.mutex);
    int n = (int)server->requests.size;
    uv_mutex_unlock(&server->requests.mutex);

    return n;
}

CTEST(svr_stoprelease_e2e, ws_msgs_resident_at_stop)
{
    alarm(WATCHDOG_SECONDS);

    uv_sem_init(&held, 0);
    uv_sem_init(&release, 0);
    atomic_store(&delivered, 0);

    // One worker.
    vws_svr* server     = vws_svr_new(1, 0, 0);
    server->process_ws  = process;
    vws_tcp_svr* base   = (vws_tcp_svr*)server;

    uv_thread_t server_tid;
    uv_thread_create(&server_tid, server_thread, server);

    while (vws_tcp_svr_state(base) != VS_RUNNING)
    {
        vws_msleep(10);
    }

    // Client: three messages, no reply expected.
    vws_cnx* cnx = vws_cnx_new();
    ASSERT_TRUE(vws_connect(cnx, uri));

    unsigned char payload[115];
    memset(payload, 0x5A, sizeof(payload));

    for (int i = 0; i < 3; i++)
    {
        ASSERT_TRUE(vws_msg_send_binary(cnx, payload, sizeof(payload)) > 0);
    }

    // The worker holds the first; wait until the other two are SEEN resident.
    uv_sem_wait(&held);

    int resident = 0;
    for (int i = 0; i < 1000 && resident < 2; i++)
    {
        resident = resident_requests(base);

        if (resident < 2)
        {
            vws_msleep(10);
        }
    }

    ASSERT_EQUAL(2, resident);
    ASSERT_EQUAL(1, atomic_load(&delivered));

    // Stop with the two messages queued. stop() returns when the server has
    // halted; the helper releases the worker once the requests queue is no
    // longer running.
    uv_thread_t releaser_tid;
    uv_thread_create(&releaser_tid, releaser_thread, base);

    vws_tcp_svr_stop(base);

    uv_thread_join(&releaser_tid);
    uv_thread_join(&server_tid);

    // Only the first message ever reached the application; the two resident
    // ones were released by the server's stop code.
    ASSERT_EQUAL(1, atomic_load(&delivered));

    vws_disconnect(cnx);
    vws_cnx_free(cnx);
    vws_svr_free(server);

    uv_sem_destroy(&held);
    uv_sem_destroy(&release);

    alarm(0);
}

int main(int argc, const char* argv[])
{
    return ctest_main(argc, argv);
}
