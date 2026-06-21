#!/usr/bin/env python3
# PERMANENT regression gate (ga-fix, we1): the SSL-SIGPIPE seam.
#
# Every SSL_read/SSL_write on the SSL path MUST route through the shared
# ssl_read_nosigpipe / ssl_write_nosigpipe guards (defined in socket.c) so a
# broken-pipe SSL op cannot raise SIGPIPE in the EMBEDDED host. This gate
# asserts the ONLY raw SSL_read(/SSL_write( CALLS across socket.c + async.c are
# the two inside those guard definitions. A future edit that re-adds a raw SSL
# op -- in async.c, or anywhere else in socket.c -- bumps the count and REDs
# this gate. It strips // and /* */ comments and "..." / '...' literals first,
# so it cannot be fooled by an "SSL_read()" mention in a comment or a string.
#
# Run by CTest (test name: ssl_sigpipe_guard) and standalone:
#   python3 ssl_sigpipe_guard.py [vws/src dir]   (defaults to this file's ..)

import sys
import os


def strip(src):
    # Remove comments and string/char literals so only real tokens remain.
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        d = src[i + 1] if i + 1 < n else ''
        if c == '/' and d == '/':
            i += 2
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and d == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
        elif c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == q:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def counts(path):
    s = strip(open(path, encoding='utf-8').read())
    return s.count('SSL_read('), s.count('SSL_write(')


base = os.path.dirname(os.path.abspath(__file__))
srcdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(base, '..')
socket_c = os.path.join(srcdir, 'socket.c')
async_c = os.path.join(srcdir, 'async.c')

sr_s, sw_s = counts(socket_c)
sr_a, sw_a = counts(async_c)

ok = True

# socket.c holds exactly the two guard bodies: 1 SSL_read + 1 SSL_write.
if (sr_s, sw_s) != (1, 1):
    print("FAIL socket.c: SSL_read=%d SSL_write=%d (expected 1,1 -- only the "
          "two ssl_*_nosigpipe guard bodies)" % (sr_s, sw_s))
    ok = False

# async.c routes everything through the guards: ZERO raw SSL ops.
if (sr_a, sw_a) != (0, 0):
    print("FAIL async.c: SSL_read=%d SSL_write=%d (expected 0,0 -- must route "
          "through ssl_read_nosigpipe/ssl_write_nosigpipe)" % (sr_a, sw_a))
    ok = False

if ok:
    print("OK ssl_sigpipe_guard: raw SSL ops only in the shared guards "
          "(socket.c 1/1, async.c 0/0)")
    sys.exit(0)

print("REGRESSION: a raw SSL_read/SSL_write bypasses the shared SIGPIPE guard "
      "-- route it through ssl_read_nosigpipe / ssl_write_nosigpipe")
sys.exit(1)
