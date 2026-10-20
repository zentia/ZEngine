# =====================================================================
# ShaderLab SPIR-V cache-manifest smoke-test
# ---------------------------------------------------------------------
# Pins down the invalidation rule for on-disk SPIR-V blobs. Like the
# include resolver, ShaderLabCacheManifest.cpp is deliberately free of any
# render-backend dependency, so this target configures and runs even with
# ZENGINE_USE_VULKAN=OFF -- where ShaderLabCompiler.cpp (the only real
# consumer) is excluded from ZRuntime and would otherwise be unverifiable.
#
# Opt in with:
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_CACHE_TEST=ON
#   cmake --build build --target ShaderLabCacheManifestTest
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_CACHE_TEST
       "Build the ShaderLab SPIR-V cache-manifest smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_CACHE_TEST)
    set(_cache_target ShaderLabCacheManifestTest)

    add_executable(${_cache_target}
        ShaderLabCacheManifestTest.cpp
    )

    set_target_properties(${_cache_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_cache_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    # The manifest lives in ZRuntime (ShaderLab/*.cpp is globbed into it on
    # every configuration that compiles the ShaderLab sources).
    target_link_libraries(${_cache_target} PRIVATE ZRuntime)

    if(MSVC)
        target_compile_options(${_cache_target} PRIVATE /W4)
    endif()

    add_test(NAME ${_cache_target} COMMAND ${_cache_target})

    message(STATUS "Configured ShaderLab cache-manifest smoke-test: ${_cache_target}")
endif()

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

# =====================================================================
# ShaderLab #include resolver smoke-test
# ---------------------------------------------------------------------
# Pins down ResolveIncludes() in ShaderLabIncludeProcessor.cpp. That TU is
# deliberately free of any render-backend dependency (no Vulkan, no DX12),
# so this target configures and runs on EVERY configuration -- including
# ZENGINE_USE_VULKAN=OFF, where ShaderLabCompiler.cpp is excluded from
# ZRuntime and would otherwise be completely unverified.
#
# Opt in with:
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_INCLUDE_TEST=ON
#   cmake --build build --target ShaderLabIncludeResolverTest
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_INCLUDE_TEST
       "Build the ShaderLab #include resolver smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_INCLUDE_TEST)
    set(_include_target ShaderLabIncludeResolverTest)

    add_executable(${_include_target}
        ShaderLabIncludeResolverTest.cpp
    )

    set_target_properties(${_include_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # Absolute fixture path, so the test does not depend on CWD.
    target_compile_definitions(${_include_target} PRIVATE
        ZENGINE_SHADERLAB_INCLUDE_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_include_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    # ResolveIncludes() lives in ZRuntime (ShaderLab/*.cpp is globbed into it
    # on every configuration that compiles the ShaderLab sources).
    target_link_libraries(${_include_target} PRIVATE ZRuntime)

    if(MSVC)
        target_compile_options(${_include_target} PRIVATE /W4)
    endif()

    # Fixtures -- read at runtime, never compiled.
    target_sources(${_include_target} PRIVATE
        include_proc/lib/once.h
        include_proc/lib/plain.h
        include_proc/lib/angle.h
        include_proc/lib/empty.h
        include_proc/lib/self_cycle.h
        include_proc/lib/self_once.h
        include_proc/lib/cycle_a.h
        include_proc/lib/cycle_b.h
        include_proc/lib/nested_parent.h
        include_proc/lib/nested_child.h
        include_proc/owning/local.h
    )
    set_source_files_properties(
        include_proc/lib/once.h
        include_proc/lib/plain.h
        include_proc/lib/angle.h
        include_proc/lib/empty.h
        include_proc/lib/self_cycle.h
        include_proc/lib/self_once.h
        include_proc/lib/cycle_a.h
        include_proc/lib/cycle_b.h
        include_proc/lib/nested_parent.h
        include_proc/lib/nested_child.h
        include_proc/owning/local.h
        PROPERTIES VS_TOOL_OVERRIDE "None"
    )

    add_test(NAME ${_include_target} COMMAND ${_include_target})

    message(STATUS "Configured ShaderLab include-resolver smoke-test: ${_include_target}")
endif()

# =====================================================================
# ShaderLab Unity-compatibility smoke-test
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
#
# ENGINE_ROOT_DIR is <repo>/engine. engine/shader/*.shader include via
# project-relative paths (`#include "shader/include/common.h"`), so the
# include root for those files IS ENGINE_ROOT_DIR -- not .../engine/shader.
target_compile_definitions(${_smoke_target} PRIVATE
    ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    ZENGINE_SHADERLAB_ENGINE_SHADER_DIR="${ENGINE_ROOT_DIR}"
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
    unity_compat/inc_root/lib/shared.h
    unity_compat/inc_root/shaders/inc_root_proj.shader
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
    unity_compat/inc_root/lib/shared.h
    unity_compat/inc_root/shaders/inc_root_proj.shader
    PROPERTIES VS_TOOL_OVERRIDE "None"
)

add_test(NAME ${_smoke_target} COMMAND ${_smoke_target})

message(STATUS "Configured ShaderLab Unity-compat smoke-test target: ${_smoke_target}")
