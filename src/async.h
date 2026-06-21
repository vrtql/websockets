#ifndef VWS_ASYNC_DECLARE
#define VWS_ASYNC_DECLARE

/*
 * C11 atomics in a struct that a C++ TU may include (the parked Channel wrapper
 * over the reactor). `_Atomic`/atomic_bool/<stdatomic.h> are C-only; present
 * std::atomic<T> to C++. Layout-compatible for these integral T on the supported
 * toolchains. See server.h for the same shim.
 */
#if defined(__cplusplus)
#include <atomic>
#define VWS_ATOMIC(T) ::std::atomic<T>
#else
#include <stdatomic.h>
#define VWS_ATOMIC(T) _Atomic T
#endif

#include "socket.h"
#include "websocket.h"

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Async client reactor
//
// A scaled-down, single-connection, poll()-driven client event loop — a small
// slice of the libuv pattern (the server's loop), with no libuv dependency.
// It drives ONE non-blocking connection's steady-state I/O: it hides the SSL
// read/write direction (WANT_READ/WANT_WRITE) and the WebSocket framing behind
// read-ready / write-ready handlers, and a socketpair wake lets a producer
// thread inject an outbound send.
//
// New types EMBED the existing structs as their first member (the same C
// inheritance idiom vws_cnx already uses: `struct vws_socket base`). All async
// state lives on the extension; vws_socket / vws_cnx and their source files are
// byte-unchanged, so every existing function (vws_connect, vws_socket_read/
// _write, vws_cnx_ingress, vws_msg_send) works on the embedded base verbatim.
//
// Threading: the loop thread owns ALL I/O and runs ALL handlers. The ONLY
// thread-safe entry points are vws_socket_want_write / vws_socket_want_read /
// vws_loop_wake / vws_loop_stop; everything else is loop-thread-only. The
// producer sequence is enqueue → vws_socket_want_write → (implicit wake).
//------------------------------------------------------------------------------

/**
 * @brief Read-ready handler. Called on the loop thread when post-SSL bytes are
 *        in the connection buffer (socket level), or — for the ws extension —
 *        after vws_cnx_ingress has delivered complete frames. Consume from the
 *        connection buffer; never blocks; never sees SSL WANT_*.
 *
 * @param s The socket (the embedded base of the attached extension)
 */
typedef void (*vws_socket_rh)(struct vws_socket* s);

/**
 * @brief Write-ready handler. Called on the loop thread when the socket is
 *        writable AND a write is armed (vws_socket_want_write). Drain the user
 *        out-buffer via the existing send path; the reactor honors the socket
 *        flush flag and the SSL write direction.
 *
 * @param s The socket (the embedded base of the attached extension)
 */
typedef void (*vws_socket_wh)(struct vws_socket* s);

/* On-disconnect reuses the existing vws_socket_dh / vws_cnx_disconnect. */

/**
 * @brief Which SSL operation is mid-flight (returned WANT and must be retried
 *        with the SAME op on the opposite poll direction during a TLS
 *        renegotiation). NONE in steady state and on every non-SSL connection.
 */
typedef enum
{
    VWS_SSL_NONE = 0,   /**< no SSL op pending (steady state / non-SSL) */
    VWS_SSL_READ,       /**< an SSL_read pending a direction (reneg) */
    VWS_SSL_WRITE       /**< an SSL_write pending a direction (reneg) */
} vws_ssl_op;

struct vws_loop;

/**
 * @brief The reactor state for one connection. Lives on the extension (never on
 *        the base); loop-thread-only except where a function is documented
 *        thread-safe. `sock` is the managed base; `cnx` is non-NULL for the ws
 *        extension (drives vws_cnx_ingress in the read dispatch), NULL for raw.
 */
typedef struct vws_async
{
    /**< The base socket this reactor manages (the embedded base). */
    vws_socket* sock;

    /**< Non-NULL for the ws extension: drives vws_cnx_ingress on read. */
    vws_cnx* cnx;

    /**< The loop this reactor is attached to (NULL when detached). */
    struct vws_loop* loop;

    /**< Read-ready handler (loop-thread). */
    vws_socket_rh rh;

    /**< Write-ready handler (loop-thread). */
    vws_socket_wh wh;

    /**< Which SSL op is mid-flight; VWS_SSL_NONE in steady state. */
    vws_ssl_op pending;

    /**< Poll direction the pending op needs (POLLIN|POLLOUT); valid only while
         pending != VWS_SSL_NONE. */
    short want_dir;

    /**< Reactor wants inbound (POLLIN); cleared by vws_socket_pause_read for
         read-backpressure, re-armed by vws_socket_want_read. Atomic: written
         cross-thread by vws_socket_want_read (relaxed; the wake orders it). */
    VWS_ATOMIC(bool) read_armed;

    /**< User out-buffer has data (POLLOUT interest); set cross-thread by
         vws_socket_want_write. Atomic (relaxed; the wake orders it). */
    VWS_ATOMIC(bool) write_armed;

    /**< Attached + live (cleared on disconnect; stops re-arming/handlers). */
    bool live;

} vws_async;

/**
 * @brief Socket-level async extension: a non-blocking base socket + the reactor
 *        state. For raw TCP/SSL consumers.
 */
typedef struct asocket
{
    /**< The embedded base — every vws_socket_* function runs on &a->base. */
    vws_socket base;

    /**< The reactor state (loop-thread-only). */
    vws_async async;

} asocket;

/**
 * @brief WebSocket-level async extension: a vws_cnx (framing) + the reactor
 *        state. vws_loop_attach_cnx wires the read dispatch to vws_cnx_ingress.
 */
typedef struct vws_acnx
{
    /**< The embedded base — every vws_cnx_* / vws_socket_* function runs on
         &a->base (and &a->base.base for the socket). */
    vws_cnx base;

    /**< The reactor state (loop-thread-only). */
    vws_async async;

} vws_acnx;

/**
 * @defgroup AsyncFunctions
 *
 * @brief A scaled-down, single-connection, poll()-driven client event loop.
 */

/**
 * @brief Allocates a new loop.
 *
 * @return A new loop, or NULL on failure (e.g. the wake pair failed).
 *
 * @ingroup AsyncFunctions
 */
struct vws_loop* vws_loop_new(void);

/**
 * @brief Deallocates a loop (must be stopped + detached first).
 *
 * @param loop The loop
 *
 * @ingroup AsyncFunctions
 */
void vws_loop_free(struct vws_loop* loop);

/**
 * @brief Attaches an ALREADY-CONNECTED reactor (post vws_connect) + its
 *        handlers: sets the base non-blocking and registers it in the poll set.
 *
 * @param loop The loop
 * @param a    The reactor (the extension's async member, e.g. &asocket.async)
 * @param rh   Read-ready handler
 * @param wh   Write-ready handler (may be NULL until a write is armed)
 * @return true on success
 *
 * @ingroup AsyncFunctions
 */
bool vws_loop_attach(struct vws_loop* loop, vws_async* a,
                     vws_socket_rh rh, vws_socket_wh wh);

/**
 * @brief ws-level attach: wires a->async.cnx + runs vws_cnx_ingress in the read
 *        dispatch (the ws consumer's path), then vws_loop_attach.
 *
 * @param loop The loop
 * @param a    The ws reactor extension
 * @param rh   Read-ready handler (post-ingress)
 * @param wh   Write-ready handler
 * @return true on success
 *
 * @ingroup AsyncFunctions
 */
bool vws_loop_attach_cnx(struct vws_loop* loop, vws_acnx* a,
                         vws_socket_rh rh, vws_socket_wh wh);

/**
 * @brief Runs the loop on the caller's thread until vws_loop_stop().
 *
 * @param loop The loop
 * @return 0 on a clean stop; a negative VE_* on error.
 *
 * @ingroup AsyncFunctions
 */
int vws_loop_run(struct vws_loop* loop);

/**
 * @brief Runs one poll iteration with a timeout (the caller owns the thread).
 *
 * @param loop       The loop
 * @param timeout_ms poll() timeout in ms (<0 blocks until an event)
 * @return 0 on a clean iteration; a negative VE_* on error.
 *
 * @ingroup AsyncFunctions
 */
int vws_loop_run_once(struct vws_loop* loop, int timeout_ms);

/**
 * @brief Thread-safe: arm POLLOUT for the next iteration + wake the loop. The
 *        ONLY way an outside thread injects an outbound send.
 *
 * @param a The reactor (the producer holds the extension)
 *
 * @ingroup AsyncFunctions
 */
void vws_socket_want_write(vws_async* a);

/**
 * @brief Loop-thread-only (call from the read handler): apply read-backpressure
 *        by clearing read_armed, so compute_mask stops polling POLLIN and no
 *        further reads are dispatched until vws_socket_want_read re-arms. The
 *        read-side mirror of how the write handler clears write_armed when its
 *        out-buffer drains.
 *
 * @param a The reactor handle
 *
 * @ingroup AsyncFunctions
 */
void vws_socket_pause_read(vws_async* a);

/**
 * @brief Thread-safe: re-arm POLLIN + wake the loop — resume inbound delivery
 *        after a vws_socket_pause_read. The read-side mirror of
 *        vws_socket_want_write.
 *
 * @param a The reactor handle
 *
 * @ingroup AsyncFunctions
 */
void vws_socket_want_read(vws_async* a);

/**
 * @brief Loop-thread-only (call from the write handler): non-blocking send of
 *        one chunk through the reactor's SSL write direction. Unlike the
 *        blocking vws_socket_write, this performs a SINGLE send attempt and
 *        returns the bytes accepted (0 on WANT / pending-reneg / closed),
 *        driving the §6c partial/flush contract: on an inverted (renegotiation)
 *        WANT_READ it sets pending=WRITE and retries the SAME send via the
 *        handler on the opposite direction; on a same-direction WANT (socket
 *        full) or a partial send it keeps no pending; the caller re-arms. The
 *        caller drains ONLY the returned count and keeps the out-buffer
 *        write armed while more remains, so the remainder is never lost.
 *
 * @param a    The reactor handle
 * @param data The bytes to send
 * @param size The number of bytes
 * @return The bytes accepted this attempt (0 if none).
 *
 * @ingroup AsyncFunctions
 */
size_t vws_socket_async_write(vws_async* a, ucstr data, size_t size);

/**
 * @brief Thread-safe: wake a blocked poll() (the uv_async_send equivalent).
 *
 * @param loop The loop
 *
 * @ingroup AsyncFunctions
 */
void vws_loop_wake(struct vws_loop* loop);

/**
 * @brief Thread-safe: wake + cause vws_loop_run() to return.
 *
 * @param loop The loop
 *
 * @ingroup AsyncFunctions
 */
void vws_loop_stop(struct vws_loop* loop);

#ifdef __cplusplus
}
#endif

#endif /* VWS_ASYNC_DECLARE */
