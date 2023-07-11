#ifndef VRTQL_RUBY_WEBSOCKET_CONNECTION
#define VRTQL_RUBY_WEBSOCKET_CONNECTION

#include "vrtql/websocket.h"

typedef struct
{
    vws_cnx* cnx;
} vr_ws_cnx;

extern VALUE vr_ws_cnx_cls;

void init_ws_connection(VALUE module);

#endif
