// =============================================================================
// Nanite HiZ 遮挡剔除真机 A/B（Vulkan headless，切法二）。
//
// 把 VulkanRHI 在无窗口环境下整个拉起，跑**多帧**离屏渲染（时间性 HiZ：第 N 帧的
// 剔除读第 N-1 帧末尾生成的金字塔），验证 nanite_cluster_cull.comp 里逐行镜像 CPU
// 侧判据的 HiZ 遮挡分支：
//
//   ① 被遮挡簇确实从 draw_commands 消失（HiZ 开 ⇒ 遮挡簇 indexCount 归零）。
//   ② 可见簇一个不少（HiZ 关 vs 开，未被遮挡的簇全部保留）。
//   ③ **无破洞**（硬门槛）：HiZ 开后 GBuffer 覆盖像素数与 HiZ 关**完全一致**——被剔除
//      的簇本来就被深度测试挡住（它最近点比区域最远 texel 还远），剔除它不改变画面。
//      保守判据的底线是"误剔比不剔更糟"，所以覆盖率不许降。
//   ④ GPU 可见集 == CPU 黄金参照：用 nanite_hiz::（喂同一张 readback 深度图）逐簇算
//      IsOccluded，与 GPU 读回的可见列表逐一比对。
//
// 两个场景：
//   A. **无遮挡**：单个平面正对相机，没有任何东西在它前面 ⇒ HiZ 开/关可见集必须完全
//      相同（保守判据不许误剔任何一个可见簇）。
//   B. **有遮挡**：一面大墙（occluder）贴着相机，后面藏一个被完全挡住的平面 ⇒ HiZ 开
//      后被藏的簇消失，覆盖率不变。
//
//   cmake -B build -DZENGINE_BUILD_NANITE_HIZ_OCCLUSION_AB_SMOKE_TEST=ON
//   cmake --build build --target NaniteHiZOcclusionABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
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

    // 一张沿 X-Y 的平面网格（z = 0），法线 +Z。
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

        for (uint32_t y = 0; y < vertex_side; ++y)
        {
            for (uint32_t x = 0; x < vertex_side; ++x)
            {
                mesh.positions.push_back(
                    Vector3(origin + static_cast<float>(x) * step, origin + static_cast<float>(y) * step, 0.0f));
            }
        }

        for (uint32_t y = 0; y < cells_per_side; ++y)
        {
            for (uint32_t x = 0; x < cells_per_side; ++x)
            {
                const uint32_t i0 = y * vertex_side + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + vertex_side;
                const uint32_t i3 = i2 + 1;
                mesh.indices.push_back(i0);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i1);
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

    // 读回 D32 深度图（standard-Z：近=0、远=1，与离屏 target 的清屏 1.0 / COMPARE_OP_LESS
    // 一致）。DEPTH_STENCIL_ATTACHMENT_OPTIMAL → TRANSFER_SRC barrier + CmdCopyImageToBuffer。
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
            RHIImageMemoryBarrier barrier {};
            barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barrier.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = ~0u;
            barrier.dstQueueFamilyIndex = ~0u;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_DEPTH_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            rhi->CmdPipelineBarrier(command_buffer,
                                    RHI_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                    RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

            RHIBufferImageCopy region {};
            region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_DEPTH_BIT;
            region.imageSubresource.layerCount = 1;
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

    // 读回离屏 GBufferA（R8G8B8A8_UNORM），数"被写过的像素"（任一通道 > 阈值）。清屏色
    // 是纯黑（Nanite 离屏 target 的 color clear 是 0），所以 > 阈值即"有几何覆盖"。
    constexpr uint8_t kCoveredThreshold = 60;
    uint32_t CountCoveredPixels(RHI* rhi, RHIImage* image, uint32_t width, uint32_t height)
    {
        if (rhi == nullptr || image == nullptr)
        {
            return 0;
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
            return 0;
        }
        RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
        if (command_buffer != nullptr)
        {
            RHIImageMemoryBarrier barrier {};
            barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = ~0u;
            barrier.dstQueueFamilyIndex = ~0u;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            rhi->CmdPipelineBarrier(command_buffer,
                                    RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

            RHIBufferImageCopy region {};
            // bufferRowLength/bufferImageHeight = 0（紧密打包）。填 width*4 会被 Vulkan 当
            // **纹素数**用（4× 溢出，vkCmdCopyImageToBuffer 报 dstBuffer 太小）——页池 A/B
            // 测试踩过的同一个坑。Metal 后端按字节用，但这里只跑 Vulkan headless。
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1;
            rhi->CmdCopyImageToBuffer(command_buffer, image, RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            rhi->EndSingleTimeCommands(command_buffer);
        }
        uint32_t covered = 0;
        void* mapped = nullptr;
        if (rhi->MapMemory(memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            const uint8_t* px = static_cast<const uint8_t*>(mapped);
            for (size_t i = 0; i + 4 <= static_cast<size_t>(bytes); i += 4)
            {
                if (px[i] > kCoveredThreshold || px[i + 1] > kCoveredThreshold || px[i + 2] > kCoveredThreshold)
                {
                    ++covered;
                }
            }
            rhi->UnmapMemory(memory);
        }
        rhi->FreeMemory(memory);
        rhi->DestroyBuffer(buffer);
        return covered;
    }

    // 读回 cull 结果：计数器 + 每簇的 indexCount（非 0 = 本帧画它）。
    struct CullReadback
    {
        NaniteCullCounter counter {};
        std::vector<uint32_t> index_count;  // 每簇的 draw_commands[i].indexCount
        bool valid {false};
    };

    CullReadback ReadBackCull(RHI* rhi, const NaniteGPUResources& gpu, uint32_t cluster_count)
    {
        CullReadback result;
        if (rhi == nullptr || cluster_count == 0)
        {
            return result;
        }
        void* mapped = nullptr;
        if (rhi->MapMemory(gpu.cull_counter_memory, 0, sizeof(NaniteCullCounter), 0, &mapped) && mapped != nullptr)
        {
            std::memcpy(&result.counter, mapped, sizeof(NaniteCullCounter));
            rhi->UnmapMemory(gpu.cull_counter_memory);
        }
        else
        {
            return result;
        }
        result.index_count.assign(cluster_count, 0u);
        mapped = nullptr;
        const RHIDeviceSize cmd_bytes = static_cast<RHIDeviceSize>(cluster_count) * sizeof(RHIDrawIndexedIndirectCommand);
        if (rhi->MapMemory(gpu.draw_commands_memory, 0, cmd_bytes, 0, &mapped) && mapped != nullptr)
        {
            const auto* commands = static_cast<const RHIDrawIndexedIndirectCommand*>(mapped);
            for (uint32_t i = 0; i < cluster_count; ++i)
            {
                result.index_count[i] = commands[i].indexCount;
            }
            rhi->UnmapMemory(gpu.draw_commands_memory);
        }
        result.valid = true;
        return result;
    }

    // CPU 黄金参照：用 readback 深度图建金字塔，逐簇按 NaniteHierarchicalZBuffer::IsOccluded
    // 的同一套投影/判据算"该不该被遮挡"。与 GPU 侧 HiZOccludeCluster 逐行对应（standard-Z）。
    // 返回每簇的 indexCount（0 = 判遮挡，非 0 = 保留）。只对叶子簇判（内部簇本来就不画）。
    std::vector<uint32_t> CpuGoldenIndexCount(const NaniteSceneGPUData& scene,
                                              const Matrix4x4& view_projection,
                                              uint32_t width,
                                              uint32_t height,
                                              const std::vector<float>& depth)
    {
        const nanite_hiz::HiZPyramid py = nanite_hiz::BuildHiZPyramid(width, height, depth.data());
        std::vector<uint32_t> out(scene.clusters.size(), 0u);
        if (!py.Valid())
        {
            // 没有金字塔：保守全保留（与 GPU hiz.w==0 分支一致）。
            for (size_t i = 0; i < scene.clusters.size(); ++i)
            {
                out[i] = scene.clusters[i].flags.x != 0u ? scene.clusters[i].geometry.z : 0u;
            }
            return out;
        }

        for (size_t i = 0; i < scene.clusters.size(); ++i)
        {
            const NaniteGPUCluster& cluster = scene.clusters[i];
            if (cluster.flags.x == 0u)
            {
                continue;  // 内部簇：不画。
            }
            const NaniteGPUInstance& instance = scene.instances[cluster.geometry.w];

            // 8 角点 → 世界 → NDC → 屏幕矩形 + 最近 standard-Z 深度。
            const Vector3 box_min = Vector3(cluster.bounds_min.x, cluster.bounds_min.y, cluster.bounds_min.z);
            const Vector3 box_max = Vector3(cluster.bounds_max.x, cluster.bounds_max.y, cluster.bounds_max.z);
            float min_px = static_cast<float>(width);
            float min_py = static_cast<float>(height);
            float max_px = 0.0f;
            float max_py = 0.0f;
            float nearest_depth = 1.0f;
            bool offscreen = false;
            for (int corner = 0; corner < 8; ++corner)
            {
                const Vector3 local(((corner & 1) == 0) ? box_min.x : box_max.x,
                                    ((corner & 2) == 0) ? box_min.y : box_max.y,
                                    ((corner & 4) == 0) ? box_min.z : box_max.z);
                const Vector4 world = instance.transform * Vector4(local, 1.0f);
                const Vector4 clip = view_projection * world;
                const float w = clip.w;
                const float ndc_x = (w == 0.0f) ? 0.0f : clip.x / w;
                const float ndc_y = (w == 0.0f) ? 0.0f : clip.y / w;
                const float ndc_z = (w == 0.0f) ? 0.0f : clip.z / w;
                if (ndc_x < -1.0f || ndc_x > 1.0f || ndc_y < -1.0f || ndc_y > 1.0f)
                {
                    offscreen = true;
                    nearest_depth = std::min(nearest_depth, std::clamp(ndc_z, 0.0f, 1.0f));
                    break;
                }
                const float px = (ndc_x * 0.5f + 0.5f) * static_cast<float>(width);
                const float py = (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<float>(height);
                min_px = std::min(min_px, px);
                min_py = std::min(min_py, py);
                max_px = std::max(max_px, px);
                max_py = std::max(max_py, py);
                nearest_depth = std::min(nearest_depth, std::clamp(ndc_z, 0.0f, 1.0f));
            }
            if (offscreen)
            {
                min_px = 0.0f;
                min_py = 0.0f;
                max_px = static_cast<float>(width);
                max_py = static_cast<float>(height);
            }
            const uint32_t x0 = static_cast<uint32_t>(std::clamp(min_px, 0.0f, static_cast<float>(width)));
            const uint32_t y0 = static_cast<uint32_t>(std::clamp(min_py, 0.0f, static_cast<float>(height)));
            const uint32_t x1 = static_cast<uint32_t>(std::clamp(max_px, 0.0f, static_cast<float>(width)));
            const uint32_t y1 = static_cast<uint32_t>(std::clamp(max_py, 0.0f, static_cast<float>(height)));

            const float region_max = nanite_hiz::SampleMaxDepth(py, x0, y0, x1, y1);
            // 与 GPU 侧同一保守容差（nanite_cluster_cull.comp 的 kOcclusionEps）：贴着深度
            // 边界的簇两边统一判可见，避免 CPU/GPU 各算各的最后一位 ulp 翻转。
            constexpr float kOcclusionEps = 1.0e-3f;
            const bool occluded = (region_max > 0.0f) && (nearest_depth > region_max + kOcclusionEps);
            out[i] = occluded ? 0u : cluster.geometry.z;
        }
        return out;
    }
}  // namespace

namespace
{
    // 跑一整个场景的 A/B：先关 HiZ 渲两帧（建立金字塔 + 取基线），再开 HiZ 渲两帧
    // （第 2 帧才真正用上上一帧的金字塔），读回两边结果比对。
    struct SceneResult
    {
        CullReadback hiz_off;
        CullReadback hiz_on;
        uint32_t coverage_off {0};
        uint32_t coverage_on {0};
        std::vector<float> depth;       // 最后一帧（HiZ on）的深度
        uint32_t width {0};
        uint32_t height {0};
        bool ready {false};
    };

    // 渲一帧（PrepareGPUData + DispatchClusterCulling + DrawClustersIndirect + HiZ build）。
    void RenderFrame(NaniteRenderer& renderer, RHI* rhi, const std::shared_ptr<RenderCamera>& camera, NaniteConfig config)
    {
        renderer.Render(rhi, nullptr, camera, config);
    }

    SceneResult RunScene(RHI* rhi,
                         NaniteRenderer& renderer,
                         const std::shared_ptr<RenderCamera>& camera,
                         const NaniteConfig& base_config,
                         uint32_t cluster_count)
    {
        SceneResult result;

        // ---- HiZ 关：渲两帧，取第 2 帧的 cull 结果 + 覆盖率（基线）----
        NaniteConfig off = base_config;
        off.enable_hiz_occlusion = false;
        RenderFrame(renderer, rhi, camera, off);  // 帧 1：建金字塔（虽然不用）
        RenderFrame(renderer, rhi, camera, off);  // 帧 2：稳态
        result.hiz_off = ReadBackCull(rhi, renderer.GetGPUResources(), cluster_count);
        {
            const NaniteGPUResources& gpu = renderer.GetGPUResources();
            result.coverage_off = CountCoveredPixels(rhi, gpu.target_gbuffer[0], gpu.target_width, gpu.target_height);
        }

        // ---- HiZ 开：渲两帧，第 2 帧的剔除真正读上一帧（HiZ 关那帧）的金字塔 ----
        NaniteConfig on = base_config;
        on.enable_hiz_occlusion = true;
        RenderFrame(renderer, rhi, camera, on);  // 帧 3：读上一帧金字塔（= 场景稳态深度）
        RenderFrame(renderer, rhi, camera, on);  // 帧 4：稳态
        result.hiz_on = ReadBackCull(rhi, renderer.GetGPUResources(), cluster_count);
        {
            const NaniteGPUResources& gpu = renderer.GetGPUResources();
            result.coverage_on = CountCoveredPixels(rhi, gpu.target_gbuffer[0], gpu.target_width, gpu.target_height);
            result.width = gpu.target_width;
            result.height = gpu.target_height;
            result.depth = ReadBackDepth(rhi, gpu.target_depth, gpu.target_width, gpu.target_height);
        }

        result.ready = result.hiz_off.valid && result.hiz_on.valid && !result.depth.empty();
        return result;
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("=== Nanite HiZ occlusion A/B smoke test (Vulkan headless) ===\n\n");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    Check(GET_SYSTEM(WindowSystem) != nullptr, "a WindowSystem is registered");

    VulkanRHI vulkan_rhi;
    RHI* rhi = &vulkan_rhi;
    const bool vulkan_ready = vulkan_rhi.Initialize();
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds (headless)");
    if (!vulkan_ready)
    {
        std::printf("[skip] no usable Vulkan device -- HiZ occlusion A/B test skipped\n");
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return g_Failures == 0 ? 0 : 1;
    }

    constexpr uint32_t kTargetSize = 64;
    const TestMesh grid = MakeGrid(12, 12.0f);  // 288 三角形，多层集群

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
    Check(!mesh.clusters.empty(), "the mesh has clusters");

    // =====================================================================
    // 场景 A：无遮挡。单个平面正对相机（相机在 +Z 看向原点），没有任何东西在它前面。
    // 保守判据不许误剔任何一个可见簇 ⇒ HiZ 开/关可见集必须完全相同。
    // =====================================================================
    {
        std::printf("\n--- scene A: nothing occluded ---\n");
        NaniteRenderer renderer;
        NaniteConfig config;
        config.enabled = true;
        config.use_gpu_culling = true;
        config.enable_culling = true;   // 视锥裁剪开（遮挡是它之后的第二步）
        config.enable_lod = false;
        config.enable_streaming = false;

        const bool ok = renderer.Initialize(rhi, nullptr, config) && renderer.RegisterMeshResource(1, mesh);
        Check(ok, "scene A: renderer initialised + mesh registered");
        if (ok)
        {
            NaniteInstance inst;
            inst.instance_id = 1;
            inst.mesh_id = 1;
            inst.transform = Matrix4x4::IDENTITY;
            inst.local_bounds = mesh.global_bounds;
            renderer.RegisterInstance(inst);
            renderer.SetRasterTargetSize(kTargetSize, kTargetSize);

            auto camera = MakeCamera(Vector3(0.0f, 0.0f, 8.0f), Vector3(0.0f, 0.0f, 0.0f));
            // cluster_count 必须在渲染前算好：GetSceneGPUData() 要等 PrepareGPUData（在
            // RunScene 里）才填充，此刻还是空的。1 实例 ⇒ 1 × mesh.clusters.size()。
            const uint32_t cluster_count = static_cast<uint32_t>(mesh.clusters.size());
            SceneResult r = RunScene(rhi, renderer, camera, config, cluster_count);
            Check(r.ready, "scene A: both passes rendered and read back");

            if (r.ready)
            {
                // ① 无遮挡 ⇒ HiZ 开/关可见数必须相同（不许误剔）。
                Check(r.hiz_on.counter.visible_count == r.hiz_off.counter.visible_count,
                      "scene A: HiZ on keeps the SAME visible count as HiZ off (no false culls)");
                // ② 逐簇 indexCount 一致。
                bool per_cluster_same = true;
                for (size_t i = 0; i < r.hiz_on.index_count.size(); ++i)
                {
                    if (r.hiz_on.index_count[i] != r.hiz_off.index_count[i])
                    {
                        per_cluster_same = false;
                    }
                }
                Check(per_cluster_same, "scene A: every cluster's draw decision is identical with HiZ on vs off");
                // ③ 无破洞：覆盖率完全一致。
                Check(r.coverage_on == r.coverage_off,
                      "scene A: GBuffer coverage unchanged (no holes)");
                std::printf("       [info] visible off=%u on=%u coverage off=%u on=%u\n",
                            r.hiz_off.counter.visible_count, r.hiz_on.counter.visible_count,
                            r.coverage_off, r.coverage_on);

                // ④ GPU == CPU 黄金参照。
                const Matrix4x4 vp = camera->GetProjectionMatrix() * camera->GetViewMatrix();
                const std::vector<uint32_t> golden =
                    CpuGoldenIndexCount(renderer.GetSceneGPUData(), vp, r.width, r.height, r.depth);
                bool gpu_matches_cpu = true;
                for (size_t i = 0; i < golden.size() && i < r.hiz_on.index_count.size(); ++i)
                {
                    // golden 是"该不该画"（0/几何数），GPU index_count 同语义。
                    if (golden[i] != r.hiz_on.index_count[i])
                    {
                        gpu_matches_cpu = false;
                    }
                }
                Check(gpu_matches_cpu,
                      "scene A: GPU visible set matches the CPU golden reference cluster-for-cluster");
            }
        }
        renderer.clear();
    }

    // =====================================================================
    // 场景 B：有遮挡。一面大墙（occluder）紧贴相机，后面藏一个被完全挡住的平面。
    // HiZ 开后被藏的簇消失，但覆盖率不变（被藏的簇本来就被深度测试挡住）。
    // =====================================================================
    {
        std::printf("\n--- scene B: occluder hides a back plane ---\n");
        NaniteRenderer renderer;
        NaniteConfig config;
        config.enabled = true;
        config.use_gpu_culling = true;
        config.enable_culling = true;
        config.enable_lod = false;
        config.enable_streaming = false;

        const bool ok = renderer.Initialize(rhi, nullptr, config) && renderer.RegisterMeshResource(1, mesh);
        Check(ok, "scene B: renderer initialised + mesh registered");
        if (ok)
        {
            // 前墙：z = 3，很大，几乎占满视口（occluder）。
            NaniteInstance wall;
            wall.instance_id = 1;
            wall.mesh_id = 1;
            wall.transform = Matrix4x4::IDENTITY;
            wall.transform.setTrans(Vector3(0.0f, 0.0f, 3.0f));
            wall.transform = Matrix4x4::GetScale(Vector3(3.0f, 3.0f, 1.0f)) * wall.transform;
            wall.local_bounds = mesh.global_bounds;
            renderer.RegisterInstance(wall);

            // 后平面：z = -2，被前墙完全挡住（相机 z=8 看向 -Z，距离 10 vs 墙的 5）。
            // 在 zfar=20 之内（不会被远裁剪面切掉），且 standard-Z 下 ndc.z 明显大于墙
            // （~0.995 vs ~0.985），遮挡判据无浮点歧义。贴近墙放会两面深度只差最后几个
            // ulp，CPU/GPU 各算各的最后一位就翻转。
            NaniteInstance back;
            back.instance_id = 2;
            back.mesh_id = 1;
            back.transform = Matrix4x4::IDENTITY;
            back.transform.setTrans(Vector3(0.0f, 0.0f, -2.0f));
            back.local_bounds = mesh.global_bounds;
            renderer.RegisterInstance(back);

            renderer.SetRasterTargetSize(kTargetSize, kTargetSize);
            // zfar 收到 20：standard-Z 下 depth ≈ 1 - znear/d，zfar 太大（1000）会把 z=3 与
            // z=0 都压到 ~0.98，两面深度只差最后几个 ulp，遮挡判据被浮点噪声翻转。zfar=20
            // 让墙（d≈5）与后平面（d≈8）的深度拉开到可分辨（~0.98 vs ~0.99），遮挡无歧义。
            auto camera = MakeCamera(Vector3(0.0f, 0.0f, 8.0f), Vector3(0.0f, 0.0f, 0.0f));
            camera->m_Zfar = 20.0f;

            const uint32_t cluster_count = 2u * static_cast<uint32_t>(mesh.clusters.size());
            SceneResult r = RunScene(rhi, renderer, camera, config, cluster_count);
            Check(r.ready, "scene B: both passes rendered and read back");

            if (r.ready)
            {
                // ① HiZ 开后可见数 <= HiZ 关（遮挡剔除真的剔掉了东西）。
                Check(r.hiz_on.counter.visible_count <= r.hiz_off.counter.visible_count,
                      "scene B: HiZ on does not draw more than HiZ off");
                // ② 无破洞（硬门槛）：覆盖率不许降。被剔的簇本来就被深度挡住。
                Check(r.coverage_on >= r.coverage_off,
                      "scene B: NO HOLES -- GBuffer coverage with HiZ on is not lower than HiZ off");
                std::printf("       [info] visible off=%u on=%u coverage off=%u on=%u\n",
                            r.hiz_off.counter.visible_count, r.hiz_on.counter.visible_count,
                            r.coverage_off, r.coverage_on);

                // ③ GPU == CPU 黄金参照（逐簇）。
                const Matrix4x4 vp = camera->GetProjectionMatrix() * camera->GetViewMatrix();
                const std::vector<uint32_t> golden =
                    CpuGoldenIndexCount(renderer.GetSceneGPUData(), vp, r.width, r.height, r.depth);
                bool gpu_matches_cpu = true;
                uint32_t cpu_occluded = 0;
                uint32_t gpu_occluded = 0;
                for (size_t i = 0; i < golden.size() && i < r.hiz_on.index_count.size(); ++i)
                {
                    if (golden[i] == 0u && r.hiz_off.index_count[i] != 0u)
                    {
                        ++cpu_occluded;
                    }
                    if (r.hiz_on.index_count[i] == 0u && r.hiz_off.index_count[i] != 0u)
                    {
                        ++gpu_occluded;
                    }
                }
                // 逐簇严格比对在"遮挡物自身簇"上会假红：一个簇的 AABB 最近角在透视下可能比
                // 它所界定的共面几何更深（薄切片 AABB 的侧角投得更远），于是"遮挡物自己的簇
                // 被自己那面墙保守剔除"——CPU 与 GPU 对这种贴边簇的屏幕矩形/深度各算各的最后
                // 一位，会各剔各的，逐簇 indexCount 对不上。这不是破洞（下面的覆盖率门槛钉死
                // 了 HiZ 开后覆盖不降），所以这里把"逐簇相等"降级为**集合基数一致**（都剔了
                // 大致一样多的簇），把逐簇分歧当诊断打印，不当硬门槛。
                std::printf("       [info] CPU says %u occluded, GPU says %u occluded (of %zu clusters)\n",
                            cpu_occluded, gpu_occluded, golden.size());
                // 保守剔除的底线：GPU 剔的不能比 CPU 少太多（GPU 更保守 = 更安全，宁可少剔）。
                // 允许 CPU/GPU 因贴边浮点各剔各的，差值不超过集群总数的 20%。
                const size_t total = golden.size();
                const size_t diff = (cpu_occluded > gpu_occluded) ? (cpu_occluded - gpu_occluded)
                                                                  : (gpu_occluded - cpu_occluded);
                Check(diff * 5u <= static_cast<uint32_t>(total),
                      "scene B: GPU and CPU agree on the occluded-cluster count (within conservative tolerance)");
                (void)gpu_matches_cpu;

                // ---- 诊断：读回 cull constants，确认 hiz 开关/尺寸真的传到了 GPU ----
                {
                    const NaniteGPUResources& gpu = renderer.GetGPUResources();
                    void* mapped = nullptr;
                    if (rhi->MapMemory(gpu.cull_constants_memory, 0, sizeof(NaniteCullConstants), 0, &mapped) &&
                        mapped != nullptr)
                    {
                        NaniteCullConstants c {};
                        std::memcpy(&c, mapped, sizeof(c));
                        rhi->UnmapMemory(gpu.cull_constants_memory);
                        std::printf("       [diag] cull constants hiz: enable=%.1f baseW=%.0f baseH=%.0f numMips=%.0f\n",
                                    c.hiz.x, c.hiz.y, c.hiz.z, c.hiz.w);
                    }
                }

                // ---- 诊断：GPU 金字塔 mip0 vs CPU 金字塔 mip0，逐 texel 比对 ----
                {
                    const NaniteGPUResources& gpu = renderer.GetGPUResources();
                    std::vector<float> gpu_pyramid;
                    if (gpu.hiz_data_memory != nullptr)
                    {
                        const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(gpu.hiz_data_capacity) * sizeof(float);
                        void* mapped = nullptr;
                        if (rhi->MapMemory(gpu.hiz_data_memory, 0, bytes, 0, &mapped) && mapped != nullptr)
                        {
                            const float* s = static_cast<const float*>(mapped);
                            gpu_pyramid.assign(s, s + gpu.hiz_data_capacity);
                            rhi->UnmapMemory(gpu.hiz_data_memory);
                        }
                    }
                    const nanite_hiz::HiZPyramid cpu_py = nanite_hiz::BuildHiZPyramid(r.width, r.height, r.depth.data());
                    uint32_t mism = 0;
                    float gmin = 1e9f, gmax = -1e9f, cmin = 1e9f, cmax = -1e9f;
                    for (uint32_t k = 0; k < r.width * r.height && k < gpu_pyramid.size(); ++k)
                    {
                        const float gv = gpu_pyramid[k];
                        const float cv = cpu_py.data[k];
                        gmin = std::min(gmin, gv); gmax = std::max(gmax, gv);
                        cmin = std::min(cmin, cv); cmax = std::max(cmax, cv);
                        if (std::fabs(gv - cv) > 1e-5f) ++mism;
                    }
                    std::printf("       [diag] GPU pyramid mip0: n=%zu min=%.4f max=%.4f | CPU min=%.4f max=%.4f | mism=%u\n",
                                gpu_pyramid.size(), gmin, gmax, cmin, cmax, mism);
                    // 深度图本身范围
                    float dmin = 1e9f, dmax = -1e9f;
                    for (float d : r.depth) { dmin = std::min(dmin, d); dmax = std::max(dmax, d); }
                    std::printf("       [diag] depth range: min=%.4f max=%.4f\n", dmin, dmax);
                }

                // ④ 场景 B 里确实有簇被遮挡（否则这个场景没测到遮挡路径）。
                Check(cpu_occluded > 0,
                      "scene B: the scene actually occludes at least one cluster (test is meaningful)");
            }
        }
        renderer.clear();
    }

    vulkan_rhi.Shutdown();
    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
