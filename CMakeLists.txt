cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

project(ZEngine VERSION 0.1.0)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

include(CMakeDependentOption)

# ---- Include guards ----
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(
        FATAL_ERROR
        "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
    )
endif()

set(Z_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(ZENGINE_OUTPUT_SUBDIR "" CACHE STRING "Optional output subdirectory under bin, e.g. android/arm64-v8a or ohos/arm64-v8a")
if(ZENGINE_OUTPUT_SUBDIR)
    file(TO_CMAKE_PATH "${ZENGINE_OUTPUT_SUBDIR}" ZENGINE_OUTPUT_SUBDIR)
    set(CMAKE_INSTALL_PREFIX "${Z_ROOT_DIR}/bin/${ZENGINE_OUTPUT_SUBDIR}")
else()
    set(CMAKE_INSTALL_PREFIX "${Z_ROOT_DIR}/bin")
endif()
set(BINARY_ROOT_DIR "${CMAKE_INSTALL_PREFIX}/")

# Set unified output directories for libraries and executables
# This allows multiple projects to share the same engine build outputs
# and avoid recompiling the engine for each project
if(NOT DEFINED ZENGINE_UNIFIED_OUTPUT_DIR)
    set(ZENGINE_UNIFIED_OUTPUT_DIR "${BINARY_ROOT_DIR}" CACHE PATH "Unified output directory for ZEngine libraries")
endif()

# Set library output directory to unified location
# This ensures all engine libraries are built to the same location
# regardless of which project is building them
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${ZENGINE_UNIFIED_OUTPUT_DIR}")

# Set archive (static library) output directory
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ZENGINE_UNIFIED_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${ZENGINE_UNIFIED_OUTPUT_DIR}")

message(STATUS "ZEngine unified output directory: ${ZENGINE_UNIFIED_OUTPUT_DIR}")

# =============================================================================
# Git Detection (required for FetchContent)
# =============================================================================
# Fix: Force standard Git to avoid UGit version parsing issue
if(NOT CMAKE_GIT_EXECUTABLE)
    # Try standard Git first (not UGit wrapper)
    find_program(GIT_EXECUTABLE
        NAMES git.exe git
        PATHS "C:/Program Files/Git/bin"
        NO_DEFAULT_PATH
    )
    if(GIT_EXECUTABLE)
        set(CMAKE_GIT_EXECUTABLE "${GIT_EXECUTABLE}" CACHE FILEPATH "Path to git executable")
    else()
        # Fallback to default search
        find_program(GIT_EXECUTABLE git)
    endif()
else()
    set(GIT_EXECUTABLE "${CMAKE_GIT_EXECUTABLE}")
endif()

if(GIT_EXECUTABLE)
    set(CMAKE_GIT_EXECUTABLE "${GIT_EXECUTABLE}" CACHE FILEPATH "Path to git executable")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" --version
        OUTPUT_VARIABLE GIT_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    message(STATUS "Found git: ${GIT_EXECUTABLE} (${GIT_VERSION_OUTPUT})")
else()
    message(WARNING "Git not found. FetchContent may fail if git repositories are needed.")
endif()

# =============================================================================
# Platform Detection and Macros
# =============================================================================

# Detect operating system
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS" OR OHOS)
    set(Z_PLATFORM_OHOS TRUE CACHE BOOL "Building for OHOS" FORCE)
    set(Z_PLATFORM_NAME "OHOS")
    message(STATUS "Platform: OHOS")
elseif(ANDROID)
    set(Z_PLATFORM_ANDROID TRUE)
    set(Z_PLATFORM_NAME "Android")
    message(STATUS "Platform: Android")
elseif(IOS)
    set(Z_PLATFORM_IOS TRUE)
    set(Z_PLATFORM_NAME "iOS")
    message(STATUS "Platform: iOS")
elseif(WIN32)
    set(Z_PLATFORM_WINDOWS TRUE)
    set(Z_PLATFORM_NAME "Windows")
    message(STATUS "Platform: Windows")
elseif(APPLE)
    set(Z_PLATFORM_MACOS TRUE)
    set(Z_PLATFORM_NAME "macOS")
    message(STATUS "Platform: macOS")
elseif(UNIX)
    set(Z_PLATFORM_LINUX TRUE)
    set(Z_PLATFORM_NAME "Linux")
    message(STATUS "Platform: Linux")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()


# Detect architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(Z_ARCH_X64 TRUE)
    set(Z_ARCH_NAME "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86")
    set(Z_ARCH_X86 TRUE)
    set(Z_ARCH_NAME "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(Z_ARCH_ARM64 TRUE)
    set(Z_ARCH_NAME "ARM64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(Z_ARCH_ARM32 TRUE)
    set(Z_ARCH_NAME "ARM32")
else()
    set(Z_ARCH_NAME "Unknown")
    message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Detect compiler
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(Z_COMPILER_MSVC TRUE)
    set(Z_COMPILER_NAME "MSVC")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(Z_COMPILER_GCC TRUE)
    set(Z_COMPILER_NAME "GCC")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(Z_COMPILER_CLANG TRUE)
    set(Z_COMPILER_NAME "Clang")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(Z_COMPILER_APPLE_CLANG TRUE)
    set(Z_COMPILER_NAME "AppleClang")
else()
    set(Z_COMPILER_NAME "Unknown")
    message(WARNING "Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Define platform macros for C++ code
if(Z_PLATFORM_WINDOWS)
    add_compile_definitions(
        Z_PLATFORM_WINDOWS=1
        Z_PLATFORM_WIN=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
elseif(Z_PLATFORM_ANDROID)
    add_compile_definitions(
        Z_PLATFORM_ANDROID=1
        Z_PLATFORM_ANDROID=1
    )
elseif(Z_PLATFORM_OHOS)
    add_compile_definitions(
        Z_PLATFORM_OHOS=1
        Z_PLATFORM_HARMONYOS=1
    )
elseif(Z_PLATFORM_MACOS)
    add_compile_definitions(
        Z_PLATFORM_MACOS=1
        Z_PLATFORM_MAC=1
    )
elseif(Z_PLATFORM_LINUX)
    add_compile_definitions(
        Z_PLATFORM_LINUX=1
        Z_PLATFORM_LINUX=1
    )
elseif(Z_PLATFORM_IOS)
    add_compile_definitions(
        Z_PLATFORM_IOS=1
        Z_PLATFORM_IOS=1
    )
endif()

# Define architecture macros
if(Z_ARCH_X64)
    add_compile_definitions(Z_ARCH_X64=1)
    # Windows SDK requires _WIN64 and _AMD64_ for x64 builds
    if(Z_PLATFORM_WINDOWS)
        add_compile_definitions(_WIN64 _AMD64_)
    endif()
elseif(Z_ARCH_X86)
    add_compile_definitions(Z_ARCH_X86=1)
    # Windows SDK requires _X86_ for x86 builds
    if(Z_PLATFORM_WINDOWS)
        add_compile_definitions(_X86_)
    endif()
elseif(Z_ARCH_ARM64)
    add_compile_definitions(Z_ARCH_ARM64=1)
    # Windows SDK requires _WIN64 and _ARM64_ for ARM64 builds
    if(Z_PLATFORM_WINDOWS)
        add_compile_definitions(_WIN64 _ARM64_)
    endif()
elseif(Z_ARCH_ARM32)
    add_compile_definitions(Z_ARCH_ARM32=1)
    # Windows SDK requires _ARM_ for ARM32 builds
    if(Z_PLATFORM_WINDOWS)
        add_compile_definitions(_ARM_)
    endif()
endif()

# Define compiler macros
if(Z_COMPILER_MSVC)
    add_compile_definitions(Z_COMPILER_MSVC=1)
elseif(Z_COMPILER_GCC)
    add_compile_definitions(Z_COMPILER_GCC=1)
elseif(Z_COMPILER_CLANG)
    add_compile_definitions(Z_COMPILER_CLANG=1)
elseif(Z_COMPILER_APPLE_CLANG)
    add_compile_definitions(Z_COMPILER_APPLE_CLANG=1)
endif()

# Print platform information
message(STATUS "Platform: ${Z_PLATFORM_NAME}")
message(STATUS "Architecture: ${Z_ARCH_NAME}")
message(STATUS "Compiler: ${Z_COMPILER_NAME}")

# =============================================================================
# Build Options and Performance Optimizations
# =============================================================================

# Enable build caching if available
option(USE_CCACHE "Use ccache for faster rebuilds" ON)
if(USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "Found ccache: ${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    else()
        message(STATUS "ccache not found, skipping cache optimization")
    endif()
endif()

# Enable unity builds for faster compilation (optional)
option(USE_UNITY_BUILD "Use unity builds to reduce compilation time" OFF)
if(USE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 32)
    message(STATUS "Unity builds enabled with batch size: ${CMAKE_UNITY_BUILD_BATCH_SIZE}")
endif()

# Parallel compilation
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC already uses /MP by default in engine/CMakeLists.txt
    # /MP enables parallel compilation of multiple source files within a project
    message(STATUS "MSVC parallel compilation enabled (/MP flag)")
    # Note: Parallel project builds in Visual Studio IDE are controlled via:
    # Tools -> Options -> Projects and Solutions -> Build and Run
    # "Maximum number of parallel project builds" (recommended: CPU core count)
    
    # Optimize Visual Studio generator build performance
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        # Disable unnecessary project dependencies to speed up builds
        set(CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD OFF CACHE BOOL "" FORCE)
        set(CMAKE_VS_INCLUDE_PACKAGE_TO_DEFAULT_BUILD OFF CACHE BOOL "" FORCE)
        
        # Enable faster project loading
        set(CMAKE_VS_GENERATE_DEVELOPER_COMMAND_PROMPT ON CACHE BOOL "" FORCE)
        
        # Set parallel project builds (for IDE builds)
        # This is a hint for Visual Studio IDE, actual control is in IDE settings
        # For command line builds, use: cmake --build . --parallel [N]
        include(ProcessorCount)
        ProcessorCount(N)
        if(NOT N EQUAL 0)
            # Set environment variable hint for MSBuild
            set(ENV{CL_MPCOUNT} ${N})
            message(STATUS "Visual Studio: Hinting ${N} parallel project builds (set in IDE: Tools->Options->Projects->Build and Run)")
        endif()
        
        message(STATUS "Visual Studio generator optimizations enabled")
        message(STATUS "  - For IDE builds: Set 'Maximum parallel project builds' to ${N} in Tools->Options->Projects->Build and Run")
        message(STATUS "  - For command line: Use 'cmake --build build --parallel ${N}' or 'cmake --build build -j ${N}'")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # For GCC/Clang, use job server
    include(ProcessorCount)
    ProcessorCount(N)
    if(NOT N EQUAL 0)
        set(CMAKE_BUILD_PARALLEL_LEVEL ${N} CACHE STRING "Number of parallel jobs for build")
        message(STATUS "Parallel build level: ${CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
endif()

# Export compile commands for IDE support
option(CMAKE_EXPORT_COMPILE_COMMANDS "Export compile commands for IDE support" ON)
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# =============================================================================
# PAPI_TYPE-driven global toolchain switches
# =============================================================================
#
# When -DPAPI_TYPE=v8 is selected on Windows, we must align the C runtime
# library with the vendored `wee8.lib` (engine/3rdparty/puerts/unity/native/
# papi-v8/.backends/papi-v8/Lib/Win64/wee8.lib), which is built as
# `MT_StaticRelease` (i.e. `/MT`, `_ITERATOR_DEBUG_LEVEL=0`). ZEngine's
# default is `/MD[d]`, which produces `LNK2038: 'RuntimeLibrary'` mismatches
# at final-binary link time (ZEditor.exe / ZRuntimeShared.dll).
#
# This block ("Plan C" in AGENTS.md 2.7):
#   1. Force the entire build (engine + 3rdparty) to use `/MT` static CRT
#      via CMAKE_MSVC_RUNTIME_LIBRARY. Set BEFORE any add_subdirectory()
#      call so every target inherits the policy.
#   2. Hard-fail the stock `Debug` config because the vendored wee8.lib only
#      ships `MT_StaticRelease` (no `MTd_StaticDebug`). Users who need to
#      step through engine/script glue under V8 should pick `RelWithDebInfo`
#      or the custom `DebugV8` config (registered below).
# Quickjs path is unaffected -- this block only fires for v8 on Windows.
#
# `DebugV8` config (multi-config generators only):
#   Synthesizes a "Debug-like" build that is ABI-compatible with wee8.lib.
#   Differences vs. stock Debug:
#       - /MT       (not /MTd)        -> matches wee8.lib RuntimeLibrary
#       - _ITERATOR_DEBUG_LEVEL=0     -> matches wee8.lib (no STL bound checks)
#       - _DEBUG NOT defined          -> matches wee8.lib
#       - /Od /Zi /RTC1 still active  -> full single-step + variable visibility
#   Differences vs. RelWithDebInfo:
#       - /Od instead of /O2          -> nothing optimised away, single-step
#                                        lands on every source line.
#   What you LOSE compared to stock Debug:
#       - STL `_ITERATOR_DEBUG_LEVEL=2` bound/iterator checks (you still
#         have `/RTC1` for stack/uninitialised-var checks though).
#       - `assert()` from <cassert> stays active (NDEBUG is not defined for
#         this config either; we explicitly clear it).
#   Limitation: only meaningful on multi-config generators (VS / Xcode-style)
#   because single-config generators just use CMAKE_BUILD_TYPE. On Ninja
#   single-config you'd reconfigure with `-DCMAKE_BUILD_TYPE=DebugV8`, which
#   also works but is rare.
if(WIN32 AND DEFINED PAPI_TYPE AND PAPI_TYPE STREQUAL "v8")
    # ----- Register DebugV8 as a known config for multi-config generators -----
    # This must happen BEFORE the first project()/add_subdirectory() call has
    # finalised CMAKE_CONFIGURATION_TYPES. project() above already ran, but
    # CMake permits appending to CMAKE_CONFIGURATION_TYPES afterwards; the new
    # entry only needs to be in place by the time targets are created.
    get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_is_multi_config)
        if(NOT "DebugV8" IN_LIST CMAKE_CONFIGURATION_TYPES)
            list(APPEND CMAKE_CONFIGURATION_TYPES "DebugV8")
            set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
                CACHE STRING "Available build configs" FORCE)
        endif()
    endif()

    # Map DebugV8 onto Release for any per-config CMake variable that we
    # haven't customised. This is what makes IMPORTED libraries (which only
    # ship Release/Debug variants) resolve to their Release form under
    # DebugV8 -- exactly what we want, since wee8.lib IS a Release-built lib.
    
    # ----- CRT selection -----
    # CMAKE_MSVC_RUNTIME_LIBRARY requires CMP0091=NEW (default since 3.15
    # when policy is set, and we declare cmake_minimum_required 3.19 above
    # so we're fine).
    #
    # All v8 configs (Debug / DebugV8 / Release / RelWithDebInfo / MinSizeRel)
    # use plain `/MT`. Stock Debug WOULD also pick `/MTd` via the generator
    # expression, but we hard-fail the build before it links, so its CRT
    # choice is academic.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded"
        CACHE STRING "MSVC runtime library (forced /MT for V8 wee8.lib compatibility)" FORCE)
    cmake_policy(SET CMP0091 NEW)
    message(STATUS "PAPI_TYPE=v8 on Windows: forcing CMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY} (/MT) to match vendored wee8.lib")

    # ----- DebugV8 compile/link flag overrides -----
    # CMake sees a never-before-known config as "Release-like" by default
    # (no /Od, no /Zi). We have to spell out the flags explicitly.
    # Use init-then-cache pattern so user can still override on cmd line.
    if(MSVC)
        set(CMAKE_C_FLAGS_DEBUGV8_INIT
            "/Od /Ob0 /Zi /RTC1 /D_ITERATOR_DEBUG_LEVEL=0")
        set(CMAKE_CXX_FLAGS_DEBUGV8_INIT
            "/Od /Ob0 /Zi /RTC1 /D_ITERATOR_DEBUG_LEVEL=0")
        set(CMAKE_EXE_LINKER_FLAGS_DEBUGV8_INIT    "/DEBUG /INCREMENTAL")
        set(CMAKE_SHARED_LINKER_FLAGS_DEBUGV8_INIT "/DEBUG /INCREMENTAL")
        set(CMAKE_STATIC_LINKER_FLAGS_DEBUGV8_INIT "")
        set(CMAKE_MODULE_LINKER_FLAGS_DEBUGV8_INIT "/DEBUG /INCREMENTAL")

        foreach(_lang C CXX)
            if(NOT CMAKE_${_lang}_FLAGS_DEBUGV8)
                set(CMAKE_${_lang}_FLAGS_DEBUGV8 "${CMAKE_${_lang}_FLAGS_DEBUGV8_INIT}"
                    CACHE STRING "${_lang} flags for DebugV8 config" FORCE)
            endif()
        endforeach()
        foreach(_kind EXE SHARED STATIC MODULE)
            if(NOT CMAKE_${_kind}_LINKER_FLAGS_DEBUGV8)
                set(CMAKE_${_kind}_LINKER_FLAGS_DEBUGV8
                    "${CMAKE_${_kind}_LINKER_FLAGS_DEBUGV8_INIT}"
                    CACHE STRING "${_kind} linker flags for DebugV8 config" FORCE)
            endif()
        endforeach()
    endif()

    # ----- Reject stock Debug for v8 -----
    # The vendored wee8.lib is MT_StaticRelease only; no Debug flavor exists.
    # Refuse stock `Debug` early. `DebugV8` IS allowed.
    # For multi-config generators (Visual Studio), we can't reject at
    # configure time -- we only know the active config at build time. So we
    # only check single-config generators here (Ninja, Makefiles, ...).
    if(NOT _is_multi_config)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
            message(FATAL_ERROR
                "PAPI_TYPE=v8 on Windows requires Release / RelWithDebInfo / "
                "DebugV8 (vendored wee8.lib is MT_StaticRelease only; no "
                "Debug flavor). Reconfigure with -DCMAKE_BUILD_TYPE=DebugV8 "
                "for a /Od + /Zi debug-friendly build, or use stock Debug "
                "with -DPAPI_TYPE=quickjs.")
        endif()
    else()
        message(STATUS "PAPI_TYPE=v8: multi-config generator detected -- "
                       "build with --config Release / RelWithDebInfo / DebugV8. "
                       "--config Debug will fail at link time with LNK2038.")
    endif()
elseif(WIN32)
    # quickjs (or any non-v8) path: restore the default DynamicDLL CRT.
    # We FORCE-write so that flipping back from a previous v8 configure
    # in the same build/ directory cleanly resets the cached value.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CACHE STRING "MSVC runtime library (default /MD for non-v8 builds)" FORCE)
endif()

# =============================================================================
# Tests
# =============================================================================

# Include platform tests
add_subdirectory(tests)

# =============================================================================
# Main Engine
# =============================================================================

# Include the main engine
add_subdirectory(engine)
