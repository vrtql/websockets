#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

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
 * vrtql_svr_data instances. When a worker sends a vrtql_svr_data instance, it
 * adds it to the queue (server->responses) and notifies (wakes up) the main UV
 * loop (uv_run() in vrtql_svr_run()) by calling uv_async_send(&server->wakeup)
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
 * Constructs a new server instance. This takes a new, empty vrtql_svr instance
 * and initializes all of its members. It is used by derived structs as well
 * (vrtql_msg_svr) to construct the base struct.
 *
 * @param server The server instance to be initialized
 * @return The initialized server instance
 *
 * @ingroup ServerFunctions
 */

static vrtql_svr* svr_ctor(vrtql_svr* s, int nt, int backlog, int queue_size);

/**
 * @brief Server instance destructor
 *
 * Destructs an initialized server instance. This vrtql_svr instance and
 * deallocates all of its members -- everything but the top-level struct. This
 * is used by derived structs as well (vrtql_msg_svr) to destruct the base
 * struct.
 *
 * @param server The server instance to be destructed
 *
 * @ingroup ServerFunctions
 */

static void svr_dtor(vrtql_svr* s);

/**
 * @brief Initiates the server shutdown process.
 *
 * It signals all worker threads to stop processing new data and
 * to finish any requests they are currently processing.
 *
 * @param server The server that needs to be shutdown.
 *
 * @ingroup ServerFunctions
 */
static void svr_shutdown(vrtql_svr* server);

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
static vrtql_svr_cnx* svr_cnx_new(vrtql_svr* s, uv_stream_t* c);

/**
 * @brief Frees a server connection.
 *
 * @param c The connection to be freed.
 *
 * @ingroup ServerFunctions
 */
static void svr_cnx_free(vrtql_svr_cnx* c);

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
static void svr_client_connect(vrtql_svr_cnx* c);

/**
 * @brief Callback for client disconnection.
 *
 * This function is triggered when a client connection is terminated. It is
 * responsible for processing any cleanup or other steps necessary at the end of
 * a connection.
 *
 * @param c The connection structure representing the client that has disconnected.
 *
 * @ingroup ServerFunctions
 */
static void svr_client_disconnect(vrtql_svr_cnx* c);

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
static void svr_client_read(vrtql_svr_cnx* c, ssize_t size, const uv_buf_t* buf);

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
static void svr_client_data_in(vrtql_svr_data* data);

/**
 * @brief Callback for processing client data in (ingress)
 *
 * This function is triggered to process data arriving from worker thread to be
 * send back to client. It is responsible for transferring the data from the
 * response argument (vrtql_svr_data ) onto the wire (client socket via
 * libuv). actual computation or other work associated with the data. This takes
 * place in the context of uv_thread().
 *
 * @param data The outgoing data from worker to client
 *
 * @ingroup ServerFunctions
 */
static void svr_client_data_out(vrtql_svr_data* data);




/**
 * @defgroup MessageServerFunctions
 *
 * @brief Functions that support message server operation
 *
 */

/**
 * @brief Callback for client connection.
 *
 * This function is triggered when a new client connection is established.  It
 * is responsible for processing any steps necessary at the start of a
 * connection.
 *
 * @param c The connection structure representing the client that has connected.
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_connect(vrtql_svr_cnx* c);

/**
 * @brief Callback for client disconnection.
 *
 * This function is triggered when a client connection is terminated. It is
 * responsible for processing any cleanup or other steps necessary at the end of
 * a connection.
 *
 * @param c The connection structure representing the client that has disconnected.
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_disconnect(vrtql_svr_cnx* c);

/**
 * @brief Callback for processing client data in (ingress) for msg server
 *
 * This function processes data arriving from client to worker thread. It
 * collects data until there is a complete message. It passes message to
 * msg_svr_client_msg_in() for processing. This takes place in the context of
 * worker_thread().
 *
 * @param server The server instance
 * @param data The incoming data from the client to process.
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_data_in(vrtql_svr_data* data);

/**
 * @brief Callback for client message processing
 *
 * This function is triggered when a message is read from a client
 * connection. It is responsible for processing the message. This takes place in
 * the context of worker_thread().
 *
 * @param c The connection that sent the data.
 * @param m The message to process
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_msg_in(vrtql_svr_cnx* c, vrtql_msg* m);

/**
 * @brief Callback for sending message to client. It takes a message as input,
 * serializes it to a binary WebSocket message and then sends it to the
 * uv_thread() to send back to client. This takes place in the context of
 * worker_thread() (only to be called within that context).
 *
 * This function sends a message back to a client.
 *
 * @param c The connection that sent the data.
 * @param m The message to send
 *
 * @ingroup MessageServerFunctions
 */
static void msg_svr_client_msg_out(vrtql_svr_cnx* c, vrtql_msg* m);




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
static vrtql_svr_cnx* svr_cnx_map_get(vrtql_svr_cnx_map* map, uv_stream_t* key);

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
static int8_t svr_cnx_map_set( vrtql_svr_cnx_map* map,
                               uv_stream_t* key,
                               vrtql_svr_cnx* value );

/**
 * @brief Remove a client's connection from the connection map.
 *
 * @param map The connection map.
 * @param key The client (used as key in the map).
 *
 * @ingroup ConnectionMap
 */
static void svr_cnx_map_remove(vrtql_svr_cnx_map* map, uv_stream_t* key);

/**
 * @brief Clear all connections from the connection map.
 *
 * @param map The connection map.
 *
 * @ingroup ConnectionMap
 */
static void svr_cnx_map_clear(vrtql_svr_cnx_map* map);

/**
 * @defgroup QueueGroup
 *
 * @brief Queue functions which bridge the network thread and workers
 *
 * The network thread and worker threads pass data via queues. These queues
 * contain vrtql_svr_data instances. There is a requests queue which passes
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
 *
 * @ingroup QueueGroup
 */
void queue_init(vrtql_svr_queue* queue, int capacity);

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
void queue_destroy(vrtql_svr_queue* queue);

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
void queue_push(vrtql_svr_queue* queue, vrtql_svr_data* data);

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
vrtql_svr_data* queue_pop(vrtql_svr_queue* queue);

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
bool queue_empty(vrtql_svr_queue* queue);

//------------------------------------------------------------------------------
// Threads
//------------------------------------------------------------------------------

void worker_thread(void* arg)
{
    vrtql_svr* server = (vrtql_svr*)arg;

    while (true)
    {
        //> Wait for arrival

        // This will put the thread to sleep on a condition variable until
        // something arrives in queue.
        vrtql_svr_data* request = queue_pop(&server->requests);

        // If there's no request (null request), check the server's state
        if (request == NULL)
        {
            // If server is in halting state, return
            if (server->state == VS_HALTING)
            {
                if (server->trace)
                {
                    vrtql_trace(VL_INFO, "worker_thread(): exiting");
                }

                return;
            }
            else
            {
                // If not halting, skip to the next iteration of the loop
                continue;
            }
        }

        if (server->trace)
        {
            vrtql_trace(VL_INFO, "worker_thread(): process %p", request->cnx);
        }

        server->on_data_in(request);
    }
}

void uv_thread(uv_async_t* handle)
{
    vrtql_svr* server = (vrtql_svr*)handle->data;

    if (server->state == VS_HALTING)
    {
        if (server->trace)
        {
            vrtql_trace(VL_INFO, "uv_thread(): stop");
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
        vrtql_svr_data* data = queue_pop(&server->responses);

        if (vrtql_is_flag(&data->flags, VM_SVR_DATA_CLOSE))
        {
            // Close connection
            uv_close((uv_handle_t*)data->cnx->handle, svr_on_close);
        }
        else
        {
            server->on_data_out(data);
        }
    }
}

//------------------------------------------------------------------------------
// Server API
//------------------------------------------------------------------------------

vrtql_svr_data* vrtql_svr_data_new(vrtql_svr_cnx* c, ucstr data, size_t size)
{
    vrtql_svr_data* item;
    item = (vrtql_svr_data*)vrtql.malloc(sizeof(vrtql_svr_data));

    item->cnx   = c;
    item->size  = size;
    item->data  = data;
    item->flags = 0;

    return item;
}

void vrtql_svr_data_free(vrtql_svr_data* t)
{
    if (t != NULL)
    {
        free(t->data);
        free(t);
    }
}

vrtql_svr* svr_ctor(vrtql_svr* server, int nt, int backlog, int queue_size)
{
    if (backlog == 0)
    {
        backlog = 128;
    }

    if (queue_size == 0)
    {
        queue_size = 1024;
    }

    server->threads       = vrtql.malloc(sizeof(uv_thread_t) * nt);
    server->pool_size     = nt;
    server->trace         = 0;
    server->on_connect    = svr_client_connect;
    server->on_disconnect = svr_client_disconnect;
    server->on_read       = svr_client_read;
    server->on_data_in    = svr_client_data_in;
    server->on_data_out   = svr_client_data_out;
    server->backlog       = 128;
    server->loop          = (uv_loop_t*)vrtql.malloc(sizeof(uv_loop_t));
    server->state         = VS_HALTED;

    uv_loop_init(server->loop);
    sc_map_init_64v(&server->cnxs, 0, 0);
    queue_init(&server->requests, queue_size);
    queue_init(&server->responses, queue_size);
    uv_mutex_init(&server->mutex);

    server->wakeup.data = server;
    uv_async_init(server->loop, &server->wakeup, uv_thread);

    return server;
}

vrtql_svr* vrtql_svr_new(int num_threads, int backlog, int queue_size)
{
    vrtql_svr* server = vrtql.malloc(sizeof(vrtql_svr));
    return svr_ctor(server, num_threads, backlog, queue_size);
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
            vrtql_trace( VL_WARN,
                         "on_uv_close(): libuv resource not properly freed: %p",
                         (void*)handle->data );
        }
    }
}

void on_uv_walk(uv_handle_t* handle, void* arg)
{
    vrtql_trace(VL_INFO, "on_uv_walk(): %p", (void*)handle);

    // If this handle has not been closed, it should have been. Nevertheless we
    // will make an attempt to close it.
    if (uv_is_closing(handle) == 0)
    {
        uv_close(handle, (uv_close_cb)on_uv_close);
    }
}

void svr_dtor(vrtql_svr* server)
{
    if (server->state == VS_RUNNING)
    {
        vrtql_svr_stop(server);
    }

    svr_shutdown(server);
    free(server->threads);

    // Close the async handle
    server->wakeup.data = NULL;
    uv_close((uv_handle_t*)&server->wakeup, NULL);

    // If uv_loop_close() does not return 0, it is because there are active
    // handles preventing it. So we need to deal with them.
    while (uv_loop_close(server->loop))
    {
        // Walk the loop to close everything
        uv_walk(server->loop, on_uv_walk, NULL);

        // Run the loop until there are no more active handles
        while (uv_loop_alive(server->loop))
        {
            uv_run(server->loop, UV_RUN_DEFAULT);
        }
    }

    free(server->loop);

    svr_cnx_map_clear(&server->cnxs);
    sc_map_term_64v(&server->cnxs);
}

void vrtql_svr_free(vrtql_svr* server)
{
    if (server == NULL)
    {
        return;
    }

    svr_dtor(server);
    free(server);
}

int vrtql_svr_send(vrtql_svr* server, vrtql_svr_data* data)
{
    queue_push(&server->responses, data);

    // Notify event loop about the new response
    uv_async_send(&server->wakeup);
}

int vrtql_svr_run(vrtql_svr* server, cstr host, int port)
{
    for (int i = 0; i < server->pool_size; i++)
    {
        uv_thread_create(&server->threads[i], worker_thread, server);
        vrtql_trace( VL_INFO,
                     "vrtql_svr_run(): starting worker %lu",
                     server->threads[i] );
    }

    uv_tcp_t socket;
    uv_tcp_init(server->loop, &socket);
    socket.data = server;

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", port, &addr);

    uv_tcp_bind(&socket, (const struct sockaddr*)&addr, 0);

    int r = uv_listen((uv_stream_t*)&socket, server->backlog, svr_on_connect);

    if (r)
    {
        fprintf(stderr, "Listen error %s\n", uv_strerror(r));
        return -1;
    }

    uv_run(server->loop, UV_RUN_DEFAULT);

    // Close the listening socket handle
    socket.data = NULL;
    uv_close((uv_handle_t*)&socket, NULL);

    if (server->trace)
    {
        vrtql_trace(VL_INFO, "vrtql_svr_run(): stop");
    }

    server->state = VS_HALTED;

    return 0;
}

void vrtql_svr_trace(vrtql_svr* server, int flag)
{
    server->trace           = flag;
    server->requests.trace  = flag;
    server->responses.trace = flag;
}

void vrtql_svr_stop(vrtql_svr* server)
{
    // Set shutdown flags
    server->state           = VS_HALTING;
    server->requests.state  = VS_HALTING;
    server->responses.state = VS_HALTING;

    // Wakeup all worker threads
    if (server->trace)
    {
        vrtql_trace(VL_INFO, "vrtql_svr_stop(): shutdown workers");
    }

    uv_mutex_lock(&server->requests.mutex);
    uv_cond_broadcast(&server->requests.cond);
    uv_mutex_unlock(&server->requests.mutex);

    // Wakeup the main event loop to shutdown main thread
    if (server->trace)
    {
        vrtql_trace(VL_INFO, "vrtql_svr_stop(): shutdown main thread");
    }

    uv_async_send(&server->wakeup);

    while (server->state != VS_HALTED)
    {
        sleep(1);
    }
}

//------------------------------------------------------------------------------
// Client connection callbacks
//------------------------------------------------------------------------------

void svr_client_connect(vrtql_svr_cnx* c)
{
    if (c->server->trace)
    {
        vrtql_trace(VL_INFO, "svr_client_connect(%p)", c->handle);
    }
}

void svr_client_disconnect(vrtql_svr_cnx* c)
{
    if (c->server->trace)
    {
        vrtql_trace(VL_INFO, "svr_client_disconnect(%p)", c->handle);
    }
}

void svr_client_read(vrtql_svr_cnx* c, ssize_t size, const uv_buf_t* buf)
{
    vrtql_svr* server = c->server;

    if (server->trace)
    {
        vrtql_trace(VL_INFO, "svr_client_read(%p)", c);
    }

    // Queue data to worker pool for processing
    vrtql_svr_data* data = vrtql_svr_data_new(c, buf->base, size);
    queue_push(&server->requests, data);
}

void svr_client_data_in(vrtql_svr_data* req)
{
    vrtql_svr* server = req->cnx->server;

    //> Prepare the response: echo the data back

    // Allocate memory for the data to be sent in response
    char* data = (char*)vrtql.malloc(req->size);

    // Copy the request's data to the response data
    strncpy(data, req->data, req->size);

    // Create response
    vrtql_svr_data* reply = vrtql_svr_data_new(req->cnx, data, req->size);

    // Free request
    vrtql_svr_data_free(req);

    if (server->trace)
    {
        vrtql_trace(VL_INFO, "worker_thread(): %p queing", reply->cnx);
    }

    // Send reply. This will wakeup network thread.
    vrtql_svr_send(server, reply);

    if (server->trace)
    {
        vrtql_trace(VL_INFO, "worker_thread(): %p done", reply->cnx);
    }
}

void svr_client_data_out(vrtql_svr_data* data)
{
    uv_buf_t buf    = uv_buf_init(data->data, strlen(data->data));
    uv_write_t* req = (uv_write_t*)vrtql.malloc(sizeof(uv_write_t));
    req->data       = data;

    uv_write(req, data->cnx->handle, &buf, 1, svr_on_write_complete);
}

//------------------------------------------------------------------------------
// Server Utilities
//------------------------------------------------------------------------------

vrtql_svr_cnx* svr_cnx_map_get(vrtql_svr_cnx_map* map, uv_stream_t* key)
{
    vrtql_svr_cnx* cnx = sc_map_get_64v(map, (uint64_t)key);

    // If entry exists
    if (sc_map_found(map) == true)
    {
        return cnx;
    }

    return NULL;
}

void svr_cnx_map_remove(vrtql_svr_cnx_map* map, uv_stream_t* key)
{
    vrtql_svr_cnx* cnx = sc_map_get_64v(map, (uint64_t)key);

    // If entry exists
    if (sc_map_found(map) == true)
    {
        sc_map_del_64v(map, (uint64_t)key);

        // Call on_disconnect() handler
        cnx->server->on_disconnect(cnx);

        // Cleanup
        free(cnx);
    }
}

int8_t svr_cnx_map_set( vrtql_svr_cnx_map* map,
                        uv_stream_t* key,
                        vrtql_svr_cnx* value )
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

void svr_cnx_map_clear(vrtql_svr_cnx_map* map)
{
    uint64_t key; vrtql_svr_cnx* cnx;
    sc_map_foreach(map, key, cnx)
    {
        cnx->server->on_disconnect(cnx);
        free(cnx);
    }

    sc_map_clear_64v(map);
}

vrtql_svr_cnx* svr_cnx_new(vrtql_svr* s, uv_stream_t* handle)
{
    vrtql_svr_cnx* cnx = vrtql.malloc(sizeof(vrtql_svr_cnx));
    cnx->server        = s;
    cnx->handle        = handle;
    cnx->data          = NULL;
    cnx->format        = VM_MPACK_FORMAT;

    return cnx;
}

void svr_cnx_free(vrtql_svr_cnx* c)
{
    if (c != NULL)
    {
        free(c);
    }
}

void svr_shutdown(vrtql_svr* server)
{
    // Cleanup libuv
    queue_destroy(&server->requests);
    queue_destroy(&server->responses);

    // Stop the loop. This will cause uv_run() to return in vrtql_svr_run()
    // which will also return.
    uv_stop(server->loop);
}

void svr_on_connect(uv_stream_t* socket, int status)
{
    vrtql_svr* server = (vrtql_svr*) socket->data;

    if (status < 0)
    {
        cstr e = uv_strerror(status);
        vrtql.error(VE_RT, "Error in connection callback: %s", e);
        return;
    }

    uv_tcp_t* client = (uv_tcp_t*)vrtql.malloc(sizeof(uv_tcp_t));

    if (client == NULL)
    {
        vrtql.error(VE_RT, "Failed to allocate memory for client");
        return;
    }

    client->data = server;

    if (uv_tcp_init(server->loop, client) != 0)
    {
        vrtql.error(VE_RT, "Failed to initialize client");
        return;
    }

    if (uv_accept(socket, (uv_stream_t*)client) == 0)
    {
        if (uv_read_start((uv_stream_t*)client, svr_on_realloc, svr_on_read) != 0)
        {
            vrtql.error(VE_RT, "Failed to start reading from client");
            return;
        }
    }
    else
    {
        uv_close((uv_handle_t*)client, svr_on_close);
    }

    //> Add connection to registry and initialize

    vrtql_svr_cnx* cnx = svr_cnx_new(server, (uv_stream_t*)client);

    if (svr_cnx_map_set(&server->cnxs, (uv_stream_t*)client, cnx) == false)
    {
        vrtql.error(VE_FATAL, "Connection already registered");
    }

    //> Call svr_on_connect() handler

    server->on_connect(cnx);
}

void svr_on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf)
{
    vrtql_svr* server = (vrtql_svr*)client->data;

    if (nread < 0)
    {
        uv_close((uv_handle_t*)client, svr_on_close);
        free(buf->base);
        return;
    }

    struct sc_map_64v* map = &server->cnxs;
    vrtql_svr_cnx* cnx     = sc_map_get_64v(map, (uint64_t)client);

    if (sc_map_found(map) == true)
    {
        server->on_read(cnx, nread, buf);
    }
}

void svr_on_write_complete(uv_write_t* req, int status)
{
    vrtql_svr_data_free((vrtql_svr_data*)req->data);
    free(req);
}

void svr_on_close(uv_handle_t* handle)
{
    vrtql_svr* server      = handle->data;
    vrtql_svr_cnx_map* map = &server->cnxs;
    vrtql_svr_cnx* cnx     = sc_map_get_64v(map, (uint64_t)handle);

    // If entry exists
    if (sc_map_found(map) == true)
    {
        // Call on_disconnect() handler
        server->on_disconnect(cnx);

        // Remove from map
        sc_map_del_64v(map, (uint64_t)handle);

        // Cleanup
        free(cnx);
    }

    free(handle);
}

void svr_on_realloc(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
    buf->base = (char*)vrtql.realloc(buf->base, size);
    buf->len  = size;
}

//------------------------------------------------------------------------------
// Queue API
//------------------------------------------------------------------------------

void queue_init(vrtql_svr_queue* queue, int size)
{
    queue->buffer   = (vrtql_svr_data**)vrtql.malloc(size * sizeof(vrtql_svr_data*));
    queue->size     = 0;
    queue->capacity = size;
    queue->head     = 0;
    queue->tail     = 0;
    queue->state    = VS_RUNNING;
    queue->trace    = 0;

    // Initialize mutex and condition variable
    uv_mutex_init(&queue->mutex);
    uv_cond_init(&queue->cond);
}

void queue_destroy(vrtql_svr_queue* queue)
{
    if (queue->buffer != NULL)
    {
        uv_mutex_destroy(&queue->mutex);
        uv_cond_destroy(&queue->cond);
        free(queue->buffer);
        queue->buffer = NULL;
        queue->state  = VS_HALTED;
    }
}

void queue_push(vrtql_svr_queue* queue, vrtql_svr_data* data)
{
    if (queue->trace)
    {
        vrtql_trace(VL_INFO, "queue_push(enter) %p", data->cnx);
    }

    uv_mutex_lock(&queue->mutex);

    while (queue->size == queue->capacity)
    {
        uv_cond_wait(&queue->cond, &queue->mutex);
    }

    queue->buffer[queue->tail] = data;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;

    // Signal condition variable
    uv_cond_signal(&queue->cond);
    uv_mutex_unlock(&queue->mutex);

    if (queue->trace)
    {
        vrtql_trace(VL_INFO, "queue_push(leave) %p", data->cnx);
    }
}

vrtql_svr_data* queue_pop(vrtql_svr_queue* queue)
{
    if (queue->trace)
    {
        vrtql_trace(VL_INFO, "queue_pop(enter)");
    }

    uv_mutex_lock(&queue->mutex);

    while (queue->size == 0 && queue->state == VS_RUNNING)
    {
        uv_cond_wait(&queue->cond, &queue->mutex);
    }

    if (queue->trace)
    {
        vrtql_trace(VL_INFO, "queue_pop(wakeup)");
    }

    if (queue->state == VS_HALTING)
    {
        uv_cond_broadcast(&queue->cond);
        uv_mutex_unlock(&queue->mutex);

        return NULL;
    }

    vrtql_svr_data* data = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    uv_cond_signal(&queue->cond);
    uv_cond_broadcast(&queue->cond);
    uv_mutex_unlock(&queue->mutex);

    return data;
}

bool queue_empty(vrtql_svr_queue* queue)
{
    uv_mutex_lock(&queue->mutex);
    bool empty = (queue->size == 0);
    uv_mutex_unlock(&queue->mutex);

    return empty;
}

//------------------------------------------------------------------------------
// Messaging Server
//------------------------------------------------------------------------------

void msg_svr_process_frame(vws_cnx* c, vws_frame* f)
{
    vrtql_svr_cnx* cnx = (vrtql_svr_cnx*)c->data;

    switch (f->opcode)
    {
        case CLOSE_FRAME:
        {
            // Build the response frame
            vrtql_buffer* buffer = vws_generate_close_frame();

            // Send back to cliane Send the PONG response
            vrtql_svr_data* response;

            response = vrtql_svr_data_new(cnx, buffer->data, buffer->size);
            vrtql_svr_send(cnx->server, response);

            // Free buffer: response took ownership of buffer memory
            buffer->data = NULL;
            buffer->size = 0;
            vrtql_buffer_free(buffer);

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
            vrtql_buffer* buffer = vws_generate_pong_frame(f->data, f->size);

            // Send back to cliane Send the PONG response
            vrtql_svr_data* response;
            response = vrtql_svr_data_new(cnx, buffer->data, buffer->size);
            vrtql_svr_send(cnx->server, response);

            // Free buffer: response took ownership of buffer memory
            buffer->data = NULL;
            buffer->size = 0;
            vrtql_buffer_free(buffer);

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

    vrtql.success();
}

void msg_svr_client_connect(vrtql_svr_cnx* c)
{
    if (c->server->trace)
    {
        vrtql_trace(VL_INFO, "msg_svr_client_connect(%p)", c->handle);
    }

    // Create a new vws_cnx
    vws_cnx* cnx = (void*)vws_cnx_new();
    cnx->process = msg_svr_process_frame;
    cnx->data    = (void*)c;   // Link cnx -> c
    c->data      = (void*)cnx; // Link c -> cnx
}

void msg_svr_client_disconnect(vrtql_svr_cnx* c)
{
    if (c->server->trace)
    {
        vrtql_trace(VL_INFO, "msg_svr_client_disconnect(%p)", c->handle);
    }

    if (c->data != NULL)
    {
        vws_cnx_free((vws_cnx*)c->data);
        c->data = NULL;
    }
}

void msg_svr_client_data_in(vrtql_svr_data* data)
{
    //> Append data to connection buffer

    vrtql_svr_cnx* cnx    = data->cnx;
    vrtql_msg_svr* server = (vrtql_msg_svr*)cnx->server;
    vws_cnx* c            = (vws_cnx*)cnx->data;
    vrtql_buffer_append(c->base.buffer, data->data, data->size);

    // If we are in HTTP mode
    if (server->upgraded == false)
    {
        // Parse incoming data as HTTP request.

        cstr data   = c->base.buffer->data;
        size_t size = c->base.buffer->size;
        ssize_t n   = vrtql_http_req_parse(server->http, data, size);

        // Did we get a complete request?
        if (server->http->complete == true)
        {
            // Check for parsing errors
            enum http_errno err = HTTP_PARSER_ERRNO(server->http->parser);

            // If there was a parsing error, close connection
            if(err != HPE_OK)
            {
                vrtql.error(VE_RT, "Error: %s (%s)",
                            http_errno_name(err),
                            http_errno_description(err) );

                // Create reply
                vrtql_svr_data* reply;
                reply = vrtql_svr_data_new(cnx, NULL, 0);

                // Set connection close flag
                vrtql_set_flag(&reply->flags, VM_SVR_DATA_CLOSE);

                // Queue the data to uv_thread() to send out on wire
                vrtql_svr_send(cnx->server, reply);

                return;
            }

            // Drain HTTP request data from cnx->data buffer
            vrtql_buffer_drain(c->base.buffer, n);

            //> Generate HTTP response and send

            vrtql_buffer* http = vrtql_buffer_new();

            struct sc_map_str* headers = &server->http->headers;
            cstr key   = vrtql_map_get(headers, "sec-websocket-key");
            cstr proto = vrtql_map_get(headers, "sec-websocket-protocol");

            vrtql_buffer_printf(http, "HTTP/1.1 101 Switching Protocols\r\n");
            vrtql_buffer_printf(http, "Upgrade: websocket\r\n");
            vrtql_buffer_printf(http, "Connection: Upgrade\r\n");

            cstr ac = vws_accept_key(key);
            vrtql_buffer_printf(http, "Sec-WebSocket-Accept: %s\r\n", ac);
            free(ac);

            vrtql_buffer_printf(http, "Sec-WebSocket-Version: 13\r\n");

            vrtql_buffer_printf(http, "Sec-WebSocket-Protocol: ");

            if (strlen(proto) > 0)
            {
                vrtql_buffer_printf(http, "%s\r\n", proto);
            }
            else
            {
                vrtql_buffer_printf(http, "vrtql\r\n");
            }

            vrtql_buffer_printf(http, "\r\n");

            // Package up response
            vrtql_svr_data* reply;
            reply = vrtql_svr_data_new(cnx, http->data, http->size);

            // Queue the data to uv_thread() to send out on wire
            vrtql_svr_send(cnx->server, reply);

            // Cleanup. Buffer data was passed to reply.
            http->data = NULL; http->size = 0;
            vrtql_buffer_free(http);

            //> Change state to WebSocket mode

            // Set the flag that we are in WebSockets mode
            server->upgraded = true;

            // Free HTTP request we don't need it
            vrtql_http_req_free(server->http);
            server->http = NULL;

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
        // WebSocket data to process (c->buffer->size > 0)
    }

    //> Process connection buffer data for complete messages

    if (vws_socket_ingress(c) > 0)
    {
        // Process as many messages as possible
        while (true)
        {
            // Check for a complete message
            vws_msg* wsm = vws_pop_message(c);

            if (wsm == NULL)
            {
                return;
            }

            //> Deserialize message

            vrtql_msg* m = vrtql_msg_new();
            cstr data    = wsm->data->data;
            size_t size  = wsm->data->size;

            if (vrtql_msg_deserialize(m, data, size) == false)
            {
                // Error already set
                vws_msg_free(wsm);
                vrtql_msg_free(m);

                // Try for more messages
                continue;
            }
            else
            {
                // Deserialized succeeded.

                // We send back the format we get. More than that, a request
                // with different format effectively changes the entire
                // connection default format setting.
                cnx->format = m->format;

                // Free websocket message
                vws_msg_free(wsm);

                // Process message
                server->on_msg_in(cnx, m);
            }
        }
    }
}

void msg_svr_client_msg_in(vrtql_svr_cnx* cnx, vrtql_msg* m)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)cnx->server;

    // Route to application-specific processing callback
    server->process(cnx, m);
}

void msg_svr_client_process(vrtql_svr_cnx* cnx, vrtql_msg* req)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)cnx->server;

    if (server->base.trace)
    {
        vrtql_trace(VL_INFO, "msg_svr_process(%p) %s", cnx, req);
    }

    // Default: Do nothing
    //
    // Note: You should always set reply messages format to the format of the
    // connection. For example, when creating reply messages:
    //
    //    vrtql_msg* reply = vrtql_msg_new();
    //    reply->format    = cnx->format;
    //
    // We Could echo back (then we don't free message as reply() does it)
    // server->send(cnx, reply);

    // Clean up message.
    vrtql_msg_free(req);
}

void msg_svr_client_msg_out(vrtql_svr_cnx* cnx, vrtql_msg* m)
{
    // Serialize to WebSocket message
    vrtql_buffer* buffer;
    buffer = vrtql_msg_serialize(m);

    // Pack message binary into queue data
    vrtql_svr_data* response;
    response = vrtql_svr_data_new(cnx, buffer->data, buffer->size);

    // Queue the data to uv_thread() to send out on wire
    vrtql_svr_send(cnx->server, response);

    // Free buffer
    vrtql_buffer_free(buffer);

    // Free message
    vrtql_msg_free(m);
}

vrtql_msg_svr* vrtql_msg_svr_new(int num_threads, int backlog, int queue_size)
{
    vrtql_msg_svr* server = vrtql.malloc(sizeof(vrtql_msg_svr));
    svr_ctor((vrtql_svr*)server, num_threads, backlog, queue_size);

    // Server base function overrides
    server->base.on_connect    = msg_svr_client_connect;
    server->base.on_disconnect = msg_svr_client_disconnect;
    server->base.on_data_in    = msg_svr_client_data_in;

    // Initialize HTTP state
    server->upgraded           = false;
    server->http               = vrtql_http_req_new();

    // Message handling
    server->on_msg_in          = msg_svr_client_msg_in;
    server->on_msg_out         = msg_svr_client_msg_out;

    // Application functions
    server->process            = msg_svr_client_process;
    server->send               = msg_svr_client_msg_out;

    return server;
}

void vrtql_msg_svr_free(vrtql_msg_svr* server)
{
    if (server == NULL)
    {
        return;
    }

    if (server->http != NULL)
    {
        vrtql_http_req_free(server->http);
    }

    svr_dtor((vrtql_svr*)server);
    free(server);
}

