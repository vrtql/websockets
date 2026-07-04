SET(CMAKE_BUILD_TYPE "Debug")

#-------------------------------------------------------------------------------
# Test programs
#-------------------------------------------------------------------------------

# Create project C/C++ configuration file
CONFIGURE_FILE( ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
                ${CMAKE_CURRENT_BINARY_DIR}/config.h )

# Common code needed for tests
SET(testlib_sources common.c)

# Static Library
add_library(static_test_lib STATIC ${testlib_sources})
set_target_properties(static_test_lib PROPERTIES
  OUTPUT_NAME ${PROJECT_NAME}_test )

add_dependencies(static_test_lib static_lib)

# A list of all targets. A target is exactly one source file which is compiled
# into one standalone test program. e.g:
#
# test_net -> test_net.c -> test_net (exe)

set(test_targets
  test_common
  test_example
  test_json
  test_rpc_server
  test_rpc_client
  test_websocket )

if(BUILD_SERVER)
  list(APPEND test_targets
    test_http_request
    test_server
    test_peering
    test_http_server
    test_ws_server
    test_inetd_server
    test_msg_server )
endif()

foreach(x ${test_targets})
  add_executable(${x} ${x}.c)
  target_include_directories(${x} PRIVATE ${PREFIX}/include)
  target_link_libraries(${x} PRIVATE static_test_lib static_lib ${OS_LIBS} -lm)

  if(WIN32)
    target_link_libraries(${x} PRIVATE ws2_32)
  endif()

  add_test(NAME ${x} COMMAND ${x})

  add_custom_target(${x}_leakcheck
    COMMAND valgrind --leak-check=full --show-reachable=yes
            --suppressions=valgrind.suppress ./${x} )
  add_dependencies(${x} static_test_lib)
  add_dependencies(${x} static_lib)
endforeach(x)

# The async reactor test needs the shared SSL test harness (ssl_test_util.c:
# D1 localhost-cert trust + the BIO-pair / real-fd TLS control servers) and the
# mongoose ws/wss echo peer, so it builds outside the single-source loop above.
add_executable(test_async test_async.c ssl_test_util.c util/mongoose.c)
target_include_directories(test_async PRIVATE ${PREFIX}/include)
target_link_libraries(test_async PRIVATE static_test_lib static_lib ${OS_LIBS} -lm)
# Fault injection (literal-100% coverage of async.c's error/defensive arms): the
# test binary wraps libc primitives; arms are one-shot + default pass-through.
target_link_options(test_async PRIVATE
  -Wl,--wrap=calloc -Wl,--wrap=socketpair
  -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=poll)

if(WIN32)
  target_link_libraries(test_async PRIVATE ws2_32)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(test_async PRIVATE -Wno-error=implicit-function-declaration)
endif()

add_test(NAME test_async COMMAND test_async)
add_dependencies(test_async static_test_lib static_lib)

add_custom_target(test_async_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_async )

# Sync socket.c SSL coverage — reuses the shared SSL harness (no mongoose).
add_executable(test_socket_ssl test_socket_ssl.c ssl_test_util.c)
target_include_directories(test_socket_ssl PRIVATE ${PREFIX}/include)
target_link_libraries(test_socket_ssl PRIVATE static_test_lib static_lib ${OS_LIBS} -lm)
target_link_options(test_socket_ssl PRIVATE -Wl,--wrap=poll)

if(WIN32)
  target_link_libraries(test_socket_ssl PRIVATE ws2_32)
endif()

add_test(NAME test_socket_ssl COMMAND test_socket_ssl)
add_dependencies(test_socket_ssl static_test_lib static_lib)

add_custom_target(test_socket_ssl_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_socket_ssl )

# PERMANENT regression gate (ga-fix, we1): the SSL-SIGPIPE seam. Asserts every
# raw SSL_read/SSL_write in socket.c + async.c lives ONLY inside the shared
# ssl_read_nosigpipe / ssl_write_nosigpipe guards; FAILS THE BUILD if a future
# edit re-adds a raw SSL op that bypasses the SIGPIPE guard. Comment/string
# robust. This project has no enable_testing(), so add_test alone would be inert
# -- the keeper is a build-time custom target (ALL) that the SSL test targets
# depend on, so a regression fails `make`. add_test is also registered for
# CTest-enabled CI.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_custom_target(ssl_sigpipe_guard_check ALL
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/ssl_sigpipe_guard.py
            ${CMAKE_CURRENT_SOURCE_DIR}/..
    COMMENT "ga-fix gate: SSL-SIGPIPE seam (no raw SSL ops outside the guards)")
  add_dependencies(test_socket_ssl ssl_sigpipe_guard_check)
  add_dependencies(test_async ssl_sigpipe_guard_check)
  add_test(NAME ssl_sigpipe_guard
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/ssl_sigpipe_guard.py
                   ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()

# url.c coverage (item-4 BATCH-1). test_url.c #includes url.c directly (reaches
# the static helpers + isolates coverage to url.c), so it must NOT link
# static_lib -- that would duplicate url's symbols. url.c is self-contained
# (pure libc). One-shot ld --wrap on malloc/calloc drives the allocation-failure
# arms.
add_executable(test_url test_url.c)
target_include_directories(test_url PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_options(test_url PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc)
target_link_libraries(test_url PRIVATE ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_url PRIVATE ws2_32)
endif()

add_test(NAME test_url COMMAND test_url)

# regression guard. url_get_protocol must
# not overflow its fixed 32-byte buffer on a long scheme prefix. Built with ASan
# and url.c compiled in-tree so the buffer is an instrumented allocation; on the
# buggy source ASan aborts on the overflow write, on the fixed source it exits 0.
add_executable(test_url_overflow test_url_overflow.c ../url.c)
target_include_directories(test_url_overflow PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_compile_options(test_url_overflow PRIVATE -fsanitize=address)
target_link_options(test_url_overflow PRIVATE -fsanitize=address)

add_test(NAME test_url_overflow COMMAND test_url_overflow)

add_custom_target(test_url_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_url )

# http_message.c coverage (item-4 BATCH-1, module 2). test_http_message.c
# #includes http_message.c (reaches the static callbacks + process_header/lcase
# + isolates coverage to http_message.c); the #included symbols shadow
# static_lib's http_message.o (never pulled), so no duplicate definition and
# libvws stays non-instrumented. One-shot --wrap=llhttp_execute forces the
# abnormal-pause arm (a real WS upgrade does not reliably produce it).
add_executable(test_http_message test_http_message.c)
target_include_directories(test_http_message PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_options(test_http_message PRIVATE -Wl,--wrap=llhttp_execute)
target_link_libraries(test_http_message PRIVATE
  static_test_lib static_lib ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_http_message PRIVATE ws2_32)
endif()

add_test(NAME test_http_message COMMAND test_http_message)

add_custom_target(test_http_message_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_http_message )
add_dependencies(test_http_message static_test_lib static_lib)

# message.c coverage (item-4 BATCH-1, module 3). test_message.c #includes
# message.c (reaches the static mpack parsers + isolates coverage); the
# #included symbols shadow static_lib's message.o (never pulled). One-shot
# --wrap drives the writer/reader-destroy + frame-send + msg-recv arms.
add_executable(test_message test_message.c)
target_include_directories(test_message PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_options(test_message PRIVATE
  -Wl,--wrap=mpack_writer_destroy -Wl,--wrap=mpack_reader_destroy
  -Wl,--wrap=mpack_expect_array
  -Wl,--wrap=vws_frame_send_binary -Wl,--wrap=vws_msg_recv)
target_link_libraries(test_message PRIVATE
  static_test_lib static_lib ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_message PRIVATE ws2_32)
endif()

add_test(NAME test_message COMMAND test_message)

# regression guard. vrtql_msg_deserialize
# must reject a JSON message with a non-string routing/header value instead of
# crashing on strlen(NULL) in vws_kvs_set_cstring.
add_executable(test_msg_json_nonstr test_msg_json_nonstr.c)
target_include_directories(test_msg_json_nonstr PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_libraries(test_msg_json_nonstr PRIVATE static_lib ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_msg_json_nonstr PRIVATE ws2_32)
endif()

add_test(NAME test_msg_json_nonstr COMMAND test_msg_json_nonstr)

add_custom_target(test_message_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_message )
add_dependencies(test_message static_test_lib static_lib)

# websocket.c NON-SSL coverage (item-4 BATCH-1, module 4). test_ws_frame.c
# #includes websocket.c (statics + isolated coverage); the #included symbols
# shadow static_lib's websocket.o (never pulled). One-shot --wrap on RAND_bytes
# (masking + key gen) and vws_socket_write (the process_frame + send responses).
# Covers the non-transport surface (framing / dispatch / msg assembly /
# generators / key helpers); the SSL/socket transport is out of scope.
add_executable(test_ws_frame test_ws_frame.c)
target_include_directories(test_ws_frame PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_options(test_ws_frame PRIVATE
  -Wl,--wrap=RAND_bytes -Wl,--wrap=vws_socket_write)
target_link_libraries(test_ws_frame PRIVATE
  static_test_lib static_lib ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_ws_frame PRIVATE ws2_32)
endif()

add_test(NAME test_ws_frame COMMAND test_ws_frame)

add_custom_target(test_ws_frame_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_ws_frame )
add_dependencies(test_ws_frame static_test_lib static_lib)

# rpc.c coverage (item-4 BATCH-1, module 5). test_rpc_cov.c #includes rpc.c
# (statics + isolated coverage); the #included symbols shadow static_lib's
# rpc.o. The network is removed by --wrap on the message layer (vrtql_msg_send/
# recv + vws_reconnect) so exec/invoke/dispatch run deterministically.
add_executable(test_rpc_cov test_rpc_cov.c)
target_include_directories(test_rpc_cov PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_options(test_rpc_cov PRIVATE
  -Wl,--wrap=RAND_bytes -Wl,--wrap=vrtql_msg_send
  -Wl,--wrap=vrtql_msg_recv -Wl,--wrap=vws_reconnect)
target_link_libraries(test_rpc_cov PRIVATE
  static_test_lib static_lib ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_rpc_cov PRIVATE ws2_32)
endif()

add_test(NAME test_rpc_cov COMMAND test_rpc_cov)

# regression guard. vrtql_rpc_exec must
# not crash on a reply carrying no tag routing entry; the recv wrap returns one
# tagless reply then times out so the fixed retry loop drains and returns NULL.
add_executable(test_rpc_notag test_rpc_notag.c)
target_include_directories(test_rpc_notag PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_options(test_rpc_notag PRIVATE
  -Wl,--wrap=vrtql_msg_send -Wl,--wrap=vrtql_msg_recv)
target_link_libraries(test_rpc_notag PRIVATE static_lib ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_rpc_notag PRIVATE ws2_32)
endif()

add_test(NAME test_rpc_notag COMMAND test_rpc_notag)

add_custom_target(test_rpc_cov_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_rpc_cov )
add_dependencies(test_rpc_cov static_test_lib static_lib)

# server.c coverage (item-4 module 6) -- needs libuv (BUILD_SERVER).
if(BUILD_SERVER)
  # SP1: reactor lifecycle / accept-read-close / worker-pool+queue. #includes
  # server.c (statics + isolated coverage); the #included symbols shadow
  # static_lib's server.o. --wrap=calloc for the alloc-fail arms. SELF-BOUNDING
  # harness (bounded live-server + watchdog + stop/join each cell).
  add_executable(test_svr_sp1 test_svr_sp1.c)
  target_include_directories(test_svr_sp1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_options(test_svr_sp1 PRIVATE -Wl,--wrap=calloc)
  target_link_libraries(test_svr_sp1 PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_sp1 COMMAND test_svr_sp1)
  add_custom_target(test_svr_sp1_leakcheck
    COMMAND valgrind --leak-check=full --show-reachable=yes
            --suppressions=valgrind.suppress ./test_svr_sp1 )
  add_dependencies(test_svr_sp1 static_test_lib static_lib)

  # SP2: peering (mock-peer lifecycle). #include-server.c isolation; WS server.
  add_executable(test_svr_sp2 test_svr_sp2.c)
  target_include_directories(test_svr_sp2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_libraries(test_svr_sp2 PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_sp2 COMMAND test_svr_sp2)
  add_dependencies(test_svr_sp2 static_test_lib static_lib)

  # SP3: inetd mode (adopted fd over a loopback pair, auto-stop).
  add_executable(test_svr_sp3 test_svr_sp3.c)
  target_include_directories(test_svr_sp3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_libraries(test_svr_sp3 PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_sp3 COMMAND test_svr_sp3)
  add_dependencies(test_svr_sp3 static_test_lib static_lib)

  # SP4: race-arms (run under VWS_TSAN=ON for the TSan gate; K>=15 stress).
  add_executable(test_svr_sp4 test_svr_sp4.c)
  target_include_directories(test_svr_sp4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_libraries(test_svr_sp4 PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_sp4 COMMAND test_svr_sp4)
  add_dependencies(test_svr_sp4 static_test_lib static_lib)

  # SP5: deferred error/leak arms (the SP4-leftover --wrap coverage). One-shot
  # ld --wrap on the libuv primitives drives svr_on_connect's accept failure
  # paths + the inetd-adoption failure arm; default pass-through so the live
  # server still starts/serves. Self-bounding harness (watchdog + stop/join).
  add_executable(test_svr_sp5 test_svr_sp5.c)
  target_include_directories(test_svr_sp5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_options(test_svr_sp5 PRIVATE
    -Wl,--wrap=uv_tcp_init -Wl,--wrap=uv_read_start)
  target_link_libraries(test_svr_sp5 PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_sp5 COMMAND test_svr_sp5)
  # Leak-surfacing target: SP5 cells DRIVE unmodified server.c error paths that
  # leak on the SUBJECT side (accept c/cinfo + the queue shutdown-drain payload)
  # -- this target is the candidate EVIDENCE, not a zero-leak gate.
  add_custom_target(test_svr_sp5_leakcheck
    COMMAND valgrind --leak-check=full --show-reachable=yes
            --suppressions=valgrind.suppress ./test_svr_sp5 )
  add_dependencies(test_svr_sp5 static_test_lib static_lib)

  # SP6: teardown-safety regression (FIX-NOW #1 double uv_close + #3 accept-fail
  # fall-through, ticket 7247dcc924). #include-server.c isolation; one-shot ld
  # --wrap=uv_accept drives svr_on_connect's accept-failure arm. Self-bounding
  # harness (own-loop unit for #1 + bounded live server, watchdog, stop/join).
  add_executable(test_svr_sp6 test_svr_sp6.c)
  target_include_directories(test_svr_sp6 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_options(test_svr_sp6 PRIVATE -Wl,--wrap=uv_accept)
  target_link_libraries(test_svr_sp6 PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_sp6 COMMAND test_svr_sp6)
  add_custom_target(test_svr_sp6_leakcheck
    COMMAND valgrind --leak-check=full --show-reachable=yes
            --suppressions=valgrind.suppress ./test_svr_sp6 )
  add_dependencies(test_svr_sp6 static_test_lib static_lib)

  # shutdown must drain the requests queue before destroy. RED
  # #includes server.c (statics shadow static_lib's server.o); queues one
  # request resident at shutdown and drives svr_shutdown. Functional cell is
  # the ctest gate; the leak is surfaced by the _leakcheck evidence target
  # (valgrind reports 264 bytes leaked against the pre-fix source).
  add_executable(test_svr_reqleak test_svr_reqleak.c)
  target_include_directories(test_svr_reqleak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_libraries(test_svr_reqleak PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_reqleak COMMAND test_svr_reqleak)
  add_custom_target(test_svr_reqleak_leakcheck
    COMMAND valgrind --leak-check=full --show-reachable=yes
            --suppressions=valgrind.suppress ./test_svr_reqleak )
  add_dependencies(test_svr_reqleak static_test_lib static_lib)

  # adoption error arms must uv_close, not bare-free, a registered
  # handle. RED #includes server.c; one-shot --wrap=uv_tcp_open forces the
  # inetd adopt to fail after uv_tcp_init, then the real dtor uv_walk trips the
  # UAF on the freed handle. Functional cell is the ctest gate; the UAF is
  # surfaced by the _leakcheck evidence target (valgrind reports an invalid
  # read/write in uv_walk against pre-fix -- the fault is inside libuv, so
  # valgrind, not ASan, is the observer).
  add_executable(test_svr_uafopen test_svr_uafopen.c)
  target_include_directories(test_svr_uafopen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
  target_link_options(test_svr_uafopen PRIVATE -Wl,--wrap=uv_tcp_open)
  target_link_libraries(test_svr_uafopen PRIVATE
    static_test_lib static_lib ${OS_LIBS} -lm)
  add_test(NAME test_svr_uafopen COMMAND test_svr_uafopen)
  add_custom_target(test_svr_uafopen_leakcheck
    COMMAND valgrind --leak-check=full --show-reachable=yes
            --suppressions=valgrind.suppress ./test_svr_uafopen )
  add_dependencies(test_svr_uafopen static_test_lib static_lib)
endif()

# client/server test programs
set(test_utils client server)

foreach(x ${test_utils})
  add_executable(${x} ${x}.c util/mongoose.c)
  target_link_libraries(${x} PRIVATE ${OS_LIBS})

  if(WIN32)
    target_link_libraries(${x} PRIVATE ws2_32)
  elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(${x} PRIVATE -Wno-error=implicit-function-declaration)
  endif()

  add_test(NAME ${x} COMMAND ${x})
endforeach(x)
