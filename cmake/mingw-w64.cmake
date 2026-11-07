# Cross-compile for 64-bit Windows with MinGW-w64 (Debian/Ubuntu's
# g++-mingw-w64-x86-64), e.g.:
#   cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
# The -posix compiler (winpthreads threads) is the one the Windows build has
# always been checked with.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
