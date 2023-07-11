#include "rb_ws_common.h"
#include "rb_ws_frame.h"

#define CLASS_NAME "Frame"

VALUE vr_ws_frame_cls;

#include "rb_ws_common.h"
#include "rb_ws_frame.h"

VALUE vr_ws_frame_new(vws_frame* frame)
{
    // Wrap the vws_msg pointer with a Ruby object
    vr_ws_frame* obj = ruby_xmalloc(sizeof(vr_ws_frame_cls));
    obj->frame = frame;

    // Wrap the vr_ws_msg object with a Ruby object and set the vr_ws_msg_free
    // function as the deallocator
    return Data_Wrap_Struct(vr_ws_frame_cls, NULL, vr_ws_frame_free, obj);
}

void vr_ws_frame_free(vr_ws_frame* object)
{
    if (object->frame)
    {
        vws_frame_free(object->frame);
    }

    ruby_xfree(object);
}

/*
 * Document-method: m_data
 *
 * call-seq: m_data -> String
 *
 * Returns the frame data.
 *
 * Returns:
 *   The frame data as a Ruby string
 */
static VALUE m_data(VALUE self)
{
    vr_ws_frame* object;
    Data_Get_Struct(self, vr_ws_frame, object);

    // Get the frame data and its length
    const char* data = object->frame->data;
    size_t size      = object->frame->size;

    // Return the frame data as a Ruby string with length
    return rb_str_new(data, size);
}

/*
 * Document-method: m_fin
 *
 * call-seq: m_fin -> true or false
 *
 * Returns the FIN value of the frame.
 *
 * Returns:
 *   The FIN value as a Ruby boolean
 */
static VALUE m_fin(VALUE self)
{
    vr_ws_frame* object;
    Data_Get_Struct(self, vr_ws_frame, object);

    // Return the FIN value as a Ruby boolean
    return object->frame->fin ? Qtrue : Qfalse;
}

/*
 * Document-method: m_opcode
 *
 * call-seq: m_opcode -> Integer
 *
 * Returns the opcode of the frame.
 *
 * Returns:
 *   The opcode value as a Ruby integer
 */
static VALUE m_opcode(VALUE self)
{
    vr_ws_frame* object;
    Data_Get_Struct(self, vr_ws_frame, object);

    // Return the opcode value as a Ruby integer
    return INT2NUM(object->frame->opcode);
}

/*
 * Document-method: m_size
 *
 * call-seq: m_size -> Integer
 *
 * Returns the size of the frame.
 *
 * Returns:
 *   The size of the frame as a Ruby integer
 */
static VALUE m_size(VALUE self)
{
    vr_ws_frame* object;
    Data_Get_Struct(self, vr_ws_frame, object);

    // Get the frame size
    size_t size = object->frame->size;

    // Return the frame size as a Ruby integer
    return INT2NUM(size);
}

void init_ws_frame(VALUE module)
{
    vr_ws_frame_cls = rb_define_class_under(module, CLASS_NAME, rb_cObject);

    rb_define_method(vr_ws_frame_cls, "data", m_data,     0);
    rb_define_method(vr_ws_frame_cls, "size", m_size,     0);
    rb_define_method(vr_ws_frame_cls, "fin",    m_fin,    0);
    rb_define_method(vr_ws_frame_cls, "opcode", m_opcode, 0);
}
