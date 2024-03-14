cmake_minimum_required(VERSION 3.15)
mark_as_advanced(CMAKE_BACKWARDS_COMPATIBILITY)
set(CMAKE_ALLOW_LOOSE_LOOP_CONSTRUCTS TRUE)

project(vws)

include(config/platform.cmake)

#-------------------------------------------------------------------------------
# Project / Files
#-------------------------------------------------------------------------------

set(PROJECT_NAME vws)

# Version
set(VERSION_MAJ      1)
set(VERSION_MIN      0)
set(VERSION_CL       0)
set(VERSION_PL       0)
set(VERSION_BN       0)
set(VERSION ${VERSION_MAJ}.${VERSION_MIN}.${VERSION_PL})

string(TIMESTAMP RELEASE_DATE)
message(STATUS "RELEASE_DATE: ${RELEASE_DATE}")
message(STATUS "VERSION:      ${VERSION}")

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/src/common.h.in
                ${CMAKE_CURRENT_SOURCE_DIR}/src/common.h )

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/doc/xml/doc.mod.in
                ${CMAKE_CURRENT_SOURCE_DIR}/doc/xml/doc.mod )

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/src/ruby/config/version.rb.in
                ${CMAKE_CURRENT_SOURCE_DIR}/src/ruby/lib/vrtql/ws/version.rb )

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/src/ruby/config/vrtql-ws.gemspec.in
                ${CMAKE_CURRENT_SOURCE_DIR}/src/ruby/config/vrtql-ws.gemspec )

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/src/ruby/scripts/common.sh.in
                ${CMAKE_CURRENT_SOURCE_DIR}/src/ruby/scripts/common.sh )

set(CMAKE_BUILD_TYPE "Debug")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_subdirectory(src)
