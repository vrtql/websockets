#include "rb_map.h"
#include "rb_mq_message.h"

#define CLASS_NAME "Message"

/*
 * Document-class: Message
 *
 * This class represents a VRTQL::Message. It provides methods for creating,
 * managing, and manipulating messages in VRTQL::Connection which uses it as the
 * basis for sending and receiving messages.
 */

VALUE vr_mq_msg_cls;

/*
 * Document-method: vr_mq_msg_new
 *
 * call-seq: vr_mq_msg_new(msg) -> VRTQL::Message
 *
 * Creates a new VRTQL::Message object.
 *
 * Parameters:
 *   msg - A pointer to a vrtql_msg struct
 *
 * Returns:
 *   A new VRTQL::Message object
 */
VALUE vr_mq_msg_new(vrtql_msg* msg)
{
    // Wrap the vrtql_msg pointer with a Ruby object
    vr_mq_msg* msg_obj = ruby_xmalloc(sizeof(vr_mq_msg));
    msg_obj->msg = msg;

    // Wrap the vr_msg object with a Ruby object and set the vr_msg_free
    // function as the deallocator
    return Data_Wrap_Struct(vr_mq_msg_cls, NULL, vr_mq_msg_free, msg_obj);
}

void vr_mq_msg_free(vr_mq_msg* msg)
{
    if (msg->msg)
    {
        vrtql_msg_free(msg->msg);
    }

    ruby_xfree(msg);
}

static VALUE msg_allocator(VALUE the_cls)
{
    vr_mq_msg* handle = ruby_xmalloc(sizeof(vr_mq_msg));
    handle->msg       = vrtql_msg_new();

    return Data_Wrap_Struct(the_cls, NULL, &vr_mq_msg_free, handle);
}

static vrtql_msg* get_object(VALUE self)
{
    vr_mq_msg* handle;
    Data_Get_Struct(self, vr_mq_msg, handle);

    return handle->msg;
}

vrtql_msg* vr_mq_get_object(VALUE obj)
{
    if (rb_obj_class(obj) != vr_mq_msg_cls)
    {
        rb_raise(rb_eRuntimeError, "Not a VRTQL::Message object");
    }

    return get_object(obj);
}

/*
 * Document-method: m_get_headers
 *
 * call-seq: m_get_headers -> Hash
 *
 * Retrieves the headers from the Message.
 *
 * Returns:
 *   A hash of headers
 */
static VALUE m_get_headers(VALUE self)
{
    vrtql_msg* msg = get_object(self);

    VALUE map = vr_map_new(&msg->headers);

    // Assign the reference from table to self to keep a reference count on it.
    rb_ivar_set(map, rb_intern("message"), self);

    return map;
}

// Function to be called for each key/value pair in the hash
static int hash_iter(VALUE key, VALUE value, st_data_t map_ptr)
{
    // Check that the key and value are both strings
    Check_Type(key, T_STRING);
    Check_Type(value, T_STRING);

    // Get pointers to the key and value strings
    char* c_key   = RSTRING_PTR(key);
    char* c_value = RSTRING_PTR(value);

    // Get the map from the data pointer
    struct sc_map_str* map = (struct sc_map_str*)map_ptr;

    // Set the key/value in the map
    vrtql_map_set(map, c_key, c_value);
}

/*
 * Document-method: m_is_valid
 *
 * call-seq: m_is_valid -> true or false
 *
 * Checks if the Message is valid.
 *
 * Returns:
 *   True if the message is valid, false otherwise.
 */
static VALUE m_is_valid(VALUE self)
{
    vrtql_msg* msg = get_object(self);

    if (vrtql_msg_is_flag(msg, VM_MSG_VALID))
    {
        return Qtrue;
    }

    return Qfalse;
}

/*
 * Document-method: m_set_headers
 *
 * call-seq: m_set_headers(hash) -> self
 *
 * Sets the headers in the Message.
 *
 * Parameters:
 *   hash - A hash of headers
 *
 * Returns:
 *   The updated Message object
 */
static VALUE m_set_headers(VALUE self, VALUE hash)
{
    vrtql_msg* msg = get_object(self);

    // Check that hash is a Hash
    Check_Type(hash, T_HASH);

    // Clear map contents
    char* key; char* value;
    sc_map_foreach(&msg->headers, key, value)
    {
        free(key);
        free(value);
    }

    // Iterate over Hash and set each key/value pair
    rb_hash_foreach(hash, hash_iter, (st_data_t)&msg->headers);

    return self;
}

/*
 * Document-method: m_set_routing
 *
 * call-seq: m_set_routing(hash) -> self
 *
 * Sets the routing in the Message.
 *
 * Parameters:
 *   hash - A hash of routing information
 *
 * Returns:
 *   The updated Message object
 */
static VALUE m_set_routing(VALUE self, VALUE hash)
{
    vrtql_msg* msg = get_object(self);

    // Check that hash is a Hash
    Check_Type(hash, T_HASH);

    // Clear map contents
    char* key; char* value;
    sc_map_foreach(&msg->routing, key, value)
    {
        free(key);
        free(value);
    }

    // Iterate over Hash and set each key/value pair
    rb_hash_foreach(hash, hash_iter, (st_data_t)&msg->routing);

    return self;
}

/*
 * Document-method: m_get_routing
 *
 * call-seq: m_get_routing -> Hash
 *
 * Retrieves the routing from the Message.
 *
 * Returns:
 *   A hash of routing
 */
static VALUE m_get_routing(VALUE self)
{
    vrtql_msg* msg = get_object(self);

    VALUE map = vr_map_new(&msg->routing);

    // Assign the reference from table to self to keep a reference count on it.
    rb_ivar_set(map, rb_intern("message"), self);

    return map;
}

/*
 * Document-method: m_set_content
 *
 * call-seq: m_set_content(content) -> nil
 *
 * Sets the content of the Message.
 *
 * Parameters:
 *   content - A string representing the content
 *
 * Returns:
 *   nil
 */
static VALUE m_set_content(VALUE self, VALUE content)
{
    vrtql_msg* msg = get_object(self);

    // Ensure that the content is a string
    Check_Type(content, T_STRING);

    // Get the pointer to the string and its size
    cstr data = RSTRING_PTR(content);
    long size = RSTRING_LEN(content);

    // Use the pointer to the string and the size to set the message content
    vrtql_msg_set_content_binary(msg, data, size);

    return Qnil;
}

/*
 * Document-method: m_get_content
 *
 * call-seq: m_get_content -> String
 *
 * Retrieves the content from the Message.
 *
 * Returns:
 *   A string of the message content
 */
static VALUE m_get_content(VALUE self)
{
    vrtql_msg* msg = get_object(self);
    char* val      = vrtql_msg_get_content(msg);

    if (val == NULL)
    {
        return Qnil;
    }

    return rb_str_new_cstr(val);
}

/*
 * Document-method: m_serialize
 *
 * call-seq: m_serialize -> String
 *
 * Serializes the Message.
 *
 * Returns:
 *   A string representing the serialized message
 */
static VALUE m_serialize(VALUE self)
{
    vrtql_msg* msg    = get_object(self);
    vrtql_buffer* buf = vrtql_msg_serialize(msg, VM_MPACK_FORMAT);
    VALUE binary      = rb_str_new(buf->data, buf->size);

    vrtql_buffer_free(buf);

    return binary;
}

/*
 * Document-method: m_deserialize
 *
 * call-seq: m_deserialize(data) -> true or false
 *
 * Deserializes the Message.
 *
 * Parameters:
 *   data - A string representing the serialized message
 *
 * Returns:
 *   True if deserialization is successful, false otherwise
 */
static VALUE m_deserialize(VALUE self, VALUE data)
{
    // Ensure that the data is a string
    Check_Type(data, T_STRING);

    vrtql_msg* msg = get_object(self);
    bool ret       = vrtql_msg_deserialize( msg,
                                            RSTRING_PTR(data),
                                            RSTRING_LEN(data) );

    return ret ? Qtrue : Qfalse;
}

// Function to print key-value pairs from the map
static void map_dump_iter(const char *key, const char *value)
{
    printf("%s: %s\n", key, value);
}

/*
 * Document-method: m_dump
 *
 * call-seq: m_dump -> nil
 *
 * Dumps Message to human readable output
 *
 * Returns:
 *   nil
 */
static VALUE m_dump(VALUE self)
{
    char* key; char* value;

    vrtql_msg* msg = get_object(self);

    printf("[Message Dump]\n");

    printf("routing\n");

    sc_map_foreach(&msg->routing, key, value)
    {
        printf("  [%s]: %s\n", key, value);
    }

    printf("headers\n");

    sc_map_foreach(&msg->headers, key, value)
    {
        printf("  [%s]: %s\n", key, value);
    }

    printf("content: ");
    const char* content = vrtql_msg_get_content(msg);
    if (content)
    {
        printf("%s\n", content);
    }
    else
    {
        printf("NULL\n");
    }

    printf("\n");

    return Qnil;
}

void init_mq_message(VALUE module)
{
    vr_mq_msg_cls = rb_define_class_under(module, CLASS_NAME, rb_cObject);
    rb_define_alloc_func(vr_mq_msg_cls, msg_allocator);

    rb_define_method(vr_mq_msg_cls, "dump",         m_dump,         0);
    rb_define_method(vr_mq_msg_cls, "valid?",       m_is_valid,     0);
    rb_define_method(vr_mq_msg_cls, "headers",      m_get_headers,  0);
    rb_define_method(vr_mq_msg_cls, "headers=",     m_set_headers,  1);
    rb_define_method(vr_mq_msg_cls, "routing",      m_get_routing,  0);
    rb_define_method(vr_mq_msg_cls, "routing=",     m_set_routing,  1);
    rb_define_method(vr_mq_msg_cls, "content=",     m_set_content,  1);
    rb_define_method(vr_mq_msg_cls, "content",      m_get_content,  0);
    rb_define_method(vr_mq_msg_cls, "serialize",    m_serialize,    0);
    rb_define_method(vr_mq_msg_cls, "deserialize",  m_deserialize,  1);
}
