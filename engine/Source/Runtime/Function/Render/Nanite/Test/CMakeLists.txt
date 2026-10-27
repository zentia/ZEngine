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

# =====================================================================
# Nanite 页池簿记层冒烟测试（step 2.5 后续：真正的按页驻留）。
#
# 验证 header-only 纯核心 NanitePagePool.h（nanite_pages::PagePool）：预算闸
# （近页优先、装不下的远页被拒）、距离优先 + LRU 破平、结构页永不驱逐、
# 增量换入/换出队列、LRU 驱逐、Touch 防驱逐、RemovePage/SetBudget/零预算边界。
# 纯 CPU，只链 include root，不链 ZRuntime。
#
#   cmake -B build -DZENGINE_BUILD_NANITE_PAGEPOOL_SMOKE_TEST=ON
#   cmake --build build --target NanitePagePoolSmokeTest
# =====================================================================
option(ZENGINE_BUILD_NANITE_PAGEPOOL_SMOKE_TEST
       "Build the Nanite page-pool bookkeeping smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_PAGEPOOL_SMOKE_TEST)
    add_executable(NanitePagePoolSmokeTest NanitePagePoolSmokeTest.cpp)

    set_target_properties(NanitePagePoolSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # Header-only include chain: only the engine/Source root is needed.
    target_include_directories(NanitePagePoolSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source
    )

    if(MSVC)
        target_compile_options(NanitePagePoolSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME NanitePagePoolSmokeTest COMMAND NanitePagePoolSmokeTest)

    message(STATUS "Configured Nanite page-pool smoke test: NanitePagePoolSmokeTest")
endif()

# =============================================================================
# NanitePageSlotTableSmokeTest — 页槽位分配器（NanitePageSlotTable.h）的无头验证
# -----------------------------------------------------------------------------
# 页池 admitted/evicted 增量队列与真实 RHI 上传/回收之间唯一 CPU 可验的桥：
# 稳定句柄、空闲表回收不泄漏不重叠、churn、best-fit 碎片、空间不足整体失败。
# 纯 CPU，只链 include root，不链 ZRuntime。
#
#   cmake -B build -DZENGINE_BUILD_NANITE_PAGETABLE_SMOKE_TEST=ON
#   cmake --build build --target NanitePageSlotTableSmokeTest
# =====================================================================
option(ZENGINE_BUILD_NANITE_PAGETABLE_SMOKE_TEST
       "Build the Nanite page-slot-table smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_PAGETABLE_SMOKE_TEST)
    add_executable(NanitePageSlotTableSmokeTest NanitePageSlotTableSmokeTest.cpp)

    set_target_properties(NanitePageSlotTableSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # Header-only include chain: only the engine/Source root is needed.
    target_include_directories(NanitePageSlotTableSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source
    )

    if(MSVC)
        target_compile_options(NanitePageSlotTableSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME NanitePageSlotTableSmokeTest COMMAND NanitePageSlotTableSmokeTest)

    message(STATUS "Configured Nanite page-slot-table smoke test: NanitePageSlotTableSmokeTest")
endif()

# =============================================================================
# NanitePageUploadPlannerSmokeTest — 页池增量上传计划器（NanitePageUploadPlanner.h）
# 的无头验证。
# -----------------------------------------------------------------------------
# 整条 pipeline：PagePool.Update → PlanStreamingUploads（evicted 先释放腾位 →
# admitted 认领槽位并产出拷贝命令）→ 字节估算。纯 CPU，只链 include root，不链 ZRuntime。
#
#   cmake -B build -DZENGINE_BUILD_NANITE_UPLOADPLANNER_SMOKE_TEST=ON
#   cmake --build build --target NanitePageUploadPlannerSmokeTest
# =====================================================================
option(ZENGINE_BUILD_NANITE_UPLOADPLANNER_SMOKE_TEST
       "Build the Nanite page-upload-planner smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_UPLOADPLANNER_SMOKE_TEST)
    add_executable(NanitePageUploadPlannerSmokeTest NanitePageUploadPlannerSmokeTest.cpp)

    set_target_properties(NanitePageUploadPlannerSmokeTest PROPERTIES
        CXX_STANDARD 20
        FOLDER "Engine/Tests"
    )

    # Header-only include chain: only the engine/Source root is needed.
    target_include_directories(NanitePageUploadPlannerSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source
    )

    if(MSVC)
        target_compile_options(NanitePageUploadPlannerSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME NanitePageUploadPlannerSmokeTest COMMAND NanitePageUploadPlannerSmokeTest)

    message(STATUS "Configured Nanite page-upload-planner smoke test: NanitePageUploadPlannerSmokeTest")
endif()

# =============================================================================
# NanitePagePoolABSmokeTest — 页池 A/B 真机对比（Vulkan headless）。
# -----------------------------------------------------------------------------
# 整条页池链唯一一处真的跑在 GPU 上的验证：把 VulkanRHI 在无窗口环境下整个拉起
# （依赖本批改的 headless 路径），同一场景跑两遍 —— budget=0（稠密基线）vs
# budget=巨大（页池全驻留，几何拷进持久页缓冲 + 集群 geometry 改写成槽位基址）——
# 判据是两张离屏 GBufferA 逐像素一致（±1 LSB 容差）。附带钉住页池真的在跑：pool-on
# 必须建出持久页缓冲、pool-off 必须留空。
#
# 与 NaniteGPUCullingSmokeTest 不同：它链具体后端（VulkanRHI.h），且需要注册一个
# 无主窗口的 WindowSystem（VulkanRHI::Initialize 无条件 deref 它判 headless）。只在
# Windows/Linux（有 Vulkan 后端）下配置；Apple 走 Metal，另有 GPU-culling 测试覆盖。
#
#   cmake -B build -DZENGINE_BUILD_NANITE_PAGEPOOL_AB_SMOKE_TEST=ON
#   cmake --build build --target NanitePagePoolABSmokeTest
# =============================================================================
option(ZENGINE_BUILD_NANITE_PAGEPOOL_AB_SMOKE_TEST
       "Build the Nanite page-pool A/B (Vulkan headless) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_NANITE_PAGEPOOL_AB_SMOKE_TEST)
    if(NOT WIN32 AND NOT (UNIX AND NOT APPLE))
        message(STATUS "Nanite page-pool A/B smoke test needs the Vulkan backend (Windows/Linux); skipping on this platform")
    else()
        add_executable(NanitePagePoolABSmokeTest NanitePagePoolABSmokeTest.cpp)

        set_target_properties(NanitePagePoolABSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        target_link_libraries(NanitePagePoolABSmokeTest PRIVATE ZRuntime)

        if(TARGET ZEnginePCH)
            target_link_libraries(NanitePagePoolABSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(NanitePagePoolABSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(NanitePagePoolABSmokeTest ZEnginePCH)
            target_precompile_headers(NanitePagePoolABSmokeTest REUSE_FROM ZEnginePCH)
        endif()

        if(MSVC)
            target_compile_options(NanitePagePoolABSmokeTest PRIVATE /W4)
        endif()

        add_test(NAME NanitePagePoolABSmokeTest COMMAND NanitePagePoolABSmokeTest)

        message(STATUS "Configured Nanite page-pool A/B smoke test: NanitePagePoolABSmokeTest")
    endif()
endif()
