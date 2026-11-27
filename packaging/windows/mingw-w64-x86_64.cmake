# CMake toolchain for 64-bit Windows, cross-compiled with MinGW-w64 (gcc).
# Made for the image of packaging/windows/Dockerfile, which builds static
# zlib and libcurl (Schannel) into /opt/x86_64-w64-mingw32 with this same
# file; packaging/windows/release.sh uses it for the app.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Headers, libraries and CMake packages come only from the MinGW sysroot
# and the image's prefix, never from the build machine; programs (cmake,
# git) from the build machine.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 /opt/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Fully static executables: libgcc and winpthreads go into the .exe, so it
# needs no DLL beside it, only Windows' own.
set(CMAKE_EXE_LINKER_FLAGS_INIT -static)
