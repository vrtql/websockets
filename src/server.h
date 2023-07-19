#ifndef VRTQL_SVR_DECLARE
#define VRTQL_SVR_DECLARE

#include <uv.h>

#include "vrtql.h"
#include "message.h"
#include "http_request.h"

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
 * The data items are stored in a generic structure called vrtql_svr_data, which
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

struct vrtql_svr_cnx;

typedef enum
{
    /* uv_thread() is to close connection */
    VM_SVR_DATA_CLOSE  = (1 << 1)

} vrtql_svr_data_state_t;

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
    /**< The client connection associated with the data */
    struct vrtql_svr_cnx* cnx;

    /**< The number of bytes of data */
    size_t size;

    /**< The data */
    char* data;

    /**< Message state flags */
    uint64_t flags;

} vrtql_svr_data;

/**
 * @brief Struct representing a server queue, including information about
 * buffer, size, capacity, and threading.
 */
typedef struct
{
    /**< The buffer holding data in the queue */
    vrtql_svr_data** buffer;

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
    uint8_t state;

    /**< Queue name */
    cstr name;

} vrtql_svr_queue;

struct vrtql_svr;

/**
 * @brief Represents a client connection.
 */
typedef struct vrtql_svr_cnx
{
    /**< The server associated with the connection */
    struct vrtql_svr* server;

    /**< The client associated with the connection */
    uv_stream_t* handle;

    /**< User-defined data associated with the connection */
    char* data;

    /**< The format to serialize. If VM_MPACK_FORMAT, serialize into MessagePack
     *   binary format. If VM_JSON_FORMAT, then serialize into JSON format.
     */
    vrtql_msg_format_t format;

} vrtql_svr_cnx;

/**
 * @brief Callback for a new connection connection
 * @param c The connection structure
 */
typedef void (*vrtql_svr_connect)(vrtql_svr_cnx* c);

/**
 * @brief Callback for connection disconnection
 * @param c The connection structure
 */
typedef void (*vrtql_svr_disconnect)(vrtql_svr_cnx* c);

/**
 * @brief Callback for connection read
 * @param c The connection structure
 * @param n The number of bytes in the buffer
 * @param b The buffer
 */
typedef void (*vrtql_svr_read)(vrtql_svr_cnx* c, ssize_t n, const uv_buf_t* b);

/**
 * @brief Callback for data processing within a worker thread
 * @param s The server instance
 * @param t The incoming request to process
 */
typedef void (*vrtql_svr_process_data)(vrtql_svr_data* t);

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

} vrtql_svr_state_t;

/** Abbreviation for the connection map */
typedef struct sc_map_64v vrtql_svr_cnx_map;

/**
 * @brief Struct representing a basic server. It does not do anything but
 * process raw data. It does not have any knowledge of WebSockets.
 */
typedef struct vrtql_svr
{
    /**< Current state of the server */
    uint8_t state;

    /**< Asynchronous handle for event-based programming */
    uv_async_t* wakeup;

    /**< Event loop handle */
    uv_loop_t* loop;

    /**< Request queue */
    vrtql_svr_queue requests;

    /**< Response queue */
    vrtql_svr_queue responses;

    /**< Maximum connections allowed */
    int backlog;

    /**< Number of threads in the worker pool */
    int pool_size;

    /**< Thread handles */
    uv_thread_t* threads;

    /**< Map of active connections */
    vrtql_svr_cnx_map cnxs;

    /**< Callback function for connect */
    vrtql_svr_connect on_connect;

    /**< Callback function for disconnect */
    vrtql_svr_disconnect on_disconnect;

    /**< Callback function for reading incoming data */
    vrtql_svr_read on_read;

    /**< Function for processing data from the client */
    vrtql_svr_process_data on_data_in;

    /**< Function for processing data from the worker back to the client */
    vrtql_svr_process_data on_data_out;

    /**< Tracing leve (0 is off) */
    uint8_t trace;

} vrtql_svr;

/**
 * @brief Creates a new thread data. This TAKES OWNERSHIP of the buffer data and
 * sets the buffer to zero.
 *
 * @param c The connection
 * @param buffer The buffer
 * @return A new vrtql_svr_data instance with memory
 */
vrtql_svr_data* vrtql_svr_data_new(vrtql_svr_cnx* c, vrtql_buffer* memory);

/**
 * @brief Creates a new thread data. This TAKES OWNERSHIP of the data. The
 * caller MUST NOT free() this data.
 *
 * @param c The connection
 * @param size The number of bytes of data
 * @param data The data
 * @return A new vrtql_svr_data instance with memory
 */
vrtql_svr_data* vrtql_svr_data_own(vrtql_svr_cnx* c, ucstr data, size_t size);

/**
 * @brief Frees the resources allocated to a thread data
 *
 * @param t The data
 */
void vrtql_svr_data_free(vrtql_svr_data* t);

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
vrtql_svr* vrtql_svr_new(int pool_size, int backlog, int queue_size);

/**
 * @brief Frees the resources allocated to a VRTQL server.
 *
 * @param s The server to free.
 */
void vrtql_svr_free(vrtql_svr* s);

/**
 * @brief Starts a VRTQL server.
 *
 * @param server The server to run.
 * @param host The host to bind the server.
 * @param port The port to bind the server.
 * @return 0 if successful, an error code otherwise.
 */
int vrtql_svr_run(vrtql_svr* server, cstr host, int port);

/**
 * @brief Sends data from a VRTQL server.
 *
 * @param server The server to send the data.
 * @param data The data to be sent.
 * @return 0 if successful, an error code otherwise.
 */
int vrtql_svr_send(vrtql_svr* server, vrtql_svr_data* data);

/**
 * @brief Close a VRTQL server connection.
 *
 * @param cnx The server connection to close.
 */
void vrtql_svr_close(vrtql_svr_cnx* cnx);

/**
 * @brief Stops a VRTQL server.
 *
 * @param server The server to stop.
 */
void vrtql_svr_stop(vrtql_svr* server);

/**
 * @brief Returns the server operational state.
 *
 * @param server The server to check.
 * @return The state of the server in the form of the vrtql_svr_state_t enum.
 */
uint8_t vrtql_svr_state(vrtql_svr* server);

//------------------------------------------------------------------------------
// WebSocket Server
//------------------------------------------------------------------------------

/**
 * @brief Callback for data processing a WebSocket frame
 * @param s The server instance
 * @param f The incoming frame to process
 */
typedef void (*vws_svr_process_frame)(vrtql_svr_cnx* s, vws_frame* f);

/**
 * @brief Callback for data processing a WebSocket message
 * @param s The server instance
 * @param f The incoming message to process
 */
typedef void (*vws_svr_process_msg)(vrtql_svr_cnx* s, vws_msg* f);

/**
 * @brief Struct representing a WebSocket server. It speaks the WebSocket
 * protocol and processes both WebSocket frames and messages.
 */
typedef struct vrtql_ws_svr
{
    /**< Base class */
    struct vrtql_svr base;

    /** Flag that holds whether we have upgraded connection from HTTP to
     *  WebSockets */
    bool upgraded;

    /* Flag holds the HTTP request that started connection. */
    vrtql_http_req* http;

    /**< Function for processing data from the client */
    vrtql_svr_process_data on_data_in;

    /**< Function for processing an incoming frame */
    vws_svr_process_frame on_frame_in;

    /**< Function for sending a frame to the client */
    vws_svr_process_frame on_frame_out;

    /**< Function for processing an incoming message */
    vws_svr_process_msg on_msg_in;

    /**< Function for sending a message to the client */
    vws_svr_process_msg on_msg_out;

    /**< Derived: for processing incoming messages (called by on_msg_in()) */
    vws_svr_process_msg process;

    /**< Derived: for sending messages to the client (calls on_msg_out()) */
    vws_svr_process_msg send;

} vrtql_ws_svr;

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
vrtql_ws_svr* vrtql_ws_svr_new(int pool_size, int backlog, int queue_size);

/**
 * @brief Frees the resources allocated to a WebSocket server.
 *
 * @param s The server to free.
 */
void vrtql_ws_svr_free(vrtql_ws_svr* s);

/**
 * @brief Starts a WebSocket server.
 *
 * @param server The server to run.
 * @param host The host to bind the server.
 * @param port The port to bind the server.
 * @return 0 if successful, an error code otherwise.
 */
int vrtql_ws_svr_run(vrtql_ws_svr* server, cstr host, int port);

//------------------------------------------------------------------------------
// Messaging Server
//------------------------------------------------------------------------------

/**
 * @brief Callback for data processing a message
 * @param s The server instance
 * @param m The incoming message to process
 */
typedef void (*vrtql_svr_process_msg)(vrtql_svr_cnx* s, vrtql_msg* m);

/**
 * @brief Struct representing a VTQL message server. It is derived from the
 * WebSocket server and operates in terms of VRTQL messages (vrtql_msg) as
 * defined in message.h.
 */
typedef struct vrtql_msg_svr
{
    /**< Base class */
    struct vrtql_ws_svr base;

    /**< Function for processing an incoming message */
    vrtql_svr_process_msg on_msg_in;

    /**< Function for sending a message to the client */
    vrtql_svr_process_msg on_msg_out;

    /**< Derived: for processing incoming messages (called by on_msg_in()) */
    vrtql_svr_process_msg process;

    /**< Derived: for sending messages to the client (calls on_msg_out()) */
    vrtql_svr_process_msg send;

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
 * @brief Starts a VRTQL message server.
 *
 * @param server The server to run.
 * @param host The host to bind the server.
 * @param port The port to bind the server.
 * @return 0 if successful, an error code otherwise.
 */
int vrtql_msg_svr_run(vrtql_msg_svr* server, cstr host, int port);

#endif /* VRTQL_SVR_DECLARE */
