# VRTQL Websockets Client Library

## Description

This is a WebSocket client library written in C. It provides a simple yet
flexible API for building WebSocket clients and supports all standard WebSocket
features, including text and binary messages, ping/pong frames, and connection
control frames. It supports both plain and encrypted connections (via
OpenSSL).

The motivation behind the project is to have a portable WebSockets client
library under a permissive license (MIT) which feels like a traditional socket
API (blocking with optional timeout) which can also provide a foundation for
some additional messaging features similar to (but lighter weight than) AMQP and
MQTT.

As a client-side library, connections are blocking with optional timeout. The
API is threadsafe in so far as each connection must be maintained in its own
thread. All global structures and common services (error-reporting and tracing)
use thread-local variables.

The library compiles and runs on Linux, FreeBSD, NetBSD, OpenBSD, Mac, Solaris
and Windows. The build system (CMake) includes built-in support to for
cross-compiling from Linux/BSD to Windows, provided MinGW compiler and tools are
installed on the host system. Instructions on cross-compiling are included in
the documentation.

The code is under a permissive license (MIT) allowing its use in commercial
(closed-source) applications.

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

## Library API

There are two APIs in the library: the WebSockts API and the Messaging API.

For working examples beyond that shown here, see the `test_websocket.c` file in
the `src/test` directory. After building the project, stop into that directory
and run `./server` which starts a simple websocket server. Then run
`test_websocket`.

### Websockets API

The WebSockets API is built solely upon WebSocket constructs: frames, messages
and connections, as you would expect. It intuitively follows the concepts and
structure laid out in the standard. The following is a basic example of the
Websockets API:

```c
#include <vrtql/websocket.h>

int main()
{
    // Allocate a connection
    vws_cnx* cnx = vws_cnx_new();

    // Connect: vws_connect() will detect "wss" scheme and automatically use SSL
    cstr uri = "ws://localhost:8000/websocket";
    vws_connect(cnx, uri);

    // Check if the connection was successful
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Check connection state. This should always be true here.
    assert(vws_cnx_is_connected(cnx) == true);

    // Set timeout to 60 seconds (default is 10)
    vws_cnx_set_timeout(cnx, 60);

    // Enable tracing. This will dump frames sent and received.
    cnx->trace = true;

    // Send a text message
    vws_send_text(cnx, "Hello, world!");

    // Receive websocket message
    vws_msg* reply = vws_recv_msg(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    // Send a binary message
    vws_send_binary(cnx, "Hello, world!", 14);

    // Receive websocket message
    reply = vws_recv_msg(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    // Disconnect and cleanup
    vws_cnx_free(cnx);

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
#include <vrtql/websocket.h>
#include <vrtql/message.h>

int main()
{
    // Allocate a connection
    vws_cnx* cnx = vws_cnx_new();

    // Connect: vws_connect() will detect "wss" scheme and automatically use SSL
    cstr uri = "ws://localhost:8000/websocket";
    vws_connect(cnx, uri);

    // Check if the connection was successful
    if (vws_connect(cnx, uri) == false)
    {
        printf("Failed to connect to the WebSocket server\n");
        vws_cnx_free(cnx);
        return 1;
    }

    // Check connection state. This should always be true here.
    assert(vws_cnx_is_connected(cnx) == true);

    // Create
    vrtql_msg* request = vrtql_msg_new();

    vrtql_msg_set_routing(request, "key", "value");
    vrtql_msg_set_header(request, "key", "value");
    vrtql_msg_set_content(request, "payload");

    // Send
    if (vrtql_msg_send(cnx, request) < 0)
    {
        printf("Failed to send: %s\n", vrtql.e.text);
        vrtql_msg_free(request);
        vws_cnx_free(cnx);
        return 1;
    }

    // Receive
    vrtql_msg* reply = vrtql_msg_receive(cnx);

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

    // Disconnect and cleanup
    vws_cnx_free(cnx);

    return 0;
}
```

## Documentation

Full documentation is located [here](https://vrtql.github.io/ws/ws.html).

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
