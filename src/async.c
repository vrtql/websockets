#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#endif

#if defined(__windows__)
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "async.h"

//------------------------------------------------------------------------------
// Platform shims — keep the rest of the file platform-agnostic.
//------------------------------------------------------------------------------

#if defined(__windows__)
typedef SOCKET vws_wake_fd;
#define VWS_BAD_FD INVALID_SOCKET
#define vws_poll   WSAPoll
typedef WSAPOLLFD vws_pollfd;
#else
typedef int vws_wake_fd;
#define VWS_BAD_FD (-1)
#define vws_poll   poll
typedef struct pollfd vws_pollfd;
#endif

//------------------------------------------------------------------------------
// Internal SSL/transport op result — the direction state machine reduces every
// SSL_read/SSL_write (or recv/send) to one of these.
//------------------------------------------------------------------------------

typedef enum
{
    ASYNC_OK,          /**< made progress (read delivered / write sent) */
    ASYNC_WANT_READ,   /**< needs the fd readable to continue */
    ASYNC_WANT_WRITE,  /**< needs the fd writable to continue */
    ASYNC_CLOSE,       /**< clean close (ZERO_RETURN / recv==0) */
    ASYNC_FATAL        /**< fatal SSL/socket error */
} async_result;

//------------------------------------------------------------------------------
// The loop — one connection, one poll() set (the cnx fd + a socketpair wake).
//------------------------------------------------------------------------------

struct vws_loop
{
    /**< Wake pair: the loop polls wake_rd; any thread writes wake_wr. */
    vws_wake_fd wake_rd;
    vws_wake_fd wake_wr;

    /**< The single attached reactor (NULL when none). */
    vws_async* client;

    /**< Set by vws_loop_stop (thread-safe via the wake): run() returns. Atomic
         (relaxed; the wake orders the wakeup -> the loop re-checks). */
    atomic_int stop;

    /**< Run() is active. Atomic (read cross-thread). */
    atomic_int running;
};

//------------------------------------------------------------------------------
// Wake pair — a real pollable fd-pair (not a flag): POSIX socketpair, Windows a
// self-connected loopback socket pair. Both ends non-blocking. The read end is
// FULLY DRAINED on every wakeup + handles POLLERR/HUP + spurious wakes.
//------------------------------------------------------------------------------

#if !defined(__windows__)

static bool wake_pair_make(vws_wake_fd fds[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
    {
        vws.error(VE_SOCKET, "socketpair()");
        return false;
    }

    for (int i = 0; i < 2; i++)
    {
        int fl = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
    }

    return true;
}

static void wake_pair_close(vws_wake_fd fds[2])
{
    if (fds[0] != VWS_BAD_FD) close(fds[0]);
    if (fds[1] != VWS_BAD_FD) close(fds[1]);
}

#else

// Windows: WSAPoll watches SOCKETS only — a pipe cannot be in the set — so the
// wake is a self-connected loopback TCP pair (the standard portable recipe).
static bool wake_pair_make(vws_wake_fd fds[2])
{
    fds[0] = VWS_BAD_FD;
    fds[1] = VWS_BAD_FD;

    SOCKET lsn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsn == INVALID_SOCKET)
    {
        vws.error(VE_SOCKET, "wake socket()");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    int len = sizeof(addr);

    if (bind(lsn, (struct sockaddr*)&addr, len) == SOCKET_ERROR ||
        listen(lsn, 1) == SOCKET_ERROR ||
        getsockname(lsn, (struct sockaddr*)&addr, &len) == SOCKET_ERROR)
    {
        closesocket(lsn);
        vws.error(VE_SOCKET, "wake listen/bind");
        return false;
    }

    SOCKET cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKET acc = INVALID_SOCKET;

    if (cli == INVALID_SOCKET ||
        connect(cli, (struct sockaddr*)&addr, len) == SOCKET_ERROR ||
        (acc = accept(lsn, NULL, NULL)) == INVALID_SOCKET)
    {
        if (cli != INVALID_SOCKET) closesocket(cli);
        closesocket(lsn);
        vws.error(VE_SOCKET, "wake connect/accept");
        return false;
    }

    closesocket(lsn);

    u_long nb = 1;
    ioctlsocket(acc, FIONBIO, &nb);
    ioctlsocket(cli, FIONBIO, &nb);

    fds[0] = acc;   // read end (loop)
    fds[1] = cli;   // write end (producers)

    return true;
}

static void wake_pair_close(vws_wake_fd fds[2])
{
    if (fds[0] != VWS_BAD_FD) closesocket(fds[0]);
    if (fds[1] != VWS_BAD_FD) closesocket(fds[1]);
}

#endif

static void wake_signal(vws_wake_fd wr)
{
    char b = 1;

    #if defined(__windows__)
    send(wr, &b, 1, 0);
    #else
    ssize_t n = write(wr, &b, 1);
    (void)n;
    #endif
}

static void wake_drain(vws_wake_fd rd)
{
    char buf[64];

    while (true)
    {
        #if defined(__windows__)
        int n = recv(rd, buf, (int)sizeof(buf), 0);
        #else
        ssize_t n = read(rd, buf, sizeof(buf));
        #endif

        if (n <= 0)
        {
            break;   // EAGAIN/EWOULDBLOCK -> fully drained; 0/err -> done
        }
    }
}

//------------------------------------------------------------------------------
// Non-blocking transport ops — the SSL direction reduced to async_result. The
// non-SSL path degenerates cleanly (pending is never set; recv/send + EAGAIN).
//------------------------------------------------------------------------------

static async_result async_read(vws_async* a)
{
    vws_socket* s    = a->sock;
    ucstr       data = &vws.sslbuf[0];
    ssize_t     size = (ssize_t)sizeof(vws.sslbuf);
    ssize_t     n    = 0;

    if (s->ssl != NULL)
    {
        int total = 0;

        // Drain decrypted data to EXHAUSTION (SSL_pending may hold app data the
        // raw poll() cannot see).
        while ((n = ssl_read_nosigpipe(s->ssl, data, (int)size)) > 0)
        {
            total += (int)n;
            vws_buffer_append(s->buffer, data, n);

            if (n < size && SSL_pending(s->ssl) == 0)
            {
                break;
            }
        }

        if (n <= 0)
        {
            int err = SSL_get_error(s->ssl, (int)n);

            if (err == SSL_ERROR_WANT_READ)
            {
                return ASYNC_WANT_READ;    // same-dir: drained
            }

            if (err == SSL_ERROR_WANT_WRITE)
            {
                return ASYNC_WANT_WRITE;   // inverted: renegotiation
            }

            if (err == SSL_ERROR_ZERO_RETURN)
            {
                return ASYNC_CLOSE;
            }

            if (err == SSL_ERROR_SYSCALL)
            {
                #if defined(__windows__)
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS)
                {
                    return ASYNC_WANT_READ;
                }
                #else
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                {
                    return ASYNC_WANT_READ;
                }
                #endif
            }

            return ASYNC_FATAL;
        }

        return ASYNC_OK;
    }

    // Non-SSL.
    #if defined(__linux__) || defined(__sunos__)
    n = recv(s->sockfd, data, size, MSG_NOSIGNAL);
    #elif defined(__windows__)
    n = recv(s->sockfd, (char*)data, (int)size, 0);
    #else
    n = recv(s->sockfd, data, size, 0);
    #endif

    if (n > 0)
    {
        vws_buffer_append(s->buffer, data, n);
        return ASYNC_OK;
    }

    if (n == 0)
    {
        return ASYNC_CLOSE;
    }

    #if defined(__windows__)
    if (WSAGetLastError() == WSAEWOULDBLOCK) return ASYNC_WANT_READ;
    #else
    if (errno == EWOULDBLOCK || errno == EAGAIN) return ASYNC_WANT_READ;
    #endif

    return ASYNC_FATAL;
}

// Non-blocking send of one chunk from the caller-owned buffer; `sent` gets the
// bytes accepted. (The wh handler drives this from the user out-buffer; it owns
// the buffer + offset, the reactor owns the SSL direction.) FLAGGED: the exact
// wh<->reactor split for partial/flush is a §6c contract wa3 stress-tests.
static async_result async_write(vws_async* a, ucstr data, size_t size,
                                 size_t* sent)
{
    vws_socket* s = a->sock;
    ssize_t     n = 0;

    *sent = 0;

    if (s->ssl != NULL)
    {
        n = ssl_write_nosigpipe(s->ssl, data, (int)size);

        if (n <= 0)
        {
            int err = SSL_get_error(s->ssl, (int)n);

            if (err == SSL_ERROR_WANT_WRITE)
            {
                return ASYNC_WANT_WRITE;   // same-dir: socket full
            }

            if (err == SSL_ERROR_WANT_READ)
            {
                return ASYNC_WANT_READ;    // inverted: renegotiation
            }

            if (err == SSL_ERROR_ZERO_RETURN)
            {
                return ASYNC_CLOSE;
            }

            if (err == SSL_ERROR_SYSCALL)
            {
                #if defined(__windows__)
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS)
                {
                    return ASYNC_WANT_WRITE;
                }
                #else
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                {
                    return ASYNC_WANT_WRITE;
                }
                #endif
            }

            return ASYNC_FATAL;
        }

        *sent = (size_t)n;
        return ASYNC_OK;
    }

    // Non-SSL.
    #if defined(__linux__) || defined(__sunos__)
    n = send(s->sockfd, data, size, MSG_NOSIGNAL);
    #elif defined(__windows__)
    n = send(s->sockfd, (const char*)data, (int)size, 0);
    #else
    n = send(s->sockfd, data, size, 0);
    #endif

    if (n > 0)
    {
        *sent = (size_t)n;
        return ASYNC_OK;
    }

    #if defined(__windows__)
    if (WSAGetLastError() == WSAEWOULDBLOCK) return ASYNC_WANT_WRITE;
    #else
    if (errno == EWOULDBLOCK || errno == EAGAIN) return ASYNC_WANT_WRITE;
    #endif

    return ASYNC_FATAL;
}

//------------------------------------------------------------------------------
// Reactor state transitions.
//------------------------------------------------------------------------------

static void async_disconnect(vws_async* a)
{
    if (a->live == false)
    {
        return;
    }

    a->live    = false;
    a->pending = VWS_SSL_NONE;

    // The consumer's sync reconnect path re-attaches; here we just stop driving
    // I/O. Fire the existing disconnect hook if present.
    if (a->sock->disconnect != NULL)
    {
        a->sock->disconnect(a->sock);
    }
}

// op is the SSL op that produced r (VWS_SSL_READ or VWS_SSL_WRITE).
static void apply_result(vws_async* a, vws_ssl_op op, async_result r)
{
    switch (r)
    {
        case ASYNC_OK:
            a->pending = VWS_SSL_NONE;
            break;

        case ASYNC_WANT_READ:
            if (op == VWS_SSL_WRITE)
            {
                // INVERTED (renegotiation): retry the SAME write when readable.
                a->pending  = VWS_SSL_WRITE;
                a->want_dir = POLLIN;
            }
            else
            {
                // SAME-DIR: drained / need more bytes -> re-arm read only.
                a->pending = VWS_SSL_NONE;
            }
            break;

        case ASYNC_WANT_WRITE:
            if (op == VWS_SSL_READ)
            {
                // INVERTED (renegotiation): retry the SAME read when writable.
                a->pending  = VWS_SSL_READ;
                a->want_dir = POLLOUT;
            }
            else
            {
                // SAME-DIR: socket full -> keep write armed only.
                a->pending = VWS_SSL_NONE;
            }
            break;

        case ASYNC_CLOSE:
        case ASYNC_FATAL:
            async_disconnect(a);
            break;
    }
}

//------------------------------------------------------------------------------
// Poll-mask computation — pending DOMINATES (busy-loop-safe; ga1-confirmed).
//------------------------------------------------------------------------------

static short compute_mask(vws_async* a)
{
    if (a->live == false)
    {
        return 0;
    }

    if (a->pending != VWS_SSL_NONE)
    {
        // While an SSL op is mid-flight, poll ONLY its required direction.
        return a->want_dir;
    }

    short e = 0;

    if (atomic_load_explicit(&a->read_armed,  memory_order_relaxed))
    {
        e |= POLLIN;
    }
    // ACQUIRE: see vws_socket_want_write — synchronizes the producer's enqueue
    // into the loop thread before the write handler drains it.
    if (atomic_load_explicit(&a->write_armed, memory_order_acquire))
    {
        e |= POLLOUT;
    }

    return e;
}

//------------------------------------------------------------------------------
// On-event dispatch — pending-op precedence.
//------------------------------------------------------------------------------

static void on_ready(vws_async* a, short revents)
{
    if (a->live == false)
    {
        return;
    }

    if (revents & (POLLERR | POLLNVAL))
    {
        async_disconnect(a);
        return;
    }

    // [vws V-11] POLLHUP means the peer closed. When reads are PAUSED (flow
    // control cleared read_armed), compute_mask requests no events yet poll()
    // still reports POLLHUP, so on_ready does nothing and the reactor re-polls
    // and returns immediately -- 100% CPU spin until reads resume. Disconnect.
    // When reads ARE armed, fall through so the POLLIN path drains any final
    // buffered bytes and reads EOF for an orderly close.
    if ((revents & POLLHUP) &&
        (atomic_load_explicit(&a->read_armed, memory_order_relaxed) == false))
    {
        async_disconnect(a);
        return;
    }

    // 1. Pending SSL op precedence: retry the SAME op before user callbacks.
    if (a->pending != VWS_SSL_NONE)
    {
        bool ready = (a->want_dir == POLLIN  && (revents & POLLIN)) ||
                     (a->want_dir == POLLOUT && (revents & POLLOUT));

        if (ready == true)
        {
            vws_ssl_op op = a->pending;

            if (op == VWS_SSL_READ)
            {
                async_result r = async_read(a);
                apply_result(a, op, r);

                if (a->live == true && a->pending == VWS_SSL_NONE)
                {
                    // The reneg read completed -> deliver what drained.
                    if (a->cnx != NULL)
                    {
                        vws_cnx_ingress(a->cnx);
                    }
                    if (a->rh != NULL)
                    {
                        a->rh(a->sock);
                    }
                }
            }
            else
            {
                // The reneg write retries the SAME send: the wh re-drives
                // vws_socket_async_write with its undrained out-buffer.
                if (a->wh != NULL)
                {
                    a->wh(a->sock);
                }
            }
        }

        return;   // always re-poll after touching the pending op
    }

    // 2. No pending op -> user-level dispatch (each may START a new SSL op).
    if ((revents & POLLIN) &&
        atomic_load_explicit(&a->read_armed, memory_order_relaxed))
    {
        async_result r = async_read(a);
        apply_result(a, VWS_SSL_READ, r);

        if (a->live == true)
        {
            if (a->cnx != NULL)
            {
                vws_cnx_ingress(a->cnx);   // drain to exhaustion
            }
            if (a->rh != NULL && a->pending == VWS_SSL_NONE)
            {
                a->rh(a->sock);
            }
        }
    }

    // KEEP the pending==NONE guard before starting the opposite-dir user op.
    if (a->live == true && a->pending == VWS_SSL_NONE &&
        (revents & POLLOUT) &&
        atomic_load_explicit(&a->write_armed, memory_order_relaxed))
    {
        if (a->wh != NULL)
        {
            a->wh(a->sock);   // drains out-buffer via vws_socket_async_write
        }
    }
}

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

struct vws_loop* vws_loop_new(void)
{
    struct vws_loop* loop = calloc(1, sizeof(struct vws_loop));

    if (loop == NULL)
    {
        return NULL;
    }

    vws_wake_fd fds[2] = { VWS_BAD_FD, VWS_BAD_FD };

    if (wake_pair_make(fds) == false)
    {
        free(loop);
        return NULL;
    }

    loop->wake_rd = fds[0];
    loop->wake_wr = fds[1];
    loop->client  = NULL;
    atomic_init(&loop->stop, 0);
    atomic_init(&loop->running, 0);

    return loop;
}

void vws_loop_free(struct vws_loop* loop)
{
    if (loop == NULL)
    {
        return;
    }

    vws_wake_fd fds[2] = { loop->wake_rd, loop->wake_wr };
    wake_pair_close(fds);

    free(loop);
}

bool vws_loop_attach(struct vws_loop* loop, vws_async* a,
                     vws_socket_rh rh, vws_socket_wh wh)
{
    if (loop == NULL || a == NULL || a->sock == NULL)
    {
        return false;
    }

    // Steady state is poll()-driven: the base socket goes non-blocking. The
    // synchronous vws_connect already ran on the base.
    #if defined(__windows__)
    u_long nb = 1;
    ioctlsocket(a->sock->sockfd, FIONBIO, &nb);
    #else
    int fl = fcntl(a->sock->sockfd, F_GETFL, 0);
    fcntl(a->sock->sockfd, F_SETFL, fl | O_NONBLOCK);
    #endif

    a->loop        = loop;
    a->rh          = rh;
    a->wh          = wh;
    a->pending     = VWS_SSL_NONE;
    a->want_dir    = 0;
    // a live connection wants inbound by default
    atomic_store_explicit(&a->read_armed,  true,  memory_order_relaxed);
    atomic_store_explicit(&a->write_armed, false, memory_order_relaxed);
    a->live        = true;

    loop->client = a;

    return true;
}

bool vws_loop_attach_cnx(struct vws_loop* loop, vws_acnx* a,
                         vws_socket_rh rh, vws_socket_wh wh)
{
    if (a == NULL)
    {
        return false;
    }

    a->async.sock = &a->base.base;   // vws_cnx.base is the vws_socket
    a->async.cnx  = &a->base;        // drives vws_cnx_ingress in the read path

    return vws_loop_attach(loop, &a->async, rh, wh);
}

int vws_loop_run_once(struct vws_loop* loop, int timeout_ms)
{
    if (loop == NULL)
    {
        return -VE_RT;
    }

    vws_async* a = loop->client;

    vws_pollfd fds[2];
    int        nfds = 0;

    // Slot 0: the wake fd (always polled).
    fds[nfds].fd      = loop->wake_rd;
    fds[nfds].events  = POLLIN;
    fds[nfds].revents = 0;
    nfds++;

    // Slot 1: the connection fd (its computed direction mask).
    int cslot = -1;

    if (a != NULL && a->live == true)
    {
        short mask = compute_mask(a);

        fds[nfds].fd      = a->sock->sockfd;
        fds[nfds].events  = mask;
        fds[nfds].revents = 0;
        cslot             = nfds;
        nfds++;
    }

    int rc = vws_poll(fds, nfds, timeout_ms);

    if (rc < 0)
    {
        #if defined(__windows__)
        return 0;   // retry next iteration
        #else
        if (errno == EINTR) return 0;
        return -VE_SOCKET;
        #endif
    }

    if (rc == 0)
    {
        return 0;   // timeout: idle tick
    }

    // Wake: drain fully + re-arm (a perpetually-readable wake would busy-loop).
    if (fds[0].revents & POLLIN)
    {
        wake_drain(loop->wake_rd);
    }

    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
    {
        wake_drain(loop->wake_rd);   // spurious wake-fd condition; ignore
    }

    // Connection events.
    if (cslot >= 0 && fds[cslot].revents != 0)
    {
        on_ready(a, fds[cslot].revents);
    }

    return 0;
}

int vws_loop_run(struct vws_loop* loop)
{
    if (loop == NULL)
    {
        return -VE_RT;
    }

    atomic_store_explicit(&loop->running, 1, memory_order_relaxed);
    atomic_store_explicit(&loop->stop,    0, memory_order_relaxed);

    while (atomic_load_explicit(&loop->stop, memory_order_relaxed) == 0)
    {
        int rc = vws_loop_run_once(loop, -1);

        if (rc < 0)
        {
            atomic_store_explicit(&loop->running, 0, memory_order_relaxed);
            return rc;
        }
    }

    atomic_store_explicit(&loop->running, 0, memory_order_relaxed);

    return 0;
}

void vws_socket_want_write(vws_async* a)
{
    if (a == NULL || a->loop == NULL)
    {
        return;
    }

    // RELEASE: pairs with compute_mask's ACQUIRE so the producer's enqueue
    // (sequenced before this store) happens-before the loop's drain. This makes
    // the documented "enqueue -> want_write" contract data-race-free WITHOUT
    // relying on the (C11-invisible) wake-fd ordering.
    atomic_store_explicit(&a->write_armed, true, memory_order_release);

    wake_signal(a->loop->wake_wr);
}

void vws_socket_pause_read(vws_async* a)
{
    if (a == NULL)
    {
        return;
    }

    // Loop-thread-only: compute_mask drops POLLIN next iteration. No wake needed
    // (the loop re-computes the mask).
    atomic_store_explicit(&a->read_armed, false, memory_order_relaxed);
}

void vws_socket_want_read(vws_async* a)
{
    if (a == NULL || a->loop == NULL)
    {
        return;
    }

    // Set BEFORE the wake so the loop observes it after draining the wake.
    atomic_store_explicit(&a->read_armed, true, memory_order_relaxed);

    wake_signal(a->loop->wake_wr);
}

size_t vws_socket_async_write(vws_async* a, ucstr data, size_t size)
{
    if (a == NULL || a->live == false)
    {
        return 0;
    }

    size_t sent = 0;

    // One non-blocking send attempt; the transition table owns the SSL write
    // direction (inverted WANT_READ -> pending=WRITE; same-dir/partial ->
    // no pending; CLOSE/FATAL -> disconnect).
    async_result r = async_write(a, data, size, &sent);

    apply_result(a, VWS_SSL_WRITE, r);

    return sent;
}

void vws_loop_wake(struct vws_loop* loop)
{
    if (loop == NULL)
    {
        return;
    }

    wake_signal(loop->wake_wr);
}

void vws_loop_stop(struct vws_loop* loop)
{
    if (loop == NULL)
    {
        return;
    }

    atomic_store_explicit(&loop->stop, 1, memory_order_relaxed);

    wake_signal(loop->wake_wr);
}
