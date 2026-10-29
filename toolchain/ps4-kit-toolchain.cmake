# The kit's CMake toolchain: love-ps4's (PacBrew ps4.cmake: ld.lld called directly, OpenOrbis CRT and
# C library) with clang 21 + libc++ 21 instead of clang 12 + libc++ 12. libc++ 21 is built by
# toolchain/build-libcxx.sh into PS4_SYSROOT together with the libc-overlay headers (FreeBSD values
# for the musl headers' clock ids, mmap flags and signals).
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=~/ps4-kit/toolchain/ps4-kit-toolchain.cmake ...
#   include(~/ps4-kit/ps4-kit.cmake) in the project (or -DCMAKE_PROJECT_<name>_INCLUDE=...)
#
# The port's own libraries (Boost, SDL, FFmpeg, ...) are found in PS4_SYSROOT ahead of PacBrew's.

if (DEFINED ENV{OPENORBIS})
    set(OPENORBIS $ENV{OPENORBIS})
else ()
    set(OPENORBIS /opt/pacbrew/ps4/openorbis)
endif ()
include(${OPENORBIS}/cmake/ps4.cmake)

if (DEFINED ENV{PS4_SYSROOT})
    set(PS4_SYSROOT $ENV{PS4_SYSROOT})
elseif (NOT DEFINED PS4_SYSROOT)
    set(PS4_SYSROOT $ENV{HOME}/ps4kit-build/sysroot)
endif ()
if (NOT DEFINED CLANG_VER)
    set(CLANG_VER 21)
endif ()

find_program(PS4_MODERN_CC clang-${CLANG_VER} REQUIRED)
find_program(PS4_MODERN_CXX clang++-${CLANG_VER} REQUIRED)
set(CMAKE_C_COMPILER ${PS4_MODERN_CC} CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER ${PS4_MODERN_CXX} CACHE PATH "" FORCE)
set(CMAKE_ASM_COMPILER ${PS4_MODERN_CC} CACHE PATH "" FORCE)

# Same flags as ps4.cmake, plus the libc overlay ahead of the OpenOrbis C headers, and code for the
# PS4's CPU (AMD Jaguar: SSE4.2, AVX, BMI1, F16C, MOVBE; no AVX2) rather than baseline x86-64 (SSE2).
# No -I for PacBrew's port libraries (usr/include: old fmt, SDL2, ...): an -I is searched before every
# target's own include directories, so those headers would hide the versions a project builds itself.
set(_ps4_c_flags
  "-target x86_64-pc-freebsd12-elf \
   -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ \
   -DPS4 -D__BSD_VISIBLE -D_BSD_SOURCE \
   -fPIC -funwind-tables -march=btver2 \
   -isysroot ${OPENORBIS} -isystem ${PS4_SYSROOT}/libc-overlay -isystem ${OPENORBIS}/include")
set(CMAKE_ASM_FLAGS_INIT "${_ps4_c_flags}")
set(CMAKE_C_FLAGS_INIT "${_ps4_c_flags}")
# libc++ 21 headers replace the OpenOrbis libc++ 12 ones and must come before the C headers.
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${PS4_SYSROOT}/include/c++/v1 ${_ps4_c_flags}")

set(CMAKE_CXX_STANDARD_LIBRARIES
    "${CMAKE_C_STANDARD_LIBRARIES} ${PS4_SYSROOT}/lib/libc++.a ${PS4_SYSROOT}/lib/libc++abi.a ${OPENORBIS}/lib/libunwind.a")

list(PREPEND CMAKE_FIND_ROOT_PATH ${PS4_SYSROOT})
# pthreads are in libkernel, but CMake's probe programs don't link: FindThreads is told.
set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE INTERNAL "")
set(CMAKE_THREAD_LIBS_INIT "" CACHE INTERNAL "")
set(Threads_FOUND TRUE CACHE INTERNAL "")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OPENORBIS PS4_SYSROOT CLANG_VER CMAKE_FIND_ROOT_PATH)
