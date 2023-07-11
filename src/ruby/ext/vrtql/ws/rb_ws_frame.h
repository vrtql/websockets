#ifndef VRTQL_RUBY_WEBSOCKET_FRAME
#define VRTQL_RUBY_WEBSOCKET_FRAME

#include "rb_ws_common.h"

#include "vrtql/websocket.h"

// Define the structure to hold the vws_frame pointer
typedef struct
{
    vws_frame* frame;
} vr_ws_frame;

// Create a new frame object
VALUE vr_ws_frame_new(vws_frame* frame);

// Memory deallocation function for the vr_ws_frame object
void vr_ws_frame_free(vr_ws_frame* frame);

extern VALUE vr_ws_frame_cls;

void init_ws_frame(VALUE module);

#endif /* VRTQL_RUBY_WEBSOCKET_FRAME */
