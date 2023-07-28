# VRTQL WebSockets Library

## Description

This is a robust and performance-oriented WebSocket library written in C. It
provides a simple yet flexible API for building WebSocket clients and
servers. It supports all standard WebSocket features including text and binary
messages, ping/pong frames, control frames and includes built-in OpenSSL
support.

The motivation behind the project is to have a portable WebSockets client
library under a permissive license (MIT) which feels like a traditional socket
API (blocking with optional timeout) which can also provide a foundation for
some additional messaging features similar to (but lighter weight than) AMQP and
MQTT.

The code compiles and runs on Linux, FreeBSD, NetBSD, OpenBSD, OS X,
Illumos/Solaris and Windows. It is fully commented and well-documented.
Furthermore, the code is under a permissive license (MIT) allowing its use in
commercial (closed-source) applications. The build system (CMake) includes
built-in support to for cross-compiling from Linux/BSD to Windows, provided
MinGW compiler and tools are installed on the host system.

There are two parts to the library: a client-side component and an optional
server-side component. The two are built from completely different networking
designs, each suited to their particular use-cases. The client architecture is
designed for single connections and operates synchronously, waiting for
responses from the server. The server architecture is designed for many
concurrent connections and operates asynchronously.

The client-side API is simple and flexible. Connections wait (block) for
responses and can employ a timeout in which to take action if a response does
not arrive in a timely manner (or at all). The API is threadsafe in so far as
each connection must be maintained in its own thread. All global structures and
common services (error-reporting and tracing) use thread-local variables. The
API runs atop the native operating system’s networking facilities, using
`poll()` and thus no additional libraries are required.

The server-side API implements a non-blocking, multiplexing, multithreaded
server atop [`libuv`](https://libuv.org/). The server consists of a main
networking thread and a pool of worker threads that process the data. The
networking thread runs the `libuv` loop to handle socket I/O and evenly
distributes incoming data to the worker threads via a synchronized queue. The
worker threads process the data and optionally send back replies via a separate
queue. The server takes care of all the WebSocket protocol serialization and
communication between the the network and worker threads. Developers only need
to focus on the actual message processing logic to service incoming messages.

The requirement of `libuv` is what makes the server component optional. While
`libuv` runs on every major operating system, it is not expected to be a
requirement of this library, as its original intent was to provide client-side
connections only. Thus if you want use the server-side API, you simple add a
configuration switch at build time to include the code

## Webockets Overview

WebSockets significantly enhance the capabilities of web applications compared
to standard HTTP or raw TCP connections. They enable real-time data exchange
with reduced latency due to the persistent connection, making them ideal for
applications like live chat, gaming, real-time trading, and live sports
updates.

Several large-scale applications and platforms utilize WebSockets today,
including Slack, WhatsApp, and Facebook for real-time messaging. WebSockets are
also integral to the functionality of collaborative coding platforms like
Microsoft's Visual Studio Code Live Share. On the server-side, many popular
software systems support WebSocket, including Node.js, Apache, and Nginx.

### Background

Websockets emerged in the late 2000s in response to the growing need for
real-time, bidirectional communication in web applications. The goal was to
provide a standardized way for web servers to send content to browsers without
being prompted by the user, and vice versa. In December 2011 they were
standardized by the Internet Engineering Task Force (IETF) in RFC 6455. They now
enjoy wide support and integration in modern browsers, smartphones, IoT devices
and server software. They have become a fundamental technology in modern web
applications.

### Concepts and Operation

Unlike traditional HTTP connections, which are stateless and unidirectional,
WebSocket connections are stateful and bidirectional. The connection is
established through an HTTP handshake (HTTP Upgrade request), which is then
upgraded to a WebSocket connection if the server supports it. The connection
remains open until explicitly closed, enabling low-latency data exchange.

The WebSocket protocol communicates through a series of data units called
frames. Each WebSocket frame has a maximum size of 2^64 bytes (but the actual
size limit may be smaller due to network or system constraints). There are
several types of frames, including text frames, binary frames, continuation
frames, and control frames.

Text frames contain Unicode text data, while binary frames carry binary
data. Continuation frames allow for larger messages to be broken down into
smaller chunks. Control frames handle protocol-level interactions and include
close frames, ping frames, and pong frames. The close frame is used to terminate
a connection, ping frames are for checking the liveness of the connection, and
pong frames are responses to ping frames.

## Usage

For working examples beyond that shown here, see the `test_websocket.c` file in
the `src/test` directory. After building the project, stop into that directory
and run `./server` which starts a simple websocket server. Then run
`test_websocket`.

### Client API

The WebSockets API is built solely upon WebSocket constructs: frames, messages
and connections, as you would expect. It intuitively follows the concepts and
structure laid out in the standard. The following is a basic example of the
Websockets API:

```c
#include <vws/websocket.h>

int main(int argc, const char* argv[])
{
    // Create connection object
    vws_cnx* cnx = vws_cnx_new();

    // Set connection timeout to 2 seconds (the default is 10). This applies
    // both to connect() and to read operations (i.e. poll()).
    vws_socket_set_timeout((vws_socket*)cnx, 2);

    // Connect. This will automatically use SSL if "wss" scheme is used.
    cstr uri = "ws://localhost:8181/websocket";
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Can check connection state this way. Should always be true here as we
    // just successfully connected.
    assert(vws_socket_is_connected((vws_socket*)cnx) == true);

    // Enable tracing. This will dump frames to the console in human-readable
    // format as they are sent and received.
    vws.tracelevel = VT_PROTOCOL;

    // Send a TEXT frame
    vws_frame_send_text(cnx, "Hello, world!");

    // Receive websocket message
    vws_msg* reply = vws_msg_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    // Send a BINARY message
    vws_msg_send_binary(cnx, (ucstr)"Hello, world!", 14);

    // Receive websocket message
    reply = vws_msg_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    vws_disconnect(cnx);

    return 0;
}
```

### Messaging API

The Messaging API is built on top of the WebSockets API. While WebSockets
provide a mechanism for real-time bidirectional communication, it doesn't
inherently offer things like you would see in more heavyweight message protocols
like AMQP. The Messaging API provides a small step in that direction, but
without the heft. It mainly provides a more structured message format with
built-in serialization. The message structure includes two maps (hashtables of
string key/value pairs) and a payload. One map, called `routing`, is designed to
hold routing information for messaging applications. The other map, called
`headers`, is for application use. The payload can hold both text and binary
data.

The message structure operates with a higher-level connection API which works
atop the native WebSocket API. The connection API mainly adds support to send
and receive the messages, automatically handling serialization and
deserialization on and off the wire. It really just boils down to `send()` and
`receive()` calls which operate with these messages.

Messages can be serialized in two formats: JSON and MessagePack. Both formats
can be sent over the same connection on a message-by-message basis. That is, the
connection is able to auto-detect each incoming message's format and deserialize
accordingly. Thus connections support mixed-content messages: JSON and
MessagePack.

The following is a basic example of using the high-level messaging API.

```c
#include <vws/message.h>

int main()
{
    // Create connection object
    vws_cnx* cnx = vws_cnx_new();

    // Connect. This will automatically use SSL if "wss" scheme is used.
    cstr uri = "ws://localhost:8181/websocket";
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Enable tracing. This will dump frames to the console in human-readable
    // format as they are sent and received.
    vws.tracelevel = VT_PROTOCOL;

  // Create
    vrtql_msg* request = vrtql_msg_new();

    vrtql_msg_set_routing(request, "key", "value");
    vrtql_msg_set_header(request, "key", "value");
    vrtql_msg_set_content(request, "payload");

    // Send
    if (vrtql_msg_send(cnx, request) < 0)
    {
        printf("Failed to send: %s\n", vws.e.text);
        vrtql_msg_free(request);
        vws_cnx_free(cnx);
        return 1;
    }

    // Receive
    vrtql_msg* reply = vrtql_msg_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vrtql_msg_free(reply);
    }

    // Cleanup
    vrtql_msg_free(request);

    // Diconnect
    vws_disconnect(cnx);

    // Free the connection
    vws_cnx_free(cnx);

    return 0;
}
```

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
- Well tested with extensive unit tests
- Well documented (well, soon to be)
- Provides a high-level API for messaging applications supporing both JSON and
  MessagePack serialization formats within same connection.
- Includes native Ruby C extension with RDoc documentaiton.

## Installation

In order to build the code you need CMake version 3.0 or higher on your
system.

### C Library

Build as follows:

```bash
$ git clone https://github.com/vrtql/websockets.git
$ cd websockets
$ cmake .
$ make
$ sudo make install
```

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

The RDoc documentaton is located [here](https://vrtql.github.io/ws/ruby/).

### Cross-Compiling for Windows

You must have the requisite MinGW compiler and tools installed on your
system. For Debian/Devuan you would install these as follows:

```bash
$ apt-get install mingw-w64 mingw-w64-tools mingw-w64-common \
                  g++-mingw-w64-x86-64 mingw-w64-x86-64-dev
```

You will need to have OpenSSL for Windows on your system as well. If you don't
have it you can build as follows. First download the version you want to
build. Here we will use `openssl-1.1.1u.tar.gz` as an example. Create the
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
$    make
$    make DESTDIR=~/mingw install
```

Now within the `websockets` project. Modify the `CMAKE_FIND_ROOT_PATH` in the
`config/windows-toolchain.cmake` file to point to where you installed
OpenSSL. In this example it would be `~/mingw/openssl` (you might want to use
full path). Then invoke CMake as follows:

```bash
$ cmake -DCMAKE_TOOLCHAIN_FILE=config/windows-toolchain.cmake
```

Then build as normal

```bash
$ make
```

## Contributing

We welcome contributions! Please fork this repository, make your changes, and
submit a pull request. We'll review your changes and merge them if they're a
good fit for the project.

## License

This project and all third party code used by it is licensed under the MIT
License. See the [LICENSE](LICENSE) file for details.
