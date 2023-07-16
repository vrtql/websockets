#-------------------------------------------------------------------------------
# Build Environment
#-------------------------------------------------------------------------------

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_definitions(-fPIC -W -DMG_ENABLE_LINES -DMG_ENABLE_OPENSSL)
  add_compile_options(-Werror)
  add_compile_options(-Wimplicit-function-declaration)
endif()

if (CMAKE_C_COMPILER_ID MATCHES "GNU")
  add_compile_options(-Wno-discarded-qualifiers)
endif()

if (CMAKE_C_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wno-ignored-qualifiers)
  add_compile_options(-Wno-incompatible-pointer-types-discards-qualifiers)
endif()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/mpack
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PREFIX}/include
)

# Add OpenSSL include directories
include_directories(${OPENSSL_INCLUDE_DIR})

set(OS_LIBS OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

if(${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
  list(APPEND OS_LIBS socket)
endif()

#-------------------------------------------------------------------------------
# Source Targets
#-------------------------------------------------------------------------------

file(GLOB mpack_files mpack/*.c)
file(GLOB util_files util/*.c)

set(core_sources
  message.c
  vrtql.c
  websocket.c
  rpc.c
  socket.c
  ${mpack_files}
  ${util_files}
)

# Server (optional)
option(BUILD_SERVER "Build the server" OFF)

if(BUILD_SERVER)
  set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/config ${CMAKE_MODULE_PATH})
  find_package(LibUV REQUIRED)
  include_directories(${LIBUV_INCLUDE_DIRS})
  list(APPEND OS_LIBS ${LIBUV_LIBRARIES})
  list(APPEND core_sources server.c http_parser.c http_request.c)
endif()

#-------------------------------------------------------------------------------
# Build Targets
#-------------------------------------------------------------------------------

# Shared Library
add_library(shared_lib SHARED ${core_sources})
set_target_properties(shared_lib PROPERTIES
  CLEAN_DIRECT_OUTPUT 1
  OUTPUT_NAME ${PROJECT_NAME}
)

# Static Library: for unit tests
add_library(static_lib STATIC ${core_sources})
set_target_properties(static_lib PROPERTIES
  COMPILE_FLAGS "-fPIC"
  OUTPUT_NAME ${PROJECT_NAME}
)

# Link against required libraries
target_link_libraries(static_lib PRIVATE ${OS_LIBS})

if(WIN32)
  target_link_libraries(shared_lib PRIVATE ws2_32)
  target_link_libraries(static_lib PRIVATE ws2_32)
endif()

target_link_libraries(shared_lib PRIVATE ${OS_LIBS})

#-------------------------------------------------------------------------------
# Testing
#-------------------------------------------------------------------------------

add_subdirectory(test)
