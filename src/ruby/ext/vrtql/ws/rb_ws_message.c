#include "rb_ws_common.h"
#include "rb_ws_message.h"

#define CLASS_NAME "Message"

VALUE vr_ws_msg_cls;

// Create a new message object
VALUE vr_ws_msg_new(vws_msg* msg)
{
    // Wrap the vws_msg pointer with a Ruby object
    vr_ws_msg* msg_obj = ruby_xmalloc(sizeof(msg_obj));
    msg_obj->msg = msg;

    // Wrap the vr_ws_msg object with a Ruby object and set the vr_ws_msg_free
    // function as the deallocator
    return Data_Wrap_Struct(vr_ws_msg_cls, NULL, vr_ws_msg_free, msg_obj);
}

// Memory deallocation function for the vr_ws_msg object
void vr_ws_msg_free(vr_ws_msg* object)
{
    if (object->msg)
    {
        vws_msg_free(object->msg);
    }

    ruby_xfree(object);
}
/*
 * Document-method: m_data
 *
 * call-seq: m_data -> String
 *
 * Returns the message data.
 *
 * Returns:
 *   The message data as a Ruby string
 */
static VALUE m_data(VALUE self)
{
    vr_ws_msg* object;
    Data_Get_Struct(self, vr_ws_msg, object);

    // Get the message data and its length
    const char* data = object->msg->data->data;
    size_t size      = object->msg->data->size;

    // Return the message data as a Ruby string with length
    return rb_str_new(data, size);
}

/*
 * Document-method: m_opcode
 *
 * call-seq: m_opcode -> Integer
 *
 * Returns the opcode of the message.
 *
 * Returns:
 *   The opcode value as a Ruby integer
 */
static VALUE m_opcode(VALUE self)
{
    vr_ws_msg* object;
    Data_Get_Struct(self, vr_ws_msg, object);

    // Return the opcode value as a Ruby integer
    return INT2NUM(object->msg->opcode);
}

/*
 * Document-method: m_is_text
 *
 * call-seq: m_is_text -> true or false
 *
 * Checks if the message is a text message.
 *
 * Returns:
 *   True if the message is a text message, false otherwise
 */
static VALUE m_is_text(VALUE self)
{
    vr_ws_msg* object;
    Data_Get_Struct(self, vr_ws_msg, object);

    // If opcode is 1, then message is a text message
    if(object->msg->opcode == 1)
    {
        return Qtrue;
    }

    return Qfalse;
}

/*
 * Document-method: m_is_binary
 *
 * call-seq: m_is_binary -> true or false
 *
 * Checks if the message is a binary message.
 *
 * Returns:
 *   True if the message is a binary message, false otherwise
 */
static VALUE m_is_binary(VALUE self)
{
    vr_ws_msg* object;
    Data_Get_Struct(self, vr_ws_msg, object);

    // If opcode is 2, then message is a binary message
    if(object->msg->opcode == 2)
    {
        return Qtrue;
    }

    return Qfalse;
}

void init_ws_message(VALUE module)
{
    vr_ws_msg_cls = rb_define_class_under(module, CLASS_NAME, rb_cObject);

    rb_define_method(vr_ws_msg_cls, "data",     m_data,     0);
    rb_define_method(vr_ws_msg_cls, "opcode",   m_opcode,   0);
    rb_define_method(vr_ws_msg_cls, "isText",   m_is_text,  0);
    rb_define_method(vr_ws_msg_cls, "isBinary", m_is_binary,0);
}
