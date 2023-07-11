#include <stdbool.h>

#include "rb_map.h"
#include "rb_ws_common.h"
#include "rb_ws_connection.h"
#include "rb_ws_frame.h"
#include "rb_ws_message.h"
#include "rb_mq_connection.h"
#include "rb_mq_message.h"

VALUE vrtql_release_date(VALUE self);
VALUE vrtql_version(VALUE self);
VALUE vrtql_version_major(VALUE self);
VALUE vrtql_version_minor(VALUE self);
VALUE vrtql_version_patchlevel(VALUE self);
VALUE vrtql_version_candidate(VALUE self);
VALUE vrtql_version_buildnumber(VALUE self);

#ifdef __WINDOWS__
#include <winsock2.h>

// Declare the Winsock data structure. No need for a = {}.
// The data structure is large, so don't put it on the stack.
// Make it a global or static.
WSADATA wsaData;

#endif

void Init_ws()
{

#ifdef __WINDOWS__
    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
    {
        // We can't proceed with the application, consider exiting
        printf("WSAStartup failed: %d\n", rc);
    }
#endif

    VALUE vrtql_mod;
    VALUE ws_mod;

    vrtql_mod = rb_define_module("VRTQL");
    ws_mod    = rb_define_module_under(vrtql_mod, "WS");

    rb_define_const(vrtql_mod, "RELEASE_DATE", rb_str_new2(RELEASE_DATE));
    rb_define_const(vrtql_mod, "VERSION", rb_str_new2(RELEASE_VERSION));
    rb_define_const(vrtql_mod, "VERSION_MAJOR", rb_str_new2(VERSION_MAJ));
    rb_define_const(vrtql_mod, "VERSION_MINOR", rb_str_new2(VERSION_MIN));
    rb_define_const(vrtql_mod, "VERSION_PATCHLEVEL", rb_str_new2(VERSION_CL));
    rb_define_const(vrtql_mod, "VERSION_CANDIDATE", rb_str_new2(VERSION_PL));
    rb_define_const(vrtql_mod, "VERSION_BUILDNUMBER", rb_str_new2(VERSION_BN));

    init_ws_connection(ws_mod);
    init_ws_frame(ws_mod);
    init_ws_message(ws_mod);
    init_map(vrtql_mod);
    init_mq_connection(vrtql_mod);
    init_mq_message(vrtql_mod);
}
