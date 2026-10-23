#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#include "Runtime/Function/Render/Spine/SpineGpuClipper.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"
#include "Runtime/Function/Render/Spine/SpineGpuBatch.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"
#include "Runtime/Function/Render/Spine/SpineGpuTintBlack.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// 着色器来源（按后端，见 EnsurePipelines）：
//   Vulkan / Metal —— 构建期 SPIR-V（Metal 由 MetalRHI 经 SPIRV-Cross 转 MSL）；
//   DX12          —— 运行时编 HLSL（shader/hlsl/rp2/spine_gpu.*.hlsl，与 RP2 的其它 pass 同一做法）。
#if defined(Z_HAS_SPIRV_SHADERS)
    #include <spine_gpu_frag.h>
    #include <spine_gpu_vert.h>
#endif
#if !defined(__EMSCRIPTEN__)
    #include <stb_image.h>
#endif

namespace
{
    // GpuDraw / GpuInst / GpuCpuVert 与绘制项标志位在 SpineGpuBatch.h（多实例拼包与单测共用）
    using GpuDraw = SpineGpuGpuDraw;
    using GpuInst = SpineGpuGpuInst;
    using GpuCpuVert = SpineGpuCpuVert;

    constexpr uint32_t kSet0Bindings = 9;        // b0..b6 + b7 deform（S6）+ b8 CPU 裁剪顶点（v15）
    constexpr uint32_t kBonesBinding = 4;
    constexpr uint32_t kDrawsBinding = 5;
    constexpr uint32_t kInstsBinding = 6;
    constexpr uint32_t kDeformBinding = 7;
    constexpr uint32_t kCpuVertBinding = 8;
    // 各 SSBO 的元素大小（字节）。Vulkan / Metal 不看；**DX12 必须给**：它把 SSBO 建成结构化 SRV，
    // 没给 stride 时按 range 猜（80 B 或 4 B），b1 / b3（float2）、b5（48 B）、b6（96 B）、b8（16 B）
    // 全会猜错 ⇒ 读到错位的数据且不报错。顺序 = binding 号，与 spine_gpu.vert(.hlsl) 一一对应。
    constexpr uint32_t kSet0Strides[kSet0Bindings] = {4u, 8u, 4u, 8u, 4u, 48u, 96u, 4u, 16u};

    // 骨架空间 y 向上；裁剪空间 y 的方向按后端：Vulkan 向下（不翻视口），DX12 / Metal 向上。
    float ClipSpaceYSign(RHI* rhi) { return rhi->getGraphicsAPI() == GraphicsAPI::Vulkan ? -1.0f : 1.0f; }

    std::string ShaderRoot()
    {
#ifdef ZENGINE_SHADER_ROOT
        return ZENGINE_SHADER_ROOT;
#else
        return "e:/Engine/ZEngine/engine/shader";
#endif
    }

    bool ReadWholeFile(const std::filesystem::path& p, std::vector<uint8_t>& out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f)
        {
            return false;
        }
        f.seekg(0, std::ios::end);
        const std::streamoff n = f.tellg();
        f.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(n));
        if (n > 0)
        {
            f.read(reinterpret_cast<char*>(out.data()), n);
        }
        return static_cast<bool>(f) || n == 0;
    }

    // C++20 起 u8string() 返回 std::u8string；日志 / 错误串统一要 UTF-8 的 std::string
    std::string ToUtf8(const std::filesystem::path& p)
    {
        const std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    std::filesystem::path FromUtf8(const std::string& s)
    {
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
    }

    bool EndsWithLower(const std::string& s, const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        if (s.size() < n)
        {
            return false;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const char c = s[s.size() - n + i];
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(c))) != suffix[i])
            {
                return false;
            }
        }
        return true;
    }

    // 两层：控制台实例画在 UI 层（视口像素空间、无深度）；世界实例（SpineRenderer）画在 RP1 的
    // forward / 透明 subpass（深度测试、不写深度，与透明网格 / 粒子同一站）。两层在同一帧的不同时刻录制
    // ⇒ 每帧缓冲 / 合包 / 管线**按层各一份**：共用的话后录的那层会覆盖先录那层已经引用的数据。
    constexpr int kLayerOverlay = 0;
    constexpr int kLayerWorld = 1;
    constexpr int kLayerCount = 2;

    // 世界实例的 b6：clip_from_world = proj_view · model，root = 单位阵 —— 与 mesh.vert 的
    // `proj_view_matrix * model` 完全同一个乘法，深度才能和场景对上（同一次 CmdSetViewport、同一个投影，
    // RenderCamera 的投影自带 Vulkan 的 y 翻转，各后端网格都是这么画的，这里不再另翻）。
    // 两个输入都是 Matrix4x4 的 m_Mat（行主序、列向量约定：v' = M · v）；着色器的 mat4 是列主序 ⇒ 写出时转置。
    GpuInst WorldInstance(const float proj_view[16], const float local_to_world[16])
    {
        float pvm[16];
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    s += proj_view[r * 4 + k] * local_to_world[k * 4 + c];
                }
                pvm[r * 4 + c] = s;
            }
        }
        GpuInst inst {};
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                inst.clip_from_world[c * 4 + r] = pvm[r * 4 + c];
            }
        }
        inst.root_abcd[0] = 1.0f;
        inst.root_abcd[3] = 1.0f;
        return inst;
    }
}  // namespace

#if !defined(__EMSCRIPTEN__)

struct SpineGpuRenderer::Impl
{
    // ---- 游戏线程 → 渲染线程：**一条**有序命令队列 ----
    // 实例的增删 / 摆放与多轨播放命令（S5）都进同一个队列，渲染线程在下一次 Draw 里按提交顺序执行
    // ⇒ "spine.add 之后立刻 spine.play" 作用在刚加的那个实例上。目标实例在**入队时**解析成 id
    // （游戏线程的"当前选中"），渲染线程不看选中状态。
    mutable std::mutex mutex;
    struct Command
    {
        enum class Op
        {
            kAddInstance,      // path + anim → 新实例 `target`
            kRemoveInstance,   // 删 `target`
            kClearInstances,   // 删全部
            kPlace,            // `target` 手动摆放：(value, delay) = 像素偏移，scale
            kPlaceAuto,        // `target` 回到自动网格
            kSet,              // setAnimation(track, anim, loop)
            kAdd,              // addAnimation(track, anim, loop, delay)
            kEmpty,            // setEmptyAnimation(track, mix)
            kMix,              // 所有实例的 AnimationStateData::default_mix = value（全局，跨实例 / 资产保留）
            kSpeed,            // `target` 的 AnimationState::time_scale = value
            kAlpha,            // `target` 轨道当前那条的 alpha = value
            kSkin,             // `target` 换肤：anim = 皮肤名 / 下标 / "none"；loop = 之后 setSlotsToSetupPose
        };
        Op op {Op::kSet};
        uint32_t target {0};   // 实例 id（入队时解析；0 = 当时没有选中的实例）
        int track {0};
        std::string anim;      // 下标或名字（kAddInstance：放到轨道 0 的动画下标）
        std::string path;      // kAddInstance
        bool loop {true};
        float value {0.0f};
        float delay {0.0f};
        float scale {1.0f};
        bool world {false};    // kAddInstance：世界实例（SpineRenderer 组件）
    };
    std::vector<Command> pending_cmds;
    // 游戏线程视角的实例 id：入队即生效（加载失败时渲染线程会把它摘掉）
    uint32_t next_id {1};
    uint32_t selected {0};
    std::vector<uint32_t> live_ids;

    // 游戏线程入队一条"作用在当前选中实例上"的命令（调用方已持锁）。0 ⇒ 渲染线程找不到实例，丢弃并记日志
    void PushTargeted(Command c)
    {
        c.target = selected;
        pending_cmds.push_back(std::move(c));
    }

    // 世界实例的游戏线程侧状态（受 mutex 保护）。变换 / dt 不走命令队列："最新的赢"，每帧一份，
    // 组件每帧都推也不会把队列撑大。dt 累加到下一次 Draw 才消费（渲染跟不上逻辑时不丢时间）。
    struct WorldEntry
    {
        uint64_t owner {0};
        float model[16] {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float pending_dt {0.0f};
        bool visible {true};
        bool failed {false};   // 加载失败：仍然"认识"这个 id（组件不重建），原因在 status
        uint64_t stamp {0};    // 最近一次推变换时的 world_serial
        SpineGpuBillboard billboard;
    };
    std::unordered_map<uint32_t, WorldEntry> world;
    // 逻辑帧序号（WorldManager::Tick 每帧 +1）。世界实例只有在"上一个或这一个逻辑帧里被组件推过"时才画：
    // 组件 disabled / GameObject 非激活 / 对象已不在任何关卡里 ⇒ 不 tick ⇒ 自动隐藏；暂停时逻辑帧不前进
    // ⇒ 照常显示（定格）。这比靠析构可靠：本引擎删 GameObject 时组件对象不一定析构（撤销要留着它）。
    uint64_t world_serial {0};

    void PushTo(uint32_t id, Command c)
    {
        c.target = id;
        pending_cmds.push_back(std::move(c));
    }

    float default_mix {0.2f};   // 渲染线程独占（spine.mix 经命令队列改）；跨资产保留
    std::string status {"idle"};
    // 上一帧每层的统计（渲染线程写、spine.status 读，受 mutex 保护）
    struct LayerStats
    {
        std::string skip_reason;   // 最近一次"整帧没画"的原因（变化时才记日志）
        uint32_t draw_count {0};
        uint32_t deform_draws {0};    // 带 deform 的绘制项数（S6）
        uint32_t deform_floats {0};   // 上传的 deform float 数
        uint32_t clipped_draws {0};   // CPU 裁剪过的项（v15）
        uint32_t clip_vertices {0};   // 裁剪输出的顶点数
        uint32_t tint_black_draws {0};   // 带暗色项的绘制项数
        std::string summary;          // 每个实例的概况
    };
    LayerStats stats[kLayerCount];
    // tint black 开关（= SpineSettings::tintBlack）。默认关：OSGame 线上全部是 0，且引擎分支
    // 根本不上传 uv2/uv3（见 SpineGpuTintBlack.h）。游戏线程写、渲染线程读，受 mutex 保护。
    bool tint_black {false};
    bool tint_black_logged {false};   // 每次加实例 / 每次拨开关后只记一行"tint black active"

    // ---- 管线（与资产无关）----
    RHI* rhi {nullptr};
    RHIRenderPass* pipeline_render_pass[kLayerCount] {nullptr, nullptr};
    uint32_t pipeline_subpass[kLayerCount] {0, 0};
    RHIDescriptorSetLayout* set0_layout {nullptr};
    RHIDescriptorSetLayout* set1_layout {nullptr};   // 图集页：binding 0 = combined image sampler（片元）
    RHIPipelineLayout* pipeline_layout {nullptr};
    RHIPipeline* pipelines[kLayerCount][4] {};   // [层][SpineBlendMode]；世界层开深度测试

    // ---- 资产（同一路径的所有实例共用一份）----
    struct Buffer
    {
        RHIBuffer* buffer {nullptr};
        RHIDeviceMemory* memory {nullptr};
        RHIDeviceSize size {0};
    };
    struct FrameSlot
    {
        // 该资产**所有实例**本帧的拼包（SpineGpuBatch）。容量按需倍增（旧缓冲延迟销毁，只重写**本帧
        // 槽位**的那一格 —— 它上一次被 GPU 用是 frames.size() 帧之前，已经完成）。
        Buffer bones;
        Buffer draws;
        Buffer insts;
        Buffer deform;      // 本帧生效的 mesh deform（S6）
        Buffer cpu_verts;   // v15 CPU 裁剪的输出：GpuCpuVert[]
        Buffer cpu_index;   // u16[]，局部于各项的起点
        RHIDescriptorSet* set0 {nullptr};
        std::vector<RHIDescriptorSet*> set1;   // 每个图集页一个（每帧槽位一份：DX12 每帧重写，见 RefreshDescriptors）
    };
    // 图集页贴图：自己建、自己销毁（不经 UIGpuResources —— 它的描述符集只写一次，
    // 而 DX12 的 SRV 落在每帧回收的堆分区里，第二轮就成了野句柄）
    struct Page
    {
        RHIImage* image {nullptr};
        RHIImageView* view {nullptr};
        RHISampler* sampler {nullptr};   // 默认线性采样器，归 RHI 所有
    };
    struct Asset
    {
        std::string path;
        SpineGpuAsset asset;
        SpineGpuRuntimeData data;          // 查找表：只读，所有实例共用
        SpineGpuBatchAssetInfo info;       // deform 长度 / infl_base / 单实例 deform 上界
        Buffer vertex_words;
        Buffer infl_bind;
        Buffer infl_bone;
        Buffer uvs;
        Buffer index;
        // DX12 专用：u32 0,1,2,…（per-instance 顶点属性 = 绘制项在 b5 里的下标；D3D12 的 SV_InstanceID 不含
        // StartInstanceLocation，见 spine_gpu.vert.hlsl 文件头）。多实例时按合包的项数倍增。其它后端为空。
        Buffer draw_ids;
        uint32_t draw_ids_count {0};
        std::vector<Page> pages;
        std::vector<FrameSlot> frames[kLayerCount];   // [层][帧槽位]
        // 每个 (动画, 皮肤) t=0 的包围盒（x0, y0, x1, y1），自动排布用；第一次用到时算。
        // 下标 = anim · (皮肤数 + 1) + 皮肤（无皮肤 = 皮肤数）
        std::vector<std::array<float, 4>> bounds;
        std::vector<uint8_t> bounds_valid;
        SpineGpuBatch batch[kLayerCount];   // 本帧拼包（按层；渲染线程暂存，跨帧复用）
    };
    std::vector<std::shared_ptr<Asset>> assets;

    // ---- 实例 ----
    struct Instance
    {
        uint32_t id {0};
        std::shared_ptr<Asset> asset;   // 先声明 ⇒ 后析构：anim_state 引用 asset->asset / asset->data
        uint32_t anim {0};              // 加载时放到轨道 0 的动画
        // S5：跨帧存活的骨架状态 + 多轨调度。`anim_state` 引用 asset / data / mix ⇒ 必须声明在它们之后。
        SpineGpuSkeletonState state;
        SpineGpuAnimationStateData mix;
        std::unique_ptr<SpineGpuAnimationState> anim_state;
        std::chrono::steady_clock::time_point last_tick;
        float bounds[4] {0, 0, 1, 1};
        uint32_t skin {kSpineGpuNoSkin};   // 只给 status / 包围盒用；真正的当前皮肤在 state.skin
        // 世界实例（SpineRenderer 组件）：摆放 = 相机 · model；dt 由组件喂（见 WorldEntry）
        bool world {false};
        bool world_visible {true};
        float world_model[16] {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float world_dt {0.0f};
        SpineGpuBillboard world_billboard;
        float world_model_used[16] {};   // 本帧实际用的 model（公告板时已换掉旋转）
        float world_view_z {0.0f};   // 本帧枢轴点的观察空间 z（排序用：越负越远）
        bool world_logged {false};
        int world_shown_logged {-1};   // 上次记日志时的可见性（-1 = 还没记过）：变化时记一行   // 第一次画出来时记一行包围盒在 NDC 里的位置（排查"画了但看不见"）
        // 摆放：自动 = 视口网格里按实例顺序占一格；手动 = 视口中心偏 (x, y) 像素、scale 像素 / 单位
        bool manual {false};
        float place_x {0.0f};
        float place_y {0.0f};
        float place_scale {1.0f};
        // 本帧解算结果（跨帧复用内存）
        SpineGpuPoseResult pose;
        SpineGpuSkeletonClipping clipper;
        SpineGpuClipFrameResult clip;
        bool solved {false};
        SpineGpuBatchRange range;
        std::string error;   // 最近一次"这个实例没画"的原因（变化时记日志）
    };
    std::vector<std::unique_ptr<Instance>> instances;   // 也是**绘制顺序**（先加的先画、在下面）

    // 延迟销毁：GPU 可能还在用（最多领先 MaxFramesInFlight 帧）。描述符集到期后进空闲表复用
    // （RHI 没有 FreeDescriptorSets；不复用的话每加 / 删一次资产就漏一组）。
    struct Retired
    {
        std::vector<Buffer> buffers;
        std::vector<Page> pages;
        std::vector<RHIDescriptorSet*> set0s;
        std::vector<RHIDescriptorSet*> set1s;
        uint32_t frames_left {0};
    };
    std::vector<Retired> retired;
    std::vector<RHIDescriptorSet*> free_set0;
    std::vector<RHIDescriptorSet*> free_set1;

    // ------------------------------------------------------------------

    bool CreateBuffer(Buffer& b, RHIDeviceSize size, RHIBufferUsageFlags usage, const void* init)
    {
        b.size = std::max<RHIDeviceSize>(size, 4);
        rhi->CreateBuffer(b.size, usage, RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          b.buffer, b.memory);
        if (b.buffer == nullptr || b.memory == nullptr)
        {
            return false;
        }
        if (init != nullptr && size > 0)
        {
            void* dst = nullptr;
            if (!rhi->MapMemory(b.memory, 0, size, 0, &dst) || dst == nullptr)
            {
                return false;
            }
            std::memcpy(dst, init, static_cast<size_t>(size));
            rhi->UnmapMemory(b.memory);
        }
        return true;
    }

    bool Upload(const Buffer& b, const void* src, size_t bytes)
    {
        if (bytes == 0)
        {
            return true;
        }
        if (bytes > b.size)
        {
            return false;
        }
        void* dst = nullptr;
        if (!rhi->MapMemory(b.memory, 0, b.size, 0, &dst) || dst == nullptr)
        {
            return false;
        }
        std::memcpy(dst, src, bytes);
        rhi->UnmapMemory(b.memory);
        return true;
    }

    void DestroyBufferNow(Buffer& b)
    {
        if (b.buffer != nullptr)
        {
            rhi->DestroyBuffer(b.buffer);
        }
        if (b.memory != nullptr)
        {
            rhi->FreeMemory(b.memory);
        }
        b = Buffer {};
    }

    bool IsDx12() const { return rhi != nullptr && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12; }

    uint32_t RetireFrames() const { return static_cast<uint32_t>(rhi->GetMaxFramesInFlight()) + 1u; }

    void RetireBuffer(Buffer& b)
    {
        if (b.buffer == nullptr && b.memory == nullptr)
        {
            return;
        }
        Retired r;
        r.frames_left = RetireFrames();
        r.buffers.push_back(b);
        retired.push_back(std::move(r));
        b = Buffer {};
    }

    // 先从空闲表拿（到期的旧描述符集），没有再向共享池要
    bool AllocSet(RHIDescriptorSetLayout* layout, std::vector<RHIDescriptorSet*>& free_list, RHIDescriptorSet*& out)
    {
        if (!free_list.empty())
        {
            out = free_list.back();
            free_list.pop_back();
            return true;
        }
        RHIDescriptorSetAllocateInfo alloc {};
        alloc.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = rhi->GetDescriptorPoor();
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        out = nullptr;
        return rhi->AllocateDescriptorSets(&alloc, out) && out != nullptr;
    }

    // set 0 的一格 / 全部。stride 一律给（DX12 要，其它后端忽略）。
    void WriteSet0Binding(RHIDescriptorSet* set, uint32_t binding, const Buffer& b)
    {
        RHIDescriptorBufferInfo info {};
        info.buffer = b.buffer;
        info.offset = 0;
        info.range = b.size;
        info.stride = kSet0Strides[binding];
        RHIWriteDescriptorSet w {};
        w.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &info;
        rhi->UpdateDescriptorSets(1, &w, 0, nullptr);
    }

    void WriteSet0(Asset& as, FrameSlot& f)
    {
        const Buffer* bufs[kSet0Bindings] = {&as.vertex_words, &as.infl_bind, &as.infl_bone, &as.uvs, &f.bones,
                                             &f.draws,         &f.insts,     &f.deform,    &f.cpu_verts};
        for (uint32_t i = 0; i < kSet0Bindings; ++i)
        {
            WriteSet0Binding(f.set0, i, *bufs[i]);
        }
    }

    void WriteSet1(Asset& as, FrameSlot& f)
    {
        for (size_t i = 0; i < as.pages.size() && i < f.set1.size(); ++i)
        {
            RHIDescriptorImageInfo image {};
            image.sampler = as.pages[i].sampler;
            image.imageView = as.pages[i].view;
            image.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            RHIWriteDescriptorSet w {};
            w.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = f.set1[i];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &image;
            rhi->UpdateDescriptorSets(1, &w, 0, nullptr);
        }
    }

    // DX12：UpdateDescriptorSets 建的 SRV 落在**当前帧**的 CBV/SRV/UAV 堆分区里，那个分区在同一帧号
    // 下一轮开始时整体回收（DX12RHI::AllocateCbvSrvUavDescriptor）⇒ 只写一次的描述符集第二轮就指向
    // 别人的 SRV（读到 0 / 别的缓冲，不报错）。所以 DX12 每帧录制前把本帧槽位的 set0 / set1 全部重写
    // （与 MainCameraRp2Pass::DrawRP2 开头的 UpdateDescriptorBindings 同一个处理）。其它后端写一次即可。
    void RefreshDescriptors(Asset& as, FrameSlot& f)
    {
        if (!IsDx12())
        {
            return;
        }
        WriteSet0(as, f);
        WriteSet1(as, f);
    }

    // 本帧槽位的某个缓冲容量不够 ⇒ 换一个更大的（旧的延迟销毁）。`binding` 非负时重写 set0 的那一格。
    bool EnsureFrameBuffer(FrameSlot& f, Buffer& b, RHIDeviceSize need, RHIBufferUsageFlags usage, int binding)
    {
        if (need <= b.size)
        {
            return true;
        }
        RetireBuffer(b);
        RHIDeviceSize cap = 256;
        while (cap < need)
        {
            cap *= 2;
        }
        if (!CreateBuffer(b, cap, usage, nullptr))
        {
            return false;
        }
        if (binding >= 0)
        {
            WriteSet0Binding(f.set0, static_cast<uint32_t>(binding), b);
        }
        return true;
    }

    // DX12：draw_ids 至少要有 `need` 项（firstInstance 是合包里的全局下标）
    bool EnsureDrawIds(Asset& as, uint32_t need)
    {
        if (!IsDx12() || need <= as.draw_ids_count)
        {
            return true;
        }
        RetireBuffer(as.draw_ids);
        uint32_t cap = 64;
        while (cap < need)
        {
            cap *= 2;
        }
        std::vector<uint32_t> ids(cap);
        for (uint32_t i = 0; i < cap; ++i)
        {
            ids[i] = i;
        }
        as.draw_ids_count = 0;
        if (!CreateBuffer(as.draw_ids, ids.size() * sizeof(uint32_t), RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT, ids.data()))
        {
            return false;
        }
        as.draw_ids_count = cap;
        return true;
    }

    void RetireAsset(Asset& as)
    {
        Retired r;
        r.frames_left = RetireFrames();
        for (Buffer* b : {&as.vertex_words, &as.infl_bind, &as.infl_bone, &as.uvs, &as.index, &as.draw_ids})
        {
            r.buffers.push_back(*b);
            *b = Buffer {};
        }
        r.pages = as.pages;
        as.pages.clear();
        for (std::vector<FrameSlot>& layer_frames : as.frames)
        for (FrameSlot& f : layer_frames)
        {
            for (Buffer* b : {&f.bones, &f.draws, &f.insts, &f.deform, &f.cpu_verts, &f.cpu_index})
            {
                r.buffers.push_back(*b);
                *b = Buffer {};
            }
            if (f.set0 != nullptr)
            {
                r.set0s.push_back(f.set0);
            }
            for (RHIDescriptorSet* s1 : f.set1)
            {
                if (s1 != nullptr)
                {
                    r.set1s.push_back(s1);
                }
            }
            f.set0 = nullptr;
            f.set1.clear();
        }
        retired.push_back(std::move(r));
    }

    // 没有实例再引用的资产 ⇒ 延迟销毁
    void PurgeUnusedAssets()
    {
        for (std::shared_ptr<Asset>& as : assets)
        {
            if (as.use_count() == 1)
            {
                RetireAsset(*as);
                as.reset();
            }
        }
        assets.erase(std::remove(assets.begin(), assets.end(), nullptr), assets.end());
    }

    void TickRetired()
    {
        for (Retired& r : retired)
        {
            if (r.frames_left > 0)
            {
                --r.frames_left;
            }
        }
        for (Retired& r : retired)
        {
            if (r.frames_left == 0)
            {
                for (Buffer& b : r.buffers)
                {
                    DestroyBufferNow(b);
                }
                r.buffers.clear();
                for (Page& pg : r.pages)
                {
                    if (pg.view != nullptr)
                    {
                        rhi->DestroyImageView(pg.view);
                    }
                    if (pg.image != nullptr)
                    {
                        rhi->DestroyImage(pg.image);
                    }
                }
                r.pages.clear();
                free_set0.insert(free_set0.end(), r.set0s.begin(), r.set0s.end());
                free_set1.insert(free_set1.end(), r.set1s.begin(), r.set1s.end());
                r.set0s.clear();
                r.set1s.clear();
            }
        }
        retired.erase(std::remove_if(retired.begin(), retired.end(),
                                     [](const Retired& r) {
                                         return r.buffers.empty() && r.pages.empty() && r.set0s.empty() &&
                                                r.set1s.empty();
                                     }),
                      retired.end());
    }

    // ------------------------------------------------------------------
    // 管线：set 0 = 9 个 SSBO，set 1 = 图集页（combined image sampler）；4 种混合各一条
    // ------------------------------------------------------------------
    // 描述符布局 + 管线布局（与层 / render pass 无关；加载资产要它们来分配描述符集）
    bool EnsureLayouts()
    {
        if (set0_layout == nullptr)
        {
            RHIDescriptorSetLayoutBinding bindings[kSet0Bindings] {};
            for (uint32_t i = 0; i < kSet0Bindings; ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
            }
            // draws 片元里也读（tint / flags）
            bindings[5].stageFlags = RHI_SHADER_STAGE_VERTEX_BIT | RHI_SHADER_STAGE_FRAGMENT_BIT;
            RHIDescriptorSetLayoutCreateInfo info {};
            info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = kSet0Bindings;
            info.pBindings = bindings;
            if (!rhi->CreateDescriptorSetLayout(&info, set0_layout) || set0_layout == nullptr)
            {
                LOG_ERROR(ZRender, "SpineGpuRenderer: CreateDescriptorSetLayout failed");
                return false;
            }
        }
        if (set1_layout == nullptr)
        {
            RHIDescriptorSetLayoutBinding binding {};
            binding.binding = 0;
            binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
            RHIDescriptorSetLayoutCreateInfo info {};
            info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = 1;
            info.pBindings = &binding;
            if (!rhi->CreateDescriptorSetLayout(&info, set1_layout) || set1_layout == nullptr)
            {
                LOG_ERROR(ZRender, "SpineGpuRenderer: CreateDescriptorSetLayout (atlas page) failed");
                return false;
            }
        }
        if (pipeline_layout == nullptr)
        {
            RHIDescriptorSetLayout* layouts[2] = {set0_layout, set1_layout};
            RHIPipelineLayoutCreateInfo info {};
            info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.setLayoutCount = 2;
            info.pSetLayouts = layouts;
            if (!rhi->CreatePipelineLayout(&info, pipeline_layout) || pipeline_layout == nullptr)
            {
                LOG_ERROR(ZRender, "SpineGpuRenderer: CreatePipelineLayout failed");
                return false;
            }
        }
        return true;
    }

    bool EnsurePipelines(int layer, RHIRenderPass* render_pass, uint32_t subpass)
    {
        if (pipelines[layer][0] != nullptr && pipeline_render_pass[layer] == render_pass &&
            pipeline_subpass[layer] == subpass)
        {
            return true;
        }
        if (!EnsureLayouts())
        {
            return false;
        }

        RHIShader* vs = nullptr;
        RHIShader* fs = nullptr;
        const bool dx12 = IsDx12();
        if (dx12)
        {
            std::vector<uint8_t> binary;
            vs = rhi->CreateShaderModuleFromFile(ShaderRoot() + "/hlsl/rp2/spine_gpu.vert.hlsl", ShaderStage::Vertex, {},
                                                 {}, binary);
            binary.clear();
            fs = rhi->CreateShaderModuleFromFile(ShaderRoot() + "/hlsl/rp2/spine_gpu.frag.hlsl", ShaderStage::Fragment,
                                                 {}, {}, binary);
        }
        else
        {
#if defined(Z_HAS_SPIRV_SHADERS)
            vs = rhi->CreateShaderModule(Z::Shader::SPINE_GPU_VERT);
            fs = rhi->CreateShaderModule(Z::Shader::SPINE_GPU_FRAG);
#endif
        }
        if (vs == nullptr || fs == nullptr)
        {
            LOG_ERROR(ZRender, "SpineGpuRenderer: CreateShaderModule failed ({})",
                      dx12 ? "runtime HLSL: shader/hlsl/rp2/spine_gpu.*.hlsl" : "build-time SPIR-V");
            if (vs != nullptr)
            {
                rhi->DestroyShaderModule(vs);
            }
            if (fs != nullptr)
            {
                rhi->DestroyShaderModule(fs);
            }
            return false;
        }
        RHIPipelineShaderStageCreateInfo stages[2] {};
        stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        // Vulkan / Metal：无顶点流，全部从 SSBO 取，绘制项下标 = gl_InstanceIndex（含 firstInstance；
        // Metal 的 [[instance_id]] 同样含 baseInstance）。
        // DX12：一条 per-instance 的 u32 属性（location 3 ⇒ TEXCOORD0，DX12RHI 的语义映射）。
        RHIPipelineVertexInputStateCreateInfo vertex_input {};
        vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        RHIVertexInputBindingDescription id_binding {};
        id_binding.binding = 0;
        id_binding.stride = sizeof(uint32_t);
        id_binding.inputRate = RHI_VERTEX_INPUT_RATE_INSTANCE;
        RHIVertexInputAttributeDescription id_attr {};
        id_attr.location = 3;
        id_attr.binding = 0;
        id_attr.format = RHI_FORMAT_R32_UINT;
        id_attr.offset = 0;
        if (dx12)
        {
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &id_binding;
            vertex_input.vertexAttributeDescriptionCount = 1;
            vertex_input.pVertexAttributeDescriptions = &id_attr;
        }
        RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
        input_assembly.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        RHIPipelineViewportStateCreateInfo viewport_state {};
        viewport_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        RHIPipelineRasterizationStateCreateInfo raster {};
        raster.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = RHI_POLYGON_MODE_FILL;
        raster.lineWidth = 1.0f;
        raster.cullMode = RHI_CULL_MODE_NONE;   // Spine 网格可以镜像（负缩放），不剔除
        raster.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        RHIPipelineMultisampleStateCreateInfo multisample {};
        multisample.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;
        RHIPipelineDepthStencilStateCreateInfo depth {};
        depth.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        if (layer == kLayerWorld)
        {
            // 透明物体的标准做法：测深度（被场景挡住）、不写深度（自身各项按绘制顺序叠，不互相挡）
            depth.depthTestEnable = RHI_TRUE;
            depth.depthWriteEnable = RHI_FALSE;
            depth.depthCompareOp = RHI_COMPARE_OP_LESS_OR_EQUAL;
        }
        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state {};
        dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates = dynamic_states;

        bool ok = true;
        for (uint32_t mode = 0; mode < 4 && ok; ++mode)
        {
            // §8：输出是预乘 alpha。additive 用与 normal 同一组因子、靠片元 alpha = 0 实现
            // （UI 层随后按 `ui.rgb + scene * (1 - ui.a)` 合成，正好是"加上去"）。
            RHIPipelineColorBlendAttachmentState blend {};
            blend.colorWriteMask =
                RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
            blend.blendEnable = RHI_TRUE;
            blend.colorBlendOp = RHI_BLEND_OP_ADD;
            blend.alphaBlendOp = RHI_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            switch (static_cast<SpineBlendMode>(mode))
            {
                case SpineBlendMode::kMultiply:
                    blend.srcColorBlendFactor = RHI_BLEND_FACTOR_DST_COLOR;
                    blend.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
                case SpineBlendMode::kScreen:
                    blend.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
                    blend.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                    break;
                default:   // normal / additive
                    blend.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
                    blend.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
            }
            RHIPipelineColorBlendStateCreateInfo color_blend {};
            color_blend.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            color_blend.attachmentCount = 1;
            color_blend.pAttachments = &blend;

            RHIGraphicsPipelineCreateInfo pi {};
            pi.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pi.stageCount = 2;
            pi.pStages = stages;
            pi.pVertexInputState = &vertex_input;
            pi.pInputAssemblyState = &input_assembly;
            pi.pViewportState = &viewport_state;
            pi.pRasterizationState = &raster;
            pi.pMultisampleState = &multisample;
            pi.pColorBlendState = &color_blend;
            pi.pDepthStencilState = &depth;
            pi.pDynamicState = &dynamic_state;
            pi.layout = pipeline_layout;
            pi.renderPass = render_pass;
            pi.subpass = subpass;
            pi.basePipelineHandle = RHI_NULL_HANDLE;
            pipelines[layer][mode] = nullptr;
            ok = rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pi, pipelines[layer][mode]) &&
                 pipelines[layer][mode] != nullptr;
        }
        rhi->DestroyShaderModule(vs);
        rhi->DestroyShaderModule(fs);
        if (!ok)
        {
            LOG_ERROR(ZRender, "SpineGpuRenderer: CreateGraphicsPipelines failed ({} layer)",
                      layer == kLayerWorld ? "world" : "overlay");
            return false;
        }
        pipeline_render_pass[layer] = render_pass;
        pipeline_subpass[layer] = subpass;
        return true;
    }

    // ------------------------------------------------------------------
    // 命令（渲染线程）
    // ------------------------------------------------------------------
    // 动画按下标（纯数字）或名字解析；找不到 ⇒ 返回 false，命令丢弃并记日志（不猜）。
    static bool ResolveAnim(const SpineGpuAsset& a, const std::string& s, uint32_t& out)
    {
        if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos)
        {
            const unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
            if (v < a.anims.size())
            {
                out = static_cast<uint32_t>(v);
                return true;
            }
            return false;
        }
        for (size_t i = 0; i < a.anim_names.size(); ++i)
        {
            if (a.anim_names[i] == s)
            {
                out = static_cast<uint32_t>(i);
                return true;
            }
        }
        return false;
    }

    Instance* FindInstance(uint32_t id)
    {
        for (std::unique_ptr<Instance>& in : instances)
        {
            if (in->id == id)
            {
                return in.get();
            }
        }
        return nullptr;
    }

    // 渲染线程删掉 / 没能建出来的实例：同步给游戏线程视角（选中的话改选最后一个）
    void ForgetId(uint32_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto w = world.find(id);
        if (w != world.end())
        {
            w->second.failed = true;
        }
        live_ids.erase(std::remove(live_ids.begin(), live_ids.end(), id), live_ids.end());
        if (selected == id)
        {
            selected = live_ids.empty() ? 0u : live_ids.back();
        }
    }

    // 资产 t=0 的包围盒（每个 (动画, 皮肤) 算一次）：CPU 按 §6 同一公式蒙一遍，只为自动排布。
    // 收 default 皮肤 + 当前皮肤的全部可画项（不只是 t=0 可见的：动画里会显隐，框按"可能出现的"取）。
    bool AssetBounds(Asset& as, uint32_t anim, uint32_t skin, float out[4], std::string& err)
    {
        const SpineGpuAsset& a = as.asset;
        const size_t skins = a.skin_names.size();
        const size_t stride = skins + 1u;
        if (as.bounds.size() != a.anims.size() * stride)
        {
            as.bounds.assign(a.anims.size() * stride, {0, 0, 1, 1});
            as.bounds_valid.assign(a.anims.size() * stride, 0u);
        }
        const size_t bi = static_cast<size_t>(anim) * stride + (skin == kSpineGpuNoSkin ? skins : skin);
        if (bi >= as.bounds.size())
        {
            err = "包围盒下标越界（皮肤 / 动画下标不对）";
            return false;
        }
        if (as.bounds_valid[bi] != 0u)
        {
            std::memcpy(out, as.bounds[bi].data(), sizeof(float) * 4u);
            return true;
        }
        SpineGpuPoseResult pose;
        const SpineImportResult pr = SpineGpuSamplePoseSkinned(a, skin, anim, 0.0f, pose);
        if (!pr.ok)
        {
            err = pr.error;
            return false;
        }
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        const auto grow = [&](float x, float y) {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        };
        const auto word = [&](size_t i) {
            uint32_t v = 0;
            std::memcpy(&v, a.vertex_buffer.data() + i * 4u, 4);
            return v;
        };
        for (const SpineDrawItem& di : a.draw_items)
        {
            if (!((as.data.has_default_skin && di.skin_id == SpineGpuConstants::kDefaultSkinId) ||
                  (skin != kSpineGpuNoSkin && di.skin_id == skin)))
            {
                continue;
            }
            for (uint32_t q = 0; q < di.index_count; ++q)
            {
                const uint32_t vi = a.index_buffer[di.index_offset + q];
                if (di.vertex_format == SpineVertexFormat::kRigid)
                {
                    float bx = 0.0f, by = 0.0f;
                    std::memcpy(&bx, a.vertex_buffer.data() + vi * 8u, 4);
                    std::memcpy(&by, a.vertex_buffer.data() + vi * 8u + 4u, 4);
                    const SpineGpuBoneSample& b = pose.bones[a.slot_bone[di.slot_index]];
                    grow(bx * b.a + by * b.b + b.world_x, bx * b.c + by * b.d + b.world_y);
                }
                else
                {
                    const size_t w0 = a.weighted_segment_offset / 4u + vi * 2u;
                    const uint32_t off = word(w0);
                    const uint32_t cnt = word(w0 + 1u);
                    float x = 0.0f, y = 0.0f;
                    for (uint32_t k = 0; k < cnt; ++k)
                    {
                        const float bx = a.influence_bind[(off + k) * 2u];
                        const float by = a.influence_bind[(off + k) * 2u + 1u];
                        const SpineInfluenceBone& ib = a.influence_bone[off + k];
                        const SpineGpuBoneSample& b = pose.bones[ib.bone_idx];
                        const float wgt = static_cast<float>(ib.weight) / 255.0f;
                        x += (bx * b.a + by * b.b + b.world_x) * wgt;
                        y += (bx * b.c + by * b.d + b.world_y) * wgt;
                    }
                    grow(x, y);
                }
            }
        }
        if (x0 > x1)
        {
            x0 = -100.0f;
            y0 = -100.0f;
            x1 = 100.0f;
            y1 = 100.0f;
        }
        as.bounds[bi] = {x0, y0, x1, y1};
        as.bounds_valid[bi] = 1u;
        std::memcpy(out, as.bounds[bi].data(), sizeof(float) * 4u);
        return true;
    }

    // 同一路径的资产只加载一次（文件改了要 spine.clear 再加：缓存按路径、不看时间戳）
    std::shared_ptr<Asset> GetOrLoadAsset(const std::string& path, std::string& err)
    {
        for (const std::shared_ptr<Asset>& as : assets)
        {
            if (as->path == path)
            {
                return as;
            }
        }
        std::shared_ptr<Asset> as = LoadAsset(path, err);
        if (as)
        {
            assets.push_back(as);
        }
        return as;
    }

    bool CreateInstance(uint32_t id, const std::string& path, const std::string& anim_s, bool loop, bool world_inst,
                        std::string& err)
    {
        std::shared_ptr<Asset> as = GetOrLoadAsset(path, err);
        if (!as)
        {
            return false;
        }
        const SpineGpuAsset& a = as->asset;
        auto in = std::make_unique<Instance>();
        in->id = id;
        in->asset = as;
        uint32_t ai = 0;
        if (!anim_s.empty() && !ResolveAnim(a, anim_s, ai))
        {
            // 加载用的下标越界时与多实例之前一致：夹到最后一个（spine.load 的老语义）
            const long v = std::strtol(anim_s.c_str(), nullptr, 10);
            ai = static_cast<uint32_t>(std::clamp<long>(v, 0, static_cast<long>(a.anims.size()) - 1));
            if (world_inst)
            {
                LOG_WARNING(ZRender, "SpineGpuRenderer: instance #{}: animation '{}' not found in '{}', playing #{} '{}'",
                            id, anim_s, path, ai, a.anim_names[ai]);
            }
        }
        in->world = world_inst;
        in->anim = ai;
        if (!AssetBounds(*as, ai, kSpineGpuNoSkin, in->bounds, err))
        {
            return false;
        }
        const SpineImportResult rr = SpineGpuInitSkeletonState(a, as->data, in->state);
        if (!rr.ok)
        {
            err = rr.error;
            return false;
        }
        in->mix.default_mix = default_mix;
        in->anim_state = std::make_unique<SpineGpuAnimationState>(as->asset, as->data, &in->mix);
        in->anim_state->SetAnimation(0, ai, loop);
        in->last_tick = std::chrono::steady_clock::now();
        instances.push_back(std::move(in));
        return true;
    }

    void RunCommands(std::vector<Command>& cmds)
    {
        for (const Command& c : cmds)
        {
            switch (c.op)
            {
                case Command::Op::kAddInstance:
                {
                    std::string err;
                    const bool ok = EnsureLayouts() && CreateInstance(c.target, c.path, c.anim, c.loop, c.world, err);
                    if (ok)
                    {
                        const Instance& in = *instances.back();
                        LOG_INFO(ZRender,
                                 "SpineGpuRenderer: instance #{} '{}' anim #{} '{}' ({} bones, {} draw items; {} "
                                 "instances / {} assets)",
                                 in.id, c.path, in.anim, in.asset->asset.anim_names[in.anim],
                                 in.asset->asset.header.bone_count, in.asset->asset.draw_items.size(), instances.size(),
                                 assets.size());
                        std::lock_guard<std::mutex> lock(mutex);
                        tint_black_logged = false;
                    }
                    else
                    {
                        if (err.empty())
                        {
                            err = "descriptor layouts are not ready";
                        }
                        LOG_ERROR(ZRender, "SpineGpuRenderer: instance #{} load '{}' failed: {}", c.target, c.path, err);
                        ForgetId(c.target);
                        std::lock_guard<std::mutex> lock(mutex);
                        status = "load failed: " + err;
                    }
                    continue;
                }
                case Command::Op::kRemoveInstance:
                {
                    const size_t before = instances.size();
                    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                                   [&](const std::unique_ptr<Instance>& in) { return in->id == c.target; }),
                                    instances.end());
                    if (instances.size() == before)
                    {
                        LOG_WARNING(ZRender, "SpineGpuRenderer: no instance #{} to remove", c.target);
                    }
                    else
                    {
                        LOG_INFO(ZRender, "SpineGpuRenderer: instance #{} removed ({} left)", c.target, instances.size());
                    }
                    ForgetId(c.target);
                    continue;
                }
                case Command::Op::kClearInstances:
                    // 只清控制台实例：世界实例归场景里的组件管（spine.load / spine.clear 不该把场景清空）
                    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                                   [](const std::unique_ptr<Instance>& in) { return !in->world; }),
                                    instances.end());
                    continue;
                case Command::Op::kMix:
                    default_mix = std::max(0.0f, c.value);
                    for (std::unique_ptr<Instance>& in : instances)
                    {
                        in->mix.default_mix = default_mix;
                    }
                    LOG_INFO(ZRender, "SpineGpuRenderer: default mix = {}s", default_mix);
                    continue;
                default:
                    break;
            }
            Instance* in = FindInstance(c.target);
            if (in == nullptr)
            {
                LOG_WARNING(ZRender, "SpineGpuRenderer: no instance #{} (selected), command dropped", c.target);
                continue;
            }
            SpineGpuAnimationState& st = *in->anim_state;
            const size_t track = static_cast<size_t>(std::max(0, c.track));
            switch (c.op)
            {
                case Command::Op::kSet:
                case Command::Op::kAdd:
                {
                    uint32_t ai = 0;
                    if (!ResolveAnim(in->asset->asset, c.anim, ai))
                    {
                        LOG_WARNING(ZRender, "SpineGpuRenderer: instance #{}: animation '{}' not found ({} animations)",
                                    in->id, c.anim, in->asset->asset.anims.size());
                        break;
                    }
                    if (c.op == Command::Op::kSet)
                    {
                        st.SetAnimation(track, ai, c.loop);
                    }
                    else
                    {
                        st.AddAnimation(track, ai, c.loop, c.delay);
                    }
                    break;
                }
                case Command::Op::kEmpty:
                    st.SetEmptyAnimation(track, std::max(0.0f, c.value));
                    break;
                case Command::Op::kSpeed:
                    st.time_scale = c.value;
                    break;
                case Command::Op::kAlpha:
                    if (SpineGpuTrackEntry* e = st.GetCurrent(track))
                    {
                        e->alpha = std::clamp(c.value, 0.0f, 1.0f);
                    }
                    break;
                case Command::Op::kPlace:
                    in->manual = true;
                    in->place_x = c.value;
                    in->place_y = c.delay;
                    in->place_scale = c.scale;
                    break;
                case Command::Op::kSkin:
                {
                    // "none" / 空 ⇒ setSkin(NULL)；纯数字 ⇒ 皮肤下标；否则按名字（findSkin）
                    const SpineGpuAsset& a = in->asset->asset;
                    uint32_t sk = kSpineGpuNoSkin;
                    bool known = c.anim.empty() || c.anim == "none";
                    if (!known && c.anim.find_first_not_of("0123456789") == std::string::npos)
                    {
                        const unsigned long v = std::strtoul(c.anim.c_str(), nullptr, 10);
                        known = v < a.skin_names.size();
                        sk = known ? static_cast<uint32_t>(v) : kSpineGpuNoSkin;
                    }
                    else if (!known)
                    {
                        known = SpineGpuFindSkin(a, c.anim, sk);
                    }
                    if (!known)
                    {
                        std::string names;
                        for (const std::string& n : a.skin_names)
                        {
                            names += (names.empty() ? "" : ", ") + n;
                        }
                        LOG_WARNING(ZRender, "SpineGpuRenderer: instance #{}: skin '{}' not found (skins: {})", in->id,
                                    c.anim, names.empty() ? std::string("<none>") : names);
                        break;
                    }
                    const SpineImportResult sr = SpineGpuSetSkin(a, in->asset->data, in->state, sk);
                    if (!sr.ok)
                    {
                        LOG_WARNING(ZRender, "SpineGpuRenderer: instance #{}: setSkin failed: {}", in->id, sr.error);
                        break;
                    }
                    if (c.loop)
                    {
                        // spine-unity 的惯用法：SetSkin 之后 SetSlotsToSetupPose，让 setup 附件按新皮肤重挂
                        SpineGpuSetSlotsToSetupPose(a, in->asset->data, in->state);
                    }
                    in->skin = sk;
                    std::string berr;
                    if (!AssetBounds(*in->asset, in->anim, sk, in->bounds, berr))
                    {
                        LOG_WARNING(ZRender, "SpineGpuRenderer: instance #{}: bounds for skin failed: {}", in->id, berr);
                    }
                    LOG_INFO(ZRender, "SpineGpuRenderer: instance #{} skin = {}{}", in->id,
                             sk == kSpineGpuNoSkin ? std::string("<none>") : "'" + a.skin_names[sk] + "'",
                             c.loop ? " (+ setSlotsToSetupPose)" : "");
                    break;
                }
                case Command::Op::kPlaceAuto:
                    in->manual = false;
                    break;
                default:
                    break;
            }
        }
        cmds.clear();
    }

    // "track 0: 'walk' t=1.23 (mixing from 'idle' 40%); track 1: …"
    std::string DescribeTracks(const Instance& in) const
    {
        const SpineGpuAnimationState& st = *in.anim_state;
        const SpineGpuAsset& a = in.asset->asset;
        const auto name = [&](uint32_t ai) -> std::string {
            if (ai == kSpineGpuEmptyAnimation)
            {
                return "<empty>";
            }
            return ai < a.anim_names.size() ? a.anim_names[ai] : std::string("?");
        };
        std::string out;
        for (size_t i = 0; i < st.TrackCount(); ++i)
        {
            const SpineGpuTrackEntry* e = st.GetCurrent(i);
            if (e == nullptr)
            {
                continue;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), " t=%.2f", static_cast<double>(e->track_time));
            out += (out.empty() ? "" : "; ") + std::string("track ") + std::to_string(i) + ": '" + name(e->animation) +
                   "'" + buf;
            if (e->alpha != 1.0f)
            {
                std::snprintf(buf, sizeof(buf), " alpha=%.2f", static_cast<double>(e->alpha));
                out += buf;
            }
            if (e->mixing_from != nullptr && e->mix_duration > 0.0f)
            {
                std::snprintf(buf, sizeof(buf), " %d%%",
                              static_cast<int>(100.0f * std::min(1.0f, e->mix_time / e->mix_duration)));
                out += " (mixing from '" + name(e->mixing_from->animation) + "'" + buf + ")";
            }
            if (e->delay > 0.0f)
            {
                std::snprintf(buf, sizeof(buf), " (waiting %.2fs)", static_cast<double>(e->delay));
                out += buf;
            }
        }
        if (st.time_scale != 1.0f)
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%sspeed %.2f", out.empty() ? "" : "; ", static_cast<double>(st.time_scale));
            out += buf;
        }
        return out.empty() ? std::string("no tracks") : out;
    }

    // ------------------------------------------------------------------
    // 加载资产：骨架 + 图集 → 烘焙 → **容器往返**（与运行时读的是同一份字节）→ GPU
    // ------------------------------------------------------------------
    std::shared_ptr<Asset> LoadAsset(const std::string& path, std::string& err)
    {
        namespace fs = std::filesystem;
        const fs::path skel_path = FromUtf8(path);
        std::vector<uint8_t> skel_bytes;
        if (!ReadWholeFile(skel_path, skel_bytes))
        {
            err = "读不到骨架文件 " + path;
            return nullptr;
        }
        const std::string fname = ToUtf8(skel_path.filename());
        auto as = std::make_shared<Asset>();
        as->path = path;
        // 图集页按页名相对这个目录找：源骨架 = 图集所在目录；.zspg = 容器所在目录（SpineCook 会把页拷过去）
        fs::path page_dir;
        SpineImportResult r;
        if (EndsWithLower(fname, ".zspg"))
        {
            // 已烘焙的容器：直接读（与下面"烘焙 → 写 → 读"的往返落到同一个读端）
            r = ReadSpineGpuContainer(skel_bytes.data(), skel_bytes.size(), as->asset);
            if (!r.ok)
            {
                err = "容器 " + fname + "：" + r.error;
                return nullptr;
            }
            page_dir = skel_path.parent_path();
        }
        else if (!BakeFromSource(skel_path, fname, skel_bytes, as->asset, page_dir, err))
        {
            return nullptr;
        }
        r = SpineGpuBuildRuntimeData(as->asset, as->data);
        if (!r.ok)
        {
            err = r.error;
            return nullptr;
        }
        return UploadAsset(as, page_dir, err);
    }

    // 骨架源文件 + 同名图集 → 烘焙 → **容器往返**（与运行时读 .zspg 的是同一份字节）
    bool BakeFromSource(const std::filesystem::path& skel_path, const std::string& fname,
                        const std::vector<uint8_t>& skel_bytes, SpineGpuAsset& out, std::filesystem::path& page_dir,
                        std::string& err)
    {
        namespace fs = std::filesystem;
        const auto ends_with = [&](const std::string& suffix) {
            return fname.size() >= suffix.size() && fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        const bool is_json = ends_with(".json");
        std::string stem = fname;
        for (const char* suf : {".skel.bytes", ".skel", ".json"})
        {
            const std::string s(suf);
            if (ends_with(s))
            {
                stem = fname.substr(0, fname.size() - s.size());
                break;
            }
        }
        fs::path atlas_path = skel_path.parent_path() / FromUtf8(stem + ".atlas.txt");
        if (!fs::exists(atlas_path))
        {
            atlas_path = skel_path.parent_path() / FromUtf8(stem + ".atlas");
        }
        std::vector<uint8_t> atlas_bytes;
        if (!ReadWholeFile(atlas_path, atlas_bytes))
        {
            err = "找不到图集 " + ToUtf8(atlas_path);
            return false;
        }

        SpineAtlas atlas;
        SpineImportResult r = ParseSpineAtlas(reinterpret_cast<const char*>(atlas_bytes.data()), atlas_bytes.size(), atlas);
        SpineSourceSkeleton src;
        if (r.ok)
        {
            r = is_json ? ParseSpineSkeletonJson(reinterpret_cast<const char*>(skel_bytes.data()), skel_bytes.size(),
                                                 &atlas, src)
                        : ParseSpineSkeletonBinary(skel_bytes.data(), skel_bytes.size(), &atlas, src);
        }
        SpineGpuAsset baked;
        if (r.ok)
        {
            r = BakeSpineGpuAsset(src, baked);
        }
        std::vector<uint8_t> container;
        if (r.ok)
        {
            r = WriteSpineGpuContainer(baked, container);
        }
        if (r.ok)
        {
            r = ReadSpineGpuContainer(container.data(), container.size(), out);
        }
        if (!r.ok)
        {
            err = r.error;
            return false;
        }
        page_dir = atlas_path.parent_path();
        return true;
    }

    // 运行时数据已就绪的资产 → GPU（静态缓冲 / 图集页 / 每帧缓冲 + 描述符集）
    std::shared_ptr<Asset> UploadAsset(const std::shared_ptr<Asset>& as, const std::filesystem::path& page_dir,
                                       std::string& err)
    {
        namespace fs = std::filesystem;
        SpineGpuAsset& a = as->asset;
        if (a.anims.empty())
        {
            err = "骨架没有动画";
            return nullptr;
        }
        SpineGpuBuildBatchAssetInfo(a, as->info);

        // 失败时把已建的 GPU 资源交给延迟销毁（`as` 还没发布，GPU 没用过它们）
        struct Guard
        {
            Impl& impl;
            Asset& as;
            bool released {false};
            ~Guard()
            {
                if (!released)
                {
                    impl.RetireAsset(as);
                }
            }
        } guard {*this, *as};

        // ---- 静态缓冲：每个资产一份，所有实例共用 ----
        const RHIBufferUsageFlags ssbo = RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bool ok = CreateBuffer(as->vertex_words, a.vertex_buffer.size(), ssbo, a.vertex_buffer.data()) &&
                  CreateBuffer(as->infl_bind, a.influence_bind.size() * sizeof(float), ssbo, a.influence_bind.data()) &&
                  CreateBuffer(as->infl_bone, a.influence_bone.size() * sizeof(SpineInfluenceBone), ssbo,
                               a.influence_bone.data()) &&
                  CreateBuffer(as->uvs, a.uv_buffer.size() * sizeof(float), ssbo, a.uv_buffer.data()) &&
                  CreateBuffer(as->index, a.index_buffer.size() * sizeof(uint16_t), RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               a.index_buffer.data());
        if (!ok)
        {
            err = "创建静态缓冲失败";
            return nullptr;
        }

        // ---- 图集页 ----
        for (const std::string& page : a.atlas_page_names)
        {
            const fs::path png = page_dir / FromUtf8(page);
            // 自己读字节再解码：stbi_load 吃的是 ANSI 路径，中文目录会打不开
            std::vector<uint8_t> png_bytes;
            int w = 0, h = 0, ch = 0;
            stbi_uc* pixels = ReadWholeFile(png, png_bytes) && !png_bytes.empty()
                                  ? stbi_load_from_memory(png_bytes.data(), static_cast<int>(png_bytes.size()), &w, &h,
                                                          &ch, 4)
                                  : nullptr;
            if (pixels == nullptr)
            {
                err = "读不到图集页 " + ToUtf8(png);
                return nullptr;
            }
            Page pg;
            rhi->CreateGlobalImage(pg.image, pg.view, nullptr, static_cast<uint32_t>(w), static_cast<uint32_t>(h), pixels,
                                   RHI_FORMAT_R8G8B8A8_UNORM, 1);
            stbi_image_free(pixels);
            pg.sampler = rhi->GetOrCreateDefaultSampler(RHIDefaultSamplerType::Default_Sampler_Linear);
            as->pages.push_back(pg);   // 失败时已建的部分也交给 Guard
            if (pg.image == nullptr || pg.view == nullptr || pg.sampler == nullptr)
            {
                err = "上传图集页失败 " + ToUtf8(png);
                return nullptr;
            }
        }
        if (as->pages.empty())
        {
            err = "图集没有页";
            return nullptr;
        }

        // ---- 每帧缓冲 + 描述符集：初始容量按单实例给，多实例时 Draw 里按需倍增 ----
        const uint32_t frames = std::max<uint32_t>(1u, rhi->GetMaxFramesInFlight());
        for (std::vector<FrameSlot>& layer_frames : as->frames)
        layer_frames.resize(frames);
        for (std::vector<FrameSlot>& layer_frames : as->frames)
        for (FrameSlot& f : layer_frames)
        {
            ok = CreateBuffer(f.bones, static_cast<RHIDeviceSize>(a.header.bone_count) * sizeof(SpineGpuBoneSample), ssbo,
                              nullptr) &&
                 CreateBuffer(f.draws, static_cast<RHIDeviceSize>(std::max<size_t>(a.draw_items.size(), 1)) * sizeof(GpuDraw),
                              ssbo, nullptr) &&
                 CreateBuffer(f.insts, sizeof(GpuInst), ssbo, nullptr) &&
                 CreateBuffer(f.deform,
                              static_cast<RHIDeviceSize>(std::max<size_t>(as->info.deform_capacity, 1)) * sizeof(float),
                              ssbo, nullptr) &&
                 // 没有裁剪的资产永远用不到；有裁剪的第一帧按需扩
                 CreateBuffer(f.cpu_verts, (a.clip_polygons.empty() ? 1u : 4096u) * sizeof(GpuCpuVert), ssbo, nullptr) &&
                 CreateBuffer(f.cpu_index, (a.clip_polygons.empty() ? 2u : 8192u) * sizeof(uint16_t),
                              RHI_BUFFER_USAGE_INDEX_BUFFER_BIT, nullptr);
            if (!ok)
            {
                err = "创建每帧缓冲失败";
                return nullptr;
            }
            if (!AllocSet(set0_layout, free_set0, f.set0))
            {
                err = "分配描述符集失败（共享池可能满了）";
                return nullptr;
            }
            f.set1.assign(as->pages.size(), nullptr);
            for (RHIDescriptorSet*& s1 : f.set1)
            {
                if (!AllocSet(set1_layout, free_set1, s1))
                {
                    err = "分配图集页描述符集失败（共享池可能满了）";
                    return nullptr;
                }
            }
            WriteSet0(*as, f);
            WriteSet1(*as, f);
        }
        if (!EnsureDrawIds(*as, static_cast<uint32_t>(std::max<size_t>(a.draw_items.size(), 1))))
        {
            err = "创建绘制项下标顶点缓冲失败";
            return nullptr;
        }
        guard.released = true;
        return as;
    }
};

SpineGpuRenderer::SpineGpuRenderer() : m_Impl(new Impl()) {}   // 进程级单例，随进程退出

SpineGpuRenderer& SpineGpuRenderer::Get()
{
    static SpineGpuRenderer s_renderer;
    return s_renderer;
}

SpineGpuRenderer::Impl& SpineGpuRenderer::impl()
{
    return *m_Impl;
}

void SpineGpuRenderer::RequestLoad(const std::string& skeleton_path, int anim_index)
{
    // 多实例之前的语义：替换掉全部实例
    RequestClear();
    RequestAddInstance(skeleton_path, anim_index);
}

uint32_t SpineGpuRenderer::RequestAddInstance(const std::string& skeleton_path, int anim_index)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kAddInstance;
    c.target = d.next_id++;
    c.path = skeleton_path;
    c.anim = std::to_string(std::max(0, anim_index));
    d.pending_cmds.push_back(c);
    d.live_ids.push_back(c.target);
    d.selected = c.target;
    d.status = "loading " + skeleton_path;
    return c.target;
}

bool SpineGpuRenderer::RequestRemoveInstance(uint32_t id)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    if (id == 0)
    {
        id = d.selected;
    }
    if (std::find(d.live_ids.begin(), d.live_ids.end(), id) == d.live_ids.end())
    {
        return false;
    }
    Impl::Command c;
    c.op = Impl::Command::Op::kRemoveInstance;
    c.target = id;
    d.pending_cmds.push_back(c);
    d.live_ids.erase(std::remove(d.live_ids.begin(), d.live_ids.end(), id), d.live_ids.end());
    if (d.selected == id)
    {
        d.selected = d.live_ids.empty() ? 0u : d.live_ids.back();
    }
    return true;
}

void SpineGpuRenderer::RequestClear()
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kClearInstances;
    d.pending_cmds.push_back(c);
    d.live_ids.clear();
    d.selected = 0;
    d.stats[kLayerOverlay].summary.clear();
}

bool SpineGpuRenderer::RequestSelect(uint32_t id)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    if (std::find(d.live_ids.begin(), d.live_ids.end(), id) == d.live_ids.end())
    {
        return false;
    }
    d.selected = id;
    return true;
}

uint32_t SpineGpuRenderer::Selected() const
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->selected;
}

void SpineGpuRenderer::RequestPlace(float x, float y, float scale)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kPlace;
    c.value = x;
    c.delay = y;
    c.scale = scale;
    d.PushTargeted(std::move(c));
}

void SpineGpuRenderer::RequestPlaceAuto()
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kPlaceAuto;
    d.PushTargeted(std::move(c));
}

void SpineGpuRenderer::RequestSetAnimation(int track, const std::string& anim, bool loop)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kSet;
    c.track = track;
    c.anim = anim;
    c.loop = loop;
    d.PushTargeted(std::move(c));
}

void SpineGpuRenderer::RequestAddAnimation(int track, const std::string& anim, bool loop, float delay)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kAdd;
    c.track = track;
    c.anim = anim;
    c.loop = loop;
    c.delay = delay;
    d.PushTargeted(std::move(c));
}

void SpineGpuRenderer::RequestSetEmptyAnimation(int track, float mix_duration)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kEmpty;
    c.track = track;
    c.value = mix_duration;
    d.PushTargeted(std::move(c));
}

void SpineGpuRenderer::RequestDefaultMix(float seconds)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kMix;
    c.value = seconds;
    d.pending_cmds.push_back(std::move(c));
}

void SpineGpuRenderer::RequestTimeScale(float scale)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kSpeed;
    c.value = scale;
    d.PushTargeted(std::move(c));
}

void SpineGpuRenderer::RequestSkin(const std::string& skin, bool setup_slots)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kSkin;
    c.anim = skin;
    c.loop = setup_slots;
    d.PushTargeted(std::move(c));
}

void SpineGpuRenderer::RequestTintBlack(bool enabled)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.tint_black = enabled;
    d.tint_black_logged = false;
}

void SpineGpuRenderer::RequestTrackAlpha(int track, float alpha)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kAlpha;
    c.track = track;
    c.value = alpha;
    d.PushTargeted(std::move(c));
}

uint32_t SpineGpuRenderer::RequestAddWorldInstance(const std::string& path, const std::string& anim, bool loop,
                                                   uint64_t owner)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kAddInstance;
    c.target = d.next_id++;
    c.path = path;
    c.anim = anim;
    c.loop = loop;
    c.world = true;
    d.pending_cmds.push_back(c);
    Impl::WorldEntry& e = d.world[c.target];
    e.owner = owner;
    return c.target;
}

bool SpineGpuRenderer::RequestSetWorldTransform(uint32_t id, const float local_to_world[16], float dt, bool visible,
                                                const SpineGpuBillboard& billboard)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    const auto w = d.world.find(id);
    if (w == d.world.end())
    {
        return false;
    }
    std::memcpy(w->second.model, local_to_world, sizeof(w->second.model));
    // 渲染长时间没跑（最小化 / 视口 0×0）时别无限累加：Draw 反正会夹到 0.25 s
    w->second.pending_dt = std::min(w->second.pending_dt + std::max(0.0f, dt), 1.0f);
    w->second.visible = visible;
    w->second.billboard = billboard;
    w->second.stamp = d.world_serial;
    return true;
}

void SpineGpuRenderer::AdvanceWorldTick()
{
    if (m_Impl == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    ++m_Impl->world_serial;
}

void SpineGpuRenderer::RequestRemoveWorldInstance(uint32_t id)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.world.erase(id) == 0)
    {
        return;
    }
    Impl::Command c;
    c.op = Impl::Command::Op::kRemoveInstance;
    d.PushTo(id, std::move(c));
}

void SpineGpuRenderer::RemoveWorldInstancesOfOwner(uint64_t owner)
{
    if (m_Impl == nullptr)
    {
        return;
    }
    Impl& d = *m_Impl;
    std::lock_guard<std::mutex> lock(d.mutex);
    for (auto it = d.world.begin(); it != d.world.end();)
    {
        if (it->second.owner != owner)
        {
            ++it;
            continue;
        }
        Impl::Command c;
        c.op = Impl::Command::Op::kRemoveInstance;
        d.PushTo(it->first, std::move(c));
        it = d.world.erase(it);
    }
}

void SpineGpuRenderer::RequestSetAnimationOn(uint32_t id, int track, const std::string& anim, bool loop)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kSet;
    c.track = track;
    c.anim = anim;
    c.loop = loop;
    d.PushTo(id, std::move(c));
}

void SpineGpuRenderer::RequestTimeScaleOn(uint32_t id, float scale)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kSpeed;
    c.value = scale;
    d.PushTo(id, std::move(c));
}

void SpineGpuRenderer::RequestSkinOn(uint32_t id, const std::string& skin, bool setup_slots)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::Command c;
    c.op = Impl::Command::Op::kSkin;
    c.anim = skin;
    c.loop = setup_slots;
    d.PushTo(id, std::move(c));
}

uint32_t SpineGpuRenderer::LastDrawCount() const
{
    if (m_Impl == nullptr)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->stats[kLayerOverlay].draw_count + m_Impl->stats[kLayerWorld].draw_count;
}

std::string SpineGpuRenderer::Status() const
{
    if (m_Impl == nullptr)
    {
        return "idle";
    }
    const Impl& d = *m_Impl;
    std::lock_guard<std::mutex> lock(d.mutex);
    std::string out = d.status + " (selected #" + std::to_string(d.selected) + "; tint black " +
                      (d.tint_black ? std::string("on") : std::string("off")) + ")";
    const char* const names[kLayerCount] = {"overlay", "world"};
    for (int layer = 0; layer < kLayerCount; ++layer)
    {
        const Impl::LayerStats& st = d.stats[layer];
        out += std::string("\n  ") + names[layer] + " layer, last frame: " + std::to_string(st.draw_count) +
               " draw items, " + std::to_string(st.deform_draws) + " deformed / " + std::to_string(st.deform_floats) +
               " deform floats, " + std::to_string(st.clipped_draws) + " clipped / " +
               std::to_string(st.clip_vertices) + " clip vertices" +
               (d.tint_black ? ", " + std::to_string(st.tint_black_draws) + " dark draws" : std::string()) +
               (st.skip_reason.empty() ? std::string() : "; skipped: " + st.skip_reason) + st.summary;
    }
    return out;
}

namespace
{
    // 公告板：把 model 的旋转换成"朝相机"，平移 = 枢轴点世界坐标，骨架 x / y 各乘带符号的缩放。
    // `view` 是 Matrix4x4 的 m_Mat（行主序）；MakeLookAtMatrix 的第 0 / 1 / 2 行 = 相机右 / 上 / 后（世界空间），
    // 游戏相机的 view 矩阵同一形状 ⇒ 从 view 取轴，两种相机通吃（RenderCamera::right() 对游戏相机不准）。
    // full：骨架平面 = 相机平面；vertical：只绕世界 Z 转（Z 向上），人物保持直立（2.5D 角色常用）。
    // 退化（vertical 时相机正对天 / 地、右向量水平分量为 0）⇒ 退回 full。
    void BillboardModel(const float view[16], const SpineGpuBillboard& bb, float out[16])
    {
        float right[3] = {view[0], view[1], view[2]};
        float up[3] = {view[4], view[5], view[6]};
        if (bb.mode == SpineGpuBillboardMode::kVertical)
        {
            const float len = std::sqrt(right[0] * right[0] + right[1] * right[1]);
            if (len > 1e-4f)
            {
                right[0] /= len;
                right[1] /= len;
                right[2] = 0.0f;
                up[0] = 0.0f;
                up[1] = 0.0f;
                up[2] = 1.0f;
            }
        }
        const float normal[3] = {right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2],
                                 right[0] * up[1] - right[1] * up[0]};
        const float sz = 0.5f * (std::fabs(bb.scale_x) + std::fabs(bb.scale_y));
        for (int r = 0; r < 3; ++r)
        {
            out[r * 4 + 0] = right[r] * bb.scale_x;
            out[r * 4 + 1] = up[r] * bb.scale_y;
            out[r * 4 + 2] = normal[r] * sz;
            out[r * 4 + 3] = bb.position[r];
        }
        out[12] = 0.0f;
        out[13] = 0.0f;
        out[14] = 0.0f;
        out[15] = 1.0f;
    }
}  // namespace

void SpineGpuRenderer::Draw(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
                            ViewportType viewport_type)
{
    DrawLayer(kLayerOverlay, rhi, render_pass, subpass, viewport, viewport_type);
}

void SpineGpuRenderer::DrawWorld(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
                                 ViewportType viewport_type)
{
    DrawLayer(kLayerWorld, rhi, render_pass, subpass, viewport, viewport_type);
}

void SpineGpuRenderer::DrawLayer(int layer, RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass,
                                 const RHIViewport& viewport, ViewportType viewport_type)
{
    if (rhi == nullptr || render_pass == nullptr)
    {
        return;
    }
    Impl& d = *m_Impl;
    {
        // 从没人加过实例：一次加锁 + 几个判断，不碰 GPU（instances / assets / retired 只有渲染线程碰）
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.pending_cmds.empty() && d.instances.empty() && d.assets.empty() && d.retired.empty())
        {
            return;
        }
    }
    d.rhi = rhi;
    const bool world_layer = layer == kLayerWorld;
    // 每帧只做一次的家务（延迟销毁倒计时 / 回收没人用的资产）挂在 UI 层：三个后端每帧都会录它，
    // 而且它在世界层（RP1）之后 ⇒ 回收不会抢在本帧世界层的录制之前。
    if (!world_layer)
    {
        d.TickRetired();
    }

    // ---- 处理游戏线程的命令（按提交顺序；哪层先到哪层跑，加载只要描述符布局、与层无关）----
    std::vector<Impl::Command> cmds;
    bool tint_black_on = false;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        cmds.swap(d.pending_cmds);
        tint_black_on = d.tint_black;
    }
    if (!cmds.empty())
    {
        d.RunCommands(cmds);
    }
    // 世界实例：取组件最新推来的变换 / 公告板 / 可见性，消费累加的 dt（"最新的赢"，不走命令队列）
    if (world_layer)
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        for (std::unique_ptr<Impl::Instance>& ip : d.instances)
        {
            if (!ip->world)
            {
                continue;
            }
            const auto w = d.world.find(ip->id);
            if (w == d.world.end())
            {
                ip->world_visible = false;   // 已被删（kRemoveInstance 还在路上）
                ip->world_dt = 0.0f;
                continue;
            }
            std::memcpy(ip->world_model, w->second.model, sizeof(ip->world_model));
            ip->world_billboard = w->second.billboard;
            ip->world_visible = w->second.visible && w->second.stamp + 1u >= d.world_serial;
            if (ip->world_shown_logged != static_cast<int>(ip->world_visible))
            {
                ip->world_shown_logged = static_cast<int>(ip->world_visible);
                LOG_INFO(ZRender, "SpineGpuRenderer: world instance #{} {} (owner #{}{})", ip->id,
                         ip->world_visible ? "shown" : "hidden", w->second.owner,
                         w->second.visible ? (ip->world_visible ? "" : ", not ticked this frame")
                                           : ", component / object disabled");
            }
            ip->world_dt = w->second.pending_dt;
            w->second.pending_dt = 0.0f;
        }
    }
    else
    {
        d.PurgeUnusedAssets();
    }

    // 本层的实例（世界层随后按远→近重排）
    std::vector<Impl::Instance*> order;
    for (std::unique_ptr<Impl::Instance>& ip : d.instances)
    {
        if (ip->world == world_layer)
        {
            order.push_back(ip.get());
        }
    }
    // 每条"整帧没画"的原因都进 status，且**变化时**记一行日志（不刷屏、也不静默）
    const char* const layer_name = world_layer ? "world" : "overlay";
    const auto skip = [&d, layer, layer_name](const std::string& why) {
        std::lock_guard<std::mutex> lock(d.mutex);
        Impl::LayerStats& st = d.stats[layer];
        if (st.skip_reason != why)
        {
            st.skip_reason = why;
            LOG_WARNING(ZRender, "SpineGpuRenderer: skipping {} layer: {}", layer_name, why);
        }
        st.draw_count = 0;
    };
    if (order.empty())
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.instances.empty() && d.status.rfind("load failed", 0) != 0)
        {
            d.status = "idle";
        }
        d.stats[layer] = Impl::LayerStats {};
        return;
    }
    if (viewport.width <= 0.0f || viewport.height <= 0.0f)
    {
        skip("viewport is empty (" + std::to_string(viewport.width) + "x" + std::to_string(viewport.height) + ")");
        return;
    }
    if (!d.EnsurePipelines(layer, render_pass, subpass))
    {
        skip("pipelines are not ready");
        return;
    }

    // ---- 1) 逐实例解算（CPU 只算骨骼与槽位，不算顶点）+ 裁剪 ----
    // S5：AnimationState 的标准三步 update(dt) → apply(skeleton) → updateWorldTransform。
    // dt 夹到 [0, 0.25]：编辑器卡顿 / 断点之后不让动画一步跳过好几圈。
    // 一个实例失败只跳过它自己（原因进 status、变化时记日志），别的照画。
    const auto now = std::chrono::steady_clock::now();
    const auto fail_instance = [](Impl::Instance& in, const std::string& why) {
        in.solved = false;
        in.range = SpineGpuBatchRange {};
        if (in.error != why)
        {
            in.error = why;
            LOG_WARNING(ZRender, "SpineGpuRenderer: instance #{} skipped: {}", in.id, why);
        }
    };
    for (Impl::Instance* ip : order)
    {
        Impl::Instance& in = *ip;
        if (in.world && !in.world_visible)
        {
            // 组件 / GameObject 关掉了：不解算、不画（动画停在原处，与 disabled 的组件不 tick 一致）
            in.solved = false;
            in.range = SpineGpuBatchRange {};
            in.last_tick = now;
            continue;
        }
        const SpineGpuAsset& a = in.asset->asset;
        // 控制台实例走墙钟；世界实例用组件喂的 dt（暂停时组件不 tick ⇒ 0 ⇒ 动画停）
        const float wall_dt = std::chrono::duration<float>(now - in.last_tick).count();
        const float dt = std::clamp(in.world ? in.world_dt : wall_dt, 0.0f, 0.25f);
        in.last_tick = now;
        in.anim_state->Update(dt);
        SpineImportResult pr = in.anim_state->Apply(in.state);
        if (pr.ok)
        {
            pr = SpineGpuUpdateWorld(a, in.asset->data, in.state, in.pose);
            if (pr.ok)
            {
                // v15：按当前绘制顺序跑 SpineMesh.h 的状态机，作用域内的项在 CPU 上蒙皮 + 裁
                pr = SpineGpuClipFrame(a, in.state, in.pose, in.clipper, in.clip);
                if (!pr.ok)
                {
                    pr.error = "clip failed: " + pr.error;
                }
            }
            else
            {
                pr.error = "solve failed: " + pr.error;
            }
        }
        else
        {
            pr.error = "solve failed: " + pr.error;
        }
        if (!pr.ok)
        {
            fail_instance(in, pr.error);
            continue;
        }
        in.solved = true;
    }

    // ---- 2) 摆放 + 拼包：同一资产的实例首尾相接（SpineGpuBatch）----
    for (std::shared_ptr<Impl::Asset>& as : d.assets)
    {
        as->batch[layer].Clear();
    }
    const float y_sign = ClipSpaceYSign(rhi);
    std::vector<GpuInst> insts(order.size());
    if (world_layer)
    {
        // 与网格同一个 proj(aspect) · view：aspect 取 RHI 视口（RenderResourcePerFrame.cpp 的口径；
        // 编辑器 Scene 视口的 render rect 与它同比例，取哪个都行，但跟网格取同一个最稳）
        RenderSystem* render_system = GET_SYSTEM(RenderSystem);
        const std::shared_ptr<RenderCamera> camera =
            render_system != nullptr ? render_system->GetCamera(viewport_type) : nullptr;
        if (camera == nullptr)
        {
            skip("no render camera for this viewport");
            return;
        }
        float aspect = viewport.width / viewport.height;
        if (const RHIViewport* rv = rhi->GetViewport(viewport_type); rv != nullptr && rv->width > 0.0f && rv->height > 0.0f)
        {
            aspect = rv->width / rv->height;
        }
        const Matrix4x4 view_m = camera->GetViewMatrix();
        const Matrix4x4 pv_m = camera->GetProjectionMatrixForAspect(aspect) * view_m;
        float view[16];
        float proj_view[16];
        std::memcpy(view, &view_m.m_Mat[0][0], sizeof(view));
        std::memcpy(proj_view, &pv_m.m_Mat[0][0], sizeof(proj_view));
        for (Impl::Instance* ip : order)
        {
            Impl::Instance& in = *ip;
            float model[16];
            if (in.world_billboard.mode != SpineGpuBillboardMode::kNone)
            {
                BillboardModel(view, in.world_billboard, model);
            }
            else
            {
                std::memcpy(model, in.world_model, sizeof(model));
            }
            std::memcpy(in.world_model_used, model, sizeof(model));
            // 枢轴点（model 的平移列）的观察空间 z：越负越远
            in.world_view_z = view[8] * model[3] + view[9] * model[7] + view[10] * model[11] + view[11];
        }
        // 透明物体按远 → 近画（不写深度 ⇒ 实例之间靠绘制顺序叠）。同距离保持加入顺序。
        std::stable_sort(order.begin(), order.end(),
                         [](const Impl::Instance* a, const Impl::Instance* b) { return a->world_view_z < b->world_view_z; });
        for (size_t k = 0; k < order.size(); ++k)
        {
            Impl::Instance& in = *order[k];
            insts[k] = WorldInstance(proj_view, in.world_model_used);
            if (!in.world_logged && in.solved)
            {
                // setup 包围盒四角 → NDC（与 spine_gpu.vert 同一乘法：clip = M · (x, y, 0, 1)，列主序）
                in.world_logged = true;
                float nx0 = 1e30f, ny0 = 1e30f, nx1 = -1e30f, ny1 = -1e30f, wmin = 1e30f;
                for (int c = 0; c < 4; ++c)
                {
                    const float bx = in.bounds[(c & 1) ? 2 : 0];
                    const float by = in.bounds[(c & 2) ? 3 : 1];
                    const float* m = insts[k].clip_from_world;
                    const float cx = m[0] * bx + m[4] * by + m[12];
                    const float cy = m[1] * bx + m[5] * by + m[13];
                    const float cw = m[3] * bx + m[7] * by + m[15];
                    wmin = std::min(wmin, cw);
                    if (cw > 1e-6f)
                    {
                        nx0 = std::min(nx0, cx / cw);
                        nx1 = std::max(nx1, cx / cw);
                        ny0 = std::min(ny0, cy / cw);
                        ny1 = std::max(ny1, cy / cw);
                    }
                }
                static const char* const kModes[] = {"none", "full", "vertical"};
                LOG_INFO(ZRender,
                         "SpineGpuRenderer: world instance #{} (billboard {}) setup bounds -> NDC x [{:.3f}, {:.3f}] "
                         "y [{:.3f}, {:.3f}] (min w {:.3f}; on screen iff it overlaps [-1, 1] and w > 0), view z {:.3f}; "
                         "pivot ({:.3f}, {:.3f}, {:.3f}) scale ({:.4f}, {:.4f}), camera right ({:.3f}, {:.3f}, {:.3f}) "
                         "up ({:.3f}, {:.3f}, {:.3f})",
                         in.id, kModes[static_cast<int>(in.world_billboard.mode) % 3], nx0, nx1, ny0, ny1, wmin,
                         in.world_view_z, in.world_model_used[3], in.world_model_used[7], in.world_model_used[11],
                         in.world_billboard.scale_x, in.world_billboard.scale_y, view[0], view[1], view[2], view[4],
                         view[5], view[6]);
            }
        }
    }
    else
    {
        uint32_t auto_count = 0;
        for (const Impl::Instance* ip : order)
        {
            auto_count += ip->manual ? 0u : 1u;   // 失败的也占格子：别的实例不因它跳来跳去
        }
        uint32_t auto_index = 0;
        for (size_t k = 0; k < order.size(); ++k)
        {
            const Impl::Instance& in = *order[k];
            if (in.manual)
            {
                insts[k] = SpineGpuPlaceInstance(in.place_x, in.place_y, in.place_scale, viewport.width, viewport.height,
                                                 y_sign);
            }
            else
            {
                const SpineGpuRect cell = SpineGpuGridCell(auto_count, auto_index++, viewport.width, viewport.height);
                insts[k] = SpineGpuFitInstance(in.bounds, cell, viewport.width, viewport.height, y_sign);
            }
        }
    }
    for (size_t k = 0; k < order.size(); ++k)
    {
        Impl::Instance& in = *order[k];
        in.range = SpineGpuBatchRange {};
        if (!in.solved)
        {
            continue;
        }
        SpineGpuBatch& batch = in.asset->batch[layer];
        const SpineImportResult br = SpineGpuAppendBatchInstance(in.asset->asset, in.asset->info, in.pose, &in.clip,
                                                                 insts[k], tint_black_on, batch, in.range);
        if (!br.ok)
        {
            fail_instance(in, br.error);
            continue;
        }
        if (world_layer)
        {
            // 场景是线性 HDR（随后 tonemap + gamma）：片元把图集的 sRGB 值转成线性再输出（spine_gpu.frag bit5）
            for (uint32_t i = in.range.draw_first; i < in.range.draw_first + in.range.draw_count; ++i)
            {
                batch.draws[i].flags |= SpineGpuDrawFlags::kSceneLinear;
            }
        }
        in.error.clear();
    }

    // ---- 3) 上传：每个资产本层本帧槽位的骨骼 / 绘制项 / 实例 / deform / 裁剪顶点 + 索引 ----
    const uint32_t frame_index = static_cast<uint32_t>(rhi->GetCurrentFrameIndex());
    for (std::shared_ptr<Impl::Asset>& asp : d.assets)
    {
        Impl::Asset& as = *asp;
        const SpineGpuBatch& b = as.batch[layer];
        if (b.draws.empty())
        {
            continue;
        }
        std::vector<Impl::FrameSlot>& slots = as.frames[layer];
        Impl::FrameSlot& fs = slots[frame_index % slots.size()];
        const RHIBufferUsageFlags ssbo = RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        const auto bytes = [](const auto& v) { return static_cast<RHIDeviceSize>(v.size() * sizeof(v[0])); };
        bool ok = d.EnsureFrameBuffer(fs, fs.bones, bytes(b.bones), ssbo, static_cast<int>(kBonesBinding)) &&
                  d.EnsureFrameBuffer(fs, fs.draws, bytes(b.draws), ssbo, static_cast<int>(kDrawsBinding)) &&
                  d.EnsureFrameBuffer(fs, fs.insts, bytes(b.insts), ssbo, static_cast<int>(kInstsBinding)) &&
                  d.EnsureFrameBuffer(fs, fs.deform, bytes(b.deform), ssbo, static_cast<int>(kDeformBinding)) &&
                  d.EnsureFrameBuffer(fs, fs.cpu_verts, bytes(b.cpu_verts), ssbo, static_cast<int>(kCpuVertBinding)) &&
                  d.EnsureFrameBuffer(fs, fs.cpu_index, bytes(b.cpu_index), RHI_BUFFER_USAGE_INDEX_BUFFER_BIT, -1) &&
                  d.EnsureDrawIds(as, static_cast<uint32_t>(b.draws.size()));
        if (ok)
        {
            static_assert(sizeof(SpineGpuBoneSample) == sizeof(SpineBoneTransform), "bone sample is the GPU bone");
            ok = d.Upload(fs.bones, b.bones.data(), b.bones.size() * sizeof(b.bones[0])) &&
                 d.Upload(fs.draws, b.draws.data(), b.draws.size() * sizeof(b.draws[0])) &&
                 d.Upload(fs.insts, b.insts.data(), b.insts.size() * sizeof(b.insts[0])) &&
                 d.Upload(fs.deform, b.deform.data(), b.deform.size() * sizeof(float)) &&
                 d.Upload(fs.cpu_verts, b.cpu_verts.data(), b.cpu_verts.size() * sizeof(b.cpu_verts[0])) &&
                 d.Upload(fs.cpu_index, b.cpu_index.data(), b.cpu_index.size() * sizeof(uint16_t));
        }
        if (!ok)
        {
            // 这个资产的实例这一帧都不画（缓冲扩不动 / 映射失败），别的资产照画
            as.batch[layer].Clear();
            for (Impl::Instance* ip : order)
            {
                if (ip->asset.get() == &as && ip->solved)
                {
                    fail_instance(*ip, "could not grow / map the per-frame buffers");
                }
            }
            continue;
        }
        d.RefreshDescriptors(as, fs);
    }

    // ---- 4) 录制：按 `order`（UI 层 = 加入顺序，先加的在下面；世界层 = 远 → 近）；一项一次 DrawIndexed，
    //         firstInstance = 绘制项在本资产合包 b5 里的全局下标。换资产时整套状态重绑。
    RHICommandBuffer* cmd = rhi->GetCurrentCommandBuffer();
    float color[4] = {0.9f, 0.6f, 1.0f, 1.0f};
    rhi->PushEvent(cmd, world_layer ? "SpineGpu.World" : "SpineGpu", color);
    RHIViewport vp = viewport;
    // Vulkan 要求 scissor offset ≥ 0（视口可能部分移出窗口）
    const float sx0 = std::max(viewport.x, 0.0f);
    const float sy0 = std::max(viewport.y, 0.0f);
    const float sx1 = std::max(viewport.x + viewport.width, sx0);
    const float sy1 = std::max(viewport.y + viewport.height, sy0);
    RHIRect2D scissor = {{static_cast<int32_t>(sx0), static_cast<int32_t>(sy0)},
                         {static_cast<uint32_t>(sx1 - sx0), static_cast<uint32_t>(sy1 - sy0)}};
    rhi->CmdSetViewportPFN(cmd, 0, 1, &vp);
    rhi->CmdSetScissorPFN(cmd, 0, 1, &scissor);
    const bool dx12 = d.IsDx12();
    const Impl::Asset* bound_asset = nullptr;
    int bound_index = -1;   // 0 = 静态索引缓冲，1 = 本帧 CPU 裁剪的动态索引缓冲
    int bound_mode = -1;
    int bound_page = -1;
    uint32_t total_draws = 0, deformed = 0, deform_floats = 0, clipped = 0, clip_vertices = 0, dark = 0;
    std::string summary;
    for (const Impl::Instance* ip : order)
    {
        const Impl::Instance& in = *ip;
        summary += " | #" + std::to_string(in.id) + " '" + ToUtf8(FromUtf8(in.asset->path).filename()) + "'" +
                   (in.skin != kSpineGpuNoSkin && in.skin < in.asset->asset.skin_names.size()
                        ? " skin '" + in.asset->asset.skin_names[in.skin] + "'"
                        : std::string()) +
                   (in.world ? (in.world_visible ? "" : " (hidden)") : (in.manual ? " placed" : "")) + ": " +
                   (in.error.empty() ? std::to_string(in.range.draw_count) + " draws [" + d.DescribeTracks(in) + "]"
                                     : "skipped: " + in.error);
        if (in.range.draw_count == 0)
        {
            continue;
        }
        Impl::Asset& as = *in.asset;
        std::vector<Impl::FrameSlot>& slots = as.frames[layer];
        Impl::FrameSlot& fs = slots[frame_index % slots.size()];
        const SpineGpuBatch& batch = as.batch[layer];
        if (&as != bound_asset)
        {
            bound_asset = &as;
            bound_index = -1;
            bound_mode = -1;
            bound_page = -1;
        }
        const SpineGpuAsset& a = as.asset;
        for (uint32_t i = in.range.draw_first; i < in.range.draw_first + in.range.draw_count; ++i)
        {
            const SpineGpuBatchCmd& dc = batch.cmds[i];
            const SpineDrawItem& di = a.draw_items[dc.item];
            const int want_index = dc.cpu ? 1 : 0;
            if (want_index != bound_index)
            {
                rhi->CmdBindIndexBufferPFN(cmd, dc.cpu ? fs.cpu_index.buffer : as.index.buffer, 0, RHI_INDEX_TYPE_UINT16);
                bound_index = want_index;
            }
            const int mode = static_cast<int>(di.blend_mode) & 3;
            if (mode != bound_mode)
            {
                rhi->CmdBindPipelinePFN(cmd, RHI_PIPELINE_BIND_POINT_GRAPHICS, d.pipelines[layer][mode]);
                if (dx12)
                {
                    // DX12RHI 的顶点缓冲 stride 取自**当前**管线 ⇒ 必须在绑管线之后绑
                    RHIBuffer* id_buf = as.draw_ids.buffer;
                    const RHIDeviceSize zero = 0;
                    rhi->CmdBindVertexBuffersPFN(cmd, 0, 1, &id_buf, &zero);
                }
                bound_mode = mode;
                bound_page = -1;
            }
            const int page = di.atlas_page < as.pages.size() ? static_cast<int>(di.atlas_page) : 0;
            if (page != bound_page)
            {
                // set 0 + set 1 **一次**绑：DX12RHI 每次 CmdBindDescriptorSets 都会重设根签名，分两次绑时
                // 只靠"同一根签名不清绑定"这条驱动行为保住 set 0 —— 一起绑就不依赖它。
                const RHIDescriptorSet* sets[2] = {fs.set0, fs.set1[static_cast<size_t>(page)]};
                rhi->CmdBindDescriptorSetsPFN(cmd, RHI_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline_layout, 0, 2, sets, 0,
                                              nullptr);
                bound_page = page;
            }
            rhi->CmdDrawIndexedPFN(cmd, dc.index_count, 1, dc.index_first, 0, i);
        }
        total_draws += in.range.draw_count;
        deformed += in.range.deformed;
        deform_floats += in.range.deform_floats;
        clipped += in.range.clipped;
        clip_vertices += in.range.clip_vertices;
        dark += in.range.tint_black;
    }
    rhi->PopEvent(cmd);

    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::LayerStats& st = d.stats[layer];
    size_t world_count = 0;
    for (const std::unique_ptr<Impl::Instance>& ip : d.instances)
    {
        world_count += ip->world ? 1u : 0u;
    }
    d.status = "playing " + std::to_string(d.instances.size() - world_count) + " overlay + " +
               std::to_string(world_count) + " world instance(s) of " + std::to_string(d.assets.size()) + " asset(s)";
    if (total_draws == 0)
    {
        if (st.skip_reason != "no visible draw items")
        {
            st.skip_reason = "no visible draw items";
            LOG_WARNING(ZRender, "SpineGpuRenderer: {} layer: no visible draw items ({})", layer_name, summary);
        }
    }
    else if (st.draw_count == 0)
    {
        // 第一帧真正录了东西时记一行：视口 / 实例 / 项数，排查"画了但看不见"
        LOG_INFO(ZRender,
                 "SpineGpuRenderer: {} layer first frame {} draw items from {} instance(s) ({} deformed, {} deform "
                 "floats), viewport ({}, {}) {}x{}",
                 layer_name, total_draws, order.size(), deformed, deform_floats, viewport.x, viewport.y, viewport.width,
                 viewport.height);
        st.skip_reason.clear();
    }
    st.draw_count = total_draws;
    st.summary = summary;
    st.deform_draws = deformed;
    st.deform_floats = deform_floats;
    if (clipped > 0 && st.clipped_draws == 0)
    {
        // 裁剪从"没有"变成"有"时记一行（不刷屏）
        LOG_INFO(ZRender, "SpineGpuRenderer: {} layer clipping active: {} clipped draw items, {} clip vertices",
                 layer_name, clipped, clip_vertices);
    }
    st.clipped_draws = clipped;
    st.clip_vertices = clip_vertices;
    if (dark > 0 && !d.tint_black_logged)
    {
        // 每次加实例 / 拨开关后第一次出现暗色项时记一行（动画循环里暗色时有时无，按"0→非 0"记会刷屏）
        d.tint_black_logged = true;
        LOG_INFO(ZRender, "SpineGpuRenderer: tint black active: {} of {} draw items carry a dark color", dark,
                 total_draws);
    }
    st.tint_black_draws = dark;
}

void SpineGpuRenderer::DrawInEditorViewport(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass)
{
    if (rhi == nullptr)
    {
        return;
    }
    const RHIViewport* scene = rhi->GetViewport(ViewportType::scene);
    const bool use_scene = scene != nullptr && scene->width > 0.0f && scene->height > 0.0f;
    const RHIViewport* vp = use_scene ? scene : rhi->GetViewport(ViewportType::game);
    if (vp == nullptr)
    {
        return;
    }
    Draw(rhi, render_pass, subpass, *vp, use_scene ? ViewportType::scene : ViewportType::game);
}

#else  // __EMSCRIPTEN__ —— WebGL2 没有 SSBO / gl_InstanceIndex 偏移：接口保留，什么都不画

struct SpineGpuRenderer::Impl
{
    std::mutex mutex;
};

SpineGpuRenderer::SpineGpuRenderer() : m_Impl(new Impl()) {}

SpineGpuRenderer& SpineGpuRenderer::Get()
{
    static SpineGpuRenderer s_renderer;
    return s_renderer;
}
SpineGpuRenderer::Impl& SpineGpuRenderer::impl()
{
    return *m_Impl;
}
void SpineGpuRenderer::RequestLoad(const std::string&, int) {}
uint32_t SpineGpuRenderer::RequestAddInstance(const std::string&, int) { return 0; }
bool SpineGpuRenderer::RequestRemoveInstance(uint32_t) { return false; }
bool SpineGpuRenderer::RequestSelect(uint32_t) { return false; }
uint32_t SpineGpuRenderer::Selected() const { return 0; }
void SpineGpuRenderer::RequestPlace(float, float, float) {}
void SpineGpuRenderer::RequestPlaceAuto() {}
void SpineGpuRenderer::RequestClear() {}
void SpineGpuRenderer::RequestSetAnimation(int, const std::string&, bool) {}
void SpineGpuRenderer::RequestAddAnimation(int, const std::string&, bool, float) {}
void SpineGpuRenderer::RequestSetEmptyAnimation(int, float) {}
void SpineGpuRenderer::RequestDefaultMix(float) {}
void SpineGpuRenderer::RequestTimeScale(float) {}
void SpineGpuRenderer::RequestTrackAlpha(int, float) {}
void SpineGpuRenderer::RequestTintBlack(bool) {}
void SpineGpuRenderer::RequestSkin(const std::string&, bool) {}
uint32_t SpineGpuRenderer::RequestAddWorldInstance(const std::string&, const std::string&, bool, uint64_t) { return 1; }
// true：别让组件以为 id 丢了而每帧重建
bool SpineGpuRenderer::RequestSetWorldTransform(uint32_t, const float*, float, bool, const SpineGpuBillboard&)
{
    return true;
}
void SpineGpuRenderer::RequestRemoveWorldInstance(uint32_t) {}
void SpineGpuRenderer::RemoveWorldInstancesOfOwner(uint64_t) {}
void SpineGpuRenderer::AdvanceWorldTick() {}
void SpineGpuRenderer::RequestSetAnimationOn(uint32_t, int, const std::string&, bool) {}
void SpineGpuRenderer::RequestTimeScaleOn(uint32_t, float) {}
void SpineGpuRenderer::RequestSkinOn(uint32_t, const std::string&, bool) {}
void SpineGpuRenderer::Draw(RHI*, RHIRenderPass*, uint32_t, const RHIViewport&, ViewportType) {}
void SpineGpuRenderer::DrawWorld(RHI*, RHIRenderPass*, uint32_t, const RHIViewport&, ViewportType) {}
void SpineGpuRenderer::DrawLayer(int, RHI*, RHIRenderPass*, uint32_t, const RHIViewport&, ViewportType) {}
void SpineGpuRenderer::DrawInEditorViewport(RHI*, RHIRenderPass*, uint32_t) {}
uint32_t SpineGpuRenderer::LastDrawCount() const { return 0; }
std::string SpineGpuRenderer::Status() const { return "GPU-Spine rendering is not available on WebGL2"; }

#endif
