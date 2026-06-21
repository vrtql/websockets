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
# 2b. Build libuv (only needed for BUILD_SERVER=1)
#
#    git clone --depth 1 --branch v1.40.0 \
#        https://github.com/libuv/libuv.git
#    cd libuv
#    cmake -B build-mingw -G "Unix Makefiles" \
#          -DCMAKE_TOOLCHAIN_FILE=<vws>/config/windows-toolchain.cmake \
#          -DCMAKE_INSTALL_PREFIX=/opt/mingw/libuv \
#          -DBUILD_TESTING=OFF -DLIBUV_BUILD_TESTS=OFF
#    make -C build-mingw install
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

# Set to where you have it installed. libuv is only required for
# BUILD_SERVER=1; the openssl prefix alone suffices for the client core.
set(CMAKE_FIND_ROOT_PATH "/opt/mingw/openssl" "/opt/mingw/libuv")

# modify default behavior of FIND_XXX() commands
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
