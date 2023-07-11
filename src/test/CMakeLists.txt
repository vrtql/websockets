SET(CMAKE_BUILD_TYPE "Debug")

#-------------------------------------------------------------------------------
# Test programs
#-------------------------------------------------------------------------------

# Create project C/C++ configuration file
CONFIGURE_FILE( ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
                ${CMAKE_CURRENT_BINARY_DIR}/config.h )

# Common code needed for tests
SET(testlib_sources common.c util/mongoose.c)

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
  test_json
  test_message
  test_websocket
  client
  server)

foreach(x ${test_targets})
  add_executable(${x} ${x}.c)
  target_include_directories(${x} PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${gtest_dir}/include"
    ${PREFIX}/include)
  target_link_libraries(${x} PRIVATE
    static_test_lib
    static_lib
    ${OS_LIBS})

  if(WIN32)
    target_link_libraries(${x} PRIVATE ws2_32)
  endif()

  add_test(NAME ${x} COMMAND ${x})

  add_custom_target(${x}_leakcheck
    COMMAND valgrind --leak-check=full --show-reachable=yes ./${x} )

  add_dependencies(${x} static_test_lib)
  add_dependencies(${x} static_lib)
endforeach(x)
