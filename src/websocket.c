#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(__windows__)
#define _WIN32_WINNT 0x0601  // Windows 7 or later
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/rand.h>

#include "websocket.h"

#define MAX_BUFFER_SIZE 1024

// The fs_t enum describes the state of a WebSocket frame being processed.
typedef enum
{
    // Not all of the frame data has been received or processed.
    FRAME_INCOMPLETE,

    // All of the frame data has been received and processed.
    FRAME_COMPLETE,

    // There was an error in processing the frame data.
    FRAME_ERROR
} fs_t;

// The cnx_flags_t enum defines the various states that a WebSocket client
// connection can be in.
typedef enum
{
    // The connection with the client is closed.
    CNX_CLOSED    = 0,

    // The connection with the client is established and open.
    CNX_CONNECTED = (1 << 1),

    // The connection with the client is in the process of being closed.
    CNX_CLOSING   = (1 << 2),

    // The connection with the client is in the initial SSL handshake phase.
    CNX_SSL_INIT  = (1 << 3),

    // The connection is in server mode
    CNX_SERVER    = (1 << 4)
} cnx_flags_t;

// The frame_type_t enum represents the different types of WebSocket frames that
// can be received or sent.
typedef enum
{
    // A continuation frame, part of a data message split across multiple frames.
    CONTINUATION_FRAME = 0x0,

    // A text data frame.
    TEXT_FRAME         = 0x1,

    // A binary data frame.
    BINARY_FRAME       = 0x2,

    // A close control frame.
    CLOSE_FRAME        = 0x8,

    // A ping control frame.
    PING_FRAME         = 0x9,

    // A pong control frame, in response to a ping.
    PONG_FRAME         = 0xA
} frame_type_t;

typedef enum
{
    // Normal Closure. This means that the purpose for which the connection was
    // established has been fulfilled.
    WS_CLOSE_NORMAL = 1000,

    // Going Away. A server is going down, a browser has navigated away from a
    // page, etc.
    WS_CLOSE_GOING_AWAY = 1001,

    // Protocol Error. The endpoint is terminating the connection due to a
    // protocol error.
    WS_CLOSE_PROTOCOL_ERROR = 1002,

    // Unsupported Data. The connection is being terminated because an
    // endpoint received a type of data it cannot accept.
    WS_CLOSE_UNSUPPORTED = 1003,

    // Reserved. The specific meaning might be defined in the future.
    WS_CLOSE_RESERVED = 1004,

    // No Status Received. Reserved value. The connection is closed with
    // no status code.
    WS_CLOSE_NO_STATUS = 1005,

    // Abnormal Closure. Reserved value. The connection is closed with
    // no status code.
    WS_CLOSE_ABNORMAL = 1006,

    // Invalid frame payload data. The endpoint is terminating the connection
    // because a message was received that contains inconsistent data.
    WS_CLOSE_INVALID_PAYLOAD = 1007,

    // Policy Violation. The endpoint is terminating the connection because
    // it received a message that violates its policy.
    WS_CLOSE_POLICY_VIOLATION = 1008,

    // Message Too Big. The endpoint is terminating the connection because a
    // data frame was received that is too large.
    WS_CLOSE_TOO_BIG = 1009,

    // Missing Extension. The client is terminating the connection because it
    // wanted the server to negotiate one or more extension, but the server
    // didn't.
    WS_CLOSE_MISSING_EXTENSION = 1010,

    // Internal Error. The server is terminating the connection because it
    // encountered an unexpected condition that prevented it from fulfilling the
    // request.
    WS_CLOSE_INTERNAL_ERROR = 1011,

    // Service Restart. The server is terminating the connection because
    // it is restarting.
    WS_CLOSE_SERVICE_RESTART = 1012,

    // Try Again Later. The server is terminating the connection due to a
    // temporary condition, e.g. it is overloaded and is casting off some of its
    // clients.
    WS_CLOSE_TRY_AGAIN_LATER = 1013,

    // Bad Gateway. The server was acting as a gateway or proxy and received an
    // invalid response from the upstream server. This is similar to 502 HTTP
    // Status Code.
    WS_CLOSE_BAD_GATEWAY = 1014,

    // TLS handshake. Reserved value. The connection is closed due to a failure
    // to perform a TLS handshake.
    WS_CLOSE_TLS_HANDSHAKE = 1015
} ws_close_code_t;

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

// Creates and initializes a new vws_cnx object
static vws_cnx* cnx_new();

// Connects to a host at a specific port and returns the connection status
static int connect_to_host(const char* host, const char* port);

// Generates a new, random WebSocket key for handshake process
static char* generate_websocket_key();

// Extracts the WebSocket accept key from a server's handshake response
static char* extract_websocket_accept_key(const char* response);

// Verifies the server's handshake response using the client's original key
static int verify_handshake(const char* key, const char* response);

// Serializes a vws_frame into a buffer that can be sent over the network
static vrtql_buffer* serialize(vws_frame* f);

// Deserializes raw network data into a vws_frame, updating consumed with the
// number of bytes processed
static fs_t deserialize(ucstr data, size_t size, vws_frame* f, size_t* consumed);

// Processes a single frame from a WebSocket connection
static void process_frame(vws_cnx* c, vws_frame* frame);

// Generates a close frame for a WebSocket connection
static vrtql_buffer* generate_close_frame();

// Generates a pong frame in response to a received ping frame
static vrtql_buffer* generate_pong_frame(ucstr ping_data, size_t s);

// Reads data from a WebSocket connection into a buffer
static ssize_t socket_read(vws_cnx* c, ucstr data, size_t size);

// Writes data from a buffer to a WebSocket connection
static ssize_t socket_write(vws_cnx* c, ucstr data, size_t size);

// Reads data from a WebSocket connection and deserializes it into a frame
static ssize_t socket_ingress(vws_cnx* c);

// Waits for a complete frame to be available from a WebSocket connection
static ssize_t socket_wait_for_frame(vws_cnx* c);

// Closes a WebSocket connection
static void socket_close(vws_cnx* c);

// Sets a socket to non-blocking mode
static bool socket_set_nonblocking(int sockfd);

// Sets a timeout on a socket read/write operations
static bool socket_set_timeout(int fd, int sec);

// Frees memory associated with a vws_frame object
static void frame_free(vws_frame* frame);

// Checks if a complete message is available in the connection's message queue
static bool has_complete_message(vws_cnx* c);

// Removes and returns the first complete message from the connection's message
// queue
static vws_msg* pop_message(vws_cnx* c);

// Structure representing a WebSocket header
typedef struct
{
    uint8_t fin;          // Indicates the final frame of a message
    uint8_t opcode;       // Identifies the frame type
    uint8_t mask;         // Indicates if the frame payload is masked
    uint64_t payload_len; // Length of the payload data
    uint32_t masking_key; // Key used for payload data masking
} ws_header;

// Dumps the contents of a WebSocket header for debugging purposes
static void dump_websocket_header(const ws_header* header);

// Dumps the contents of a WebSocket frame for debugging purposes
static void dump_websocket_frame(const unsigned char* data, size_t size);

//------------------------------------------------------------------------------
//> Connection API
//------------------------------------------------------------------------------

vws_cnx* vws_cnx_new()
{
    vws_cnx* c = (vws_cnx*)vrtql.malloc(sizeof(vws_cnx));

    memset(c, 0, sizeof(vws_cnx));
    c->flags   = CNX_CLOSED;
    c->url     = vrtql_url_new();
    c->key     = generate_websocket_key();
    c->buffer  = vrtql_buffer_new();
    c->ssl     = NULL;
    c->ssl_ctx = NULL;
    c->timeout = 10000;
    c->trace   = 0;

    sc_queue_init(&c->queue);

    return c;
}

void vws_cnx_free(vws_cnx* c)
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
    vrtql_url_free(c->url);

    // Free websocket key
    free(c->key);

    // Free receive buffer
    vrtql_buffer_free(c->buffer);

    if (c->sockfd >= 0)
    {
        close(c->sockfd);
    }

    // Free connection
    free(c);
}

bool vws_cnx_set_timeout(vws_cnx* c, int sec)
{
    return socket_set_timeout(c->sockfd, sec);
}

void vws_cnx_set_server_mode(vws_cnx* c)
{
    vrtql_set_flag(&c->flags, CNX_SERVER);
}

bool vws_connect(vws_cnx* c, cstr uri)
{
    if (c == NULL)
    {
        // Return early if failed to create a connection.
        vrtql.error(VE_RT, "Invalid connection pointer()");
        return false;
    }

    vrtql_url_free(c->url);
    c->url = vrtql_url_parse(uri);

    if (c->url.host == NULL)
    {
        vrtql.error(VE_MEM, "Invalid or missing host");
        return false;
    }

    // Connect to the server
    cstr default_port = strcmp(c->url.scheme, "wss") == 0 ? "443" : "80";
    cstr port = c->url.port != NULL ? c->url.port : default_port;

    if (strcmp(c->url.scheme, "wss") == 0)
    {
        if (vrtql_is_flag(&vrtql.state, CNX_SSL_INIT) == false)
        {
            SSL_library_init();
            RAND_poll();
            SSL_load_error_strings();

            c->ssl_ctx = SSL_CTX_new(TLS_method());

            if (c->ssl_ctx == NULL)
            {
                vrtql.error(VE_SYS, "Failed to create new SSL context");
                return false;
            }

            SSL_CTX_set_options(c->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

            c->ssl = SSL_new(c->ssl_ctx);

            if (c->ssl == NULL)
            {
                vrtql.error(VE_SYS, "Failed to create new SSL object");
                socket_close(c);
                return false;
            }

            vrtql_set_flag(&vrtql.state, CNX_SSL_INIT);
        }
    }

    c->sockfd = connect_to_host(c->url.host, port);

    if (c->sockfd < 0)
    {
        vrtql.error(VE_SYS, "Connection failed");
        socket_close(c);
        return false;
    }

    c->flags = CNX_CONNECTED;

    // Set default timeout

    if (socket_set_timeout(c->sockfd, c->timeout/1000) == false)
    {
        // Error already set
        socket_close(c);
        return false;
    }

    if (c->ssl != NULL)
    {
        SSL_set_fd(c->ssl, c->sockfd);

        if (SSL_connect(c->ssl) <= 0)
        {
            vrtql.error(VE_SYS, "SSL connection failed");
            socket_close(c);
            return false;
        }
    }

    // Send the WebSocket handshake request
    const char* websocket_handshake =
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
    snprintf( req,
              sizeof(req),
              websocket_handshake,
              c->url.path,
              c->url.host,
              uri,
              c->key);

    ssize_t n;

    n = send(c->sockfd, req, strlen(req), 0);

    if (n == -1)
    {
#if defined(__windows__)
        if (WSAGetLastError() == WSAETIMEDOUT)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
        {
            vrtql.error(VE_TIMEOUT, "connection timed out");
        }
        else
        {
            vrtql.error(VE_SYS, "send() failed");
        }

        socket_close(c);
        return false;
    }

    // Receive the WebSocket handshake response
    char buffer[MAX_BUFFER_SIZE] = {0};

    n = recv(c->sockfd, buffer, sizeof(buffer) - 1, 0);

    if (n == -1)
    {
#if defined(__windows__)
        if (WSAGetLastError() == WSAETIMEDOUT)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
        {
            vrtql.error(VE_TIMEOUT, "connection timed out");
        }
        else
        {
            vrtql.error(VE_SYS, "recv() failed");
        }

        socket_close(c);
        return false;
    }

    cstr accept_key = extract_websocket_accept_key(buffer);

    if (accept_key == NULL)
    {
        vrtql.error(VE_SYS, "connect failed: no accept key returned");

        socket_close(c);
        return false;
    }

    if (verify_handshake(c->key, accept_key) == false)
    {
        vrtql.error(VE_RT, "Handshake verification failed");
        free(accept_key);
        socket_close(c);
        return false;
    }

    free(accept_key);

    // Go into non-blocking mode as we are using poll() for socket_read() and
    // socket_write().
    if (socket_set_nonblocking(c->sockfd) == false)
    {
        // Error already set
        socket_close(c);
        return false;
    }

    vrtql.success();

    return true;
}

bool vws_cnx_is_connected(vws_cnx* c)
{
    if (c == NULL)
    {
        return false;
    }

    return c->sockfd > 0;
}

void vws_disconnect(vws_cnx* c)
{
    if (vws_cnx_is_connected(c) == false)
    {
        return;
    }

    c->flags = CNX_CLOSED;

    vrtql_buffer* buffer = generate_close_frame();

    for (size_t i = 0; i < buffer->size;)
    {
        int n = socket_write(c, buffer->data + i, buffer->size - i);

        if (n < 0)
        {
            break;
        }

        i += n;
    }

    vrtql_buffer_free(buffer);
    socket_close(c);

    vrtql.success();
}

//------------------------------------------------------------------------------
//> Messaging API
//------------------------------------------------------------------------------

int vws_send_text(vws_cnx* c, cstr data)
{
    return vws_send_data(c, (ucstr)data, strlen(data), 0x1);
}

int vws_send_binary(vws_cnx* c, ucstr data, size_t size)
{
    return vws_send_data(c, data, size, 0x2);
}

ssize_t vws_send_data(vws_cnx* c, ucstr data, size_t size, int oc)
{

    return vws_send_frame(c, vws_frame_new(data, size, oc));
}

ssize_t vws_send_frame(vws_cnx* c, vws_frame* frame)
{
    if (vws_cnx_is_connected(c) == false)
    {
        vrtql.error(VE_RT, "Not connected");
        return -1;
    }

    vrtql_buffer* binary = serialize(frame);

    if (c->trace)
    {
        printf("\n\n");
        printf("+----------------------------------------------------+\n");
        printf("| Frame Sent                                         |\n");
        printf("+----------------------------------------------------+\n");

        dump_websocket_frame(binary->data, binary->size);
        printf("------------------------------------------------------\n");
    }

    ssize_t n = 0;

    if (binary->data != NULL)
    {
        n = socket_write(c, binary->data, binary->size);
        vrtql_buffer_free(binary);
    }

    vrtql.success();

    return n;
}

//------------------------------------------------------------------------------
//> Message API
//------------------------------------------------------------------------------

vws_msg* vws_msg_new()
{
    vws_msg* m = vrtql.malloc(sizeof(vws_msg));
    m->opcode  = 0;
    m->data    = vrtql_buffer_new();

    return m;
}

void vws_msg_free(vws_msg* m)
{
    if (m != NULL)
    {
        vrtql_buffer_free(m->data);
        free(m);
    }
}

vws_msg* vws_recv_msg(vws_cnx* c)
{
    // Default success unless error
    vrtql.success();

    if (vws_cnx_is_connected(c) == false)
    {
        vrtql.error(VE_RT, "Not connected");
        return NULL;
    }

    while (true)
    {
        vws_msg* msg = pop_message(c);

        if (msg != NULL)
        {
            return msg;
        }

        if (socket_wait_for_frame(c) == false)
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
    vws_frame* f = vrtql.malloc(sizeof(vws_frame));

    // We must make our own copy of the data for deterministic memory management

    f->fin    = 1;
    f->opcode = oc;
    f->mask   = 1;
    f->offset = 0;
    f->size   = s;
    f->data   = NULL;

    if (f->size > 0)
    {
        f->data = vrtql.malloc(f->size);
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
            free(f->data);
            f->data = NULL;
        }

        f->size = 0;

        free(f);
    }
}

vws_frame* vws_recv_frame(vws_cnx* c)
{
    // Default success unless error
    vrtql.success();

    if (vws_cnx_is_connected(c) == false)
    {
        vrtql.error(VE_RT, "Not connected");
        return NULL;
    }

    while (true)
    {
        if (sc_queue_size(&c->queue) > 0)
        {
            return sc_queue_del_last(&c->queue);
        }

        if (socket_wait_for_frame(c) == false)
        {
            break;
        }
    }

    return NULL;
}

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

vrtql_buffer* serialize(vws_frame* f)
{
    if (f == NULL)
    {
        vrtql.error(VE_RT, "empty frame");

        return NULL;
    }

    //> Section 1: Size calculation

    // Calculate the frame size
    size_t payload_length = f->size;

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

    // Set the masking bit
    header[1] |= 0x80;

    //> Section 2: Frame allocation

    size_t frame_size = header_size + payload_length;

    if (f->mask)
    {
        // Additional bytes for masking key
        frame_size += 4;
    }

    // Allocate memory for the frame
    unsigned char* frame_data = (unsigned char*)vrtql.malloc(frame_size);

    // Copy the header to the frame
    memcpy(frame_data, header, header_size);

    //> Section 3: Masking

    if (f->mask)
    {
        // Generate a random masking key

        unsigned char masking_key[4];

        if (RAND_bytes(masking_key, sizeof(masking_key)) != 1)
        {
            vrtql.error(VE_RT, "RAND_bytes() failed");
            free(frame_data);
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

    //> Section 4: Finalizing

    // Free the frame
    vws_frame_free(f);

    // Create the vrtql_buffer to hold the frame data
    vrtql_buffer* buffer = vrtql_buffer_new();

    if (buffer == NULL)
    {
        free(frame_data);
        vrtql.error(VE_MEM, "vrtql_buffer_new() failed");
        return NULL;
    }

    vrtql.success();

    buffer->data = frame_data;
    buffer->size = frame_size;

    return buffer;
}

void process_frame(vws_cnx* c, vws_frame* f)
{
    switch (f->opcode)
    {
        case CLOSE_FRAME:
        {
            // Set closing state
            vrtql_set_flag(&c->flags, CNX_CLOSING);

            // Build the response frame
            vrtql_buffer* buffer = generate_close_frame();

            // Send the response frame
            socket_write(c, buffer->data, buffer->size);

            // Clean up
            vrtql_buffer_free(buffer);
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
            vrtql_buffer* buffer = generate_pong_frame(f->data, f->size);

            // Send the PONG response
            socket_write(c, buffer->data, buffer->size);

            // Clean up
            vrtql_buffer_free(buffer);
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

int connect_to_host(const char* host, const char* port)
{
    int sockfd = -1;

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    // Resolve the host
    struct addrinfo hints, *res, *res0;
    int error;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = PF_UNSPEC; // Accept any family (IPv4 or IPv6)
    hints.ai_socktype = SOCK_STREAM;

    error = getaddrinfo(host, port, &hints, &res0);

    if (error)
    {
        if (vrtql.trace)
        {
            cstr msg = gai_strerror(error);
            vrtql_trace(VL_ERROR, "getaddrinfo failed: %s: %s", host, msg);
        }

        vrtql.error(VE_SYS, "getaddrinfo() failed");

        return -1;
    }

    for (res = res0; res; res = res->ai_next)
    {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        if (sockfd == -1)
        {
            vrtql.error(VE_SYS, "Failed to create socket");
            continue;
        }

        if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1)
        {
            close(sockfd);
            sockfd = -1;

            vrtql.error(VE_SYS, "Failed to connect");
            continue;
        }

        break; // If we get here, we must have connected successfully
    }

    freeaddrinfo(res0); // Free the addrinfo structure for this host

#elif defined(__windows__)

    // Windows specific implementation
    // Please refer to Windows Socket programming guide

    WSADATA wsaData;
    struct addrinfo *result = NULL, *ptr = NULL, hints;
    sockfd = INVALID_SOCKET;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
    {
        vrtql.error(VE_SYS, "WSAStartup failed");
        return -1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve the server address and port
    if (getaddrinfo(host, port, &hints, &result) != 0)
    {
        vrtql.error(VE_SYS, "getaddrinfo failed\n");
        return -1;
    }

    // Attempt to connect to an address until one succeeds
    for (ptr = result; ptr != NULL; ptr =ptr->ai_next)
    {
        // Create a SOCKET for connecting to server
        sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);

        if (sockfd == INVALID_SOCKET)
        {
            char buf[256];
            int e = WSAGetLastError();
            snprintf(buf, sizeof(buf), "socket failed with error: %ld", e);
            vrtql.error(VE_RT, buf);

            WSACleanup();
            return -1;
        }

        // Connect to server.
        if (connect(sockfd, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR)
        {
            closesocket(sockfd);
            sockfd = INVALID_SOCKET;
            continue;
        }

        break;
    }

    freeaddrinfo(result);

    if (sockfd == INVALID_SOCKET)
    {
        vrtql.error(VE_SYS, "Unable to connect to host");
        WSACleanup();
        return -1;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return sockfd;
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
    char* encoded_key = vrtql_base64_encode(random_bytes, sizeof(random_bytes));

    if (encoded_key == NULL)
    {
        return NULL;
    }

    return encoded_key;
}

char* extract_websocket_accept_key(const char* response)
{
    const char* key_prefix   = "Sec-WebSocket-Accept: ";
    size_t key_prefix_length = strlen(key_prefix);
    const char* key_start    = strstr(response, key_prefix);

    if (key_start == NULL)
    {
        return NULL;
    }

    key_start += key_prefix_length;

    const char* key_end = strchr(key_start, '\r');

    if (key_end == NULL)
    {
        return NULL;
    }

    size_t key_length = key_end - key_start;
    char* accept_key  = (char*)vrtql.malloc(key_length + 1);

    strncpy(accept_key, key_start, key_length);
    accept_key[key_length] = '\0';

    return accept_key;
}

int verify_handshake(const char* key, const char* response)
{
    // Concatenate the key and WebSocket GUID
    const char* websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t key_length          = strlen(key);
    size_t guid_length         = strlen(websocket_guid);
    size_t input_length        = key_length + guid_length;
    char* input                = (char*)vrtql.malloc(input_length + 1);

    strncpy(input, key, key_length);
    strncpy(input + key_length, websocket_guid, guid_length);
    input[input_length] = '\0';

    // Compute the SHA-1 hash of the concatenated value
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)input, input_length, hash);

    // Base64-encode the hash
    char* encoded_hash = vrtql_base64_encode(hash, sizeof(hash));

    // Compare the encoded hash with the expected response
    int result = strcmp(encoded_hash, response);

    free(input);
    free(encoded_hash);

    return result == 0;
}

fs_t deserialize(ucstr data, size_t size, vws_frame* f, size_t* consumed)
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
        // Check if the data contains the masking key and payload data
        required_bytes += 4 + f->size;

        if (size < required_bytes)
        {
            return FRAME_INCOMPLETE;
        }

        // Read the masking key
        memcpy(f->data - 4, data + 2 + size_bytes, 4);

        // Read the payload data and apply the masking
        for (size_t i = 0; i < f->size; i++)
        {
            f->data[i] = data[2 + size_bytes + 4 + i] ^ f->data[i % 4];
        }
    }
    else
    {
        // Check if the data contains the payload data

        required_bytes += f->size;

        if (size < required_bytes)
        {
            return FRAME_INCOMPLETE;
        }

        // Store the payload offset
        f->offset = 2 + size_bytes;

        // Allocate the frame data
        f->data = vrtql.malloc(f->size);

        // Copy the payload data
        memcpy(f->data, data + f->offset, f->size);
    }

    // Update the bytes consumed
    *consumed = required_bytes;

    return FRAME_COMPLETE;
}

bool socket_set_timeout(int fd, int sec)
{
#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    if (fd < 0)
    {
        vrtql.error(VE_RT, "Not connected");
        return false;
    }

    struct timeval tm;
    tm.tv_sec  = sec;
    tm.tv_usec = 0;

    // Set the send timeout
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

#elif defined(__windows__)

    if (fd == INVALID_SOCKET)
    {
        vrtql.error(VE_RT, "Not connected");
        return false;
    }

    // Convert from sec to ms for Windows
    DWORD tm = sec * 1000;

    // Set the send timeout
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (cstr)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

    // Set the receive timeout
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (cstr)&tm, sizeof(tm)) < 0)
    {
        vrtql.error(VE_SYS, "setsockopt failed");

        return false;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return true;
}

bool socket_set_nonblocking(int sockfd)
{
#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    int flags = fcntl(sockfd, F_GETFL, 0);

    if (flags == -1)
    {
        vrtql.error(VE_SYS, "fcntl(sockfd, F_GETFL, 0) failed");

        return false;
    }

    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        vrtql.error(VE_SYS, "fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) failed");

        return false;
    }

#elif defined(__windows__)

    unsigned long arg = 1;
    if (ioctlsocket(sockfd, FIONBIO, &arg) == SOCKET_ERROR)
    {
        vrtql.error(VE_SYS, "ioctlsocket(sockfd, FIONBIO, &arg)");

        return false;
    }

#else
    #error Platform not supported
#endif

    vrtql.success();

    return true;
}

ssize_t socket_read(vws_cnx* c, ucstr data, size_t size)
{
    // Default success unless error
    vrtql.success();

    // Validate input parameters
    if (c == NULL || data == NULL || size == 0)
    {
        vrtql.error(VE_WARN, "Invalid parameters");
        return -1;
    }

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    struct pollfd fds;
    fds.fd     = c->sockfd;
    fds.events = POLLIN;

    int poll_result = poll(&fds, 1, c->timeout);

#elif defined(__windows__)

    WSAPOLLFD fds;
    fds.fd     = c->sockfd;
    fds.events = POLLIN;

    int poll_result = WSAPoll(&fds, 1, c->timeout);

#else
    #error Platform not supported
#endif

    if (poll_result == -1)
    {
        vrtql.error(VE_RT, "poll() failed");
        return -1;
    }

    if (poll_result == 0)
    {
        vrtql.error(VE_WARN, "timeout");
        return 0;
    }

    if (fds.revents & POLLIN)
    {
        if (c->ssl != NULL)
        {
            // SSL socket is readable, perform SSL_read() operation
            return SSL_read(c->ssl, data, size);
        }
        else
        {
            // Non-SSL socket is readable, perform recv() operation
            ssize_t n = recv(c->sockfd, data, size, 0);
            if (n == -1)
            {
                vrtql.error(VE_WARN, "recv() failed");
            }

            return n;
        }
    }

    vrtql.error(VE_WARN, "Unexpected event in poll()");

    return -1;
}

ssize_t socket_write(vws_cnx* c, const ucstr data, size_t size)
{
    // Default success unless error
    vrtql.success();

    if (vws_cnx_is_connected(c) == false)
    {
        vrtql.error(VE_RT, "Not connected");
        return -1;
    }

    // Validate input parameters
    if (c == NULL || data == NULL || size == 0)
    {
        vrtql.error(VE_WARN, "Invalid parameters");
        return -1;
    }

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)

    struct pollfd fds;
    fds.fd     = c->sockfd;
    fds.events = POLLOUT;

    int poll_result = poll(&fds, 1, c->timeout);

#elif defined(__windows__)

    WSAPOLLFD fds;
    fds.fd     = c->sockfd;
    fds.events = POLLOUT;

    int poll_result = WSAPoll(&fds, 1, c->timeout);

#else
    #error Platform not supported
#endif

    if (poll_result == -1)
    {
        vrtql.error(VE_SYS, "poll() failed");
        return -1;
    }

    if (poll_result == 0)
    {
        vrtql.error(VE_TIMEOUT, "timeout");
        return 0;
    }

    if (fds.revents & POLLOUT)
    {
        if (c->ssl != NULL)
        {
            // SSL socket is writable, perform SSL_write() operation
            return SSL_write(c->ssl, data, size);
        }
        else
        {
            // Non-SSL socket is writable, perform send() operation
            ssize_t bytes_sent = send(c->sockfd, data, size, 0);
            if (bytes_sent == -1)
            {
                vrtql.error(VE_SYS, "send() failed");
            }

            return bytes_sent;
        }
    }

    vrtql.error(VE_SYS, "Unexpected event in poll()");

    return -1;
}

ssize_t socket_wait_for_frame(vws_cnx* c)
{
    // Default success unless error
    vrtql.success();

    if (vws_cnx_is_connected(c) == false)
    {
        vrtql.error(VE_RT, "Not connected");
        return -1;
    }

    ssize_t n;
    unsigned char buf[1024];

    while (true)
    {
        memset(buf, 0, 1024);
        n = socket_read(c, buf, 1023);

        if (n < 0)
        {
            vrtql.error(VE_RT, "Connection closed");
            socket_close(c);
            return n;
        }

        if (n == 0)
        {
            return 0;
        }

        vrtql_buffer_append(c->buffer, &buf[0], n);

        if (socket_ingress(c) > 0)
        {
            break;
        }
    }

    return n;
}

ssize_t socket_ingress(vws_cnx* c)
{
    size_t total_consumed = 0;

    while (true)
    {
        // If there is no more data in socket buffer
        if (c->buffer->size == 0)
        {
            break;
        }

        // Attempt to parse complete frame
        size_t consumed  = 0;
        vrtql_buffer* b  = c->buffer;
        vws_frame* frame = vws_frame_new(NULL, 0, TEXT_FRAME);

        if (c->trace)
        {
            printf("\n+----------------------------------------------------+\n");
            printf("| Frame Received                                     |\n");
            printf("+----------------------------------------------------+\n");
            dump_websocket_frame(b->data, b->size);
            printf("------------------------------------------------------\n");
        }

        fs_t rc = deserialize(b->data, b->size, frame, &consumed);

        if (rc == FRAME_ERROR)
        {
            vrtql.error(VE_WARN, "FRAME_ERROR");
            vws_frame_free(frame);

            return 0;
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
        process_frame(c, frame);

        // Drain the consumed frame data from buffer
        vrtql_buffer_drain(c->buffer, consumed);
    }

    vrtql.success();

    return total_consumed;
}

void socket_close(vws_cnx* c)
{
    if (c->ssl != NULL)
    {
        int rc = SSL_shutdown(c->ssl);

        if (rc == 0)
        {
            // the shutdown is not yet finished
            rc = SSL_shutdown(c->ssl);
        }

        if (rc != 1)
        {
            vrtql.error(VE_WARN, "SSL_shutdown failed");
        }

        SSL_free(c->ssl);
        c->ssl = NULL;
    }

    if (c->ssl_ctx != NULL)
    {
        SSL_CTX_free(c->ssl_ctx);
        c->ssl_ctx = NULL;
    }

    if (c->sockfd > 0)
    {
#if defined(__windows__)
        if (closesocket(c->sockfd) == SOCKET_ERROR)
#else
        if (close(c->sockfd) == -1)
#endif
        {
            vrtql.error(VE_SYS, "Socket close failed");
        }

#if defined(__windows__)
        WSACleanup();
#endif

        c->sockfd = -1;
    }
}

vrtql_buffer* generate_close_frame()
{
    size_t size   = sizeof(int16_t);
    int16_t* data = vrtql.malloc(size);

    // Convert to network byte order before assignement
    *data        = htons(WS_CLOSE_NORMAL);
    vws_frame* f = vws_frame_new((ucstr)data, size, CLOSE_FRAME);

    free(data);

    return serialize(f);
}

vrtql_buffer* generate_pong_frame(ucstr ping_data, size_t s)
{
    // We create a new frame with the same data as the received ping frame
    vws_frame* f = vws_frame_new(ping_data, s, PONG_FRAME);

    // Serialize the frame and return it
    return serialize(f);
}

vws_msg* pop_message(vws_cnx* c)
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
        vrtql_buffer_append(m->data, f->data, f->size);

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
    printf("  payload:  %lu bytes\n", header->payload_len);
    printf("\n");
}

void dump_websocket_frame(const uint8_t* frame, size_t size)
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