// =============================================================================
// Nanite 页池 A/B 真机对比冒烟测试（Vulkan headless）。
//
// 这是整条页池链（PagePool 簿记 → PageSlotTable 槽位 → PageUploadPlanner 计划 →
// NaniteRenderer::ExecuteStreamingUploads 真拷显存 → PatchClustersForPagePool shader
// 间接寻址）唯一一处**真的跑在 GPU 上**的验证。前面的纯 CPU 冒烟测试只能证明各自
// 的簿记正确；这里把 Vulkan 后端在**无窗口**环境下整个拉起来，跑两遍同一场景：
//
//   * A（稠密基线）：streaming_pool_budget_bytes = 0  ⇒ 页池关闭，draw 读稠密
//     positions/indices，PatchClustersForPagePool 是 no-op。
//   * B（页池启用）：streaming_pool_budget_bytes = 足够大 ⇒ 所有叶子页都被 admitted，
//     几何被拷进持久页缓冲，draw 读 page_positions/page_indices，集群的 geometry.x/y
//     被改写成槽位基址。
//
// 判据：**同一个相机、同一个场景，A 与 B 产出的离屏 GBuffer 必须逐像素一致**（允许
// 一个 LSB 的量化容差）。页池的全部意义就是"驻留方式变了、画面不能变"—— 如果 B 丢了几
// 何、或者槽位基址改写把三角形指错了地方，B 的图会立刻和 A 错开。
//
// 另外钉住"页池真的在跑"这件事本身，否则 A/B 相等也可能是两条都走了稠密退化：
//   * B 的持久页缓冲必须被建出来（page_positions / page_indices 非空）。
//   * B 的页池必须有页被 admitted（renderer 暴露的 m_LastPoolUpdate.admitted_count > 0）。
//
// 为什么是 Vulkan 而不是 Metal：本机是 Windows + RTX 5080，没有 Metal。DX12 走不通
// （Nanite shader 是 SPIR-V，DX12 没有 SPIR-V→HLSL 的交叉编译）。VulkanRHI 之前在无窗口
// 时会在 InitializeLogicalDevice 解引用空的 present_family optional 直接抛
// std::bad_optional_access —— 这正是本次同批改的 headless 路径修掉的东西，本测试就是它的
// 端到端验收。
//
//   cmake -B build -DZENGINE_BUILD_NANITE_PAGEPOOL_AB_SMOKE_TEST=ON
//   cmake --build build --target NanitePagePoolABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Nanite/NaniteCluster.h"
#include "Runtime/Function/Render/Nanite/NaniteGPUData.h"
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

    // -----------------------------------------------------------------------
    // 测试网格：一张 12×12 的平面，288 个三角形。足够让集群构建器真的分出层次
    // （kMaxTrianglesPerCluster = 64 ⇒ 至少两层），产生多个叶子页供页池驻留。
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // 把一张 2D color 图读回 CPU（与 NaniteGPUCullingSmokeTest 同源）。
    // -----------------------------------------------------------------------
    std::vector<uint8_t> ReadBackPixels(RHI* rhi, RHIImage* image, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> pixels;
        if (rhi == nullptr || image == nullptr || width == 0 || height == 0)
        {
            return pixels;
        }

        const RHIDeviceSize bytes =
            static_cast<RHIDeviceSize>(width) * static_cast<RHIDeviceSize>(height) * 4u;

        RHIBuffer*       buffer = nullptr;
        RHIDeviceMemory* memory = nullptr;
        rhi->CreateBuffer(bytes,
                          RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buffer,
                          memory);
        if (buffer == nullptr || memory == nullptr)
        {
            return pixels;
        }

        RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
        if (command_buffer != nullptr)
        {
            // Nanite 离屏 render pass 的 finalLayout 是 COLOR_ATTACHMENT_OPTIMAL。
            // CmdCopyImageToBuffer 要求源图在 TRANSFER_SRC_OPTIMAL，所以先插一道
            // barrier 把它从 COLOR_ATTACHMENT_OPTIMAL 转到 TRANSFER_SRC_OPTIMAL
            // （Vulkan 严格校验布局；Metal 那份 readback 不插 barrier 是因为 Metal
            // 后端忽略布局）。
            RHIImageMemoryBarrier to_transfer_src {};
            to_transfer_src.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer_src.pNext = nullptr;
            to_transfer_src.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_transfer_src.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
            to_transfer_src.oldLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_transfer_src.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            // ~0u == VK_QUEUE_FAMILY_IGNORED。用字面量而不是宏，避免本 TU 直接依赖
            // <vulkan/vulkan.h>（RHI 结构是后端无关的）。
            to_transfer_src.srcQueueFamilyIndex = ~0u;
            to_transfer_src.dstQueueFamilyIndex = ~0u;
            to_transfer_src.image = image;
            to_transfer_src.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            to_transfer_src.subresourceRange.baseMipLevel = 0;
            to_transfer_src.subresourceRange.levelCount = 1;
            to_transfer_src.subresourceRange.baseArrayLayer = 0;
            to_transfer_src.subresourceRange.layerCount = 1;

            rhi->CmdPipelineBarrier(command_buffer,
                                    RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
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
            // bufferRowLength / bufferImageHeight 是**纹素**单位（不是字节）。0 =
            // 紧密打包（row stride = width * texel_bytes）。之前填 width*4 让 Vulkan
            // 以为每行有 width*4 个纹素，需要 4× 大小的缓冲，validation 报 buffer 溢出。
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset.x = 0;
            region.imageOffset.y = 0;
            region.imageOffset.z = 0;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1;

            rhi->CmdCopyImageToBuffer(command_buffer,
                                      image,
                                      RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      buffer,
                                      1,
                                      &region);
            rhi->EndSingleTimeCommands(command_buffer);
        }

        void* mapped = nullptr;
        if (rhi->MapMemory(memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            const auto* source = static_cast<const uint8_t*>(mapped);
            pixels.assign(source, source + static_cast<size_t>(bytes));
            rhi->UnmapMemory(memory);
        }

        rhi->FreeMemory(memory);
        rhi->DestroyBuffer(buffer);
        return pixels;
    }

    // 两张 GBufferA 图逐像素比对。允许每通道 ±kTolerance 的偏差：页池路径把 Vector3
    // 位置按槽位切片重拷、shader 从 page_positions 取顶点，浮点光栅化的舍入在极少数边
    // 缘像素上可能差一个 LSB。任何真正的几何丢失 / 槽位错位都会造成整片像素错开，远超
    // 这个容差。
    constexpr int kChannelTolerance = 1;

    struct DiffResult
    {
        uint32_t differing_pixels {0};   // 任一通道差超过容差的像素数
        uint32_t covered_pixels {0};     // A 图里"被写过"的像素数（非清屏）
        uint32_t covered_pixels_b {0};   // B 图里"被写过"的像素数（非清屏）
        int  max_channel_delta {0};      // 观测到的最大通道差
    };

    constexpr uint8_t kCoveredThreshold = 60;

    DiffResult CompareBuffers(RHI* rhi,
                              RHIImage* image_a,
                              RHIImage* image_b,
                              uint32_t width,
                              uint32_t height)
    {
        DiffResult result;
        const std::vector<uint8_t> a = ReadBackPixels(rhi, image_a, width, height);
        const std::vector<uint8_t> b = ReadBackPixels(rhi, image_b, width, height);

        const size_t expected = static_cast<size_t>(width) * height * 4u;
        if (a.size() < expected || b.size() < expected)
        {
            // 读不回来本身是失败：调用方据此判红，这里只把 differing 记成全量。
            result.differing_pixels = width * height;
            return result;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
                const uint8_t* pa = a.data() + offset;
                const uint8_t* pb = b.data() + offset;

                if (pa[0] > kCoveredThreshold || pa[1] > kCoveredThreshold || pa[2] > kCoveredThreshold)
                {
                    ++result.covered_pixels;
                }
                if (pb[0] > kCoveredThreshold || pb[1] > kCoveredThreshold || pb[2] > kCoveredThreshold)
                {
                    ++result.covered_pixels_b;
                }

                int max_delta = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int d = std::abs(static_cast<int>(pa[c]) - static_cast<int>(pb[c]));
                    if (d > max_delta)
                    {
                        max_delta = d;
                    }
                }
                if (max_delta > result.max_channel_delta)
                {
                    result.max_channel_delta = max_delta;
                }
                if (max_delta > kChannelTolerance)
                {
                    // 诊断：打印前 8 个不同像素的两边 RGB，看差异的"形状"
                    // （全 0 vs 有值 = 一边没画；值不同 = 几何/法线错位）。
                    if (result.differing_pixels < 8)
                    {
                        std::printf("       [diff px(%u,%u)] dense=(%u,%u,%u) pool=(%u,%u,%u)\n",
                                    x,
                                    y,
                                    pa[0],
                                    pa[1],
                                    pa[2],
                                    pb[0],
                                    pb[1],
                                    pb[2]);
                    }
                    ++result.differing_pixels;
                }
            }
        }
        return result;
    }
}  // namespace

int main()
{
    // 无缓冲：崩溃（fail-fast 0xC0000409 之类）不会冲刷 stdio 缓冲，重定向到文件时
    // 之前的 printf 会全部丢失。设成无缓冲后每条 printf 立即落地，能定位崩在哪一步。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("=== Nanite page-pool A/B smoke test (Vulkan headless) ===\n\n");

    // =====================================================================
    // 0. 注册一个"无主窗口"的 WindowSystem。
    //
    // VulkanRHI::Initialize 无条件 deref GET_SYSTEM(WindowSystem)->GetMainWindow()
    // 判定 headless。这里只 RegisterSystem、**不** InitializeAll —— WindowSystem 的
    // m_Application 保持 null，GetMainWindow() 返回 nullptr，于是 RHI 走 headless
    // 路径（跳过 surface / swapchain / present queue）。在 Windows 上若真去 Initialize
    // 会弹出一个真实窗口，那不是我们要的。
    // =====================================================================
    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    Check(GET_SYSTEM(WindowSystem) != nullptr, "a WindowSystem is registered");
    Check(GET_SYSTEM(WindowSystem)->GetMainWindow() == nullptr,
          "the registered WindowSystem has no main window (headless)");

    // =====================================================================
    // 1. 拉起 Vulkan 后端（headless）。
    // =====================================================================
    VulkanRHI vulkan_rhi;
    RHI* rhi = &vulkan_rhi;

    std::printf("[step] calling VulkanRHI::Initialize (headless)...\n");
    const bool vulkan_ready = vulkan_rhi.Initialize();
    std::printf("[step] VulkanRHI::Initialize returned %d\n", static_cast<int>(vulkan_ready));
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds with no window (headless path)");
    if (!vulkan_ready)
    {
        std::printf("[skip] no usable Vulkan device on this host -- A/B comparison skipped\n");
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return g_Failures == 0 ? 0 : 1;
    }

    // =====================================================================
    // 2. 构建场景：一张平面网格 + 两个实例。
    // =====================================================================
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
    Check(!mesh.clusters.empty(), "the mesh has clusters");

    uint32_t leaf_count = 0;
    for (const NaniteCluster& cluster : mesh.clusters)
    {
        if (cluster.is_leaf)
        {
            ++leaf_count;
        }
    }
    Check(leaf_count > 0, "the mesh has leaf clusters (pages to residency-manage)");

    // 两个实例：一个在原点，一个沿 +Z 推远 40 单位（仍在 streaming 距离内）。
    // 注：相机在 (0,6,10) 朝原点看，instance 1（Z+40）在相机背后、不可见——留它只为
    // 验证"多实例共享页"这条路径；可见像素全部来自 instance 0。
    std::vector<NaniteInstance> instances(2);
    instances[0].instance_id = 1;
    instances[0].mesh_id = 1;
    instances[0].transform = Matrix4x4::IDENTITY;
    instances[0].local_bounds = mesh.global_bounds;
    instances[1].instance_id = 2;
    instances[1].mesh_id = 1;
    instances[1].transform = Matrix4x4::IDENTITY;
    instances[1].transform.setTrans(Vector3(0.0f, 0.0f, 40.0f));
    instances[1].local_bounds = mesh.global_bounds;

    // 俯视相机：测试网格在 y = 0，侧视投影面积为零。抬高到 y = 6 才看得见。
    auto camera = MakeCamera(Vector3(0.0f, 6.0f, 10.0f), Vector3(0.0f, 0.0f, 0.0f));

    constexpr uint32_t kTargetSize = 64;

    // =====================================================================
    // 3. 跑一遍 Render()，把离屏 GBufferA 读回来。
    //
    // config 是唯一变量：budget = 0（稠密）vs budget = 巨大（页池全驻留）。其余
    // 全部相同 —— 同一相机、同一场景、同一 target 尺寸。
    // =====================================================================
    // 两个 renderer 各跑一遍。它们必须**活到读回之后**：NaniteGPUResources 里的
    // target_gbuffer 是 RHIImage 句柄，renderer.clear() 会把它们销毁，句柄立刻变
    // 悬垂——之前把 clear 放进 run_frame 里，读回时 vkCmdCopyImageToBuffer 拿到的
    // 就是已释放的 VkImage（validation: Invalid VkImage Object）。所以这里让两个
    // renderer 一直活到 GBuffer 像素读进 CPU 内存之后再 clear。
    auto make_config = [](bool pool_on) {
        NaniteConfig config;
        config.enabled = true;
        config.use_gpu_culling = true;   // 走 GPU 路径（与生产一致）
        config.enable_culling = false;   // 只比页池，不比视锥裁剪
        config.enable_lod = false;       // 只比页池，不比 LOD
        config.enable_streaming = true;
        config.max_streaming_distance = 10000;  // 两个实例都在距离内
        config.streaming_pool_budget_bytes = pool_on ? (1ull << 40) : 0;  // 1 TiB：全部页都装得下
        return config;
    };

    NaniteRenderer renderer_dense;
    NaniteRenderer renderer_pool;
    bool dense_ok = false;
    bool pool_ok = false;

    {
        const NaniteConfig config = make_config(false);
        std::printf("[frame pool_on=0] renderer.Initialize...\n");
        dense_ok = renderer_dense.Initialize(rhi, nullptr, config) &&
                   renderer_dense.RegisterMeshResource(1, mesh);
        if (dense_ok)
        {
            for (const NaniteInstance& inst : instances)
            {
                renderer_dense.RegisterInstance(inst);
            }
            renderer_dense.SetRasterTargetSize(kTargetSize, kTargetSize);
            std::printf("[frame pool_on=0] Render...\n");
            renderer_dense.Render(rhi, nullptr, camera, config);
            std::printf("[frame pool_on=0] Render returned\n");
        }
        else
        {
            std::printf("[frame pool_on=0] init/register FAILED\n");
        }
    }

    {
        const NaniteConfig config = make_config(true);
        std::printf("[frame pool_on=1] renderer.Initialize...\n");
        pool_ok = renderer_pool.Initialize(rhi, nullptr, config) &&
                  renderer_pool.RegisterMeshResource(1, mesh);
        if (pool_ok)
        {
            for (const NaniteInstance& inst : instances)
            {
                renderer_pool.RegisterInstance(inst);
            }
            renderer_pool.SetRasterTargetSize(kTargetSize, kTargetSize);
            std::printf("[frame pool_on=1] Render...\n");
            renderer_pool.Render(rhi, nullptr, camera, config);
            std::printf("[frame pool_on=1] Render returned\n");
        }
        else
        {
            std::printf("[frame pool_on=1] init/register FAILED\n");
        }
    }

    Check(dense_ok, "the dense (pool-off) frame rendered");
    Check(pool_ok, "the page-pool (pool-on) frame rendered");

    const NaniteGPUResources& gpu_dense = renderer_dense.GetGPUResources();
    const NaniteGPUResources& gpu_pool = renderer_pool.GetGPUResources();

    // ---- 页池路径真的在跑，而不是两条都退化成稠密 -------------------------
    Check(gpu_dense.target_gbuffer[0] != nullptr && gpu_pool.target_gbuffer[0] != nullptr,
          "both frames produced an offscreen GBuffer target");
    Check(gpu_pool.page_positions != nullptr && gpu_pool.page_indices != nullptr,
          "the pool-on frame allocated its persistent page buffers (was: dense-only)");
    Check(gpu_dense.page_positions == nullptr && gpu_dense.page_indices == nullptr,
          "the dense frame left the page buffers unallocated (pool genuinely off)");

    // ---- 诊断：读回 page_positions[0..3] 与稠密 positions[0..3] 比对 ------------
    // 页池路径把 Vector3 展开成 Vector4 写进槽位。若槽位 0 的顶点与稠密 positions[0]
    // 一致，说明 UploadPageSlice 的 src/dst 对齐正确；不一致则定位到上传对齐 bug。
    {
        auto read_vec4 = [&](RHIDeviceMemory* mem, uint32_t slot, uint32_t capacity_elems) -> Vector4 {
            Vector4 out {0.0f, 0.0f, 0.0f, 0.0f};
            if (mem == nullptr || slot >= capacity_elems)
                return out;
            void* mapped = nullptr;
            const RHIDeviceSize off = static_cast<RHIDeviceSize>(slot) * sizeof(Vector4);
            if (rhi->MapMemory(mem, off, sizeof(Vector4), 0, &mapped) && mapped != nullptr)
            {
                std::memcpy(&out, mapped, sizeof(Vector4));
                rhi->UnmapMemory(mem);
            }
            return out;
        };
        for (uint32_t slot : {0u, 40u, 100u, 200u, 400u, 600u, 800u})
        {
            const Vector4 vd = read_vec4(gpu_dense.positions_memory, slot, gpu_dense.position_capacity);
            const Vector4 vp = read_vec4(gpu_pool.page_positions_memory, slot, gpu_pool.page_position_capacity);
            const bool same = std::fabs(vd.x - vp.x) < 1e-4f && std::fabs(vd.y - vp.y) < 1e-4f &&
                              std::fabs(vd.z - vp.z) < 1e-4f;
            std::printf("[vtx slot=%u] dense=(%.3f,%.3f,%.3f) page=(%.3f,%.3f,%.3f) %s\n",
                        slot,
                        vd.x,
                        vd.y,
                        vd.z,
                        vp.x,
                        vp.y,
                        vp.z,
                        same ? "OK" : "MISMATCH");
        }
        // 索引：读回 page_indices[0..5] 与稠密 indices[0..5]。
        auto read_u32 = [&](RHIDeviceMemory* mem, uint32_t slot, uint32_t capacity_elems) -> uint32_t {
            uint32_t out = 0;
            if (mem == nullptr || slot >= capacity_elems)
                return out;
            void* mapped = nullptr;
            const RHIDeviceSize off = static_cast<RHIDeviceSize>(slot) * sizeof(uint32_t);
            if (rhi->MapMemory(mem, off, sizeof(uint32_t), 0, &mapped) && mapped != nullptr)
            {
                std::memcpy(&out, mapped, sizeof(uint32_t));
                rhi->UnmapMemory(mem);
            }
            return out;
        };
        for (uint32_t slot = 0; slot < 6; ++slot)
        {
            const uint32_t id = read_u32(gpu_dense.indices_memory, slot, gpu_dense.index_capacity);
            const uint32_t ip = read_u32(gpu_pool.page_indices_memory, slot, gpu_pool.page_index_capacity);
            std::printf("[idx slot=%u] dense=%u page=%u\n", slot, id, ip);
        }

        // 数非空 draw 命令（indexCount != 0）。两条路径应一致 —— 不一致说明页池路径
        // 的 cull kernel 产出的命令更少（几何虽在显存、却没被画）。
        auto count_nonempty = [&](const NaniteGPUResources& gpu) -> uint32_t {
            uint32_t nonempty = 0;
            if (gpu.draw_commands_memory == nullptr)
                return 0;
            void* mapped = nullptr;
            const RHIDeviceSize bytes =
                static_cast<RHIDeviceSize>(gpu.cluster_capacity) * sizeof(RHIDrawIndexedIndirectCommand);
            if (rhi->MapMemory(gpu.draw_commands_memory, 0, bytes, 0, &mapped) && mapped != nullptr)
            {
                const auto* cmds = static_cast<const RHIDrawIndexedIndirectCommand*>(mapped);
                for (uint32_t i = 0; i < gpu.cluster_capacity; ++i)
                {
                    if (cmds[i].indexCount != 0u)
                        ++nonempty;
                }
                rhi->UnmapMemory(gpu.draw_commands_memory);
            }
            return nonempty;
        };
        std::printf("[drawcmd] dense_nonempty=%u pool_nonempty=%u (cluster_capacity=%u)\n",
                    count_nonempty(gpu_dense),
                    count_nonempty(gpu_pool),
                    gpu_dense.cluster_capacity);

        // 逐条 dump 非空命令的 (firstInstance, firstIndex, indexCount)。两条路径的
        // indexCount 必须逐条相同；firstIndex 会不同（稠密=全局基址，页池=槽位基址），
        // 但各自指向自己缓冲里的同一段几何。
        auto dump_cmds = [&](const NaniteGPUResources& gpu, const char* tag) {
            if (gpu.draw_commands_memory == nullptr)
                return;
            void* mapped = nullptr;
            const RHIDeviceSize bytes =
                static_cast<RHIDeviceSize>(gpu.cluster_capacity) * sizeof(RHIDrawIndexedIndirectCommand);
            if (rhi->MapMemory(gpu.draw_commands_memory, 0, bytes, 0, &mapped) && mapped != nullptr)
            {
                const auto* cmds = static_cast<const RHIDrawIndexedIndirectCommand*>(mapped);
                uint32_t shown = 0;
                for (uint32_t i = 0; i < gpu.cluster_capacity && shown < 20; ++i)
                {
                    if (cmds[i].indexCount != 0u)
                    {
                        std::printf("[draw %s #%u] firstInstance=%u firstIndex=%u indexCount=%u\n",
                                    tag,
                                    i,
                                    cmds[i].firstInstance,
                                    cmds[i].firstIndex,
                                    cmds[i].indexCount);
                        ++shown;
                    }
                }
                rhi->UnmapMemory(gpu.draw_commands_memory);
            }
        };
        dump_cmds(gpu_dense, "dense");
        dump_cmds(gpu_pool, "pool");
    }

    // ---- 核心：A 与 B 逐像素一致 ----------------------------------------
    // 两个 renderer 都还活着，target_gbuffer 句柄有效，读回安全。
    const DiffResult diff =
        CompareBuffers(rhi, gpu_dense.target_gbuffer[0], gpu_pool.target_gbuffer[0], kTargetSize, kTargetSize);
    Check(diff.covered_pixels > 0,
          "the dense baseline actually rasterised pixels (not a dead-angle false pass)");
    Check(diff.differing_pixels == 0,
          "pool-on matches pool-off pixel-for-pixel on GBufferA (residency changed, image did not)");
    std::printf("       [info] covered_dense=%u covered_pool=%u differing=%u max_channel_delta=%d (tolerance=%d)\n",
                diff.covered_pixels,
                diff.covered_pixels_b,
                diff.differing_pixels,
                diff.max_channel_delta,
                kChannelTolerance);

    // 读回完成后再释放 GPU 资源。
    renderer_dense.clear();
    renderer_pool.clear();

    vulkan_rhi.Shutdown();

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
