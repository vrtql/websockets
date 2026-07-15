#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#if defined(__windows__)
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601  // Windows 7 or later
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <openssl/rand.h>

#include "http_message.h"
#include "websocket.h"
#include "url.h"

#define MAX_BUFFER_SIZE 1024



//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

/**
 * @defgroup ConnectionFunctions
 *
 * @brief Functions that manage WebSocket connections
 *
 */

/**
 * @brief Attempts connection based on previously parsed URL
 *
 * @param c The websocket connection.
 * @return Returns true if the connection is successful, false otherwise.
 */
static bool cnx_connect();

/**
 * @brief Generates a new, random WebSocket key for the handshake process.
 *
 * @return A pointer to the generated WebSocket key.
 *
 * @ingroup ConnectionFunctions
 */
static char* generate_websocket_key();

/**
 * @brief Extracts the WebSocket accept key from a server's handshake response.
 *
 * @param  response The server's handshake response.
 * @return A pointer to the extracted WebSocket accept key.
 *
 * @ingroup ConnectionFunctions
 */
static char* extract_websocket_accept_key(const char* response);

/**
 * @brief Verifies the server's handshake response using the client's original
 *        key.
 *
 * @param key The client's original key.
 * @param response The server's handshake response.
 * @return 0 if the handshake is verified successfully, an error code otherwise.
 *
 * @ingroup ConnectionFunctions
 */
static int verify_handshake(const char* key, const char* response);




/**
 * @defgroup SocketFunctions
 *
 * @brief Functions that manage sockets
 *
 */

/**
 * @brief Performs WebSocket connection. This is integrated as callback into
 *        vws_connect().
 *
 * @param s The WebSocket connection
 * @return Returns true if handshake succeeded, false otherwise
 *
 * @ingroup ConnectionFunctions
 */

static bool socket_handshake(vws_socket* s);

/**
 * @brief Waits for a complete frame to be available from a WebSocket
 *        connection.
 *
 * @param c The vws_cnx representing the WebSocket connection.
 * @return The number of bytes read and processed, or an error code if an error
 *         occurred.
 *
 * @ingroup SocketFunctions
 */
static ssize_t socket_wait_for_frame(vws_cnx* c);

/**
 * @defgroup FrameFunctions
 *
 * @brief Functions that manage websocket frames
 *
 */

/**
 * @brief Processes a single frame from a WebSocket connection.
 *
 * @param c The vws_cnx representing the WebSocket connection.
 * @param frame The vws_frame to process.
 * @return void
 *
 * @ingroup FrameFunctions
 */
static void process_frame(vws_cnx* c, vws_frame* frame);




/**
 * @defgroup MessageFunctions
 *
 * @brief Functions that manage websocket messages
 *
 */

/**
 * @brief Checks if a complete message is available in the connection's message
 *        queue.
 *
 * @param c The vws_cnx representing the WebSocket connection.
 * @return True if a complete message is available, false otherwise.
 *
 * @ingroup MessageFunctions
 */
static bool has_complete_message(vws_cnx* c);




/**
 * @defgroup TraceFunctions
 *
 * @brief Functions that provide tracing and debugging
 *
 */

/**
 * @brief Structure representing a WebSocket header.
 *
 * @ingroup TraceFunctions
 */
typedef struct
{
    uint8_t fin;          /**< Indicates the final frame of a message */
    uint8_t opcode;       /**< Identifies the frame type */
    uint8_t mask;         /**< Indicates if the frame payload is masked */
    uint64_t payload_len; /**< Length of the payload data */
    uint32_t masking_key; /**< Key used for payload data masking */
} ws_header;

/**
 * @brief Dumps the contents of a WebSocket header for debugging purposes.
 *
 * @param header The WebSocket header to dump.
 *
 * @ingroup TraceFunctions
 */
static void dump_websocket_header(const ws_header* header);

//------------------------------------------------------------------------------
//> Connection API
//------------------------------------------------------------------------------

bool cnx_connect(vws_cnx* c)
{
    // [F4] url_parse() (in vws_connect) can return NULL, and a partial parse
    // can leave protocol/host NULL; guard before every deref below -- host at
    // the check itself, protocol at the strcmp()s, and path/host in
    // socket_handshake.
    if (c->url == NULL || c->url->host == NULL || c->url->protocol == NULL)
    {
        vws.error(VE_MEM, "Invalid or missing URL (host/protocol)");
        return false;
    }

    // Connect to the server
    cstr default_port = strcmp(c->url->protocol, "wss") == 0 ? "443" : "80";
    cstr port = c->url->port != NULL ? c->url->port : default_port;

    bool ssl = false;
    if (strcmp(c->url->protocol, "wss") == 0)
    {
        ssl = true;
    }

    return vws_socket_connect((vws_socket*)c, c->url->host, atoi(port), ssl);
}

vws_cnx* vws_cnx_new()
{
    vws_cnx* c = (vws_cnx*)vws.malloc(sizeof(vws_cnx));

    return vws_cnx_ctor(c);
}

vws_cnx* vws_cnx_ctor(vws_cnx* c)
{
    memset(c, 0, sizeof(vws_cnx));

    // Call base constructor
    vws_socket_ctor((vws_socket*)c);

    c->base.hs    = socket_handshake;
    c->flags      = CNX_CLOSED;
    c->url        = NULL;
    c->key        = generate_websocket_key();
    c->process    = process_frame;
    c->disconnect = NULL;
    c->data       = NULL;

    sc_queue_init(&c->queue);

    // Default the reassembly cap (settable post-construction, e.g. by
    // the broker from app.conf). msg_bytes is 0 from the memset above.
    c->max_message_size = VWS_MAX_MESSAGE_SIZE;

    // Heartbeat liveness: ping_outstanding/ping_sent_ts are 0 from the memset.
    // Seed last_active to now so a fresh connection is not immediately idle.
    c->last_active = vws_now_ms();

    return c;
}

void vws_cnx_free(vws_cnx* c)
{
    if (c == NULL)
    {
        return;
    }

    // In-place teardown, then release the object (the alloc symmetric to new).
    vws_cnx_dtor(c);

    vws.free(c);
}

void vws_cnx_dtor(vws_cnx* c)
{
    if (c == NULL)
    {
        return;
    }

    vws_disconnect(c);

    // Free receive queue contents
    vws_frame* f;
    sc_queue_foreach (&c->queue, f)
    {
        vws_frame_free(f);
    }

    // Free receive queue
    sc_queue_term(&c->queue);

    // Free URL
    if (c->url != NULL)
    {
        url_free((url_data_t*)c->url);
        c->url = NULL;
    }

    // Free websocket key
    vws.free(c->key);

    // Base teardown WITHOUT freeing the object (mirrors vws_socket_dtor minus
    // the final vws.free): disconnect closes ssl + fd, then free the buffer.
    vws_socket_disconnect((vws_socket*)c);
    vws_buffer_free(c->base.buffer);
    c->base.buffer = NULL;
}

void vws_cnx_set_server_mode(vws_cnx* c)
{
    vws_set_flag(&c->flags, CNX_SERVER);
}

bool vws_connect(vws_cnx* c, cstr uri)
{
    if (c == NULL)
    {
        // Return early if failed to create a connection.
        vws.error(VE_RT, "Invalid connection pointer()");
        return false;
    }

    if (c->url != NULL)
    {
        url_free((url_data_t*)c->url);
    }

    c->url = (vws_url_data*)url_parse(uri);

    return cnx_connect(c);
}

bool vws_reconnect(vws_cnx* c)
{
    if (vws_cnx_is_connected(c) == true)
    {
        return true;
    }

    if (c->url != NULL)
    {
        // [F9] Return the reconnect result; it was discarded, so vws_reconnect
        // always reported false even on a successful reconnect.
        return cnx_connect(c);
    }

    return false;
}

bool vws_cnx_is_connected(vws_cnx* c)
{
    if (vws_socket_is_connected((vws_socket*)c) == false)
    {
        return false;
    }

    return true;
}

bool socket_handshake(vws_socket* s)
{
    vws_cnx* c = (vws_cnx*)s;

    // Send the WebSocket handshake request
    const char* rt =
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Cache-Control: no-cache\r\n"
        "Origin: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    char req[MAX_BUFFER_SIZE];

    // [F11] c->key can be NULL if RAND_bytes failed in the ctor, and a partial
    // URL parse can leave path/href NULL; passing NULL to a %s conversion is
    // undefined behavior.
    if (c->key == NULL || c->url->path == NULL || c->url->href == NULL)
    {
        vws.error(VE_RT, "handshake: missing key or URL path/href");
        return false;
    }

    int req_len = snprintf(req, sizeof(req), rt, c->url->path, c->url->host,
                           c->url->href, c->key);

    // [F11] Detect truncation: snprintf returns the length it WOULD have
    // written, so a long URL (~1 KiB) would otherwise send a malformed request.
    if (req_len < 0 || (size_t)req_len >= sizeof(req))
    {
        vws.error(VE_RT, "handshake: request too large (URL too long)");
        return false;
    }

    ssize_t n;
    size_t total = 0;
    size_t size  = strlen(req);

    while (true)
    {
        // [F10] Write from the current offset; the old form re-sent the whole
        // buffer each pass, so a partial write duplicated the leading bytes.
        n = vws_socket_write(s, (ucstr)req + total, size - total);

        if (vws_cnx_is_connected(c) == false)
        {
            return false;
        }

        if (n > 0)
        {
            total += n;

            if (total == size)
            {
                break;
            }
        }

        if (n == 0)
        {
            if (vws.e.code == VE_TIMEOUT)
            {
                return false;
            }
        }

        if (n < 0)
        {
            return false;
        }
    }

    // Create HTTP response message
    vws_http_msg* http = vws_http_msg_new(HTTP_RESPONSE);

    // Read until full HTTP response is received
    while (true)
    {
        n = vws_socket_read(s);

        if (vws_cnx_is_connected(c) == false)
        {
            // Clear the socket buffer of anything that did arrive,
            // otherwise it will possibly be in inconsistent state.
            vws_buffer_clear(c->base.buffer);

            // Free HTTP response
            vws_http_msg_free(http);

            return false;
        }

        // If there was a timeout
        if (n == 0)
        {
            // Fail
            if (vws.e.code == VE_TIMEOUT)
            {
                // Clear the socket buffer of anything that did arrive,
                // otherwise it will possibly be in inconsistent state.
                vws_buffer_clear(c->base.buffer);

                // Free HTTP response
                vws_http_msg_free(http);

                return false;
            }
        }

        if (n < 0)
        {
            // Clear the socket buffer of anything that did arrive,
            // otherwise it will possibly be in inconsistent state.
            vws_buffer_clear(c->base.buffer);

            // Free HTTP response
            vws_http_msg_free(http);

            return false;
        }

        if (s->buffer->size > 0)
        {
            cstr data   = (cstr)s->buffer->data;
            size_t size = s->buffer->size;
            ssize_t n   = vws_http_msg_parse(http, data, size);

            // [F5] A parse error (-1) is a handshake failure. Draining
            // (size_t)(-1) below would clear the ENTIRE buffer, discarding any
            // frames that arrived after the 101.
            if (n < 0)
            {
                vws_buffer_clear(c->base.buffer);
                vws_http_msg_free(http);
                return false;
            }

            // [F2] llhttp is incremental/stateful: drain the bytes it consumed
            // THIS pass so the next read feeds only NEW bytes. Re-feeding
            // already-parsed bytes errors the parser and poisons its state, so
            // headers_complete may never be reached on a split 101 response.
            vws_buffer_drain(c->base.buffer, n);

            if (http->headers_complete == true)
            {
                break;
            }
        }
    }

    vws_kvs* headers = http->headers;
    cstr accept_key  = vws_kvs_get_cstring(headers, "sec-websocket-accept");

    if (accept_key == NULL)
    {
        vws.error(VE_SYS, "connect failed: no accept key returned");
        // [F3] free the HTTP response on this failure path (was leaked).
        vws_http_msg_free(http);
        return false;
    }

    // [F3] accept_key is a kvs-OWNED (borrowed) pointer -- never free it (that
    // frees memory the kvs still references). It also dangles once http (which
    // owns the kvs) is freed, so capture the verify result BEFORE the free.
    bool verified = verify_handshake(c->key, accept_key);

    vws_http_msg_free(http);

    if (verified == false)
    {
        vws.error(VE_RT, "Handshake verification failed");
        return false;
    }

    return true;
}

void vws_disconnect(vws_cnx* c)
{
    vws_socket* s = (vws_socket*)c;

    if (vws_cnx_is_connected(c) == false)
    {
        return;
    }

    // If disconnect callback is registered
    if (c->disconnect != NULL)
    {
        // Call it
        c->disconnect(c);
    }

    c->flags = CNX_CLOSED;

    vws_buffer* buffer = vws_generate_close_frame();

    // [F6] vws_generate_close_frame -> vws_serialize returns NULL on a
    // RAND_bytes failure; guard before dereferencing buffer below.
    if (buffer != NULL)
    {
        for (size_t i = 0; i < buffer->size;)
        {
            int n = vws_socket_write(s, buffer->data + i, buffer->size - i);

            if (n < 0)
            {
                break;
            }

            i += n;
        }

        vws_buffer_free(buffer);
    }

    vws_socket_disconnect(s);
}

//------------------------------------------------------------------------------
//> Messaging API
//------------------------------------------------------------------------------

ssize_t vws_frame_send_text(vws_cnx* c, cstr data)
{
    return vws_frame_send_data(c, (ucstr)data, strlen(data), 0x1);
}

ssize_t vws_frame_send_binary(vws_cnx* c, ucstr data, size_t size)
{
    return vws_frame_send_data(c, data, size, 0x2);
}

ssize_t vws_frame_send_data(vws_cnx* c, ucstr data, size_t size, int oc)
{
    return vws_frame_send(c, vws_frame_new(data, size, oc));
}

ssize_t vws_msg_send_text(vws_cnx* c, cstr data)
{
    return vws_frame_send_data(c, (ucstr)data, strlen(data), 0x1);
}

ssize_t vws_msg_send_binary(vws_cnx* c, ucstr data, size_t size)
{
    return vws_frame_send_data(c, data, size, 0x2);
}

ssize_t vws_msg_send_data(vws_cnx* c, ucstr data, size_t size, int oc)
{
    return vws_frame_send(c, vws_frame_new(data, size, oc));
}

ssize_t vws_frame_send(vws_cnx* c, vws_frame* frame)
{
    if (vws_cnx_is_connected(c) == false)
    {
        // C-WS-1: this function owns `frame` (the connected path frees it via
        // vws_serialize). The not-connected early-return leaked it.
        vws_frame_free(frame);
        return -1;
    }

    // Is connection in server mode?
    if (vws_is_flag(&c->flags, CNX_SERVER))
    {
        // Don't mask it
        frame->mask = 0;
    }

    vws_buffer* binary = vws_serialize(frame);

    // [F6] vws_serialize returns NULL on RAND_bytes failure (and has already
    // freed the frame); bail rather than dereference binary->data below.
    if (binary == NULL)
    {
        return -1;
    }

    if (vws.tracelevel >= VT_PROTOCOL)
    {
        vws.trace(VL_INFO, "Sending frame");
        vws_trace_lock();
        printf("+----------------------------------------------------+\n");
        printf("| Frame Sent                                         |\n");
        printf("+----------------------------------------------------+\n");

        vws_dump_websocket_frame(binary->data, binary->size);
        printf("------------------------------------------------------\n");
        vws_trace_unlock();
    }

    ssize_t n = 0;

    if (binary->data != NULL)
    {
        n = vws_socket_write((vws_socket*)c, binary->data, binary->size);
        vws_buffer_free(binary);

        if (vws_cnx_is_connected(c) == false)
        {
            return -1;
        }
    }

    vws.success();

    return n;
}

//------------------------------------------------------------------------------
//> Message API
//------------------------------------------------------------------------------

vws_msg* vws_msg_new()
{
    vws_msg* m = vws.malloc(sizeof(vws_msg));
    m->opcode  = 0;
    m->data    = vws_buffer_new();

    return m;
}

void vws_msg_free(vws_msg* m)
{
    if (m != NULL)
    {
        vws_buffer_free(m->data);
        vws.free(m);
    }
}

vws_msg* vws_msg_recv(vws_cnx* c)
{
    // Default success unless error
    vws.success();

    // [F11] Return an already-complete queued message even if the connection
    // has since disconnected -- those bytes already arrived and were framed;
    // dropping them on disconnect loses delivered data.
    vws_msg* queued = vws_msg_pop(c);

    if (queued != NULL)
    {
        return queued;
    }

    if (vws_cnx_is_connected(c) == false)
    {
        return NULL;
    }

    while (true)
    {
        vws_msg* msg = vws_msg_pop(c);

        if (msg != NULL)
        {
            return msg;
        }

        if (socket_wait_for_frame(c) <= 0)
        {
            break;
        }
    }

    return NULL;
}

//------------------------------------------------------------------------------
//> Frame API
//------------------------------------------------------------------------------

vws_frame* vws_frame_new(ucstr data, size_t s, unsigned char oc)
{
    vws_frame* f = vws.malloc(sizeof(vws_frame));

    // We must make our own copy of the data for deterministic memory management

    f->fin    = 1;
    f->opcode = oc;
    f->mask   = 1;
    f->offset = 0;
    f->size   = s;
    f->data   = NULL;

    if (f->size > 0)
    {
        f->data = vws.malloc(f->size);
        memcpy(f->data, data, f->size);
    }

    return f;
}

void vws_frame_free(vws_frame* f)
{
    if (f != NULL)
    {
        if (f->data != NULL)
        {
            vws.free(f->data);
            f->data = NULL;
        }

        f->size = 0;

        vws.free(f);
    }
}

vws_frame* vws_frame_recv(vws_cnx* c)
{
    // Default success unless error
    vws.success();

    if (vws_cnx_is_connected(c) == false)
    {
        return NULL;
    }

    while (true)
    {
        if (sc_queue_size(&c->queue) > 0)
        {
            return sc_queue_del_last(&c->queue);
        }

        if (socket_wait_for_frame(c) <= 0)
        {
            break;
        }
    }

    return NULL;
}

vws_buffer* vws_serialize(vws_frame* f)
{
    if (f == NULL)
    {
        vws.error(VE_RT, "empty frame");

        return NULL;
    }

    //> Section 1: Size calculation

    // Calculate the frame size
    uint64_t payload_length = f->size;

    // Set the mask bit and payload length. Maximum frame size with extended
    // payload length and masking key
    unsigned char header[14];

    // Minimum frame size
    size_t header_size = 2;

    // Set the FIN bit and opcode
    header[0] = f->fin << 7 | f->opcode;

    if (payload_length <= 125)
    {
        header[1] = payload_length;
    }
    else if (payload_length <= 65535)
    {
        header[1] = 126;
        header[2] = (payload_length >> 8) & 0xFF;
        header[3] = payload_length & 0xFF;

        // Additional bytes for payload length
        header_size += 2;
    }
    else
    {
        header[1] = 127;
        header[2] = (payload_length >> 56) & 0xFF;
        header[3] = (payload_length >> 48) & 0xFF;
        header[4] = (payload_length >> 40) & 0xFF;
        header[5] = (payload_length >> 32) & 0xFF;
        header[6] = (payload_length >> 24) & 0xFF;
        header[7] = (payload_length >> 16) & 0xFF;
        header[8] = (payload_length >> 8)  & 0xFF;
        header[9] = payload_length & 0xFF;

        // Additional bytes for payload length
        header_size += 8;
    }

    //> Section 2: Frame allocation

    size_t frame_size = header_size + payload_length;

    if (f->mask)
    {
        // Set the masking bit
        header[1] |= 0x80;

        // Additional bytes for masking key
        frame_size += 4;
    }

    // Allocate memory for the frame
    unsigned char* frame_data = (unsigned char*)vws.malloc(frame_size);

    // Copy the header to the frame
    memcpy(frame_data, header, header_size);

    //> Section 3: Masking

    if (f->mask)
    {
        // Generate a random masking key

        unsigned char masking_key[4];

        if (RAND_bytes(masking_key, sizeof(masking_key)) != 1)
        {
            vws.error(VE_RT, "RAND_bytes() failed");
            vws.free(frame_data);
            vws_frame_free(f);

            return NULL;
        }

        // Copy the masking key to the frame
        memcpy(frame_data + header_size, masking_key, 4);

        // Apply masking to the payload data
        size_t payload_start = header_size + 4;
        for (size_t i = 0; i < payload_length; i++)
        {
            frame_data[payload_start + i] = f->data[i] ^ masking_key[i % 4];
        }
    }
    else
    {
        // Copy the payload data without masking
        memcpy(frame_data + header_size, f->data, payload_length);
    }

    //> Section 4: Finalizing

    // Free the frame
    vws_frame_free(f);

    // Create the vws_buffer to hold the frame data
    vws_buffer* buffer = vws_buffer_new();

    // Have buffer take ownership of data
    buffer->data = frame_data;
    buffer->size = frame_size;

    vws.success();

    return buffer;
}

// C-WS-2: maximum accepted single-frame payload. A peer-supplied 8-byte length
// beyond this is rejected as malformed (FRAME_ERROR) -- this caps the allocation
// AND guarantees the required-bytes arithmetic below cannot overflow size_t.
// TUNABLE protocol-limit (Mike keeps 64 MiB) -- revisit against the
// broker message-size profile if larger frames are required.
#define VWS_MAX_FRAME_SIZE (64u * 1024u * 1024u)   // 64 MiB


fs_t vws_deserialize(ucstr data, size_t size, vws_frame* f, size_t* consumed)
{
    // Check if the data contains the minimum required frame header bytes
    if (size < 2)
    {
        return FRAME_INCOMPLETE;
    }

    // Read the first byte (FIN bit and opcode)
    f->fin    = (data[0] >> 7) & 0x01;
    f->opcode = data[0] & 0x0F;

    // Read the second byte (mask bit and payload length)
    f->mask = (data[1] >> 7) & 0x01;
    f->size = data[1] & 0x7F;

    // Check if the payload length requires additional bytes
    size_t size_bytes = 0;
    if (f->size == 126)
    {
        size_bytes = 2;
    }
    else if (f->size == 127)
    {
        size_bytes = 8;
    }

    // Check if the data contains complete frame header and payload
    size_t required_bytes = 2 + size_bytes;

    if (size < required_bytes)
    {
        return FRAME_INCOMPLETE;
    }

    // Read the payload
    if (size_bytes > 0)
    {
        f->size = 0;
        for (size_t i = 0; i < size_bytes; i++)
        {
            f->size = (f->size << 8) | data[2 + i];
        }
    }

    // Check if the frame has masking key and payload data
    if (f->mask)
    {
        // C-WS-2: reject an over-cap (or overflow-inducing) peer length as
        // malformed BEFORE any size math or malloc.
        if (f->size > VWS_MAX_FRAME_SIZE)
        {
            return FRAME_ERROR;
        }

        // Check if the data contains the masking key and payload data
        required_bytes += 4 + f->size;

        if (size < required_bytes)
        {
            return FRAME_INCOMPLETE;
        }

        // Store the payload offset
        f->offset = 2 + size_bytes + 4;

        // Allocate the frame data. [F11] A zero-length frame must not call
        // vws.malloc(0): POSIX permits a NULL return, and the V-3 allocator
        // abort would then crash the process on a peer-sent empty frame
        // (SunOS/illumos). Skip the alloc; the xor loop below is a no-op.
        f->data = f->size > 0 ? vws.malloc(f->size) : NULL;

        // Create a temp variable for the masking key
        unsigned char mask[4];
        memcpy(mask, data + 2 + size_bytes, 4);

        // Read the payload data and apply the masking
        for (size_t i = 0; i < f->size; i++)
        {
            f->data[i] = data[f->offset + i] ^ mask[i % 4];
        }
    }
    else
    {
        // C-WS-2: same overflow-safe cap on the unmasked path.
        if (f->size > VWS_MAX_FRAME_SIZE)
        {
            return FRAME_ERROR;
        }

        // Check if the data contains the payload data

        required_bytes += f->size;

        if (size < required_bytes)
        {
            return FRAME_INCOMPLETE;
        }

        // Store the payload offset
        f->offset = 2 + size_bytes;

        // Allocate the frame data. [F11] skip vws.malloc(0) on a zero-length
        // frame (see the masked path) so the V-3 allocator abort cannot crash
        // the process on a peer-sent empty frame.
        if (f->size > 0)
        {
            f->data = vws.malloc(f->size);

            // Copy the payload data
            memcpy(f->data, data + f->offset, f->size);
        }
        else
        {
            f->data = NULL;
        }
    }

    // Update the bytes consumed
    *consumed = required_bytes;

    return FRAME_COMPLETE;
}

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

void process_frame(vws_cnx* c, vws_frame* f)
{
    // Any inbound frame is liveness activity.
    c->last_active = vws_now_ms();

    switch (f->opcode)
    {
        case CLOSE_FRAME:
        {
            // Set closing state
            vws_set_flag(&c->flags, CNX_CLOSING);

            // [F7] Route the close reply through vws_frame_send so a SERVER
            // connection sends it UNMASKED (RFC 6455) -- the generate helpers
            // always build masked frames. [F6] vws_frame_send is NULL-safe on a
            // serialize failure (and frees the frame it is given).
            int16_t code = htons(WS_CLOSE_NORMAL);
            vws_frame_send(c, vws_frame_new((ucstr)&code, sizeof(code),
                                            CLOSE_FRAME));

            vws_frame_free(f);

            break;
        }

        case TEXT_FRAME:
        case BINARY_FRAME:
        case CONTINUATION_FRAME:
        {
            // Bound the in-progress reassembled message. Accumulate
            // the queued frame bytes; if this message's aggregate exceeds the
            // cap, send a 1009 (Message Too Big) close, flag the connection
            // closing, and drop the frame instead of queuing without limit.
            c->msg_bytes += f->size;
            if (c->msg_bytes > c->max_message_size)
            {
                vws_set_flag(&c->flags, CNX_CLOSING);

                // [F7] send the 1009 (Too Big) close UNMASKED for a SERVER via
                // vws_frame_send; [F6] NULL-safe on serialize failure.
                int16_t code = htons(WS_CLOSE_TOO_BIG);
                vws_frame_send(c, vws_frame_new((ucstr)&code, sizeof(code),
                                                CLOSE_FRAME));

                // [F8] The aborted message's already-queued fin=0 partials stay
                // in c->queue. They are not glued onto a later message because
                // ingress/wait now stop once CNX_CLOSING is set (below and in
                // socket_wait_for_frame), so no subsequent completion occurs;
                // the partials are reclaimed when the connection is torn down.
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
            // [F7] Route the PONG through vws_frame_send so a SERVER connection
            // sends it UNMASKED (RFC 6455). [F6] NULL-safe on serialize failure
            // (and it frees the frame it is given).
            vws_frame_send(c, vws_frame_new(f->data, f->size, PONG_FRAME));

            vws_frame_free(f);

            break;
        }

        case PONG_FRAME:
        {
            // Liveness: a PONG answers our proactive PING -> the peer is alive,
            // so clear the outstanding-ping state (last_active bumped above).
            c->ping_outstanding = false;

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

char* generate_websocket_key()
{
    // Generate a random 16-byte value
    unsigned char random_bytes[16];
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1)
    {
        return NULL;
    }

    // Base64-encode the random bytes
    char* encoded_key = vws_base64_encode(random_bytes, sizeof(random_bytes));

    if (encoded_key == NULL)
    {
        return NULL;
    }

    return encoded_key;
}

cstr vws_accept_key(cstr key)
{
    // Concatenate the key and WebSocket GUID
    const char* websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t key_length          = strlen(key);
    size_t guid_length         = strlen(websocket_guid);
    size_t input_length        = key_length + guid_length;
    char* input                = (char*)vws.malloc(input_length + 1);

    strncpy(input, key, key_length);
    strncpy(input + key_length, websocket_guid, guid_length);
    input[input_length] = '\0';

    // Compute the SHA-1 hash of the concatenated value
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)input, input_length, hash);

    // Base64-encode the hash
    char* encoded_hash = vws_base64_encode(hash, sizeof(hash));

    vws.free(input);

    return encoded_hash;
}

int verify_handshake(const char* key, const char* response)
{
    char* hash = vws_accept_key(key);
    int result = strcmp(hash, response);
    vws.free(hash);

    return result == 0;
}

ssize_t socket_wait_for_frame(vws_cnx* c)
{
    // Default success unless error
    vws.success();

    if (vws_cnx_is_connected(c) == false)
    {
        return -1;
    }

    ssize_t n;

    while (true)
    {
        n = vws_socket_read((vws_socket*)c);

        if (n <= 0)
        {
            // Error already set
            return n;
        }

        ssize_t ic = vws_cnx_ingress(c);

        if (ic > 0)
        {
            break;
        }

        // [F1] ingress failed the connection on a malformed frame (-1) -- stop,
        // do not spin re-reading the same bad bytes. [F8] Likewise stop once
        // the connection is CLOSING (e.g. a TOO_BIG abort) so we do not wait
        // for more frames on a dying connection.
        if (ic < 0 || vws_is_flag(&c->flags, CNX_CLOSING))
        {
            return -1;
        }
    }

    return n;
}

ssize_t vws_cnx_ingress(vws_cnx* c)
{
    size_t total_consumed = 0;

    // Process as many frames as possible
    while (true)
    {
        // If there is no more data in socket buffer
        if (c->base.buffer->size == 0)
        {
            break;
        }

        // [F8] Stop ingesting once the connection is CLOSING (a FRAME_ERROR or
        // TOO_BIG abort set it): processing further frames would let a later
        // fin=1 completion glue the aborted message's stale queued partials
        // onto the front of the next message.
        if (vws_is_flag(&c->flags, CNX_CLOSING))
        {
            break;
        }

        // Attempt to parse complete frame
        size_t consumed  = 0;
        vws_buffer* b    = c->base.buffer;
        vws_frame* frame = vws_frame_new(NULL, 0, TEXT_FRAME);

        if (vws.tracelevel >= VT_PROTOCOL)
        {
            vws.trace(VL_INFO, "Receiving frame");
            vws_trace_lock();
            printf("+----------------------------------------------------+\n");
            printf("| Frame Received                                     |\n");
            printf("+----------------------------------------------------+\n");
            vws_dump_websocket_frame(b->data, b->size);
            printf("------------------------------------------------------\n");
            vws_trace_unlock();
        }

        fs_t rc = vws_deserialize(b->data, b->size, frame, &consumed);

        if (rc == FRAME_ERROR)
        {
            // [F1] A malformed frame must fail the connection, NOT be retried:
            // the offending bytes are never drained, and socket_wait_for_frame
            // treats a 0 return as "keep reading", so it would re-parse the same
            // bad frame forever while the socket buffer grows unboundedly. Per
            // RFC 6455 send a 1002 (protocol error) close, flag CLOSING, clear
            // the buffer, and return -1 so callers distinguish ERROR from the
            // INCOMPLETE 0 return below.
            vws.error(VE_WARN, "FRAME_ERROR");
            vws_frame_free(frame);

            vws_set_flag(&c->flags, CNX_CLOSING);

            int16_t code       = htons(WS_CLOSE_PROTOCOL_ERROR);
            vws_buffer* binary =
                vws_serialize(vws_frame_new((ucstr)&code, sizeof(code),
                                            CLOSE_FRAME));

            if (binary != NULL)
            {
                vws_socket_write((vws_socket*)c, binary->data, binary->size);
                vws_buffer_free(binary);
            }

            vws_buffer_clear(c->base.buffer);

            return -1;
        }

        if (rc == FRAME_INCOMPLETE)
        {
            // No complete frame in socket buffer
            vws_frame_free(frame);

            return 0;
        }

        // Update
        total_consumed += consumed;

        // We have a frame. Process it.
        c->process(c, frame);

        // Drain the consumed frame data from buffer
        vws_buffer_drain(c->base.buffer, consumed);
    }

    vws.success();

    return total_consumed;
}

vws_buffer* vws_generate_close_frame_code(int16_t code)
{
    size_t size   = sizeof(int16_t);
    int16_t* data = vws.malloc(size);

    // Convert to network byte order before assignement
    *data        = htons(code);
    vws_frame* f = vws_frame_new((ucstr)data, size, CLOSE_FRAME);

    vws.free(data);

    return vws_serialize(f);
}

vws_buffer* vws_generate_close_frame()
{
    return vws_generate_close_frame_code(WS_CLOSE_NORMAL);
}

vws_buffer* vws_generate_pong_frame(ucstr ping_data, size_t s)
{
    // We create a new frame with the same data as the received ping frame
    vws_frame* f = vws_frame_new(ping_data, s, PONG_FRAME);

    // Serialize the frame and return it
    return vws_serialize(f);
}

vws_buffer* vws_generate_ping_frame()
{
    // A payload-less PING control frame for liveness probing.
    vws_frame* f = vws_frame_new(NULL, 0, PING_FRAME);

    return vws_serialize(f);
}

vws_msg* vws_msg_pop(vws_cnx* c)
{
    if (has_complete_message(c) == false)
    {
        return NULL;
    }

    // Create new message
    vws_msg* m = vws_msg_new();

    // Set to sentinel value to detect first frame
    m->opcode = 100;

    do
    {
        vws_frame* f = sc_queue_del_last(&c->queue);

        // If this is first frame, opcode is sentinel value. We take the opcode
        // from the first frame only
        if (m->opcode == 100)
        {
            m->opcode = f->opcode;
        }

        // Copy frame data into message buffer
        vws_buffer_append(m->data, f->data, f->size);

        // Is this the completion frame?
        bool complete = (f->fin == 1);

        // Free frame
        vws_frame_free(f);

        if (complete)
        {
            break;
        }
    }
    while (true);

    return m;
}

bool has_complete_message(vws_cnx* c)
{
    vws_frame* f;
    sc_queue_foreach (&c->queue, f)
    {
        if (f->fin == 1)
        {
            return true;
        }
    }

    return false;
}

void dump_websocket_header(const ws_header* header)
{
    printf("  fin:      %u\n", header->fin);
    printf("  opcode:   %u\n", header->opcode);
    printf("  mask:     %u (0x%08x)\n", header->mask, header->masking_key);
    // [F11] payload_len is uint64_t; %lu is wrong on 32-bit -- cast to match.
    printf("  payload:  %llu bytes\n", (unsigned long long)header->payload_len);
    printf("\n");
}

void vws_dump_websocket_frame(const uint8_t* frame, size_t size)
{
    if (size < 2)
    {
        printf("Invalid WebSocket frame\n");
        return;
    }

    ws_header header;
    size_t header_size = 2;
    header.fin         = (frame[0] & 0x80) >> 7;
    header.opcode      = frame[0] & 0x0F;
    header.mask        = (frame[1] & 0x80) >> 7;
    header.payload_len = frame[1] & 0x7F;

    if (header.payload_len == 126)
    {
        if (size < 4)
        {
            printf("Invalid WebSocket frame\n");
            return;
        }

        header_size += 2;
        header.payload_len = ((uint64_t)frame[2] << 8) | frame[3];
    }
    else if (header.payload_len == 127)
    {
        if (size < 10)
        {
            printf("Invalid WebSocket frame\n");
            return;
        }

        header_size += 8;
        header.payload_len =
            ((uint64_t)frame[2] << 56) |
            ((uint64_t)frame[3] << 48) |
            ((uint64_t)frame[4] << 40) |
            ((uint64_t)frame[5] << 32) |
            ((uint64_t)frame[6] << 24) |
            ((uint64_t)frame[7] << 16) |
            ((uint64_t)frame[8] << 8)  |
            frame[9];
    }

    if (header.mask)
    {
        if (size < header_size + 4)
        {
            printf("Invalid WebSocket frame\n");
            return;
        }
        header.masking_key =
            ((uint32_t)frame[header_size]     << 24) |
            ((uint32_t)frame[header_size + 1] << 16) |
            ((uint32_t)frame[header_size + 2] << 8)  |
            frame[header_size + 3];
        header_size += 4;
    }
    else
    {
        header.masking_key = 0;
    }

    printf("  header:   %zu bytes\n", header_size);
    dump_websocket_header(&header);

    if (size > header_size)
    {
        for (size_t i = header_size; i < size; ++i)
        {
            printf("%02x ", frame[i]);
            if ((i - header_size + 1) % 16 == 0)
            {
                printf("\n");
            }
        }

        printf("\n");
    }
}
