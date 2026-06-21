# VRTQL WebSockets Library

## Description

VRTQL WebSockets is a robust, performance-oriented WebSocket library written in
C. It provides a simple yet flexible API for building both WebSocket **clients**
and **servers**, and it supports all standard WebSocket features: text and binary
messages, ping/pong and control frames, and built-in OpenSSL (`wss://`) support.

The library has two parts, built from completely different networking designs,
each suited to its use case:

- A **client-side** component designed for single connections that operates
  synchronously (blocking with an optional timeout), feeling like a traditional
  socket API. It uses the operating system's native `poll()` and requires no
  additional libraries.
- An optional **server-side** component designed for many concurrent
  connections that operates asynchronously atop [`libuv`](https://libuv.org/),
  with a worker-thread pool for processing.

On top of the client API there is also an optional **messaging** layer that adds
a structured message format with JSON and MessagePack serialization — a small
step toward AMQP/MQTT-style messaging without the heft.

### Why vws?

- **Portable C** — compiles and runs on Linux, FreeBSD, NetBSD, OpenBSD, OS X,
  Illumos/Solaris and Windows. The CMake build supports cross-compiling from
  Linux/BSD to Windows with MinGW.
- **MIT licensed** — usable in commercial, closed-source applications.
- **Dual client/server** — a synchronous client and an asynchronous,
  multithreaded server, with the server component entirely optional.
- **Optional messaging layer** — JSON and MessagePack over the same connection.
- **Hardened** — bounds and NULL guards on the wire-facing parser and a
  frame-size cap to resist wire-reachable denial-of-service inputs.

## Quick Start

The fastest path to a working client and a working server. Both examples are
complete, compilable programs; the fuller variants appear under
[Usage](#usage).

### A minimal client

This connects, sends a text message, waits for a reply, and disconnects.

```c
// quickstart-client — minimal WebSocket client (subset of client-basic)
#include <vws/websocket.h>
#include <stdio.h>

int main(int argc, const char* argv[])
{
    // Create a connection object
    vws_cnx* cnx = vws_cnx_new();

    // Connect. TLS is used automatically if the "wss" scheme is given.
    if (vws_connect(cnx, "ws://localhost:8181/websocket") == false)
    {
        printf("Failed to connect\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Send a TEXT frame
    vws_frame_send_text(cnx, "Hello, world!");

    // Receive a message (returns NULL on timeout)
    vws_msg* reply = vws_msg_recv(cnx);
    if (reply != NULL)
    {
        printf("Received %zu bytes\n", reply->data->size);
        vws_msg_free(reply);
    }

    vws_disconnect(cnx);
    vws_cnx_free(cnx);

    return 0;
}
```

### A minimal WebSocket echo server

> **Before you begin:** the server component requires `libuv` and is built only
> when `BUILD_SERVER` is enabled (the default). See [Installation](#installation).

This accepts connections and echoes each message back.

```c
// quickstart-server — minimal WebSocket echo server (subset of server-ws)
#include <vws/server.h>

// Runs in the context of a worker thread. Assigned to the derived process_ws
// hook (on_msg_in is the framework's internal dispatcher — do not overwrite it).
void echo(vws_svr* s, vws_cid_t cid, vws_msg* m, void* ctx)
{
    // Echo the message back, preserving its opcode (TEXT/BINARY)
    vws_msg* reply = vws_msg_new();
    reply->opcode  = m->opcode;
    vws_buffer_append(reply->data, m->data->data, m->data->size);

    // send() takes ownership of the reply; we free the incoming message
    s->send(s, cid, reply, NULL);
    vws_msg_free(m);
}

int main(int argc, const char* argv[])
{
    vws_svr* server    = vws_svr_new(10, 0, 0);
    server->process_ws = echo;

    // Run (blocks until the server is stopped)
    vws_svr_run(server, "127.0.0.1", 8181);

    // Stop via the base type, then free and clean up
    vws_tcp_svr_stop((vws_tcp_svr*)server);
    vws_svr_free(server);
    vws_cleanup();

    return 0;
}
```

To see the library exercised end-to-end, build the project and step into the
`src/test` directory: run `./server` to start a simple WebSocket server, then run
`test_websocket`.

## Concepts

Unlike traditional HTTP connections, which are stateless and unidirectional,
WebSocket connections are stateful and bidirectional. A connection is
established through an HTTP handshake (an HTTP Upgrade request), then upgraded to
a WebSocket connection if the server supports it, and remains open until
explicitly closed.

The protocol communicates through data units called **frames**. The protocol
permits a frame payload of up to 2^64 bytes, though the effective limit is
usually smaller due to network or system constraints. In addition, this library
caps a single frame at **64 MiB** (`VWS_MAX_FRAME_SIZE`): a frame whose payload
length exceeds that is rejected as a `FRAME_ERROR`. The cap is a compile-time
tunable defined in `src/websocket.c`.

While frames are the unit on the wire, applications generally work in terms of
**messages** — one or more frames terminated by a frame with the `FIN` bit set.
The deeper frame taxonomy (opcodes, masking, control frames) is covered under
[Advanced Topics](#advanced-topics). For the full reference, see the
[online documentation](https://vrtql.github.io/ws/ws.html).

## Usage

The examples below are each complete, standalone programs. They progress from
the synchronous client API, through the optional messaging layer, to the
asynchronous server API.

### Client API

The client API is built solely on WebSocket constructs — frames, messages and
connections. The connection object (`vws_cnx`) embeds a `vws_socket` as its first
member, which is why socket-level calls such as `vws_socket_set_timeout()` and
`vws_socket_is_connected()` take the connection cast to `(vws_socket*)`.

```c
// client-basic — synchronous WebSocket client
#include <vws/websocket.h>
#include <stdio.h>
#include <assert.h>

int main(int argc, const char* argv[])
{
    // Create connection object
    vws_cnx* cnx = vws_cnx_new();

    // Set connection timeout to 2 seconds (the default is 10). This applies
    // both to connect() and to read operations (i.e. poll()).
    vws_socket_set_timeout((vws_socket*)cnx, 2);

    // Connect. This will automatically use SSL if the "wss" scheme is used.
    cstr uri = "ws://localhost:8181/websocket";
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Check connection state. Should be true here as we just connected.
    assert(vws_socket_is_connected((vws_socket*)cnx) == true);

    // Enable tracing. This dumps frames to the console in human-readable
    // format as they are sent and received.
    vws.tracelevel = VT_PROTOCOL;

    // Send a TEXT frame
    vws_frame_send_text(cnx, "Hello, world!");

    // Receive a websocket message (NULL on timeout)
    vws_msg* reply = vws_msg_recv(cnx);
    if (reply != NULL)
    {
        vws_msg_free(reply);
    }

    // Send a BINARY message
    vws_msg_send_binary(cnx, (ucstr)"Hello, world!", 14);

    // Receive a websocket message (NULL on timeout)
    reply = vws_msg_recv(cnx);
    if (reply != NULL)
    {
        vws_msg_free(reply);
    }

    vws_disconnect(cnx);
    vws_cnx_free(cnx);

    return 0;
}
```

### Messaging API

The messaging API is built on top of the client API. WebSockets provide
real-time bidirectional communication but do not, by themselves, offer the
structure of heavier protocols like AMQP. The messaging layer adds a structured
message (`vrtql_msg`) with two string maps — `routing` (for routing information)
and `headers` (for application use) — plus a payload that can hold text or
binary data. It handles serialization automatically and can auto-detect each
incoming message's format, so JSON and MessagePack can be mixed on the same
connection.

```c
// client-message — structured messaging over a WebSocket connection
#include <vws/message.h>
#include <stdio.h>

int main(int argc, const char* argv[])
{
    // Create connection object
    vws_cnx* cnx = vws_cnx_new();

    // Connect. This will automatically use SSL if the "wss" scheme is used.
    cstr uri = "ws://localhost:8181/websocket";
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Enable tracing
    vws.tracelevel = VT_PROTOCOL;

    // Create a message
    vrtql_msg* request = vrtql_msg_new();
    vrtql_msg_set_routing(request, "key", "value");
    vrtql_msg_set_header(request, "key", "value");
    vrtql_msg_set_content(request, "payload");

    // Send (returns < 0 on error)
    if (vrtql_msg_send(cnx, request) < 0)
    {
        printf("Failed to send: %s\n", vws.e.text);
        vrtql_msg_free(request);
        vws_cnx_free(cnx);
        return 1;
    }

    // Receive (NULL on timeout)
    vrtql_msg* reply = vrtql_msg_recv(cnx);
    if (reply != NULL)
    {
        vrtql_msg_free(reply);
    }

    // Cleanup
    vrtql_msg_free(request);
    vws_disconnect(cnx);
    vws_cnx_free(cnx);

    return 0;
}
```

### Server API

The server API is layered and works the same way across media formats: raw
binary data, WebSocket messages, HTTP requests, and VRTQL messages. There are
only a few steps to create a server: you create a server instance, define a
processing function for incoming data, and send data back to the peer.

**The model.** Every server is created with a `*_svr_new(pool_size, backlog,
queue_size)` call:

- `pool_size` — the number of worker threads in the processing pool.
- `backlog` — the `listen()` connection backlog; `0` selects the default (128).
- `queue_size` — the maximum request/response queue size; `0` selects the
  default (1024).

The three server types are layered on one another: `vws_svr` (WebSocket) embeds
`vws_tcp_svr` (core) as its first member, and `vrtql_msg_svr` (messaging) embeds
`vws_svr`. Base operations such as `vws_tcp_svr_stop()` take a `vws_tcp_svr*`, so
the derived servers are cast with `(vws_tcp_svr*)` when stopped. The
type-specific calls — `vws_svr_run()`, `vrtql_msg_svr_run()`, and so on — take
the derived type directly.

#### Core server (unstructured data)

The core server (`vws_tcp_svr`) deals in unstructured blobs. Its processing
callback (`vws_tcp_svr_process_data`) takes two arguments: the `vws_svr_data`
instance (a blob of data, allocated on the heap), and the worker thread context.

The worker context is built by the optional `worker_ctor`, `worker_ctor_data`
and `worker_dtor` members. `worker_ctor` constructs a per-thread context that is
passed into the processing function as its last argument; `worker_ctor_data` is
user data passed into `worker_ctor`; and `worker_dtor` is called on worker
shutdown to clean the context up. These are ideal for per-thread resources such
as dedicated database connections.

```c
// server-core — echo server over the unstructured (TCP) server
#include <vws/server.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

/* Anything you want allocated per worker thread for your processing. */
typedef struct my_env
{
    void* thingy;
} my_env;

void process(vws_svr_data* req, void* ctx)
{
    vws.trace(VL_INFO, "process (%p)", req);

    // Instance of your my_env for this specific worker thread
    my_env* env = (my_env*)ctx;

    vws_tcp_svr* server = req->server;

    //> Prepare the response: echo the data back

    // Allocate memory for the data to be sent in response
    char* data = (char*)vws.malloc(req->size);

    // Copy the request's data to the response data
    strncpy(data, req->data, req->size);

    // Create response
    vws_svr_data* reply = vws_svr_data_own(server, req->cid, (ucstr)data, req->size);

    // Free request
    vws_svr_data_free(req);

    if (vws.tracelevel >= VT_APPLICATION)
    {
        vws.trace(VL_INFO, "process(cid=%" PRId64 "): %zu bytes",
                  reply->cid.key, reply->size);
    }

    // Send reply. This will wake up the network thread.
    vws_tcp_svr_send(reply);
}

// Allocate context for a worker thread
void* worker_thread_startup(void* data)
{
    my_env* env = (my_env*)malloc(sizeof(my_env));
    env->thingy = malloc(1);

    return env;
}

// Deallocate context for a worker thread
void worker_thread_shutdown(void* data)
{
    my_env* env = (my_env*)data;
    free(env->thingy);
    free(env);
}

int main(int argc, const char* argv[])
{
    // Run server with 10 worker threads, default TCP listen backlog (128) and
    // default work queue size (1024 pending requests).
    vws_tcp_svr* server = vws_tcp_svr_new(10, 0, 0);
    server->on_data_in  = process;
    vws.tracelevel      = VT_THREAD;

    // Worker thread context
    server->worker_ctor      = worker_thread_startup;
    server->worker_ctor_data = server;
    server->worker_dtor      = worker_thread_shutdown;

    // Run (blocks until vws_tcp_svr_stop() is called)
    vws_tcp_svr_run(server, server_host, server_port);

    // Shutdown
    vws_tcp_svr_stop(server);
    vws_tcp_svr_free(server);
    vws_cleanup();

    return 0;
}
```

#### WebSocket server (delta from core)

Writing a WebSocket server is the same pattern with two changes: create it with
`vws_svr_new()`, and set the `process_ws` callback (`vws_svr_process_msg`), which
operates on WebSocket messages (`vws_msg`) rather than raw data. (Set your
handler on `process_ws`, not `on_msg_in` — the latter is the framework's internal
dispatcher, which calls your `process_ws`.)

```c
// server-ws — WebSocket echo server
#include <vws/server.h>
#include <inttypes.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

// Process messages. Runs in the context of a worker thread.
void process(vws_svr* s, vws_cid_t cid, vws_msg* m, void* ctx)
{
    vws.trace(VL_INFO, "process (cid=%" PRId64 ") %p", cid.key, m);

    // Echo back. Note: always set the reply's format to that of the connection.

    // Create reply message
    vws_msg* reply = vws_msg_new();

    // Use the same format/opcode
    reply->opcode = m->opcode;

    // Copy content
    vws_buffer_append(reply->data, m->data->data, m->data->size);

    // Send. We don't free the reply; send() does it for us.
    s->send(s, cid, reply, NULL);

    // Clean up the request
    vws_msg_free(m);
}

int main(int argc, const char* argv[])
{
    // Setup
    vws_svr* server    = vws_svr_new(10, 0, 0);
    server->process_ws = process;

    // Run (blocks until stopped)
    vws_svr_run(server, server_host, server_port);

    // Shutdown — stop takes the base type
    vws_tcp_svr_stop((vws_tcp_svr*)server);
    vws_svr_free(server);
    vws_cleanup();

    return 0;
}
```

#### HTTP handler (delta)

The framework can also serve plain HTTP. If an incoming HTTP request is not a
WebSocket upgrade, the framework passes it to a user-defined `process_http`
callback (`vws_svr_process_http_req`) if one is set. This can run in tandem with
the WebSocket server.

```c
// server-http — pure HTTP handler
#include <vws/server.h>
#include <string.h>
#include <inttypes.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

// Process HTTP requests. Runs in the context of a worker thread.
bool process(vws_svr* s, vws_cid_t cid, vws_http_msg* msg, void* ctx)
{
    vws_tcp_svr* server = (vws_tcp_svr*)s;

    if (vws.tracelevel >= VT_APPLICATION)
    {
        vws.trace(VL_INFO, "process (cid=%" PRId64 ") %p", cid.key, msg);
    }

    cstr response = "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "Hello world";

    // Allocate memory for the data to be sent in response
    char* data = (char*)vws.strdup(response);

    // Create response
    vws_svr_data* reply = vws_svr_data_own(server, cid, (ucstr)data, strlen(data));

    // Send reply. This will wake up the network thread.
    vws_tcp_svr_send(reply);

    return true;
}

int main(int argc, const char* argv[])
{
    // Setup
    vws_svr* server      = vws_svr_new(10, 0, 0);
    server->process_http = process;

    // Run (blocks until stopped)
    vws_svr_run(server, server_host, server_port);

    // Shutdown
    vws_tcp_svr_stop((vws_tcp_svr*)server);
    vws_svr_free(server);
    vws_cleanup();

    return 0;
}
```

#### Message server (delta)

The messaging server works the same way, operating on `vrtql_msg` messages.

```c
// server-message — VRTQL messaging echo server
#include <vws/server.h>
#include <inttypes.h>

cstr server_host = "127.0.0.1";
int  server_port = 8181;

// Process messages. Runs in the context of a worker thread.
void process(vws_svr* s, vws_cid_t cid, vrtql_msg* m, void* ctx)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)s;

    vws.trace(VL_INFO, "process (cid=%" PRId64 ") %p", cid.key, m);

    // Echo back. Note: always set the reply's format to that of the connection.

    // Create reply message
    vrtql_msg* reply = vrtql_msg_new();
    reply->format    = m->format;

    // Copy content
    ucstr  data = m->content->data;
    size_t size = m->content->size;
    vws_buffer_append(reply->content, data, size);

    // Send via the derived type (reply is a vrtql_msg). We don't free the
    // reply; send() does it for us.
    server->send(s, cid, reply, NULL);

    // Clean up the request
    vrtql_msg_free(m);
}

int main(int argc, const char* argv[])
{
    // Setup
    vrtql_msg_svr* server = vrtql_msg_svr_new(10, 0, 0);
    server->process       = process;

    // Run (blocks until stopped)
    vrtql_msg_svr_run(server, server_host, server_port);

    // Shutdown
    vws_tcp_svr_stop((vws_tcp_svr*)server);
    vrtql_msg_svr_free(server);
    vws_cleanup();

    return 0;
}
```

## Error Handling

Error and tracing state live in a single thread-local global, `vws` (of type
`vws_env`). Because it is thread-local, each thread has its own independent copy
— consistent with the rule that each client connection is used only from the
thread that created it.

After a call fails, inspect `vws.e`, a `vws_error_value` with a numeric `code`
and a human-readable `text`:

```c
// vws.e.code is a uint64_t; printing it portably needs <inttypes.h> (PRIu64).
if (vws_connect(cnx, "ws://localhost:8181/websocket") == false)
{
    fprintf(stderr, "connect failed: [%" PRIu64 "] %s\n", vws.e.code, vws.e.text);
    vws_cnx_free(cnx);
    return 1;
}
```

The error codes are the `vws_error_code_t` values: `VE_SUCCESS`, `VE_TIMEOUT`,
`VE_WARN`, `VE_SOCKET`, `VE_SEND`, `VE_RECV`, `VE_SYS`, `VE_RT`, `VE_MEM` and
`VE_FATAL`.

**Return-value conventions.** The send functions (for example
`vws_frame_send_text()`, `vws_msg_send_binary()` and `vrtql_msg_send()`) return
an `ssize_t` and yield `-1` on error. The receive functions (`vws_msg_recv()`,
`vrtql_msg_recv()`) return `NULL` on timeout or error. `vws_connect()` returns a
`bool`.

**Tracing.** Set `vws.tracelevel` to a `vws_tl_t` value to control how much is
logged: `VT_OFF`, `VT_APPLICATION`, `VT_MODULE`, `VT_SERVICE`, `VT_PROTOCOL`,
`VT_THREAD`, `VT_TCPIP`, `VT_LOCK`, `VT_MEMORY` or `VT_ALL`. At `VT_PROTOCOL` and
above, frames are dumped to the console in human-readable form as they are sent
and received. Emit your own trace lines with `vws.trace(level, fmt, ...)`, where
`level` is a `vws_log_level_t` severity: `VL_DEBUG`, `VL_INFO`, `VL_WARN` or
`VL_ERROR`.

## Threading Model

**Client.** The client API is synchronous and thread-affine: maintain each
`vws_cnx` in its own thread and do not share a connection across threads. All
common services (error reporting and tracing) use the thread-local `vws` global,
so each thread carries its own error and trace state.

**Server.** The server runs one main networking thread plus a pool of
`pool_size` worker threads. The networking thread runs the `libuv` loop, handles
all socket I/O, and distributes incoming data to the workers over a synchronized
queue; workers process the data and return replies over a separate queue. The
framework handles all WebSocket protocol serialization between the network and
worker threads, so your processing function only needs to service the message.

Per-worker state is established through `worker_ctor` / `worker_ctor_data` /
`worker_dtor` (see the [core server example](#core-server-unstructured-data)).
Use the per-worker context for resources that should not be shared across
threads — for instance a dedicated database handle per worker.

## TLS / SSL

TLS is enabled automatically when a connection URI uses the `wss://` scheme; it
runs over the library's global OpenSSL context, `vws_ssl_ctx` (an `SSL_CTX*`).
OpenSSL is a build dependency.

This version exposes **no public API** for certificate configuration,
verification policy, or supplying a custom SSL context — `wss://` simply enables
TLS through the built-in context. For Windows builds, OpenSSL must be cross-built
and made available to the toolchain (see
[Cross-Compiling for Windows](#cross-compiling-for-windows)).

## Lifecycle and Teardown

**Client:**

```
vws_cnx_new()  →  vws_connect()  →  … send/recv …  →  vws_disconnect()  →  vws_cnx_free()
```

**Server:**

```
*_svr_new()  →  set callbacks / worker context  →  *_svr_run()   [blocks until stopped]
             →  vws_tcp_svr_stop((vws_tcp_svr*)server)  →  *_svr_free()  →  vws_cleanup()
```

`*_svr_run()` blocks until the server is stopped. Stop a WebSocket or message
server by casting it to the base type:
`vws_tcp_svr_stop((vws_tcp_svr*)server)` — there is no `vws_svr_stop` or
`vrtql_msg_svr_stop`. Call `vws_cleanup()` once at process exit to release
global resources.

## Advanced Topics

### Experimental async reactor

The library additionally includes an experimental, non-blocking,
single-connection asynchronous reactor (`src/async.h`) for client-side use
without `libuv` — a scaled-down, `poll()`-driven slice of the server's loop. It
is currently low-level foundation; its ergonomic public API arrives with a
forthcoming C++ `Channel` wrapper and will be documented then. It is not yet part
of the supported public API.

### Frame taxonomy

On the wire, a message is a sequence of frames. The first frame of a message
carries its type (`TEXT` or `BINARY`); subsequent frames are `CONTINUATION`
frames, allowing a large message to be split into smaller chunks. The final
frame of a message has the `FIN` bit set. Control frames handle protocol-level
interactions: `CLOSE` terminates a connection, `PING` checks liveness, and
`PONG` answers a ping. Text frames carry Unicode text; binary frames carry
arbitrary bytes.

As noted under [Concepts](#concepts), the library rejects any single frame whose
payload exceeds `VWS_MAX_FRAME_SIZE` (64 MiB) as a `FRAME_ERROR`, before
allocating memory for it; the cap is a compile-time `#define` in
`src/websocket.c`.

## Installation

To build the code you need CMake version 3.15 or higher.

### C Library

Build as follows:

```bash
$ git clone https://github.com/vrtql/websockets.git
$ cd websockets
$ cmake .
$ make
$ sudo make install
```

To build without the server-side API (and drop the `libuv` dependency):

```bash
$ cmake . -DBUILD_SERVER=0
```

The build recognizes these options (each passed as `-D<NAME>=<VALUE>` to
`cmake`):

- `BUILD_SERVER` (default `ON`) — build the optional server component. It
  requires **libuv** (`find_package(LibUV REQUIRED)`); install your platform's
  `libuv` development package (for example, `apt-get install libuv1-dev`)
  before building with the server enabled.
- `ASAN` (default `OFF`) — build with AddressSanitizer.
- `VWS_TSAN` (default `OFF`) — build the library and tests with ThreadSanitizer
  for race detection.

### Ruby Gem

The Ruby extension can be built as follows:

```bash
$ git clone https://github.com/vrtql/websockets.git
$ cd src/ruby
$ cmake .
$ make
$ make gem
$ sudo gem install vrtql-ws*.gem
```

Alternately, without using `gem`:

```bash
    cd websockets-ruby/src/ruby/ext/vrtql/ws/
    ruby extconf.rb
    make
    make install
```

The RDoc documentation is located [here](https://vrtql.github.io/ws/ruby/).

### Cross-Compiling for Windows

You must have the requisite MinGW compiler and tools installed on your
system. For Debian/Devuan you would install these as follows:

```bash
$ apt-get install mingw-w64 mingw-w64-tools mingw-w64-common \
                  g++-mingw-w64-x86-64 mingw-w64-x86-64-dev
```

You will need to have OpenSSL for Windows on your system as well. If you don't
have it you can build as follows. First download the version you want to
build. Here we will use `openssl-1.1.1u.tar.gz` as an example. (The OpenSSL
`1.1.1` series has reached end of life; for new builds prefer a current
OpenSSL 3.x release and substitute the version accordingly.) Create the
install directory you intend to put OpenSSL in. For example:

```bash
$ mkdir ~/mingw
```

Build OpenSSL. You want to ensure you set the `--prefix` to the directory you
specified above. This is where OpenSSL will install to.

```bash
$ cd /tmp
$ tar xzvf openssl-1.1.1u.tar.gz
$ cd openssl-1.1.1u
$ ./Configure --cross-compile-prefix=x86_64-w64-mingw32- \
              --prefix=~/mingw shared mingw64 no-tests
$ make
$ make DESTDIR=~/mingw install
```

Now within the `websockets` project. Modify the `CMAKE_FIND_ROOT_PATH` in the
`config/windows-toolchain.cmake` file to point to where you installed
OpenSSL. In this example it would be `~/mingw/openssl` (you might want to use
full path). Then invoke CMake as follows:

```bash
$ cmake -DCMAKE_TOOLCHAIN_FILE=config/windows-toolchain.cmake
```

Then build as normal:

```bash
$ make
```

## Compatibility

| Aspect             | Support                                                              |
|--------------------|---------------------------------------------------------------------|
| Version            | 2.0.0                                                                |
| Operating systems  | Linux, FreeBSD, NetBSD, OpenBSD, OS X, Illumos/Solaris, Windows      |
| Build system       | CMake ≥ 3.15                                                         |
| Language standard  | C11 (uses C11 atomics)                                               |
| Compilers          | GCC, Clang; MinGW-w64 for Windows cross-compilation                  |
| Server dependency  | libuv (only when `BUILD_SERVER=ON`)                                  |
| TLS                | OpenSSL (via the `wss://` scheme)                                    |
| Bindings           | Ruby C extension                                                     |

## Documentation

Full documentation is located [here](https://vrtql.github.io/ws/ws.html). Source
code annotation is located [here](https://vrtql.github.io/ws-code-doc/root/).

## Feature Summary

- Written in C for maximum portability
- Runs on all major operating systems
- OpenSSL support built in
- Thread safe
- Liberal license allowing use in closed-source applications
- Simple, intuitive API.
- Handles complicated tasks like socket-upgrade on connection, PING requests,
  proper shutdown, frame formatting/masking, message sending and receiving.
- Well tested with extensive unit tests, including a ThreadSanitizer race suite
- Hardened against wire-reachable denial-of-service inputs (frame-size cap,
  parser bounds and NULL guards)
- Well documented
- Provides a high-level API for messaging applications supporting both JSON and
  MessagePack serialization formats within the same connection.
- Includes native Ruby C extension with RDoc documentation.

## Contributing

We welcome contributions! Please fork this repository, make your changes, and
submit a pull request. We'll review your changes and merge them if they're a
good fit for the project.

## License

This project and all third party code used by it is licensed under the MIT
License. See the [LICENSE](LICENSE) file for details.
