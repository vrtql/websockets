#ifndef VRTQL_SVR_DECLARE
#define VRTQL_SVR_DECLARE

#include <uv.h>

#include "vrtql.h"
#include "util/sc_map.h"

/**
 * @brief Struct representing a server data for inter-thread communication
 * between the main network thread and worker threads. This is the way data is
 * passed between them. When passed from network thread to work, these take form
 * of incoming data from the client. When passed from the worker thread the the
 * networking thread, outgoing data to the client.
 */

typedef struct
{
    /**< The client associated with the data */
    uv_stream_t* client;

    /**< The number of bytes of data */
    size_t size;

    /**< The data */
    char* data;

} vrtql_svr_data;

/**
 * @brief Struct representing a server queue, including information about
 * buffer, size, capacity and threading.
 */
typedef struct
{
    /**< The buffer holding datas in the queue */
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

    /**< Debugging trace flag */
    uint8_t trace;

} vrtql_svr_queue;

struct vrtql_svr;

/**
 * @brief Represents a client connection.
 */
typedef struct
{
    /**< The server associated with the connection */
    struct vrtql_svr* server;

    /**< The client associated with the connection */
    uv_stream_t* client;

    /**< User-defined data associated with the connection */
    char* data;

} vrtql_svr_cnx;

/**
 * @brief Callback for new connection connect
 * @param c The connection structure
 */
typedef void (*vrtql_svr_connect)(vrtql_svr_cnx* c);

/**
 * @brief Callback for connection disconnect
 * @param c The connection structure
 */
typedef void (*vrtql_svr_disconnect)(vrtql_svr_cnx* c);

/**
 * @brief Callback for connection read
 * @param c The connection structure
 * @param n The number of bytes in buffer
 * @param b The buffer
 */
typedef void (*vrtql_svr_read)(vrtql_svr_cnx* c, ssize_t n, const uv_buf_t* b);

/**
 * @brief Callback for data processing within worker thread
 * @param s The server instance
 * @param t The incoming request to process
 */
typedef void (*vrtql_svr_process)(struct vrtql_svr* s, vrtql_svr_data* t);

/**
 * @brief Enumerates server state
 */
typedef enum vrtql_svr_state
{
    VS_RUNNING = 0,  /**< Server is running       */
    VS_HALTING = 1,  /**< Server is shutting down */
    VS_HALTED  = 2,  /**< Server is no running    */
} vrtql_svr_state;

/** Abbreviation for connection map */
typedef struct sc_map_64v vrtql_svr_cnx_map;

/**
 * @brief Struct representing a VRTQL server.
 */
typedef struct vrtql_svr
{
    /**< Current state of the server */
    uint8_t state;

    /**< Mutex for thread safety */
    uv_mutex_t mutex;

    /**< Asynchronous handle for event-based programming */
    uv_async_t wakeup;

    /**< Event loop handle */
    uv_loop_t* loop;

    /**< Request queue */
    vrtql_svr_queue requests;

    /**< Response queue */
    vrtql_svr_queue responses;

    /**< Maximum connections allowed */
    int backlog;

    /**< Number of threads in worker pool*/
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

    /**< Function for request processing */
    vrtql_svr_process process;

    /**< Debugging trace flag */
    uint8_t trace;

} vrtql_svr;

/**
 * @brief Creates a new thread data. This TAKES OWNERSHIP of the data. The
 * called is not to free this data.
 *
 * @param c The connection structure
 * @param size The number of bytes of data
 * @param data The data
 * @return A new thread data.
 */
vrtql_svr_data* vrtql_svr_data_new(uv_stream_t* c, size_t size, ucstr data);

/**
 * @brief Frees the resources allocated to a thread data
 *
 * @param t The data
 */
void vrtql_svr_data_free(vrtql_svr_data* t);

/**
 * @brief Creates a new VRTQL server.
 *
 * @param threads The number of threads for the server.
 * @return A new VRTQL server.
 */
vrtql_svr* vrtql_svr_new(int threads);

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
 * @brief Stops a VRTQL server.
 *
 * @param server The server to stop.
 */
void vrtql_svr_stop(vrtql_svr* server);

/**
 * @brief Toggles debugging trace for a VRTQL server.
 *
 * @param server The server to trace.
 * @param flag The flag to set the trace. Non-zero value enables tracing,
 *   zero disables.
 */
void vrtql_svr_trace(vrtql_svr* server, int flag);

#endif /* VRTQL_SVR_DECLARE */
