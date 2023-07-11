# VRTQL

The VRTQL Ruby extension is a robust and performance-oriented WebSocket client
library `libvrtql` C library. It provided a high-level interface to establish
and manage client-side WebSocket connections and messages.

The library offers a tiered set of classes to cater to different levels of
abstraction and control over the WebSocket protocol: `VRTQL::Connection` and
`VRTQL::Message` for a high-level and user-friendly messaging, and
`VRTQL::Websocket::Connection` and `VRTQL::Websocket::Message` for more granular
control and lower-level websocket operations.

### VRTQL::Connection

The `VRTQL::Connection` class provides a high-level interface to establish and
manage a WebSocket connection. In contrast to the `VRTQL::Websocket::Connection`
class, it includes handling of custom VRTQL protocol details, making it more
convenient for most use cases.

#### `connect(url)`

Connects to a WebSocket server at the given URL. If the connection fails, it
raises a `RuntimeError`. Returns a `Connection` object on success.

#### `send(message)`

Sends a `VRTQL::Message` to the WebSocket server. Returns the number of bytes
sent.

#### `receive()`

Receives a `VRTQL::Message` from the server. Returns a `Message` object.

### VRTQL::Message

The `VRTQL::Message` class provides a user-friendly interface to handle messages
similar tomessage protocols like AMQP. It contains two maps, one for message
routing called `routing`, another map for application headers called `headers`
and content body to hold the message data.

Messages can be serialized into two formats: JSON and MessagePack. The
`VRTQL::Connection` class supports sending and receiving both formats over a
single connection, auto-detecting the incoming format. The default format is
MessagePack.

### Lower-Level Classes

For granular control and lower-level operations, `VRTQL::Websocket::Connection`
and `VRTQL::Websocket::Message` classes are provided. Please refer to the
original README for their detailed explanation.

### VRTQL::Webocket::Connection

#### `connect(url)`

Connects to a WebSocket server at the given URL. The URL must be a Ruby
string. If the connection fails, it raises a `RuntimeError`. Returns a
`Connection` object on success.

#### `sendText(text)`

Sends a text message to the WebSocket server. The text must be a Ruby
string. Returns the number of bytes sent.

#### `sendBinary(binary_data)`

Sends a binary message to the WebSocket server. The binary data must be a Ruby
string. Returns the number of bytes sent.

#### `recvFrame()`

Receives a WebSocket frame from the server. If no frame is available, it returns
`nil`. Otherwise, it returns a `Frame` object.

#### `recvMessage()`

Receives a WebSocket message from the server. Returns a `Message` object.

#### `setTimeout(timeout)`

Sets the timeout for the WebSocket connection. The timeout value must be a
number (a `Float` or `Fixnum`) representing the number of seconds.

### VRTQL::Frame

The `Frame` class represents a WebSocket frame and provides several methods to
inspect its properties:

#### `data()`

Returns the frame data as a Ruby string.

#### `fin()`

Returns a boolean value indicating whether the frame's FIN bit is set. The FIN
bit indicates whether this frame is the final frame in a message. A value of
`true` means it is the final frame, while `false` means it is not.

#### `opcode()`

Returns the frame's opcode as a Ruby integer. The opcode indicates the type of
data contained in the frame. It can be one of the following values:

- `0`: Continuation frame
- `1`: Text frame
- `2`: Binary frame
- `8`: Connection close frame
- `9`: Ping frame
- `10`: Pong frame

### VRTQL::Websocket::Message

The `Message` class represents a WebSocket message and provides several methods
to inspect its properties:

#### `data()`

Returns the message data as a Ruby string.

#### `opcode()`

Returns the message's opcode as a Ruby integer. The opcode indicates the type of
data contained in the message.

## Usage

The following illustrates use of the high-level messaging API:

```ruby

require 'vrtql/ws'

# Create a new connection to a WebSocket server
connection = VRTQL::Connection.new()
server = 'wss://example.com/websocket'
connection.connect(server)

# Create a new message
message = VRTQL::Message.new()
message.routing = { "to" => "user" }
message.headers = { "id" => "message_id" }
message.content = "Hello, world!"

# Send the message
connection.send(message)

# Receive a message
received_message = connection.receive()
```

The following illustrates use of the low-level websocket API:

```ruby
require 'vrtql/ws'

# Create a new connection to a WebSocket server
connection = VRTQL::Connection.connect('wss://example.com/websocket')

# Set the timeout for the connection
connection.setTimeout(30.0)

# Send a text message
connection.sendText('Hello, world!')

# Send a binary message
connection.sendBinary("\x01\x02\x03\x04")

# Receive a frame and inspect its properties
frame = connection.recvFrame()

puts "Frame data: #{frame.data}"
puts "Frame FIN: #{frame.fin}"
puts "Frame opcode: #{frame.opcode}"

# Receive a message and inspect its properties
message = connection.recvMessage()

puts "Message data: #{message.data}"
puts "Message opcode: #{message.opcode}"

```

Remember to replace `'wss://example.com/websocket'` with the URL of the actual
WebSocket server you want to connect to.

## Installation

This is a Ruby C extension for VRTQL websockets library.

To install from Ruby Gems:

```
    gem install vrtql-websockets
```

To build/install from source:

```
    git clone git@github.com:vrtql/websockets-ruby.git
    cd websockets-ruby
    gem install rake-compiler
    rake
    rake gem
    cd pkg
    gem install -l vrtql-websockets-*.gem
```
