#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#include "server.h"
#include "websocket.h"

//------------------------------------------------------------------------------
// Monotonic clock
//------------------------------------------------------------------------------

// Test-only override for vws_now_ms(); NULL in production (see websocket.h).
uint64_t (*vws_now_ms_fn)(void) = NULL;

// Monotonic milliseconds for all heartbeat/liveness deadlines. uv_hrtime() is a
// monotonic nanosecond counter -- it never moves backward and is unaffected by
// wall-clock/NTP steps -- so a clock adjustment cannot spuriously trip a
// deadline (a false peer-down) or defer detection of a real one. Dividing by
// 1e6 yields ms, matching the *_ms deadline units (so the deadline math no
// longer multiplies a seconds delta by 1000).
uint64_t vws_now_ms(void)
{
    if (vws_now_ms_fn != NULL)
    {
        return vws_now_ms_fn();
    }

    return uv_hrtime() / 1000000;
}

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

// Enable OS-level dead-peer detection on an accepted/adopted connection.
// Without this an accepted fd has SO_KEEPALIVE=0, so a dead or half-open peer
// (network partition / power-off / cable pull -- no FIN) is only detected
// after the ~2h OS default, stranding the connection slot and any queued
// state. uv_tcp_keepalive turns on SO_KEEPALIVE with an idle interval; a
// platform-specific socket option additionally bounds how long a stalled or
// unacknowledged send is tolerated before the connection is dropped, so a
// dead peer is detected in seconds rather than the ~2h default. Best-effort:
// failures are non-fatal (the connection still works, just without the
// accelerated detection).
static void vws_enable_keepalive(vws_tcp_svr* server, uv_tcp_t* handle)
{
    // SO_KEEPALIVE on with a settable idle interval (keepalive_idle_sec,
    // default 30). uv_tcp_keepalive is portable across platforms.
    uv_tcp_keepalive(handle, 1, server->keepalive_idle_sec);

    // The fast-abort bound (keepalive_user_timeout_ms, default 20000) is
    // applied via the platform's native option. Each branch is compile-guarded
    // so the file builds everywhere; a platform providing none of them keeps
    // SO_KEEPALIVE alone.
    uv_os_fd_t fd;
    if (uv_fileno((uv_handle_t*)handle, &fd) != 0)
    {
        return;
    }

    int sock = (int)fd;
    int ms   = server->keepalive_user_timeout_ms;

#if defined(TCP_USER_TIMEOUT)
    // Linux: bound total unacknowledged in-flight time directly (ms).
    setsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, sizeof(ms));
#elif defined(TCP_KEEPALIVE_ABORT_THRESHOLD)
    // Illumos / Solaris: keepalive abort threshold in ms -- bounds how long
    // unanswered keepalive probes are tolerated before the connection is
    // aborted. This is the keepalive (idle) path: it detects a dead peer after
    // the connection goes idle. It does NOT bound a stalled ACTIVE send (data
    // unacked mid-transfer) -- on illumos that is the separate retransmit-abort
    // path. Linux TCP_USER_TIMEOUT covers both idle and active-send in one
    // option; here only the idle case is bounded per-socket.
    // Checked BEFORE the keepalive-probe fallback below: illumos defines both
    // TCP_KEEPINTVL/CNT and this, and this is the stronger, intended option.
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPALIVE_ABORT_THRESHOLD, &ms, sizeof(ms));
#elif defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    // BSD / macOS: no single total-timeout option; approximate it as a probe
    // interval x probe count summing to keepalive_user_timeout_ms. (Detects a
    // dead peer only while idle, not a stalled active send -- weaker, so it is
    // the last resort.)
    int intvl = 5;                                  // seconds between probes
    int cnt   = ms / (intvl * 1000);
    if (cnt < 1)
    {
        cnt = 1;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
#else
    (void)sock;
    (void)ms;
#endif
}

void vws_cid_clear(vws_cid_t* cid)
{
    cid->key   = -1;
    cid->flags = 0;
    cid->data  = NULL;
    cid->plane = 0;
    memset(&cid->addr, 0, sizeof(struct sockaddr_storage));
}

bool vws_cid_valid(vws_cid_t* cid)
{
    return cid->key >= 0;
}

/**
 * @defgroup AddressPool
 */

// A key packs the slot index (low 32 bits) with the slot's reuse generation
// (bits 32..62; bit 63 stays clear so vws_cid_valid's key >= 0 contract
// holds). The slot index alone is NOT an identity: the cyclic first-fit scan
// reissues a freed index while a consumer (e.g. a deferred connection
// teardown holding a cid) may still hold the old key, and an index-only key
// then resolves -- or frees -- the slot's NEW resident. The generation
// advances on free, so a stale key mismatches and resolves nothing.

#define POOL_GEN_MASK 0x7FFFFFFFu

static int64_t pool_key(uint32_t index, uint32_t gen)
{
    return (int64_t)(((uint64_t)(gen & POOL_GEN_MASK) << 32) | index);
}

static uint32_t pool_key_index(int64_t key)
{
    return (uint32_t)(key & 0xFFFFFFFFu);
}

static uint32_t pool_key_gen(int64_t key)
{
    return (uint32_t)((uint64_t)key >> 32) & POOL_GEN_MASK;
}

address_pool* address_pool_new(int initial_size, int growth_factor)
{
    address_pool* pool    = (address_pool*)malloc(sizeof(address_pool));
    pool->slots           = (uintptr_t*)calloc(initial_size, sizeof(uintptr_t));
    pool->generations     = (uint32_t*)calloc(initial_size, sizeof(uint32_t));
    pool->capacity        = initial_size;
    pool->count           = 0;
    pool->last_used_index = 0;
    pool->growth_factor   = growth_factor;

    return pool;
}

void address_pool_free(address_pool** pool)
{
    if (*pool != NULL)
    {
        free((*pool)->slots);
        free((*pool)->generations);
        free(*pool);
        (*pool) = NULL;
    }
}

void address_pool_resize(address_pool *pool)
{
    int new_capacity     = pool->capacity * pool->growth_factor;
    uintptr_t* new_slots = (uintptr_t *)calloc(new_capacity, sizeof(uintptr_t));
    uint32_t* new_gens   = (uint32_t *)calloc(new_capacity, sizeof(uint32_t));
    memcpy(new_slots, pool->slots, pool->capacity * sizeof(uintptr_t));
    memcpy(new_gens, pool->generations, pool->capacity * sizeof(uint32_t));

    free(pool->slots);
    free(pool->generations);

    pool->slots       = new_slots;
    pool->generations = new_gens;
    pool->capacity    = new_capacity;
}

int64_t address_pool_set(address_pool* pool, uintptr_t address)
{
    if (pool->count == pool->capacity)
    {
        address_pool_resize(pool);
    }

    while (pool->slots[pool->last_used_index] != 0)
    {
        pool->last_used_index = (pool->last_used_index + 1) % pool->capacity;
    }

    // Placeholder for actual address or ID
    pool->slots[pool->last_used_index] = address;
    pool->count++;

    uint32_t allocated_index = pool->last_used_index;
    pool->last_used_index    = (pool->last_used_index + 1) % pool->capacity;

    return pool_key(allocated_index, pool->generations[allocated_index]);
}

uintptr_t address_pool_get(address_pool* pool, int64_t i)
{
    if (i < 0)
    {
        return 0;
    }

    uint32_t index = pool_key_index(i);

    if (index >= pool->capacity || pool->slots[index] == 0)
    {
        return 0;
    }

    if ((pool->generations[index] & POOL_GEN_MASK) != pool_key_gen(i))
    {
        return 0;
    }

    return pool->slots[index];
}

void address_pool_remove(address_pool* pool, int64_t i)
{
    if (i < 0)
    {
        return;
    }

    uint32_t index = pool_key_index(i);

    if (index >= pool->capacity)
    {
        return;
    }

    if ((pool->generations[index] & POOL_GEN_MASK) != pool_key_gen(i))
    {
        return;
    }

    if (pool->slots[index] != 0)
    {
        pool->slots[index] = 0;
        pool->count--;

        // The freed slot's next resident gets a new identity; any key issued
        // before this free now mismatches and resolves nothing.
        pool->generations[index]++;
    }
}

/**
 * @defgroup ThreadFunctions
 *
 * @brief Program thread organization
 *
 * There are two thread functions: the network thread (main thread) and work
 * pool threads.
 */

/**
 * @brief UV callback executed in response to (server->wakeup) async signal.
 *
 * This function handles asynchronous events in the main thread, specifically
 * managing outgoing network I/O from worker threads back to clients. It runs
 * within the main UV loop, within the main thread, also referred to as the
 * networking thread.
 *
 * Worker threads pass data to it through a queue. The data is in the form of
 * vws_svr_data instances. When a worker sends a vws_svr_data instance, it adds
 * it to the queue (server->responses) and notifies (wakes up) the main UV loop
 * (uv_run() in vrtql_tcp_svr_run()) by calling uv_async_send(server->wakeup)
 * which in turn calls uv_thread() to check the server->responses queue. The
 * uv_thread() function unloads all data in the queue and sends the data out to
 * each respective client. It then returns control back to the main UV loop
 * which resumes polling the network connections (blocking if there is no
 * activity).
 *
 * @param handle A pointer to the uv_async_t handle that triggered the callback.
 *
 * @ingroup ThreadFunctions
 */
static void uv_thread(uv_async_t* handle);

/**
 * @brief The entry point for a worker thread.
 *
 * This function implements the worker thread pool. It is what each worker
 * thread runs. It loops continuously, handling incoming data from clients,
 * processing them and returning data back to them via the uv_thread(). It
 * processes data by taking data (requests) from the server->requests queue,
 * dispatching them to server->process(data) for processing, which in turn
 * generates data (responses), sending them back to the client by putting them
 * on the server->responses queue (processed by uv_thread()).
 *
 * @param arg A void pointer, typically to the server object,
 *        used to pass data into the thread.
 *
 * @ingroup ThreadFunctions
 */
static void worker_thread(void* arg);

/**
 * @defgroup ServerFunctions
 *
 * @brief Functions that support server operation
 *
 */

/**
 * @brief Server instance constructor
 *
 * Constructs a new server instance. This takes a new, empty vws_tcp_svr instance
 * and initializes all of its members. It is used by derived structs as well
 * (vrtql_msg_svr) to construct the base struct.
 *
 * @param server The server instance to be initialized
 * @return The initialized server instance
 *
 * @ingroup ServerFunctions
 */

static vws_tcp_svr* tcp_svr_ctor(vws_tcp_svr* s, int nt, int bl, int qs);

/**
 * @brief Server instance destructor
 *
 * Destructs an initialized server instance. This takes a vws_tcp_svr instance
 * and deallocates all of its members -- everything but the top-level
 * struct. This is used by derived structs as well (vrtql_msg_svr) to destruct
 * the base struct.
 *
 * @param server The server instance to be destructed
 *
 * @ingroup ServerFunctions
 */

static void tcp_svr_dtor(vws_tcp_svr* s);

/**
 * @brief Initiates the server shutdown process.
 *
 * This function is responsible for shutting down a vws_tcp_svr server instance.
 * It stops the server if it's currently running, performs necessary cleanup,
 * shuts down the libuv event loop, frees memory, and clears the connection map.
 * It signals all worker threads to stop processing new data and to finish any
 * requests they are currently processing.
 *
 * @param server The server that needs to be shutdown.
 *
 * @ingroup ServerFunctions
 */
static void svr_shutdown(vws_tcp_svr* server);

/**
 * @brief Handles the close event for a libuv handle.
 *
 * This function is called when a libuv handle is closed. It checks if the handle
 * has associated heap data stored in `handle->data` and generates a warning if
 * the resource was not properly freed.
 *
 * @param handle The libuv handle being closed.
 */
static void on_uv_close(uv_handle_t* handle);

/**
 * @brief Walks through libuv handles and attempts to close them.
 *
 * This function is used to walk through libuv handles and attempts to close each
 * handle. It is typically called during libuv shutdown to ensure that all handles
 * are properly closed. If a handle has not been closed, it will be closed and
 * the `on_uv_close()` function will be called.
 *
 * @param handle The libuv handle being walked.
 * @param arg    Optional argument passed to the function (not used in this case).
 */
static void on_uv_walk(uv_handle_t* handle, void* arg);
static void svr_on_close_discard(uv_handle_t* handle);

/**
 * @brief Callback for new client connection.
 *
 * This function is invoked when a new client successfully connects to the
 * server.
 *
 * @param server The server to which the client connected.
 * @param status The status of the connection.
 *
 * @ingroup ServerFunctions
 */
static void svr_on_connect(uv_stream_t* server, int status);

/**
 * @brief Callback for buffer reallocation.
 *
 * This function is invoked when a handle requires a buffer reallocation.
 *
 * @param handle The handle requiring reallocation.
 * @param size The size of the buffer to be allocated.
 * @param buf The buffer.
 *
 * @ingroup ServerFunctions
 */
static void svr_on_realloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);

/**
 * @brief Callback for handle closure.
 *
 * This function is invoked when a handle is closed.
 *
 * @param handle The handle that was closed.
 *
 * @ingroup ServerFunctions
 */
static void svr_on_close(uv_handle_t* handle);

/**
 * @brief Callback for handle closure.
 *
 * This function is invoked when a peer_timer handle is closed.
 *
 * @param handle The handle that was closed.
 *
 * @ingroup ServerFunctions
 */
static void svr_on_timer_close(uv_handle_t* handle);

/**
 * @brief Callback for reading data.
 *
 * This function is invoked when data is read from a client.
 *
 * @param client The client from which data was read.
 * @param size The number of bytes read.
 * @param buf The buffer containing the data.
 *
 * @ingroup ServerFunctions
 */
static void svr_on_read(uv_stream_t* client, ssize_t size, const uv_buf_t* buf);

/**
 * @brief Callback for write completion.
 *
 * This function is invoked when a write operation to a client completes.
 *
 * @param req The write request.
 * @param status The status of the write operation.
 *
 * @ingroup ServerFunctions
 */
static void svr_on_write_complete(uv_write_t* req, int status);

/**
 * @defgroup Connection Functions
 *
 * @brief Functions that support connection operation
 *
 * Connections always refer to client-side connections, on the server. They are
 * active connections established within the main UV loop.
 *
 * @ingroup ServerFunctions
 */

/**
 * @brief Creates a new server connection.
 *
 * @param s The server for the connection.
 * @param c The client for the connection.
 * @return A new server connection.
 *
 * @ingroup ServerFunctions
 */
static vws_svr_cnx* svr_cnx_new(vws_tcp_svr* s, uv_stream_t* c);

/**
 * @brief Frees a server connection.
 *
 * @param c The connection to be freed.
 *
 * @ingroup ServerFunctions
 */
static void svr_cnx_free(vws_svr_cnx* c);

/**
 * @brief Actively close a client connection
 *
 * @param c The connection
 */
static void svr_cnx_close(vws_tcp_svr* server, vws_cid_t c);

/**
 * @brief Callback for client connection.
 *
 * This function is triggered when a new client connection is established.  It
 * is responsible for processing any steps necessary at the start of a
 * connection.
 *
 * @param c The connection structure representing the client that has connected.
 *
 * @ingroup ServerFunctions
 */
static void svr_client_connect(vws_svr_cnx* c);

/**
 * @brief Callback for inetd initial connection.
 *
 * This function is triggered when the first client connection is
 * established. This callback is so that the main client socket can be collected
 * and identified for use in the application.
 *
 * @param c The connection structure representing the client that has connected.
 *
 * @ingroup ServerFunctions
 */
static void svr_client_inetd_connect(vws_svr_cnx* c);

/**
 * @brief Callback for client disconnection.
 *
 * This function is triggered when a client connection is terminated. It is
 * responsible for processing any cleanup or other steps necessary at the end of
 * a connection.
 *
 * @param c The connection
 *
 * @ingroup ServerFunctions
 */
static void svr_client_disconnect(vws_svr_cnx* c);

/**
 * @brief Callback for client read operations.
 *
 * This function is triggered when data is read from a client connection. It is
 * responsible for processing the received data.
 *
 * @param c The connection that sent the data.
 * @param size The size of the data that was read.
 * @param buf The buffer containing the data that was read.
 *
 * @ingroup ServerFunctions
 */
static void svr_client_read(vws_svr_cnx* c, ssize_t size, const uv_buf_t* buf);

/**
 * @brief Callback for processing client data in (ingress)
 *
 * This function processes data arriving from client to worker thread. It is
 * responsible for handling the actual computation or other work associated with
 * the data. This takes place in the context of worker_thread().
 *
 * @param data The incoming data from the client to process.
 *
 * @ingroup ServerFunctions
 */
static void svr_client_data_in(vws_svr_data* data, void* x);

/**
 * @brief Callback for processing client data in (ingress)
 *
 * This function is triggered to process data arriving from worker thread to be
 * send back to client. It is responsible for transferring the data from the
 * response argument (vws_svr_data) onto the wire (client socket via libuv),
 * actual computation or other work associated with the data. This takes place
 * in the context of uv_thread().
 *
 * @param data The outgoing data from worker to client
 *
 * @ingroup ServerFunctions
 */
static void svr_client_data_out(vws_svr_data* data, void* x);




/**
 * @defgroup WebSocketServerFunctions
 *
 * @brief Functions that support message server operation
 *
 */

/**
 * @brief WebSocket server constructor.
 *
 * @param server The WebSocket server instance.
 * @param nt The number of threads to run in the worker pool.
 * @param bl The connection backlog for listen(). If set to 0, it uses the
 *        default value (128).
 * @param qs The maximum queue size for requests and responses. If set to 0, it
 *        uses the default value (1024).
 */
static void ws_svr_ctor(vws_svr* server, int nt, int bl, int qs);

/**
 * @brief WebSocket server destructor.
 *
 * @param server The WebSocket server instance to free.
 */
static void ws_svr_dtor(vws_svr* server);

/**
 * @brief Callback for client connection.
 *
 * This function is triggered when a new client connection is established.  It
 * is responsible for processing any steps necessary at the start of a
 * connection.
 *
 * @param c The connection structure representing the client that has connected.
 *
 * @ingroup WebSocketServerFunctions
 */
static void ws_svr_client_connect(vws_svr_cnx* c);

/**
 * @brief Callback for client disconnection.
 *
 * This function is triggered when a client connection is terminated. It is
 * responsible for processing any cleanup or other steps necessary at the end of
 * a connection.
 *
 * @param c The connection
 *
 * @ingroup WebSocketServerFunctions
 */
static void ws_svr_client_disconnect(vws_svr_cnx* c);

/**
 * @brief Callback for client read operations.
 *
 * This function is triggered when data is read from a client connection. It is
 * responsible for processing the received data.
 *
 * @param c The connection that sent the data.
 * @param size The size of the data that was read.
 * @param buf The buffer containing the data that was read.
 *
 * @ingroup WebSocketServerFunctions
 */
static void ws_svr_client_read(vws_svr_cnx* c, ssize_t size, const uv_buf_t* buf);

/**
 * @brief Callback for processing client data in (ingress) for msg server
 *
 * This function processes data arriving from client to worker thread. It
 * collects data until there is a complete message. It passes message to
 * ws_svr_client_msg_in() for processing. This takes place in the context of
 * worker_thread().
 *
 * @param server The server instance
 * @param data The incoming data from the client to process.
 * @param x The user-defined context
 *
 * @ingroup WebSocketServerFunctions
 */
static void ws_svr_client_data_in(vws_svr_data* data, void* x);

/**
 * @brief Sends data from a server connection to a client WebSocket connection.
 *
 * @param server The server
 * @param c The connection index
 * @param buffer The data to send.
 * @param opcode The opcode for the WebSocket frame.
 */
static void ws_svr_client_data_out( vws_svr* server,
                                    vws_cid_t c,
                                    vws_buffer* buffer,
                                    unsigned char opcode);

/**
 * @brief Process a WebSocket frame received from a client.
 *
 * @param c The WebSocket connection.
 * @param f The incoming frame to process.
 */
static void ws_svr_process_frame(vws_cnx* c, vws_frame* f);

/**
 * @brief Callback for client message processing
 *
 * This function is triggered when a message is read from a client
 * connection. It is responsible for processing the message. This takes place in
 * the context of worker_thread().
 *
 * @param s The server
 * @param c The connection ID
 * @param m The message to process
 * @param x The user-defined context
 *
 * @ingroup WebSocketServerFunctions
 */
static void ws_svr_client_msg_in(vws_svr* s, vws_cid_t cid, vws_msg* m, void* x);

/**
 * @brief Callback for sending message to client. It takes a message as input,
 * serializes it to a binary WebSocket message and then sends it to the
 * uv_thread() to send back to client. This takes place in the context of
 * worker_thread() (only to be called within that context).
 *
 * This function sends a message back to a client.
 *
 * @param s The server
 * @param c The connection ID
 * @param m The message to send
 * @param x The user-defined context
 *
 * @ingroup WebSocketServerFunctions
 */
static void ws_svr_client_msg_out(vws_svr* s, vws_cid_t c, vws_msg* m, void* x);

/**
 * @brief Default WebSocket message processing function. This is mean to be
 * overriden by application to perform message processing, specifically by
 * assigning the vws_svr.process callback to the desired processing
 * handler. The default implementation simple drops (deletes) the incoming
 * message.
 *
 * @param s The server instance.
 * @param c The server connection.
 * @param m The incoming WebSocket message.
 * @param x The user-defined context
 *
 * @ingroup MessageServerFunctions
 */
static void ws_svr_client_process(vws_svr* s, vws_cid_t c, vws_msg* m, void* x);




/**
 * @defgroup MessageServerFunctions
 *
 * @brief Functions that support message server operation
 *
 */

/**
 * @brief Convert incoming WebSocket messages to VRTQL messages for processing.
 *
 * @param s The server instance
 * @param c The connection ID
 * @param m The incoming WebSocket message.
 * @param x The user-defined context
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_ws_msg_in(vws_svr* s, vws_cid_t c, vws_msg* m, void* x);

/**
 * @brief Callback function for handling incoming VRTQL messages from a client.
 *
 * @param s The server instance
 * @param c The connection ID
 * @param m The incoming VRTQL message.
 * @param x The user-defined context
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_msg_in(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x);

/**
 * @brief Callback function for sending VRTQL messages to a client.
 *
 * @param s The server instance
 * @param c The server connection.
 * @param m The outgoing VRTQL message.
 * @param x The user-defined context
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_msg_out(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x);

/**
 * @brief Callback function for sending VRTQL messages to a client. This is jut
 * like msg_svr_client_msg_out() but it does NOT deallocate msg. This is for
 * when you want to send multiple copies of the same message.
 *
 * @param s The server instance
 * @param c The server connection.
 * @param m The outgoing VRTQL message.
 * @param x The user-defined context
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_msg_dispatch(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x);

/**
 * @brief Default VRTQL message processing function.
 *
 * @param s The server instance
 * @param c The server connection.
 * @param m The incoming VRTQL message.
 * @param x The user-defined context
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_process(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x);




/**
 * @defgroup QueueGroup
 *
 * @brief Queue functions which bridge the network thread and workers
 *
 * The network thread and worker threads pass data via queues. These queues
 * contain vws_svr_data instances. There is a requests queue which passes
 * data from the network thread to workers for processing. There is a response
 * queue that passed data from worker queues to the network thread. When passed
 * from network thread to work, data take form of incoming data from the
 * client. When passed from the worker threads the the networking thread, data
 * take the form of outgoing data to be sent back to the client.
 *
 * Queues have built-in synchronization mechanisms that allow the data to be
 * safely passed between threads, as well as ways to gracefull put waiting
 * threads to sleep until data arrives for processing.
 */

/**
 * @brief Initializes a server queue.
 *
 * This function sets up the provided queue with the given capacity. It also
 * initializes the synchronization mechanisms associated with the queue.
 *
 * @param queue Pointer to the server queue to be initialized.
 * @param capacity The maximum capacity of the queue.
 * @param name The queue name
 *
 * @ingroup QueueGroup
 */
static void queue_init(vws_svr_queue* queue, int capacity, cstr name);

/**
 * @brief Destroys a server queue.
 *
 * This function cleans up the resources associated with the provided queue.
 * It also handles the synchronization mechanisms associated with the queue.
 *
 * @param queue Pointer to the server queue to be destroyed.
 *
 * @ingroup QueueGroup
 */
static void queue_destroy(vws_svr_queue* queue);

/**
 * @brief Pushes data to the server queue.
 *
 * This function adds data to the end of the provided queue. It also handles
 * the necessary synchronization to ensure thread safety.
 *
 * @param queue Pointer to the server queue.
 * @param data Data to be added to the queue.
 *
 * @ingroup QueueGroup
 */
static void queue_push(vws_svr_queue* queue, vws_svr_data* data);
static bool queue_try_push(vws_svr_queue* queue, vws_svr_data* data);
static void svr_resume_paused_reads(vws_tcp_svr* server);

/**
 * @brief Pops data from the server queue.
 *
 * This function removes and returns data from the front of the provided queue.
 * It also handles the necessary synchronization to ensure thread safety.
 *
 * @param queue Pointer to the server queue.
 * @return A data element from the front of the queue.
 *
 * @ingroup QueueGroup
 */
static vws_svr_data* queue_pop(vws_svr_queue* queue);

/**
 * @brief Checks if the server queue is empty.
 *
 * This function checks whether the provided queue is empty.
 * It also handles the necessary synchronization to ensure thread safety.
 *
 * @param queue Pointer to the server queue.
 * @return True if the queue is empty, false otherwise.
 *
 * @ingroup QueueGroup
 */
static bool queue_empty(vws_svr_queue* queue);

//------------------------------------------------------------------------------
// Peer timer
//------------------------------------------------------------------------------

void peer_timer_callback(uv_timer_t* handle)
{
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "peer_timer_callback(%p)", handle);
    }

    // Wakeup server. The timer repeats until uv_thread() observes every
    // peer CONNECTED and stops it, so peer retry never depends on a
    // one-shot re-arm racing the wakeup it requested.
    vws_tcp_svr* s = (vws_tcp_svr*)handle->data;

    // Defense-in-depth NULL guard. tcp_svr_dtor stops this repeating timer
    // before nulling data + uv_close, so the teardown uv_run drain can no
    // longer dispatch this callback with stale state. If any residual fire
    // slips through, NULL data means the server is already gone -> drop
    // harmlessly instead of dereferencing s->wakeup (which crashed
    // intermittently during server teardown).
    if (s == NULL)
    {
        return;
    }

    uv_async_send(s->wakeup);
}

void vws_tcp_svr_peer_timer(vws_tcp_svr* s)
{
    if (uv_is_active((uv_handle_t*)s->peer_timer))
    {
        // The repeating timer is already running
        return;
    }

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_peer_timer(%p)", s);
    }

    // Set data
    s->peer_timer->data = s;

    // Start the retry tick: first fire in 200ms, then every 200ms until
    // uv_thread() stops it. The previous one-shot guarded re-arm through
    // time_t-plus-float wall-clock math: float granularity past epoch 2^30
    // is 128 seconds, so the recorded timeout could land up to ~64s in the
    // future and the guard then refused every re-arm while no timer was
    // pending -- permanently extinguishing the peer retry chain.
    uv_timer_start(s->peer_timer, peer_timer_callback, 200, 200);
}

//------------------------------------------------------------------------------
// Threads
//------------------------------------------------------------------------------

void worker_thread(void* arg)
{
    vws_tcp_svr* server = (vws_tcp_svr*)arg;

    // Set thread tracing level to server.
    vws.tracelevel = server->trace;

    if (vws.tracelevel >= VT_THREAD)
    {
        vws.trace(VL_INFO, "worker_thread(): Starting");
    }

    vws_thread_ctx ctx;
    ctx.ctor      = server->worker_ctor;
    ctx.ctor_data = server->worker_ctor_data;
    ctx.dtor      = server->worker_dtor;
    ctx.data      = NULL;

    if (ctx.ctor != NULL)
    {
        ctx.data = ctx.ctor(ctx.ctor_data);
    }

    while (true)
    {
        //> Wait for arrival

        // This will put the thread to sleep on a condition variable until
        // something arrives in queue.
        vws_svr_data* request = queue_pop(&server->requests);

        // A slot just freed in the request
        // queue. If any connection's reads were paused by backpressure, wake
        // the reactor to run a resume pass (uv_read_start must run on the loop
        // thread). uv_async_send coalesces, so this is cheap even under load.
        if (atomic_load(&server->reads_paused) > 0)
        {
            uv_async_send(server->wakeup);
        }

        // If there's no request (null request), check the server's state
        if (request == NULL)
        {
            // If server is in halting state, return
            if (server->state == VS_HALTING)
            {
                if (vws.tracelevel >= VT_THREAD)
                {
                    vws.trace(VL_INFO, "worker_thread(): Exiting");
                }

                break;
            }
            else
            {
                // If not halting, skip to the next iteration of the loop
                continue;
            }
        }

        server->on_data_in(request, ctx.data);
    }

    if (ctx.dtor != NULL)
    {
        ctx.dtor(ctx.data);
    }

    // C-SVR-6: release this worker thread's thread-local error state before it
    // exits. vws.e.text is __thread storage; left set, its last strdup'd message
    // is definitely lost when the thread is joined. Touches only thread-local
    // state (not the global SSL ctx), so siblings are unaffected.
    vws_cleanup();
}

// Feed ANY close of a CONNECTED peer's connection back to the peer state
// machine BEFORE the close is requested. Only the read-EOF and pong-deadline
// arms did this; every other close of a peer link (the CLOSE-response arm,
// the write-queue-cap shed, peer_disconnect) freed the cnx and left the peer
// record CONNECTED with info.cnx dangling: the heartbeat sweep then read and
// wrote through the freed cnx (and could uv_close the freed handle), the
// reconnect branch never re-fired (the peer was never re-dialed until process
// restart), and the unrecoverable report never triggered. Matching by
// info.cid.key (the PEER flag lives on cnx->cid, not this copy) and marking
// CLOSED here — on the loop thread, before uv_close — closes every arm at
// once with the EOF arm's proven ordering: no window where the sweep can see
// CONNECTED alongside a closing handle.
static void svr_peer_close_feedback(vws_tcp_svr* server, vws_cid_t cid)
{
    for (size_t i = 0; i < server->peers->used; i++)
    {
        vws_peer* pr = (vws_peer*)server->peers->array[i].value.data;

        if (pr != NULL && pr->state == VWS_PEER_CONNECTED
            && pr->info.cid.key == cid.key)
        {
            pr->state = VWS_PEER_CLOSED;
            break;
        }
    }
}

void vws_tcp_svr_uv_close(vws_tcp_svr* server, uv_handle_t* handle)
{
    // Guard against a redundant close. Under reconnect-with-inflight two
    // cnx-teardown paths can reach the SAME handle (svr_on_read's EOF
    // close, the uv_thread CLOSE-response close, peer_disconnect, the run()
    // sweep). libuv asserts !uv__is_closing on a second uv_close and aborts the
    // process, so every cnx-teardown close funnels through here and becomes a
    // no-op once the handle is already closing.
    if (uv_is_closing(handle) != 0)
    {
        return;
    }

    // Peer-close feedback for every funneled close (see helper above). All
    // cnx-teardown closes carry a vws_cinfo in handle->data (svr_on_close
    // relies on the same invariant).
    if (handle->data != NULL)
    {
        svr_peer_close_feedback(server, ((vws_cinfo*)handle->data)->cid);
    }

    uv_close(handle, svr_on_close);
}

// A dialed peer socket failed adoption into the loop. Close the descriptor
// (uv never took ownership of it) and return the peer to VWS_PEER_CLOSED so
// uv_thread() re-queues the dial on the next retry tick, rather than parking
// it in a state peer processing never revisits.
static void peer_adopt_fail(vws_peer* peer)
{
#if defined(__windows__)
    closesocket(peer->sockfd);
#else
    close(peer->sockfd);
#endif

    peer->sockfd = VWS_INVALID_SOCKET;
    peer->state  = VWS_PEER_CLOSED;
}

void uv_thread(uv_async_t* handle)
{
    vws_cinfo* cinfo    = (vws_cinfo*)handle->data;
    vws_tcp_svr* server = cinfo->server;

    if (vws.tracelevel >= VT_THREAD)
    {
        vws.trace(VL_INFO, "uv_thread()");
    }

    if (server->state == VS_HALTING)
    {
        if (vws.tracelevel >= VT_THREAD)
        {
            vws.trace(VL_INFO, "uv_thread(): stop");
        }

        // Join worker threads. Worker threads must all exit before we can
        // shutdown libuv as they must release their mutexes and condition
        // variables first, otherwise uv_stop() will not actually stop the loop.
        for (int i = 0; i < server->pool_size; i++)
        {
            uv_thread_join(&server->threads[i]);
        }

        svr_shutdown(server);

        return;
    }

    // We process responses first so that if a peer has disconnected, its cid
    // still points to the closed connect. If a response maps to a closed peer
    // connection, we can catch it and call hook which allows outside queue to
    // queue and resend data when peer reconnects.
    while (queue_empty(&server->responses) == false)
    {
        vws_svr_data* data = queue_pop(&server->responses);

        if ((data == NULL) || (server->responses.state != VS_RUNNING))
        {
            if (data != NULL)
            {
                vws_svr_data_free(data);
            }

            return;
        }

        if (vws_is_flag(&data->flags, VWS_SVR_STATE_CLOSE))
        {
            // Lookup connection
            uintptr_t ptr = address_pool_get(server->cpool, data->cid.key);

            if (ptr != 0)
            {
                // Close connections
                vws_svr_cnx* cnx = (vws_svr_cnx*)ptr;
                vws_tcp_svr_uv_close(server, (uv_handle_t*)cnx->handle);
            }

            vws_svr_data_free(data);
        }
        else
        {
            server->on_data_out(data, NULL);
        }
    }

    // Check for closed peer connections.
    for (size_t i = 0; i < server->peers->used; i++)
    {
        vws_peer* peer = (vws_peer*)server->peers->array[i].value.data;

        // If closed
        if (peer->state == VWS_PEER_CLOSED)
        {
            // Duration-track anchor (down-retry branch). Stamp the first moment
            // this peer became unreachable ONCE -- never re-stamp on a retry, or
            // elapsed would reset every dial and never accumulate. Once the peer
            // has stayed down past peer_unrecoverable_ms, fire the unrecoverable
            // report a single time. This one-timestamp form covers both a peer
            // that was up then froze and one that never connected. The P2
            // duration-track composes here; the up-side ping/deadline is above.
            uint64_t unreach_now = vws_now_ms();
            if (peer->first_unreachable_ts == 0)
            {
                peer->first_unreachable_ts = unreach_now;
            }
            else if ( server->peer_unrecoverable_ms > 0            &&
                      server->peer_unrecoverable_cb != NULL        &&
                      peer->unrecoverable_reported == false        &&
                      (unreach_now - peer->first_unreachable_ts)
                          > (uint64_t)server->peer_unrecoverable_ms )
            {
                peer->unrecoverable_reported = true;
                server->peer_unrecoverable_cb(server, peer->info.cid);
            }

            // Set to pending
            peer->state = VWS_PEER_PENDING;

            // Create queue element
            vws_svr_data* block;
            block = vws_svr_data_own( server,
                                      peer->info.cid,
                                      (ucstr)peer,
                                      sizeof(vws_peer*) );

            // Flag as closed connection
            vws_set_flag(&block->flags, VWS_SVR_STATE_PEER_CONNECT);

            // Queue request
            queue_push(&server->requests, block);
        }

        // NOTE: This code is very similar to vws_tcp_svr_inetd_run() but
        // different enough in that it cannot be easily factored and reused. In
        // both cases we are taking an open socket descriptor and importing it
        // into libuv loop.
        if (peer->state == VWS_PEER_RECONNECTED)
        {
            // We have successfully connected to remote peer and have an active
            // socket descriptor. We now need to take ownership of the socket
            // descriptor, create an associated vws_svr_cnx connection instance
            // for that socket descriptor, customize it for websocket operation
            // and add to connection pool.

            // Set socket to non-blocking mode for libuv
            if (vws_socket_set_nonblocking(peer->sockfd) == false)
            {
                vws.error(VE_RT, "Failed to set socket to nonblocking");
                peer_adopt_fail(peer);
                return;
            }

            // Adopt socket descriptor into libuv loop
            uv_tcp_t* c = (uv_tcp_t*)vws.malloc(sizeof(uv_tcp_t));

            if (uv_tcp_init(server->loop, c))
            {
                // Handle uv_tcp_init failure.
                vws.error(VE_RT, "Failed to initialize new TCP handle");
                vws.free(c);
                peer_adopt_fail(peer);
                return;
            }

            if (uv_tcp_open(c, peer->sockfd))
            {
                // Handle uv_tcp_open failure. Handle is registered in the loop;
                // uv_close (not bare free) to avoid a dangling handle_queue node
                // (UAF at teardown's uv_walk). No cinfo yet, so data is NULL.
                vws.error(VE_RT, "Failed to adopt the socket descriptor.");
                c->data = NULL;
                uv_close((uv_handle_t*)c, svr_on_close_discard);
                peer_adopt_fail(peer);
                return;
            }

            // OS-level dead-peer detection on the
            // adopted peer fd (same as the accept path).
            vws_enable_keepalive(server, c);

            // Create socket info structure
            vws_cinfo* ci = vws.malloc(sizeof(vws_cinfo));
            memcpy(ci, &peer->info, sizeof(vws_cinfo));
            ci->server = server;
            c->data    = ci;

            // Start reads on socket
            if (uv_read_start((uv_stream_t*)c, svr_on_realloc, svr_on_read) != 0)
            {
                // Handle + adopted fd registered; uv_close (not bare free) so the
                // handle leaves the loop queue cleanly and the fd is closed. The
                // close cb frees both the handle and its cinfo (on ->data).
                vws.error(VE_RT, "Failed to start reading from client");
                uv_close((uv_handle_t*)c, svr_on_close_discard);

                // The adopted fd is closed by uv_close above; only return
                // the peer to CLOSED so the dial is re-queued.
                peer->sockfd = VWS_INVALID_SOCKET;
                peer->state  = VWS_PEER_CLOSED;
                return;
            }

            if (vws.tracelevel >= VT_THREAD)
            {
                vws.trace(VL_INFO, "uv_thread(): connecting peer");
            }

            //> Add connection to registry and initialize

            vws_svr_cnx* cnx = svr_cnx_new(server, (uv_stream_t*)c);
            ci->cnx          = cnx;
            ci->cid          = cnx->cid;

            // We have already upgraded this connection.
            cnx->upgraded = true;
            vws_http_msg_free(cnx->http);
            cnx->http = NULL;

            // Call svr_on_connect() handler to complete initialization
            server->on_connect(cnx);

            // Mark connection as peer
            vws_set_flag(&cnx->cid.flags, VWS_SVR_STATE_PEER);

            // Remove UNAUTH flag as peers are automatically trusted
            vws_clear_flag(&cnx->cid.flags, VWS_SVR_STATE_UNAUTH);

            //> Update peer record

            // New connection information
            peer->info = *ci;

            // New connection state
            peer->state = VWS_PEER_CONNECTED;

            // Recovered: clear the duration-track anchor so a later down-span
            // measures from its own start, and re-arm the one-shot report.
            peer->first_unreachable_ts   = 0;
            peer->unrecoverable_reported = false;
        }

        // Heartbeat liveness -- UP branch of the per-peer state gate. On an
        // established peer connection we proactively PING when the link goes
        // idle and clean-close it if a PING is left unanswered past the
        // deadline. The DOWN states (CLOSED/PENDING/FAILED) are handled by the
        // reconnect path above and are the composition point for higher-level
        // fault handling; this branch touches only the CONNECTED case.
        if (peer->state == VWS_PEER_CONNECTED && peer->info.cnx != NULL)
        {
            vws_cnx* c   = (vws_cnx*)peer->info.cnx->data;
            uint64_t now = vws_now_ms();

            if (c != NULL)
            {
                if ( server->pong_deadline_ms > 0 &&
                     c->ping_outstanding &&
                     (now - c->ping_sent_ts) > (uint64_t)server->pong_deadline_ms )
                {
                    // Frozen peer: an outstanding PING went unanswered past the
                    // deadline. Transition the peer OUT of CONNECTED first --
                    // exactly as svr_on_read() does on a read-side EOF -- so this
                    // sweep's CONNECTED branch will not touch the (about-to-be-
                    // freed) cnx again and wd's reconnect branch re-dials it.
                    // Then clean-close with the same close used on EOF/shutdown;
                    // that removes the cid from the pool, so any responses still
                    // queued for it fall to the existing data-lost drop path. No
                    // failure record is emitted here; that policy belongs to the
                    // layer above this transport.
                    peer->state = VWS_PEER_CLOSED;
                    vws_tcp_svr_uv_close( server,
                                          (uv_handle_t*)peer->info.cnx->handle );
                }
                else if ( server->ping_interval_ms > 0 &&
                          c->ping_outstanding == false &&
                          (now - c->last_active)
                              > (uint64_t)server->ping_interval_ms )
                {
                    // Idle peer: send a proactive PING and arm the deadline.
                    vws_buffer*   frame = vws_generate_ping_frame();
                    vws_svr_data* data  =
                        vws_svr_data_new(server, peer->info.cid, &frame);

                    server->on_data_out(data, NULL);
                    vws_buffer_free(frame);

                    c->ping_outstanding = true;
                    c->ping_sent_ts     = now;
                }
            }
        }
    }

    // Resume any reads paused by request-queue
    // backpressure now that workers have drained (this wakeup was signalled by
    // a worker after a pop). Runs on the reactor thread, so uv_read_start is
    // safe here.
    if (atomic_load(&server->reads_paused) > 0)
    {
        svr_resume_paused_reads(server);
    }

    // Call user-defined loop callback if defined
    if (server->loop_cb != NULL)
    {
        server->loop_cb(server);
    }

    // If not all peers are connected
    if (vws_tcp_svr_peers_online(server) == false)
    {
        // Ensure the repeating peer retry tick is running
        vws_tcp_svr_peer_timer(server);
    }
    else if (server->ping_interval_ms > 0 || server->pong_deadline_ms > 0)
    {
        // All peers connected, but heartbeat liveness is configured: the tick
        // must keep running so the CONNECTED-peer sweep above can probe idle
        // peers (idle-PING) and time out silent ones (pong-deadline). Keep it
        // armed rather than stopping it.
        vws_tcp_svr_peer_timer(server);
    }
    else
    {
        // All peers connected and no heartbeat configured: the retry tick is no
        // longer needed. A later disconnect or vws_tcp_svr_peer_add() restarts
        // it.
        uv_timer_stop(server->peer_timer);
    }
}

// Reactor-thread resume pass. For each
// connection whose reads were paused because the request queue was full, try
// to push its stashed pending item now; on success, clear the pause and
// restart reads (kernel TCP flow-control lifts). If the queue is still full,
// leave the connection paused -- the next drain will signal another pass.
static void svr_resume_paused_reads(vws_tcp_svr* server)
{
    address_pool* pool = server->cpool;

    for (uint32_t i = 0; i < pool->capacity; i++)
    {
        uintptr_t ptr = pool->slots[i];
        if (ptr == 0)
        {
            continue;
        }

        vws_svr_cnx* cnx = (vws_svr_cnx*)ptr;

        if (cnx->read_paused == false || cnx->pending == NULL)
        {
            continue;
        }

        if (queue_try_push(&server->requests, cnx->pending) == false)
        {
            // Still full -- try again on the next drain signal.
            continue;
        }

        cnx->pending     = NULL;
        cnx->read_paused = false;
        atomic_fetch_sub(&server->reads_paused, 1);

        // Resume reading from this socket (unless it is closing).
        if (uv_is_closing((uv_handle_t*)cnx->handle) == 0)
        {
            uv_read_start(cnx->handle, svr_on_realloc, svr_on_read);
        }
    }
}

//------------------------------------------------------------------------------
// Server API
//------------------------------------------------------------------------------

vws_svr_data* vws_svr_data_new(vws_tcp_svr* s, vws_cid_t cid, vws_buffer** b)
{
    // Create a new vws_svr_data taking ownership of the buffer's data
    vws_svr_data* item = vws_svr_data_own(s, cid, (*b)->data, (*b)->size);

    // Since we take ownership of buffer data, we clear the buffer.
    (*b)->data = NULL;
    (*b)->size = 0;

    return item;
}

vws_svr_data* vws_svr_data_own(vws_tcp_svr* s, vws_cid_t cid, ucstr data, size_t size)
{
    vws_svr_data* item;
    item = (vws_svr_data*)vws.malloc(sizeof(vws_svr_data));

    item->server = s;
    item->cid    = cid;
    item->size   = size;
    item->data   = data;
    item->flags  = 0;

    // Born with the creator's single reference. Every vws_svr_data is created
    // through this function, so this is the one place the count is initialized.
    atomic_init(&item->refs, 1);

    return item;
}

void vws_svr_data_free(vws_svr_data* t)
{
    if (t == NULL)
    {
        return;
    }

    // Release this holder's reference; only the holder that drops the count to
    // zero performs the actual free. atomic_fetch_sub returns the PRE-decrement
    // value, so a return of 1 means we were the last reference.
    if (atomic_fetch_sub(&t->refs, 1) != 1)
    {
        return;
    }

    vws.free(t->data);
    vws.free(t);
}

vws_tcp_svr* vws_tcp_svr_new(int num_threads, int backlog, int queue_size)
{
    vws_tcp_svr* server = vws.malloc(sizeof(vws_tcp_svr));
    return tcp_svr_ctor(server, num_threads, backlog, queue_size);
}

void vws_tcp_svr_close(vws_tcp_svr* server, vws_cid_t cid)
{
    // Create reply
    vws_svr_data* reply;
    reply = vws_svr_data_own(server, cid, NULL, 0);

    // Set connection close flag
    vws_set_flag(&reply->flags, VWS_SVR_STATE_CLOSE);

    // Queue the data to uv_thread() to send out on wire
    vws_tcp_svr_send(reply);
}

uint8_t vws_tcp_svr_state(vws_tcp_svr* server)
{
    return server->state;
}

void vws_tcp_svr_free(vws_tcp_svr* server)
{
    if (server == NULL)
    {
        return;
    }

    tcp_svr_dtor(server);
    vws.free(server);
}

bool vws_tcp_svr_peers_online(vws_tcp_svr* server)
{
    // Server is online if there are no peers in state VWS_PEER_PENDING
    for (size_t i = 0; i < server->peers->used; i++)
    {
        vws_peer* peer = (vws_peer*)server->peers->array[i].value.data;

        if (peer->state != VWS_PEER_CONNECTED)
        {
            if (vws.tracelevel >= VT_SERVICE)
            {
                vws.trace( VL_INFO,
                           "vws_tcp_svr_peers_online(%p): "
                           "peer offline %s state=%lu",
                           server,
                           peer->host,
                           peer->state );
            }

            return false;
        }
    }

    return true;
}

int vws_tcp_svr_send(vws_svr_data* data)
{
    // Take the loop's reference BEFORE the data becomes visible to the loop.
    // This point DOMINATES the race window: the instant queue_push hands the
    // data to the responses queue, the loop can pop it, write it, and free it
    // (svr_on_write_complete) concurrently. Acquiring the ref here -- while the
    // worker still exclusively holds the data -- guarantees the count is >= 1
    // through our own uv_async_send read below, so the loop's free cannot pull
    // the object out from under us. (Acquiring it AFTER the push, e.g. inside
    // uv_async_send, would itself race the free -- the same UAF, just moved.)
    atomic_fetch_add(&data->refs, 1);

    queue_push(&data->server->responses, data);

    // Notify event loop about the new response. This is the worker's LAST read
    // of data; it is safe because the ref taken above keeps the object alive.
    uv_async_send(data->server->wakeup);

    // Drop the worker's reference now that its last read is done. If the loop
    // already consumed and released its ref, this drops the count to 0 and
    // frees; otherwise the loop's svr_on_write_complete will free at 0.
    vws_svr_data_free(data);

    return 0;
}

void vws_tcp_svr_wakeup(vws_tcp_svr* s)
{
    uv_async_send(s->wakeup);
}

int vws_tcp_svr_run(vws_tcp_svr* server, cstr host, int port)
{
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace( VL_INFO,
                   "vws_tcp_svr_run(%p): Starting worker %i threads",
                   server,
                   server->pool_size );
    }

    for (int i = 0; i < server->pool_size; i++)
    {
        uv_thread_create(&server->threads[i], worker_thread, server);
    }

    //> Create listening socket

    uv_tcp_t* socket = vws.malloc(sizeof(uv_tcp_t));
    uv_tcp_init(server->loop, socket);

    vws_cinfo* ci = vws.malloc(sizeof(vws_cinfo));
    ci->cnx       = NULL;
    ci->server    = server;
    ci->cid.key   = -1;   // never a pool slot: 0 is a VALID slot (safe close path)
    socket->data  = ci;

    //> Bind to address

    int rc;
    struct sockaddr_in addr;
    uv_ip4_addr(host, port, &addr);
    rc = uv_tcp_bind(socket, (const struct sockaddr*)&addr, 0);

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace( VL_INFO,
                   "vws_tcp_svr_run(%p): Bind %s:%lu",
                   server, host, port );
    }

    if (rc)
    {
        vws.error(VE_RT, "Bind error %s", uv_strerror(rc));

        // the listen handle is registered in the loop (uv_tcp_init
        // succeeded), so uv_close it (freeing the handle + its ci via
        // svr_on_close_discard) rather than leaking both on the early return.
        uv_close((uv_handle_t*)socket, svr_on_close_discard);

        // expose a distinct failure state so a caller polling for
        // startup breaks instead of spinning forever on VS_HALTED.
        server->state = VS_FAILED;
        return -1;
    }

    //> Listen

    rc = uv_listen((uv_stream_t*)socket, server->backlog, svr_on_connect);

    if (rc)
    {
        vws.error(VE_RT, "Listen error %s", uv_strerror(rc));

        // same as the bind arm above -- close the registered handle.
        uv_close((uv_handle_t*)socket, svr_on_close_discard);

        // expose the failure state (see the bind arm above).
        server->state = VS_FAILED;
        return -1;
    }

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace( VL_INFO,
                   "vws_tcp_svr_run(%p): Listen %s:%lu",
                   server, host, port );
    }

    //> Start server

    // Set state to running
    server->state = VS_RUNNING;

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_run(%p): Starting uv_run()", server);
    }

    while (vws_tcp_svr_is_running(server))
    {
        // Run UV loop. This runs indefinitely, passing network I/O and and out
        // of system until server is shutdown by vws_tcp_svr_stop() (by external
        // thread).
        uv_run(server->loop, UV_RUN_DEFAULT);
    }

    //> Shutdown server

    // Close the listening socket handle
    vws_tcp_svr_uv_close(server, (uv_handle_t*)socket);

    vws.trace( VL_INFO, "vws_tcp_svr_run(%p): Shutdown connections=%lu",
               server,
               server->cpool->count );

    // Close connections.
    for (uint32_t i = 0; i < server->cpool->capacity; i++)
    {
        uintptr_t ptr = server->cpool->slots[i];

        if (ptr != 0)
        {
            // Close connection
            vws_svr_cnx* cnx = (vws_svr_cnx*)ptr;

            if (vws.tracelevel >= VT_SERVICE)
            {
                vws.trace( VL_INFO,
                           "vws_tcp_svr_run(%p): closing %p",
                           server,
                           cnx->handle );
            }

            vws_tcp_svr_uv_close(server, (uv_handle_t*)cnx->handle);
        }
    }

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_run(%p): Shutdown complete", server);
    }

    // Set state to halted
    server->state = VS_HALTED;

    // C-SVR-6: release the network thread's thread-local error state. svr_on_connect
    // and other reactor callbacks run on this thread and may set vws.e.text; left
    // set, the last strdup is definitely lost when this thread exits (this is the
    // gate-blocking accept-path leak the SP5 accept cells surface). Thread-local
    // only -- no effect on workers or the global SSL ctx.
    vws_cleanup();

    return 0;
}

bool vws_tcp_svr_is_running(vws_tcp_svr* server)
{
    if (server->state == VS_RUNNING)
    {
        return true;
    }

    return false;
}

bool vws_tcp_svr_is_halted(vws_tcp_svr* server)
{
    if (server->state == VS_HALTED)
    {
        return true;
    }

    return false;
}

void vws_tcp_svr_stop(vws_tcp_svr* server)
{
    // Failed-start fast path decision: read BEFORE the clobber below.
    int failed_start = (server->state == VS_FAILED);

    // Set shutdown flags
    server->state           = VS_HALTING;
    server->requests.state  = VS_HALTING;
    server->responses.state = VS_HALTING;

    // Wakeup all worker threads
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_stop(): stop worker threads");
    }

    uv_mutex_lock(&server->requests.mutex);
    uv_cond_broadcast(&server->requests.cond);
    uv_mutex_unlock(&server->requests.mutex);

    if (failed_start)
    {
        // A failed start (bind/listen error, after run() already spawned the
        // worker pool) means the uv loop never ran: the wakeup async below is
        // inert and uv_thread's halting arm — the only setter of VS_HALTED on
        // the stop path and the only worker-join site — can never fire. The
        // drain below would burn its full budget waiting for a state no live
        // thread can set, and the workers (woken by the broadcast above,
        // exiting through queue_pop's HALTING arm) would never be joined —
        // free to still be inside queue_pop when the owner destroys the queue
        // mutex/cond right after stop() returns. Join them deterministically
        // instead, then mark the server halted.
        for (int i = 0; i < server->pool_size; i++)
        {
            uv_thread_join(&server->threads[i]);
        }

        server->state = VS_HALTED;
        return;
    }

    // Wakeup the main event loop to shutdown main thread
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_stop(): stop main thread");
    }

    uv_async_send(server->wakeup);

    // Bound the shutdown drain (a57b5e8ad7 / cea2369900 #4). The original loop
    // waited forever for VS_HALTED: if a uv handle never finishes closing — e.g.
    // a connection dropped with unflushed inflight data leaves a write/handle
    // whose close never completes — the loop never observes VS_HALTED and stop()
    // hangs the caller indefinitely. Cap the wait so stop() always returns within
    // a bounded budget; on timeout we proceed best-effort (the process is
    // shutting down regardless) rather than block the caller forever.
    const int drain_budget_ms = 15000;   // bounded shutdown budget
    const int poll_ms         = 100;     // finer granularity than the old 1000ms
    int       waited_ms       = 0;

    while (server->state != VS_HALTED && waited_ms < drain_budget_ms)
    {
        // uv_sleep() is portable (POSIX sleep() is unavailable on Windows).
        uv_sleep(poll_ms);
        waited_ms += poll_ms;
    }

    if (server->state != VS_HALTED)
    {
        vws.error( VE_RT,
                   "vws_tcp_svr_stop(): drain did not reach VS_HALTED within "
                   "%d ms; returning best-effort (a handle may not have closed)",
                   drain_budget_ms );
    }
}

int vws_tcp_svr_inetd_run(vws_tcp_svr* server, vws_sockfd_t sockfd)
{
    if (sockfd == VWS_INVALID_SOCKET)
    {
        vws.error(VE_RT, "Invalid server or socket descriptor provided");
        return 1;
    }

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace( VL_INFO,
                   "vws_tcp_svr_run(%p): Starting worker %i threads",
                   server,
                   server->pool_size );
    }

    for (int i = 0; i < server->pool_size; i++)
    {
        uv_thread_create(&server->threads[i], worker_thread, server);
    }

    // Go into non-blocking mode as we are using poll() for socket_read() and
    // socket_write().
    if (vws_socket_set_nonblocking(sockfd) == false)
    {
        vws.error(VE_RT, "Failed to set socket to nonblocking");
        return 1;
    }

    // Set to inetd mode
    server->inetd_mode = 1;

    // Adopt socket descriptor into libuv loop
    uv_tcp_t* c = (uv_tcp_t*)vws.malloc(sizeof(uv_tcp_t));

    if (uv_tcp_init(server->loop, c))
    {
        // Handle uv_tcp_init failure.
        vws.error(VE_RT, "Failed to initialize new TCP handle");
        vws.free(c);
        return 1;
    }

    if (uv_tcp_open(c, sockfd))
    {
        // Handle uv_tcp_open failure. The handle is already registered in the
        // loop (uv_tcp_init succeeded), so it must be uv_close'd -- a bare free
        // leaves a dangling node in loop->handle_queue (UAF at teardown's
        // uv_walk). No cinfo yet, so data is NULL.
        vws.error(VE_RT, "Failed to adopt the socket descriptor.");
        c->data = NULL;
        uv_close((uv_handle_t*)c, svr_on_close_discard);
        return 1;
    }

    // Create socket info structure
    vws_cinfo* ci = vws.malloc(sizeof(vws_cinfo));
    ci->cnx       = NULL;
    ci->server    = server;
    c->data       = ci;

    // Start reads on socket
    if (uv_read_start((uv_stream_t*)c, svr_on_realloc, svr_on_read) != 0)
    {
        // Handle + adopted fd are registered; uv_close (not bare free) so the
        // handle leaves the loop queue cleanly and the fd is closed. The close
        // cb frees both the handle and its cinfo (carried on ->data).
        vws.error(VE_RT, "Failed to start reading from client");
        uv_close((uv_handle_t*)c, svr_on_close_discard);
        return 1;
    }

    //> Add connection to registry and initialize

    vws_svr_cnx* cnx = svr_cnx_new(server, (uv_stream_t*)c);
    ci->cnx          = cnx;
    ci->cid          = cnx->cid;

    // Call svr_on_connect() handler to complete initialization
    server->on_connect(cnx);

    // Call for callback to collect the client cid
    server->on_inetd_connect(cnx);

    // C-SVR-3: mark the server RUNNING (mirror vws_tcp_svr_run) so the
    // close -> svr_on_close -> inetd_stop auto-stop (its is_running check) fires
    // when the single client disconnects -- otherwise the inetd process hangs
    // forever (resource-exhaustion DoS under the deployed perpd/inetd model).
    server->state = VS_RUNNING;

    // Now, the handle is associated with the socket and is ready to be used.
    // Start the libuv loop.
    uv_run(server->loop, UV_RUN_DEFAULT);

    // C-SVR-6: release this thread's thread-local error state (same pattern as
    // vws_tcp_svr_run). Thread-local only -- safe whether inetd_run was invoked
    // on the main thread or a dedicated one.
    vws_cleanup();

    return 0;
}

void vws_tcp_svr_inetd_stop(vws_tcp_svr* server)
{
    // Set shutdown flags
    server->state           = VS_HALTING;
    server->requests.state  = VS_HALTING;
    server->responses.state = VS_HALTING;

    // Stop the loop. We have not more I/O to deal with. We don't want the loop
    // to run any more for any reason.
    uv_stop(server->loop);

    // Wakeup all worker threads
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_inetd_stop(): stop worker threads");
    }

    uv_mutex_lock(&server->requests.mutex);
    uv_cond_broadcast(&server->requests.cond);
    uv_mutex_unlock(&server->requests.mutex);

    // Wakeup the main event loop to shutdown main thread
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_inetd_stop(): stop main thread");
    }

    // Wait for all threads to complete
    uv_thread(server->wakeup);

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_inetd_stop(): done");
    }

    // Set state to halted
    server->state = VS_HALTED;
}

bool svr_resolve(cstr host, int port, struct sockaddr_storage** s)
{
    vws_sockfd_t sockfd = VWS_INVALID_SOCKET;

    // Resolve the host
    struct addrinfo hints, *res, *res0;
    int error;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = PF_UNSPEC; // Accept any family (IPv4 or IPv6)
    hints.ai_socktype = SOCK_STREAM;

    char port_str[50];
    sprintf(port_str, "%lu", port);

    error = getaddrinfo(host, &port_str[0], &hints, &res0);

    if (error)
    {
        if (vws.tracelevel > 0)
        {
            cstr msg = gai_strerror(error);
            vws.trace(VL_ERROR, "getaddrinfo failed: %s: %s", host, msg);
        }

        vws.error(VE_SYS, "getaddrinfo() failed");

        // [vws V-8] svr_resolve is bool; `return -1` converts to TRUE, so the
        // PEER_CONNECT fast-fail arm read a DNS-resolution failure as success
        // and proceeded to the real dial (which then re-failed on its own
        // resolution) -- the fast-fail was dead for exactly this case.
        return false;
    }

    for (res = res0; res; res = res->ai_next)
    {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        if (sockfd == VWS_INVALID_SOCKET)
        {
            vws.error(VE_SYS, "Failed to create socket");
            continue;
        }

        if (vws.tracelevel >= VT_SERVICE)
        {
            cstr host; int port;
            struct sockaddr* addr = (struct sockaddr*)res->ai_addr;
            if (vws_socket_addr_info(addr, &host, &port) == true)
            {
                vws.trace(VL_INFO, "svr_resolve() connect %s:%lu", host, port);
                free(host);
            }
            else
            {
                vws.trace(VL_INFO, "svr_resolve(): connect");
            }
        }

        if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1)
        {
#if defined(__windows__)
            closesocket(sockfd);
#else
            close(sockfd);
#endif
            sockfd = VWS_INVALID_SOCKET;

            vws.error(VE_SYS, "Failed to connect");
            continue;
        }

        break; // If we get here, we must have connected successfully
    }

    // Free the addrinfo structure for this host
    freeaddrinfo(res0);

    if (sockfd == VWS_INVALID_SOCKET)
    {
        vws.error(VE_SYS, "Unable to resolve host %s:%lu", host, port);
        return false;
    }

#if defined(__windows__)
    closesocket(sockfd);
#else
    close(sockfd);
#endif

    return true;
}

bool vws_tcp_svr_peer_connect(vws_tcp_svr* s, vws_peer* peer, void* x)
{
    // Attempt reconnection
    peer->sockfd = peer->connect(peer, x);

    // Successful if sockfd is valid
    return (peer->sockfd != VWS_INVALID_SOCKET);
}

void vws_tcp_svr_peer_disconnect(vws_tcp_svr* s, vws_peer* peer)
{
    // Close the server-side connection
    vws_tcp_svr_uv_close(s, (uv_handle_t*)peer->info.cnx->handle);
}

vws_peer* vws_tcp_svr_peer_add( vws_tcp_svr* s,
                                cstr h,
                                int p,
                                vws_peer_connect fn,
                                void* data )
{
    vws_peer peer;

    if (fn == NULL)
    {
        vws.error( VL_WARN,
                   "vws_tcp_svr_peer_add(): "
                   "connection function not provided" );
        return NULL;
    }

    // Zero the record so info/sockfd and the duration-track anchor
    // (first_unreachable_ts / unrecoverable_reported) start well-defined; the
    // first not-CONNECTED sweep stamps the anchor and CONNECTED clears it.
    memset(&peer, 0, sizeof(peer));
    vws_cid_clear(&peer.info.cid);

    // Set connection function
    peer.connect = fn;

    // Set connection as closed. uv_thread() will connect
    peer.state = VWS_PEER_CLOSED;
    peer.host  = strdup(h);
    peer.port  = p;
    peer.data  = data;

    char key[514];
    sprintf(key, "%s:%lu", h, p);
    vws_kvs_set(s->peers, key, &peer, sizeof(vws_peer));

    // Set wakeup timer
    vws_tcp_svr_peer_timer(s);

    return vws_kvs_get(s->peers, key)->data;
}

void vws_tcp_svr_peer_remove(vws_tcp_svr* s, cstr h, int p)
{
    char key[514];
    sprintf(key, "%s:%lu", h, p);

    vws_peer* peer = vws_kvs_get(s->peers, key)->data;

    if (peer != NULL)
    {
        free(peer->host);
    }

    vws_kvs_remove(s->peers, key);
}

//------------------------------------------------------------------------------
// Server construction / destruction
//------------------------------------------------------------------------------

vws_tcp_svr* tcp_svr_ctor(vws_tcp_svr* svr, int nt, int backlog, int queue_size)
{
    if (backlog == 0)
    {
        backlog = 128;
    }

    if (queue_size == 0)
    {
        queue_size = 1024;
    }

    svr->threads          = vws.malloc(sizeof(uv_thread_t) * nt);
    svr->pool_size        = nt;
    svr->on_connect       = svr_client_connect;
    svr->on_inetd_connect = svr_client_inetd_connect;
    svr->on_disconnect    = svr_client_disconnect;
    svr->on_read          = svr_client_read;
    svr->on_data_in       = svr_client_data_in;
    svr->on_data_out      = svr_client_data_out;
    svr->worker_ctor      = NULL;
    svr->worker_ctor_data = NULL;
    svr->worker_dtor      = NULL;
    svr->loop_cb          = NULL;
    svr->shutdown_cb      = NULL;
    svr->data_lost_cb     = NULL;
    svr->cnx_open_cb      = NULL;
    svr->cnx_close_cb     = NULL;
    svr->backlog          = backlog;
    svr->write_queue_cap  = 0;   // 0 => default 2 x VWS_MAX_MESSAGE_SIZE
    atomic_init(&svr->reads_paused, 0);

    // In-code defaults (broker overrides from
    // vrtql.conf grid.broker.*); these preserve today's behavior.
    svr->keepalive_idle_sec        = 30;
    svr->keepalive_user_timeout_ms = 20000;
    svr->request_reassembly_cap    = (size_t)VWS_MAX_MESSAGE_SIZE;
    svr->max_message_size          = (size_t)VWS_MAX_MESSAGE_SIZE;
    svr->ping_interval_ms          = 10000;
    svr->pong_deadline_ms          = 20000;
    svr->peer_unrecoverable_ms     = 60000;
    svr->peer_unrecoverable_cb     = NULL;

    svr->loop             = (uv_loop_t*)vws.malloc(sizeof(uv_loop_t));
    svr->state            = VS_HALTED;
    svr->trace            = vws.tracelevel;
    svr->inetd_mode       = 0;
    svr->peers            = vws_kvs_new(10, false);
    svr->peer_timeout     = 0;

    uv_loop_init(svr->loop);
    svr->cpool = address_pool_new(1000, 2);
    queue_init(&svr->requests, queue_size, "requests");
    queue_init(&svr->responses, queue_size, "responses");

    vws_cinfo* cinfo = vws.malloc(sizeof(vws_cinfo));
    cinfo->cnx       = NULL;
    cinfo->server    = svr;
    cinfo->cid.key   = -1;   // never a pool slot: 0 is a VALID slot (safe close path)

    svr->wakeup = vws.malloc(sizeof(uv_async_t));
    svr->wakeup->data = cinfo;
    uv_async_init(svr->loop, svr->wakeup, uv_thread);

    svr->peer_timer = vws.malloc(sizeof(uv_timer_t));
    uv_timer_init(svr->loop, svr->peer_timer);

    return svr;
}

void on_uv_close(uv_handle_t* handle)
{
    if (handle != NULL)
    {
        // As a policy we don't put heap data on handle->data. If that is ever
        // done then it must be freed in the appropriate place. It is ignored
        // here because it's impossible to tell what it is by the very nature of
        // uv_handle_t. We will generate a warning however.
        if (handle->data != NULL)
        {
            vws.trace( VL_WARN,
                       "on_uv_close(): libuv resource not properly freed:"
                       "handle=%p handle->data=%p",
                       (void*)handle,
                       (void*)handle->data );
        }
    }
}

void on_uv_walk(uv_handle_t* handle, void* arg)
{
    // If this handle has not been closed, it should have been. Nevertheless we
    // will make an attempt to close it.
    if (uv_is_closing(handle) == 0)
    {
        uv_close(handle, (uv_close_cb)on_uv_close);
    }
}

// Close callback for a half-adopted handle that was registered in the loop
// (uv_tcp_init succeeded) but failed a later adoption step (uv_tcp_open /
// uv_read_start). It has no registered cnx, so there is no pool/cnx cleanup —
// just free the optional cinfo carried on ->data and the handle itself. Freeing
// such a handle bare (without uv_close) would leave a dangling node in
// loop->handle_queue and a use-after-free at teardown's uv_walk.
static void svr_on_close_discard(uv_handle_t* handle)
{
    if (handle->data != NULL)
    {
        vws.free(handle->data);
    }

    vws.free(handle);
}

void tcp_svr_dtor(vws_tcp_svr* svr)
{
    vws.free(svr->threads);

    // Close the server async handle
    uv_close((uv_handle_t*)svr->wakeup, svr_on_close);

    // Close peer timer. STOP the repeating timer FIRST (mirrors the normal-path
    // uv_timer_stop in vws_tcp_svr_peer_timer's
    // all-connected branch): peer_timer is armed with uv_timer_start(...,200,200)
    // and left active; nulling data + uv_close alone did NOT stop the subsequent
    // teardown uv_run drain from dispatching one more peer_timer_callback with data
    // already NULL -> uv_async_send(NULL->wakeup) SIGSEGV. Stopping first removes
    // the pending fire at its source (the callback null-guard is belt-and-suspenders).
    uv_timer_stop(svr->peer_timer);
    svr->peer_timer->data = NULL;
    uv_close((uv_handle_t*)svr->peer_timer, svr_on_timer_close);

    //> Shutdown libuv

    // Walk the loop to close everything
    uv_walk(svr->loop, on_uv_walk, NULL);

    while (uv_loop_close(svr->loop))
    {
        // Run the loop until there are no more active handles
        while (uv_loop_alive(svr->loop))
        {
            uv_run(svr->loop, UV_RUN_DEFAULT);
        }
    }

    // Free loop
    vws.free(svr->loop);

    // Call user-defined loop callback if defined
    if (svr->shutdown_cb != NULL)
    {
        svr->shutdown_cb(svr);
    }

    // Free address pool
    address_pool_free(&svr->cpool);

    // C-SVR-4: free the request/response queues allocated in tcp_svr_ctor. On
    // the run path svr_shutdown already destroyed them; queue_destroy is
    // idempotent (it no-ops once queue->buffer is NULL), so this does real work
    // only for a server that was created but never run -- which otherwise leaks
    // the ctor-allocated queue buffers, names, mutex and cond (~16 KB).
    queue_destroy(&svr->requests);
    queue_destroy(&svr->responses);

    // Free peers

    for (size_t i = 0; i < svr->peers->used; i++)
    {
        vws_peer* peer = (vws_peer*)svr->peers->array[i].value.data;
        free(peer->host);
    }

    vws_kvs_free(svr->peers);
}

//------------------------------------------------------------------------------
// Client connection callbacks
//------------------------------------------------------------------------------

void svr_client_connect(vws_svr_cnx* c)
{
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "svr_client_connect(%p)", c->handle);
    }
}

void svr_client_inetd_connect(vws_svr_cnx* c)
{
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "svr_client_inetd_connect(%p)", c->handle);
    }
}

void svr_client_disconnect(vws_svr_cnx* c)
{
    if (vws.tracelevel >= VT_SERVICE)
    {
        if (c != NULL)
        {
            vws.trace(VL_INFO, "svr_client_disconnect(%p)", c->handle);
        }
        else
        {
            vws.trace(VL_INFO, "svr_client_disconnect(%p)", NULL);
        }
    }
}

void svr_client_read(vws_svr_cnx* c, ssize_t size, const uv_buf_t* buf)
{
    vws_tcp_svr* server = c->server;

    // Queue data to worker pool for processing
    vws_svr_data* data = vws_svr_data_own(server, c->cid, (ucstr)buf->base, size);

    // Store reference to server
    data->server = c->server;

    // Non-blocking hand-off. This runs on the
    // REACTOR thread; the old blocking queue_push froze the whole reactor when
    // the request queue was full (reproduced: harness_backpressure ROW-6). If
    // the queue is full, apply read-side TCP flow control instead: uv_read_stop
    // this socket (the kernel stops accepting from the peer), stash the one
    // in-flight item, and mark the connection paused. A worker signals the
    // reactor on drain, which pushes the pending item and resumes reads. The
    // reactor never blocks; nothing is dropped.
    if (queue_try_push(&server->requests, data) == false)
    {
        uv_read_stop((uv_stream_t*)c->handle);
        c->pending     = data;
        c->read_paused = true;
        atomic_fetch_add(&server->reads_paused, 1);
    }
}

void svr_client_data_in(vws_svr_data* m, void* x)
{
    // Default: Do nothing. Drop data.
    vws_svr_data_free(m);
}

void svr_client_data_out(vws_svr_data* data, void* x)
{
    if (data->size == 0)
    {
        vws.trace(VL_INFO, "svr_client_data_out(): no data");
        vws.error(VL_WARN, "svr_client_data_out(): no data");

        vws_svr_data_free(data);

        return;
    }

    // Check address pool and ensure connection is still active
    uintptr_t ptr = address_pool_get(data->server->cpool, data->cid.key);

    if (ptr == 0)
    {
        // Connection no longer exists.

        // Call user-defined loop callback if defined
        if (data->server->data_lost_cb != NULL)
        {
            data->server->data_lost_cb(data, x);
        }
        else
        {
            // Unhandled. Delete.
            vws_svr_data_free(data);
        }

        return;
    }

    uv_stream_t* handle = ((vws_svr_cnx*)ptr)->handle;

    // If the connection is already being torn down (a prior shed, or a normal
    // close in flight), drop this message: uv_write on a closing handle is
    // invalid and a second uv_close would abort libuv. The cid lingers in the
    // pool until svr_on_close runs, so without this guard the responses still
    // queued to a just-shed cid would each re-enter the shed below and double-
    // close. 
    if (uv_is_closing((uv_handle_t*)handle))
    {
        vws_svr_data_free(data);
        return;
    }

    // Outbound backpressure cap + SHED.
    // Without this, uv_write queues unboundedly for a slow/stalled consumer
    // (TCP recv-window full) -> libuv write heap grows without limit -> broker
    // OOM (reproduced: harness_backpressure ROW 5). Cap the per-connection
    // queued write bytes; on exceed, drop this message and force-close the
    // connection. The close runs normal teardown -> the consumer's bindings
    // release -> each pending sender gets the EXISTING per-message M2 refused
    // NACK (producer-paced request-reply, no mass-NACK blast) -> it reconnects
    // when healthy (down-is-down).
    size_t cap = data->server->write_queue_cap;
    if (cap == 0)
    {
        cap = 2 * (size_t)VWS_MAX_MESSAGE_SIZE;   // default
    }
    if (cap < (size_t)VWS_MAX_MESSAGE_SIZE)
    {
        cap = (size_t)VWS_MAX_MESSAGE_SIZE;       // floor: never false-shed a
                                                  // single legit max message
    }

    size_t queued = uv_stream_get_write_queue_size(handle);

    if (queued + data->size > cap)
    {
        vws.error(VE_RT,
                  "svr_client_data_out(): write-queue cap exceeded "
                  "(%zu queued + %zu > %zu cap) -- shedding cid",
                  queued, data->size, cap);

        vws_svr_data_free(data);

        // Force-close DIRECTLY on the loop thread (we are on it) via uv_close ->
        // svr_on_close -> on_disconnect (bindings release -> the existing
        // per-message M2 refused NACK to each pending sender). NOT the
        // response-queue close (vws_tcp_svr_close), which would deadlock here:
        // under a flood the response queue is full, so enqueuing a close from
        // the loop thread would block in queue_push while the producer also
        // blocks pushing -> hang. Direct uv_close has no queue round-trip.
        vws_tcp_svr_uv_close(data->server, (uv_handle_t*)handle);

        return;
    }

    uv_buf_t buf    = uv_buf_init(data->data, data->size);
    uv_write_t* req = (uv_write_t*)vws.malloc(sizeof(uv_write_t));
    req->data       = data;

    // Write out to libuv
    if (uv_write(req, handle, &buf, 1, svr_on_write_complete) != 0)
    {
        // Connection is closing/closed.
        vws_svr_data_free(data);
        vws.free(req);
    }
}

//------------------------------------------------------------------------------
// Server Connection
//------------------------------------------------------------------------------

vws_svr_cnx* svr_cnx_new(vws_tcp_svr* s, uv_stream_t* handle)
{
    vws_svr_cnx* cnx = vws.malloc(sizeof(vws_svr_cnx));
    cnx->server      = s;
    cnx->handle      = handle;
    cnx->data        = NULL;
    cnx->format      = VM_MPACK_FORMAT;
    cnx->read_paused = false;    // 
    cnx->pending     = NULL;

    vws_cid_clear(&cnx->cid);

    // Initialize HTTP state
    cnx->upgraded    = false;
    cnx->http        = vws_http_msg_new(HTTP_REQUEST);

    // Add to address pool
    cnx->cid.key     = address_pool_set(s->cpool, (uintptr_t)cnx);

    // Mark connection as unauthorized
    vws_set_flag(&cnx->cid.flags, VWS_SVR_STATE_UNAUTH);

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "svr_cnx_new(%p): added %lu", s, cnx->cid.key);
    }

    // Get peer info
    int len = sizeof(struct sockaddr_storage);
    uv_tcp_getpeername( (const uv_tcp_t*)handle,
                        (struct sockaddr*)&cnx->cid.addr, &len );

    // Call user-defined connection open callback if defined
    if (s->cnx_open_cb != NULL)
    {
        s->cnx_open_cb(cnx);
    }

    return cnx;
}

void svr_cnx_free(vws_svr_cnx* cnx)
{
    if (cnx != NULL)
    {
        if (vws.tracelevel >= VT_SERVICE)
        {
            vws.trace(VL_INFO, "svr_cnx_free(%p)", cnx);
        }

        // Call user-defined connection close callback if defined
        if (cnx->server->cnx_close_cb != NULL)
        {
            cnx->server->cnx_close_cb(cnx);
        }

        if (cnx->http != NULL)
        {
            vws_http_msg_free(cnx->http);
        }

        // [vws V-7] A connection closed while backpressure-PAUSED still owns its
        // stashed in-flight item and its reads_paused reservation. The drain/
        // resume path (svr_client_read pause arm) is never reached for a closing
        // cnx, so without this cnx->pending LEAKS and server->reads_paused stays
        // incremented forever -- the drain check then spuriously async-wakes the
        // reactor on every request for the life of the server. Release the
        // stashed item (refcounted free) and give back the reservation.
        if (cnx->read_paused == true)
        {
            if (cnx->pending != NULL)
            {
                vws_svr_data_free(cnx->pending);
                cnx->pending = NULL;
            }

            cnx->read_paused = false;
            atomic_fetch_sub(&cnx->server->reads_paused, 1);
        }

        // Remove from pool
        address_pool_remove(cnx->server->cpool, cnx->cid.key);

        vws.free(cnx);
    }
}

void svr_cnx_close(vws_tcp_svr* server, vws_cid_t cid)
{
    vws_tcp_svr_close(server, cid);
}

//------------------------------------------------------------------------------
// Server Utilities
//------------------------------------------------------------------------------

void svr_shutdown(vws_tcp_svr* server)
{
    if (server->state == VS_HALTED)
    {
        return;
    }

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "svr_shutdown(%p): Shutdown starting", server);
    }

    //> Cleanup queues

    // Drain BOTH queues before destroy. queue_destroy frees only the ring
    // buffer array, not the vws_svr_data items still resident in it. The
    // requests queue can hold residual items at shutdown: once stop() flips
    // its state to VS_HALTING, queue_pop returns NULL immediately (even with
    // size>0), so worker threads stop draining with items still queued. Draining
    // requests here, symmetric to responses, frees those items + their payloads.
    vws_svr_queue* queues[2] = { &server->requests, &server->responses };
    for (int i = 0; i < 2; i++)
    {
        vws_svr_queue* queue = queues[i];
        uv_mutex_lock(&queue->mutex);
        while (queue->size > 0)
        {
            vws_svr_data* data = queue->buffer[queue->head];
            queue->head        = (queue->head + 1) % queue->capacity;
            queue->size--;

            // A PEER_CONNECT block carries a BORROWED pointer to the kvs-owned
            // vws_peer (built at the peer-reconnect producer with (ucstr)peer).
            // The normal consumer (svr_route_data) clears ->data before free for
            // exactly this reason; this generic drain must mirror it, else it
            // frees the peer that tcp_svr_dtor's vws_kvs_free later double-frees
            // (use-after-free on peer->host + double-free). The peer is owned by
            // the peers kvs and freed there exactly once; only the block struct
            // is this drain's to reclaim.
            if (vws_is_flag(&data->flags, VWS_SVR_STATE_PEER_CONNECT))
            {
                data->data = NULL;
            }

            vws_svr_data_free(data);
        }
        uv_mutex_unlock(&queue->mutex);
    }

    queue_destroy(&server->requests);
    queue_destroy(&server->responses);

    // Stop the loop. This will cause uv_run() to return in vws_tcp_svr_run()
    // which will also return.
    uv_stop(server->loop);
}

void svr_on_connect(uv_stream_t* socket, int status)
{
    vws_cinfo* svr_addr = (vws_cinfo*)socket->data;
    vws_tcp_svr* server = svr_addr->server;

    if (status < 0)
    {
        cstr e = uv_strerror(status);
        vws.error(VE_RT, "Error in connection callback: %s", e);
        return;
    }

    uv_tcp_t* c = (uv_tcp_t*)vws.malloc(sizeof(uv_tcp_t));

    if (c == NULL)
    {
        vws.error(VE_RT, "Failed to allocate memory for client");
        return;
    }

    vws_cinfo* cinfo = vws.malloc(sizeof(vws_cinfo));
    cinfo->cnx       = NULL;
    cinfo->server    = server;
    cinfo->cid.key   = -1;   // invalid until a cnx is registered (safe close path)
    c->data          = cinfo;

    if (uv_tcp_init(server->loop, c) != 0)
    {
        // C-SVR-2: the handle never entered the loop (init failed), so free the
        // just-allocated client handle + its cinfo directly instead of leaking
        // them on the early return (peer-drivable accept-path leak).
        vws.error(VE_RT, "Failed to initialize client");
        vws.free(cinfo);
        vws.free(c);
        return;
    }

    if (uv_accept(socket, (uv_stream_t*)c) == 0)
    {
        // OS-level dead-peer detection on the accepted fd.
        vws_enable_keepalive(server, c);

        if (uv_read_start((uv_stream_t*)c, svr_on_realloc, svr_on_read) != 0)
        {
            // C-SVR-2: the handle is already registered + the fd adopted, so it
            // cannot be bare-freed -- close it through svr_on_close (which frees
            // the handle + cinfo; cid is invalid so no cnx cleanup runs) rather
            // than leaking it on the early return.
            vws.error(VE_RT, "Failed to start reading from client");
            vws_tcp_svr_uv_close(server, (uv_handle_t*)c);
            return;
        }
    }
    else
    {
        // Accept failed: close the handle AND return. Without the return the
        // path fell through to svr_cnx_new() + on_connect() on a closing handle
        // -- a spurious connection (and, with the shutdown sweep, a candidate
        // double-close). The read_start-fail arm above already returns.
        vws_tcp_svr_uv_close(server, (uv_handle_t*)c);
        return;
    }

    //> Add connection to registry and initialize

    vws_svr_cnx* cnx = svr_cnx_new(server, (uv_stream_t*)c);
    cinfo->cnx       = cnx;
    cinfo->cid       = cnx->cid;

    //> Call svr_on_connect() handler

    server->on_connect(cnx);

    if (server->inetd_mode == 1)
    {
        // Call for callback to collect the client cid
        server->on_inetd_connect(cnx);
    }
}

void svr_on_read(uv_stream_t* c, ssize_t nread, const uv_buf_t* buf)
{
    vws_cinfo* cinfo    = (vws_cinfo*)c->data;
    vws_svr_cnx* cnx    = cinfo->cnx;
    vws_tcp_svr* server = cinfo->server;
    vws_cid_t cid       = cinfo->cid;

    if (nread < 0)
    {
        // A PEER link's socket-close feeds back to the peer state machine
        // inside vws_tcp_svr_uv_close (svr_peer_close_feedback) -- the same
        // discipline this arm pioneered, now on every close path.
        vws_tcp_svr_uv_close(server, (uv_handle_t*)c);
        vws.free(buf->base);
        return;
    }

    // Lookup connection
    uintptr_t ptr = address_pool_get(server->cpool, cid.key);

    if (ptr != 0)
    {
        vws_svr_cnx* cnx = (vws_svr_cnx*)ptr;
        server->on_read(cnx, nread, buf);
    }
}

void svr_on_write_complete(uv_write_t* req, int status)
{
    vws_svr_data_free((vws_svr_data*)req->data);
    vws.free(req);
}

void svr_on_timer_close(uv_handle_t* handle)
{
    vws.free(handle);
}

void svr_on_close(uv_handle_t* handle)
{
    vws_cinfo* cinfo    = (vws_cinfo*)handle->data;
    vws_svr_cnx* cnx    = cinfo->cnx;
    vws_tcp_svr* server = cinfo->server;
    vws_cid_t cid       = cinfo->cid;

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "svr_on_close(%p, %p)", server, handle);
    }

    if (server == NULL)
    {
        assert(false);
        vws.free(handle->data);
        vws.free(handle);

        return;
    }

    // Lookup connection. If it's not in pool then it's already been cleaned up.
    uintptr_t ptr = address_pool_get(server->cpool, cid.key);

    if (ptr != 0)
    {
        vws_svr_cnx* cnx = (vws_svr_cnx*)ptr;

        // Call on_disconnect() handler
        server->on_disconnect(cnx);

        // Cleanup
        svr_cnx_free(cnx);
    }

    vws.free(handle->data);
    vws.free(handle);

    // If we are running in inetd mode, there is only one socket and its closing
    // means we are done and should exit process.

    if ((server->inetd_mode == 1) && vws_tcp_svr_is_running(server))
    {
        vws_tcp_svr_inetd_stop(server);
    }
}

void svr_on_realloc(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
    buf->base = (char*)vws.realloc(buf->base, size);
    buf->len  = size;
}

//------------------------------------------------------------------------------
// Queue API
//------------------------------------------------------------------------------

void queue_init(vws_svr_queue* queue, int size, cstr name)
{
    queue->buffer   = (vws_svr_data**)vws.malloc(size * sizeof(vws_svr_data*));
    queue->size     = 0;
    queue->capacity = size;
    queue->head     = 0;
    queue->tail     = 0;
    queue->state    = VS_RUNNING;
    queue->name     = strdup(name);

    // Initialize mutex and condition variable
    uv_mutex_init(&queue->mutex);
    uv_cond_init(&queue->cond);
}

void queue_destroy(vws_svr_queue* queue)
{
    if (queue->buffer != NULL)
    {
        vws.free(queue->name);
        uv_mutex_destroy(&queue->mutex);
        uv_cond_destroy(&queue->cond);
        vws.free(queue->buffer);
        queue->buffer = NULL;
        queue->state  = VS_HALTED;
    }
}

void queue_push(vws_svr_queue* queue, vws_svr_data* data)
{
    if (queue->state != VS_RUNNING)
    {
        // Drop this hand-off's reference rather than free outright: vws_svr_data
        // is reference-counted, and on the response path the sender holds a
        // second ref it will release after its post-push read. A raw free here
        // would pull the object out from under that read (UAF) and double-free.
        vws_svr_data_free(data);
        return;
    }

    uv_mutex_lock(&queue->mutex);

    while (queue->size == queue->capacity)
    {
        uv_cond_wait(&queue->cond, &queue->mutex);
    }

    if (queue->state != VS_RUNNING)
    {
        uv_cond_signal(&queue->cond);
        uv_mutex_unlock(&queue->mutex);

        // drain-leak: a pusher that blocked on a full queue and woke to find the
        // queue no longer RUNNING still owns this hand-off's reference. Drop it
        // here -- mirroring the pre-lock not-running path above -- rather than
        // returning without freeing (an ownership leak inconsistent with that
        // path). vws_svr_data_free, not a raw free, honors the response path's
        // second ref (refcount).
        vws_svr_data_free(data);
        return;
    }

    queue->buffer[queue->tail] = data;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;

    // Signal condition variable
    uv_cond_signal(&queue->cond);
    uv_mutex_unlock(&queue->mutex);
}

// Non-blocking push. Returns true if the item
// was enqueued, false if the queue is full (caller applies read-side flow
// control instead of blocking) or not running. NEVER blocks -- this is what
// the reactor thread uses so a full request queue can never freeze it.
static bool queue_try_push(vws_svr_queue* queue, vws_svr_data* data)
{
    if (queue->state != VS_RUNNING)
    {
        vws_svr_data_free(data);
        return true;   // consumed (dropped a not-running hand-off); no retry
    }

    uv_mutex_lock(&queue->mutex);

    if (queue->state != VS_RUNNING)
    {
        uv_mutex_unlock(&queue->mutex);
        vws_svr_data_free(data);
        return true;
    }

    if (queue->size == queue->capacity)
    {
        // Full: do NOT block. Tell the caller to pause reads + stash.
        uv_mutex_unlock(&queue->mutex);
        return false;
    }

    queue->buffer[queue->tail] = data;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;

    uv_cond_signal(&queue->cond);
    uv_mutex_unlock(&queue->mutex);

    return true;
}

vws_svr_data* queue_pop(vws_svr_queue* queue)
{
    uv_mutex_lock(&queue->mutex);

    while (queue->size == 0 && queue->state == VS_RUNNING)
    {
        uv_cond_wait(&queue->cond, &queue->mutex);
    }

    if (queue->state == VS_HALTING)
    {
        uv_cond_broadcast(&queue->cond);
        uv_mutex_unlock(&queue->mutex);

        return NULL;
    }

    vws_svr_data* data = queue->buffer[queue->head];
    queue->head        = (queue->head + 1) % queue->capacity;
    queue->size--;

    // [vws V-6] Wake a producer blocked on a FULL queue. push() and pop() share
    // one cond; push waits on (size == capacity) and pop must signal after it
    // frees a slot, or a pusher that blocked on a full queue is never woken --
    // the loop keeps draining responses but the workers stay wedged in
    // queue_push after a flood fills the queue (a permanent lost-wakeup). Right
    // after a successful pop no CONSUMER can be waiting (consumers wait only on
    // size == 0), so this single signal reliably targets a waiting producer.
    uv_cond_signal(&queue->cond);

    uv_mutex_unlock(&queue->mutex);

    return data;
}

bool queue_empty(vws_svr_queue* queue)
{
    uv_mutex_lock(&queue->mutex);
    bool empty = (queue->size == 0);
    uv_mutex_unlock(&queue->mutex);

    return empty;
}

//------------------------------------------------------------------------------
// Pure WebSocket Server
//------------------------------------------------------------------------------

// For handling HTTP requests
int ws_svr_on_http_read(vws_svr_cnx* cnx)
{
    // The the HTTP request complete?
    if (cnx->http->done == true)
    {
        vws_svr* server = (vws_svr*)cnx->server;

        // Pass to some processing function if defined
        if (server->process_http != NULL)
        {
            // Pass message pointer in block
            vws_svr_data* block;
            block = vws_svr_data_own( cnx->server,
                                      cnx->cid,
                                      (ucstr)cnx->http,
                                      sizeof(vws_http_msg*) );

            // Flag this message as HTTP request
            vws_set_flag(&block->flags, VWS_SVR_STATE_HTTP);

            // Queue request
            queue_push(&cnx->server->requests, block);
        }
        else
        {
            // No HTTP processing function defined. Since we take ownership of
            // message to we must up clean up.
            vws_http_msg_free(cnx->http);
        }

        // By returning 1 we tell the caller we handled the request and took
        // owndership of it (freed memory). Caller will allocate a new request
        // and continue with connection processing.
        return 1;
    }

    // By returning 0, we tell caller that the HTTP request is not
    // complete. Keep reading and call us again when more data arrives.
    return 0;
}

void ws_svr_process_frame(vws_cnx* c, vws_frame* f)
{
    vws_svr_cnx* cnx = (vws_svr_cnx*)c->data;

    switch (f->opcode)
    {
        case CLOSE_FRAME:
        {
            // Build the response frame
            vws_buffer* buffer = vws_generate_close_frame();

            // Send back to cliane Send the PONG response
            vws_svr_data* response;

            response = vws_svr_data_new(cnx->server, cnx->cid, &buffer);
            vws_tcp_svr_send(response);

            // Free buffer
            vws_buffer_free(buffer);

            // Free frame
            vws_frame_free(f);

            break;
        }

        case TEXT_FRAME:
        case BINARY_FRAME:
        case CONTINUATION_FRAME:
        {
            // Bound the in-progress reassembled message (twin of
            // websocket.c process_frame). Accumulate the queued frame bytes; if
            // this message's aggregate exceeds the cap, send a 1009 (Message Too
            // Big) close to the client, flag the connection closing, drop the
            // frame, and stop queuing instead of buffering without limit.
            c->msg_bytes += f->size;
            // Two independent layered caps, MIN-WINS
            // (Mike-confirmed): the SRV aggregate-reassembly cap
            // (request_reassembly_cap) AND the per-cnx ws message cap
            // (max_message_size, inherited from server->max_message_size on
            // accept). Both default VWS_MAX_MESSAGE_SIZE = behavior-preserving;
            // the smaller of the two gates an over-size in-progress message.
            size_t reassembly_cap = cnx->server->request_reassembly_cap;
            if (c->max_message_size < reassembly_cap)
            {
                reassembly_cap = c->max_message_size;
            }
            if (c->msg_bytes > reassembly_cap)
            {
                vws_buffer* buffer =
                    vws_generate_close_frame_code(WS_CLOSE_TOO_BIG);
                vws_svr_data* response =
                    vws_svr_data_new(cnx->server, cnx->cid, &buffer);
                vws_tcp_svr_send(response);
                vws_buffer_free(buffer);

                vws_set_flag(&c->flags, CNX_CLOSING);
                vws_frame_free(f);
                c->msg_bytes = 0;

                break;
            }

            // Add to queue
            sc_queue_add_first(&c->queue, f);

            // Message complete: reset the aggregate for the next message.
            if (f->fin == 1)
            {
                c->msg_bytes = 0;
            }

            break;
        }

        case PING_FRAME:
        {
            // Generate the PONG response
            vws_buffer* buffer = vws_generate_pong_frame(f->data, f->size);

            // Send back to cliane Send the PONG response
            vws_svr_data* response;
            response = vws_svr_data_new(cnx->server, cnx->cid, &buffer);
            vws_tcp_svr_send(response);

            // Free buffer
            vws_buffer_free(buffer);

            // Free frame
            vws_frame_free(f);

            break;
        }

        case PONG_FRAME:
        {
            // No need to send a response

            vws_frame_free(f);

            break;
        }

        default:
        {
            // Invalid frame type
            vws_frame_free(f);
        }
    }

    vws.success();
}

void ws_svr_client_connect(vws_svr_cnx* c)
{
    if (vws.tracelevel >= VT_SERVICE)
    {
        cstr host;
        int port;

        struct sockaddr* addr = (struct sockaddr*)&c->cid.addr;
        if (vws_socket_addr_info(addr, &host, &port) == true)
        {
            vws.trace( VL_INFO,
                       "ws_svr_client_connect(%p, %p) socket %s:%lu",
                       c->server, c->handle, host, port);
            free(host);
        }
        else
        {
            vws.trace(VL_INFO, "ws_svr_client_connect(%p)", c->handle);
        }
    }

    // Create a new vws_cnx
    vws_cnx* cnx = (void*)vws_cnx_new();
    cnx->process = ws_svr_process_frame;

    // Inherit the server-level max message size
    // default onto this per-connection ws cnx (broker sets server->
    // max_message_size from vrtql.conf; default VWS_MAX_MESSAGE_SIZE =
    // behavior-preserving).
    cnx->max_message_size = c->server->max_message_size;

    cnx->data    = (void*)c;   // Link cnx -> c
    c->data      = (void*)cnx; // Link c -> cnx
}

void ws_svr_client_disconnect(vws_svr_cnx* c)
{
    if (c == NULL)
    {
        return;
    }

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "ws_svr_client_disconnect(%p)", c->handle);
    }

    if (c->data != NULL)
    {
        vws_cnx_free((vws_cnx*)c->data);
        c->data = NULL;
    }
}

// Runs in uv_thread()
void ws_svr_client_read(vws_svr_cnx* cnx, ssize_t size, const uv_buf_t* buf)
{
    vws_svr* server = (vws_svr*)cnx->server;
    vws_cnx* c      = (vws_cnx*)cnx->data;

    // Add to client socket buffer
    vws_buffer_append(c->base.buffer, (ucstr)buf->base, size);

    // Free libuv memory
    vws.free(buf->base);

    // If we are in HTTP mode
    if (cnx->upgraded == false)
    {
        // Parse incoming data as HTTP request. Loop: one read event may
        // carry several pipelined COMPLETE requests, and this function is
        // the only parse entry and only runs on a new event -- without the
        // loop a fully buffered second request waits for client bytes that
        // may never come. Each pass either consumes one complete request
        // (and re-enters), leaves a partial tail to the drain arm below
        // (done == false), or exits via upgrade/close/return.
        while (true)
        {

            ucstr data  = c->base.buffer->data;
            size_t size = c->base.buffer->size;
            ssize_t n   = vws_http_msg_parse(cnx->http, (cstr)data, size);

            // Fatal parse error before the request completed. If the total-request
            // size cap was exceeded, reply with the matching status -- 414 (URI Too
            // Long, RFC 7231) if the request-line tripped it, else 431 (Request
            // Header Fields Too Large, RFC 6585) -- before closing. Either way close
            // the connection rather than keep buffering an oversized/broken request
            // pre-handshake.
            if (n < 0)
            {
                int status = cnx->http->oversize_status;

                if (status == 414 || status == 431)
                {
                    cstr reason = (status == 414) ? "URI Too Long"
                                                  : "Request Header Fields Too Large";
                    vws_buffer* http = vws_buffer_new();
                    vws_buffer_printf(http, "HTTP/1.1 %d %s\r\n", status, reason);
                    vws_buffer_printf(http, "Connection: close\r\n");
                    vws_buffer_printf(http, "Content-Length: 0\r\n\r\n");

                    vws_svr_data* reply = vws_svr_data_new(cnx->server, cnx->cid, &http);
                    cnx->server->on_data_out(reply, NULL);
                    vws_buffer_free(http);
                }

                svr_cnx_close(cnx->server, cnx->cid);
                return;
            }

            // Did we get a complete request? The gate is `done` (set only by
            // on_message_complete, which pauses the parser), NOT headers_complete:
            // a read event that completes the HEADERS but not the BODY leaves
            // llhttp at HPE_OK with done==false, and entering this branch then
            // misread HPE_OK as a parse error -- every bodied request (POST/PUT)
            // whose body arrived in a later read event was closed with no reply.
            // With the done gate, that event falls through to the incomplete-parse
            // drain arm below and the errno check here only ever sees HPE_PAUSED.
            // (ws_svr_on_http_read gates on done the same way.)
            if (cnx->http->done == true)
            {
                // Check for parsing errors
                enum llhttp_errno err = llhttp_get_errno(cnx->http->parser);

                // If there was a parsing error, close connection
                if(err != HPE_PAUSED)
                {
                    vws.error( VE_RT, "Error: %s (%s)",
                               llhttp_errno_name(err),
                               llhttp_get_error_reason(cnx->http->parser) );

                    // Close connection
                    svr_cnx_close(cnx->server, cnx->cid);

                    return;
                }

                // Drain HTTP request data from cnx->data buffer
                vws_buffer_drain(c->base.buffer, n);

                //> Generate HTTP response and send

                vws_kvs* headers = cnx->http->headers;

                // Check if it's an upgrade request
                cstr upgrade = vws_kvs_get_cstring(headers, "upgrade");

                if (upgrade == NULL)
                {
                    // Handle regular HTTP request via callback
                    if (server->on_http_read)
                    {
                        int rc = server->on_http_read(cnx);

                        if (rc == 0)
                        {
                            // Keep reading
                            return;
                        }

                        if (rc == 1)
                        {
                            // Allocate new message
                            cnx->http = vws_http_msg_new(HTTP_REQUEST);

                            // A pipelining client may have written further
                            // requests into this same event's buffer. Serve
                            // any COMPLETE one now: re-enter the parse with
                            // the fresh parser. A partial tail parses to
                            // done == false and drains below; an empty
                            // buffer is done for this event.
                            if (c->base.buffer->size > 0)
                            {
                                continue;
                            }
                        }

                        if (rc == -1)
                        {
                            // We are not handling HTTP requests
                            svr_cnx_close(cnx->server, cnx->cid);
                        }
                    }
                    else
                    {
                        // We are not handling HTTP requests
                        svr_cnx_close(cnx->server, cnx->cid);
                    }

                    return;
                }

                cstr key   = vws_kvs_get_cstring(headers, "sec-websocket-key");
                cstr proto = vws_kvs_get_cstring(headers, "sec-websocket-protocol");

                if (key == NULL)
                {
                    vws.error(VE_RT, "Error: missing sec-websocket-key");

                    // Close connection
                    svr_cnx_close(cnx->server, cnx->cid);

                    return;
                }

                vws_buffer* http = vws_buffer_new();

                vws_buffer_printf(http, "HTTP/1.1 101 Switching Protocols\r\n");
                vws_buffer_printf(http, "Upgrade: websocket\r\n");
                vws_buffer_printf(http, "Connection: Upgrade\r\n");

                cstr ac = vws_accept_key(key);
                vws_buffer_printf(http, "Sec-WebSocket-Accept: %s\r\n", ac);
                vws.free(ac);

                vws_buffer_printf(http, "Sec-WebSocket-Version: 13\r\n");
                vws_buffer_printf(http, "Sec-WebSocket-Protocol: ");

                if (proto != NULL)
                {
                    vws_buffer_printf(http, "%s\r\n", proto);
                }
                else
                {
                    vws_buffer_printf(http, "vrtql\r\n");
                }

                vws_buffer_printf(http, "\r\n");

                // Package up response
                vws_svr_data* reply;
                reply = vws_svr_data_new(cnx->server, cnx->cid, &http);

                // Send directly out as we are in uv_thread()
                cnx->server->on_data_out(reply, NULL);

                // Cleanup. Buffer data was passed to reply.
                vws_buffer_free(http);

                //> Change state to WebSocket mode

                // Set the flag that we are in WebSocket mode
                cnx->upgraded = true;

                // Notify application of the upgrade
                if (server->on_upgrade != NULL)
                {
                    server->on_upgrade(cnx);
                }

                // Free HTTP request as we don't need it anymore
                vws_http_msg_free(cnx->http);
                cnx->http = NULL;

                // Do we have any data in the socket after consuming the HTTP
                // request? We shouldn't but if so this is WebSocket data.
                if (c->base.buffer->size == 0)
                {
                    // No more data in the socket buffer. Done for now.
                    return;
                }
            }
            else
            {
                // No complete HTTP request yet. llhttp has CONSUMED every byte fed
                // this event (vws_http_msg_parse returns size for an incomplete
                // message) and holds the parse state, so drain them -- mirroring the
                // headers-complete drain above -- so the next read event feeds ONLY
                // new bytes. Leaving them re-fed the already-consumed prefix into
                // the mid-state parser on the next event: a fatal parse error that
                // closed any split request with no reply, and made the 414 oversize
                // reply unreachable (an oversized request line always spans read
                // events).
                vws_buffer_drain(c->base.buffer, n);

                return;
            }

            // Only the upgrade path falls out of the request arm: WebSocket
            // bytes buffered behind the 101 are handled below.
            break;
        }

        // If we get here, we have a complete request and we have incoming
        // WebSocket data to process (c->base.buffer->size > 0)
    }

    if (vws_cnx_ingress(c) > 0)
    {
        // Process as many messages as possible
        while (true)
        {
            // Check for a complete message
            vws_msg* wsm = vws_msg_pop(c);

            if (wsm == NULL)
            {
                return;
            }

            // Pass message pointer in block
            vws_svr_data* block;
            block = vws_svr_data_own( cnx->server,
                                      cnx->cid,
                                      (ucstr)wsm,
                                      sizeof(vws_msg*) );
            queue_push(&cnx->server->requests, block);
        }
    }
}

// Runs in worker_thread()
void ws_svr_client_data_in(vws_svr_data* block, void* x)
{
    //> Append data to connection buffer

    vws_cid_t cid   = block->cid;
    vws_svr* server = (vws_svr*)block->server;

    // If this is an internal request
    if (vws_is_flag(&block->flags, VWS_SVR_STATE_IRQ))
    {
        // If handler defined
        if (server->process_irq != NULL)
        {
            // Process request
            server->process_irq(server, cid, block->data, block->size, x);
        }

        // Free incoming data
        block->data = NULL;
        block->size = 0;
        vws_svr_data_free(block);

        return;
    }

    // If this is a HTTP request
    if (vws_is_flag(&block->flags, VWS_SVR_STATE_HTTP))
    {
        // Data simply contains a pointer to a websocket message
        vws_http_msg* req = (vws_http_msg*)block->data;

        // Free incoming data
        block->data = NULL;
        block->size = 0;
        vws_svr_data_free(block);

        // Process message
        if (server->process_http(server, cid, req, x) == false)
        {
            // If processing function returns false, we close connection.
            vws_tcp_svr_close((vws_tcp_svr*)server, cid);
        }

        // Since we take ownership of message to we must up clean up.
        vws_http_msg_free(req);

        return;
    }

    // If this is a peer which needs connecting
    if (vws_is_flag(&block->flags, VWS_SVR_STATE_PEER_CONNECT))
    {
        // Data simply contains a pointer to a websocket message
        vws_peer* peer = (vws_peer*)block->data;

        // Free incoming data
        block->data = NULL;
        block->size = 0;
        vws_svr_data_free(block);

        // Resolve
        struct sockaddr_storage* addr = &peer->info.cid.addr;
        if (svr_resolve(peer->host, peer->port, &addr) == false)
        {
            vws.error( VL_WARN,
                       "vws_tcp_svr_peer_add(): "
                       "Failed to resolve host %s:%lu",
                       peer->host, peer->port );

            // Set state back to closed
            peer->state = VWS_PEER_CLOSED;

            return;
        }

        // Connect
        if (vws_tcp_svr_peer_connect((vws_tcp_svr*)server, peer, x) == false)
        {
            // Set state back to closed
            peer->state = VWS_PEER_CLOSED;
        }
        else
        {
            // Set state back to connected. uv_thread() will add it to
            // connection pool.
            peer->state = VWS_PEER_RECONNECTED;
        }

        // We don't need to send message back regarding this request, but we do
        // need the server to wakeup and realize that this peer connection state
        // has changed and needs to be attended to.
        uv_async_send(((vws_tcp_svr*)server)->wakeup);

        return;
    }

    // Data simply contains a pointer to a websocket message
    vws_msg* wsm = (vws_msg*)block->data;

    // Free incoming data
    block->data = NULL;
    block->size = 0;
    vws_svr_data_free(block);

    //> Process connection buffer data for complete messages
    server->on_msg_in(server, cid, wsm, x);
}

void ws_svr_client_data_out( vws_svr* server,
                             vws_cid_t cid,
                             vws_buffer* buffer,
                             unsigned char opcode )
{
    // Create a binary websocket frame containing message
    vws_frame* frame;
    ucstr data  = buffer->data;
    size_t size = buffer->size;
    frame       = vws_frame_new(data, size, opcode);

    // This frame is from server to we don't mask it
    frame->mask = 0;

    // Serialize frame: frame is freed by function
    vws_buffer* fdata = vws_serialize(frame);

    // Pack frame binary into queue data
    vws_svr_data* response;

    response = vws_svr_data_new((vws_tcp_svr*)server, cid, &fdata);

    // Queue the data to uv_thread() to send out on wire
    vws_tcp_svr_send(response);

    // Free buffers
    vws_buffer_free(fdata);
}

void ws_svr_client_msg_in(vws_svr* s, vws_cid_t c, vws_msg* m, void* x)
{
    vws_svr* server = (vws_svr*)s;

    // Route to application-specific processing callback
    server->process_ws(server, c, m, x);
}

void ws_svr_client_process(vws_svr* s, vws_cid_t c, vws_msg* m, void* x)
{
    // Default: Do nothing. Drop message.
    vws_msg_free(m);
}

void ws_svr_client_msg_out(vws_svr* s, vws_cid_t c, vws_msg* m, void* x)
{
    ws_svr_client_data_out(s, c, m->data, m->opcode);
    vws_msg_free(m);
}

void ws_svr_ctor(vws_svr* server, int nt, int bl, int qs)
{
    tcp_svr_ctor((vws_tcp_svr*)server, nt, bl, qs);

    // Server base function overrides
    server->base.on_connect    = ws_svr_client_connect;
    server->base.on_disconnect = ws_svr_client_disconnect;
    server->base.on_read       = ws_svr_client_read;
    server->base.on_data_in    = ws_svr_client_data_in;

    // Message handling
    server->on_msg_in          = ws_svr_client_msg_in;
    server->on_msg_out         = ws_svr_client_msg_out;

    // HTTP handlers
    server->on_http_read       = ws_svr_on_http_read;
    server->process_http       = NULL;

    // Upgrade callback
    server->on_upgrade         = NULL;

    // Internal request handler
    server->process_irq        = NULL;

    // Application functions
    server->process_ws         = ws_svr_client_process;
    server->send               = ws_svr_client_msg_out;
}

vws_svr* vws_svr_new(int num_threads, int backlog, int queue_size)
{
    vws_svr* server = vws.malloc(sizeof(vws_svr));
    ws_svr_ctor(server, num_threads, backlog, queue_size);
    return server;
}

void ws_svr_dtor(vws_svr* server)
{
    if (server == NULL)
    {
        return;
    }

    tcp_svr_dtor((vws_tcp_svr*)server);
}

void vws_svr_free(vws_svr* server)
{
    if (server == NULL)
    {
        return;
    }

    ws_svr_dtor(server);
    vws.free(server);
}

int vws_svr_run(vws_svr* server, cstr host, int port)
{
    // The WebSocket server has no run loop of its own; it is driven through its
    // base. vws_svr embeds vws_tcp_svr as its first member, so the cast is
    // layout-sound.
    return vws_tcp_svr_run((vws_tcp_svr*)server, host, port);
}

void vws_svr_send_irq(vws_svr* s, vws_cid_t cid, void* data, size_t size)
{
    vws_tcp_svr* svr = (vws_tcp_svr*)s;

    // Queue data to worker pool for processing
    vws_svr_data* block = vws_svr_data_own(svr, cid, data, size);

    // Set IRQ flag
    vws_set_flag(&block->flags, VWS_SVR_STATE_IRQ);

    // Store reference to server
    block->server = svr;

    // Put on queue
    queue_push(&svr->requests, block);
}

//------------------------------------------------------------------------------
// Messaging Server: Derived from WebSocket server
//------------------------------------------------------------------------------

// Convert incoming WebSocket messages to VRTQL messages for processing
void msg_svr_client_ws_msg_in(vws_svr* s, vws_cid_t cid, vws_msg* wsm, void* x)
{
    // Deserialize message

    vrtql_msg* msg = vrtql_msg_new(HTTP_REQUEST);
    ucstr data     = wsm->data->data;
    size_t size    = wsm->data->size;

    if (vrtql_msg_deserialize(msg, data, size) == false)
    {
        // Deserialized failed

        // Error already set
        vws_msg_free(wsm);
        vrtql_msg_free(msg);

        vws_tcp_svr_close((vws_tcp_svr*)s, cid);

        return;
    }

    // Deserialized succeeded

    // TODO: We send back the format we get. More than that, a request with
    // different format effectively changes the entire connection default format
    // setting.
    //
    // cnx->format = msg->format;

    // Free websocket message
    vws_msg_free(wsm);

    // Process message
    vrtql_msg_svr* server = (vrtql_msg_svr*)s;
    server->on_msg_in(s, cid, msg, x);
}

// Receive/handle incoming VRTQL messages
void msg_svr_client_msg_in(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)s;

    // Route to application-specific processing callback
    server->process(s, c, m, x);
}

// Send VRTQL messages
void msg_svr_client_msg_out(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x)
{
    // Serialize message
    vws_buffer* mdata = vrtql_msg_serialize(m);

    // Send to base class
    ws_svr_client_data_out(s, c, mdata, BINARY_FRAME);

    // Cleanup
    vws_buffer_free(mdata);
    vrtql_msg_free(m);
}

// Send VRTQL messages
void msg_svr_client_msg_dispatch(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x)
{
    // Serialize message
    vws_buffer* mdata = vrtql_msg_serialize(m);

    // Send to base class
    ws_svr_client_data_out(s, c, mdata, BINARY_FRAME);

    // Cleanup
    vws_buffer_free(mdata);
}

// Process incoming VRTQL messages
void msg_svr_client_process(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x)
{
    // Default: Do nothing. Drop message.
    vrtql_msg_free(m);
}

vrtql_msg_svr* vrtql_msg_svr_new(int num_threads, int backlog, int queue_size)
{
    vrtql_msg_svr* server = vws.malloc(sizeof(vrtql_msg_svr));
    return vrtql_msg_svr_ctor(server, num_threads, backlog, queue_size);
}

void vrtql_msg_svr_free(vrtql_msg_svr* server)
{
    vrtql_msg_svr_dtor(server);
}

int vrtql_msg_svr_run(vrtql_msg_svr* server, cstr host, int port)
{
    // The message server has no run loop of its own; it is driven through its
    // base. vrtql_msg_svr embeds vws_svr (which embeds vws_tcp_svr) as its first
    // member, so the cast to the base tcp server is layout-sound.
    return vws_tcp_svr_run((vws_tcp_svr*)server, host, port);
}

vrtql_msg_svr*
vrtql_msg_svr_ctor(vrtql_msg_svr* server, int threads, int backlog, int qsize)
{
    ws_svr_ctor((vws_svr*)server, threads, backlog, qsize);

    // Server base function overrides
    server->base.process_ws = msg_svr_client_ws_msg_in;

    // Message handling
    server->on_msg_in       = msg_svr_client_msg_in;
    server->on_msg_out      = msg_svr_client_msg_out;

    // Application functions
    server->process        = msg_svr_client_process;
    server->send           = msg_svr_client_msg_out;
    server->dispatch       = msg_svr_client_msg_dispatch;

    // User-defined data
    server->data           = NULL;

    return server;
}

void vrtql_msg_svr_dtor(vrtql_msg_svr* server)
{
    if (server == NULL)
    {
        return;
    }

    ws_svr_dtor((vws_svr*)server);
    vws.free(server);
}
