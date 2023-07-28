#include "rb_map.h"
#include "vrtql/message.h"

#define CLASS_NAME "Map"

VALUE vr_map_cls;

// Create a new message object
VALUE vr_map_new(struct sc_map_str* map)
{
    // Wrap the sc_map_str pointer with a Ruby object
    vr_map* obj = ruby_xmalloc(sizeof(vr_map));
    obj->map    = map;

    // Wrap the vr_msg object with a Ruby object and set the vr_msg_free
    // function as the deallocator
    return Data_Wrap_Struct(vr_map_cls, NULL, vr_map_free, obj);
}

void vr_map_free(vr_map* obj)
{
    ruby_xfree(obj);
}

static VALUE msg_allocator(VALUE the_cls)
{
    rb_raise(rb_eRuntimeError, "Cannot create a VRTQL::Map");

    vr_map* handle = ruby_xmalloc(sizeof(vr_map));
    handle->map       = NULL;

    return Data_Wrap_Struct(the_cls, NULL, &vr_map_free, handle);
}

static struct sc_map_str* get_object(VALUE self)
{
    vr_map* handle;
    Data_Get_Struct(self, vr_map, handle);

    return handle->map;
}

static VALUE m_equals(VALUE self, VALUE other)
{
    // If value is not a Map object, return Qfalse
    if (rb_obj_class(self) != rb_obj_class(other))
    {
        return Qfalse;
    }

    // Get C structures

    struct sc_map_str* me   = get_object(self);
    struct sc_map_str* them = get_object(other);

    // If sizes are the different, return Qfalse
    if (sc_map_size_str(me) != sc_map_size_str(them))
    {
        return Qfalse;
    }

    // Compare elements

    char* key; char* value;
    sc_map_foreach(me, key, value)
    {
        cstr v = sc_map_get_str(them, key);

        // If value not found, return Qfalse
        if (sc_map_found(them) == false)
        {
            return Qfalse;
        }

        // If the the values are not equal, return Qfalse
        if (strcmp(v, value) != 0)
        {
            return Qfalse;
        }
    }

    return Qtrue;
}

static VALUE m_set(VALUE self, VALUE key, VALUE value)
{
    struct sc_map_str* map = get_object(self);

    char* c_key   = StringValueCStr(key);
    char* c_value = StringValueCStr(value);

    vws_map_set(map, c_key, c_value);

    return Qnil;
}

static VALUE m_get(VALUE self, VALUE key)
{
    struct sc_map_str* map = get_object(self);

    char* c_key = StringValueCStr(key);
    char* val   = vws_map_get(map, c_key);

    if (val == NULL)
    {
        return Qnil;
    }

    return rb_str_new_cstr(val);
}

void init_map(VALUE module)
{
    vr_map_cls = rb_define_class_under(module, CLASS_NAME, rb_cObject);
    rb_define_alloc_func(vr_map_cls, msg_allocator);

    rb_define_method(vr_map_cls, "==",      m_equals,  1);
    rb_define_method(vr_map_cls, "set",        m_set,  2);
    rb_define_method(vr_map_cls, "[]=",        m_set,  2);
    rb_define_method(vr_map_cls, "get",        m_get,  1);
    rb_define_method(vr_map_cls, "[]",         m_get,  1);
}
