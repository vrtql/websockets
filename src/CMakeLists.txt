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
  add_compile_options(-Wno-unused-parameter)
endif()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
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

file(GLOB llhttp_files llhttp/*.c)
file(GLOB mpack_files mpack/*.c)
file(GLOB util_files util/*.c)

set(core_sources
  http_message.c
  message.c
  rpc.c
  socket.c
  vws.c
  url.c
  websocket.c
  ${llhttp_files}
  ${mpack_files}
  ${util_files}
)

# Server (optional)
option(BUILD_SERVER "Build the server" OFF)

# Address Sanitizer
option(ASAN "Build the server" OFF)

if(BUILD_SERVER)
  set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/config ${CMAKE_MODULE_PATH})
  find_package(LibUV REQUIRED)
  include_directories(${LIBUV_INCLUDE_DIRS})
  list(APPEND OS_LIBS ${LIBUV_LIBRARIES})
  list(APPEND core_sources server.c)
endif()

if(ASAN)
  add_definitions(-fsanitize=address -g -O1)
  list(APPEND OS_LIBS asan)
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
# Install
#-------------------------------------------------------------------------------

file(GLOB core_headers *.h)
set(HEADERS ${core_headers})

set_property(TARGET shared_lib PROPERTY PUBLIC_HEADER ${HEADERS})
install( TARGETS shared_lib
  PUBLIC_HEADER DESTINATION include/vws
  LIBRARY DESTINATION lib PERMISSIONS
  ARCHIVE DESTINATION lib PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE
  GROUP_READ GROUP_EXECUTE
  WORLD_READ WORLD_EXECUTE )

install( DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/llhttp"
         DESTINATION "include/vws"
         FILES_MATCHING
         PATTERN "*.h" )

install( DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/mpack"
         DESTINATION "include/vws"
         FILES_MATCHING
         PATTERN "*.h" )

install( DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/util"
         DESTINATION "include/vws"
         FILES_MATCHING
         PATTERN "*.h" )

#-------------------------------------------------------------------------------
# Testing
#-------------------------------------------------------------------------------

add_subdirectory(test)
