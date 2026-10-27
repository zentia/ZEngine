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
