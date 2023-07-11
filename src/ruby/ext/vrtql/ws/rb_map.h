#ifndef VRTQL_RUBY_MAP
#define VRTQL_RUBY_MAP

#include "rb_ws_common.h"
#include "vrtql/util/sc_map.h"

// Define the structure to hold the map pointer
typedef struct
{
    struct sc_map_str* map;
} vr_map;

// Create a new map object
VALUE vr_map_new(struct sc_map_str* map);

// Memory deallocation function for the map object
void vr_map_free(vr_map* map);

extern VALUE vr_map_cls;

void init_map(VALUE module);

#endif /* VRTQL_RUBY_MAP */
