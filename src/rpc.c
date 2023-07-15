#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpc.h"

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

typedef void (*vrtql_rpc_map_free)(void* e);

/**
 * @brief Retrieves a value from the map using a string key.
 *
 * Returns a constant pointer to the value associated with the key.
 *
 * @param map The map from which to retrieve the value.
 * @param key The string key to use for retrieval.
 * @return A constant pointer to the value associated with the key.
 */
static void* sys_map_get(vrtql_rpc_map* map, cstr key);

/**
 * @brief Sets a value in the map using a string key and value.
 *
 * It will create a new key-value pair or update the value if the key already exists.
 *
 * @param map The map in which to set the value.
 * @param key The string key to use for setting.
 * @param value The value to set.
 */
static void sys_map_set(vrtql_rpc_map* map, cstr key, void* value);

/**
 * @brief Removes a key-value pair from the map using a string key.
 *
 * If the key exists in the map, it will be removed along with its associated value.
 *
 * @param map The map from which to remove the key-value pair.
 * @param key The string key to use for removal.
 */
static void sys_map_clear(vrtql_rpc_map* map, cstr key, vrtql_rpc_map_free cb);

//------------------------------------------------------------------------------
// RPC Module
//------------------------------------------------------------------------------

vrtql_rpc_module* vrtql_rpc_module_new(cstr name)
{
    if (name == NULL)
    {
        vrtql.error(VE_RT, "module name cannot be NULL");
    }

    vrtql_rpc_module* m;

    m = (vrtql_rpc_module*)vrtql.malloc(sizeof(vrtql_rpc_module));

    m->name = strdup(name);
    sc_map_init_sv(&m->calls, 0, 0);

    return m;
}

void vrtql_rpc_module_free(vrtql_rpc_module* m)
{
    if (m != NULL)
    {
        cstr key; vrtql_rpc_call* call;
        sc_map_foreach(&m->calls, key, call)
        {
            free(key);
        }

        sc_map_term_sv(&m->calls);

        free(m->name);
        free(m);
    }
}

void vrtql_rpc_module_set(vrtql_rpc_module* m, cstr n, vrtql_rpc_call c)
{
    sys_map_set(&m->calls, n, c);
}

vrtql_rpc_call vrtql_rpc_module_get(vrtql_rpc_module* m, cstr n)
{
    return sys_map_get(&m->calls, n);
}

//------------------------------------------------------------------------------
// RPC System
//------------------------------------------------------------------------------

vrtql_rpc_system* vrtql_rpc_system_new()
{
    vrtql_rpc_system* s;
    s = (vrtql_rpc_system*)vrtql.malloc(sizeof(vrtql_rpc_system));
    sc_map_init_sv(&s->modules, 0, 0);

    return s;
}

void vrtql_rpc_system_free(vrtql_rpc_system* s)
{
    if (s != NULL)
    {
        cstr key; vrtql_rpc_module* module;
        sc_map_foreach(&s->modules, key, module)
        {
            vrtql_rpc_module_free(module);
            free(key);

            /*
            sys_map_clear( &s->modules,
                           key,
                           (vrtql_rpc_map_free)vrtql_rpc_module_free);
            */
        }

        sc_map_term_sv(&s->modules);

        free(s);
    }
}

void vrtql_rpc_system_set(vrtql_rpc_system* s, vrtql_rpc_module* m)
{
    sys_map_set(&s->modules, m->name, m);
}

vrtql_rpc_module* vrtql_rpc_system_get(vrtql_rpc_system* s, cstr n)
{
    return sys_map_get(&s->modules, n);
}

//------------------------------------------------------------------------------
// RPC API
//------------------------------------------------------------------------------

bool parse_rpc_string(const char* input, char** module, char** function)
{
    // Find the first occurrence of the period
    const char* delimiter = strchr(input, '.');

    if (delimiter != NULL)
    {
        // Calculate the lengths of the module and function substrings
        size_t m_len = delimiter - input;
        size_t f_len = strlen(input) - m_len - 1;

        // Allocate memory for the module and function strings
        *module   = (char*)vrtql.malloc((m_len + 1) * sizeof(char));
        *function = (char*)vrtql.malloc((f_len + 1) * sizeof(char));

        // Copy the module substring
        strncpy(*module, input, m_len);
        (*module)[m_len] = '\0';

        // Copy the function substring
        strncpy(*function, delimiter + 1, f_len);
        (*function)[f_len] = '\0';

        return true;
    }

    // Invalid input format
    return false;
}

vrtql_msg* vrtql_rpc(vrtql_rpc_system* s, vrtql_rpc_env* e, vrtql_msg* m)
{
    vrtql.success();

    cstr id = vrtql_msg_get_header(m, "id");

    if (id == NULL)
    {
        vrtql.error(VE_RT, "ID not specified");

        vrtql_msg_free(m);

        return NULL;
    }

    // Parse ID into module and function
    char* mn; char* fn;
    if (parse_rpc_string(id, &mn, &fn) == false)
    {
        vrtql.error(VE_RT, "Invalid ID format");

        vrtql_msg_free(m);

        return NULL;
    }

    // Lookup module in system
    vrtql_rpc_module* module = vrtql_rpc_system_get(s, mn);

    if (module == NULL)
    {
        vrtql.error(VE_RT, "RPC does not exist");

        vrtql_msg_free(m);
        free(mn);
        free(fn);

        return NULL;
    }

    // Lookup RPC in module
    vrtql_rpc_call rpc = vrtql_rpc_module_get(module, fn);

    if (rpc == NULL)
    {
        vrtql.error(VE_RT, "RPC does not exist");

        vrtql_msg_free(m);
        free(mn);
        free(fn);

        return NULL;
    }

    free(mn);
    free(fn);

    // Invoke RPC
    vrtql_msg* reply = rpc(e, m);

    // Free request
    vrtql_msg_free(m);

    // Return reply
    return reply;
}

//------------------------------------------------------------------------------
// Internal Functions
//------------------------------------------------------------------------------

void* sys_map_get(vrtql_rpc_map* map, cstr key)
{
    // See if we have an existing entry
    cstr v = sc_map_get_sv(map, key);

    if (sc_map_found(map) == false)
    {
        return NULL;
    }

    return v;
}

void sys_map_set(vrtql_rpc_map* map, cstr key, void* value)
{
    // See if we have an existing entry
    sc_map_get_sv(map, key);

    if (sc_map_found(map) == false)
    {
        // We don't. Therefore we need to allocate new key.
        key = strdup(key);
    }

    sc_map_put_sv(map, key, value);
}

void sys_map_clear(vrtql_rpc_map* map, cstr key, vrtql_rpc_map_free cb)
{
    // See if we have an existing entry
    cstr v = sc_map_get_sv(map, key);

    if (sc_map_found(map) == true)
    {
        // Call callback function to cleanup
        cb(v);
    }

    sc_map_del_sv(map, key);
}

