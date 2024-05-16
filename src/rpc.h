#ifndef VRTQL_RPC_DECLARE
#define VRTQL_RPC_DECLARE

#include "message.h"

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Client Side
//------------------------------------------------------------------------------

/**
 * @file rpc.h
 * @brief WebSocket remote procedure call implementation
 *
 * This file implements a simple RPC mechanism for message processing.
 */

struct vrtql_rpc;


/**
 * @brief Callback for out-of-band messages received during RPC call
 * @param e The RPC environment
 * @param m The incoming message to process
 */
typedef void (*vrtql_rpc_ob)(struct vrtql_rpc* rpc, vrtql_msg* m);

/**
 * @brief Callback for reconnect. This is called after successful websocket
 *        reconnection.
 * @param e The RPC environment
 * @return Returns true on successful reconnect, false otherwise.
 */
typedef bool (*vrtql_rpc_reconnect)(struct vrtql_rpc* rpc);

/**
 * @brief Struct representing a RPC environment
 */
typedef struct vrtql_rpc
{
    /**< WebSocket connection */
    vws_cnx* cnx;

    /**< The number of retries of timeout occurs (default 5) */
    uint8_t retries;

    /**> User-defined handler for out-of-band messages (default delete) */
    vrtql_rpc_ob out_of_band;

    /**> User-defined handler for reconnect */
    vrtql_rpc_reconnect reconnect;

    /**< Data from last response */
    vws_buffer* val;

    /**> User-defined data*/
    void* data;

} vrtql_rpc;

/**
 * @brief Creates a new RPC module.
 *
 * @param cnx The WebSocket connection to use
 * @return A new thread RPC module.
 */
vrtql_rpc* vrtql_rpc_new(vws_cnx* cnx);

/**
 * @brief Frees the resources allocated to a RPC module
 *
 * @param rpc The RPC instance
 */
void vrtql_rpc_free(vrtql_rpc* rpc);

/**
 * @brief Creates random tag for message identification
 *
 * @param length The size (characters) of the tag to be generated
 * @return A C string (caller from free());
 */
char* vrtql_rpc_tag(uint16_t length);

/**
 * @brief Low-level RPC call invocation. This takes a message as input, sends
 * and waits for a response.
 *
 * @param rpc The RPC instance
 * @param req The message to send
 * @return The response message on success, NULL otherwise. The caller must free
 *         message with vrtql_msg_free(). If connection fails, error will have
 *         VE_SOCKET along with either VE_SEND if failure was on send or VE_RECV
 *         if failure was on receive. Caller can check by using
 *         vws_is_flag(&vws.e, VE_SEND) or vws_is_flag(&vws.e, VE_RECV).
 */
vrtql_msg* vrtql_rpc_exec(vrtql_rpc* rpc, vrtql_msg* req);

/**
 * @brief RPC call invocation. This takes a message as input, sends and waits
 * for a response. It translates the response's "rc" and "msg" attributes to the
 * vws.e.code and vws.e.text values respectively. The message content, if any,
 * is copied into the rpc->val member.
 *
 * @param rpc The RPC instance
 * @param req The message to send. This is automatically freed. Caller should
 *        NOT use this message again.
 * @return True if the RPC call received a response. False otherwise. Call
 *         information is copies into vws.e. If connection fails, error will
 *         have VE_SOCKET along with either VE_SEND if failure was on send or
 *         VE_RECV if failure was on receive. Caller can check by using
 *         vws_is_flag(&vws.e, VE_SEND) or vws_is_flag(&vws.e, VE_RECV).
 */
bool vrtql_rpc_invoke(vrtql_rpc* rpc, vrtql_msg* req);

//------------------------------------------------------------------------------
// Server Side
//------------------------------------------------------------------------------

/** Abbreviation for generic RPC map This is used to store registry of modules
 * for the module system and registry of RPC calls for each module. */
typedef struct sc_map_sv vrtql_rpc_map;

typedef struct vrtql_rpc_module
{
    /**< Module name. */
    cstr name;

    /**< Map of RPC calls. Key is call name. Value is vrtql_rpc_call. */
    vrtql_rpc_map calls;

    /**> User-defined data*/
    void* data;

} vrtql_rpc_module;

/**
 * @brief Struct representing a RPC environment
 */
typedef struct vrtql_rpc_env
{
    /**< The user-defined data */
    void* data;

    /**< Reference to current module */
    vrtql_rpc_module* module;

} vrtql_rpc_env;

/**
 * @brief Callback for RPC call
 * @param e The RPC environment
 * @param m The incoming message to process
 */
typedef vrtql_msg* (*vrtql_rpc_call)(vrtql_rpc_env* e, vrtql_msg* m);

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
 * @brief Creates and initializes reply message for RPC call
 * @param m The incoming message to process
 * @return A reply message. User takes ownership of memory and must free.
 */
vrtql_msg* vrtql_rpc_reply(vrtql_msg* req);

/**
 * @brief Service an RPC call
 * @param c The RPC system instance
 * @param e The RPC environment
 * @param m The incoming message to process
 */
vrtql_msg* vrtql_rpc_service(vrtql_rpc_system* s, vrtql_rpc_env* e, vrtql_msg* m);

#ifdef __cplusplus
}
#endif

#endif /* VRTQL_RPC_DECLARE */
