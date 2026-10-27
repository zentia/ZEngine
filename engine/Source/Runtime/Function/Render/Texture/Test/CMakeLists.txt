# =============================================================================
# VirtualTextureGpuUploadABSmokeTest — VT 页上传 + 页表纹理的真机 A/B（Vulkan headless）
# -----------------------------------------------------------------------------
# 2.6 落地时 VT 的 GPU 侧两处 TODO 是空的（UploadPageToPhysicalPool /
# UpdatePageTableTexture），簿记全对但显存里一个字节都没有。本测试把 VulkanRHI 在
# 无窗口环境下拉起，注册一张 256x256 源纹理（4x4 页），同步 ForceLoadPages 全部 16 页，
# 然后把物理页池整图拷回 host 逐页与源区域逐字节比对，并读回页表纹理逐 texel 比对。
#
# 判据：
#   ① 每页落在自己槽位对应的像素块上（offset 算错当场红）；
#   ② 逐页增量上传不冲掉先前驻留的页（discard_prior_content guard 的回归闸）；
#   ③ 页表纹理被建出来且 texel == CPU PageTableEntry。
#
# 与 NanitePagePoolABSmokeTest 同一套 harness 手法（链 VulkanRHI.h + 注册无主窗口
# WindowSystem），只在 Windows/Linux（有 Vulkan 后端）下配置。
#
#   cmake -B build -DZENGINE_BUILD_VT_GPU_UPLOAD_AB_SMOKE_TEST=ON
#   cmake --build build --target VirtualTextureGpuUploadABSmokeTest
# =============================================================================
option(ZENGINE_BUILD_VT_GPU_UPLOAD_AB_SMOKE_TEST
       "Build the VirtualTexture GPU page-upload A/B (Vulkan headless) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_VT_GPU_UPLOAD_AB_SMOKE_TEST)
    if(NOT WIN32 AND NOT (UNIX AND NOT APPLE))
        message(STATUS "VT GPU upload A/B smoke test needs the Vulkan backend (Windows/Linux); skipping on this platform")
    else()
        add_executable(VirtualTextureGpuUploadABSmokeTest VirtualTextureGpuUploadABSmokeTest.cpp)

        set_target_properties(VirtualTextureGpuUploadABSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        target_link_libraries(VirtualTextureGpuUploadABSmokeTest PRIVATE ZRuntime)

        if(TARGET ZEnginePCH)
            target_link_libraries(VirtualTextureGpuUploadABSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(VirtualTextureGpuUploadABSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(VirtualTextureGpuUploadABSmokeTest ZEnginePCH)
            target_precompile_headers(VirtualTextureGpuUploadABSmokeTest REUSE_FROM ZEnginePCH)
        endif()

        if(MSVC)
            target_compile_options(VirtualTextureGpuUploadABSmokeTest PRIVATE /W4)
        endif()

        add_test(NAME VirtualTextureGpuUploadABSmokeTest COMMAND VirtualTextureGpuUploadABSmokeTest)

        message(STATUS "Configured VT GPU upload A/B smoke test: VirtualTextureGpuUploadABSmokeTest")
    endif()
endif()

# =============================================================================
# VirtualTexturePageTableLifecycleABSmokeTest — 页表纹理生命周期 A/B（Vulkan headless）
# -----------------------------------------------------------------------------
# 上面那个测试只验"加载"方向。本测试验页表纹理的另外两半：失效同步（unload /
# 跨 VT 的池满 LRU 驱逐后 GPU 页表随之失效，不留指向别人像素的陈旧项）与释放
# （Unregister / Shutdown 后 VMA 分配数回到基线）。判据是模拟 shader 间接寻址：
# 读回 GPU 页表 → 槽位 → 读回 GPU 页池，必须与直接取源纹理逐字节一致。
# 会自死锁的调用包在看门狗里，回归表现为 FAIL 而不是挂死。
#
#   cmake -B build -DZENGINE_BUILD_VT_PAGETABLE_LIFECYCLE_AB_SMOKE_TEST=ON
#   cmake --build build --target VirtualTexturePageTableLifecycleABSmokeTest
# =============================================================================
option(ZENGINE_BUILD_VT_PAGETABLE_LIFECYCLE_AB_SMOKE_TEST
       "Build the VirtualTexture page-table lifecycle A/B (Vulkan headless) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_VT_PAGETABLE_LIFECYCLE_AB_SMOKE_TEST)
    if(NOT WIN32 AND NOT (UNIX AND NOT APPLE))
        message(STATUS "VT page-table lifecycle A/B smoke test needs the Vulkan backend (Windows/Linux); skipping")
    else()
        add_executable(VirtualTexturePageTableLifecycleABSmokeTest VirtualTexturePageTableLifecycleABSmokeTest.cpp)
        set_target_properties(VirtualTexturePageTableLifecycleABSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )
        target_link_libraries(VirtualTexturePageTableLifecycleABSmokeTest PRIVATE ZRuntime)
        if(TARGET ZEnginePCH)
            target_link_libraries(VirtualTexturePageTableLifecycleABSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(VirtualTexturePageTableLifecycleABSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(VirtualTexturePageTableLifecycleABSmokeTest ZEnginePCH)
            target_precompile_headers(VirtualTexturePageTableLifecycleABSmokeTest REUSE_FROM ZEnginePCH)
        endif()
        if(MSVC)
            target_compile_options(VirtualTexturePageTableLifecycleABSmokeTest PRIVATE /W4)
        endif()
        add_test(NAME VirtualTexturePageTableLifecycleABSmokeTest COMMAND VirtualTexturePageTableLifecycleABSmokeTest)
        message(STATUS "Configured VT page-table lifecycle A/B smoke test: VirtualTexturePageTableLifecycleABSmokeTest")
    endif()
endif()

# =============================================================================
# VirtualTextureResolveABSmokeTest — 页表间接寻址采样 A/B（Vulkan headless）
# -----------------------------------------------------------------------------
# 前两个 VT 测试验"数据对不对"，但它们的 A 路是测试里的 C++ 在**模拟** shader。本测试
# 让真的 GLSL（vt_resolve.comp → vt_common.h）读页表做间接寻址，读回逐 texel 与源纹理比：
# 驻留页 == 源；被驱逐页是 fallback 而不是占了它旧槽位的别的页的像素；整数入口 == UV 入口；
# 非整页大小（右/下边缘残缺页）也对。依赖 ZShaderCompile 生成 vt_resolve_comp.h。
#
#   cmake -B build -DZENGINE_BUILD_VT_RESOLVE_AB_SMOKE_TEST=ON
#   cmake --build build --target VirtualTextureResolveABSmokeTest
# =============================================================================
option(ZENGINE_BUILD_VT_RESOLVE_AB_SMOKE_TEST
       "Build the VirtualTexture page-table indirect-sampling A/B (Vulkan headless) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_VT_RESOLVE_AB_SMOKE_TEST)
    if(NOT WIN32 AND NOT (UNIX AND NOT APPLE))
        message(STATUS "VT resolve A/B smoke test needs the Vulkan backend (Windows/Linux); skipping")
    else()
        add_executable(VirtualTextureResolveABSmokeTest VirtualTextureResolveABSmokeTest.cpp)
        set_target_properties(VirtualTextureResolveABSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )
        target_link_libraries(VirtualTextureResolveABSmokeTest PRIVATE ZRuntime)
        if(TARGET ZEnginePCH)
            target_link_libraries(VirtualTextureResolveABSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(VirtualTextureResolveABSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(VirtualTextureResolveABSmokeTest ZEnginePCH)
            target_precompile_headers(VirtualTextureResolveABSmokeTest REUSE_FROM ZEnginePCH)
        endif()
        if(MSVC)
            target_compile_options(VirtualTextureResolveABSmokeTest PRIVATE /W4)
        endif()
        add_test(NAME VirtualTextureResolveABSmokeTest COMMAND VirtualTextureResolveABSmokeTest)
        message(STATUS "Configured VT resolve A/B smoke test: VirtualTextureResolveABSmokeTest")
    endif()
endif()

# =============================================================================
# VirtualTextureAsyncLoadABSmokeTest — 异步加载线程安全 A/B（Vulkan headless）
# -----------------------------------------------------------------------------
# enable_async_loading 默认 true，但后台加载线程此前从未真正跑起来（RequestPages 不唤醒它），
# 一旦跑起来又会在 worker 线程里直接 LoadPage（分配槽位 + 提交 GPU 命令）。本测试在真实多线程下
# 钉死"worker 只做 CPU、GPU 活只在 owner 线程"：4 个线程并发 RequestPages、owner 同时 Pump，
# 36 页纹理压 16 槽位的池，真 shader 读回逐 texel 比对；并验证背压有界、带队列关停不死锁。
#
#   cmake -B build -DZENGINE_BUILD_VT_ASYNC_LOAD_AB_SMOKE_TEST=ON
#   cmake --build build --target VirtualTextureAsyncLoadABSmokeTest
# =============================================================================
option(ZENGINE_BUILD_VT_ASYNC_LOAD_AB_SMOKE_TEST
       "Build the VirtualTexture async-load thread-safety A/B (Vulkan headless) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_VT_ASYNC_LOAD_AB_SMOKE_TEST)
    if(NOT WIN32 AND NOT (UNIX AND NOT APPLE))
        message(STATUS "VT async-load A/B smoke test needs the Vulkan backend (Windows/Linux); skipping")
    else()
        add_executable(VirtualTextureAsyncLoadABSmokeTest VirtualTextureAsyncLoadABSmokeTest.cpp)
        set_target_properties(VirtualTextureAsyncLoadABSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )
        target_link_libraries(VirtualTextureAsyncLoadABSmokeTest PRIVATE ZRuntime)
        if(TARGET ZEnginePCH)
            target_link_libraries(VirtualTextureAsyncLoadABSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(VirtualTextureAsyncLoadABSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(VirtualTextureAsyncLoadABSmokeTest ZEnginePCH)
            target_precompile_headers(VirtualTextureAsyncLoadABSmokeTest REUSE_FROM ZEnginePCH)
        endif()
        if(MSVC)
            target_compile_options(VirtualTextureAsyncLoadABSmokeTest PRIVATE /W4)
        endif()
        add_test(NAME VirtualTextureAsyncLoadABSmokeTest COMMAND VirtualTextureAsyncLoadABSmokeTest)
        message(STATUS "Configured VT async-load A/B smoke test: VirtualTextureAsyncLoadABSmokeTest")
    endif()
endif()
