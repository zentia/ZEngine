# =============================================================================
# ZEngine Web (Emscripten) Launcher - minimal browser host
# -----------------------------------------------------------------------------
# Builds a single Emscripten executable target (ZWebLauncher) that produces
# ZWebLauncher.html / .js / .wasm under build/build_web_<config>/bin/<config>/.
#
# This target intentionally avoids the desktop launcher's GLFW + ImGui
# backends. It exists to:
#   * exercise the full Emscripten link graph for libZRuntime.a (catching
#     unresolved symbols at link time, before bring-up of higher-level
#     systems begins),
#   * give developers something runnable in a browser to verify the WebGL2
#     context creation path end-to-end.
# =============================================================================

if(NOT (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten"))
    message(FATAL_ERROR "WebLauncher target is Emscripten-only; it should not be added on this platform.")
endif()

set(TARGET_NAME ZWebLauncher)

set(WEBLAUNCHER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)
set(WEBLAUNCHER_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${WEBLAUNCHER_SOURCES})

add_executable(${TARGET_NAME} ${WEBLAUNCHER_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ZWebLauncher"
    # emcc treats .html as the suffix that triggers full HTML/JS/WASM emission.
    SUFFIX ".html"
    FOLDER "Engine"
)

z_set_config_output_dirs(${TARGET_NAME} "${BINARY_ROOT_DIR}" RUNTIME)

# ---------------------------------------------------------------------------
# Link against the runtime static library. ZRuntime exposes its include dirs
# transitively, so we do not have to repeat target_include_directories here.
# ---------------------------------------------------------------------------
target_link_libraries(${TARGET_NAME} PRIVATE ZRuntime)

# ---------------------------------------------------------------------------
# Reuse the shared precompiled header so this TU sees the same EASTL / std /
# rapidjson configuration as the rest of the engine. Without this main.cpp
# would pull in headers that depend on PCH-supplied symbols (eastl::*,
# std::unordered_set forward-includes, etc.) and fail to compile.
# Mirrors the desktop Launcher in engine/Source/Launcher/CMakeLists.txt.
# ---------------------------------------------------------------------------
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16" AND TARGET ZEnginePCH)
    target_link_libraries(${TARGET_NAME} PRIVATE ZEnginePCH)
    target_include_directories(${TARGET_NAME} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(${TARGET_NAME} ZEnginePCH)
    target_precompile_headers(${TARGET_NAME} REUSE_FROM ZEnginePCH)
    message(STATUS "WebLauncher: precompiled headers reused from ZEnginePCH")
endif()

# ---------------------------------------------------------------------------
# Emscripten link-time settings.
#
# We keep the option set deliberately small and explicit:
#   USE_WEBGL2=1            request a WebGL 2.0 context
#   FULL_ES3=1              expose the full GLES3 entrypoint table to GL code
#   MIN_WEBGL_VERSION=2     refuse to fall back to WebGL1
#   MAX_WEBGL_VERSION=2
#   ALLOW_MEMORY_GROWTH=1   resize the wasm heap on demand (required by the
#                           runtime's asset / preload managers)
#   INITIAL_MEMORY=64MB     reasonable starting heap for the bring-up loop
#   STACK_SIZE=2MB          larger than the 64KB default; the engine recurses
#                           through serialization paths that overflow easily
#   ENVIRONMENT=web         strip node/worker scaffolding from the loader
#   EXIT_RUNTIME=0          we use emscripten_set_main_loop with
#                           simulate_infinite_loop=1; never return from main
#   FILESYSTEM=1            keep MEMFS so the runtime's FileSystem can mount
#                           preloaded assets later (cheap; ~30KB)
# ---------------------------------------------------------------------------
set(_WEB_LINK_FLAGS
    "-sUSE_WEBGL2=1"
    "-sFULL_ES3=1"
    "-sMIN_WEBGL_VERSION=2"
    "-sMAX_WEBGL_VERSION=2"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=67108864"
    "-sSTACK_SIZE=2097152"
    "-sENVIRONMENT=web"
    "-sEXIT_RUNTIME=0"
    "-sFILESYSTEM=1"
    "-sFETCH=1"
    "-sASSERTIONS=1"
    "-sNO_DISABLE_EXCEPTION_CATCHING"
    "-sALLOW_UNIMPLEMENTED_SYSCALLS=1"
    # mimalloc's Emscripten primitive backend (engine/3rdparty/mimalloc/src/
    # prim/emscripten/prim.c) calls emmalloc_free / emmalloc_memalign as the
    # underlying system allocator. Those symbols are only emitted by emcc
    # when the system malloc is selected as 'emmalloc'. Without this the
    # link fails with `undefined symbol: emmalloc_free / emmalloc_memalign`.
    "-sMALLOC=emmalloc"
    # Keep the canvas selector hard-coded to '#canvas' to match the shell.
    # Override at the command line if you embed a different DOM id.
    "--shell-file=${WEBLAUNCHER_SHELL}"
)

# Debug builds: keep DWARF in the wasm and the JS loader unminified so the
# browser devtools "Sources" tab shows real C++ frames.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_CONFIGURATION_TYPES)
    list(APPEND _WEB_LINK_FLAGS "-g")
    list(APPEND _WEB_LINK_FLAGS "-sSAFE_HEAP=0") # too noisy with EASTL containers
endif()

target_link_options(${TARGET_NAME} PRIVATE ${_WEB_LINK_FLAGS})
unset(_WEB_LINK_FLAGS)

# Rebuild whenever the shell template changes.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    APPEND PROPERTY OBJECT_DEPENDS "${WEBLAUNCHER_SHELL}")

# Make sure the per-config bin/ directory exists for the .html/.js/.wasm trio
# (Emscripten does not create it by itself for non-default output dirs).
add_custom_command(TARGET ${TARGET_NAME} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TARGET_NAME}>"
    VERBATIM
)

message(STATUS "WebLauncher: target ${TARGET_NAME} configured (shell=${WEBLAUNCHER_SHELL})")
