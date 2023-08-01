#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/rand.h>

#include "rpc.h"

//------------------------------------------------------------------------------
// Internal functions
//------------------------------------------------------------------------------

static bool reconnect(vrtql_rpc* rpc)
{
    // Try to reconnect
    if (vws_reconnect(rpc->cnx) == false)
    {
        return false;
    }

    if (rpc->reconnect != NULL)
    {
        if (rpc->reconnect(rpc) == false)
        {
            return false;
        }
    }

    return true;
}

//------------------------------------------------------------------------------
// Client-side Internal functions
//------------------------------------------------------------------------------

char* vrtql_rpc_tag(uint16_t length)
{
    char valid_chars[]  = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char* data = (unsigned char*)malloc(length);
    unsigned char* tag  = (unsigned char*)malloc(length);

    if (RAND_bytes(data, length) != 1)
    {
        free(data);
        free(tag);

        return NULL;
    }

    uint16_t vc_len = strlen(valid_chars);
    for (uint16_t cnt = 0; cnt < length; cnt++)
    {
        uint16_t c = data[cnt] % vc_len;
        tag[cnt]   = valid_chars[c];
    }

    free(data);

    return (char*)tag;
}

bool vrtql_rpc_invoke(vrtql_rpc* rpc, vrtql_msg* req)
{
    // Clear previous response
    vws_buffer_clear(rpc->val);

    // Make the call
    vrtql_msg* reply = vrtql_rpc_exec(rpc, req);

    // If no response
    if (reply == NULL)
    {
        // Error already set
        return false;
    }

    // If we received content
    if (req->content->size > 0)
    {
        // Copy it
        vws_buffer_append(rpc->val, reply->content->data, reply->content->size);
    }

    // Translate response code and message
    cstr rc  = vrtql_msg_get_header(reply, "rc");
    cstr msg = vrtql_msg_get_header(reply, "msg");

    if ((rc != NULL) && (msg != NULL))
    {
        vws.error(atoi(rc), msg);
    }
    else
    {
        vws.e.code = atoi(rc);
    }

    // Cleanup
    vrtql_msg_free(req);
    vrtql_msg_free(reply);

    return true;
}

vrtql_msg* vrtql_rpc_exec(vrtql_rpc* rpc, vrtql_msg* req)
{
    //> Send request

    // Assign a tag to verify response
    cstr tag = vrtql_rpc_tag(7);
    vrtql_msg_set_routing(req, "tag", tag);

    if (vws.tracelevel >= VT_SERVICE)
    {
        vws_trace_lock();
        printf("\n\n");
        printf("+----------------------------------------------------+\n");
        printf("| Message Sent                                       |\n");
        printf("+----------------------------------------------------+\n");
        vrtql_msg_dump(req);
        printf("------------------------------------------------------\n");
        vws_trace_unlock();
    }

    // Loop until message sent or fatal error. Timeouts are ignored: keeps
    // grinding until message is fully sent or error.
    while (true)
    {
        if (vrtql_msg_send(rpc->cnx, req) > 0)
        {
            // Message successfully sent
            break;
        }

        // If connection dropped
        if (vws.e.code == VE_SOCKET)
        {
            // Try to reconnect
            if (reconnect(rpc) == true)
            {
                // Reconnect worked. Try again.
                continue;
            }

            // Failed to reconnect. Modify error to indicate the failure was on
            // send so the caller knows the message was not sent. Error will
            // have both bits set: VE_SOCKET and VE_SEND.
            vws_set_flag(&vws.e.code, VE_SEND);
        }

        // Hand error back to caller.
        free(tag);
        return NULL;
    }

    //> Wait for response

    int retries      = 0;
    vrtql_msg* reply = NULL;

    while (retries < rpc->retries)
    {
        reply = vrtql_msg_recv(rpc->cnx);

        // If we have a message
        if (reply != NULL)
        {
            // Get message tag
            cstr t = vrtql_msg_get_routing(reply, "tag");

            // If tags do not match
            if (strncmp(tag, t, strlen(tag)) != 0)
            {
                // This is not response message. Send to handler.
                rpc->out_of_band(rpc, reply);

                // Keep waiting for response.
                continue;
            }

            vws.success();

            break;
        }

        if (vws.e.code == VE_TIMEOUT)
        {
            retries++;
            continue;
        }
        else
        {
            if (vws.e.code == VE_SOCKET)
            {
                // Modify error to indicate the failure was on recv, so the
                // caller knows the message was sent. Error will have both bits
                // set: VE_SOCKET and VE_RECV.
                vws_set_flag(&vws.e.code, VE_RECV);
            }

            // Something unexpected happend. We expect vrtql_msg_recv() to set
            // appropriate error. Even if disconenct (VE_SOCKET) there is no
            // point in reconnecting because we have lost our response no matter
            // what. We will leave error as it is and hand back to caller.
            break;
        }
    }

    free(tag);

    if ((vws.tracelevel >= VT_SERVICE) && (reply != NULL))
    {
        vws_trace_lock();
        printf("\n\n");
        printf("+----------------------------------------------------+\n");
        printf("| Message Received                                   |\n");
        printf("+----------------------------------------------------+\n");
        vrtql_msg_dump(reply);
        printf("------------------------------------------------------\n");
        vws_trace_unlock();
    }

    return reply;
}

void out_of_band_default(vrtql_rpc* rpc, vrtql_msg* m)
{
    if (m != NULL)
    {
        vrtql_msg_free(m);
    }
}

//------------------------------------------------------------------------------
// Client-side API
//------------------------------------------------------------------------------

vrtql_rpc* vrtql_rpc_new(vws_cnx* cnx)
{
    vrtql_rpc* rpc   = (vrtql_rpc*)vws.malloc(sizeof(vrtql_rpc));
    rpc->cnx         = cnx;
    rpc->retries     = 5;
    rpc->out_of_band = out_of_band_default;
    rpc->reconnect   = NULL;
    rpc->data        = NULL;
    rpc->val         = vws_buffer_new();

    return rpc;
}

void vrtql_rpc_free(vrtql_rpc* rpc)
{
    if (rpc != NULL)
    {
        vws_buffer_new(rpc->val);
        vws.free(rpc);
    }
}

//------------------------------------------------------------------------------
// Server-side Internal functions
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
// Server-side API
//------------------------------------------------------------------------------

vrtql_rpc_module* vrtql_rpc_module_new(cstr name)
{
    if (name == NULL)
    {
        vws.error(VE_RT, "module name cannot be NULL");
    }

    vrtql_rpc_module* m;

    m = (vrtql_rpc_module*)vws.malloc(sizeof(vrtql_rpc_module));

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
            vws.free(key);
        }

        sc_map_term_sv(&m->calls);

        vws.free(m->name);
        vws.free(m);
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
    s = (vrtql_rpc_system*)vws.malloc(sizeof(vrtql_rpc_system));
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
            vws.free(key);

            /*
            sys_map_clear( &s->modules,
                           key,
                           (vrtql_rpc_map_free)vrtql_rpc_module_free);
            */
        }

        sc_map_term_sv(&s->modules);

        vws.free(s);
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
        *module   = (char*)vws.malloc((m_len + 1) * sizeof(char));
        *function = (char*)vws.malloc((f_len + 1) * sizeof(char));

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

vrtql_msg* vrtql_rpc_service(vrtql_rpc_system* s, vrtql_rpc_env* e, vrtql_msg* m)
{
    vws.success();

    cstr id = vrtql_msg_get_header(m, "id");

    if (id == NULL)
    {
        vws.error(VE_RT, "ID not specified");

        vrtql_msg_free(m);

        return NULL;
    }

    // Parse ID into module and function
    char* mn; char* fn;
    if (parse_rpc_string(id, &mn, &fn) == false)
    {
        vws.error(VE_RT, "Invalid ID format");

        vrtql_msg_free(m);

        return NULL;
    }

    // Lookup module in system
    vrtql_rpc_module* module = vrtql_rpc_system_get(s, mn);

    if (module == NULL)
    {
        vws.error(VE_RT, "RPC does not exist");

        vrtql_msg_free(m);
        vws.free(mn);
        vws.free(fn);

        return NULL;
    }

    // Lookup RPC in module
    vrtql_rpc_call rpc = vrtql_rpc_module_get(module, fn);

    if (rpc == NULL)
    {
        vws.error(VE_RT, "RPC does not exist");

        vrtql_msg_free(m);
        vws.free(mn);
        vws.free(fn);

        return NULL;
    }

    vws.free(mn);
    vws.free(fn);

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

