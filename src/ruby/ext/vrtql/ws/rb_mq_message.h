#ifndef VRTQL_RUBY_MESSAGE
#define VRTQL_RUBY_MESSAGE

#include "rb_ws_common.h"

#include "vrtql/websocket.h"
#include "vrtql/message.h"

// Define the structure to hold the vws_msg pointer
typedef struct
{
    vrtql_msg* msg;
} vr_mq_msg;

// Create a new message object
VALUE vr_mq_msg_new(vrtql_msg* msg);

// Memory deallocation function for the vr_mq_msg object
void vr_mq_msg_free(vr_mq_msg* msg);

// Get C object from VALUJE
vrtql_msg* vr_mq_get_object(VALUE obj);

extern VALUE vr_mq_msg_cls;

void init_mq_message(VALUE module);

#endif /* VRTQL_RUBY_MESSAGE */
