# =====================================================================
# PR5c: DX12 bindless smoke-test
# ---------------------------------------------------------------------
# Builds a standalone executable that validates the DX12 bindless
# toolchain end-to-end (DXC -> root signature -> graphics PSO).
#
# Default OFF -- no normal build is affected. Opt in with:
#
#   cmake -B build -DZENGINE_BUILD_BINDLESS_SMOKE_TEST=ON
#   cmake --build build --target DX12BindlessSmokeTest
#   ./build/.../DX12BindlessSmokeTest
#
# Why a separate target instead of CTest / GoogleTest:
#   - The engine doesn't ship a test harness yet (only `shader_lab_test`
#     exists, and it's a manually-invoked main() too). PR5c isn't the
#     right place to introduce one -- this PR's contract is "smoke test
#     for the bindless toolchain", not "design a test framework for
#     ZEngine".
#   - The test deliberately runs OUTSIDE the editor / launcher, so it
#     can validate the toolchain on a headless / WARP host without
#     dragging in the window-system manager.
# =====================================================================

option(ZENGINE_BUILD_BINDLESS_SMOKE_TEST
       "Build the DX12 bindless smoke-test executable (PR5c)"
       OFF)

if(NOT ZENGINE_BUILD_BINDLESS_SMOKE_TEST)
    return()
endif()

# Sanity gate: the test pulls in d3d12.lib + dxcompiler at runtime, so
# it only makes sense on Windows builds where DX12 is enabled. This
# matches the platform gating that already excludes dx12/*.cpp from
# ZRuntime on Apple / Mobile / Web.
if(NOT WIN32)
    message(WARNING
        "ZENGINE_BUILD_BINDLESS_SMOKE_TEST is ON but the host platform is not "
        "Windows -- DX12 bindless smoke-test cannot be built. Skipping.")
    return()
endif()

set(_smoke_target DX12BindlessSmokeTest)

add_executable(${_smoke_target}
    DX12BindlessSmokeTest.cpp
)

set_target_properties(${_smoke_target} PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Inject the absolute path to this directory so the test can locate
# its .hlsl sources without depending on CWD. Falls back to a CWD-
# relative search at runtime if the macro is somehow unavailable
# (see resolveShaderPaths in dx12_bindless_smoke_test.cpp).
target_compile_definitions(${_smoke_target} PRIVATE
    ZENGINE_BINDLESS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# We need ZRuntime for:
#   - DX12ShaderCompiler (the unit under test).
#   - Logging macros pulled in transitively by dx12_shader_compiler.cpp.
#   - render_type.h's RHI_SHADER_STAGE_*_BIT enums.
# Linking ZRuntime PRIVATE so the smoke-test's main() doesn't get
# WHOLEARCHIVE'd into anything else (matches the
# REMOVE_ITEM-from-SOURCE_FILES policy for shader_lab_test).
target_link_libraries(${_smoke_target} PRIVATE
    ZRuntime
    # System DX12 / DXGI -- ZRuntime already PUBLIC-links these on
    # Windows, but listing them here makes the dependency explicit and
    # safe even if ZRuntime's link policy changes.
    d3d12
    dxgi
)

# ZRuntime PRIVATE-links ZEnginePCH (which carries EASTL's include
# directories), so its INTERFACE include paths do NOT propagate to
# downstream consumers. The smoke-test transitively pulls EASTL via
# rhi.h -> render_type.h -> core/base/hash.h -> <EASTL/functional.h>,
# so we must wire the same PCH dependency that every other engine
# subproject does (Editor / Launcher / WebLauncher / profiler all
# follow this exact pattern -- see their CMakeLists for prior art).
# We do NOT REUSE_FROM the PCH here -- the smoke-test is intentionally
# a stripped-down TU and the cost of a fresh PCH compile is < 1s on
# a one-file executable.
if(TARGET ZEnginePCH)
    target_link_libraries(${_smoke_target} PRIVATE ZEnginePCH)
    target_include_directories(${_smoke_target} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(${_smoke_target} ZEnginePCH)
endif()

# Match the rest of the engine's MSVC warning policy without /WX so a
# new SM 6.6 deprecation warning from the Windows SDK doesn't break
# this scratch build.
if(MSVC)
    target_compile_options(${_smoke_target} PRIVATE /W4)
endif()

# Ship the .hlsl source files into the IDE project tree so they're
# easy to find / edit alongside the test main. They are NOT compiled
# at build time -- DXC is invoked at runtime from the test, which is
# the entire point of the toolchain validation.
target_sources(${_smoke_target} PRIVATE
    bindless_smoke.hlsl
    bindless_smoke_vs.hlsl
)
set_source_files_properties(
    bindless_smoke.hlsl
    bindless_smoke_vs.hlsl
    PROPERTIES VS_TOOL_OVERRIDE "None" # don't let VS try to FXC them
)

message(STATUS "Configured DX12 bindless smoke-test target: ${_smoke_target}")
