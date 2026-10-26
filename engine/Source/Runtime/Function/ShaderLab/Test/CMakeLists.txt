# =====================================================================
# ShaderLab legacy output-semantic rewrite smoke-test
# ---------------------------------------------------------------------
# Pins down the Cg -> Shader-Model-4+ fix (`: COLOR` -> `: SV_Target`) that
# lets a byte-identical Unity shader compile on a modern target. Backend-free
# like the two above, so it runs on every configuration; the end-to-end proof
# that DXC actually accepts the result lives in ShaderLabUnityCompatTest,
# which needs dxcompiler.dll.
#
# Opt in with:
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_SEMANTIC_TEST=ON
#   cmake --build build --target ShaderLabSemanticRewriteTest
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_SEMANTIC_TEST
       "Build the ShaderLab legacy output-semantic rewrite smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_SEMANTIC_TEST)
    set(_semantic_target ShaderLabSemanticRewriteTest)

    add_executable(${_semantic_target}
        ShaderLabSemanticRewriteTest.cpp
    )

    set_target_properties(${_semantic_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_semantic_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    target_link_libraries(${_semantic_target} PRIVATE ZRuntime)

    # ShaderLabSemanticRewrite.h pulls in RenderType.h -> Core/Base/Hash.h ->
    # <EASTL/functional.h>. ZRuntime PRIVATE-links ZEnginePCH, so EASTL's
    # include dir does not reach us through the edge above; link the PCH like
    # the Unity-compat test does.
    if(TARGET ZEnginePCH)
        target_link_libraries(${_semantic_target} PRIVATE ZEnginePCH)
        target_include_directories(${_semantic_target} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(${_semantic_target} ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(${_semantic_target} PRIVATE /W4)
    endif()

    add_test(NAME ${_semantic_target} COMMAND ${_semantic_target})

    message(STATUS "Configured ShaderLab semantic-rewrite smoke-test: ${_semantic_target}")
endif()

# =====================================================================
# ShaderLab surface-shader desugaring smoke-test
# ---------------------------------------------------------------------
# Pins down ParseSurfaceDirective() and GenerateSurfacePasses() -- the
# `#pragma surface` expansion that turns one Pass-less CGPROGRAM under a
# SubShader into the ordinary passes every backend already understands.
# Backend-free like the ones below (ShaderLabSurface.h pulls in nothing but
# <string>/<vector>), so it configures and runs on every configuration.
# The end-to-end proof that DXC accepts the generated code lives in
# ShaderLabUnityCompatTest.
#
# Opt in with:
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_SURFACE_TEST=ON
#   cmake --build build --target ShaderLabSurfaceTest
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_SURFACE_TEST
       "Build the ShaderLab surface-shader desugaring smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_SURFACE_TEST)
    set(_surface_target ShaderLabSurfaceTest)

    add_executable(${_surface_target}
        ShaderLabSurfaceTest.cpp
    )

    set_target_properties(${_surface_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_surface_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    # The generator lives in ZRuntime (ShaderLab/*.cpp is globbed into it on
    # every configuration that compiles the ShaderLab sources).
    target_link_libraries(${_surface_target} PRIVATE ZRuntime)

    if(MSVC)
        target_compile_options(${_surface_target} PRIVATE /W4)
    endif()

    add_test(NAME ${_surface_target} COMMAND ${_surface_target})

    message(STATUS "Configured ShaderLab surface-shader smoke-test: ${_surface_target}")
endif()

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
# ShaderLab constant-buffer reflection smoke-test
# ---------------------------------------------------------------------
# Pins down "property name -> UBO byte offset", the piece that lets a script
# say SetFloat("_WhateverYourShaderDeclared", v) instead of the hard-coded
# names in MaterialCustomParamNames.h.
#
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_CBUFFER_TEST=ON
#   cmake --build build --target ShaderLabCbufferReflectionTest
#
# Unlike the three above this one does NOT link ZRuntime -- it compiles only
# ShaderCbufferReflection.cpp, which depends on nothing but <string> / <vector>.
# That is deliberate: the reflection is pure CPU and must be runnable on a host
# with no GPU and no shader compiler.
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_CBUFFER_TEST
       "Build the ShaderLab constant-buffer reflection smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_CBUFFER_TEST)
    set(_cbuffer_target ShaderLabCbufferReflectionTest)

    add_executable(${_cbuffer_target}
        ShaderLabCbufferReflectionTest.cpp
        ../ShaderCbufferReflection.cpp
    )

    set_target_properties(${_cbuffer_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Function/ShaderLab/..." includes.
    target_include_directories(${_cbuffer_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    if(MSVC)
        target_compile_options(${_cbuffer_target} PRIVATE /W4)
    else()
        target_compile_options(${_cbuffer_target} PRIVATE -Wall)
    endif()

    add_test(NAME ${_cbuffer_target} COMMAND ${_cbuffer_target})

    message(STATUS "Configured ShaderLab cbuffer reflection smoke-test: ${_cbuffer_target}")
endif()

# =====================================================================
# ShaderLab property-blob smoke-test
# ---------------------------------------------------------------------
# Pins down "which bytes of the material UBO does this property name write",
# the CPU half of `SetFloat("_WhateverYourShaderDeclared", v)`. Everything
# downstream of it (staging buffer -> CopyBuffer -> descriptor set) needs a
# GPU, so this is the only part of the write path verifiable without one.
#
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_PROPERTY_BLOB_TEST=ON
#   cmake --build build --target ShaderLabPropertyBlobTest
#
# Like the cbuffer test it does NOT link ZRuntime -- ShaderPropertyBlob.h is
# header-only and depends on nothing but <cstring> / <string> / <vector>.
# That is deliberate: it is pure CPU and must run on a host with no GPU and no
# shader compiler. (It also has to survive the configurations that strip
# Function/ShaderLab/*.cpp from ZRuntime -- see Runtime/CMakeLists.txt.)
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_PROPERTY_BLOB_TEST
       "Build the ShaderLab property-blob smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_PROPERTY_BLOB_TEST)
    set(_blob_target ShaderLabPropertyBlobTest)

    add_executable(${_blob_target}
        ShaderLabPropertyBlobTest.cpp
    )

    set_target_properties(${_blob_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_blob_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    if(MSVC)
        target_compile_options(${_blob_target} PRIVATE /W4)
    else()
        target_compile_options(${_blob_target} PRIVATE -Wall)
    endif()

    add_test(NAME ${_blob_target} COMMAND ${_blob_target})

    message(STATUS "Configured ShaderLab property-blob smoke-test: ${_blob_target}")
endif()

# =====================================================================
# ShaderLab texture-slot smoke-test
# ---------------------------------------------------------------------
# Pins down "2D property -> which of the 5 material texture bindings", the
# CPU half of `SetTexture("_BumpMap", t)`. The rule is **declaration order**
# in Properties{}: the material descriptor set has 6 bindings (0 = UBO,
# 1..5 = the five PBR textures) and the pool is sized `3 + 5 *
# m_MaxMaterialCount`, so custom shaders reuse 1..5 instead of adding
# bindings. That caps a shader at 5 textures and makes reordering
# properties move slots -- both pinned by the test.
#
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_TEXTURE_SLOT_TEST=ON
#   cmake --build build --target ShaderLabTextureSlotTest
#
# Like the cbuffer / property-blob tests it does NOT link ZRuntime --
# ShaderTextureSlot.h is header-only and depends on nothing but
# <cstdint> / <string> / <vector>.
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_TEXTURE_SLOT_TEST
       "Build the ShaderLab texture-slot smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_TEXTURE_SLOT_TEST)
    set(_texslot_target ShaderLabTextureSlotTest)

    add_executable(${_texslot_target}
        ShaderLabTextureSlotTest.cpp
    )

    set_target_properties(${_texslot_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_texslot_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    if(MSVC)
        target_compile_options(${_texslot_target} PRIVATE /W4)
    else()
        target_compile_options(${_texslot_target} PRIVATE -Wall)
    endif()

    add_test(NAME ${_texslot_target} COMMAND ${_texslot_target})

    message(STATUS "Configured ShaderLab texture-slot smoke-test: ${_texslot_target}")
endif()

# =====================================================================
# ShaderLab program-extract smoke-test
# ---------------------------------------------------------------------
# Pins down "which program block does a backend actually compile". A `.shader`
# may declare both HLSLPROGRAM (DX12) and GLSLPROGRAM (Vulkan / Metal) in the
# same Pass; extraction must be language-filtered, or the wrong body gets
# handed to the wrong compiler and every backend pays for a failed compile
# before falling back to the built-in shader.
#
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_PROGRAM_EXTRACT_TEST=ON
#   cmake --build build --target ShaderLabProgramExtractTest
#
# Backend-free like the cbuffer test (no Vulkan, no DX12, no GPU). It links
# ZRuntime only because the parser chain pulls in RenderType.h / EASTL.
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_PROGRAM_EXTRACT_TEST
       "Build the ShaderLab program-extract smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_PROGRAM_EXTRACT_TEST)
    set(_extract_target ShaderLabProgramExtractTest)

    add_executable(${_extract_target}
        ShaderLabProgramExtractTest.cpp
    )

    set_target_properties(${_extract_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_extract_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    # ExtractHlslFromShaderLab* lives in ZRuntime (ShaderLab/*.cpp is globbed
    # into it); ShaderLabSemanticRewrite.h pulls in RenderType.h -> EASTL.
    target_link_libraries(${_extract_target} PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(${_extract_target} PRIVATE ZEnginePCH)
        target_include_directories(${_extract_target} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(${_extract_target} ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(${_extract_target} PRIVATE /W4)
    else()
        target_compile_options(${_extract_target} PRIVATE -Wall)
    endif()

    add_test(NAME ${_extract_target} COMMAND ${_extract_target})
    message(STATUS "Configured ShaderLab program-extract smoke-test: ${_extract_target}")
endif()

# =====================================================================
# ShaderLab property-layout smoke-test
# ---------------------------------------------------------------------
# Pins down the JOIN between a ShaderLab `Properties {}` block and the
# `CBUFFER_START(UnityPerMaterial)` block -- the thing that lets a script call
# SetFloat("_WhateverYourShaderDeclared", v) on a custom shader. Each block was
# already reachable on its own, but nothing connected them, so the runtime had
# property names with no byte offsets.
#
#   cmake -B build -DZENGINE_BUILD_SHADERLAB_PROPERTY_LAYOUT_TEST=ON
#   cmake --build build --target ShaderLabPropertyLayoutTest
#
# Backend-free: no Vulkan, no DX12, no GPU. Links ZRuntime because the parser
# chain pulls in RenderType.h / EASTL.
# =====================================================================

option(ZENGINE_BUILD_SHADERLAB_PROPERTY_LAYOUT_TEST
       "Build the ShaderLab property-layout smoke-test executable"
       OFF)

if(ZENGINE_BUILD_SHADERLAB_PROPERTY_LAYOUT_TEST)
    set(_layout_target ShaderLabPropertyLayoutTest)

    add_executable(${_layout_target}
        ShaderLabPropertyLayoutTest.cpp
    )

    set_target_properties(${_layout_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # engine/Source is the root for "Runtime/Function/ShaderLab/..." includes.
    target_include_directories(${_layout_target} PRIVATE ${ENGINE_ROOT_DIR}/Source)

    target_link_libraries(${_layout_target} PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(${_layout_target} PRIVATE ZEnginePCH)
        target_include_directories(${_layout_target} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(${_layout_target} ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(${_layout_target} PRIVATE /W4)
    else()
        target_compile_options(${_layout_target} PRIVATE -Wall)
    endif()

    add_test(NAME ${_layout_target} COMMAND ${_layout_target})
    message(STATUS "Configured ShaderLab property-layout smoke-test: ${_layout_target}")
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
    unity_compat/Surface-Standard.shader
    unity_compat/Surface-Lambert-AddShadow.shader
    unity_compat/Surface-NormalMap-AlphaTest.shader
    unity_compat/Surface-Custom-Lighting.shader
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
    unity_compat/Surface-Standard.shader
    unity_compat/Surface-Lambert-AddShadow.shader
    unity_compat/Surface-NormalMap-AlphaTest.shader
    unity_compat/Surface-Custom-Lighting.shader
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
