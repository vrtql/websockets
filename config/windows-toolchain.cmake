# 1. Set up Linux environment
#
#     apt-get install mingw-w64 mingw-w64-tools mingw-w64-common \
#                     g++-mingw-w64-x86-64 mingw-w64-x86-64-dev
#
# 2 Build OpenSSL
#
# A. As root create the install dir and set permission for user
#     mkdir /opt/mingw
#     chown -R owensmk:owensmk /opt/mingw
#
# B. Download and unpack
#
#    tar xzvf openssl-1.1.1u.tar.gz
#    cd openssl-1.1.1u
#    ./Configure --cross-compile-prefix=x86_64-w64-mingw32- \
#                --prefix=/opt/mingw shared mingw64 no-tests
#    make
#    make DESTDIR=/opt/mingw install
#
# 3. Invoke CMake
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=config/windows-toolchain.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# cross compilers to use for C, C++
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_Fortran_COMPILER ${TOOLCHAIN_PREFIX}-gfortran)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Set to where you have it installed
set(CMAKE_FIND_ROOT_PATH "/opt/mingw/openssl")

# modify default behavior of FIND_XXX() commands
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
