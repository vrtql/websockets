class WebsocketTest < Minitest::Test

  def setup
    # Establish a connection before each test
    @connection = VRTQL::WS::Connection.new()
    @server     = 'ws://localhost:8181/websocket'

    assert @connection.connect(@server) == true
  end

  def teardown
    # Close the connection after each test
    @connection.close
  end

  def test_send_text
    data = 'Hello, world!'

    bytes = @connection.sendText(data)
    assert_operator bytes, :>, 0

    message = @connection.recvMessage()
    refute_nil message
    assert_instance_of VRTQL::WS::Message, message

    assert message.data == data
  end

  def test_send_binary
    data = "\x01\x02\x03\x04"
    bytes = @connection.sendBinary(data)
    assert_operator bytes, :>, 0

    message = @connection.recvMessage()
    refute_nil message
    assert_instance_of VRTQL::WS::Message, message

    assert message.data == data
  end

  def test_receive_frame
    data = 'Hello, world!'
    bytes = @connection.sendText(data)
    assert_operator bytes, :>, 0

    frame = @connection.recvFrame()
    refute_nil frame
    assert_instance_of VRTQL::WS::Frame, frame

    assert frame.data == data
  end

  def test_set_timeout
    skip
    @connection.setTimeout(30.0)
    # Add assertions to test the timeout value
  end
end
