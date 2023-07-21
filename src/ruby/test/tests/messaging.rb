class MessagingTest < Minitest::Test

  def setup
    # Establish a connection before each test
    @connection = VRTQL::Connection.new()
    @server     = 'ws://localhost:8181/websocket'

    assert @connection.connect(@server) == true
  end

  def teardown
    @connection.close
  end

  def test_message_serialization
    content = "content"
    routing = { "id" => "test" }
    headers = { "id" => "test" }

    m1 = VRTQL::Message.new()

    m1.routing = routing
    m1.headers = headers
    m1.content = content

    data = m1.serialize()

    m2 = VRTQL::Message.new()

    m2.deserialize(data)

    puts m1.dump()
    puts m2.dump()

    assert m1.routing == m2.routing
    assert m1.headers == m2.headers
    assert m2.content == m1.content
  end

  # Base class send/receive
  def test_send_text
    data = 'Hello, world!'

    bytes = @connection.sendText(data)
    assert_operator bytes, :>, 0

    message = @connection.recvMessage()
    refute_nil message
    assert_instance_of VRTQL::WS::Message, message

    assert message.data == data
  end

  # VRTQL::Message send/receive
  def test_send
    content = "content"

    # Send/receive MessagePack

    m1 = VRTQL::Message.new()

    m1.routing = { "to" => "mike" }
    m1.headers = { "id" => "test" }
    m1.content = content

    bytes = @connection.send(m1)
    assert_operator bytes, :>, 0

    m2 = @connection.receive()
    refute_nil m2
    assert_instance_of VRTQL::Message, m2
    puts m2.dump()
    assert m2.content == content

    # Send/receive JSON

    @connection.format = :json

    bytes = @connection.send(m1)
    assert_operator bytes, :>, 0

    m2 = @connection.receive()
    refute_nil m2
    assert_instance_of VRTQL::Message, m2
    puts m2.dump()
    assert m2.content == content

  end

  def test_set_timeout
    skip
    @connection.setTimeout(30.0)
    # Add assertions to test the timeout value
  end

end
