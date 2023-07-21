#include "rb_ws_common.h"
#include "rb_ws_connection.h"
#include "rb_mq_connection.h"
#include "rb_mq_message.h"

VALUE vr_mq_cnx_cls;

#define CLASS_NAME "Connection"

/*
 * Document-class: VRTQL::Connection
 *
 * This class manages a WebSocket connection to a VRTQL server.
 * It provides methods for sending and receiving VRTQL messages.
 */

// Memory deallocation function for the vr_mq_cnx object
static void vr_mq_cnx_free(vr_mq_cnx* cnx)
{
    // Base class frees the connection by calling vr_ws_cnx_free(&cnx->base). We
    // don't call it. If you do you get double-free corruption.

    ruby_xfree(cnx);
}

static VALUE vrtql_connection_allocator(VALUE the_cls)
{
    vr_mq_cnx* handle = ruby_xmalloc(sizeof(vr_mq_cnx));

    // Initialize base class
    handle->base.cnx       = vws_cnx_new();
    handle->default_format = VM_MPACK_FORMAT;

    return Data_Wrap_Struct(the_cls, NULL, &vr_mq_cnx_free, handle);
}

/*
 * Document-method: Connection#initialize
 *
 * call-seq: initialize() -> self
 *
 * Initializes a new Connection object.
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
    vr_mq_cnx* c;
    Data_Get_Struct(self, vr_mq_cnx, c);
    return c->base.cnx;
}

/*
 * Document-method: Connection#send
 *
 * call-seq: send(value) -> Integer
 *
 * Sends a VRTQL message via the WebSocket connection.
 *
 * @param  value [VRTQL::Message] The message to send.

 * @param format [:Symbol] (optional) Specify the serialization format. This can
 * be either :json or :mpack. The default is :mpack.

 * @return [Integer] The number of bytes sent.
 */

static VALUE m_send(int argc, VALUE* argv, VALUE self)
{
    // Check if the format argument is provided
    VALUE value, format_value;
    rb_scan_args(argc, argv, "11", &value, &format_value);

    // Get Ruby struct
    vr_mq_cnx* handle;
    Data_Get_Struct(self, vr_mq_cnx, handle);

    vrtql_msg_format_t format;

    // Rest of your implementation...

    if (argc == 1)
    {
        // Handle the case where format is not provided
        format = handle->default_format;
    }
    else if (argc == 2)
    {
        // Handle the case where format is provided

        if (SYM2ID(format_value) == rb_intern("json"))
        {
            format = VM_JSON_FORMAT;
        }
        else if (SYM2ID(format_value) == rb_intern("mpack"))
        {
            format = VM_MPACK_FORMAT;
        }
        else
        {
            rb_raise( rb_eArgError,
                      "Invalid format specified. "
                      "Expected :json or :mpack.");
        }
    }
    else
    {
        rb_raise(rb_eArgError, "Wrong number of arguments. Expected 1 or 2.");
    }

    vws_cnx* c = get_object(self);
    ensure_connected(c);

    // Serialize the VRTQL::Message to binary and send

    vrtql_msg* msg = vr_mq_get_object(value);
    msg->format = format;
    vrtql_buffer* binary = vrtql_msg_serialize(msg);
    int sent = vws_msg_send_binary(c, binary->data, binary->size);
    vrtql_buffer_free(binary);

    // Return the number of bytes sent as a Ruby integer
    return INT2NUM(sent);
}

/*
 * Document-method: Connection#receive
 *
 * call-seq: receive() -> VRTQL::Message or nil
 *
 * Receives a VRTQL message from the WebSocket connection.
 *
 * @return [VRTQL::Message, nil] The received message, or nil if
 *   no message was received.
 */
static VALUE m_receive(VALUE self)
{
    vws_cnx* c = get_object(self);
    ensure_connected(c);

    // Call the vws_recv_msg function from the C API
    vws_msg* m = vws_msg_recv(c);

    if (m == NULL)
    {
        return Qnil;
    }

    // Create VRTQL::Message C struct and Ruby object
    vrtql_msg* msg = vrtql_msg_new();
    VALUE object   = vr_mq_msg_new(msg);

    // Deserialize
    cstr data   = m->data->data;
    size_t size = m->data->size;
    if (vrtql_msg_deserialize(msg, data, size) == false)
    {
        // Set invalid (clear valid flag)
        vrtql_clear_flag(&msg->flags, VM_MSG_VALID);
    }

    vws_msg_free(m);

    return object;
}

/*
 * Document-method: Connection#format=
 *
 * call-seq: format(value) -> Symbol
 *
 * Sets the default message serializeation form
 *
 * @param format [:Symbol] (optional) Specify the serialization format. This can
 * be either :json or :mpack. The default is :mpack.
 *
 * @return [Integer] The number of bytes sent.
 */
static VALUE m_set_format(VALUE self, VALUE format_value)
{
    // Get Ruby struct
    vr_mq_cnx* handle;
    Data_Get_Struct(self, vr_mq_cnx, handle);

    vrtql_msg_format_t format;

    if (SYM2ID(format_value) == rb_intern("json"))
    {
        handle->default_format = VM_JSON_FORMAT;
    }
    else if (SYM2ID(format_value) == rb_intern("mpack"))
    {
        handle->default_format = VM_MPACK_FORMAT;
    }
    else
    {
        rb_raise( rb_eArgError,
                  "Invalid format specified. "
                  "Expected :json or :mpack.");
    }

    return Qnil;
}


void init_mq_connection(VALUE module)
{
    vr_mq_cnx_cls = rb_define_class_under(module, CLASS_NAME, vr_ws_cnx_cls);
    rb_define_alloc_func(vr_mq_cnx_cls, vrtql_connection_allocator);

    rb_define_method(vr_mq_cnx_cls, "initialize", m_init,        0);
    rb_define_method(vr_mq_cnx_cls, "format=",    m_set_format,  1);
    rb_define_method(vr_mq_cnx_cls, "send",       m_send,       -1);
    rb_define_method(vr_mq_cnx_cls, "receive",    m_receive,     0);
}
