set(third_party_folder "ThirdParty")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# PAPI type selection. ZEngine is JavaScript-only (TypeScript -> tsc -> .js
# -> JS VM). Supported VMs: quickjs (default) and v8.
#
# Two-tier strategy (matches Cocos Creator / UE PuerTS / Unity JS hosts):
#   * quickjs -> Web (emscripten/wasm) and mobile (iOS App Store bans JIT).
#                ~400KB pure-interpreter VM. Same VM on every JIT-restricted
#                platform so script semantics don't silently diverge.
#   * v8      -> Desktop editor + PC/console runtime. JIT brings script
#                logic close to C++ perf, plus V8 Inspector gives Chrome
#                DevTools / VSCode debugging out of the box.
#
# Node.js was removed: game scripts don't need libuv / fs / net / cluster
# (the engine owns assets, networking and scheduling), and using Node-only
# APIs would silently break the iOS / Web builds. Pure V8 gives the same
# JS engine without the embedding cost.
if(NOT DEFINED PAPI_TYPE)
    set(PAPI_TYPE "quickjs" CACHE STRING "PAPI type to use (quickjs, v8)")
    message(STATUS "PAPI_TYPE not specified, defaulting to: ${PAPI_TYPE}")
else()
    message(STATUS "Using PAPI_TYPE: ${PAPI_TYPE}")
endif()

# Validate PAPI_TYPE. Lua / Python / Node.js were removed -- ZEngine commits
# to JavaScript/TypeScript scripting with a two-VM strategy (quickjs + v8).
if(NOT PAPI_TYPE MATCHES "^(quickjs|v8)$")
    message(FATAL_ERROR "Invalid PAPI_TYPE: ${PAPI_TYPE}. Valid options: quickjs, v8")
endif()

# When PAPI_TYPE=v8 on Windows, all 3rdparty libraries that have their own
# CRT-selection knobs need to be flipped to /MT to match the vendored
# wee8.lib (MT_StaticRelease). The top-level CMakeLists.txt sets
# CMAKE_MSVC_RUNTIME_LIBRARY for libraries that rely on the standard CMake
# variable, but several vendored libs gate their CRT on per-library cache
# options that we have to set here BEFORE add_subdirectory():
#   - GLFW: USE_MSVC_RUNTIME_LIBRARY_DLL (ON=/MD by default)
#   - cURL: CURL_STATIC_CRT (OFF by default)
# Jolt is handled inline below (it ignores CMAKE_MSVC_RUNTIME_LIBRARY and
# pins itself via set_target_properties + /MD compile_options).
# We FORCE-write the cache in both branches so flipping PAPI_TYPE between
# v8 and quickjs in an already-configured build/ directory updates these
# knobs cleanly (otherwise stale cached values from the previous configure
# would carry over silently).
# See AGENTS.md 2.7 "Open issue (Win64 /MT vs /MD mismatch)".
if(WIN32 AND PAPI_TYPE STREQUAL "v8")
    message(STATUS "PAPI_TYPE=v8 on Windows: flipping vendored 3rdparty CRT knobs to /MT")
    set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)  # GLFW
    set(CURL_STATIC_CRT ON CACHE BOOL "" FORCE)                # cURL
elseif(WIN32)
    set(USE_MSVC_RUNTIME_LIBRARY_DLL ON  CACHE BOOL "" FORCE)  # GLFW (default)
    set(CURL_STATIC_CRT OFF CACHE BOOL "" FORCE)               # cURL (default)
endif()

# Helper function to configure PuertsCore and PAPI library output directories
# For shared libraries (DLL), output to bin directory alongside executables
# For static libraries, use archive output directory
function(configure_puerts_output PAPI_TARGET)
    if(TARGET PuertsCore)
        set_target_properties(PuertsCore PROPERTIES FOLDER ${third_party_folder}/puerts)
        # Disable Unity Builds for PuertsCore (causes macro conflicts with V8_INLINE)
        set_target_properties(PuertsCore PROPERTIES UNITY_BUILD OFF)
        # Only set DLL output directory if building as shared library
        get_target_property(_puerts_type PuertsCore TYPE)
        if(_puerts_type STREQUAL "SHARED_LIBRARY")
            z_set_config_output_dirs(PuertsCore "${BINARY_ROOT_DIR}" LIBRARY)
        else()
            z_set_config_archive_dirs(PuertsCore "${CMAKE_CURRENT_BINARY_DIR}/puerts")
        endif()
    endif()
    # Also configure the PAPI library output directory (PapiLua, PapiPython, etc.)
    if(TARGET ${PAPI_TARGET})
        get_target_property(_papi_type ${PAPI_TARGET} TYPE)
        if(_papi_type STREQUAL "SHARED_LIBRARY")
            z_set_config_output_dirs(${PAPI_TARGET} "${BINARY_ROOT_DIR}" LIBRARY)
        else()
            z_set_config_archive_dirs(${PAPI_TARGET} "${CMAKE_CURRENT_BINARY_DIR}/puerts")
        endif()
    endif()
endfunction()

# Force PuertsCore and PAPI libraries to build as static libraries
# This eliminates the need for separate DLLs at runtime
# Use regular variables (not CACHE) so they propagate to subdirectories correctly
set(PUERTS_BUILD_STATIC ON)
set(PAPI_BUILD_STATIC ON)

# Add the selected PAPI subdirectory. lua / python / nodejs were removed;
# their source trees stay vendored under puerts/unity/native/ (we don't fork
# upstream) but are never add_subdirectory'd here, so they don't participate
# in the build.
if(PAPI_TYPE STREQUAL "quickjs")
    if(NOT TARGET PapiQuickjs)
        add_subdirectory(puerts/unity/native/papi-quickjs)
        set_target_properties(PapiQuickjs PROPERTIES FOLDER ${third_party_folder}/puerts)
        configure_puerts_output(PapiQuickjs)
    endif()
elseif(PAPI_TYPE STREQUAL "v8")
    # Note: puerts' papi-v8 CMakeLists declares the target as PapiV8 (capital V)
    # -- see engine/3rdparty/puerts/unity/native/papi-v8/CMakeLists.txt.
    # CMake target names are case-sensitive, so we must match exactly.
    if(NOT TARGET PapiV8)
        # papi-v8/CMakeLists.txt expects four uplifted variables describing the
        # chosen V8 build:
        #   - JS_ENGINE          : subdirectory under .backends/ where the V8
        #                          headers + libs live. Defaults to "v8" upstream,
        #                          but in this clone the directory is actually
        #                          named .backends/papi-v8/, so we override.
        #   - BACKEND_INC_NAMES  : include subpaths relative to BACKEND_ROOT
        #   - BACKEND_LIB_NAMES  : library file paths relative to BACKEND_ROOT
        #   - BACKEND_DEFINITIONS: V8 version macros + WITH_INSPECTOR for the
        #                          v8_inspector code path.
        # puerts' upstream uses make_win64.bat / make_linux64.sh / make_osx64.sh
        # to spin these per-platform; we inline them here so that configuring
        # with `-DPAPI_TYPE=v8` on a fresh clone (with .backends/ populated)
        # just works. Keep these in sync with the matching make_* scripts
        # upstream when bumping V8.
        set(JS_ENGINE papi-v8)
        if(WIN32)
            set(BACKEND_INC_NAMES "/Inc")
            set(BACKEND_LIB_NAMES "/Lib/Win64/wee8.lib")
            # V8 13.6 in this clone -> requires C++20 (v8config.h enforces it).
            # papi-v8/CMakeLists.txt picks the C++ standard based on which
            # V8_*_OR_NEWER macro is set: V8_129_OR_NEWER -> C++20,
            # V8_118_OR_NEWER -> C++17, else -> C++14. WITH_INSPECTOR enables
            # the v8_inspector code path that BackendV8::OpenRemoteDebugger
            # will hook into later.
            set(BACKEND_DEFINITIONS "V8_94_OR_NEWER;V8_118_OR_NEWER;V8_129_OR_NEWER;WITH_INSPECTOR")
        elseif(APPLE AND NOT IOS)
            # macOS desktop. .backends/papi-v8/Lib/macOS{,_arm64}/ must be
            # present locally; not vendored.
            set(BACKEND_INC_NAMES "/Inc")
            if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
                set(BACKEND_LIB_NAMES "/Lib/macOS_arm64/libwee8.a")
            else()
                set(BACKEND_LIB_NAMES "/Lib/macOS/libwee8.a")
            endif()
            set(BACKEND_DEFINITIONS "V8_94_OR_NEWER;V8_118_OR_NEWER;V8_129_OR_NEWER;WITH_INSPECTOR")
        elseif(UNIX AND NOT APPLE AND NOT ANDROID)
            set(BACKEND_INC_NAMES "/Inc")
            set(BACKEND_LIB_NAMES "/Lib/Linux/libwee8.a")
            set(BACKEND_DEFINITIONS "V8_94_OR_NEWER;V8_118_OR_NEWER;V8_129_OR_NEWER;WITH_INSPECTOR")
        else()
            message(FATAL_ERROR "PAPI_TYPE=v8 is not yet wired for this platform; "
                                "see engine/3rdparty/CMakeLists.txt and add the "
                                "corresponding BACKEND_* variables.")
        endif()

        # Fail-fast on clean clones where the V8 binary blob hasn't been
        # fetched / built yet. wee8.lib (Win) / libwee8.a (mac/Linux) are NOT
        # vendored in-repo -- upstream puerts ships per-platform make_*
        # scripts under puerts/unity/native/papi-v8/ to materialise them.
        # Since gen_windows.bat now defaults to PAPI_TYPE=v8 (AGENTS.md 2.7),
        # the missing-blob case became the happy path; surfacing it here as
        # a configure-time error with a clear remedy is friendlier than a
        # cryptic LNK1181 / "ld: cannot find -lwee8" hours into the build.
        # BACKEND_LIB_NAMES is a relative path under .backends/papi-v8/ as
        # consumed by puerts/unity/native/papi-v8/CMakeLists.txt; we resolve
        # it the same way here so the diagnostic points at the exact file.
        set(_zengine_v8_backend_root
            "${CMAKE_CURRENT_SOURCE_DIR}/puerts/unity/native/papi-v8/.backends/papi-v8")
        set(_zengine_v8_lib_path "${_zengine_v8_backend_root}${BACKEND_LIB_NAMES}")
        if(NOT EXISTS "${_zengine_v8_lib_path}")
            if(WIN32)
                set(_zengine_v8_make_hint "make_win64.bat")
                set(_zengine_v8_gen_hint "gen_windows.bat --papi quickjs")
            elseif(APPLE)
                set(_zengine_v8_make_hint "make_osx64.sh")
                set(_zengine_v8_gen_hint "gen_macos.sh --papi quickjs")
            else()
                set(_zengine_v8_make_hint "make_linux64.sh")
                set(_zengine_v8_gen_hint "gen_linux.sh --papi quickjs")
            endif()
            message(FATAL_ERROR
                "PAPI_TYPE=v8 selected but the vendored V8 binary is missing:\n"
                "    ${_zengine_v8_lib_path}\n"
                "\n"
                "The wee8 binary blob is not committed to this repo. To fix, either:\n"
                "  (a) Materialise it locally:\n"
                "        cd engine/3rdparty/puerts/unity/native/papi-v8\n"
                "        ${_zengine_v8_make_hint}\n"
                "      (this writes into .backends/papi-v8/Lib/...)\n"
                "  (b) Fall back to the QuickJS backend (no extra binaries needed):\n"
                "        ${_zengine_v8_gen_hint}\n"
                "      or reconfigure with -DPAPI_TYPE=quickjs.")
        endif()
        unset(_zengine_v8_backend_root)
        unset(_zengine_v8_lib_path)
        unset(_zengine_v8_make_hint)
        unset(_zengine_v8_gen_hint)

        add_subdirectory(puerts/unity/native/papi-v8)
        set_target_properties(PapiV8 PROPERTIES FOLDER ${third_party_folder}/puerts)
        configure_puerts_output(PapiV8)
    endif()
endif()

if(NOT TARGET BqLog)
    if(EMSCRIPTEN)
        # BqLog uses POSIX backtrace (<execinfo.h>) and platform-specific inline
        # assembly that are not available on wasm. Provide a stub INTERFACE
        # target so that callers of `target_link_libraries(... BqLog)` keep
        # working; logging on Web should fall back to stdout/console.log via
        # the engine's own logging facade.
        add_library(BqLog INTERFACE)
    else()
        option(BQLOG_BUILD_EXAMPLE "" OFF)
        option(BQLOG_INSTALL "" OFF)
        if(ANDROID)
            set(TARGET_PLATFORM "android" CACHE STRING "Choose the target platform" FORCE)
        elseif(Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
            set(TARGET_PLATFORM "ohos" CACHE STRING "Choose the target platform" FORCE)
        elseif(WIN32)
            set(TARGET_PLATFORM "win64" CACHE STRING "Choose the target platform" FORCE)
        elseif(APPLE)
            if(IOS)
                set(TARGET_PLATFORM "ios" CACHE STRING "Choose the target platform" FORCE)
            else()
                set(TARGET_PLATFORM "mac" CACHE STRING "Choose the target platform" FORCE)
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(TARGET_PLATFORM "linux" CACHE STRING "Choose the target platform" FORCE)
        endif()
        # Force BqLog to build as static library (BqLog uses BUILD_TYPE variable)
        set(BUILD_TYPE "static_lib" CACHE STRING "" FORCE)
        add_subdirectory(bqlog/src)

        set_target_properties(BqLog PROPERTIES FOLDER ${third_party_folder}/BqLog)
        # BqLog's CMake uses a single output dir for win64 (no Debug/Release); force per-config
        # archive output to avoid _ITERATOR_DEBUG_LEVEL mismatch when switching Debug/Release.
        z_set_config_archive_dirs(BqLog "${CMAKE_CURRENT_BINARY_DIR}/bqlog/src")
    endif()
endif()

if(NOT TARGET stb)
    include(stb.cmake)
    set_target_properties(stb PROPERTIES FOLDER ${third_party_folder}/stb)
endif()

if(NOT TARGET tweeny)
    include(tweeny.cmake)
    set_target_properties(tweeny PROPERTIES FOLDER ${third_party_folder}/tweeny)
endif()

if(NOT TARGET tinyobjloader)
    add_subdirectory(tinyobjloader)
    set_target_properties(tinyobjloader PROPERTIES FOLDER ${third_party_folder}/tinyobjloader)
    set_target_properties(uninstall PROPERTIES FOLDER ${third_party_folder}/tinyobjloader)
    z_set_config_archive_dirs(tinyobjloader "${CMAKE_CURRENT_BINARY_DIR}/tinyobjloader")
endif()

if(NOT TARGET Jolt)
    if(EMSCRIPTEN)
        # JoltPhysics uses platform-specific SIMD intrinsics, PCH and compiler
        # flags that don't fly cleanly under emcc out of the box. We skip it on
        # Web for now and expose a stub INTERFACE `Jolt` so the rest of the
        # build keeps configuring. Bringing physics to Web should be done
        # explicitly later (likely via -msimd128 + disabling Jolt's PCH).
        add_library(Jolt INTERFACE)
    else()
        option(TARGET_HELLO_WORLD "" OFF)
        option(TARGET_PERFORMANCE_TEST "" OFF)
        option(TARGET_SAMPLES "" OFF)
        option(TARGET_UNIT_TESTS "" OFF)
        option(TARGET_VIEWER "" OFF)

        if(ENABLE_PHYSICS_DEBUG_RENDERER)
            option(TARGET_TEST_FRAMEWORK "" ON)
        else()
            option(TARGET_TEST_FRAMEWORK "" OFF)
        endif()

        if(APPLE)
            set(JPH_USE_VK OFF CACHE BOOL "Use Vulkan" FORCE)
            set(JPH_USE_MTL ON CACHE BOOL "Use Metal" FORCE)
        endif()

        add_subdirectory(JoltPhysics/Build)

        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            include(CheckCXXCompilerFlag)
            check_cxx_compiler_flag("-Wno-unqualified-std-cast-call" COMPILER_CHECK_UNQUALIFIED)
            if(COMPILER_CHECK_UNQUALIFIED)
                target_compile_options(Jolt PRIVATE "-Wno-unqualified-std-cast-call")
            endif()
            check_cxx_compiler_flag("-Wno-deprecated-copy" COMPILER_CHECK_DEPRECATED_COPY)
            if(COMPILER_CHECK_DEPRECATED_COPY)
                target_compile_options(Jolt PRIVATE "-Wno-deprecated-copy")
            endif()
            if(Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
                target_compile_options(Jolt PRIVATE "-Wno-unused-command-line-argument")
            endif()
            if(Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
                target_compile_options(Jolt PRIVATE "-Wno-unused-command-line-argument")
            endif()
        endif()

        # CRT selection for Jolt:
        # - Default (quickjs path): /MD[d] (DynamicDLL), matches the rest of
        #   ZEngine's Debug/Release builds.
        # - PAPI_TYPE=v8 on Windows: /MT (StaticRelease) to match the
        #   vendored wee8.lib (MT_StaticRelease only). See the top-level
        #   CMakeLists.txt "Plan C" block + AGENTS.md 2.7 for why.
        # - PAPI_TYPE=v8 + DebugV8 config: still /MT + _IDL=0 (NOT /MTd +
        #   _IDL=2), because DebugV8 is "Debug-friendly UX on top of a
        #   Release ABI" -- see Plan C in the top-level CMakeLists.txt.
        #   The generator expressions below are deliberately keyed on
        #   `$<CONFIG:Debug>` only; `DebugV8` falls through to the release
        #   branch, which is what we want.
        if(WIN32 AND PAPI_TYPE STREQUAL "v8")
            set(_jolt_msvc_runtime "MultiThreaded")
            set(_jolt_compile_release_crt "/MT")
            set(_jolt_compile_debug_crt "/MTd")
            set(_jolt_idl_debug "/D_ITERATOR_DEBUG_LEVEL=0")
        else()
            set(_jolt_msvc_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            set(_jolt_compile_release_crt "/MD")
            set(_jolt_compile_debug_crt "/MDd")
            set(_jolt_idl_debug "/D_ITERATOR_DEBUG_LEVEL=2")
        endif()

        if(ENABLE_PHYSICS_DEBUG_RENDERER)
            set_target_properties(Jolt TestFramework
                PROPERTIES
                FOLDER ${third_party_folder}/JoltPhysics
                MSVC_RUNTIME_LIBRARY "${_jolt_msvc_runtime}")

        else()
            # NOTE: when v8 path forces MT, the generator expression resolves
            # to "MultiThreaded" for Release/RelWithDebInfo configs, which
            # is what wee8.lib needs.
            if(WIN32 AND PAPI_TYPE STREQUAL "v8")
                set_target_properties(Jolt
                    PROPERTIES
                    FOLDER ${third_party_folder}/JoltPhysics
                    MSVC_RUNTIME_LIBRARY "${_jolt_msvc_runtime}")
            else()
                set_target_properties(Jolt
                    PROPERTIES
                    FOLDER ${third_party_folder}/JoltPhysics
                    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
            endif()
        endif()

        if(MSVC)
            target_compile_options(
                Jolt PRIVATE "/W4"
                "$<$<CONFIG:Debug>:${_jolt_compile_debug_crt}>"
                "$<$<CONFIG:Debug>:/D_DEBUG>"
                "$<$<CONFIG:Debug>:${_jolt_idl_debug}>"
                "$<$<NOT:$<CONFIG:Debug>>:${_jolt_compile_release_crt}>")
            if(TARGET TestFramework)
                target_compile_options(
                    TestFramework PRIVATE
                    "$<$<CONFIG:Debug>:${_jolt_compile_debug_crt}>"
                    "$<$<CONFIG:Debug>:/D_DEBUG>"
                    "$<$<CONFIG:Debug>:${_jolt_idl_debug}>"
                    "$<$<NOT:$<CONFIG:Debug>>:${_jolt_compile_release_crt}>")
            endif()
        else()
            target_compile_options(Jolt PRIVATE "-Wall")
        endif()
        z_set_config_archive_dirs(Jolt "${CMAKE_CURRENT_BINARY_DIR}/JoltPhysics/Build")
    endif()

endif()

if(NOT TARGET mimalloc-static)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
    # ---------------------------------------------------------------------
    # 关闭 mimalloc 的 arch 优化（默认 arm64 会启用 -march=armv8.1-a，
    # 编译出 LSE 原子指令 CASAL 等）。
    #
    # 在 iOS 上，作为 framework 被 dlopen 加载时，CASAL 指令在某些
    # 设备/模拟器/host 环境下会触发非法指令或访问异常（典型崩点：
    # _mi_atomic_once_enter -> mi_atomic_cas_strong_acq_rel）。
    #
    # 关掉 MI_OPT_ARCH 后，mimalloc 使用基础 armv8-a 指令集，
    # 原子操作回退到 LL/SC（ldaxr/stlxr）序列，兼容所有 iOS arm64 设备。
    # ---------------------------------------------------------------------
    set(MI_OPT_ARCH OFF CACHE BOOL "" FORCE)
    set(MI_NO_OPT_ARCH ON CACHE BOOL "" FORCE)
    # Apple 平台关闭 zone/interpose，避免与 dyld 早期加载冲突。
    if(APPLE)
        set(MI_OSX_ZONE OFF CACHE BOOL "" FORCE)
        set(MI_OSX_INTERPOSE OFF CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(mimalloc)

    # ---------------------------------------------------------------------
    # Apple (iOS / macOS) 上禁用 mimalloc 的 __attribute__((constructor))。
    #
    # 原因：当 mimalloc-static 被链接进一个被 host 进程在运行时 dlopen 的
    # framework / dylib 时（例如 Unity 的 UnityFramework 加载我们的
    # ZRuntimeShared.framework），dyld 在 framework 的 initializers 阶段会
    # 调用 mi_process_attach -> _mi_auto_process_init -> _mi_options_init ->
    # mi_add_stderr_output -> mi_out_buf_flush。这条路径会触碰
    # pthread_mutex / atomic / stderr 等设施；在 iOS 上某些 host（特别是
    # Unity 通过 dlopen 间接加载 framework 的场景）下 dyld 加载阶段执行这些
    # 操作会触发崩溃（参考 init.c:585 附近的注释：在 macOS 上 _tlv_bootstrap
    # 可能与 malloc 相互递归调用）。
    #
    # 定义 MI_PRIM_HAS_PROCESS_ATTACH=1 后，src/prim/prim.c 不再生成
    # __attribute__((constructor))，mimalloc 改为在首次 mi_malloc /
    # mi_thread_init 时通过 mi_process_init() 懒初始化（init.c 已有完整
    # 支持）。我们只通过显式 mi_malloc/mi_free 使用 mimalloc，因此这种
    # 懒初始化对功能没有任何影响，且彻底规避 dylib 加载阶段的崩溃。
    # ---------------------------------------------------------------------
    if(APPLE AND TARGET mimalloc-static)
        target_compile_definitions(mimalloc-static PRIVATE MI_PRIM_HAS_PROCESS_ATTACH=1)
    endif()
    if(APPLE AND TARGET mimalloc-obj)
        target_compile_definitions(mimalloc-obj PRIVATE MI_PRIM_HAS_PROCESS_ATTACH=1)
    endif()

    set_target_properties(mimalloc-static PROPERTIES FOLDER ${third_party_folder}/mimalloc)
    set_target_properties(mimalloc-obj PROPERTIES FOLDER ${third_party_folder}/mimalloc)
    set_target_properties(mimalloc-obj-target PROPERTIES FOLDER ${third_party_folder}/mimalloc)
    z_set_config_archive_dirs(mimalloc-static "${CMAKE_CURRENT_BINARY_DIR}/mimalloc")
endif()

# ImGui removed: the engine, editor and runtime UI render entirely through
# native ZSlate (BatchedUIRenderer + ZFontAtlas). The last ImGui consumers
# (the standalone ZProfiler / ZLauncher dev tools) were deleted, so the imgui
# target and its vendored sources under engine/3rdparty/imgui/ are gone.

if(NOT TARGET lmdb)
    # LMDB is a C library, we need to build it manually
    set(LMDB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lmdb")

    # Try different possible LMDB directory structures
    set(LMDB_SRC_DIR "")
    set(LMDB_INCLUDE_DIR "")

    # Check for common LMDB directory structures
    if(EXISTS "${LMDB_DIR}/libraries/liblmdb/mdb.c")
        set(LMDB_SRC_DIR "${LMDB_DIR}/libraries/liblmdb")
        set(LMDB_INCLUDE_DIR "${LMDB_DIR}/libraries/liblmdb")
    elseif(EXISTS "${LMDB_DIR}/mdb.c")
        set(LMDB_SRC_DIR "${LMDB_DIR}")
        set(LMDB_INCLUDE_DIR "${LMDB_DIR}")
    elseif(EXISTS "${LMDB_DIR}/libs/liblmdb/mdb.c")
        set(LMDB_SRC_DIR "${LMDB_DIR}/libs/liblmdb")
        set(LMDB_INCLUDE_DIR "${LMDB_DIR}/libs/liblmdb")
    else()
        message(WARNING "LMDB source files not found. Please ensure LMDB is cloned to engine/3rdparty/lmdb")
    endif()

    if(LMDB_SRC_DIR)
        # LMDB source files
        set(LMDB_SOURCES
            "${LMDB_SRC_DIR}/mdb.c"
            "${LMDB_SRC_DIR}/midl.c"
        )

        # Create static library
        add_library(lmdb STATIC ${LMDB_SOURCES})
        if(Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
            target_compile_definitions(lmdb PRIVATE MDB_USE_ROBUST=0)
        endif()

        # Include directories (use BUILD/INSTALL generator expressions)
        target_include_directories(lmdb PUBLIC
            $<BUILD_INTERFACE:${LMDB_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include/lmdb>
        )

        # Compiler-specific options
        if(MSVC)
            # MSVC: compile C files as C (not C++)
            set_source_files_properties(${LMDB_SOURCES} PROPERTIES COMPILE_FLAGS "/TC")
            # Enable C99 features
            target_compile_definitions(lmdb PRIVATE _CRT_SECURE_NO_WARNINGS)
        else()
            # For GCC/Clang, compile as C with C99 standard
            set_source_files_properties(${LMDB_SOURCES} PROPERTIES COMPILE_FLAGS "-std=c99")
        endif()

        # Platform-specific definitions
        if(WIN32)
            target_compile_definitions(lmdb PRIVATE _WIN32)
            # Define NTSTATUS if not already defined (needed for LMDB on Windows)
            # NTSTATUS is typically a LONG (32-bit integer) on Windows
            # This fixes the implicit int error when compiling with Clang/MSVC
            # We use compile options to ensure NTSTATUS is defined before windows.h is processed
            if(MSVC)
                # MSVC uses /D for preprocessor definitions
                target_compile_options(lmdb PRIVATE
                    "$<$<COMPILE_LANGUAGE:C>:/DNTSTATUS=LONG>"
                )
            else()
                # Clang/GCC use -D for preprocessor definitions
                target_compile_options(lmdb PRIVATE
                    "$<$<COMPILE_LANGUAGE:C>:-DNTSTATUS=LONG>"
                )
            endif()
        endif()

        set_target_properties(lmdb PROPERTIES FOLDER ${third_party_folder}/lmdb)
        z_set_config_archive_dirs(lmdb "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

if(NOT TARGET xxhash)
    set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
    add_subdirectory(xxHash/build/cmake)
    set_target_properties(xxhash PROPERTIES FOLDER ${third_party_folder}/xxHash)
endif()

# =============================================================================
# minizip-ng -- ZIP container reader/writer, used by the editor's XlsxImporter
# (PR #8) to crack open .xlsx files (which are ZIP archives of XML parts) under
# <Project>/Data/. Only the editor links it; it is NOT pulled into ZRuntime,
# so player builds stay free of the ZIP / zlib payload.
#
# We intentionally strip every optional feature down to the minimum needed
# for the .xlsx use case:
#   * MZ_ZLIB ON         -- DEFLATE is the only compression .xlsx uses; xlsx
#                           stores per-sheet XML compressed with deflate.
#   * MZ_BZIP2/LZMA/PPMD/ZSTD OFF
#                        -- never appear in .xlsx; saves several hundred KB
#                           and avoids fetching extra source trees.
#   * MZ_LIBCOMP OFF on Apple
#                        -- libcompression has API quirks under iOS catalyst;
#                           we use the same zlib code path on every host so
#                           the importer's behaviour stays identical.
#   * MZ_PKCRYPT/WZAES/OPENSSL/LIBBSD OFF
#                        -- no encrypted-archive handling; .xlsx written by
#                           Excel / LibreOffice is unencrypted.
#   * MZ_ICONV OFF       -- .xlsx parts are UTF-8 by spec; we don't need
#                           system iconv to decode filenames.
#   * MZ_COMPAT OFF      -- compat layer adds the legacy zlib unzip.h API
#                           we don't use; turning it off shaves a few TUs.
#   * MZ_BUILD_TESTS / MZ_BUILD_UNIT_TESTS / MZ_BUILD_FUZZ_TESTS OFF
#                        -- save build time; the upstream tests reference
#                           several files only present in their CI cache.
#
# zlib is NOT vendored under engine/3rdparty/. minizip-ng's CMake will
# `FetchContent_Declare(zlib ...)` from upstream the first time CMake
# configures (MZ_FETCH_LIBS defaults to ON on WIN32; we force it on for
# every host for consistency). The fetched copy lives under the build/
# directory and is gitignored. If a developer is offline on first config
# they can pre-place a zlib source tree at ${CMAKE_CURRENT_BINARY_DIR}/_deps/
# or set ZLIB_ROOT to a system install.
#
# CRT selection: minizip-ng is a plain C library that respects
# CMAKE_MSVC_RUNTIME_LIBRARY, so it picks up whatever CRT mode the top-level
# build is using (/MD for the default quickjs path, /MT for PAPI_TYPE=v8;
# see "Plan C" block in the top-level CMakeLists). No special handling like
# Jolt / GLFW needed.
#
# Folder placement: nested under the third_party_folder. The importer's own
# C++ TUs live in engine/Source/Editor/asset_pipeline/data_table_importer/
# and pick up ZEditor's GLOB_RECURSE; only the link-against happens in
# engine/Source/Editor/CMakeLists.txt.
# =============================================================================
if(NOT EMSCRIPTEN)
    set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
    set(MZ_ZLIB ON CACHE BOOL "" FORCE)
    set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
    set(MZ_LZMA OFF CACHE BOOL "" FORCE)
    set(MZ_PPMD OFF CACHE BOOL "" FORCE)
    set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
    set(MZ_LIBCOMP OFF CACHE BOOL "" FORCE)
    set(MZ_PKCRYPT OFF CACHE BOOL "" FORCE)
    set(MZ_WZAES OFF CACHE BOOL "" FORCE)
    set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
    set(MZ_LIBBSD OFF CACHE BOOL "" FORCE)
    set(MZ_ICONV OFF CACHE BOOL "" FORCE)
    set(MZ_FETCH_LIBS ON CACHE BOOL "" FORCE)
    set(MZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MZ_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(MZ_BUILD_FUZZ_TESTS OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

    add_subdirectory(minizip-ng)

    # NOTE: with MZ_COMPAT=OFF the vendored minizip-ng CMake sets
    # MZ_LIB_SUFFIX="-ng" -> the actual library target name is
    # `minizip-ng`, NOT `minizip`. The MINIZIP::minizip alias does
    # NOT make `if(TARGET minizip)` true, so we check the real name.
    if(TARGET minizip-ng)
        set_target_properties(minizip-ng PROPERTIES FOLDER ${third_party_folder}/minizip-ng)
        # Disable Unity Builds for minizip (C library; some TUs share static
        # function names that collide when concatenated).
        set_target_properties(minizip-ng PROPERTIES UNITY_BUILD OFF)
        z_set_config_archive_dirs(minizip-ng "${CMAKE_CURRENT_BINARY_DIR}/minizip-ng")
    endif()
    # The fetched zlibstatic / zlib targets land at the same build folder.
    # Keep them tucked away in the same VS solution group so they don't
    # pollute the top-level project view.
    foreach(_zlib_target IN ITEMS zlibstatic zlib zlib-ng zlib-ng-static example example64 minigzip64 minigzip)
        if(TARGET ${_zlib_target})
            # CMake 4.x rejects set_target_properties on ALIAS targets; resolve
            # the alias to its underlying real target before setting properties
            # (zlib-ng's CMake declares some of these names as ALIAS imports of
            # the actual zlibstatic / zlib-ng-static targets).
            get_target_property(_aliased_target ${_zlib_target} ALIASED_TARGET)
            if(_aliased_target)
                set(_real_target ${_aliased_target})
            else()
                set(_real_target ${_zlib_target})
            endif()
            set_target_properties(${_real_target} PROPERTIES
                FOLDER ${third_party_folder}/minizip-ng/zlib)
            set_target_properties(${_real_target} PROPERTIES UNITY_BUILD OFF)
        endif()
    endforeach()
endif()

# Fallback: if minizip-ng target still doesn't exist, create it manually
if(NOT TARGET minizip-ng)
    message(STATUS "minizip-ng target not found, creating manually")
    set(MINIZIP_NG_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_crypt.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_os.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_buf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_mem.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_split.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_zip.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_zip_rw.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_zlib.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_os_win32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_os_win32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_crypt_winxp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_crypt_winvista.c
    )
    set(MINIZIP_NG_HDR
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_os.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_crypt.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_buf.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_mem.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_split.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_strm_os.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_zip.h
        ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng/mz_zip_rw.h
    )
    add_library(minizip-ng STATIC ${MINIZIP_NG_SRC} ${MINIZIP_NG_HDR})
    target_include_directories(minizip-ng PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/minizip-ng)
    target_link_libraries(minizip-ng PRIVATE zlib-ng)
    set_target_properties(minizip-ng PROPERTIES FOLDER ${third_party_folder}/minizip-ng)
    set_target_properties(minizip-ng PROPERTIES UNITY_BUILD OFF)
    z_set_config_archive_dirs(minizip-ng "${CMAKE_CURRENT_BINARY_DIR}/minizip-ng")
endif()

if(NOT TARGET libcurl AND NOT TARGET libcurl_static AND NOT EMSCRIPTEN)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
    # Disable optional dependencies that may not be available
    set(CURL_ZLIB OFF CACHE STRING "" FORCE)
    set(CURL_BROTLI OFF CACHE STRING "" FORCE)
    set(CURL_ZSTD OFF CACHE STRING "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    if(ANDROID OR IOS OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
        set(CURL_ENABLE_SSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(curl)
    if(TARGET libcurl_static)
        get_target_property(libcurl_static_type libcurl_static TYPE)
        if(NOT libcurl_static_type STREQUAL "ALIAS" AND NOT libcurl_static_type STREQUAL "NOTFOUND")
            set_target_properties(libcurl_static PROPERTIES FOLDER ${third_party_folder}/curl)
            if(TARGET curl-ca-bundle)
                set_target_properties(curl-ca-bundle PROPERTIES FOLDER ${third_party_folder}/curl)
            endif()
            if(TARGET curl-ca-firefox)
                set_target_properties(curl-ca-firefox PROPERTIES FOLDER ${third_party_folder}/curl)
            endif()
            if(TARGET curl-man)
                set_target_properties(curl-man PROPERTIES FOLDER ${third_party_folder}/curl)
            endif()
            if(TARGET curl-opts-man)
                set_target_properties(curl-opts-man PROPERTIES FOLDER ${third_party_folder}/curl)
            endif()
            if(TARGET libcurl_object)
                set_target_properties(libcurl_object PROPERTIES FOLDER ${third_party_folder}/curl)
            endif()
        endif()
    endif()
endif()

# =============================================================================
# Texture-cook block encoders (editor-only). Both are compiled on the HOST
# regardless of the build target, because the Windows editor must be able to
# cross-encode mobile (ASTC) variants. They are linked PRIVATE into ZEditor
# (and the encode smoke test); ZRuntime never links them.
#   - bc7enc_rdo : BC1/BC3/BC7 (richgel999, MIT / public-domain).
#   - astc-encoder : ASTC LDR (ARM, Apache-2.0).
# =============================================================================

# bc7enc_rdo -- BC1/BC3/BC7 block encoders.
if(NOT TARGET bc7enc AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bc7enc_rdo/bc7enc.cpp")
    add_library(bc7enc STATIC
        bc7enc_rdo/bc7enc.cpp
        bc7enc_rdo/rgbcx.cpp
    )
    target_include_directories(bc7enc PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/bc7enc_rdo>
        $<INSTALL_INTERFACE:include/bc7enc_rdo>
    )
    set_target_properties(bc7enc PROPERTIES CXX_STANDARD 17 UNITY_BUILD OFF)
    if(MSVC)
        target_compile_definitions(bc7enc PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(bc7enc PRIVATE /W0)
    else()
        target_compile_options(bc7enc PRIVATE -w)
    endif()
    set_target_properties(bc7enc PROPERTIES FOLDER ${third_party_folder}/bc7enc)
    z_set_config_archive_dirs(bc7enc "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# ARM astc-encoder -- ASTC LDR encoder. Compiled with the no-intrinsics
# (scalar) ISA path forced ON so it builds identically on every host
# regardless of the inherited /arch flag; cooking is an offline step where
# the scalar speed penalty is irrelevant.
if(NOT TARGET astcenc AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/astc-encoder/Source/astcenc_entry.cpp")
    file(GLOB ASTCENC_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/astc-encoder/Source/astcenc_*.cpp")
    add_library(astcenc STATIC ${ASTCENC_SOURCES})
    target_include_directories(astcenc PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/astc-encoder/Source>
        $<INSTALL_INTERFACE:include/astc-encoder>
    )
    target_compile_definitions(astcenc PRIVATE
        ASTCENC_SSE=0 ASTCENC_AVX=0 ASTCENC_POPCNT=0
        ASTCENC_F16C=0 ASTCENC_NEON=0 ASTCENC_SVE=0)
    set_target_properties(astcenc PROPERTIES CXX_STANDARD 17 UNITY_BUILD OFF)
    if(MSVC)
        target_compile_definitions(astcenc PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(astcenc PRIVATE /W0)
    else()
        target_compile_options(astcenc PRIVATE -w)
    endif()
    set_target_properties(astcenc PROPERTIES FOLDER ${third_party_folder}/astc-encoder)
    z_set_config_archive_dirs(astcenc "${CMAKE_CURRENT_BINARY_DIR}")
endif()
