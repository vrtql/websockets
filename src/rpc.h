#ifndef VWS_RPC_DECLARE
#define VWS_RPC_DECLARE

#include "vws.h"
#include "message.h"

/**
 * @file rpc.h
 * @brief WebSocket remote procedure call implementation
 *
 * This file implements a simple RPC mechanism for message processing.
 */

/**
 * @brief Struct representing a RPC environment
 */
typedef struct vws_rpc_env
{
    /**< The data */
    char* data;

} vws_rpc_env;

/**
 * @brief Callback for RPC call
 * @param e The RPC environment
 * @param m The incoming message to process
 */
typedef vrtql_msg* (*vws_rpc_call)(vws_rpc_env* e, vrtql_msg* m);

/** Abbreviation for generic RPC map This is used to store registry of modules
 * for the module system and registry of RPC calls for each module. */
typedef struct sc_map_sv vws_rpc_map;

typedef struct vws_rpc_module
{
    /**< Module name. */
    cstr name;

    /**< Map of RPC calls. Key is call name. Value is vws_rpc_call. */
    vws_rpc_map calls;

} vws_rpc_module;

typedef struct vws_rpc_system
{
    /**< Map of RPC modules. Key is module name. Value is module instance. */
    vws_rpc_map modules;

} vws_rpc_system;

/**
 * @brief Creates a new RPC module.
 *
 * @return A new thread RPC module.
 */
vws_rpc_module* vws_rpc_module_new(cstr name);

/**
 * @brief Frees the resources allocated to a RPC module
 *
 * @param m The RPC module
 */
void vws_rpc_module_free(vws_rpc_module* m);

/**
 * @brief Adds an RPC to module.
 *
 * @param m The RPC module
 * @param n The name of the The RPC module
 * @param c The RPC call
 */
void vws_rpc_module_set(vws_rpc_module* m, cstr n, vws_rpc_call c);

/**
 * @brief Adds an RPC to module .
 *
 * @param m The RPC module
 * @param n The name of the The RPC module
 * @return c The RPC call if exists, NULL otherwise
 */
vws_rpc_call vws_rpc_module_get(vws_rpc_module* m, cstr n);

/**
 * @brief Creates a new RPC system.
 *
 * @return A new RPC system.
 */
vws_rpc_system* vws_rpc_system_new();

/**
 * @brief Frees the resources allocated to a RPC system
 *
 * @param s The RPC system
 */
void vws_rpc_system_free(vws_rpc_system* s);

/**
 * @brief Adds a module to a system.
 *
 * @param s The RPC system
 * @param m The RPC module
 */
void vws_rpc_system_set(vws_rpc_system* s, vws_rpc_module* m);

/**
 * @brief Gets a module from a system.
 *
 * @param s The RPC system
 * @param n The name of the The RPC module
 * @return m The RPC module if exists, NULL otherwise
 */
vws_rpc_module* vws_rpc_system_get(vws_rpc_system* s, cstr n);

/**
 * @brief Invoke an RPC call
 * @param c The RPC system instance
 * @param e The RPC environment
 * @param m The incoming message to process
 */
vrtql_msg* vws_rpc(vws_rpc_system* s, vws_rpc_env* e, vrtql_msg* m);

#endif /* VWS_RPC_DECLARE */
