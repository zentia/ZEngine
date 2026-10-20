# =====================================================================
# ShaderLab Unity-compatibility smoke-test
# ---------------------------------------------------------------------
# Builds a standalone executable that feeds a REAL, unmodified Unity CG
# shader (Spine/Skeleton Tint) through ShaderLab -> ShaderLabDx12Compiler
# -> DXC, with a minimal UnityCG.cginc shim next to the .shader.
#
# Default OFF -- no normal build is affected. Opt in with:
#
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_UNITY_COMPAT_TEST=ON
#   cmake --build build --target ShaderLabUnityCompatTest
#   ./build/.../ShaderLabUnityCompatTest
#
# It answers three questions at once:
#   1. Does ShaderLabParser accept real Unity ShaderLab syntax?
#   2. Does the DX12 include handler resolve `#include "UnityCG.cginc"`
#      relative to the .shader directory?
#   3. Does DXC accept the Cg-flavoured body given the shim?
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_UNITY_COMPAT_TEST
       "Build the ShaderLab Unity-compatibility smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_SHADERLAB_UNITY_COMPAT_TEST)
    return()
endif()

# DX12ShaderCompiler.cpp is only compiled into ZRuntime on Windows (the
# dx12/ tree is excluded everywhere else), so the test cannot link off it.
if(NOT WIN32)
    message(WARNING
        "ZENGINE_BUILD_SHADERLAB_UNITY_COMPAT_TEST is ON but the host is not "
        "Windows -- ShaderLabDx12Compiler is unavailable. Skipping.")
    return()
endif()

set(_smoke_target ShaderLabUnityCompatTest)

add_executable(${_smoke_target}
    ShaderLabUnityCompatTest.cpp
)

set_target_properties(${_smoke_target} PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Absolute path to this directory, so the test finds its fixtures without
# depending on CWD.
target_compile_definitions(${_smoke_target} PRIVATE
    ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# ZRuntime carries ShaderLabDx12Compiler + DX12ShaderCompiler + the logging
# macros they pull in transitively.
target_link_libraries(${_smoke_target} PRIVATE
    ZRuntime
    d3d12
    dxgi
)

# ZRuntime PRIVATE-links ZEnginePCH (which carries EASTL's include dirs), so
# its INTERFACE include paths do not propagate here. Same pattern as the DX12
# bindless smoke-test.
if(TARGET ZEnginePCH)
    target_link_libraries(${_smoke_target} PRIVATE ZEnginePCH)
    target_include_directories(${_smoke_target} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(${_smoke_target} ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(${_smoke_target} PRIVATE /W4)
endif()

# Fixtures, shipped into the IDE tree but not compiled at build time --
# they are read and compiled at runtime, which is the whole point.
target_sources(${_smoke_target} PRIVATE
    unity_compat/UnityCG.cginc
    unity_compat/Spine-Skeleton-Tint.shader
    unity_compat/Spine-Skeleton-Tint-SemanticFixed.shader
    unity_compat/Spine-Skeleton-Tint-CasterOnly.shader
    unity_compat/diag_props.shader
    unity_compat/diag_props2.shader
    unity_compat/diag_pass_variants.shader
    unity_compat/diag_pass_variants2.shader
    unity_compat/diag_semantic_macro.shader
    unity_compat/diag_missing_include.shader
    unity_compat/diag_two_passes.shader
)
set_source_files_properties(
    unity_compat/UnityCG.cginc
    unity_compat/Spine-Skeleton-Tint.shader
    unity_compat/Spine-Skeleton-Tint-SemanticFixed.shader
    unity_compat/Spine-Skeleton-Tint-CasterOnly.shader
    unity_compat/diag_props.shader
    unity_compat/diag_props2.shader
    unity_compat/diag_pass_variants.shader
    unity_compat/diag_pass_variants2.shader
    unity_compat/diag_semantic_macro.shader
    unity_compat/diag_missing_include.shader
    unity_compat/diag_two_passes.shader
    PROPERTIES VS_TOOL_OVERRIDE "None"
)

add_test(NAME ${_smoke_target} COMMAND ${_smoke_target})

message(STATUS "Configured ShaderLab Unity-compat smoke-test target: ${_smoke_target}")
