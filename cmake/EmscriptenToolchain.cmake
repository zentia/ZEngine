# =============================================================================
# ZEngine Emscripten Toolchain
# -----------------------------------------------------------------------------
# Thin wrapper around the official Emscripten toolchain shipped with EMSDK
# (`$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`).
#
# This file:
#   1. Locates the official Emscripten toolchain through the EMSDK / EMSCRIPTEN
#      environment variables (set by `emsdk_env.bat` / `emsdk_env.sh`).
#   2. Includes it so emcc/em++ are picked as the C/C++ compilers.
#   3. Layers ZEngine-specific compile / link flags required by the WebGL 2.0
#      RHI backend (USE_WEBGL2=1, FULL_ES3=1, WASM=1, ...).
#
# Usage:
#   set EMSDK before invoking cmake, then either:
#     * call `emcmake cmake --preset web_ninja_debug` (recommended, sets up env)
#     * or pass `-DCMAKE_TOOLCHAIN_FILE=cmake/EmscriptenToolchain.cmake`
#
# Reference style: WebKit / Godot / o3de all wrap `Platform/Emscripten.cmake`.
# =============================================================================

# -----------------------------------------------------------------------------
# 1. Locate the official Emscripten toolchain.
# -----------------------------------------------------------------------------
if(NOT DEFINED ENV{EMSDK} OR "$ENV{EMSDK}" STREQUAL "")
    message(FATAL_ERROR
        "EMSDK environment variable is not set. Run 'emsdk_env.bat' (Windows) "
        "or 'source ./emsdk_env.sh' (Linux/macOS) from your emsdk install "
        "directory before configuring the web build.")
endif()

file(TO_CMAKE_PATH "$ENV{EMSDK}" Z_EMSDK_ROOT)
set(Z_EMSCRIPTEN_TOOLCHAIN
    "${Z_EMSDK_ROOT}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

if(NOT EXISTS "${Z_EMSCRIPTEN_TOOLCHAIN}")
    # Some emsdk distributions place the file under fastcomp / older layouts.
    file(GLOB_RECURSE Z_EMSCRIPTEN_TOOLCHAIN_CANDIDATES
        "${Z_EMSDK_ROOT}/*/emscripten/cmake/Modules/Platform/Emscripten.cmake")
    list(LENGTH Z_EMSCRIPTEN_TOOLCHAIN_CANDIDATES Z_EMSCRIPTEN_TOOLCHAIN_COUNT)
    if(Z_EMSCRIPTEN_TOOLCHAIN_COUNT EQUAL 0)
        message(FATAL_ERROR
            "Cannot locate Emscripten.cmake under EMSDK='${Z_EMSDK_ROOT}'. "
            "Please reinstall emsdk or run 'emsdk install latest' / 'emsdk activate latest'.")
    endif()
    list(GET Z_EMSCRIPTEN_TOOLCHAIN_CANDIDATES 0 Z_EMSCRIPTEN_TOOLCHAIN)
endif()

message(STATUS "ZEngine: using Emscripten toolchain at ${Z_EMSCRIPTEN_TOOLCHAIN}")
include("${Z_EMSCRIPTEN_TOOLCHAIN}")

# -----------------------------------------------------------------------------
# 2. Project-level Emscripten flags.
#
# These mirror the requirements of the WebGL 2.0 RHI in
#   engine/source/Runtime/Function/Render/interface/webgl2/.
# Tweak per-target options (allocations, exports, ...) in the leaf
# CMakeLists.txt so this file stays focused on platform plumbing.
# -----------------------------------------------------------------------------

# Emscripten uses .a / .o for archives and objects; CMake handles that, but we
# enforce static linking everywhere on web builds so the final wasm bundle is
# self-contained.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# C++20 already enforced by the root CMakeLists.txt; keep emcc in sync.
set(CMAKE_CXX_STANDARD            20 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED   ON CACHE BOOL   "" FORCE)
set(CMAKE_CXX_EXTENSIONS          OFF CACHE BOOL  "" FORCE)

# Common compile flags for all targets in the web build.
add_compile_options(
    -Wno-unused-command-line-argument
    -Wno-deprecated-declarations
    -fwasm-exceptions      # smaller / faster than JS-based exceptions
)

# Common link flags. emcc treats these as both compile and link options when
# they start with -s, but CMake only forwards them at the link step, which is
# exactly what we need.
add_link_options(
    "SHELL:-s WASM=1"
    "SHELL:-s USE_WEBGL2=1"
    "SHELL:-s FULL_ES3=1"
    "SHELL:-s MIN_WEBGL_VERSION=2"
    "SHELL:-s MAX_WEBGL_VERSION=2"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s INITIAL_MEMORY=134217728"   # 128 MiB
    "SHELL:-s STACK_SIZE=8388608"         #   8 MiB
    "SHELL:-s DISABLE_EXCEPTION_CATCHING=0"
    "SHELL:-s NO_EXIT_RUNTIME=1"
    "SHELL:-s ENVIRONMENT=web"
    -fwasm-exceptions
)

# Static archives behave differently under emcc; tell ar/ranlib to stay quiet.
set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> qc <TARGET> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> <TARGET>")
set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> <TARGET>")

# -----------------------------------------------------------------------------
# 3. Project-level macros so client code can branch on the web RHI.
#    Z_PLATFORM_WEB / Z_RHI_WEBGL2 are picked up by ZEngine modules in the same
#    style as Z_PLATFORM_WINDOWS / Z_PLATFORM_MACOS.
# -----------------------------------------------------------------------------
add_compile_definitions(
    Z_PLATFORM_WEB=1
    Z_PLATFORM_EMSCRIPTEN=1
    Z_RHI_WEBGL2=1
)
