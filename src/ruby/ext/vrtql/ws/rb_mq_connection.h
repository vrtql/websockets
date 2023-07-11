#ifndef VRTQL_RUBY_CONNECTION
#define VRTQL_RUBY_CONNECTION

#include "rb_ws_connection.h"

typedef struct
{
    vr_ws_cnx base;
    int default_format;
} vr_mq_cnx;

extern VALUE vr_mq_cnx_cls;

void init_mq_connection(VALUE module);

#endif
