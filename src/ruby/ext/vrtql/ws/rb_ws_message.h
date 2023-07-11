#ifndef VRTQL_RUBY_WEBSOCKET_MESSAGE
#define VRTQL_RUBY_WEBSOCKET_MESSAGE

#include "rb_ws_common.h"

#include "vrtql/websocket.h"

// Define the structure to hold the vws_msg pointer
typedef struct
{
    vws_msg* msg;
} vr_ws_msg;

// Create a new message object
VALUE vr_ws_msg_new(vws_msg* msg);

// Memory deallocation function for the vr_ws_msg object
void vr_ws_msg_free(vr_ws_msg* msg);

extern VALUE vr_ws_msg_cls;

void init_ws_message(VALUE module);

#endif /* VRTQL_RUBY_WEBSOCKET_MESSAGE */
