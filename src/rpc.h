#ifndef VRTQL_RPC_DECLARE
#define VRTQL_RPC_DECLARE

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
typedef struct vrtql_rpc_env
{
    /**< The data */
    char* data;

} vrtql_rpc_env;

/**
 * @brief Callback for RPC call
 * @param e The RPC environment
 * @param m The incoming message to process
 */
typedef vrtql_msg* (*vrtql_rpc_call)(vrtql_rpc_env* e, vrtql_msg* m);

/** Abbreviation for generic RPC map This is used to store registry of modules
 * for the module system and registry of RPC calls for each module. */
typedef struct sc_map_sv vrtql_rpc_map;

typedef struct vrtql_rpc_module
{
    /**< Module name. */
    cstr name;

    /**< Map of RPC calls. Key is call name. Value is vrtql_rpc_call. */
    vrtql_rpc_map calls;

} vrtql_rpc_module;

typedef struct vrtql_rpc_system
{
    /**< Map of RPC modules. Key is module name. Value is module instance. */
    vrtql_rpc_map modules;

} vrtql_rpc_system;

/**
 * @brief Creates a new RPC module.
 *
 * @return A new thread RPC module.
 */
vrtql_rpc_module* vrtql_rpc_module_new(cstr name);

/**
 * @brief Frees the resources allocated to a RPC module
 *
 * @param m The RPC module
 */
void vrtql_rpc_module_free(vrtql_rpc_module* m);

/**
 * @brief Adds an RPC to module.
 *
 * @param m The RPC module
 * @param n The name of the The RPC module
 * @param c The RPC call
 */
void vrtql_rpc_module_set(vrtql_rpc_module* m, cstr n, vrtql_rpc_call c);

/**
 * @brief Adds an RPC to module .
 *
 * @param m The RPC module
 * @param n The name of the The RPC module
 * @return c The RPC call if exists, NULL otherwise
 */
vrtql_rpc_call vrtql_rpc_module_get(vrtql_rpc_module* m, cstr n);

/**
 * @brief Creates a new RPC system.
 *
 * @return A new RPC system.
 */
vrtql_rpc_system* vrtql_rpc_system_new();

/**
 * @brief Frees the resources allocated to a RPC system
 *
 * @param s The RPC system
 */
void vrtql_rpc_system_free(vrtql_rpc_system* s);

/**
 * @brief Adds a module to a system.
 *
 * @param s The RPC system
 * @param m The RPC module
 */
void vrtql_rpc_system_set(vrtql_rpc_system* s, vrtql_rpc_module* m);

/**
 * @brief Gets a module from a system.
 *
 * @param s The RPC system
 * @param n The name of the The RPC module
 * @return m The RPC module if exists, NULL otherwise
 */
vrtql_rpc_module* vrtql_rpc_system_get(vrtql_rpc_system* s, cstr n);

/**
 * @brief Invoke an RPC call
 * @param c The RPC system instance
 * @param e The RPC environment
 * @param m The incoming message to process
 */
vrtql_msg* vrtql_rpc(vrtql_rpc_system* s, vrtql_rpc_env* e, vrtql_msg* m);

#endif /* VRTQL_RPC_DECLARE */
