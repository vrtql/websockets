#include "rb_ws_common.h"
#include "rb_ws_connection.h"
#include "rb_ws_frame.h"
#include "rb_ws_message.h"

#define CLASS_NAME "Connection"

/*
 * Document-class: VRTQL::Websocket::Connection
 *
 * This class manages a WebSocket connection. It provides methods for sending
 * and receiving VRTQL messages.
 */

VALUE vr_ws_cnx_cls;

// Memory deallocation function for the vr_ws_cnx object
static void vr_ws_cnx_free(vr_ws_cnx* cnx)
{
    if (cnx->cnx)
    {
        vws_disconnect(cnx->cnx);
        vws_cnx_free(cnx->cnx);
    }

    ruby_xfree(cnx);
}

static VALUE ws_connection_allocator(VALUE the_cls)
{
    vr_ws_cnx* handle = ruby_xmalloc(sizeof(vr_ws_cnx));
    handle->cnx    = vws_cnx_new();

    return Data_Wrap_Struct(the_cls, NULL, &vr_ws_cnx_free, handle);
}

/*
 * Document-method: m_init
 *
 * call-seq: m_init -> self
 *
 * Initializes the vr_ws_cnx object.
 *
 * Returns:
 *   The initialized vr_ws_cnx object
 */
static VALUE m_init(VALUE self)
{
    return self;
}

// Checks that connection is established
static void ensure_connected(vws_cnx* c)
{
    if (vws_socket_is_connected((vws_socket*)c) == false)
    {
        rb_raise(rb_eRuntimeError, "Not connected");
    }
}

// Returns pointe to cnx instance
static vws_cnx* get_object(VALUE self)
{
    vr_ws_cnx* handle;
    Data_Get_Struct(self, vr_ws_cnx, handle);
    return handle->cnx;
}

/*
 * Document-method: m_connect
 *
 * call-seq: m_connect(url) -> true or false
 *
 * Connects to a specified host URL.
 *
 * Parameters:
 *   url - The URL of the host to connect to
 *
 * Returns:
 *   True if connection was successful, false otherwise
 */
static VALUE m_connect(VALUE self, VALUE url)
{
    vws_cnx* c = get_object(self);

    Check_Type(url, T_STRING);

    // Connect to the specified host URL.
    // Check if the connection was successful
    if (vws_connect(c, RSTRING_PTR(url)) == false)
    {
        return Qfalse;
    }

    // Return true to indicate successful connection
    return Qtrue;
}

/*
 * Document-method: m_is_connected
 *
 * call-seq: m_is_connected -> true or false
 *
 * Checks if the connection is established.
 *
 * Returns:
 *   True if the connection is established, false otherwise
 */
static VALUE m_is_connected(VALUE self)
{
    vws_cnx* c = get_object(self);

    // Call the vws_cnx_is_connected function from the C API
    bool connected = vws_socket_is_connected((vws_socket*)c);

    // Convert the boolean result to a Ruby value and return it
    if (connected)
    {
        return Qtrue;
    }
    else
    {
        return Qfalse;
    }
}

/*
 * Document-method: m_close
 *
 * call-seq: m_close -> nil
 *
 * Closes the connection.
 *
 * Returns:
 *   nil
 */
static VALUE m_close(VALUE self, VALUE url)
{
    vws_cnx* c = get_object(self);
    ensure_connected(c);

    // Connect to the specified host URL.
    // Check if the connection was successful
    vws_disconnect(c);

    return Qnil;
}

/*
 * Document-method: m_send_text
 *
 * call-seq: m_send_text(text) -> Integer
 *
 * Sends a text message via the WebSocket connection.
 *
 * Parameters:
 *   text - The text message to send
 *
 * Returns:
 *   The number of bytes sent
 */
static VALUE m_send_text(VALUE self, VALUE text)
{
    vws_cnx* c = get_object(self);
    ensure_connected(c);

    Check_Type(text, T_STRING);

    // Call the vws_send_text function from the C API
    int bytes_sent = vws_send_text(c, RSTRING_PTR(text));

    // Return the number of bytes sent as a Ruby integer
    return INT2NUM(bytes_sent);
}

/*
 * Document-method: m_send_binary
 *
 * call-seq: m_send_binary(value) -> Integer
 *
 * Sends a binary message via the WebSocket connection.
 *
 * Parameters:
 *   value - The binary message to send
 *
 * Returns:
 *   The number of bytes sent
 */
static VALUE m_send_binary(VALUE self, VALUE value)
{
    vws_cnx* c = get_object(self);
    ensure_connected(c);

    Check_Type(value, T_STRING);

    // Get the binary data and its length
    const char* data = RSTRING_PTR(value);
    long size        = RSTRING_LEN(value);

    // Call the vws_send_binary function from the C API
    int sent = vws_send_binary(c, data, size);

    // Return the number of bytes sent as a Ruby integer
    return INT2NUM(sent);
}

/*
 * Document-method: m_recv_frame
 *
 * call-seq: m_recv_frame -> vr_ws_frame or nil
 *
 * Receives a WebSocket frame from the connection.
 *
 * Returns:
 *   A WebSocket frame, or nil if no frame was received
 */
static VALUE m_recv_frame(VALUE self)
{
    vws_cnx* c = get_object(self);
    ensure_connected(c);

    // Call the vws_recv_frame function from the C API
    vws_frame* frame = vws_recv_frame(c);

    if (frame == NULL)
    {
        // No frame received, return Qnil
        return Qnil;
    }

    return vr_ws_frame_new(frame);
}

/*
 * Document-method: m_recv_msg
 *
 * call-seq: m_recv_msg -> vr_ws_msg or nil
 *
 * Receives a message from the connection.
 *
 * Returns:
 *   A WebSocket message, or nil if no message was received
 */
static VALUE m_recv_msg(VALUE self)
{
    vws_cnx* c = get_object(self);
    ensure_connected(c);

    // Call the vws_recv_msg function from the C API
    vws_msg* msg = vws_recv_msg(c);

    if (msg == NULL)
    {
        return Qnil;
    }

    return vr_ws_msg_new(msg);
}

/*
 * Document-method: m_set_timeout
 *
 * call-seq: m_set_timeout(timeout) -> nil
 *
 * Sets the timeout for the WebSocket connection.
 *
 * Parameters:
 *   timeout - The timeout value
 *
 * Returns:
 *   nil
 */
static VALUE m_set_timeout(VALUE self, VALUE timeout)
{
    vws_cnx* c = get_object(self);
    ensure_connected(c);

    // Check if the given timeout value is numeric
    if (!rb_obj_is_kind_of(timeout, rb_cNumeric))
    {
        rb_raise(rb_eTypeError, "Timeout value must be a number");
    }

    // Extract the timeout value as a double
    double timeout_value = NUM2DBL(timeout);

    // Call the vws_cnx_set_timeout function from the C API
    vws_socket_set_timeout((vws_socket*)c, (int)timeout_value);

    return Qnil;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"

void init_ws_connection(VALUE module)
{
    vr_ws_cnx_cls = rb_define_class_under(module, CLASS_NAME, rb_cObject);
    rb_define_alloc_func(vr_ws_cnx_cls, ws_connection_allocator);

    rb_define_method(vr_ws_cnx_cls, "initialize",   m_init,          0);
    rb_define_method(vr_ws_cnx_cls, "connect",      m_connect,       1);
    rb_define_method(vr_ws_cnx_cls, "close",        m_close,         0);
    rb_define_method(vr_ws_cnx_cls, "sendText",     m_send_text,     1);
    rb_define_method(vr_ws_cnx_cls, "sendBinary",   m_send_binary,   1);
    rb_define_method(vr_ws_cnx_cls, "recvFrame",    m_recv_frame,    0);
    rb_define_method(vr_ws_cnx_cls, "recvMessage",  m_recv_msg,      0);
    rb_define_method(vr_ws_cnx_cls, "setTimeout",   m_set_timeout,   1);
}

#pragma GCC diagnostic pop
#pragma clang diagnostic pop
