// =============================================================================
// Nanite HiZ GPU 金字塔真机验证（Vulkan headless，切法一）。
//
// 把 VulkanRHI 在无窗口环境下整个拉起，跑一遍 Nanite 离屏渲染（产出真实 D32 深度
// target），触发 nanite_hiz.comp 生成 max 归约金字塔（写进平铺 storage buffer，布局
// 与 CPU nanite_hiz::HiZPyramid.data 逐字节一致），读回 GPU 金字塔，与 CPU 侧
// nanite_hiz::BuildHiZPyramid（喂同一张 readback 深度图）逐 texel 比对。
//
// 判据：GPU 与 CPU 金字塔逐 texel 一致（±量化容差）。这验证 HiZ GPU 侧最不确定的
// 一环——compute 能否读深度附件并产出与 CPU 完全相同的金字塔——不碰剔除/draw。
//
//   cmake -B build -DZENGINE_BUILD_NANITE_HIZ_AB_SMOKE_TEST=ON
//   cmake --build build --target NaniteHiZABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Nanite/NaniteCluster.h"
#include "Runtime/Function/Render/Nanite/NaniteHiZ.h"
#include "Runtime/Function/Render/Nanite/NaniteRenderer.h"
#include "Runtime/Function/Render/Nanite/NaniteTypes.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    struct TestMesh
    {
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;
    };

    TestMesh MakeGrid(uint32_t cells_per_side, float extent)
    {
        TestMesh mesh;
        const uint32_t vertex_side = cells_per_side + 1;
        const float step = extent / static_cast<float>(cells_per_side);
        const float origin = -extent * 0.5f;

        for (uint32_t z = 0; z < vertex_side; ++z)
        {
            for (uint32_t x = 0; x < vertex_side; ++x)
            {
                mesh.positions.push_back(
                    Vector3(origin + static_cast<float>(x) * step, 0.0f, origin + static_cast<float>(z) * step));
            }
        }

        for (uint32_t z = 0; z < cells_per_side; ++z)
        {
            for (uint32_t x = 0; x < cells_per_side; ++x)
            {
                const uint32_t i0 = z * vertex_side + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + vertex_side;
                const uint32_t i3 = i2 + 1;

                mesh.indices.push_back(i0);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i3);
            }
        }

        return mesh;
    }

    std::shared_ptr<RenderCamera> MakeCamera(const Vector3& eye, const Vector3& target)
    {
        auto camera = std::make_shared<RenderCamera>();
        camera->m_Position = eye;
        camera->LookAt(eye, target, Vector3(0.0f, 1.0f, 0.0f));
        camera->SetAspect(1.0f);
        camera->setFOVx(60.0f);
        camera->m_Znear = 0.1f;
        camera->m_Zfar = 1000.0f;
        return camera;
    }
}  // namespace

namespace
{
    // 读回 D32 深度图（reversed-Z 或 standard-Z 均可——本测试只比 GPU/CPU 一致性，
    // 深度约定由渲染管线保证一致）。DEPTH_STENCIL_ATTACHMENT_OPTIMAL → TRANSFER_SRC
    // barrier + CmdCopyImageToBuffer（aspect = DEPTH）。
    std::vector<float> ReadBackDepth(RHI* rhi, RHIImage* image, uint32_t width, uint32_t height)
    {
        std::vector<float> depth;
        if (rhi == nullptr || image == nullptr || width == 0 || height == 0)
        {
            return depth;
        }

        const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(width) * static_cast<RHIDeviceSize>(height) * 4u;

        RHIBuffer* buffer = nullptr;
        RHIDeviceMemory* memory = nullptr;
        rhi->CreateBuffer(bytes,
                          RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buffer,
                          memory);
        if (buffer == nullptr || memory == nullptr)
        {
            return depth;
        }

        RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
        if (command_buffer != nullptr)
        {
            RHIImageMemoryBarrier to_transfer_src {};
            to_transfer_src.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer_src.srcAccessMask = RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            to_transfer_src.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
            to_transfer_src.oldLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            to_transfer_src.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_transfer_src.srcQueueFamilyIndex = ~0u;
            to_transfer_src.dstQueueFamilyIndex = ~0u;
            to_transfer_src.image = image;
            to_transfer_src.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_DEPTH_BIT;
            to_transfer_src.subresourceRange.baseMipLevel = 0;
            to_transfer_src.subresourceRange.levelCount = 1;
            to_transfer_src.subresourceRange.baseArrayLayer = 0;
            to_transfer_src.subresourceRange.layerCount = 1;
            rhi->CmdPipelineBarrier(command_buffer,
                                    RHI_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                    RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                    0,
                                    0,
                                    nullptr,
                                    0,
                                    nullptr,
                                    1,
                                    &to_transfer_src);

            RHIBufferImageCopy region {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_DEPTH_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset.x = 0;
            region.imageOffset.y = 0;
            region.imageOffset.z = 0;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1;
            rhi->CmdCopyImageToBuffer(command_buffer, image, RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            rhi->EndSingleTimeCommands(command_buffer);
        }

        void* mapped = nullptr;
        if (rhi->MapMemory(memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            const float* source = static_cast<const float*>(mapped);
            depth.assign(source, source + static_cast<size_t>(width) * static_cast<size_t>(height));
            rhi->UnmapMemory(memory);
        }

        rhi->FreeMemory(memory);
        rhi->DestroyBuffer(buffer);
        return depth;
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("=== Nanite HiZ GPU-pyramid smoke test (Vulkan headless) ===\n\n");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    Check(GET_SYSTEM(WindowSystem) != nullptr, "a WindowSystem is registered");
    Check(GET_SYSTEM(WindowSystem)->GetMainWindow() == nullptr,
          "the registered WindowSystem has no main window (headless)");

    VulkanRHI vulkan_rhi;
    RHI* rhi = &vulkan_rhi;

    std::printf("[step] calling VulkanRHI::Initialize (headless)...\n");
    const bool vulkan_ready = vulkan_rhi.Initialize();
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds with no window (headless path)");
    if (!vulkan_ready)
    {
        std::printf("[skip] no usable Vulkan device on this host -- HiZ GPU pyramid test skipped\n");
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return g_Failures == 0 ? 0 : 1;
    }

    // 场景：一张平面网格 + 一个实例。相机俯视，深度 target 会有真实的近/远梯度。
    const TestMesh grid = MakeGrid(12, 12.0f);
    NaniteMeshResource mesh;
    NaniteClusterBuilder builder;
    NaniteConfig build_config;
    Check(builder.BuildClustersFromMesh(grid.positions,
                                        std::vector<Vector3>(),
                                        std::vector<Vector2>(),
                                        grid.indices,
                                        mesh,
                                        build_config),
          "the test mesh built into clusters");

    std::vector<NaniteInstance> instances(1);
    instances[0].instance_id = 1;
    instances[0].mesh_id = 1;
    instances[0].transform = Matrix4x4::IDENTITY;
    instances[0].local_bounds = mesh.global_bounds;

    auto camera = MakeCamera(Vector3(0.0f, 6.0f, 10.0f), Vector3(0.0f, 0.0f, 0.0f));

    constexpr uint32_t kTargetSize = 64;

    NaniteConfig config;
    config.enabled = true;
    config.use_gpu_culling = true;
    config.enable_culling = false;   // 只验 HiZ 金字塔，不掺视锥裁剪
    config.enable_lod = false;
    config.enable_streaming = false;

    NaniteRenderer renderer;
    std::printf("[frame] renderer.Initialize...\n");
    const bool ok = renderer.Initialize(rhi, nullptr, config) && renderer.RegisterMeshResource(1, mesh);
    if (ok)
    {
        for (const NaniteInstance& inst : instances)
        {
            renderer.RegisterInstance(inst);
        }
        renderer.SetRasterTargetSize(kTargetSize, kTargetSize);
        std::printf("[frame] Render...\n");
        renderer.Render(rhi, nullptr, camera, config);
        std::printf("[frame] Render returned\n");
    }
    Check(ok, "the frame rendered (depth target populated + HiZ pyramid built)");

    const NaniteGPUResources& gpu = renderer.GetGPUResources();
    Check(gpu.target_depth != nullptr, "the frame produced an offscreen depth target");
    Check(gpu.hiz_data != nullptr, "the HiZ pyramid storage buffer was allocated");
    Check(gpu.hiz_pipeline_ready, "the HiZ compute pipeline is ready");

    const uint32_t width = gpu.hiz_base_width;
    const uint32_t height = gpu.hiz_base_height;
    const int num_mips = gpu.hiz_num_mips;
    std::printf("[info] hiz pyramid %ux%u, %d mips\n", width, height, num_mips);
    Check(width == kTargetSize && height == kTargetSize, "the HiZ pyramid matches the raster target size");
    Check(num_mips > 1, "the HiZ pyramid has more than one mip level");

    // 读回 GPU 金字塔。
    std::vector<float> gpu_pyramid;
    if (gpu.hiz_data_memory != nullptr)
    {
        const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(gpu.hiz_data_capacity) * sizeof(float);
        void* mapped = nullptr;
        if (rhi->MapMemory(gpu.hiz_data_memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            const float* source = static_cast<const float*>(mapped);
            gpu_pyramid.assign(source, source + static_cast<size_t>(gpu.hiz_data_capacity));
            rhi->UnmapMemory(gpu.hiz_data_memory);
        }
    }
    Check(gpu_pyramid.size() == static_cast<size_t>(gpu.hiz_data_capacity) && !gpu_pyramid.empty(),
          "the GPU HiZ pyramid was read back");

    // 读回同一张深度图，喂 CPU 黄金参照。
    const std::vector<float> depth = ReadBackDepth(rhi, gpu.target_depth, width, height);
    Check(depth.size() == static_cast<size_t>(width) * static_cast<size_t>(height),
          "the depth target was read back");

    uint32_t cpu_mips[16][4];
    int cpu_num_mips = 0;
    const uint32_t total_texels = nanite_hiz::HiZMipLayout(width, height, cpu_mips, cpu_num_mips);
    const nanite_hiz::HiZPyramid cpu_pyramid = nanite_hiz::BuildHiZPyramid(width, height, depth.data());
    Check(cpu_pyramid.Valid() && cpu_num_mips == num_mips &&
              total_texels == static_cast<uint32_t>(gpu_pyramid.size()),
          "CPU and GPU agree on the mip layout (count + total texels)");

    // 逐 texel 比对（每级）。允许小量化容差：GPU 采样 D32 与 CPU 读回同一张图，
    // 理论上逐位相同；容差只为吸收潜在的读回路径舍入。
    constexpr float kDepthTolerance = 1e-5f;
    uint32_t total_mismatch = 0;
    float max_abs_delta = 0.0f;
    for (int level = 0; level < num_mips; ++level)
    {
        const uint32_t w = cpu_mips[level][0];
        const uint32_t h = cpu_mips[level][1];
        const uint32_t off = cpu_mips[level][2];
        uint32_t level_mismatch = 0;
        for (uint32_t y = 0; y < h; ++y)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t idx = off + y * w + x;
                const float gpu_v = idx < gpu_pyramid.size() ? gpu_pyramid[idx] : 0.0f;
                const float cpu_v = cpu_pyramid.Texel(level, x, y);
                const float delta = std::fabs(gpu_v - cpu_v);
                max_abs_delta = std::max(max_abs_delta, delta);
                if (delta > kDepthTolerance)
                {
                    if (total_mismatch < 8)
                    {
                        std::printf("       [mip %d (%u,%u)] gpu=%.6f cpu=%.6f delta=%.6f\n",
                                    level, x, y, gpu_v, cpu_v, delta);
                    }
                    ++level_mismatch;
                }
            }
        }
        if (level_mismatch > 0)
        {
            std::printf("       [mip %d] %u/%u texels mismatch\n", level, level_mismatch, w * h);
        }
        total_mismatch += level_mismatch;
    }

    Check(total_mismatch == 0,
          "GPU HiZ pyramid matches the CPU golden reference texel-for-texel");
    std::printf("       [info] total_mismatch=%u max_abs_delta=%.6g (tolerance=%.6g)\n",
                total_mismatch, max_abs_delta, kDepthTolerance);

    renderer.clear();
    vulkan_rhi.Shutdown();

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
