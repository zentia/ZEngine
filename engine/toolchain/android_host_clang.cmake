# Android toolchain for hosts the official NDK does not ship binaries for.
#
# Why this exists: the NDK's prebuilt clang/lld live under
# toolchains/llvm/prebuilt/linux-x86_64 and are x86_64 ELF. On a Linux/aarch64
# host (this project's ARM/CI boxes) they cannot execute: "cannot run
# executable" / "required file not found". The *sysroot* and the target
# libraries, however, are architecture-independent inputs, so the host's own
# clang 18 + ld.lld 18 can cross-compile to Android by pointing at them. NDK r27
# is clang 18 itself, so headers/ABI line up.
#
# Usage:
#   cmake -S . -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=engine/toolchain/android_host_clang.cmake \
#     -DANDROID_NDK=/opt/android-sdk/ndk/27.0.12077973 \
#     -DANDROID_ABI=arm64-v8a \
#     -DANDROID_PLATFORM=android-26
#
# Prefer the stock NDK toolchain (-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake)
# whenever the host is x86_64 linux / windows / any macOS.

# A toolchain file is re-read for every try_compile, so never clobber values the
# command line already provided (-DANDROID_NDK=... would be lost otherwise).
if(NOT DEFINED ANDROID_NDK OR ANDROID_NDK STREQUAL "")
    if(DEFINED ENV{ANDROID_NDK} AND NOT "$ENV{ANDROID_NDK}" STREQUAL "")
        set(ANDROID_NDK "$ENV{ANDROID_NDK}" CACHE PATH "Path to the Android NDK")
    endif()
endif()
if(NOT DEFINED ANDROID_ABI OR ANDROID_ABI STREQUAL "")
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI (arm64-v8a, armeabi-v7a, x86_64)")
endif()
if(NOT DEFINED ANDROID_PLATFORM OR ANDROID_PLATFORM STREQUAL "")
    set(ANDROID_PLATFORM "android-26" CACHE STRING "Android API level, e.g. android-26")
endif()

# try_compile() runs in a nested project with its own empty cache; without this
# the nested configure sees an empty ANDROID_NDK and the compiler test fails.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ANDROID_NDK ANDROID_ABI ANDROID_PLATFORM)

if(ANDROID_NDK STREQUAL "")
    message(FATAL_ERROR
        "ANDROID_NDK is not set. Pass -DANDROID_NDK=/path/to/ndk or export ANDROID_NDK.")
endif()

if(NOT EXISTS "${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/sysroot")
    message(FATAL_ERROR "Not an NDK root (missing toolchains/llvm/prebuilt/linux-x86_64/sysroot): ${ANDROID_NDK}")
endif()

# Map the ABI onto clang's target triple. The NDK sysroot keeps target libs in
# per-ABI directories, and libc.so/libm.so etc. under an API-level subdirectory.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(_z_target_triple "aarch64-linux-android")
    set(_z_abi_dir "aarch64-linux-android")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(_z_target_triple "armv7a-linux-androideabi")
    set(_z_abi_dir "arm-linux-androideabi")
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(_z_target_triple "x86_64-linux-android")
    set(_z_abi_dir "x86_64-linux-android")
elseif(ANDROID_ABI STREQUAL "x86")
    set(_z_target_triple "i686-linux-android")
    set(_z_abi_dir "i686-linux-android")
else()
    message(FATAL_ERROR "Unsupported ANDROID_ABI: ${ANDROID_ABI}")
endif()

string(REPLACE "android-" "" _z_api "${ANDROID_PLATFORM}")

set(_z_ndk_prebuilt "${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64")
set(_z_sysroot "${_z_ndk_prebuilt}/sysroot")
set(_z_api_lib_dir "${_z_sysroot}/usr/lib/${_z_abi_dir}/${_z_api}")

if(NOT EXISTS "${_z_api_lib_dir}")
    message(FATAL_ERROR
        "No NDK target libraries for API ${_z_api} at ${_z_api_lib_dir}. "
        "Pick an ANDROID_PLATFORM the NDK actually ships (21..35 for NDK r27).")
endif()

# ndk r27 == clang 18; this directory carries the Android builtins
# (libclang_rt.builtins-<arch>-android.a) that the host clang cannot find on its own.
file(GLOB _z_clang_versions "${_z_ndk_prebuilt}/lib/clang/*")
list(SORT _z_clang_versions)
list(REVERSE _z_clang_versions)
list(GET _z_clang_versions 0 _z_resource_dir)

# bionic has pthread inside libc and never had pthread_cancel, so CMake's
# FindThreads probe fails and it falls back to -lpthreads/-lpthread, which then
# do not exist. Tell CMake up front that the C library provides threads.
set(CMAKE_HAVE_LIBC_PTHREAD TRUE CACHE INTERNAL "bionic ships pthread in libc")
set(CMAKE_THREAD_LIBS_INIT "" CACHE STRING "bionic ships pthread in libc")

set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION ${_z_api})
set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
set(CMAKE_SYSROOT "${_z_sysroot}")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET "${_z_target_triple}${_z_api}")
set(CMAKE_CXX_COMPILER_TARGET "${_z_target_triple}${_z_api}")
set(CMAKE_LINKER ld.lld)

# bionic folds pthread into libc: there is deliberately no libpthread.so in the
# NDK sysroot, but a few third-party CMakeLists (and CMake's own LTO/unwind
# probes) still ask for -lpthread and the link fails. Provide a linker script
# that redirects it to libc. Written under the build tree so nothing leaks into
# the NDK install.
set(_z_stub_lib_dir "${CMAKE_BINARY_DIR}/android_stub_libs")
file(MAKE_DIRECTORY "${_z_stub_lib_dir}")
foreach(_z_stub pthread pthreads rt util)
    if(NOT EXISTS "${_z_stub_lib_dir}/lib${_z_stub}.so")
        file(WRITE "${_z_stub_lib_dir}/lib${_z_stub}.so" "INPUT(libc.so)\n")
    endif()
endforeach()

# Two strings, not CMake lists: a list interpolated into a flags variable becomes
# "a;b;c" and clang sees one broken argument.
#
# Compile-time flags and link-time flags must stay separate: some targets build
# with -Werror=unused-command-line-argument, and -fuse-ld/-L are link-only, so
# leaving them in CMAKE_*_FLAGS turns every compile into an error.
set(_z_compile_flags "-resource-dir=${_z_resource_dir} -rtlib=compiler-rt")
set(_z_link_flags "-fuse-ld=lld -rtlib=compiler-rt -L${_z_api_lib_dir} -L${_z_stub_lib_dir}")

set(CMAKE_C_FLAGS_INIT   "${_z_compile_flags} --unwindlib=none")
set(CMAKE_CXX_FLAGS_INIT "${_z_compile_flags} -stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_z_link_flags} -stdlib=libc++ -lunwind")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_z_link_flags} -stdlib=libc++ -lunwind")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_z_link_flags} -stdlib=libc++ -lunwind")

# Everything must come from the NDK sysroot; the host's /usr/lib must not leak in.
# find_library() also has to see the API-level directory, otherwise -landroid /
# -llog / -lvulkan look "not found" even though the NDK ships them under
# <sysroot>/usr/lib/<abi>/<api>/.
set(CMAKE_FIND_ROOT_PATH "${_z_sysroot}")
list(APPEND CMAKE_LIBRARY_PATH "${_z_api_lib_dir}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
