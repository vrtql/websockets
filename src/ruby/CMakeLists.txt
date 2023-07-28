cmake_minimum_required(VERSION 3.12)

set(CMAKE_BUILD_TYPE Debug)

project(ws)

include(config/platform.cmake)

#-------------------------------------------------------------------------------
# Build Environment
#-------------------------------------------------------------------------------

if(WIN32)
set( RUBY_INCLUDE_DIRS
  /opt/mingw/ruby/include/ruby-2.7.0
  /opt/mingw/ruby/include/ruby-2.7.0/x64-mingw32 )
set(RUBY_LIBRARY /opt/mingw/ruby/lib/libx64-msvcrt-ruby270.dll.a)
else()
find_package(Ruby REQUIRED)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(OS_LIBS OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

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

# Add C project include directories
include_directories( ../mpack ../util ${PREFIX}/include )

# Add OpenSSL include directories
include_directories(${OPENSSL_INCLUDE_DIR})

# Include the Ruby headers
include_directories(${RUBY_INCLUDE_DIRS})

#-------------------------------------------------------------------------------
# Build Targets
#-------------------------------------------------------------------------------

file(GLOB core_files
  ../http_message.c
  ../http_parser.c
  ../message.c
  ../socket.c
  ../vws.c
  ../websocket.c
)

file(GLOB mpack_files ../mpack/*.c)
file(GLOB util_files  ../util/*.c)

# Source files
add_library(ws SHARED
  ext/vrtql/ws/main.c
  ext/vrtql/ws/rb_map.c
  ext/vrtql/ws/rb_ws_connection.c
  ext/vrtql/ws/rb_ws_frame.c
  ext/vrtql/ws/rb_ws_message.c
  ext/vrtql/ws/rb_mq_connection.c
  ext/vrtql/ws/rb_mq_message.c
  ${core_files}
  ${mpack_files}
  ${util_files} )

# Linking
target_link_libraries(ws PRIVATE ${RUBY_LIBRARY} ${OS_LIBS})

if(WIN32)
  target_link_libraries(ws PRIVATE ws2_32)
endif()

# Set the library output name and properties
set_target_properties(ws PROPERTIES
    OUTPUT_NAME ws
    PREFIX ""
)

# Install the library
install(TARGETS ws LIBRARY DESTINATION lib)

# Install the Ruby extension files
file(GLOB RUBY_FILES "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.rb")
install(FILES ${RUBY_FILES} DESTINATION lib)

#-------------------------------------------------------------------------------
# Gem build and testing
#-------------------------------------------------------------------------------

# Build RDoc documentation
add_custom_target(rdoc COMMAND yard doc --main README.md doc/vrtql.rb)

# Clean and rebuild all source from scratch
add_custom_target(gem-build COMMAND ./scripts/gem-build.sh)

# Build and unpack gem
add_custom_target(gem-unpack COMMAND ./scripts/gem-unpack.sh)

# Build and test install
add_custom_target(gem-test COMMAND ./scripts/gem-test.sh)
