# =====================================================================
# Nanite cluster-builder smoke test.
#
# Own main(); carved out of the ZRuntime glob in Runtime/CMakeLists.txt so its
# main() cannot be dragged into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST=ON
#   cmake --build build --target NaniteClusterBuilderSmokeTest
#
# Pure CPU work -- no RHI, no window, no project. Unlike the Lumen tests this
# one is NOT gated on the Vulkan/Emscripten conditions: Function/Render/Nanite
# is never stripped from ZRuntime, so the test links everywhere ZRuntime builds.
# =====================================================================

option(ZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST
       "Build the Nanite cluster-builder smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST)
    add_executable(NaniteClusterBuilderSmokeTest NaniteClusterBuilderSmokeTest.cpp)

    set_target_properties(NaniteClusterBuilderSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(NaniteClusterBuilderSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(NaniteClusterBuilderSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(NaniteClusterBuilderSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(NaniteClusterBuilderSmokeTest ZEnginePCH)
        target_precompile_headers(NaniteClusterBuilderSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(NaniteClusterBuilderSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(NaniteClusterBuilderSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME NaniteClusterBuilderSmokeTest COMMAND NaniteClusterBuilderSmokeTest)

    message(STATUS "Configured Nanite cluster-builder smoke test: NaniteClusterBuilderSmokeTest")
endif()

# =====================================================================
# Nanite culling / LOD smoke test. Same pattern. Pins that frustum culling
# actually culls (it was `return true;`), that the extracted planes match this
# repo's row-major matrices, that SelectLOD moves the level the right way, and
# that BuildRenderBatches emits non-empty batches.
# =====================================================================
option(ZENGINE_BUILD_NANITE_CULLING_SMOKE_TEST
       "Build the Nanite culling / LOD smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_CULLING_SMOKE_TEST)
    add_executable(NaniteCullingSmokeTest NaniteCullingSmokeTest.cpp)

    set_target_properties(NaniteCullingSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(NaniteCullingSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(NaniteCullingSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(NaniteCullingSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(NaniteCullingSmokeTest ZEnginePCH)
        target_precompile_headers(NaniteCullingSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(NaniteCullingSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(NaniteCullingSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME NaniteCullingSmokeTest COMMAND NaniteCullingSmokeTest)

    message(STATUS "Configured Nanite culling smoke test: NaniteCullingSmokeTest")
endif()

# =====================================================================
# Nanite scene-wiring smoke test. Same pattern. Pins that Nanite can see a
# real scene: RenderScene -> RenderResourceBase CPU mesh cache -> clusters
# -> instances -> culling -> batches, plus the two dispatchers above it
# (NaniteRenderPass::Render and RenderPipelineBase::UpdateNanite) and the
# r.Nanite.* CVars. Before this, NaniteRenderer::RegisterInstance had zero
# callers and NaniteRenderPass was never constructed anywhere.
# =====================================================================
option(ZENGINE_BUILD_NANITE_SCENE_WIRING_SMOKE_TEST
       "Build the Nanite scene-wiring smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_SCENE_WIRING_SMOKE_TEST)
    add_executable(NaniteSceneWiringSmokeTest NaniteSceneWiringSmokeTest.cpp)

    set_target_properties(NaniteSceneWiringSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(NaniteSceneWiringSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(NaniteSceneWiringSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(NaniteSceneWiringSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(NaniteSceneWiringSmokeTest ZEnginePCH)
        target_precompile_headers(NaniteSceneWiringSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(NaniteSceneWiringSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(NaniteSceneWiringSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME NaniteSceneWiringSmokeTest COMMAND NaniteSceneWiringSmokeTest)

    message(STATUS "Configured Nanite scene-wiring smoke test: NaniteSceneWiringSmokeTest")
endif()

# =====================================================================
# Nanite GPU-culling smoke test. Same pattern. This is the one that proves
# Nanite actually runs on the GPU: it creates a real MetalRHI headlessly
# (Initialize only needs an MTLDevice -- no window, no CAMetalLayer), uploads
# the packed scene, builds the compute pipeline, dispatches the cull kernel and
# reads the counter + indirect draw commands back.
#
# On Apple it also links Metal/QuartzCore through ZRuntime. Where
# <nanite_cluster_cull_comp.h> was never generated (no glslangValidator) the
# GPU section degrades to a printed skip instead of a false failure.
# =====================================================================
option(ZENGINE_BUILD_NANITE_GPU_CULLING_SMOKE_TEST
       "Build the Nanite GPU-culling smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_GPU_CULLING_SMOKE_TEST)
    add_executable(NaniteGPUCullingSmokeTest NaniteGPUCullingSmokeTest.cpp)

    set_target_properties(NaniteGPUCullingSmokeTest PROPERTIES
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
            "${CMAKE_CURRENT_SOURCE_DIR}/NaniteGPUCullingSmokeTest.cpp"
            PROPERTIES LANGUAGE OBJCXX SKIP_PRECOMPILE_HEADERS ON)
    endif()

    target_link_libraries(NaniteGPUCullingSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(NaniteGPUCullingSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(NaniteGPUCullingSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(NaniteGPUCullingSmokeTest ZEnginePCH)
        target_precompile_headers(NaniteGPUCullingSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    # The GLSL mirror checks read engine/shader/** as text (the shaders are never
    # compiled here). CMake hands the test the absolute engine root so it does not
    # depend on the working directory.
    if(DEFINED ENGINE_ROOT_DIR)
        target_compile_definitions(NaniteGPUCullingSmokeTest PRIVATE
            "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")
    endif()

    if(MSVC)
        target_compile_options(NaniteGPUCullingSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(NaniteGPUCullingSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME NaniteGPUCullingSmokeTest COMMAND NaniteGPUCullingSmokeTest)

    message(STATUS "Configured Nanite GPU-culling smoke test: NaniteGPUCullingSmokeTest")
endif()

# =====================================================================
# Nanite HiZ (层次 Z 缓冲) 纯 CPU 核心冒烟测试。
# ---------------------------------------------------------------------
# 验证 NaniteHiZ.h 的纯函数（BuildHiZPyramid / MipLevelForRect /
# SampleMaxDepth / TestOccluded）：金字塔构建（max 归约）+ 区域采样 + 遮挡判据。
# 与 EnhancedInput / GasCooldown 同一手法：被测逻辑是 header-only 纯函数，不依赖
# RHI/窗口/Nanite，故本 target 只链 include root，不链 ZRuntime。
#
#   cmake -B build -DZENGINE_BUILD_NANITE_HIZ_SMOKE_TEST=ON
#   cmake --build build --target NaniteHiZSmokeTest
# =====================================================================
option(ZENGINE_BUILD_NANITE_HIZ_SMOKE_TEST
       "Build the Nanite HiZ (hierarchical Z) pure-CPU smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_HIZ_SMOKE_TEST)
    add_executable(NaniteHiZSmokeTest NaniteHiZSmokeTest.cpp)

    set_target_properties(NaniteHiZSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # Header-only include chain: only the engine/Source root is needed.
    target_include_directories(NaniteHiZSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source
    )

    if(MSVC)
        target_compile_options(NaniteHiZSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME NaniteHiZSmokeTest COMMAND NaniteHiZSmokeTest)

    message(STATUS "Configured Nanite HiZ smoke test: NaniteHiZSmokeTest")
endif()
