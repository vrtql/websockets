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
  test_message
  test_rpc_server
  test_rpc_client
  test_websocket )

if(BUILD_SERVER)
  list(APPEND test_targets
    test_http_request
    test_http_server
    test_inetd_server
    test_msg_server
    test_peering
    test_pipe_server
    test_server
    test_ws_server )
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

# url.c coverage. test_url.c #includes url.c directly (reaches the static helpers
# + isolates coverage to url.c), so it must NOT link static_lib -- that would
# duplicate url's symbols. url.c is self-contained (pure libc). One-shot ld
# --wrap on malloc/calloc drives the allocation-failure arms.
add_executable(test_url test_url.c)
target_include_directories(test_url PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PREFIX}/include)
target_link_options(test_url PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc)
target_link_libraries(test_url PRIVATE ${OS_LIBS} -lm)

if(WIN32)
  target_link_libraries(test_url PRIVATE ws2_32)
endif()

add_test(NAME test_url COMMAND test_url)

add_custom_target(test_url_leakcheck
  COMMAND valgrind --leak-check=full --show-reachable=yes
          --suppressions=valgrind.suppress ./test_url )

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
