# =====================================================================
# AppleSceneBlit smoke test.
#
# Own main(); carved out of the ZRuntime glob in Runtime/CMakeLists.txt so its
# main() cannot be dragged into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST=ON
#   cmake --build build --target AppleSceneBlitSmokeTest
#
# This is the only guard on the MSL that carries the Metal scene onto the
# swapchain: AppleSceneBlit keeps its shaders as string literals in the .cpp, so
# they first meet a compiler at runtime (newLibraryWithSource:). Without this
# test a typo in that string is a fully green build and a black window.
#
# Off Apple it degrades to the source-text contract checks (descriptor slots,
# entry-point names, vertex count, cull mode) -- those are platform-independent
# and still worth running.
# =====================================================================

option(ZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST
       "Build the AppleSceneBlit (Metal scene -> swapchain) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST)
    add_executable(AppleSceneBlitSmokeTest AppleSceneBlitSmokeTest.cpp)

    set_target_properties(AppleSceneBlitSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # On Apple this TU includes MetalRHI.h (RHI's Initialize/Shutdown are
    # protected, so the test has to drive the concrete backend), and MetalRHI.h
    # pulls in <Metal/Metal.h> + <QuartzCore/CAMetalLayer.h> -- Objective-C
    # headers. Same treatment RegisterRuntime.cpp gets in Runtime/CMakeLists.txt:
    # compile it as ObjC++ and opt out of the C++ PCH, which was built for C++.
    if(APPLE)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/AppleSceneBlitSmokeTest.cpp"
            PROPERTIES LANGUAGE OBJCXX SKIP_PRECOMPILE_HEADERS ON)
    endif()

    target_link_libraries(AppleSceneBlitSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(AppleSceneBlitSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(AppleSceneBlitSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(AppleSceneBlitSmokeTest ZEnginePCH)
        target_precompile_headers(AppleSceneBlitSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    # The source-text contract checks read AppleSceneBlit.cpp as text. CMake hands
    # the test the absolute engine root so it does not depend on the working
    # directory.
    if(DEFINED ENGINE_ROOT_DIR)
        target_compile_definitions(AppleSceneBlitSmokeTest PRIVATE
            "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")
    endif()

    if(MSVC)
        target_compile_options(AppleSceneBlitSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(AppleSceneBlitSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME AppleSceneBlitSmokeTest COMMAND AppleSceneBlitSmokeTest)

    message(STATUS "Configured AppleSceneBlit smoke test: AppleSceneBlitSmokeTest")
endif()

# =====================================================================
# Metal SPIR-V shader smoke test.
#
# Guards the two links in "built-in shader -> Metal" that the build can never
# check: whether the passes hand their SPIR-V blobs to the RHI at all
# (Z_HAS_SPIRV_SHADERS) and whether SPIRV-Cross's MSL survives the runtime
# Metal compiler (sampler indices <= 15, subpassInput slots, ...).
#
#   cmake -B build -DZENGINE_BUILD_METAL_SPIRV_SHADER_SMOKE_TEST=ON
#   cmake --build build --target MetalSpirvShaderSmokeTest
#
# Off Apple it degrades to the blob sanity checks (size, alignment, magic).
# =====================================================================

option(ZENGINE_BUILD_METAL_SPIRV_SHADER_SMOKE_TEST
       "Build the built-in SPIR-V -> MSL smoke-test executable"
       OFF)

if(ZENGINE_BUILD_METAL_SPIRV_SHADER_SMOKE_TEST)
    add_executable(MetalSpirvShaderSmokeTest MetalSpirvShaderSmokeTest.cpp)

    set_target_properties(MetalSpirvShaderSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # Same treatment as AppleSceneBlitSmokeTest: the TU reaches into MetalRHI.h
    # (Objective-C headers), so it is compiled as ObjC++ without the C++ PCH.
    if(APPLE)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/MetalSpirvShaderSmokeTest.cpp"
            PROPERTIES LANGUAGE OBJCXX SKIP_PRECOMPILE_HEADERS ON)
    endif()

    # ZRuntime exposes engine/shader/generated/cpp publicly, which is where the
    # generated SPIR-V headers live.
    target_link_libraries(MetalSpirvShaderSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(MetalSpirvShaderSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(MetalSpirvShaderSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(MetalSpirvShaderSmokeTest ZEnginePCH)
        target_precompile_headers(MetalSpirvShaderSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(APPLE)
        target_sources(MetalSpirvShaderSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME MetalSpirvShaderSmokeTest COMMAND MetalSpirvShaderSmokeTest)

    message(STATUS "Configured Metal SPIR-V shader smoke test: MetalSpirvShaderSmokeTest")
endif()

# =====================================================================
# Directional-light shadow (Apple/Metal) wiring smoke test.
# ---------------------------------------------------------------------
# Text-only contract over RHI.h / MetalRHI.{h,mm} / DirectionalLightPass.cpp /
# RenderSystem.cpp / RenderScene.{h,cpp}. It pins the two "everything is wired
# up, yet nothing happens and nothing complains" defects that surfaced the first
# time directional shadows were switched on for Metal:
#
#   1. the shadow-map draw gate asked the legacy shared switch
#      (IsPointLightShadowEnabled), which Metal answers false -> the pass cleared
#      its 4096^2 attachment every frame and drew nothing at all;
#   2. RenderResource::ResetRingBufferOffset() was only ever called from the DX12
#      and Vulkan branches of SubmitDrawLists(), which Apple's Tick never goes
#      through -> the storage ring buffer's End grew every frame until a pass hit
#      its upper-bound assert (SIGABRT at frame 2641).
#
# Pure text: no engine dependency, so unlike the two tests above it needs neither
# ObjC++ nor the Apple link stubs.
#
# Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_DIRECTIONAL_SHADOW_SMOKE_TEST=ON
# =====================================================================

option(ZENGINE_BUILD_DIRECTIONAL_SHADOW_SMOKE_TEST
       "Build the directional-light shadow wiring smoke-test executable"
       OFF)

if(ZENGINE_BUILD_DIRECTIONAL_SHADOW_SMOKE_TEST)
    add_executable(DirectionalLightShadowSmokeTest DirectionalLightShadowSmokeTest.cpp)

    set_target_properties(DirectionalLightShadowSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # 被测源码的路径在 configure 期就钉死：CTest 的工作目录是 build 树里的某个子
    # 目录，跟源码树没有任何稳定关系，运行期猜相对路径只会时灵时不灵。
    #
    # CSM 那些判据要读 shader（三份 per-frame 镜像 + 采样点），所以这里额外给一份
    # engine 根：shader 树跟 C++ 树不在同一个前缀下。
    set(ZENGINE_DIR_TEST_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")

    target_compile_definitions(DirectionalLightShadowSmokeTest PRIVATE
        ZENGINE_RHI_H="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/Interface/RHI.h"
        ZENGINE_METAL_RHI_H="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/Interface/Metal/MetalRHI.h"
        ZENGINE_METAL_RHI_MM="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/Interface/Metal/MetalRHI.mm"
        ZENGINE_DIRECTIONAL_LIGHT_PASS_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/Passes/DirectionalLightPass.cpp"
        ZENGINE_RENDER_SYSTEM_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/RenderSystem.cpp"
        ZENGINE_RENDER_SCENE_H="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/RenderScene.h"
        ZENGINE_RENDER_SCENE_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/RenderScene.cpp"
        ZENGINE_RENDER_COMMON_H="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/RenderCommon.h"
        ZENGINE_RENDER_HELPER_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/RenderHelper.cpp"
        ZENGINE_RENDER_RESOURCE_H="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/RenderResource.h"
        ZENGINE_RENDER_PERFRAME_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../../../Function/Render/RenderResourcePerFrame.cpp"
        ZENGINE_SHADER_CONSTANTS_H="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/include/constants.h"
        ZENGINE_SHADER_STRUCTURES_H="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/include/structures.h"
        ZENGINE_SHADER_MESH_LIGHTING_H="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/include/mesh_lighting.h"
        ZENGINE_SHADER_MESH_LIGHTING_INL="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/include/mesh_lighting.inl"
        ZENGINE_SHADER_MEGALIGHTS_COMPOSE_INL="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/include/megalights_compose.inl"
        ZENGINE_SHADER_MESH_FRAG="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/glsl/mesh.frag"
        ZENGINE_SHADER_DEFERRED_LIGHTING_FRAG="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/glsl/deferred_lighting.frag"
        ZENGINE_SHADER_MEGALIGHTS_DEFERRED_FRAG="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/glsl/megalights_deferred.frag"
        ZENGINE_SHADER_MEGALIGHTS_SPATIAL_FRAG="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/glsl/megalights_spatial.frag"
        ZENGINE_SHADER_SCENE_LIGHTING_STRUCTS_HLSLI="${ZENGINE_DIR_TEST_ENGINE_ROOT}/shader/hlsl/common/scene_lighting_structs.hlsli"
    )

    add_test(NAME DirectionalLightShadowSmokeTest COMMAND DirectionalLightShadowSmokeTest)

    message(STATUS "Configured directional-light shadow smoke test: DirectionalLightShadowSmokeTest")
endif()
