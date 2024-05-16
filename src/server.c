#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "websocket.h"

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

/**
 * @defgroup AddressPool
 */

address_pool* address_pool_new(int initial_size, int growth_factor)
{
    address_pool* pool    = (address_pool*)malloc(sizeof(address_pool));
    pool->slots           = (uintptr_t*)calloc(initial_size, sizeof(uintptr_t));
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
        free(*pool);
        (*pool) = NULL;
    }
}

void address_pool_resize(address_pool *pool)
{
    int new_capacity     = pool->capacity * pool->growth_factor;
    uintptr_t* new_slots = (uintptr_t *)calloc(new_capacity, sizeof(uintptr_t));
    memcpy(new_slots, pool->slots, pool->capacity * sizeof(uintptr_t));

    free(pool->slots);

    pool->slots    = new_slots;
    pool->capacity = new_capacity;
}

uint32_t address_pool_set(address_pool* pool, uintptr_t address)
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

    return allocated_index;
}

uintptr_t address_pool_get(address_pool* pool, vws_cid_t index)
{
    if (index >= pool->capacity || pool->slots[index] == 0)
    {
        return 0;
    }

    return pool->slots[index];
}

void address_pool_remove(address_pool* pool, uint32_t index)
{
    if (pool->slots[index] != 0)
    {
        pool->slots[index] = 0;
        pool->count--;
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
 * vws_svr_data instances. When a worker sends a vws_svr_data instance, it
 * adds it to the queue (server->responses) and notifies (wakes up) the main UV
 * loop (uv_run() in vrtql_tcp_svr_run()) by calling uv_async_send(server->wakeup)
 * which in turn calls this function to check the server->responses queue. This
 * function unloads all data in the queue and sends the data out to each
 * respective client. It then returns control back to the main UV loop which
 * resumes polling the network connections (blocking if there is no activity).
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
 * @defgroup ConnectionMap
 *
 * @brief Functions that provide access to the connection map
 *
 * The connection map tracks all active connections. These functions simplify
 * the map API and include memory management and other server-specific
 * functionality where appropriate.
 *
 * @ingroup ConnectionMap
 */

/**
 * @brief Get the connection corresponding to a client from the connection map.
 *
 * @param map The connection map.
 * @param key The client (used as key in the map).
 * @return The corresponding server connection.
 *
 * @ingroup ConnectionMap
 */
static vws_svr_cnx* svr_cnx_map_get(vws_svr_cnx_map* map, uv_stream_t* key);

/**
 * @brief Set a connection for a client in the connection map.
 *
 * @param map The connection map.
 * @param key The client (used as key in the map).
 * @param value The server connection.
 * @return Success or failure status.
 *
 * @ingroup ConnectionMap
 */
static int8_t svr_cnx_map_set( vws_svr_cnx_map* map,
                               uv_stream_t* key,
                               vws_svr_cnx* value );

/**
 * @brief Remove a client's connection from the connection map.
 *
 * @param map The connection map.
 * @param key The client (used as key in the map).
 *
 * @ingroup ConnectionMap
 */
static void svr_cnx_map_remove(vws_svr_cnx_map* map, uv_stream_t* key);

/**
 * @brief Clear all connections from the connection map.
 *
 * @param map The connection map.
 *
 * @ingroup ConnectionMap
 */
static void svr_cnx_map_clear(vws_svr_cnx_map* map);

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

                return;
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
}

void uv_thread(uv_async_t* handle)
{
    vws_cinfo* cinfo    = (vws_cinfo*)handle->data;
    vws_tcp_svr* server = cinfo->server;

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

        if (vws_is_flag(&data->flags, VM_SVR_DATA_CLOSE))
        {
            // Close connection
            svr_cnx_close(server, data->cid);
            vws_svr_data_free(data);
        }
        else
        {
            server->on_data_out(data, NULL);
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

    return item;
}

void vws_svr_data_free(vws_svr_data* t)
{
    if (t != NULL)
    {
        vws.free(t->data);
        vws.free(t);
    }
}

vws_tcp_svr* vws_tcp_svr_new(int num_threads, int backlog, int queue_size)
{
    vws_tcp_svr* server = vws.malloc(sizeof(vws_tcp_svr));
    return tcp_svr_ctor(server, num_threads, backlog, queue_size);
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

int vws_tcp_svr_send(vws_svr_data* data)
{
    queue_push(&data->server->responses, data);

    // Notify event loop about the new response
    uv_async_send(data->server->wakeup);

    return 0;
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

    vws_cinfo* cinfo  = vws.malloc(sizeof(vws_cinfo));
    cinfo->cnx        = NULL;
    cinfo->server     = server;
    cinfo->cid        = 0;

    socket->data      = cinfo;

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
        return -1;
    }

    //> Listen

    rc = uv_listen((uv_stream_t*)socket, server->backlog, svr_on_connect);

    if (rc)
    {
        vws.error(VE_RT, "Listen error %s", uv_strerror(rc));
        return -1;
    }

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace( VL_INFO,
                   "vws_tcp_svr_run(%s): Listen %s:%lu",
                   server, host, port );
    }

    //> Start server

    // Set state to running
    server->state = VS_RUNNING;

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_run(%s): Starting uv_run()", server);
    }

    while (server->state == VS_RUNNING)
    {
        // Run UV loop. This runs indefinitely, passing network I/O and and out
        // of system until server is shutdown by vws_tcp_svr_stop() (by external
        // thread).
        uv_run(server->loop, UV_RUN_DEFAULT);
    }

    //> Shutdown server

    // Close the listening socket handle
    uv_close((uv_handle_t*)socket, svr_on_close);

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_run(%p): Shutdown complete", server);
    }

    // Set state to halted
    server->state = VS_HALTED;

    return 0;
}

void vws_tcp_svr_stop(vws_tcp_svr* server)
{
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

    // Wakeup the main event loop to shutdown main thread
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "vws_tcp_svr_stop(): stop main thread");
    }

    uv_async_send(server->wakeup);

    while (server->state != VS_HALTED)
    {
        sleep(1);
    }
}

int vws_tcp_svr_inetd_run(vws_tcp_svr* server, int sockfd)
{
    if (sockfd < 0)
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

    // Initialize and adopt the existing socket descriptor.
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
        // Handle uv_tcp_open failure.
        vws.error(VE_RT, "Failed to adopt the socket descriptor.");
        vws.free(c);
        return 1;
    }

    vws_cinfo* addr  = vws.malloc(sizeof(vws_cinfo));
    addr->cnx        = NULL;
    addr->server     = server;

    c->data = addr;

    if (uv_read_start((uv_stream_t*)c, svr_on_realloc, svr_on_read) != 0)
    {
        vws.error(VE_RT, "Failed to start reading from client");
        vws.free(c);
        vws.free(addr);
        return 1;
    }

    //> Add connection to registry and initialize

    // Associate server with this handle for callbacks.
    vws_svr_cnx* cnx = svr_cnx_new(server, (uv_stream_t*)c);
    addr->cnx        = cnx;
    addr->cid        = cnx->cid;

    //> Call svr_on_connect() handler

    server->on_connect(cnx);

    // Now, the handle is associated with the socket and is ready to be used.
    // Start the libuv loop.
    uv_run(server->loop, UV_RUN_DEFAULT);

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
    svr->on_disconnect    = svr_client_disconnect;
    svr->on_read          = svr_client_read;
    svr->on_data_in       = svr_client_data_in;
    svr->on_data_out      = svr_client_data_out;
    svr->worker_ctor      = NULL;
    svr->worker_ctor_data = NULL;
    svr->worker_dtor      = NULL;
    svr->backlog          = backlog;
    svr->loop             = (uv_loop_t*)vws.malloc(sizeof(uv_loop_t));
    svr->state            = VS_HALTED;
    svr->trace            = vws.tracelevel;
    svr->inetd_mode       = 0;

    uv_loop_init(svr->loop);
    svr->cpool = address_pool_new(1000, 2);
    queue_init(&svr->requests, queue_size, "requests");
    queue_init(&svr->responses, queue_size, "responses");

    vws_cinfo* cinfo  = vws.malloc(sizeof(vws_cinfo));
    cinfo->cnx        = NULL;
    cinfo->server     = svr;
    cinfo->cid        = 0;

    svr->wakeup = vws.malloc(sizeof(uv_async_t));
    svr->wakeup->data = cinfo;
    uv_async_init(svr->loop, svr->wakeup, uv_thread);

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
                       "on_uv_close(): libuv resource not properly freed: %p",
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

void tcp_svr_dtor(vws_tcp_svr* svr)
{
    //> Stop/shutdown server

    if (svr->state == VS_RUNNING)
    {
        vws_tcp_svr_stop(svr);
    }

    svr_shutdown(svr);
    vws.free(svr->threads);

    // Close the server async handle
    uv_close((uv_handle_t*)svr->wakeup, svr_on_close);

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

    // Free address pool
    address_pool_free(&svr->cpool);
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

void svr_client_disconnect(vws_svr_cnx* c)
{
    if (vws.tracelevel >= VT_SERVICE)
    {
        vws.trace(VL_INFO, "svr_client_disconnect(%p)", c->handle);
    }
}

void svr_client_read(vws_svr_cnx* c, ssize_t size, const uv_buf_t* buf)
{
    vws_tcp_svr* server = c->server;

    // Queue data to worker pool for processing
    vws_svr_data* data = vws_svr_data_own(server, c->cid, (ucstr)buf->base, size);

    // Store reference to server
    data->server = c->server;

    // Put on queue
    queue_push(&server->requests, data);
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
        return;
    }

    uv_buf_t buf    = uv_buf_init(data->data, data->size);
    uv_write_t* req = (uv_write_t*)vws.malloc(sizeof(uv_write_t));
    req->data       = data;

    // Check address pool and ensure connection is still active
    uintptr_t ptr = address_pool_get(data->server->cpool, data->cid);

    if (ptr == 0)
    {
        // Connection no longer exists.
        vws_svr_data_free(data);
        vws.free(req);
        return;
    }

    // Write out to libuv
    uv_stream_t* handle = ((vws_svr_cnx*)ptr)->handle;
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

    // Initialize HTTP state
    cnx->upgraded    = false;
    cnx->http        = vws_http_msg_new(HTTP_REQUEST);

    // Add to address pool
    cnx->cid = address_pool_set(s->cpool, (uintptr_t)cnx);

    return cnx;
}

void svr_cnx_free(vws_svr_cnx* c)
{
    if (c != NULL)
    {
        if (c->http != NULL)
        {
            vws_http_msg_free(c->http);
        }

        // Remove from pool
        address_pool_remove(c->server->cpool, c->cid);

        vws.free(c);
    }
}

void svr_cnx_close(vws_tcp_svr* server, vws_cid_t cid)
{
    vws_tcp_svr_close(server, cid);
}

//------------------------------------------------------------------------------
// Server Utilities
//------------------------------------------------------------------------------

vws_svr_cnx* svr_cnx_map_get(vws_svr_cnx_map* map, uv_stream_t* key)
{
    vws_svr_cnx* cnx = sc_map_get_64v(map, (uint64_t)key);

    // If entry exists
    if (sc_map_found(map) == true)
    {
        return cnx;
    }

    return NULL;
}

void svr_cnx_map_remove(vws_svr_cnx_map* map, uv_stream_t* key)
{
    vws_svr_cnx* cnx = sc_map_get_64v(map, (uint64_t)key);

    // If entry exists
    if (sc_map_found(map) == true)
    {
        sc_map_del_64v(map, (uint64_t)key);

        // Call on_disconnect() handler
        cnx->server->on_disconnect(cnx);

        // Cleanup
        vws.free(cnx);
    }
}

int8_t svr_cnx_map_set(vws_svr_cnx_map* map, uv_stream_t* key, vws_svr_cnx* value)
{
    // See if we have an existing entry
    sc_map_get_64v(map, (uint64_t)key);

    if (sc_map_found(map) == true)
    {
        // Exsiting entry. Return false.
        return 0;
    }

    sc_map_put_64v(map, (uint64_t)key, value);

    // True
    return 1;
}

void svr_cnx_map_clear(vws_svr_cnx_map* map)
{
    uint64_t key; vws_svr_cnx* cnx;
    sc_map_foreach(map, key, cnx)
    {
        cnx->server->on_disconnect(cnx);
        vws.free(cnx);
    }

    sc_map_clear_64v(map);
}

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

    // Cleanup queues

    vws_svr_queue* queue = &server->responses;
    uv_mutex_lock(&queue->mutex);
    while (queue->size > 0)
    {
        vws_svr_data* data = queue->buffer[queue->head];
        queue->head        = (queue->head + 1) % queue->capacity;
        queue->size--;

        vws_svr_data_free(data);
    }
    uv_mutex_unlock(&queue->mutex);

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
    //vws_tcp_svr* server = (vws_tcp_svr*) socket->data;

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
    c->data          = cinfo;

    if (uv_tcp_init(server->loop, c) != 0)
    {
        vws.error(VE_RT, "Failed to initialize client");
        return;
    }

    if (uv_accept(socket, (uv_stream_t*)c) == 0)
    {
        if (uv_read_start((uv_stream_t*)c, svr_on_realloc, svr_on_read) != 0)
        {
            vws.error(VE_RT, "Failed to start reading from client");
            return;
        }
    }
    else
    {
        uv_close((uv_handle_t*)c, svr_on_close);
    }

    //> Add connection to registry and initialize

    vws_svr_cnx* cnx = svr_cnx_new(server, (uv_stream_t*)c);
    cinfo->cnx       = cnx;
    cinfo->cid       = cnx->cid;

    //> Call svr_on_connect() handler

    server->on_connect(cnx);
}

void svr_on_read(uv_stream_t* c, ssize_t nread, const uv_buf_t* buf)
{
    vws_cinfo* cinfo    = (vws_cinfo*)c->data;
    vws_svr_cnx* cnx    = cinfo->cnx;;
    vws_tcp_svr* server = cinfo->server;
    vws_cid_t cid           = cinfo->cid;

    if (nread < 0)
    {
        uv_close((uv_handle_t*)c, svr_on_close);
        vws.free(buf->base);
        return;
    }

    // Lookup connection
    uintptr_t ptr = address_pool_get(server->cpool, cid);

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

void svr_on_close(uv_handle_t* handle)
{
    vws_cinfo* cinfo     = (vws_cinfo*)handle->data;
    vws_svr_cnx* cnx     = cinfo->cnx;
    vws_tcp_svr* server  = cinfo->server;
    vws_cid_t cid            = cinfo->cid;

    // Lookup connection
    uintptr_t ptr = address_pool_get(server->cpool, cid);

    if (ptr != 0)
    {
        vws_svr_cnx* cnx = (vws_svr_cnx*)ptr;

        // Call on_disconnect() handler
        server->on_disconnect(cnx);

        // Remove from map
        //sc_map_del_64v(map, (uint64_t)handle);

        // Cleanup
        svr_cnx_free(cnx);
    }

    vws.free(handle->data);
    vws.free(handle);

    // If we are running in inetd mode, there is only one socket and its closing
    // means we are done and should exit process.
    if ((server->inetd_mode == 1) && (server->state == VS_RUNNING))
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
        vws.free(data);
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
        return;
    }

    queue->buffer[queue->tail] = data;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;

    // Signal condition variable
    uv_cond_signal(&queue->cond);
    uv_mutex_unlock(&queue->mutex);
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
            // Add to queue
            sc_queue_add_first(&c->queue, f);

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
        vws.trace(VL_INFO, "ws_svr_client_connect(%p)", c->handle);
    }

    // Create a new vws_cnx
    vws_cnx* cnx = (void*)vws_cnx_new();
    cnx->process = ws_svr_process_frame;
    cnx->data    = (void*)c;   // Link cnx -> c
    c->data      = (void*)cnx; // Link c -> cnx
}

void ws_svr_client_disconnect(vws_svr_cnx* c)
{
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

void vws_tcp_svr_close(vws_tcp_svr* server, vws_cid_t cid)
{
    // Create reply
    vws_svr_data* reply;
    reply = vws_svr_data_own(server, cid, NULL, 0);

    // Set connection close flag
    vws_set_flag(&reply->flags, VM_SVR_DATA_CLOSE);

    // Queue the data to uv_thread() to send out on wire
    vws_tcp_svr_send(reply);
}

// Runs in uv_thread()
void ws_svr_client_read(vws_svr_cnx* cnx, ssize_t size, const uv_buf_t* buf)
{
    vws_tcp_svr* server = cnx->server;
    vws_cnx* c          = (vws_cnx*)cnx->data;

    // Add to client socket buffer
    vws_buffer_append(c->base.buffer, (ucstr)buf->base, size);

    // Free libuv memory
    vws.free(buf->base);

    // If we are in HTTP mode
    if (cnx->upgraded == false)
    {
        // Parse incoming data as HTTP request.

        ucstr data  = c->base.buffer->data;
        size_t size = c->base.buffer->size;
        ssize_t n   = vws_http_msg_parse(cnx->http, (cstr)data, size);

        // Did we get a complete request?
        if (cnx->http->headers_complete == true)
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
                svr_cnx_close(server, cnx->cid);

                return;
            }

            // Drain HTTP request data from cnx->data buffer
            vws_buffer_drain(c->base.buffer, n);

            //> Generate HTTP response and send

            vws_buffer* http = vws_buffer_new();

            struct sc_map_str* headers = &cnx->http->headers;
            cstr key   = vws_map_get(headers, "sec-websocket-key");
            cstr proto = vws_map_get(headers, "sec-websocket-protocol");

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
            server->on_data_out(reply, NULL);

            // Cleanup. Buffer data was passed to reply.
            vws_buffer_free(http);

            //> Change state to WebSocket mode

            // Set the flag that we are in WebSocket mode
            cnx->upgraded = true;

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
            // No complete HTTP request yet
            return;
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
            queue_push(&server->requests, block);
        }
    }
}

// Runs in worker_thread()
void ws_svr_client_data_in(vws_svr_data* block, void* x)
{
    //> Append data to connection buffer

    vws_cid_t cid       = block->cid;
    vws_svr* server = (vws_svr*)block->server;

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
    server->process(server, c, m, x);
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

    // Application functions
    server->process            = ws_svr_client_process;
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

    // TODO
    // We send back the format we get. More than that, a request with different
    // format effectively changes the entire connection default format setting.
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

vrtql_msg_svr*
vrtql_msg_svr_ctor(vrtql_msg_svr* server, int threads, int backlog, int qsize)
{
    ws_svr_ctor((vws_svr*)server, threads, backlog, qsize);

    // Server base function overrides
    server->base.process = msg_svr_client_ws_msg_in;

    // Message handling
    server->on_msg_in    = msg_svr_client_msg_in;
    server->on_msg_out   = msg_svr_client_msg_out;

    // Application functions
    server->process      = msg_svr_client_process;
    server->send         = msg_svr_client_msg_out;

    // User-defined data
    server->data         = NULL;

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
