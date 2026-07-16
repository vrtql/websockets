#ifndef VRTQL_SVR_DECLARE
#define VRTQL_SVR_DECLARE

#if defined(__windows__)
// uv.h pulls <winsock2.h> and <windows.h> on Windows. Without
// WIN32_LEAN_AND_MEAN, <windows.h> includes the RPC header chain, whose
// <rpc.h> resolves to vws's own src/rpc.h (src/ is on the include path),
// dragging the vws header chain into the middle of <windows.h> before SOCKET
// is defined. WIN32_LEAN_AND_MEAN excludes the RPC chain entirely. Mirrors the
// guard in socket.c. Must precede <uv.h>.
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  // Windows 7 or later
#endif
#endif

#include <uv.h>

/*
 * C11 atomics in the public struct layout. server.c (C) sees `_Atomic T`; this
 * header is also included by C++ TUs (e.g. a broker's mq.cpp), where `_Atomic`
 * is not a keyword and <stdatomic.h> does not compile -- so present std::atomic<T>
 * there. The two are layout-compatible for these integral T on the supported
 * toolchains, so the C/C++ views of these structs agree within one binary.
 */
#if defined(__cplusplus)
#include <atomic>
#define VWS_ATOMIC(T) ::std::atomic<T>
#else
#include <stdatomic.h>
#define VWS_ATOMIC(T) _Atomic T
#endif

#include "vws.h"
#include "message.h"
#include "http_message.h"

/**
 * @file server.h
 * @brief WebSocket server implementation
 *
 * This file implements a non-blocking, multiplexing, multithreaded WebSocket
 * server using the libuv library. The server consists of a main networking
 * thread that runs the libuv loop to handle socket I/O, and a pool of worker
 * threads that process the data.
 *
 * The networking thread evenly distributes incoming data from clients to the
 * worker threads using a synchronized queue. The worker threads process the
 * data and may send back replies. The server maintains two queues: a request
 * queue that transfers incoming client data to the worker threads for
 * processing, and a response queue that transfers data from the worker threads
 * back to the network thread for sending it back to the client.
 *
 * The data items are stored in a generic structure called vws_svr_data, which
 * holds the data and the associated connection. The worker threads retrieve
 * data from the request queue and perform the following steps:
 *
 *     1. Assemble data into WebSocket frames.
 *     2. Assemble frames into WebSocket messages.
 *     3. Assemble WebSocket messages into VRTQL messages.
 *     4. Pass the messages to a user-defined processing function.
 *
 * The processing function can optionally produce data to send back to the
 * client.
 *
 * From a programming standpoint, the server architecture simplifies to
 * implementing two functions that run in the context of a worker thread:
 *
 *     - process(request message): Process incoming messages.
 *     - send(reply message): Send back reply messages.
 *
 * The server takes care of all the serialization, handling the communication
 * between the network thread and worker threads. Developers only need to focus
 * on the processing logic.
 *
 * The number of worker threads is configurable, allowing you to adjust it based
 * on workload or system requirements.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct address_pool
 * @brief Manages a dynamically resizable pool of addresses.
 *
 * The address_pool structure is designed to handle a collection of memory
 * addresses. It is a dynamically resizable array that works like ring
 * buffer. It supports efficient addition, lookup and removal of items while
 * automatically handling memory resizing based on usage. It's main use is to
 * track and identify connections. It is significantly faster a hashtable which
 * provided much better overall server performance.
 *
 * Connections change over time and for the most part keep moving forward in the
 * ring buffer in a sort of statistical distribtion. By the time it reachs the
 * end of the ring to wrap around, all previous connections at the beginning
 * have most likely disconnected.
 *
 * @details
 *
 * - The pool grows in capacity by a specified growth factor each time the array
 *   reaches its current capacity limit. This growth behavior helps in managing
 *   memory more efficiently by minimizing the number of memory reallocations
 *   required as the number of items grows.
 *
 * - The implementation uses a ring buffer approach with a last used index to
 *   optimize the search for free slots, allowing for O(1) time complexity for
 *   addition in most cases, barring the necessity for resizing.
 */

typedef struct
{
    /**< Pointer to the array holding memory addresses or similar values */
    uintptr_t* slots;

    /**< Total number of slots in the array */
    uint32_t capacity;

    /**< Number of used slots */
    uint32_t count;

    /**< Last used index to optimize search for empty slots */
    uint32_t last_used_index;

    /**< Per-slot reuse generation, packed into a key's bits 32..62. A slot's
     * generation advances when the slot is freed, so a key issued before the
     * free can never resolve (or remove) the slot's next resident. */
    uint32_t* generations;

    /**< Factor by which the array size is increased upon realloc */
    uint16_t growth_factor;
} address_pool;

/**
 * @brief Creates a new address pool.
 *
 * This function allocates and initializes an address pool with specified
 * initial size and growth factor. Memory for the pool and its slots is
 * allocated dynamically.
 *
 * @param initial_size The initial number of slots in the pool.
 * @param growth_factor The factor by which the pool size is increased when
 *   resized.
 * @return address_pool* A pointer to the newly created address pool or NULL if
 *   allocation fails.
 */
address_pool* address_pool_new(int initial_size, int growth_factor);

/**
 * @brief Frees the memory associated with an address pool.
 *
 * This function deallocates the memory for the address pool and sets the
 * pointer to NULL to prevent dangling references. It safely handles NULL
 * pointers.
 *
 * @param pool A pointer to the pointer of the address pool to be freed.
 */
void address_pool_free(address_pool** pool);

/**
 * @brief Resizes an address pool to accommodate more items.
 *
 * The function increases the capacity of the address pool based on its growth
 * factor. Existing items are preserved, and additional space is initialized to
 * zero. If memory allocation fails, the pool's slots pointer remains unchanged.
 *
 * @param pool A pointer to the address pool to be resized.
 */
void address_pool_resize(address_pool* pool);

/**
 * @brief Adds a new item to the address pool.
 *
 * This function adds a new item to the address pool. If the pool is full, it is
 * resized. The function searches for the first free slot to use, starting from
 * the last used index and wrapping around if necessary.
 *
 * @param pool A pointer to the address pool.
 * @param address The uintptr_t item to be added to the pool.
 * @return int The index at which the item was added, or -1 if resizing failed.
 */
int64_t address_pool_set(address_pool* pool, uintptr_t address);

/**
 * @brief Retrieves the item stored at the specified index in the address pool.
 *
 * This function returns the value at a given index in the address pool's slots
 * array. If the index is out of bounds or the slot at the index is empty
 * (zero), the function returns 0. This method is intended for quick access to
 * items in the pool without any modifications.
 *
 * @param pool A pointer to the address pool.
 * @param index The index of the item to retrieve.
 * @return uintptr_t The value at the specified index if it exists and is not
 *   empty; otherwise, 0.
 */
uintptr_t address_pool_get(address_pool* pool, int64_t index);

/**
 * @brief Removes an item from the address pool.
 *
 * This function sets the slot at the specified index to zero, marking it as
 * free. The count of used slots is decremented.
 *
 * @param pool A pointer to the address pool.
 * @param index The index of the slot to be freed.
 */
void address_pool_remove(address_pool* pool, int64_t index);

struct vws_svr_cnx;
struct vws_svr;

// We start at 10th bit so these can be used in vrtql_msg flags (which use bits
// 1-3 with 4-9 reserved for future use)
typedef enum
{
    VWS_SVR_STATE_CLOSE        = (1 << 10),
    VWS_SVR_STATE_AUTH         = (1 << 11),
    VWS_SVR_STATE_UNAUTH       = (1 << 12),
    VWS_SVR_STATE_PEER         = (1 << 13),
    VWS_SVR_STATE_HTTP         = (1 << 14),
    VWS_SVR_STATE_PEER_CONNECT = (1 << 15),
    VWS_SVR_STATE_TRUSTED      = (1 << 16),
    VWS_SVR_STATE_IRQ          = (1 << 17)
} vws_svr_state_flags_t;

/** Connection ID. This is the index within the address pool that the
 * connection's pointer is stored. */
typedef struct vws_cid_t
{
    int64_t key;
    struct sockaddr_storage addr;
    uint64_t flags;
    uint16_t plane;
    void* data;
} vws_cid_t;

/**
 * @brief Clears cid structure so that it is in an invalid state
 *
 * @param cid The connection ID
 */
void vws_cid_clear(vws_cid_t* cid);

/**
 * @brief Clears cid structure so that it is in an invalid state
 *
 * @param cid The connection ID
 */
bool vws_cid_valid(vws_cid_t* cid);

/** This is used to associate connection info with uv_stream_t handles */
typedef struct vws_cinfo
{
    struct vws_tcp_svr* server;
    struct vws_svr_cnx* cnx;
    vws_cid_t cid;
} vws_cinfo;

typedef enum
{
    VWS_PEER_CLOSED      = 1,
    VWS_PEER_CONNECTED   = 2,
    VWS_PEER_PENDING     = 3,
    VWS_PEER_RECONNECTED = 4,
    VWS_PEER_FAILED      = 5
} vws_peer_state_t;

struct vws_peer;

/**
 * @brief Callback for establishing new peer connection.
 *
 * @param peer The peer struct (containing host and IP)
 * @param x The user-defined context
 * @return Returns the established socket descriptor upon successful connection,
 *   VWS_INVALID_SOCKET on failure.
*/
typedef vws_sockfd_t (*vws_peer_connect)(struct vws_peer* p, void* x);

/** This is used to associate connection info with uv_stream_t handles */
typedef struct vws_peer
{
    cstr host;
    int port;
    struct vws_cinfo info;

    /**< Peer connection state. CROSS-THREAD + CROSS-LIBRARY, intentionally
     * PLAIN (non-_Atomic). Written from two threads inside libvws: the uv loop
     * thread (CLOSED->PENDING, and it consumes RECONNECTED) and a worker thread
     * (the PEER_CONNECT hand-off in ws_svr_client_data_in writes
     * CLOSED/RECONNECTED + sockfd). It is also read directly by the broker
     * (libvsa, C++: broker.cpp state==VWS_PEER_CONNECTED checks) to decide peer
     * liveness. Every transition is ordered by a queue-mutex critical section
     * plus the uv_async_send/queue_pop happens-before edge between the worker
     * and the loop, so on the supported (TSO) memory model the reader always
     * observes a consistent value -- there is no torn or reordered store in
     * practice. It is left PLAIN rather than _Atomic on purpose: making it
     * _Atomic would change the vws_peer struct type and force the broker's plain
     * C++ loads to interoperate with a C11 _Atomic field across the language
     * boundary (ABI + atomics-interop change, coordinated vws+libvsa rebuild).
     * The formal C11 data race (which ThreadSanitizer will flag) is accepted
     * and documented here; if a transition is ever added OFF these two ordered
     * paths, revisit (confine writes to the loop thread, or make it atomic and
     * take the ABI change). Mike-approved 2026-07-16. */
    vws_peer_state_t state;
    vws_sockfd_t sockfd;
    vws_peer_connect connect;
    void* data;

    /**< Duration-track anchor: the time this peer FIRST became unreachable,
     * stamped once while it is not CONNECTED and cleared on the successful
     * CONNECTED transition. A single timestamp (not a per-attempt counter)
     * covers both never-was-up and was-up-then-froze, and is retry-stateless
     * (dials do not reset it), so elapsed accumulates until recovery. 0 when
     * the peer is (or has recovered to) connected. Monotonic ms (vws_now_ms);
     * uint64_t, same 8-byte width as the former time_t on LP64. */
    uint64_t first_unreachable_ts;

    /**< True once peer_unrecoverable_cb has fired for the current unreachable
     * span, so it fires exactly once per span. Cleared with
     * first_unreachable_ts on the CONNECTED transition. */
    bool unrecoverable_reported;
} vws_peer;

struct vws_tcp_svr;

/**
 * @brief Struct representing server data for inter-thread communication
 * between the main network thread and worker threads. This is the way data is
 * passed between them. When passed from the network thread to the worker
 * thread, these take the form of incoming data from the client. When passed
 * from the worker thread to the network thread, they represent outgoing data to
 * the client.
 */
typedef struct
{
    /**< The client connection index associated with the data */
    vws_cid_t cid;

    /**< The number of bytes of data */
    size_t size;

    /**< The data */
    char* data;

    /**< Message state flags */
    uint64_t flags;

    /**< Reference to server this data belongs to */
    struct vws_tcp_svr* server;

    /**< Atomic reference count guarding the object's lifetime across the
     * worker/loop hand-off. Created at 1 (the creator's ref); the send path
     * takes a second ref BEFORE handing the data to the loop so the loop's
     * free (svr_on_write_complete) cannot race the worker's post-hand-off
     * read. Every holder releases via vws_svr_data_free; the object is freed
     * only when the count reaches 0. */
    VWS_ATOMIC(int) refs;

} vws_svr_data;

/**
 * @brief Struct representing a server queue, including information about
 * buffer, size, capacity, and threading.
 */
typedef struct
{
    /**< The buffer holding data in the queue */
    vws_svr_data** buffer;

    /**< Current size of the queue */
    int size;

    /**< Maximum capacity of the queue */
    int capacity;

    /**< Head position of the queue */
    int head;

    /**< Tail position of the queue */
    int tail;

    /**< Mutex for thread safety */
    uv_mutex_t mutex;

    /**< Condition variable for thread synchronization */
    uv_cond_t cond;

    /**< Current state of the queue */
    VWS_ATOMIC(uint8_t) state;

    /**< Queue name */
    cstr name;

} vws_svr_queue;

struct vws_tcp_svr;

/**
 * @brief Represents a client connection.
 */
typedef struct vws_svr_cnx
{
    /**< The server associated with the connection */
    struct vws_tcp_svr* server;

    /**< The client associated with the connection */
    uv_stream_t* handle;

    /* Flag holds the HTTP request that started connection. */
    vws_http_msg* http;

    /** Flag that holds whether we have upgraded connection from HTTP to
     *  WebSockets */
    bool upgraded;

    /** Index in connection address pool */
    vws_cid_t cid;

    /**< User-defined data associated with the connection */
    char* data;

    /**< The format to serialize. If VM_MPACK_FORMAT, serialize into MessagePack
     *   binary format. If VM_JSON_FORMAT, then serialize into JSON format.
     */
    vrtql_msg_format_t format;

    /**< Read-side flow control. When the
     * request queue is full, svr_client_read uv_read_stop()s this socket (so
     * the kernel applies TCP backpressure) rather than blocking the reactor in
     * queue_push. read_paused records that; pending holds the one in-flight
     * item that could not be pushed (at most one, since reads are stopped). On
     * queue drain the reactor pushes `pending` and uv_read_start()s again. */
    bool           read_paused;
    vws_svr_data*  pending;

} vws_svr_cnx;

/**
 * @brief Callback for process each loop. Called from uv_thread().
 *
 * @param data A pointer to the vws_tcp_svr instance
*/
typedef void (*vws_svr_loop_cb)(void* data);

/**
 * @brief Callback for process each loop. Called from uv_thread().
 *
 * @param data A pointer to the data lost
 * @param x The user-defined context
*/
typedef void (*vws_svr_data_lost_cb)(vws_svr_data* data, void* x);

/**
 * @brief Callback for processing a new connection. Called from uv_thread().
 *
 * @param cnx The new connection.
 * @return Returns true if connection is approved, false otherwise. If false is
 * returned, connection will be closed.
*/
typedef bool (*vws_svr_cnx_open_cb)(struct vws_svr_cnx* cnx);

/**
 * @brief Callback for processing a closing connection. Called from uv_thread().
 *
 * @param cnx The new connection.
*/
typedef void (*vws_svr_cnx_close_cb)(struct vws_svr_cnx* cnx);

/** Thread context constructor (factory) function */
typedef void* (*vws_thread_ctx_ctor)(void* data);

/** Thread context constructor (factory) function */
typedef void (*vws_thread_ctx_dtor)(void* data);

/**
 * @brief Represents a worker thread context
 *
 * Thread Context. Worker threads can carry a user-defined context which they
 * pass to process_message() handlers. They call a factory method to create this
 * context. Upon exit, they likewise call a factory method to destruct it.
 *
 * This context acts as a lifelong state within the worker_thread which provides
 * tasks with additional resources to do their job. This context is ideal for
 * maintaining state across tasks like database connections.
 */
typedef struct vws_thread_ctx
{
    /**< Context constructor: A pointer to a function which creates the thread
     * context. The context is returned as a void* pointer and is stored in the
     * data member. */
    vws_thread_ctx_ctor ctor;

    /**< Context constructor data: A pointer to optional user-data passed into
     * the ctor to help in context construction. */
    void* ctor_data;

    /**< Context destructor: A pointer to a function which deallocates the
     * thread context. This is where for worker thread shutdown used to clean up
     * resources. */
    vws_thread_ctx_dtor dtor;

    /**< Thread context: This points to the context allocated by ctor. It will
     * be passed passed to from worker thread to process_message(void* ctx). */
    void* data;
} vws_thread_ctx;

/**
 * @brief Callback for a new connection connection
 * @param c The connection structure
 */
typedef void (*vws_tcp_svr_connect)(vws_svr_cnx* c);

/**
 * @brief Callback for connection disconnection
 * @param c The connection structure
 */
typedef void (*vws_tcp_svr_disconnect)(vws_svr_cnx* c);

/**
 * @brief Callback for connection read
 * @param c The connection structure
 * @param n The number of bytes in the buffer
 * @param b The buffer
 */
typedef void (*vws_tcp_svr_read)(vws_svr_cnx* c, ssize_t n, const uv_buf_t* b);

/**
 * @brief Callback for data processing within a worker thread
 * @param s The server instance
 * @param t The incoming request to process
 */
typedef void (*vws_tcp_svr_process_data)(vws_svr_data* t, void* data);

/**
 * @brief Enumerates server states
 */
typedef enum
{
    /**< Server is running */
    VS_RUNNING = 0,

    /**< Server is shutting down */
    VS_HALTING = 1,

    /**< Server is not running */
    VS_HALTED  = 2,

    /**< Server failed to start (bind/listen error); distinct from VS_HALTED so
         a caller polling for startup can tell "failed" from "not yet started" */
    VS_FAILED  = 3,

} vws_tcp_svr_state_t;

/**
 * @brief Callback invoked once when a peer has been continuously unreachable
 *   past the unrecoverable threshold (peer_unrecoverable_ms).
 *
 * @param server The server owning the peer.
 * @param cid The connection id of the unreachable peer.
 */
typedef void (*vws_peer_unrecoverable)(struct vws_tcp_svr* server, vws_cid_t cid);

/**
 * @brief Struct representing a basic server. It does not do anything but
 * process raw data. It does not have any knowledge of WebSockets.
 */
typedef struct vws_tcp_svr
{
    /**< Current state of the server */
    VWS_ATOMIC(uint8_t) state;

    /**< Asynchronous handle for event-based programming */
    uv_async_t* wakeup;

    /**< Event loop handle */
    uv_loop_t* loop;

    /**< Request queue */
    vws_svr_queue requests;

    /**< Response queue */
    vws_svr_queue responses;

    /**< Maximum connections allowed */
    int backlog;

    /**< Number of threads in the worker pool */
    int pool_size;

    /**< Per-connection OUTBOUND write-queue byte cap.
     * When libuv's queued-but-unsent write bytes for a connection would exceed
     * this, the connection is SHED (force-closed) instead of growing the write
     * heap unbounded (the slow-consumer -> OOM killer). 0 => the built-in
     * default (2 x VWS_MAX_MESSAGE_SIZE). The effective cap is floored at
     * 1 x VWS_MAX_MESSAGE_SIZE so a single legitimate max-size message can
     * never be false-shed. Configurable by the app (broker sets from config). */
    size_t write_queue_cap;

    /**< Count of connections whose reads are
     * currently paused because the request queue was full. Workers signal the
     * reactor (uv_async_send) to run a resume pass only while this is > 0. */
    VWS_ATOMIC(int) reads_paused;

    /**< Broker-settable defaults
     * (from vrtql.conf grid.broker.*). In-code defaults below stand when the
     * broker does not override, so behavior is unchanged by default. */

    /**< SO_KEEPALIVE idle interval (seconds) applied to accepted/adopted fds.
     * Default 30. */
    int keepalive_idle_sec;

    /**< TCP_USER_TIMEOUT (milliseconds) applied to accepted/adopted fds.
     * Default 20000. */
    int keepalive_user_timeout_ms;

    /**< Server-side aggregate request-reassembly cap (bytes): a single
     * in-progress reassembled message exceeding this gets a 1009 Too-Big close.
     * Default VWS_MAX_MESSAGE_SIZE. */
    size_t request_reassembly_cap;

    /**< Server-level default per-connection max message size (bytes). New
     * connections inherit this into cnx->max_message_size on accept. Default
     * VWS_MAX_MESSAGE_SIZE. */
    size_t max_message_size;

    /**< Heartbeat: idle time (ms) before a proactive PING is sent on a peer
     * connection. Default 10000; 0 disables proactive pings. */
    int ping_interval_ms;

    /**< Heartbeat: max time (ms) an outstanding PING waits for a PONG before
     * the peer is declared frozen and the connection is clean-closed. Default
     * 20000; 0 disables the liveness deadline. */
    int pong_deadline_ms;

    /**< Duration-track: how long (ms) a peer may be continuously unreachable
     * before peer_unrecoverable_cb fires. Default 60000; 0 disables the
     * unrecoverable report. */
    int peer_unrecoverable_ms;

    /**< Optional callback fired once per unreachable span when a peer has been
     * down longer than peer_unrecoverable_ms. NULL disables the report. */
    vws_peer_unrecoverable peer_unrecoverable_cb;

    /**< Thread handles */
    uv_thread_t* threads;

    /**< Pool of active connections */
    address_pool* cpool;

    /**< Callback function for connect */
    vws_tcp_svr_connect on_connect;

    /**< Callback function for inetd connect */
    vws_tcp_svr_connect on_inetd_connect;

    /**< Callback function for disconnect */
    vws_tcp_svr_disconnect on_disconnect;

    /**< Callback function for reading incoming data */
    vws_tcp_svr_read on_read;

    /**< Function for processing data from the client */
    vws_tcp_svr_process_data on_data_in;

    /**< Function for processing data from the worker back to the client */
    vws_tcp_svr_process_data on_data_out;

    /**< Worker thread constructor */
    vws_thread_ctx_ctor worker_ctor;

    /**< Worker thread constructor data */
    void* worker_ctor_data;

    /**< Worker thread destructor */
    vws_thread_ctx_dtor worker_dtor;

    /**< User-defined called back for processing each UV loop iteration */
    vws_svr_loop_cb loop_cb;

    /**< User-defined called back called after UV loop shutdown */
    vws_svr_loop_cb shutdown_cb;

    /**< User-defined callback for processing new connection */
    vws_svr_cnx_open_cb cnx_open_cb;

    /**< User-defined callback for processing closed connection */
    vws_svr_cnx_close_cb cnx_close_cb;

    /**< User-defined callback for processing data sent to a closed connection.
     * If this is defined, the callback takes ownership of data and must see
     * that it is dellocated with vws_svr_data_free().
     */
    vws_svr_data_lost_cb data_lost_cb;

    /**< Tracing level (0 is off) */
    uint8_t trace;

    /**< inetd mode (default 0). vws_tcp_svr_inetd_run() sets it to 1. */
    uint8_t inetd_mode;

    /**< A map peer connections */
    vws_kvs* peers;

    /**< The next wakeup time to check peer connections */
    uint32_t peer_timeout;

    /**< The peer timer */
    uv_timer_t* peer_timer;
} vws_tcp_svr;

/**
 * @brief Creates a new thread data. This TAKES OWNERSHIP of the buffer data and
 * sets the buffer to zero.
 *
 * @param s The server
 * @param cid The connection ID
 * @param b Pointer reference to The buffer
 * @return A new vws_svr_data instance with memory
 */
vws_svr_data* vws_svr_data_new(vws_tcp_svr* s, vws_cid_t cid, vws_buffer** b);

/**
 * @brief Creates a new thread data. This TAKES OWNERSHIP of the data. The
 * caller MUST NOT free() this data.
 *
 * @param s The server
 * @param cid The connection ID
 * @param size The number of bytes of data
 * @param data The data
 * @return A new vws_svr_data instance with memory
 */
vws_svr_data* vws_svr_data_own(vws_tcp_svr* s, vws_cid_t c, ucstr data, size_t size);

/**
 * @brief Frees the resources allocated to a thread data
 *
 * @param t The data
 */
void vws_svr_data_free(vws_svr_data* t);

/**
 * @brief Creates a new VRTQL server.
 *
 * @param pool_size The number of threads to run in the worker pool
 * @param backlog The connection backlog for listen(). If this is set to 0, it
 *   will use the default (128).
 * @param queue_size The maximum queue size for requests and responses. If this
 *   is set to 0, it will use the default (1024).
 * @return A new VRTQL server.
 */
vws_tcp_svr* vws_tcp_svr_new(int pool_size, int backlog, int queue_size);

/**
 * @brief Frees the resources allocated to a VRTQL server.
 *
 * @param s The server to free.
 */
void vws_tcp_svr_free(vws_tcp_svr* s);

/**
 * @brief Wakeup a VRTQL server network thread
 *
 * @param server The server to run.
 */
void vws_tcp_svr_wakeup(vws_tcp_svr* s);

/**
 * @brief Starts a VRTQL server.
 *
 * @param server The server to run.
 * @param host The host to bind the server.
 * @param port The port to bind the server.
 * @return 0 if successful, an error code otherwise.
 */
int vws_tcp_svr_run(vws_tcp_svr* server, cstr host, int port);

/**
 * @brief Checks if VRTQL server is running
 *
 * @param server The server to run.
 * @return true if running, false otherwise
 */
bool vws_tcp_svr_is_running(vws_tcp_svr* server);

/**
 * @brief Checks if VRTQL server is halted
 *
 * @param server The server to run.
 * @return true if halted, false otherwise
 */
bool vws_tcp_svr_is_halted(vws_tcp_svr* server);

/**
 * @brief Starts a VRTQL server with a single open socket. This is designed to
 * be used with tcpserver.
 *
 * @param server The server to run.
 * @param sockfd The incoming socket
 * @return 0 if successful, an error code otherwise.
 */
int vws_tcp_svr_inetd_run(vws_tcp_svr* server, vws_sockfd_t sockfd);

/**
 * @brief Stops a VRTQL server running in inetd_mode.
 *
 * @param server The server to stop.
 */
void vws_tcp_svr_inetd_stop(vws_tcp_svr* server);

/**
 * @brief Check that peers are all online
 *
 * @param server The server to send the data.
 * @return true if all peers are connected
 */
bool vws_tcp_svr_peers_online(vws_tcp_svr* server);

/**
 * @brief Sends data from a VRTQL server.
 *
 * @param server The server to send the data.
 * @param data The data to be sent.
 * @return 0 if successful, an error code otherwise.
 */
int vws_tcp_svr_send(vws_svr_data* data);

/**
 * @brief Close a VRTQL server connection.
 *
 * @param s The server
 * @param cnx The ID of connection to close
 */
void vws_tcp_svr_close(vws_tcp_svr* s, vws_cid_t cid);

/**
 * @brief Close a VRTQL server connection from within UV loop. This should ONLY
 * ever be called by functions operating in uv_thread(), specifically by the
 * server->on_loop callback. If called from worker threads this will CORRUPT the
 * UV loop and probably lead to SEGFAULTs and crash the program.
 *
 * @param s The server
 * @param cnx The ID of connection to close
 */
void vws_tcp_svr_uv_close(vws_tcp_svr* server, uv_handle_t* handle);

/**
 * @brief Stops a VRTQL server.
 *
 * @param server The server to stop.
 */
void vws_tcp_svr_stop(vws_tcp_svr* server);

/**
 * @brief Returns the server operational state.
 *
 * @param s The server to check.
 * @return The state of the server in the form of the vws_tcp_svr_state_t enum.
 */
uint8_t vws_tcp_svr_state(vws_tcp_svr* s);

/**
 * @brief Add a peer
 *
 * @param s The server
 * @param h The host name or IP address
 * @param p The host port
 * @param d User-defined data
 *
 * @return Returns pointer to peer on success was added, NULL otherwise.
 */
vws_peer* vws_tcp_svr_peer_add( vws_tcp_svr* s,
                                cstr h,
                                int p,
                                vws_peer_connect fn,
                                void* d );

/**
 * @brief Remove a peer
 *
 * @param s The server
 * @param h The host name or IP address
 * @param p The host port
 */
void vws_tcp_svr_peer_remove(vws_tcp_svr* s, cstr h, int p);

/**
 * @brief Connect a peer
 *
 * @param s The server
 * @param p The peer to connect
 * @param x The user-defined context
 *
 * @return Returns true if peer was connected, false otherwise.
 */
bool vws_tcp_svr_peer_connect(vws_tcp_svr* s, vws_peer* peer, void* x);

/**
 * @brief Disconnect a peer
 *
 * @param s The server
 * @param p The peer to connect
 *
 * @return Returns true if peer was connected, false otherwise.
 */
void vws_tcp_svr_peer_disconnect(vws_tcp_svr* s, vws_peer* peer);

/**
 * @brief Set timeout to wakup loop for peer connection maintenance
 *
 * @param s The server
 *
 * @return Returns true if peer was connected, false otherwise.
 */
void vws_tcp_svr_peer_timeout(vws_tcp_svr* s);

//------------------------------------------------------------------------------
// WebSocket Server
//------------------------------------------------------------------------------

/**
 * @brief Callback for data processing a WebSocket frame
 * @param s The server instance
 * @param f The incoming frame to process
 */
typedef void (*vws_svr_process_frame)(vws_svr_cnx* s, vws_frame* f);

/**
 * @brief Callback for data processing a WebSocket message
 * @param s The server instance
 * @param c The client connection index
 * @param m The message
 * @param x The user-defined context
 */
typedef void
(*vws_svr_process_msg)(struct vws_svr* s, vws_cid_t c, vws_msg* m, void* x);

/**
 * @brief Callback to process HTTP data read on a connection. This is a request
 *   in process which is NOT a websocket upgrade. This is a standalone HTTP
 *   request coming in.
 * @param c The connection structure
 * @return Returns 1, 0 or -1 as follows:
 *
 *   Return 0 if message is not complete and more data is needed.
 *
 *   Return 1 if message is complete and therefore sent for processing. In this
 *   case, the handler TAKES OWNERSHIP of the request object (c->http) object
 *   and the caller allocates a new one.
 *
 *   Return -1 if there is an error and connection needs to be close. The
 *   request will be deallocated by the server.
 */
typedef int (*vws_svr_http_read)(vws_svr_cnx* c);

/**
 * @brief Callback to process a complete HTTP request on connection
 * @param s The server
 * @param c The connection ID
 * @param m The message
 * @param x The user-defined context
 * @return Returns true of request is processed, false if error. If false is
 *   returned, connection will be closed.
 */
typedef bool (*vws_svr_process_http_req)( struct vws_svr* s,
                                          vws_cid_t c,
                                          vws_http_msg* msg,
                                          void* x );

/**
 * @brief Callback to process an internally queued request
 * @param s The server
 * @param c The connection ID
 * @param d The user-defined data pointer
 * @param l The user-defined data size in bytes
 * @param x The user-defined context
 */
typedef void (*vws_svr_process_irq)( struct vws_svr* s,
                                     vws_cid_t c,
                                     void* d,
                                     size_t l,
                                     void* x );

/**
 * @brief Callback invoked after a connection upgrades from HTTP to WebSocket.
 *
 * Called from the network thread immediately after the 101 response has been
 * sent and cnx->upgraded is set to true.  The application can use this to
 * fork a dedicated process for the WebSocket session, adjust per-connection
 * state, or perform any other post-upgrade action.
 *
 * @param cnx The connection that was upgraded.
 */
typedef void (*vws_svr_upgrade_cb)(vws_svr_cnx* cnx);

/**
 * @brief Struct representing a WebSocket server. It speaks the WebSocket
 * protocol and processes both WebSocket frames and messages.
 */
typedef struct vws_svr
{
    /**< Base class */
    struct vws_tcp_svr base;

    /**< Function for processing an incoming frame */
    vws_svr_process_frame on_frame_in;

    /**< Function for sending a frame to the client */
    vws_svr_process_frame on_frame_out;

    /**< Callback function HTTP read (not websocket upgrade) */
    vws_svr_http_read on_http_read;

    /**< Callback function HTTP request (not websocket upgrade) */
    vws_svr_process_http_req process_http;

    /**< Optional callback invoked after HTTP-to-WebSocket upgrade */
    vws_svr_upgrade_cb on_upgrade;

    /**< Callback function Internal request from network thread */
    vws_svr_process_irq process_irq;

    /**< Function for processing an incoming message */
    vws_svr_process_msg on_msg_in;

    /**< Function for sending a message to the client */
    vws_svr_process_msg on_msg_out;

    /**< Derived: for processing incoming messages (called by on_msg_in()) */
    vws_svr_process_msg process_ws;

    /**< Derived: for sending messages to the client (calls on_msg_out()) */
    vws_svr_process_msg send;

} vws_svr;

/**
 * @brief Creates a new WebSocket server.
 *
 * @param pool_size The number of threads to run in the worker pool
 * @param backlog The connection backlog for listen(). If this is set to 0, it
 *   will use the default (128).
 * @param queue_size The maximum queue size for requests and responses. If this
 *   is set to 0, it will use the default (1024).
 * @return A new WebSocket server.
 */
vws_svr* vws_svr_new(int pool_size, int backlog, int queue_size);

/**
 * @brief Frees the resources allocated to a WebSocket server.
 *
 * @param s The server to free.
 */
void vws_svr_free(vws_svr* s);

/**
 * @brief Queues data for internal processing by worker pool
 * @param s The server
 * @param c The connection ID
 * @param d The user-defined data pointer
 * @param l The user-defined data size in bytes
 * @param x The user-defined context
 */
void vws_svr_send_irq(vws_svr* s, vws_cid_t cid, void* data, size_t size);

/**
 * @brief Starts a WebSocket server.
 *
 * @param server The server to run.
 * @param host The host to bind the server.
 * @param port The port to bind the server.
 * @return 0 if successful, an error code otherwise.
 */
int vws_svr_run(vws_svr* server, cstr host, int port);

//------------------------------------------------------------------------------
// Messaging Server
//------------------------------------------------------------------------------

/**
 * @brief Callback for data processing a message
 * @param s The server instance
 * @param c The client connection index
 * @param m The incoming message to process
 * @param x The user-defined context
 */
typedef void
(*vrtql_svr_process_msg)(vws_svr* s, vws_cid_t c, vrtql_msg* m, void* x);

/**
 * @brief Struct representing a VTQL message server. It is derived from the
 * WebSocket server and operates in terms of VRTQL messages (vrtql_msg) as
 * defined in message.h.
 */
typedef struct vrtql_msg_svr
{
    /**< Base class */
    struct vws_svr base;

    /**< Function for processing an incoming message */
    vrtql_svr_process_msg on_msg_in;

    /**< Function for sending a message to the client */
    vrtql_svr_process_msg on_msg_out;

    /**< Derived: for processing incoming messages (called by on_msg_in()) */
    vrtql_svr_process_msg process;

    /**< Derived: for sending messages to the client (calls on_msg_out()) */
    vrtql_svr_process_msg send;

    /**< Derived: does all that send() does but does NOT clean up message. */
    vrtql_svr_process_msg dispatch;

    /**< User-defined data */
    void* data;

} vrtql_msg_svr;

/**
 * @brief Creates a new VRTQL message server.
 *
 * @param pool_size The number of threads to run in the worker pool
 * @param backlog The connection backlog for listen(). If this is set to 0, it
 *   will use the default (128).
 * @param queue_size The maximum queue size for requests and responses. If this
 *   is set to 0, it will use the default (1024).
 * @return A new VRTQL message server.
 */
vrtql_msg_svr* vrtql_msg_svr_new(int pool_size, int backlog, int queue_size);

/**
 * @brief Frees the resources allocated to a VRTQL message server.
 *
 * @param s The server to free.
 */
void vrtql_msg_svr_free(vrtql_msg_svr* s);

/**
 * @brief Message server instance constructor
 *
 * Constructs a new message server instance. This takes a new, empty
 * vrtql_msg_svr instance and initializes all of its members. It is used by
 * derived structs as well (vrtql_msg_svr) to construct the base struct.
 *
 * @param server The server instance to be initialized
 * @return The initialized server instance
 *
 * @ingroup ServerFunctions
 */

vrtql_msg_svr* vrtql_msg_svr_ctor( vrtql_msg_svr* server,
                                   int num_threads,
                                   int backlog,
                                   int queue_size );

/**
 * @brief Message server instance destructor
 *
 * Destructs an initialized message server instance. This takes a vrtql_msg_svr
 * instance and deallocates all of its members -- everything but the top-level
 * struct. This is used by derived structs as well to destruct the base struct.
 *
 * @param server The message server instance to be destructed
 *
 * @ingroup ServerFunctions
 */

void vrtql_msg_svr_dtor(vrtql_msg_svr* s);

/**
 * @brief Starts a VRTQL message server.
 *
 * @param server The server to run.
 * @param host The host to bind the server.
 * @param port The port to bind the server.
 * @return 0 if successful, an error code otherwise.
 */
int vrtql_msg_svr_run(vrtql_msg_svr* server, cstr host, int port);

#ifdef __cplusplus
}
#endif

#endif /* VRTQL_SVR_DECLARE */
