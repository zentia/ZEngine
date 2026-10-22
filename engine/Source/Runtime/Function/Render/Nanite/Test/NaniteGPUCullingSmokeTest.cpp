// =============================================================================
// Nanite GPU 剔除冒烟测试。
//
// Nanite 以前一行都不跑在 GPU 上：`NaniteGPUResources` 的八个缓冲全是 void*，
// UpdateGPUBuffers / PerformGPUCulling 是两行 TODO，use_gpu_culling 这个开关除了
// 被打印出来之外没有任何作用。也就是说"集群构建 → 层次 → 逐簇裁剪 → 批次"整条链
// 都在 CPU，而且逐簇裁剪根本没有 GPU 版本。
//
// 这个测试钉的是三件**只有真的跑起来才能证伪**的事：
//
//   1. CPU 与 GLSL 的字节契约。NaniteGPUData.h 有 static_assert 钉住 C++ 侧的
//      sizeof/offsetof，但没人钉过 GLSL 那半边。这里的做法是：把三个 shader 源
//      当文本读进来，检查字段声明顺序和描述符绑定槽，和 NaniteGPUBinding /
//      NaniteDrawCommand 对齐。shader 从来不在这个测试里编译，所以漏改的镜像
//      否则完全看不见。
//
//   2. 打包结果。NaniteBuildSceneGPUData 把"实例 + 集群"摊平成一份大缓冲：顶点
//      全场景一份、索引加基址、集群烘进 instance_index。任何一处算错，GPU 画出来
//      的东西都是错的，但 CPU 侧完全无感。
//
//   3. **真的 dispatch**。Apple 上创建 MetalRHI（Initialize 只要一个
//      MTLDevice，不需要窗口），把数据传上去，建 compute 管线，发一次 dispatch，
//      然后把计数器和间接绘制命令读回来比对：
//        * visible + culled == 集群总数（每个线程必须落到其中一边）
//        * 关掉裁剪 ⇒ 恰好等于叶子集群总数
//        * 打开裁剪 ⇒ 相机背后那个实例的集群一个都不许出现
//        * 相机转开 ⇒ 什么都不剩
//        * 松开 LOD 容差 ⇒ 只留 LOD 0
//
//   4. **真的画出来**。剔除只是前半段 —— 产出的间接命令要被一次 CmdDrawIndexed
//      Indirect 消费掉才算跑在 GPU 上。以前 SetRasterRenderPass 一个调用者都没有，
//      光栅管线永远建不起来，那一句绘制永远走 no-op。现在 Nanite 在没有外部 render
//      pass 时自建离屏 target 并自己开/关 pass，然后把 color target 读回 CPU 数像
//      素：俯视相机下必须有像素，转开相机后必须一个都没有。
//
//   5. **能画进别人的 pass**。真实帧循环里 Nanite 必须走注入这条路：UpdateNanite
//      在任何 pass 之前执行，那时没有 pass 可开。所以调用方自建一个 32×32 的
//      target，自己开 pass、清成纯黑、把视口限制在左上角 16×16，再让 Nanite 追加
//      一次绘制。判据是"有像素"且"所有像素都落在那 16×16 里"—— 覆盖铺满整张图
//      就说明 Nanite 抢过去自己开了 pass 并设了全尺寸视口。
//
//  非 Apple 平台（没有 Metal）退化为 1 + 2：纯布局与打包断言。
//
//   cmake -B build -DZENGINE_BUILD_NANITE_GPU_CULLING_SMOKE_TEST=ON
//   cmake --build build --target NaniteGPUCullingSmokeTest
// =============================================================================

// 这个 TU 在 Apple 上被当作 ObjC++ 编译并跳过了 PCH（见 CMakeLists：它要 include
// MetalRHI.h，而那是 Objective-C 头）。RegisterRuntime.cpp 是同样的处境，所以也
// 同样是**显式**把 pch.h 包含进来 —— 否则 Transfer / TransferMetaFlags 这些只在
// pch 里前置声明过的名字就找不到了。
#include "CommonPCH/pch.h"

#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Nanite/NaniteCluster.h"
#include "Runtime/Function/Render/Nanite/NaniteGPUData.h"
#include "Runtime/Function/Render/Nanite/NaniteRenderer.h"
#include "Runtime/Function/Render/Nanite/NaniteTypes.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// 生成的 SPIR-V 头文件（engine/shader/glsl → engine/shader/generated/cpp）。
// 没有 glslangValidator 的环境根本不会产生它，那种情况下 GPU 那一节必须跳过而不
// 是判失败 —— 否则这个测试会在"没生成 shader"的机器上假红。
#if defined(__has_include)
#if __has_include(<nanite_cluster_cull_comp.h>)
#define ZENGINE_NANITE_TEST_HAS_CULL_SPIRV 1
#endif
#if __has_include(<nanite_cluster_vert.h>)
#define ZENGINE_NANITE_TEST_HAS_VERT_SPIRV 1
#endif
#if __has_include(<nanite_cluster_frag.h>)
#define ZENGINE_NANITE_TEST_HAS_FRAG_SPIRV 1
#endif
#endif

// 只有 Apple 上真的建一个 RHI。这里是唯一一处需要认识具体后端的地方：RHI 基类的
// Initialize / Shutdown 是 protected（给系统注册表用的），而测试要直接驱动它。
#if defined(__APPLE__)
    #include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
#endif

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
    // （kMaxTrianglesPerCluster = 64 ⇒ 至少两层），但又小到打包开销可以忽略。
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
        camera->SetAspect(16.0f / 9.0f);
        camera->setFOVx(60.0f);
        camera->m_Znear = 0.1f;
        camera->m_Zfar = 1000.0f;
        return camera;
    }

    // -----------------------------------------------------------------------
    // 文本工具：把 shader 源当文本读，用来钉 C++ / GLSL 两侧的契约。
    // -----------------------------------------------------------------------
    std::string ReadFileText(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::string();
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // 依次在 text 里找每个 token，且要求下标严格递增 —— 这样"字段顺序"也被钉住了，
    // 不只是"字段存在"。
    bool ContainsInOrder(const std::string& text, const std::vector<const char*>& tokens)
    {
        size_t cursor = 0;
        for (const char* token : tokens)
        {
            if (token == nullptr)
            {
                continue;
            }
            const size_t found = text.find(token, cursor);
            if (found == std::string::npos)
            {
                return false;
            }
            cursor = found + std::strlen(token);
        }
        return true;
    }

    // 按出现顺序抽出所有**片元/顶点输出**的 `layout(location = N) out` 槽位。
    //
    // 只认紧跟 `out` 的那些：`layout(location = N) in` 是输入，混进来会让"恰好三
    // 个输出"这个断言变成恒真。
    std::vector<uint32_t> ExtractOutputLocations(const std::string& text)
    {
        std::vector<uint32_t> locations;
        size_t cursor = 0;
        while (true)
        {
            const size_t at = text.find("layout(location =", cursor);
            if (at == std::string::npos)
            {
                break;
            }
            cursor = at + std::strlen("layout(location =");

            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            {
                ++cursor;
            }
            uint32_t value = 0;
            size_t digits = 0;
            while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')
            {
                value = value * 10u + static_cast<uint32_t>(text[cursor] - '0');
                ++digits;
                ++cursor;
            }
            if (digits == 0)
            {
                continue;
            }

            // `layout(location = N)` 之后跳到 `)`，再看跟着的是不是 `out`。
            const size_t close = text.find(')', cursor);
            if (close == std::string::npos)
            {
                break;
            }
            size_t after = close + 1;
            while (after < text.size() && (text[after] == ' ' || text[after] == '\t' || text[after] == '\n'))
            {
                ++after;
            }
            if (text.compare(after, 3, "out") == 0 &&
                (after + 3 >= text.size() || !std::isalnum(static_cast<unsigned char>(text[after + 3]))))
            {
                locations.push_back(value);
            }
            cursor = close + 1;
        }
        return locations;
    }

    // 按出现顺序抽出所有 `binding = N` 的数字。
    std::vector<uint32_t> ExtractBindings(const std::string& text)
    {
        std::vector<uint32_t> bindings;
        size_t cursor = 0;
        while (true)
        {
            const size_t at = text.find("binding =", cursor);
            if (at == std::string::npos)
            {
                break;
            }
            cursor = at + std::strlen("binding =");

            // 跳过空格后读一个整数
            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            {
                ++cursor;
            }
            uint32_t value = 0;
            size_t digits = 0;
            while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')
            {
                value = value * 10u + static_cast<uint32_t>(text[cursor] - '0');
                ++cursor;
                ++digits;
            }
            if (digits == 0)
            {
                break;
            }
            bindings.push_back(value);
        }
        return bindings;
    }

    bool SameSequence(const std::vector<uint32_t>& lhs, const std::vector<uint32_t>& rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }
        for (size_t i = 0; i < lhs.size(); ++i)
        {
            if (lhs[i] != rhs[i])
            {
                return false;
            }
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // GPU 读回：一次 dispatch 之后的计数器 / 可见列表 / 间接绘制命令。
    // -----------------------------------------------------------------------
    struct CullReadback
    {
        NaniteCullCounter counter {};
        std::vector<uint32_t> visible_list;
        std::vector<RHIDrawIndexedIndirectCommand> commands;
        bool valid {false};
    };

    CullReadback ReadBackCullResult(RHI* rhi, const NaniteGPUResources& gpu, uint32_t cluster_count)
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

        result.visible_list.assign(cluster_count, 0u);
        mapped = nullptr;
        if (rhi->MapMemory(gpu.visible_list_memory, 0, cluster_count * sizeof(uint32_t), 0, &mapped) &&
            mapped != nullptr)
        {
            std::memcpy(result.visible_list.data(), mapped, cluster_count * sizeof(uint32_t));
            rhi->UnmapMemory(gpu.visible_list_memory);
        }

        result.commands.assign(cluster_count, RHIDrawIndexedIndirectCommand {});
        mapped = nullptr;
        if (rhi->MapMemory(gpu.draw_commands_memory,
                           0,
                           static_cast<RHIDeviceSize>(cluster_count) * sizeof(RHIDrawIndexedIndirectCommand),
                           0,
                           &mapped) &&
            mapped != nullptr)
        {
            std::memcpy(result.commands.data(),
                        mapped,
                        static_cast<size_t>(cluster_count) * sizeof(RHIDrawIndexedIndirectCommand));
            rhi->UnmapMemory(gpu.draw_commands_memory);
        }

        result.valid = true;
        return result;
    }

    // -----------------------------------------------------------------------
    // 把离屏 color target 读回 CPU，数一数"被写过的像素"。
    //
    // 光看 raster_ready 不足以证明命令被**消费**了：管线建成功但一条都没画出来
    // 完全可能（viewport 是 0×0、描述符没绑、drawCount 传 0、pass 没开……）。只有
    // 把图读回来才能说"真的有三角形落在上面"。
    // -----------------------------------------------------------------------
    // 把一张 2D color 图读回 CPU。读不回来就返回空 —— 调用方把它当"没有像素"，
    // 于是失败会变成断言失败而不是崩在空指针上。
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
            RHIBufferImageCopy region {};
            region.bufferOffset = 0;
            // Metal 后端把 bufferRowLength 当**字节**用（它自己的兜底值也是
            // imageExtent.width * 4），所以这里给字节数。
            region.bufferRowLength = width * 4u;
            region.bufferImageHeight = height;
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

    // 两边（Nanite 自己的离屏 target 和测试自建的调用方 target）的 GBuffer 清屏值
    // 都是全零 —— 和主相机 DrawGBufferPass 一致。shader 写进去的最暗一档也有
    // G = 0.75 ⇒ 191。取 60 当阈值，跟 0 差两个数量级；而且只看"三个通道里最大的
    // 那个"，跟 BGRA / RGBA 的通道顺序无关。
    //
    // 注意 GBufferC 是 SRGB 格式：清屏值会被编码一次，所以清屏值**必须是 0** ——
    // 换成 0.05 会被编码成 62，直接越过这个阈值，"没画东西"会被读成"画了"。
    constexpr uint8_t kCoveredThreshold = 60;

    uint32_t CountCoveredPixels(RHI* rhi, RHIImage* image, uint32_t width, uint32_t height)
    {
        const std::vector<uint8_t> pixels = ReadBackPixels(rhi, image, width, height);
        uint32_t                   covered = 0;
        for (size_t i = 0; i + 4 <= pixels.size(); i += 4)
        {
            if (pixels[i] > kCoveredThreshold || pixels[i + 1] > kCoveredThreshold ||
                pixels[i + 2] > kCoveredThreshold)
            {
                ++covered;
            }
        }
        return covered;
    }

    // 覆盖像素的**包围盒**。注入那一节靠它区分"Nanite 只追加了一次绘制"和"Nanite
    // 自己开了 pass"：后者会顺手设一个全尺寸的 viewport，覆盖就铺满整张图。
    struct CoveredBounds
    {
        uint32_t count {0};
        uint32_t max_x {0};
        uint32_t max_y {0};
    };

    CoveredBounds CoveredPixelBounds(RHI* rhi, RHIImage* image, uint32_t width, uint32_t height)
    {
        const std::vector<uint8_t> pixels = ReadBackPixels(rhi, image, width, height);
        CoveredBounds              bounds;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
                if (offset + 2 >= pixels.size())
                {
                    continue;
                }
                if (pixels[offset] > kCoveredThreshold || pixels[offset + 1] > kCoveredThreshold ||
                    pixels[offset + 2] > kCoveredThreshold)
                {
                    ++bounds.count;
                    bounds.max_x = std::max(bounds.max_x, x);
                    bounds.max_y = std::max(bounds.max_y, y);
                }
            }
        }
        return bounds;
    }

#if defined(__APPLE__) && defined(ZENGINE_NANITE_TEST_HAS_CULL_SPIRV)
    // -----------------------------------------------------------------------
    // 调用方自建的离屏 target。
    //
    // 格式必须和 Nanite 自己的离屏 target 一致（BGRA8 + D32）：RHIRenderPass 是个
    // 完全不透明的空类，Nanite 拿不到注入 pass 的附件描述，只能靠"两边建得一样"。
    // 这也正是注入接口要一次性收下 render pass **和** framebuffer 的原因 —— 少了
    // 任何一个都拼不出一次合法的 begin。
    // -----------------------------------------------------------------------
    struct CallerTarget
    {
        // 三张颜色图 = GBufferA / B / C，格式和主相机一致。
        RHIImage*        gbuffer[kNaniteRasterColorAttachmentCount] {};
        RHIDeviceMemory* gbuffer_memory[kNaniteRasterColorAttachmentCount] {};
        RHIImageView*    gbuffer_view[kNaniteRasterColorAttachmentCount] {};
        RHIImage*        depth {nullptr};
        RHIDeviceMemory* depth_memory {nullptr};
        RHIImageView*    depth_view {nullptr};
        RHIRenderPass*   render_pass {nullptr};
        RHIFramebuffer*  framebuffer {nullptr};
    };

    CallerTarget MakeCallerTarget(RHI* rhi, uint32_t width, uint32_t height)
    {
        CallerTarget target;
        if (rhi == nullptr || width == 0 || height == 0)
        {
            return target;
        }

        constexpr RHIFormat kGbufferFormats[kNaniteRasterColorAttachmentCount] = {
            RHI_FORMAT_R8G8B8A8_UNORM,  // GBufferA
            RHI_FORMAT_R8G8B8A8_UNORM,  // GBufferB
            RHI_FORMAT_R8G8B8A8_SRGB,   // GBufferC
        };
        constexpr RHIFormat kDepthFormat = RHI_FORMAT_D32_SFLOAT;

        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            rhi->CreateImage(width,
                             height,
                             kGbufferFormats[i],
                             RHI_IMAGE_TILING_OPTIMAL,
                             RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             target.gbuffer[i],
                             target.gbuffer_memory[i],
                             0,
                             1,
                             1);
            rhi->CreateImageView(target.gbuffer[i],
                                 kGbufferFormats[i],
                                 RHI_IMAGE_ASPECT_COLOR_BIT,
                                 RHI_IMAGE_VIEW_TYPE_2D,
                                 1,
                                 1,
                                 target.gbuffer_view[i]);
        }

        rhi->CreateImage(width,
                         height,
                         kDepthFormat,
                         RHI_IMAGE_TILING_OPTIMAL,
                         RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         target.depth,
                         target.depth_memory,
                         0,
                         1,
                         1);
        rhi->CreateImageView(target.depth,
                             kDepthFormat,
                             RHI_IMAGE_ASPECT_DEPTH_BIT,
                             RHI_IMAGE_VIEW_TYPE_2D,
                             1,
                             1,
                             target.depth_view);

        bool ready = target.depth_view != nullptr;
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            if (target.gbuffer_view[i] == nullptr)
            {
                ready = false;
            }
        }
        if (!ready)
        {
            return target;
        }

        constexpr uint32_t kDepthAttachmentIndex = kNaniteRasterColorAttachmentCount;
        constexpr uint32_t kAttachmentCount = kNaniteRasterColorAttachmentCount + 1;

        RHIAttachmentDescription attachments[kAttachmentCount] = {};
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            attachments[i].format        = kGbufferFormats[i];
            attachments[i].samples       = RHI_SAMPLE_COUNT_1_BIT;
            attachments[i].loadOp        = RHI_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[i].storeOp       = RHI_ATTACHMENT_STORE_OP_STORE;
            attachments[i].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
            attachments[i].finalLayout   = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        attachments[kDepthAttachmentIndex].format        = kDepthFormat;
        attachments[kDepthAttachmentIndex].samples       = RHI_SAMPLE_COUNT_1_BIT;
        attachments[kDepthAttachmentIndex].loadOp        = RHI_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[kDepthAttachmentIndex].storeOp       = RHI_ATTACHMENT_STORE_OP_STORE;
        attachments[kDepthAttachmentIndex].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        attachments[kDepthAttachmentIndex].finalLayout   = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        RHIAttachmentReference color_references[kNaniteRasterColorAttachmentCount] = {};
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            color_references[i].attachment = i;
            color_references[i].layout     = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        RHIAttachmentReference depth_reference {};
        depth_reference.attachment = kDepthAttachmentIndex;
        depth_reference.layout     = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        RHISubpassDescription subpass {};
        subpass.pipelineBindPoint     = RHI_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount  = kNaniteRasterColorAttachmentCount;
        subpass.pColorAttachments     = color_references;
        subpass.pDepthStencilAttachment = &depth_reference;

        RHIRenderPassCreateInfo render_pass_info {};
        render_pass_info.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = kAttachmentCount;
        render_pass_info.pAttachments    = attachments;
        render_pass_info.subpassCount    = 1;
        render_pass_info.pSubpasses      = &subpass;

        if (!rhi->CreateRenderPass(&render_pass_info, target.render_pass))
        {
            return target;
        }

        RHIImageView* views[kAttachmentCount] = {};
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            views[i] = target.gbuffer_view[i];
        }
        views[kDepthAttachmentIndex] = target.depth_view;

        RHIFramebufferCreateInfo framebuffer_info {};
        framebuffer_info.sType         = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass    = target.render_pass;
        framebuffer_info.attachmentCount = kAttachmentCount;
        framebuffer_info.pAttachments  = views;
        framebuffer_info.width         = width;
        framebuffer_info.height        = height;
        framebuffer_info.layers        = 1;

        rhi->CreateFramebuffer(&framebuffer_info, target.framebuffer);
        return target;
    }

    void DestroyCallerTarget(RHI* rhi, CallerTarget& target)
    {
        if (rhi == nullptr)
        {
            return;
        }
        if (target.framebuffer != nullptr)
        {
            rhi->DestroyFramebuffer(target.framebuffer);
        }
        if (target.render_pass != nullptr)
        {
            rhi->DestroyRenderPass(target.render_pass);
        }
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            if (target.gbuffer_view[i] != nullptr)
            {
                rhi->DestroyImageView(target.gbuffer_view[i]);
            }
        }
        if (target.depth_view != nullptr)
        {
            rhi->DestroyImageView(target.depth_view);
        }
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            if (target.gbuffer[i] != nullptr)
            {
                rhi->DestroyImage(target.gbuffer[i]);
            }
        }
        if (target.depth != nullptr)
        {
            rhi->DestroyImage(target.depth);
        }
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            if (target.gbuffer_memory[i] != nullptr)
            {
                rhi->FreeMemory(target.gbuffer_memory[i]);
            }
        }
        if (target.depth_memory != nullptr)
        {
            rhi->FreeMemory(target.depth_memory);
        }
        target = CallerTarget {};
    }

    // -----------------------------------------------------------------------
    // 从 GBuffer 的**像素**里把语义读回来。
    //
    // 只数像素是不够的：写 1 个 target 和写 3 个都会"有像素"。这里把 GBufferA 解
    // 码成法线、GBufferB.a 解码成 shading model id、GBufferC 当 baseColor，逐项
    // 比对 —— 少写一个 target，对应的那一项就是清屏值。
    // -----------------------------------------------------------------------
    struct GBufferProbe
    {
        uint32_t covered {0};       // 有内容的像素数（三张里任意一张不等于清屏色）
        bool     normal_valid {false};    // 每个覆盖像素解码出的法线都指向 +Y
        bool     shading_model_valid {false};  // GBufferB.a 解码出 DEFAULT_LIT(1)
        bool     base_color_valid {false};     // GBufferC 不是清屏色
        bool     clear_intact {false};    // 没被覆盖的像素仍是纯黑（清屏色没被改写）
        float    normal_y {0.0f};   // 覆盖像素解码法线的 y 分量（取第一个）
    };

    GBufferProbe ProbeGBuffer(RHI* rhi, RHIImage* const* gbuffer, uint32_t width, uint32_t height)
    {
        GBufferProbe probe;
        if (rhi == nullptr || gbuffer == nullptr)
        {
            return probe;
        }

        std::vector<uint8_t> targets[kNaniteRasterColorAttachmentCount];
        for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
        {
            targets[i] = ReadBackPixels(rhi, gbuffer[i], width, height);
            if (targets[i].size() < static_cast<size_t>(width) * height * 4u)
            {
                return probe;
            }
        }

        bool  any_covered = false;
        bool  normal_ok = true;
        bool  shading_ok = true;
        bool  base_ok = true;
        bool  clear_ok = true;
        uint32_t uncovered = 0;
        float first_normal_y = -2.0f;

        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
                const uint8_t* a = targets[0].data() + offset;
                const uint8_t* b = targets[1].data() + offset;
                const uint8_t* c = targets[2].data() + offset;

                // 清屏色 (0.05, 0.05, 0.06) ⇒ (13, 13, 15)。阈值 60 离它一个数量级。
                const bool a_covered = a[0] > kCoveredThreshold || a[1] > kCoveredThreshold ||
                                       a[2] > kCoveredThreshold;
                const bool c_covered = c[0] > kCoveredThreshold || c[1] > kCoveredThreshold ||
                                       c[2] > kCoveredThreshold;
                if (!a_covered && !c_covered)
                {
                    // 这个像素没被 Nanite 碰到 ⇒ 它必须还停在调用方的清屏色上。
                    // 纯黑 = (0,0,0)；Nanite 自己的清屏色是 (13,13,15)，一旦它自己
                    // 重开了一次 pass 就会留在图上，这里立刻变红。
                    ++uncovered;
                    if (a[0] != 0 || a[1] != 0 || a[2] != 0 || c[0] != 0 || c[1] != 0 || c[2] != 0)
                    {
                        clear_ok = false;
                    }
                    continue;
                }

                ++probe.covered;
                any_covered = true;

                // GBufferA.rgb = N * 0.5 + 0.5 ⇒ N = a/255 * 2 - 1
                const float nx = static_cast<float>(a[0]) / 255.0f * 2.0f - 1.0f;
                const float ny = static_cast<float>(a[1]) / 255.0f * 2.0f - 1.0f;
                const float nz = static_cast<float>(a[2]) / 255.0f * 2.0f - 1.0f;
                if (!(ny > 0.5f))
                {
                    // 测试网格是 y = 0 的水平面、相机在上方，正面法线必须指向 +Y。
                    // 方向反了只可能是 cross(dFdx, dFdy) 的两项写反了。
                    normal_ok = false;
                }
                if (first_normal_y < -1.0f)
                {
                    first_normal_y = ny;
                }
                // 顺便要求它是个像样的方向向量（不是清屏色解码出的垃圾）
                if (std::fabs(nx) > 0.5f || std::fabs(nz) > 0.5f)
                {
                    normal_ok = false;
                }

                // GBufferB.a = shadingModelId / 255 ⇒ DEFAULT_LIT(1) ⇒ 1
                if (b[3] != 1)
                {
                    shading_ok = false;
                }

                if (!c_covered)
                {
                    base_ok = false;
                }
            }
        }

        (void)any_covered;
        probe.normal_valid = normal_ok;
        probe.shading_model_valid = shading_ok;
        probe.base_color_valid = base_ok;
        probe.clear_intact = (uncovered > 0) && clear_ok;
        probe.normal_y = first_normal_y;
        return probe;
    }
#endif
}  // namespace

// GLSL 的 NaniteDrawCommand 是 5 个 4 字节标量（20 字节）。它必须和 RHI 的间接
// 绘制命令结构完全一致 —— kernel 直接把后者写进间接缓冲，一个字节的偏差会让
// GPU 读到完全错误的 indexCount。
static_assert(sizeof(RHIDrawIndexedIndirectCommand) == 20,
              "RHIDrawIndexedIndirectCommand must match GLSL NaniteDrawCommand (5 x uint32)");

int main()
{
    std::printf("=== Nanite GPU culling smoke test ===\n\n");

    // =====================================================================
    // 1. C++ ↔ GLSL 的字节契约（全平台）
    //
    // shader 在这个测试里从不编译，所以镜像漏改是**不可见**的 —— 唯一能抓住它的
    // 办法就是把源当文本读一遍。
    // =====================================================================
#ifdef ZENGINE_ENGINE_ROOT_DIR
    const std::string shader_root = std::string(ZENGINE_ENGINE_ROOT_DIR) + "/shader/";
    const std::string common_source = ReadFileText(shader_root + "include/nanite_common.h");
    const std::string cull_source = ReadFileText(shader_root + "glsl/nanite_cluster_cull.comp");
    const std::string vert_source = ReadFileText(shader_root + "glsl/nanite_cluster.vert");
    const std::string frag_source = ReadFileText(shader_root + "glsl/nanite_cluster.frag");
    const std::string gbuffer_source = ReadFileText(shader_root + "include/gbuffer.h");

    Check(!common_source.empty(), "the shared GLSL struct header was read");
    Check(!cull_source.empty(), "the cluster-cull compute shader was read");
    Check(!vert_source.empty(), "the cluster raster vertex shader was read");
    Check(!frag_source.empty(), "the cluster raster fragment shader was read");
    Check(!gbuffer_source.empty(), "the shared GBuffer layout header was read");

    // NaniteGPUCluster：80 字节，五个 vec4/uvec4，**顺序**必须和 C++ 一致。
    Check(ContainsInOrder(common_source,
                          {"struct NaniteGPUCluster",
                           "center_radius",
                           "bounds_min",
                           "bounds_max",
                           "geometry",
                           "flags"}),
          "GLSL NaniteGPUCluster declares its five fields in the C++ order");

    // NaniteGPUInstance：176 字节，两个 mat4 + 两个 vec4 + 一个 uvec4。
    Check(ContainsInOrder(common_source,
                          {"struct NaniteGPUInstance",
                           "transform",
                           "prev_transform",
                           "bounds_min",
                           "bounds_max",
                           "meta"}),
          "GLSL NaniteGPUInstance declares its five fields in the C++ order");

    // NaniteDrawCommand：VkDrawIndexedIndirectCommand / D3D12_DRAW_INDEXED_ARGUMENTS
    // / MTLDrawIndexedPrimitivesIndirectArguments 都是这个顺序。
    Check(ContainsInOrder(common_source,
                          {"struct NaniteDrawCommand",
                           "indexCount",
                           "instanceCount",
                           "firstIndex",
                           "vertexOffset",
                           "firstInstance"}),
          "GLSL NaniteDrawCommand matches RHIDrawIndexedIndirectCommand field for field");

    // 描述符槽：C++ 的 NaniteGPUBinding 与 GLSL 的 layout(binding = N) 必须一一对应。
    // 写错一个槽，kernel 会读到完全无关的缓冲，而编译期毫无提示。
    Check(SameSequence(ExtractBindings(cull_source),
                       {NaniteGPUBinding::kCullConstants,
                        NaniteGPUBinding::kClusters,
                        NaniteGPUBinding::kInstances,
                        NaniteGPUBinding::kVisibleList,
                        NaniteGPUBinding::kDrawCommands,
                        NaniteGPUBinding::kDrawCounter}),
          "the cull kernel binds its six resources at the slots NaniteGPUBinding declares");
    Check(SameSequence(ExtractBindings(vert_source),
                       {NaniteGPUBinding::kCullConstants,
                        NaniteGPUBinding::kClusters,
                        NaniteGPUBinding::kInstances,
                        NaniteGPUBinding::kPositions}),
          "the raster vertex shader binds constants / clusters / instances / positions in order");

    // CPU 算 group 数时写死了 64（`(n + 63) / 64`）。shader 侧的 local_size_x 改了
    // 而 CPU 没改 ⇒ 一半集群永远轮不到线程，静默少画。
    Check(cull_source.find("local_size_x = 64") != std::string::npos,
          "the cull kernel still declares local_size_x = 64 (DispatchClusterCulling hard-codes it)");

    // ---- GBuffer：片元输出必须恰好三个，槽位 0/1/2 ----
    //
    // gbuffer.h 里那句 "EXACTLY THREE" 不是修辞：GBuffer 的 render pass 只有三个
    // 颜色附件，多声明一个输出就配不上 pass（Vulkan 拒绝建管线、DX12 静默回退到
    // 内置 shader，作者写的东西全丢）。而少声明一个 ⇒ GBufferB/C 留着清屏色，
    // 延迟光照拿到的是"法线对、材质空"的半成品 —— 画面不会报错，只会不对。
    //
    // 光栅管线在 C++ 侧按 kNaniteRasterColorAttachmentCount 声明 blend 附件，所以
    // 这两边也得对上。
    Check(SameSequence(ExtractOutputLocations(frag_source),
                       {0, 1, 2}),
          "the raster fragment shader writes exactly three outputs, at locations 0/1/2");
    Check(frag_source.find("#include \"gbuffer.h\"") != std::string::npos &&
              frag_source.find("EncodeGBufferData") != std::string::npos,
          "it encodes them through the shared GBuffer helpers instead of hand-rolling the packing");
    Check(frag_source.find("SHADINGMODELID_DEFAULT_LIT") != std::string::npos,
          "Nanite surfaces are tagged DEFAULT_LIT so the deferred pass actually lights them");
    Check(gbuffer_source.find("OutGBufferB.a = EncodeShadingModelId") != std::string::npos,
          "the shared encoder still packs the shading model id into GBufferB.a");
#else
    std::printf("[skip] ZENGINE_ENGINE_ROOT_DIR is not defined -- GLSL mirror checks skipped\n");
#endif

    // =====================================================================
    // 2. 打包（全平台）
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
    Check(mesh.total_triangle_count == static_cast<uint32_t>(grid.indices.size() / 3),
          "the mesh resource reports every input triangle");

    uint32_t leaf_count = 0;
    uint32_t max_lod_level = 0;
    for (const NaniteCluster& cluster : mesh.clusters)
    {
        if (cluster.is_leaf)
        {
            ++leaf_count;
        }
        max_lod_level = std::max(max_lod_level, static_cast<uint32_t>(cluster.lod_level));
    }
    Check(leaf_count > 0, "the mesh has leaf clusters");
    Check(max_lod_level > 0, "the hierarchy has more than one LOD level (otherwise LOD is untestable)");

    // 两个实例：一个在相机前面，一个在相机背后。
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

    NaniteClusterManager manager;
    Check(manager.Initialize(nullptr, build_config), "the cluster manager initialises without an RHI");
    Check(manager.RegisterMeshResource(1, mesh), "the mesh registers with the cluster manager");

    NaniteSceneGPUData packed;
    NaniteBuildSceneGPUData(instances, manager, packed);

    const uint32_t cluster_count = static_cast<uint32_t>(packed.clusters.size());
    Check(packed.instances.size() == 2, "both instances were packed");
    Check(cluster_count == 2u * static_cast<uint32_t>(mesh.clusters.size()),
          "every instance contributes a full copy of the mesh's clusters");

    // 每个实例一段连续区间，且集群记录里烘着它属于哪个实例。
    {
        bool ranges_contiguous = true;
        bool instance_tags_correct = true;
        for (uint32_t i = 0; i < cluster_count; ++i)
        {
            const uint32_t expected_instance = (i < mesh.clusters.size()) ? 0u : 1u;
            if (packed.clusters[i].geometry.w != expected_instance)
            {
                instance_tags_correct = false;
            }
        }
        for (size_t instance_index = 0; instance_index < packed.instances.size(); ++instance_index)
        {
            const NaniteGPUInstance& gpu_instance = packed.instances[instance_index];
            if (gpu_instance.meta.z != instance_index * static_cast<uint32_t>(mesh.clusters.size()) ||
                gpu_instance.meta.w != static_cast<uint32_t>(mesh.clusters.size()))
            {
                ranges_contiguous = false;
            }
        }
        Check(instance_tags_correct, "each packed cluster carries the index of the instance it belongs to");
        Check(ranges_contiguous, "each instance owns one contiguous range of the cluster array");
    }

    // 索引值必须是**全局**顶点下标（加过该实例的顶点基址），否则间接绘制的
    // vertexOffset = 0 会把第二个实例的第一个集群画到第一个实例的顶点上。
    {
        const uint32_t per_instance_vertices = mesh.total_vertex_count;
        bool indices_are_global = true;
        bool indices_in_range = true;
        for (uint32_t instance_index = 0; instance_index < 2; ++instance_index)
        {
            const uint32_t base = instance_index * per_instance_vertices;
            const uint32_t first_cluster = instance_index * static_cast<uint32_t>(mesh.clusters.size());
            const NaniteGPUCluster& cluster = packed.clusters[first_cluster];
            for (uint32_t k = cluster.geometry.y; k < cluster.geometry.y + cluster.geometry.z &&
                                                  k < static_cast<uint32_t>(packed.indices.size());
                 ++k)
            {
                const uint32_t value = packed.indices[k];
                if (value < base || value >= base + per_instance_vertices)
                {
                    indices_are_global = false;
                }
                if (value >= static_cast<uint32_t>(packed.positions.size()))
                {
                    indices_in_range = false;
                }
            }
        }
        Check(indices_are_global, "index values are global vertex indices (the instance's base was added)");
        Check(indices_in_range, "no index points past the packed vertex array");
    }

    // 矩阵必须按**列主序**打包。直接 memcpy 行主序的 m_Mat，shader 里
    // `transform * vec4(p, 1)` 得到的是 Mᵀ·p —— 位置全错但不报错。
    {
        NaniteGPUInstance translation_only {};
        Matrix4x4 translated = Matrix4x4::IDENTITY;
        translated.setTrans(Vector3(1.0f, 2.0f, 3.0f));
        NaniteGPUPackMatrix(translated, &translation_only.transform.m_Mat[0][0]);

        // 列主序下平移分量落在第 12/13/14 个 float 上。
        const float* as_floats = &translation_only.transform.m_Mat[0][0];
        Check(std::fabs(as_floats[12] - 1.0f) < 1e-6f && std::fabs(as_floats[13] - 2.0f) < 1e-6f &&
                  std::fabs(as_floats[14] - 3.0f) < 1e-6f,
              "PackMatrix writes the translation into the column-major slots (12/13/14)");
        Check(std::fabs(as_floats[3] - 0.0f) < 1e-6f && std::fabs(as_floats[7] - 0.0f) < 1e-6f,
              "PackMatrix leaves the row-major translation slots zero (i.e. it really transposed)");
    }

    // =====================================================================
    // 3. 真的 dispatch（只有 Apple 有 Metal；没有生成 SPIR-V 就跳过）
    // =====================================================================
#if defined(__APPLE__) && defined(ZENGINE_NANITE_TEST_HAS_CULL_SPIRV)
    {
        MetalRHI metal_rhi;
        RHI* rhi = &metal_rhi;

        // Initialize 只需要一个 MTLDevice 和一个 command queue —— 不需要窗口、
        // 不需要 CAMetalLayer。这正是"无头也能验证 GPU 剔除"的前提。
        const bool metal_ready = metal_rhi.Initialize();
        Check(metal_ready, "MetalRHI::Initialize succeeds without a window");
        if (!metal_ready)
        {
            std::printf("[skip] no Metal device on this host -- GPU dispatch checks skipped\n");
            std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
            return g_Failures == 0 ? 0 : 1;
        }

        NaniteConfig config;
        config.use_gpu_culling = true;
        config.enable_culling = false;
        config.enable_lod = false;

        NaniteRenderer renderer;
        Check(renderer.Initialize(rhi, nullptr, config), "the Nanite renderer initialises against a real RHI");
        Check(renderer.RegisterMeshResource(1, mesh), "the mesh registers with the renderer");

        // 注册时**不填** local_bounds ⇒ 渲染器从网格资源补上。GPU 侧拿的是集群的
        // 模型空间盒乘实例变换，所以这一步错了世界盒就是错的。
        NaniteInstance front;
        front.instance_id = 1;
        front.mesh_id = 1;
        front.transform = Matrix4x4::IDENTITY;
        renderer.RegisterInstance(front);

        NaniteInstance back = front;
        back.instance_id = 2;
        back.transform = Matrix4x4::IDENTITY;
        back.transform.setTrans(Vector3(0.0f, 0.0f, 40.0f));
        renderer.RegisterInstance(back);

        auto camera = MakeCamera(Vector3(0.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, 0.0f));

        Check(renderer.PrepareGPUData(camera, config), "PrepareGPUData packs and uploads the scene");
        Check(renderer.GetGPUResources().compute_ready,
              "the cluster-cull compute pipeline was built from the generated SPIR-V");
        Check(renderer.GetSceneGPUData().clusters.size() == cluster_count,
              "the renderer packed the same cluster count as the direct call");

        const NaniteGPUResources& gpu = renderer.GetGPUResources();
        Check(gpu.clusters != nullptr && gpu.instances != nullptr && gpu.draw_commands != nullptr &&
                  gpu.cull_counter != nullptr && gpu.cull_constants != nullptr && gpu.visible_list != nullptr &&
                  gpu.positions != nullptr && gpu.indices != nullptr,
              "all eight GPU buffers exist (were: eight void* placeholders)");

        // ---- 3a. 关掉裁剪与 LOD：恰好等于叶子集群总数 -------------------------
        renderer.DispatchClusterCulling(camera, config);
        CullReadback no_cull = ReadBackCullResult(rhi, gpu, cluster_count);
        Check(no_cull.valid, "the cull counter could be mapped and read back");

        const uint32_t expected_leaves = 2u * leaf_count;
        Check(no_cull.counter.visible_count == expected_leaves,
              "with culling and LOD off every leaf cluster survives (the kernel actually ran)");
        Check(no_cull.counter.visible_count + no_cull.counter.culled_count == cluster_count,
              "every cluster lands in exactly one bucket -- visible + culled == total");
        Check(no_cull.counter.draw_count == no_cull.counter.visible_count,
              "draw_count matches visible_count (one non-empty command per visible cluster)");

        {
            bool commands_match = true;
            uint32_t non_empty = 0;
            for (uint32_t i = 0; i < cluster_count; ++i)
            {
                const bool should_draw = renderer.GetSceneGPUData().clusters[i].flags.x != 0u;
                const bool does_draw = no_cull.commands[i].indexCount != 0u;
                if (should_draw != does_draw)
                {
                    commands_match = false;
                }
                if (does_draw)
                {
                    ++non_empty;
                    // 每条命令必须带上它自己的几何窗口，否则画出来的是别的东西
                    if (no_cull.commands[i].firstIndex != renderer.GetSceneGPUData().clusters[i].geometry.y ||
                        no_cull.commands[i].indexCount != renderer.GetSceneGPUData().clusters[i].geometry.z ||
                        no_cull.commands[i].instanceCount != 1u ||
                        no_cull.commands[i].firstInstance != i ||
                        no_cull.commands[i].vertexOffset != 0)
                    {
                        commands_match = false;
                    }
                }
            }
            Check(non_empty == expected_leaves, "the indirect buffer holds one non-empty command per leaf");
            Check(commands_match,
                  "each command carries its own index window, one instance, and its cluster id as firstInstance");
        }

        // ---- 3b. 打开裁剪：相机背后那个实例的集群一个都不许出现 -----------------
        NaniteConfig culling_on = config;
        culling_on.enable_culling = true;
        renderer.PrepareGPUData(camera, culling_on);
        renderer.DispatchClusterCulling(camera, culling_on);
        CullReadback culled = ReadBackCullResult(rhi, gpu, cluster_count);

        Check(culled.counter.visible_count > 0, "culling on still keeps the instance in front of the camera");
        Check(culled.counter.visible_count < no_cull.counter.visible_count,
              "frustum culling actually removes clusters (was: FrustumCull was `return true;`)");
        Check(culled.counter.visible_count + culled.counter.culled_count == cluster_count,
              "the visible/culled split still accounts for every cluster");

        {
            const uint32_t back_instance_first = static_cast<uint32_t>(mesh.clusters.size());
            bool back_instance_silent = true;
            for (uint32_t i = 0; i < cluster_count; ++i)
            {
                if (culled.commands[i].indexCount != 0u && i >= back_instance_first)
                {
                    back_instance_silent = false;
                }
            }
            Check(back_instance_silent,
                  "no cluster of the instance behind the camera produced a draw command");
        }

        {
            bool visible_list_agrees = true;
            std::vector<bool> seen(cluster_count, false);
            for (uint32_t slot = 0; slot < culled.counter.visible_count; ++slot)
            {
                const uint32_t cluster_index = culled.visible_list[slot];
                if (cluster_index >= cluster_count || seen[cluster_index])
                {
                    visible_list_agrees = false;
                    continue;
                }
                seen[cluster_index] = true;
                if (culled.commands[cluster_index].indexCount == 0u)
                {
                    visible_list_agrees = false;
                }
            }
            Check(visible_list_agrees,
                  "the visible list holds distinct cluster indices, each with a non-empty command");
        }

        // ---- 3c. 相机转开：什么都不剩 ----------------------------------------
        auto looking_away = MakeCamera(Vector3(0.0f, 0.0f, 10.0f), Vector3(1000.0f, 0.0f, 10.0f));
        renderer.PrepareGPUData(looking_away, culling_on);
        renderer.DispatchClusterCulling(looking_away, culling_on);
        CullReadback away = ReadBackCullResult(rhi, gpu, cluster_count);
        Check(away.counter.visible_count == 0, "turning the camera away culls every cluster");
        Check(away.counter.draw_count == 0, "no draw command survives when nothing is visible");

        // ---- 3d. LOD：容差松到离谱 ⇒ 只留 LOD 0 --------------------------------
        NaniteConfig lod_fine = config;
        lod_fine.enable_lod = true;
        lod_fine.max_screen_error = 1e-9f;  // 一个像素都不许错 ⇒ 全都嫌粗 ⇒ 全留
        renderer.PrepareGPUData(camera, lod_fine);
        renderer.DispatchClusterCulling(camera, lod_fine);
        CullReadback lod_fine_result = ReadBackCullResult(rhi, gpu, cluster_count);
        Check(lod_fine_result.counter.visible_count == expected_leaves,
              "an impossibly tight error budget keeps every leaf (the LOD branch went the right way)");

        NaniteConfig lod_coarse = config;
        lod_coarse.enable_lod = true;
        lod_coarse.max_screen_error = 1e9f;  // 什么误差都能忍 ⇒ 往根走
        renderer.PrepareGPUData(camera, lod_coarse);
        renderer.DispatchClusterCulling(camera, lod_coarse);
        CullReadback lod_coarse_result = ReadBackCullResult(rhi, gpu, cluster_count);
        Check(lod_coarse_result.counter.visible_count < lod_fine_result.counter.visible_count,
              "a huge error budget drops clusters (LOD is not a no-op)");
        {
            bool only_root_level = true;
            for (uint32_t slot = 0; slot < lod_coarse_result.counter.visible_count; ++slot)
            {
                const uint32_t cluster_index = lod_coarse_result.visible_list[slot];
                if (cluster_index >= cluster_count ||
                    renderer.GetSceneGPUData().clusters[cluster_index].bounds_max.w > 0.5f)
                {
                    only_root_level = false;
                }
            }
            Check(only_root_level, "everything that survives the huge error budget is a LOD-0 cluster");
        }

        // ---- 3e. 尾部槽位必须被清零 -------------------------------------------
        //
        // kernel 只写 [0, cluster_count)，group 向上取整多出来的槽位它一个都不碰。
        // 那些槽位今天不会被画到（drawCount = cluster_count），但只要集群数一涨回去
        // 它们立刻进入绘制范围 —— 所以 DispatchClusterCulling 清的是整个容量段。
        // 这里往尾部灌一份垃圾再 dispatch，确认下一帧真的把它清掉了。
        {
            const uint32_t capacity = gpu.cluster_capacity;
            Check(capacity > cluster_count, "the command buffer is over-allocated, so a tail exists to check");

            void* tail = nullptr;
            const RHIDeviceSize tail_bytes = static_cast<RHIDeviceSize>(capacity) * sizeof(RHIDrawIndexedIndirectCommand);
            if (rhi->MapMemory(gpu.draw_commands_memory, 0, tail_bytes, 0, &tail) && tail != nullptr)
            {
                auto* commands = static_cast<RHIDrawIndexedIndirectCommand*>(tail);
                for (uint32_t slot = cluster_count; slot < capacity; ++slot)
                {
                    commands[slot].indexCount = 0xCDCDCDCDu;
                    commands[slot].instanceCount = 0xCDCDCDCDu;
                    commands[slot].firstIndex = 0xCDCDCDCDu;
                    commands[slot].vertexOffset = 0x7FFFFFFF;
                    commands[slot].firstInstance = 0xCDCDCDCDu;
                }
                rhi->UnmapMemory(gpu.draw_commands_memory);
            }

            renderer.PrepareGPUData(camera, culling_on);
            renderer.DispatchClusterCulling(camera, culling_on);

            CullReadback after = ReadBackCullResult(rhi, gpu, cluster_count);
            bool tail_clean = false;
            void* tail_check = nullptr;
            if (rhi->MapMemory(gpu.draw_commands_memory, 0, tail_bytes, 0, &tail_check) && tail_check != nullptr)
            {
                auto* commands = static_cast<const RHIDrawIndexedIndirectCommand*>(tail_check);
                tail_clean = true;
                for (uint32_t slot = cluster_count; slot < capacity; ++slot)
                {
                    if (commands[slot].indexCount != 0u || commands[slot].instanceCount != 0u)
                    {
                        tail_clean = false;
                    }
                }
                rhi->UnmapMemory(gpu.draw_commands_memory);
            }
            Check(tail_clean, "the tail slots the kernel never writes are cleared before the next dispatch");
            Check(after.counter.visible_count == culled.counter.visible_count,
                  "re-dispatching the same frame reproduces the same visible count");
        }

        // ---- 3f. 光栅阶段：真的把集群画到离屏 target 上 -------------------------
        //
        // SetRasterRenderPass 以前**一个调用者都没有**，于是 EnsureRasterPipeline
        // 永远不成立、DrawClustersIndirect 永远走 no-op：剔除在 GPU 上跑完了，产出的
        // 间接命令却一条都没被消费。这一节把它钉死 —— 把离屏 color target 读回来
        // 数像素，证明三角形真的落到了图上。
#if defined(ZENGINE_NANITE_TEST_HAS_VERT_SPIRV) && defined(ZENGINE_NANITE_TEST_HAS_FRAG_SPIRV)
        {
            // 测试网格是 y = 0 上的一张水平面，而 §3a~3e 的相机也在 y = 0 —— 正好
            // 侧视，投影面积为零，画出来什么都没有。光栅这一节要的是**看得见**的
            // 相机：抬高到 y = 6 俯视，同时保持"z = +40 那个实例仍在背后"的关系。
            auto raster_camera = MakeCamera(Vector3(0.0f, 6.0f, 10.0f), Vector3(0.0f, 0.0f, 0.0f));

            constexpr uint32_t kTargetSize = 64;
            renderer.SetRasterTargetSize(kTargetSize, kTargetSize);

            renderer.PrepareGPUData(raster_camera, culling_on);
            renderer.DispatchClusterCulling(raster_camera, culling_on);
            renderer.DrawClustersIndirect();

            const NaniteGPUResources& raster = renderer.GetGPUResources();
            Check(raster.raster_ready,
                  "the raster pipeline was built (DrawClustersIndirect is no longer a no-op)");
            Check(raster.target_gbuffer[0] != nullptr && raster.target_gbuffer[1] != nullptr &&
                      raster.target_gbuffer[2] != nullptr && raster.target_depth != nullptr,
                  "Nanite created its own offscreen GBuffer (three colour targets + depth)");
            Check(raster.target_render_pass != nullptr && raster.target_framebuffer != nullptr,
                  "and it owns the render pass that draws into them");
            Check(raster.target_width == kTargetSize && raster.target_height == kTargetSize,
                  "the offscreen target honours the requested size");
            Check(raster.raster_framebuffer == raster.target_framebuffer,
                  "Nanite opens and closes the pass itself when none was injected");

            const GBufferProbe drawn = ProbeGBuffer(
                rhi, raster.target_gbuffer, raster.target_width, raster.target_height);
            Check(drawn.covered > 0,
                  "the offscreen GBuffer holds pixels the clear colour cannot explain (clusters were rasterised)");
            Check(drawn.normal_valid,
                  "GBufferA decodes to a +Y world normal on every covered pixel (the test plane is horizontal)");
            Check(drawn.shading_model_valid,
                  "GBufferB.a decodes to SHADINGMODELID_DEFAULT_LIT, so the deferred pass will light Nanite geometry");
            Check(drawn.base_color_valid,
                  "GBufferC is written too -- all three targets, not just the first");

            // 对照：**同一个视点**把相机转开 ⇒ 一个集群都活不下来 ⇒ 目标只剩清屏色。
            // 没有这一条，"读回来有东西"完全可能只是没清干净的垃圾。用同一个高度是
            // 为了让"0 像素"只能归因于剔除，而不是归因于视角退化。
            auto raster_away = MakeCamera(Vector3(0.0f, 6.0f, 10.0f), Vector3(1000.0f, 6.0f, 10.0f));
            renderer.PrepareGPUData(raster_away, culling_on);
            renderer.DispatchClusterCulling(raster_away, culling_on);
            CullReadback raster_away_readback = ReadBackCullResult(rhi, raster, cluster_count);
            Check(raster_away_readback.counter.visible_count == 0,
                  "the control camera really leaves nothing visible (so 0 pixels means culling, not a dead angle)");

            renderer.DrawClustersIndirect();

            const NaniteGPUResources& away_gpu = renderer.GetGPUResources();
            Check(ProbeGBuffer(rhi, away_gpu.target_gbuffer, away_gpu.target_width, away_gpu.target_height)
                      .covered == 0,
                  "with the camera turned away the GBuffer is left at the clear colour");
        }
#endif

        // ---- 3g. 注入：Nanite 往**调用方**开着的 pass 里追加一次绘制 -------------
        //
        // 离屏那半边证明的是"Nanite 能画"，这一节证明的是"Nanite 肯在别人的 pass
        // 里画，而且不抢 pass 的控制权"。真实帧循环里 Nanite 必须走这条路：
        // UpdateNanite 在任何 pass 之前执行，那时没有 pass 可开，画出来的东西也没
        // 人读 —— 只有主相机把自己的 GBuffer pass 开着的时候画才有意义。
        //
        // 判据分四层，缺一层就会被某种"看起来对"的实现骗过去：
        //   * 有像素                      ⇒ 排除"注入被忽略 / 退回没人读的离屏 target"
        //   * 三张 GBuffer 都有语义内容   ⇒ 排除"只写了第一个附件 / 还是 LOD 调试色那套"
        //   * 覆盖全落在 16×16 里         ⇒ 排除"Nanite 自己开了 pass 并设了全尺寸视口"
        //   * 没覆盖的像素仍是调用方的纯黑 ⇒ 排除"Nanite 用自己的清屏色重清了一遍"
#if defined(ZENGINE_NANITE_TEST_HAS_VERT_SPIRV) && defined(ZENGINE_NANITE_TEST_HAS_FRAG_SPIRV)
        {
            constexpr uint32_t kCallerSize = 32;    // 调用方 target：32×32
            constexpr uint32_t kViewportSize = 16;  // 只给左上角四分之一
            constexpr uint32_t kInjectedClearCount = kNaniteRasterColorAttachmentCount + 1;  // 3 张 GBuffer + 深度

            auto inject_camera = MakeCamera(Vector3(0.0f, 6.0f, 10.0f), Vector3(0.0f, 0.0f, 0.0f));

            CallerTarget caller = MakeCallerTarget(rhi, kCallerSize, kCallerSize);
            const bool caller_ready = caller.render_pass != nullptr && caller.framebuffer != nullptr;
            Check(caller_ready,
                  "the caller built its own GBuffer (three colour + depth) target and render pass");

            if (caller_ready)
            {
                renderer.SetRasterRenderPass(caller.render_pass, caller.framebuffer);
                Check(renderer.GetInjectedRasterRenderPass() == caller.render_pass,
                      "SetRasterRenderPass takes the injected pass (both arguments non-null)");

                renderer.PrepareGPUData(inject_camera, culling_on);
                renderer.DispatchClusterCulling(inject_camera, culling_on);

                // 注入生效期间，离屏那条路必须让位：否则每帧会往没人读的图上多画一份。
                renderer.DrawClustersIndirect();
                const NaniteGPUResources& offscreen_during = renderer.GetGPUResources();
                Check(ProbeGBuffer(rhi,
                                   offscreen_during.target_gbuffer,
                                   offscreen_during.target_width,
                                   offscreen_during.target_height)
                          .covered == 0,
                      "while a pass is injected, DrawClustersIndirect does not also draw into the offscreen GBuffer");

                RHICommandBuffer* injected_cb = rhi->BeginSingleTimeCommands();
                Check(injected_cb != nullptr, "a one-shot command buffer is available for the injected pass");
                if (injected_cb != nullptr)
                {
                    // 调用方开 pass，三张 GBuffer 全部清成**纯黑**（不是 Nanite 的
                    // 0.05/0.05/0.06）。纯黑是"这个像素没人写过"的标记：GBufferA 解
                    // 码出来是 (-1,-1,-1)，既不是法线也不可能被当成清屏色混过去。
                    RHIClearValue injected_clears[kInjectedClearCount] = {};
                    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
                    {
                        injected_clears[i].color.float32[3] = 1.0f;  // 纯黑 + alpha 1
                    }
                    injected_clears[kNaniteRasterColorAttachmentCount].depthStencil.depth = 1.0f;

                    RHIRenderPassBeginInfo injected_begin {};
                    injected_begin.sType              = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                    injected_begin.renderPass          = caller.render_pass;
                    injected_begin.framebuffer         = caller.framebuffer;
                    injected_begin.renderArea.extent.width  = kCallerSize;
                    injected_begin.renderArea.extent.height = kCallerSize;
                    injected_begin.clearValueCount     = kInjectedClearCount;
                    injected_begin.pClearValues        = injected_clears;
                    rhi->CmdBeginRenderPassPFN(injected_cb, &injected_begin, RHI_SUBPASS_CONTENTS_INLINE);

                    // 视口只覆盖左上角 16×16。Nanite 不许碰它 —— 它要是自己开了 pass
                    // 就会设一个全尺寸视口，覆盖立刻铺满 32×32，下面的边界断言会红。
                    RHIViewport injected_viewport {};
                    injected_viewport.width    = static_cast<float>(kViewportSize);
                    injected_viewport.height   = static_cast<float>(kViewportSize);
                    injected_viewport.minDepth = 0.0f;
                    injected_viewport.maxDepth = 1.0f;
                    rhi->CmdSetViewportPFN(injected_cb, 0, 1, &injected_viewport);

                    RHIRect2D injected_scissor {};
                    injected_scissor.extent.width  = kViewportSize;
                    injected_scissor.extent.height = kViewportSize;
                    rhi->CmdSetScissorPFN(injected_cb, 0, 1, &injected_scissor);

                    renderer.DrawClustersIndirectInPass(injected_cb);

                    const NaniteGPUResources& injected = renderer.GetGPUResources();
                    Check(injected.raster_ready,
                          "the raster pipeline was rebuilt against the injected pass");
                    Check(injected.raster_framebuffer == nullptr,
                          "Nanite does not take the injected framebuffer (so it cannot begin/end the pass itself)");

                    rhi->CmdEndRenderPassPFN(injected_cb);
                    rhi->EndSingleTimeCommands(injected_cb);

                    // 内容判据：注入画出来的必须是**真的 GBuffer**，不是"有像素"。
                    // 只写第一个附件的实现在这里会挂 —— GBufferC 仍停在纯黑上。
                    const GBufferProbe injected_probe =
                        ProbeGBuffer(rhi, caller.gbuffer, kCallerSize, kCallerSize);
                    Check(injected_probe.covered > 0,
                          "Nanite's triangles landed in the caller's own render pass");
                    Check(injected_probe.normal_valid,
                          "the caller's GBufferA carries a +Y world normal (Nanite's frag writes GBuffer semantics, not a debug colour)");
                    Check(injected_probe.shading_model_valid,
                          "the caller's GBufferB.a decodes to DEFAULT_LIT, so the deferred pass can light it");
                    Check(injected_probe.base_color_valid,
                          "the caller's GBufferC was written too -- all three attachments, not just the first");
                    Check(injected_probe.clear_intact,
                          "the pixels Nanite never touched are still the caller's pure-black clear (Nanite did not re-clear the pass)");

                    // 边界判据：调用方的视口只给了左上角 16×16。Nanite 不许碰它 ——
                    // 它要是自己开了 pass 就会设一个全尺寸视口，覆盖立刻铺满 32×32。
                    const CoveredBounds injected_bounds =
                        CoveredPixelBounds(rhi, caller.gbuffer[0], kCallerSize, kCallerSize);
                    Check(injected_bounds.max_x < kViewportSize && injected_bounds.max_y < kViewportSize,
                          "every covered pixel is inside the caller's 16x16 viewport (Nanite never opened the pass)");
                    // 光看包围盒还不够：调用方的 scissor 也是 16×16，会顺手把"被放大
                    // 的几何"裁掉，包围盒照样在界内。真正的判据是**铺满程度** ——
                    // Nanite 一旦抢设一个更大的 viewport，几何就被放大，16×16 里每一
                    // 个像素都会被盖上（实测 184 → 256）。这条同时钉住视口的尺寸和变换。
                    Check(injected_bounds.count > 0 &&
                              injected_bounds.count < kViewportSize * kViewportSize,
                          "the caller's viewport is only partly covered -- Nanite did not rescale it (a hijacked viewport zooms in and fills every pixel)");
                }

                // 反面：给了 render pass 却不给 framebuffer ⇒ 注入被拒绝，退回离屏。
                // 少一半信息拼不出一次合法的 begin，与其猜，不如明确不接受。
                renderer.SetRasterRenderPass(caller.render_pass, nullptr);
                Check(renderer.GetInjectedRasterRenderPass() == nullptr,
                      "a render pass without a framebuffer is refused, not guessed");

                RHICommandBuffer* half_cb = rhi->BeginSingleTimeCommands();
                if (half_cb != nullptr)
                {
                    RHIClearValue half_clears[kInjectedClearCount] = {};
                    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
                    {
                        half_clears[i].color.float32[3] = 1.0f;
                    }
                    half_clears[kNaniteRasterColorAttachmentCount].depthStencil.depth = 1.0f;

                    RHIRenderPassBeginInfo half_begin {};
                    half_begin.sType              = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                    half_begin.renderPass         = caller.render_pass;
                    half_begin.framebuffer        = caller.framebuffer;
                    half_begin.renderArea.extent.width  = kCallerSize;
                    half_begin.renderArea.extent.height = kCallerSize;
                    half_begin.clearValueCount    = kInjectedClearCount;
                    half_begin.pClearValues       = half_clears;
                    rhi->CmdBeginRenderPassPFN(half_cb, &half_begin, RHI_SUBPASS_CONTENTS_INLINE);

                    renderer.DrawClustersIndirectInPass(half_cb);

                    rhi->CmdEndRenderPassPFN(half_cb);
                    rhi->EndSingleTimeCommands(half_cb);

                    Check(ProbeGBuffer(rhi, caller.gbuffer, kCallerSize, kCallerSize).covered == 0,
                          "with the injection refused, nothing is drawn into the caller's pass");
                }

                renderer.SetRasterRenderPass(nullptr, nullptr);
            }

            DestroyCallerTarget(rhi, caller);
        }
#endif

        renderer.clear();
        Check(renderer.GetGPUResources().clusters == nullptr,
              "clear() releases the GPU buffers instead of leaking them");

        metal_rhi.Shutdown();
    }
#elif defined(__APPLE__)
    std::printf("[skip] nanite_cluster_cull_comp.h was not generated -- GPU dispatch checks skipped\n");
#else
    std::printf("[skip] no Metal on this platform -- GPU dispatch checks skipped\n");
#endif

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
