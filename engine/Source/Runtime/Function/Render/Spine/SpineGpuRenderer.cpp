#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#include "Runtime/Function/Render/Spine/SpineGpuClipper.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"
#include "Runtime/Function/Render/Spine/SpineGpuTintBlack.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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
    // 着色器里的两个 std430 结构，**逐字段**与 spine_gpu.vert / .frag 对齐。
    // 尺寸钉成 static_assert：改了一边没改另一边时是"能跑、读到别人的字段"，不报错。
    struct GpuDraw
    {
        uint32_t vertex_format;
        uint32_t uv_offset;
        uint32_t slot_bone;
        uint32_t bone_base;
        uint32_t inst_index;
        uint32_t tint;
        uint32_t flags;
        uint32_t weighted_base;
        uint32_t deform_base;   // bit2：b7 的回绕偏置；bit3：b8（CPU 裁剪顶点）里的起点（见 spine_gpu.vert）
        float dark[3];          // bit4：槽位暗色 rgb（float，不截断；见 SpineGpuTintBlack.h）
    };
    static_assert(sizeof(GpuDraw) == 48, "spine_gpu.vert SpineDraw is 12 x u32 (std430)");

    struct GpuInst
    {
        float clip_from_world[16];   // 列主序（GLSL mat4 默认）
        float root_abcd[4];
        float root_t[4];
    };
    static_assert(sizeof(GpuInst) == 96, "spine_gpu.vert SpineInst is mat4 + vec4 + vec4 (std430)");

    constexpr uint32_t kDrawFlagStraightAlpha = 1u;
    constexpr uint32_t kDrawFlagAdditive = 2u;
    constexpr uint32_t kDrawFlagDeform = 4u;
    constexpr uint32_t kDrawFlagCpuVerts = 8u;   // v15：CPU 裁剪过的项，顶点取 b8
    constexpr uint32_t kDrawFlagTintBlack = 16u; // tint black：片元加暗色项（spine_gpu.frag）

    void ApplyTintBlack(GpuDraw& g, const SpineGpuSlotSample& slot, bool tint_black_on, bool additive,
                        uint32_t& counter)
    {
        const SpineGpuTintBlackParams tb = SpineGpuMakeTintBlackParams(slot.dark, tint_black_on, additive);
        if (!tb.enabled)
        {
            return;
        }
        g.flags |= kDrawFlagTintBlack;
        g.dark[0] = tb.dark[0];
        g.dark[1] = tb.dark[1];
        g.dark[2] = tb.dark[2];
        ++counter;
    }
    constexpr uint32_t kSet0Bindings = 9;        // b0..b6 + b7 deform（S6）+ b8 CPU 裁剪顶点（v15）
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

    // CPU 裁剪顶点（b8）的一项：骨架空间位置 + UV，与 spine_gpu.vert 的 vec4 对齐
    struct GpuCpuVert
    {
        float x, y, u, v;
    };
    static_assert(sizeof(GpuCpuVert) == 16, "spine_gpu.vert cpu_verts is vec4");

    uint32_t PackRgba8(const float c[4])
    {
        const auto q = [](float v) {
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<uint32_t>(std::lround(v * 255.0f));
        };
        // unpackUnorm4x8：x 在最低字节
        return q(c[0]) | (q(c[1]) << 8u) | (q(c[2]) << 16u) | (q(c[3]) << 24u);
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
}  // namespace

#if !defined(__EMSCRIPTEN__)

struct SpineGpuRenderer::Impl
{
    // ---- 游戏线程 → 渲染线程 ----
    mutable std::mutex mutex;
    bool pending_load {false};
    bool pending_clear {false};
    std::string pending_path;
    int pending_anim {0};
    // 多轨播放命令（S5）：游戏线程攒、渲染线程在下一次 Draw 的 Update 之前按顺序执行
    struct AnimCommand
    {
        enum class Op
        {
            kSet,     // setAnimation(track, anim, loop)
            kAdd,     // addAnimation(track, anim, loop, delay)
            kEmpty,   // setEmptyAnimation(track, mix)
            kMix,     // AnimationStateData::default_mix = value
            kSpeed,   // AnimationState::time_scale = value
            kAlpha,   // 轨道当前那条的 alpha = value
        };
        Op op {Op::kSet};
        int track {0};
        std::string anim;   // 下标或名字
        bool loop {true};
        float value {0.0f};
        float delay {0.0f};
    };
    std::vector<AnimCommand> pending_cmds;
    float default_mix {0.2f};   // 渲染线程独占（spine.mix 经命令队列改）；跨资产保留
    std::string status {"idle"};
    std::string skip_reason;   // 最近一次"没画"的原因（变化时才记日志）
    uint32_t last_draw_count {0};
    uint32_t last_deform_draws {0};    // 上一帧带 deform 的绘制项数（S6）
    uint32_t last_deform_floats {0};   // 上一帧上传的 deform float 数
    uint32_t last_clipped_draws {0};   // 上一帧 CPU 裁剪过的项（v15）
    uint32_t last_clip_vertices {0};   // 上一帧裁剪输出的顶点数
    uint32_t last_tint_black_draws {0};   // 上一帧带暗色项的绘制项数
    // tint black 开关（= SpineSettings::tintBlack）。默认关：OSGame 线上全部是 0，且引擎分支
    // 根本不上传 uv2/uv3（见 SpineGpuTintBlack.h）。游戏线程写、渲染线程读，受 mutex 保护。
    bool tint_black {false};
    bool tint_black_logged {false};   // 每次加载 / 每次拨开关后只记一行"tint black active"
    std::string track_summary;         // 上一帧的轨道概况（渲染线程写、spine.status 读，受 mutex 保护）

    // ---- 管线（与资产无关）----
    RHI* rhi {nullptr};
    RHIRenderPass* pipeline_render_pass {nullptr};
    uint32_t pipeline_subpass {0};
    RHIDescriptorSetLayout* set0_layout {nullptr};
    RHIDescriptorSetLayout* set1_layout {nullptr};   // 图集页：binding 0 = combined image sampler（片元）
    RHIPipelineLayout* pipeline_layout {nullptr};
    RHIPipeline* pipelines[4] {nullptr, nullptr, nullptr, nullptr};   // 按 SpineBlendMode

    // ---- 资产 ----
    struct Buffer
    {
        RHIBuffer* buffer {nullptr};
        RHIDeviceMemory* memory {nullptr};
        RHIDeviceSize size {0};
    };
    struct FrameSlot
    {
        Buffer bones;
        Buffer draws;
        Buffer insts;
        Buffer deform;   // 本帧生效的 mesh deform（S6），容量 = 可能同时生效的上界
        // v15：CPU 裁剪的输出。裁剪会产生新三角形，量没有静态上界 ⇒ 按需扩容（旧缓冲走延迟销毁，
        // 只重写**本帧槽位**的 b8 —— 它上一次被 GPU 用是 frames.size() 帧之前，已经完成）。
        Buffer cpu_verts;   // GpuCpuVert[]
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
    struct Loaded
    {
        SpineGpuAsset asset;
        uint32_t anim {0};   // 加载时放到轨道 0 的动画
        // S5：跨帧存活的骨架状态 + 多轨调度。`anim_state` 引用 asset / data / mix ⇒ 必须声明在它们之后
        //     （析构逆序），且 Loaded 不可移动（unique_ptr 持有，从不拷贝）。
        SpineGpuRuntimeData data;
        SpineGpuSkeletonState state;
        SpineGpuAnimationStateData mix;
        std::unique_ptr<SpineGpuAnimationState> anim_state;
        std::chrono::steady_clock::time_point last_tick;
        // 每个绘制项的 deform 数组长度（float 个数；rigid 2×顶点、weighted 2×影响）与
        // weighted 项的 infl_base（第 0 个顶点的 influence_offset）—— GPU 寻址偏置要用
        std::vector<uint32_t> deform_len;
        std::vector<uint32_t> infl_base;
        size_t deform_capacity {0};   // float 个数
        Buffer vertex_words;
        Buffer infl_bind;
        Buffer infl_bone;
        Buffer uvs;
        Buffer index;
        // DX12 专用：u32 0,1,2,…（per-instance 顶点属性 = 绘制项下标；D3D12 的 SV_InstanceID 不含
        // StartInstanceLocation，见 spine_gpu.vert.hlsl 文件头）。其它后端为空。
        Buffer draw_ids;
        std::vector<Page> pages;
        std::vector<FrameSlot> frames;
        float bounds[4] {0, 0, 1, 1};       // setup 姿势的包围盒（x0, y0, x1, y1）
        // v15：CPU 裁剪（SpineGpuClipper）的暂存，跨帧复用
        SpineGpuSkeletonClipping clipper;
        SpineGpuClipFrameResult clip;
    };
    std::unique_ptr<Loaded> loaded;

    // 延迟销毁：GPU 可能还在用上一份资产的缓冲（最多领先 MaxFramesInFlight 帧）
    struct Retired
    {
        std::vector<Buffer> buffers;
        std::vector<Page> pages;
        uint32_t frames_left {0};
    };
    std::vector<Retired> retired;

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

    void WriteSet0(Loaded& ld, FrameSlot& f)
    {
        const Buffer* bufs[kSet0Bindings] = {&ld.vertex_words, &ld.infl_bind, &ld.infl_bone, &ld.uvs, &f.bones,
                                             &f.draws,         &f.insts,     &f.deform,    &f.cpu_verts};
        for (uint32_t i = 0; i < kSet0Bindings; ++i)
        {
            WriteSet0Binding(f.set0, i, *bufs[i]);
        }
    }

    void WriteSet1(Loaded& ld, FrameSlot& f)
    {
        for (size_t i = 0; i < ld.pages.size() && i < f.set1.size(); ++i)
        {
            RHIDescriptorImageInfo image {};
            image.sampler = ld.pages[i].sampler;
            image.imageView = ld.pages[i].view;
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
    void RefreshDescriptors(Loaded& ld, FrameSlot& f)
    {
        if (!IsDx12())
        {
            return;
        }
        WriteSet0(ld, f);
        WriteSet1(ld, f);
    }

    // 本帧槽位的某个缓冲容量不够 ⇒ 换一个更大的（旧的延迟销毁）。`binding` 非负时重写 set0 的那一格。
    bool EnsureFrameBuffer(FrameSlot& f, Buffer& b, RHIDeviceSize need, RHIBufferUsageFlags usage, int binding)
    {
        if (need <= b.size)
        {
            return true;
        }
        Retired r;
        r.frames_left = static_cast<uint32_t>(rhi->GetMaxFramesInFlight()) + 1u;
        r.buffers.push_back(b);
        retired.push_back(std::move(r));
        b = Buffer {};
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

    void RetireLoaded()
    {
        if (!loaded)
        {
            return;
        }
        Retired r;
        r.frames_left = static_cast<uint32_t>(rhi->GetMaxFramesInFlight()) + 1u;
        for (Buffer* b : {&loaded->vertex_words, &loaded->infl_bind, &loaded->infl_bone, &loaded->uvs, &loaded->index,
                          &loaded->draw_ids})
        {
            r.buffers.push_back(*b);
        }
        r.pages = loaded->pages;
        for (FrameSlot& f : loaded->frames)
        {
            r.buffers.push_back(f.bones);
            r.buffers.push_back(f.draws);
            r.buffers.push_back(f.insts);
            r.buffers.push_back(f.deform);
            r.buffers.push_back(f.cpu_verts);
            r.buffers.push_back(f.cpu_index);
        }
        // 描述符集留在共享池里（一次性的诊断入口，量很小），不在这里回收；图集贴图随缓冲延迟销毁。
        retired.push_back(std::move(r));
        loaded.reset();
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
            }
        }
        retired.erase(std::remove_if(retired.begin(), retired.end(),
                                     [](const Retired& r) { return r.buffers.empty() && r.pages.empty(); }),
                      retired.end());
    }

    // ------------------------------------------------------------------
    // 管线：set 0 = 9 个 SSBO，set 1 = 图集页（combined image sampler）；4 种混合各一条
    // ------------------------------------------------------------------
    bool EnsurePipelines(RHIRenderPass* render_pass, uint32_t subpass)
    {
        if (pipelines[0] != nullptr && pipeline_render_pass == render_pass && pipeline_subpass == subpass)
        {
            return true;
        }
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
            pipelines[mode] = nullptr;
            ok = rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pi, pipelines[mode]) && pipelines[mode] != nullptr;
        }
        rhi->DestroyShaderModule(vs);
        rhi->DestroyShaderModule(fs);
        if (!ok)
        {
            LOG_ERROR(ZRender, "SpineGpuRenderer: CreateGraphicsPipelines failed");
            return false;
        }
        pipeline_render_pass = render_pass;
        pipeline_subpass = subpass;
        return true;
    }

    // ------------------------------------------------------------------
    // 多轨命令（渲染线程）
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

    void RunCommands(std::vector<AnimCommand>& cmds)
    {
        for (const AnimCommand& c : cmds)
        {
            if (c.op == AnimCommand::Op::kMix)
            {
                default_mix = std::max(0.0f, c.value);
                if (loaded)
                {
                    loaded->mix.default_mix = default_mix;
                }
                LOG_INFO(ZRender, "SpineGpuRenderer: default mix = {}s", default_mix);
                continue;
            }
            if (!loaded)
            {
                LOG_WARNING(ZRender, "SpineGpuRenderer: no skeleton loaded, animation command dropped");
                continue;
            }
            SpineGpuAnimationState& as = *loaded->anim_state;
            const size_t track = static_cast<size_t>(std::max(0, c.track));
            switch (c.op)
            {
                case AnimCommand::Op::kSet:
                case AnimCommand::Op::kAdd:
                {
                    uint32_t ai = 0;
                    if (!ResolveAnim(loaded->asset, c.anim, ai))
                    {
                        LOG_WARNING(ZRender, "SpineGpuRenderer: animation '{}' not found ({} animations)", c.anim,
                                    loaded->asset.anims.size());
                        break;
                    }
                    if (c.op == AnimCommand::Op::kSet)
                    {
                        as.SetAnimation(track, ai, c.loop);
                    }
                    else
                    {
                        as.AddAnimation(track, ai, c.loop, c.delay);
                    }
                    break;
                }
                case AnimCommand::Op::kEmpty:
                    as.SetEmptyAnimation(track, std::max(0.0f, c.value));
                    break;
                case AnimCommand::Op::kSpeed:
                    as.time_scale = c.value;
                    break;
                case AnimCommand::Op::kAlpha:
                    if (SpineGpuTrackEntry* e = as.GetCurrent(track))
                    {
                        e->alpha = std::clamp(c.value, 0.0f, 1.0f);
                    }
                    break;
                case AnimCommand::Op::kMix:
                    break;
            }
        }
        cmds.clear();
    }

    // "轨道 0：'walk' t=1.23（混出 'idle' 40%）；轨道 1：…"
    std::string DescribeTracks() const
    {
        if (!loaded)
        {
            return std::string();
        }
        const SpineGpuAnimationState& as = *loaded->anim_state;
        const auto name = [&](uint32_t ai) -> std::string {
            if (ai == kSpineGpuEmptyAnimation)
            {
                return "<empty>";
            }
            return ai < loaded->asset.anim_names.size() ? loaded->asset.anim_names[ai] : std::string("?");
        };
        std::string out;
        for (size_t i = 0; i < as.TrackCount(); ++i)
        {
            const SpineGpuTrackEntry* e = as.GetCurrent(i);
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
        return out.empty() ? std::string("no tracks") : out;
    }

    // ------------------------------------------------------------------
    // 加载：骨架 + 图集 → 烘焙 → **容器往返**（与运行时读的是同一份字节）→ GPU
    // ------------------------------------------------------------------
    bool Load(const std::string& path, int anim, std::string& err)
    {
        namespace fs = std::filesystem;
        const fs::path skel_path = FromUtf8(path);
        std::vector<uint8_t> skel_bytes;
        if (!ReadWholeFile(skel_path, skel_bytes))
        {
            err = "读不到骨架文件 " + path;
            return false;
        }
        const std::string fname = ToUtf8(skel_path.filename());
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
        auto ld = std::make_unique<Loaded>();
        if (r.ok)
        {
            r = ReadSpineGpuContainer(container.data(), container.size(), ld->asset);
        }
        if (!r.ok)
        {
            err = r.error;
            return false;
        }
        SpineGpuAsset& a = ld->asset;
        if (a.anims.empty())
        {
            err = "骨架没有动画";
            return false;
        }
        ld->anim = static_cast<uint32_t>(std::clamp(anim, 0, static_cast<int>(a.anims.size()) - 1));
        // deform 长度 / infl_base：每个 skin 0 绘制项一份（解算器给的 deform 数组按附件本地顺序，
        // 上传时要换算成 shader 的寻址偏置）。容量上界 = 所有 kFallback 项长度之和 ——
        // 只有被 deform 时间轴命中过的项才是 kFallback，同一帧最多它们全部生效。
        ld->deform_len.assign(a.draw_items.size(), 0u);
        ld->infl_base.assign(a.draw_items.size(), 0u);
        for (uint32_t i = 0; i < a.draw_items.size(); ++i)
        {
            const SpineDrawItem& di = a.draw_items[i];
            if (di.vertex_format == SpineVertexFormat::kRigid)
            {
                ld->deform_len[i] = 2u * di.vertex_count;
            }
            else
            {
                const size_t w0 = a.weighted_segment_offset / 4u + static_cast<size_t>(di.vertex_base) * 2u;
                uint32_t n = 0;
                for (uint32_t v = 0; v < di.vertex_count; ++v)
                {
                    uint32_t cnt = 0;
                    std::memcpy(&cnt, a.vertex_buffer.data() + (w0 + v * 2u + 1u) * 4u, 4);
                    n += cnt;
                }
                ld->deform_len[i] = 2u * n;
                if (di.vertex_count > 0)
                {
                    std::memcpy(&ld->infl_base[i], a.vertex_buffer.data() + w0 * 4u, 4);
                }
            }
            if (di.kind == SpineDrawItemKind::kFallback && di.skin_id == SpineGpuConstants::kDefaultSkinId)
            {
                ld->deform_capacity += ld->deform_len[i];
            }
        }

        // ---- 静态缓冲：每个资产一份，所有实例共用 ----
        const RHIBufferUsageFlags ssbo = RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bool ok = CreateBuffer(ld->vertex_words, a.vertex_buffer.size(), ssbo, a.vertex_buffer.data()) &&
                  CreateBuffer(ld->infl_bind, a.influence_bind.size() * sizeof(float), ssbo, a.influence_bind.data()) &&
                  CreateBuffer(ld->infl_bone, a.influence_bone.size() * sizeof(SpineInfluenceBone), ssbo,
                               a.influence_bone.data()) &&
                  CreateBuffer(ld->uvs, a.uv_buffer.size() * sizeof(float), ssbo, a.uv_buffer.data()) &&
                  CreateBuffer(ld->index, a.index_buffer.size() * sizeof(uint16_t), RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               a.index_buffer.data());
        if (!ok)
        {
            err = "创建静态缓冲失败";
            return false;
        }

        // ---- 图集页 ----
        // 失败时把已建的页交给延迟销毁（`ld` 还没发布，GPU 没用过它们，下一帧就会销毁）
        struct PageGuard
        {
            Impl& impl;
            Loaded& l;
            bool released {false};
            ~PageGuard()
            {
                if (!released && !l.pages.empty())
                {
                    Retired r;
                    r.pages = l.pages;
                    r.frames_left = 1;
                    impl.retired.push_back(std::move(r));
                    l.pages.clear();
                }
            }
        } page_guard {*this, *ld};
        for (const std::string& page : a.atlas_page_names)
        {
            const fs::path png = atlas_path.parent_path() / FromUtf8(page);
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
                return false;
            }
            Page pg;
            rhi->CreateGlobalImage(pg.image, pg.view, nullptr, static_cast<uint32_t>(w), static_cast<uint32_t>(h), pixels,
                                   RHI_FORMAT_R8G8B8A8_UNORM, 1);
            stbi_image_free(pixels);
            pg.sampler = rhi->GetOrCreateDefaultSampler(RHIDefaultSamplerType::Default_Sampler_Linear);
            if (pg.image == nullptr || pg.view == nullptr || pg.sampler == nullptr)
            {
                ld->pages.push_back(pg);   // 已建的部分交给 PageGuard
                err = "上传图集页失败 " + ToUtf8(png);
                return false;
            }
            ld->pages.push_back(pg);
        }
        if (ld->pages.empty())
        {
            err = "图集没有页";
            return false;
        }

        // ---- 每帧缓冲 + set 0 ----
        const uint32_t frames = std::max<uint32_t>(1u, rhi->GetMaxFramesInFlight());
        ld->frames.resize(frames);
        for (FrameSlot& f : ld->frames)
        {
            ok = CreateBuffer(f.bones, static_cast<RHIDeviceSize>(a.header.bone_count) * sizeof(SpineBoneTransform), ssbo,
                              nullptr) &&
                 CreateBuffer(f.draws, static_cast<RHIDeviceSize>(std::max<size_t>(a.draw_items.size(), 1)) * sizeof(GpuDraw),
                              ssbo, nullptr) &&
                 CreateBuffer(f.insts, sizeof(GpuInst), ssbo, nullptr) &&
                 CreateBuffer(f.deform, static_cast<RHIDeviceSize>(std::max<size_t>(ld->deform_capacity, 1)) * sizeof(float),
                              ssbo, nullptr) &&
                 // 初始容量给一个小值：没有裁剪的资产永远用不到；有裁剪的第一帧按需扩
                 CreateBuffer(f.cpu_verts, (a.clip_polygons.empty() ? 1u : 4096u) * sizeof(GpuCpuVert), ssbo, nullptr) &&
                 CreateBuffer(f.cpu_index, (a.clip_polygons.empty() ? 2u : 8192u) * sizeof(uint16_t),
                              RHI_BUFFER_USAGE_INDEX_BUFFER_BIT, nullptr);
            if (!ok)
            {
                err = "创建每帧缓冲失败";
                return false;
            }
            RHIDescriptorSetAllocateInfo alloc {};
            alloc.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool = rhi->GetDescriptorPoor();
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &set0_layout;
            if (!rhi->AllocateDescriptorSets(&alloc, f.set0) || f.set0 == nullptr)
            {
                err = "分配描述符集失败（共享池可能满了）";
                return false;
            }
            f.set1.assign(ld->pages.size(), nullptr);
            for (RHIDescriptorSet*& s1 : f.set1)
            {
                RHIDescriptorSetAllocateInfo a1 = alloc;
                a1.pSetLayouts = &set1_layout;
                if (!rhi->AllocateDescriptorSets(&a1, s1) || s1 == nullptr)
                {
                    err = "分配图集页描述符集失败（共享池可能满了）";
                    return false;
                }
            }
            WriteSet0(*ld, f);
            WriteSet1(*ld, f);
        }
        if (IsDx12())
        {
            std::vector<uint32_t> ids(std::max<size_t>(a.draw_items.size(), 1));
            for (uint32_t i = 0; i < ids.size(); ++i)
            {
                ids[i] = i;
            }
            if (!CreateBuffer(ld->draw_ids, ids.size() * sizeof(uint32_t), RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              ids.data()))
            {
                err = "创建绘制项下标顶点缓冲失败";
                return false;
            }
        }

        // ---- setup 姿势（第 0 帧）的包围盒：CPU 按 §6 同一公式蒙一遍，只为自动居中 ----
        {
            SpineGpuPoseResult pose;
            const SpineImportResult pr = SpineGpuSamplePose(a, ld->anim, 0.0f, pose);
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
                if (di.skin_id != SpineGpuConstants::kDefaultSkinId)
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
            ld->bounds[0] = x0;
            ld->bounds[1] = y0;
            ld->bounds[2] = x1;
            ld->bounds[3] = y1;
        }

        // ---- 运行时状态（S5）：查找表一次建好；轨道 0 循环播放请求的动画 ----
        {
            SpineImportResult rr = SpineGpuBuildRuntimeData(a, ld->data);
            if (rr.ok)
            {
                rr = SpineGpuInitSkeletonState(a, ld->data, ld->state);
            }
            if (!rr.ok)
            {
                err = rr.error;
                return false;
            }
            ld->mix.default_mix = default_mix;
            ld->anim_state = std::make_unique<SpineGpuAnimationState>(ld->asset, ld->data, &ld->mix);
            ld->anim_state->SetAnimation(0, ld->anim, true);
        }
        ld->last_tick = std::chrono::steady_clock::now();
        page_guard.released = true;
        RetireLoaded();
        loaded = std::move(ld);
        return true;
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
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.pending_load = true;
    d.pending_clear = false;
    d.pending_path = skeleton_path;
    d.pending_anim = anim_index;
    d.status = "loading " + skeleton_path;
}

void SpineGpuRenderer::RequestClear()
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.pending_clear = true;
    d.pending_load = false;
    d.track_summary.clear();
}

void SpineGpuRenderer::RequestSetAnimation(int track, const std::string& anim, bool loop)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::AnimCommand c;
    c.op = Impl::AnimCommand::Op::kSet;
    c.track = track;
    c.anim = anim;
    c.loop = loop;
    d.pending_cmds.push_back(std::move(c));
}

void SpineGpuRenderer::RequestAddAnimation(int track, const std::string& anim, bool loop, float delay)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::AnimCommand c;
    c.op = Impl::AnimCommand::Op::kAdd;
    c.track = track;
    c.anim = anim;
    c.loop = loop;
    c.delay = delay;
    d.pending_cmds.push_back(std::move(c));
}

void SpineGpuRenderer::RequestSetEmptyAnimation(int track, float mix_duration)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::AnimCommand c;
    c.op = Impl::AnimCommand::Op::kEmpty;
    c.track = track;
    c.value = mix_duration;
    d.pending_cmds.push_back(std::move(c));
}

void SpineGpuRenderer::RequestDefaultMix(float seconds)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::AnimCommand c;
    c.op = Impl::AnimCommand::Op::kMix;
    c.value = seconds;
    d.pending_cmds.push_back(std::move(c));
}

void SpineGpuRenderer::RequestTimeScale(float scale)
{
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    Impl::AnimCommand c;
    c.op = Impl::AnimCommand::Op::kSpeed;
    c.value = scale;
    d.pending_cmds.push_back(std::move(c));
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
    Impl::AnimCommand c;
    c.op = Impl::AnimCommand::Op::kAlpha;
    c.track = track;
    c.value = alpha;
    d.pending_cmds.push_back(std::move(c));
}

uint32_t SpineGpuRenderer::LastDrawCount() const
{
    if (m_Impl == nullptr)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->last_draw_count;
}

std::string SpineGpuRenderer::Status() const
{
    if (m_Impl == nullptr)
    {
        return "idle";
    }
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->status + (m_Impl->track_summary.empty() ? std::string() : " [" + m_Impl->track_summary + "]") +
           " (last frame: " + std::to_string(m_Impl->last_draw_count) + " draw items, " +
           std::to_string(m_Impl->last_deform_draws) + " deformed / " + std::to_string(m_Impl->last_deform_floats) +
           " deform floats, " + std::to_string(m_Impl->last_clipped_draws) + " clipped / " +
           std::to_string(m_Impl->last_clip_vertices) + " clip vertices, tint black " +
           (m_Impl->tint_black ? "on: " + std::to_string(m_Impl->last_tint_black_draws) + " dark draws" : std::string("off")) +
           (m_Impl->skip_reason.empty() ? std::string() : "; skipped: " + m_Impl->skip_reason) + ")";
}

void SpineGpuRenderer::Draw(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport)
{
    if (rhi == nullptr || render_pass == nullptr)
    {
        return;
    }
    Impl& d = *m_Impl;
    {
        // 从没人 RequestLoad 过：一次加锁 + 两个判断，不碰 GPU
        std::lock_guard<std::mutex> lock(d.mutex);
        if (!d.pending_load && !d.pending_clear && !d.loaded && d.retired.empty())
        {
            return;
        }
    }
    d.rhi = rhi;
    d.TickRetired();

    // ---- 处理游戏线程的请求 ----
    std::string path;
    int anim = 0;
    bool do_load = false;
    bool do_clear = false;
    std::vector<Impl::AnimCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        do_load = d.pending_load;
        do_clear = d.pending_clear;
        path = d.pending_path;
        anim = d.pending_anim;
        d.pending_load = false;
        d.pending_clear = false;
        cmds.swap(d.pending_cmds);
    }
    if (do_clear)
    {
        d.RetireLoaded();
        std::lock_guard<std::mutex> lock(d.mutex);
        d.status = "idle";
    }
    if (do_load)
    {
        std::string err;
        const bool ok = d.EnsurePipelines(render_pass, subpass) && d.Load(path, anim, err);
        std::lock_guard<std::mutex> lock(d.mutex);
        d.last_draw_count = 0;
        d.tint_black_logged = false;
        if (ok)
        {
            const SpineGpuAsset& a = d.loaded->asset;
            d.status = "playing '" + path + "' anim #" + std::to_string(d.loaded->anim) + " '" +
                       a.anim_names[d.loaded->anim] + "' (" + std::to_string(a.header.bone_count) + " bones, " +
                       std::to_string(a.draw_items.size()) + " draw items)";
            LOG_INFO(ZRender, "SpineGpuRenderer: {}", d.status);
        }
        else
        {
            d.status = "load failed: " + err;
            LOG_ERROR(ZRender, "SpineGpuRenderer: load '{}' failed: {}", path, err);
        }
    }
    // 命令在加载**之后**执行：同一帧里先 spine.load 再 spine.play 时，play 作用在新资产上
    if (!cmds.empty())
    {
        d.RunCommands(cmds);
    }
    // 每条"这一帧没画"的原因都进 status，且**变化时**记一行日志（不刷屏、也不静默）
    const auto skip = [&d](const std::string& why) {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.skip_reason != why)
        {
            d.skip_reason = why;
            LOG_WARNING(ZRender, "SpineGpuRenderer: skipping frame: {}", why);
        }
        d.last_draw_count = 0;
    };
    if (!d.loaded)
    {
        return;
    }
    if (viewport.width <= 0.0f || viewport.height <= 0.0f)
    {
        skip("scene viewport is empty (" + std::to_string(viewport.width) + "x" + std::to_string(viewport.height) + ")");
        return;
    }
    if (!d.EnsurePipelines(render_pass, subpass))
    {
        skip("pipelines are not ready");
        return;
    }

    Impl::Loaded& ld = *d.loaded;
    const SpineGpuAsset& a = ld.asset;

    // ---- 1) 解算（CPU 只算骨骼与槽位，不算顶点）----
    // S5：AnimationState 的标准三步 update(dt) → apply(skeleton) → updateWorldTransform。
    // dt 夹到 [0, 0.25]：编辑器卡顿 / 断点之后不让动画一步跳过好几圈（参考实现没有这层，
    // 是宿主的事；spine-unity 也是由调用方给 deltaTime）。
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - ld.last_tick).count(), 0.0f, 0.25f);
    ld.last_tick = now;
    ld.anim_state->Update(dt);
    SpineImportResult pr = ld.anim_state->Apply(ld.state);
    SpineGpuPoseResult pose;
    if (pr.ok)
    {
        pr = SpineGpuUpdateWorld(a, ld.data, ld.state, pose);
    }
    if (!pr.ok)
    {
        skip("solve failed: " + pr.error);
        return;
    }

    // ---- 2) 可见绘制项：按绘制顺序、取槽位当前附件对应的那一项（解算器给的 slot_draw_item）----
    //         + 本帧生效的 mesh deform（S6）：拼进一块连续数组，每项记下段起点
    std::vector<float> deform_values;
    std::vector<int64_t> deform_region(a.draw_items.size(), -1);   // 绘制项 → 段起点（float 下标）
    for (const SpineGpuMeshDeform& md : pose.mesh_deforms)
    {
        if (md.draw_item >= a.draw_items.size() || md.values.size() != ld.deform_len[md.draw_item])
        {
            skip("deform of draw item " + std::to_string(md.draw_item) + " has " + std::to_string(md.values.size()) +
                 " floats, expected " +
                 (md.draw_item < a.draw_items.size() ? std::to_string(ld.deform_len[md.draw_item]) : std::string("?")));
            return;
        }
        deform_region[md.draw_item] = static_cast<int64_t>(deform_values.size());
        deform_values.insert(deform_values.end(), md.values.begin(), md.values.end());
    }
    if (deform_values.size() > ld.deform_capacity)
    {
        // 上界是"所有 kFallback 项同时生效"；超出说明有非 fallback 的项被 deform 了 —— 容器的 kind 与
        // 解算器对不上，按静态画就是错形变，所以宁可这一帧不画、把原因报出来。
        skip("deform needs " + std::to_string(deform_values.size()) + " floats, capacity " +
             std::to_string(ld.deform_capacity));
        return;
    }

    // ---- 2b) 裁剪（v15）：按当前绘制顺序跑 SpineMesh.h 的状态机，作用域内的项在 CPU 上蒙皮 + 裁 ----
    {
        const SpineImportResult cr = SpineGpuClipFrame(a, ld.state, pose, ld.clipper, ld.clip);
        if (!cr.ok)
        {
            skip("clip failed: " + cr.error);
            return;
        }
    }
    const SpineGpuClipFrameResult& clip = ld.clip;

    // 每个绘制命令：静态项按容器的索引区间画；裁剪过的项按动态索引缓冲画（局部下标，b8 起点放在 deform_base）
    struct DrawCmd
    {
        uint32_t item;
        bool cpu;
        uint32_t index_first;
        uint32_t index_count;
    };
    std::vector<GpuDraw> draws;
    std::vector<DrawCmd> cmds_out;   // 与 draws 一一对应
    draws.reserve(a.draw_items.size());
    uint32_t deformed_draws = 0;
    uint32_t clipped_draws = 0;
    uint32_t tint_black_draws = 0;
    bool tint_black_on = false;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        tint_black_on = d.tint_black;
    }
    for (uint32_t s : pose.draw_order)
    {
        if (s >= pose.slot_draw_item.size() || pose.slot_draw_item[s] == SpineGpuPoseResult::kNoDrawItem)
        {
            continue;
        }
        const uint32_t item = pose.slot_draw_item[s];
        const SpineGpuSlotSample& slot = pose.slots[s];
        const SpineDrawItem& di = a.draw_items[item];
        const uint32_t ck = s < clip.slot_clipped.size() ? clip.slot_clipped[s] : SpineGpuClipFrameResult::kNotClipped;
        if (ck != SpineGpuClipFrameResult::kNotClipped)
        {
            const SpineGpuClippedDraw& cd = clip.draws[ck];
            if (cd.index_count == 0)
            {
                continue;   // 整个被裁掉
            }
            GpuDraw g {};
            g.vertex_format = static_cast<uint32_t>(di.vertex_format);
            g.tint = PackRgba8(slot.color);
            g.flags = kDrawFlagStraightAlpha | kDrawFlagCpuVerts |
                      (di.blend_mode == SpineBlendMode::kAdditive ? kDrawFlagAdditive : 0u);
            g.deform_base = cd.vertex_first;
            ApplyTintBlack(g, slot, tint_black_on, di.blend_mode == SpineBlendMode::kAdditive, tint_black_draws);
            draws.push_back(g);
            cmds_out.push_back({item, true, cd.index_first, cd.index_count});
            ++clipped_draws;
            continue;
        }
        GpuDraw g {};
        g.vertex_format = static_cast<uint32_t>(di.vertex_format);
        g.uv_offset = di.uv_offset;
        g.slot_bone = di.slot_index < a.slot_bone.size() ? a.slot_bone[di.slot_index] : 0u;
        g.bone_base = 0;
        g.inst_index = 0;
        g.tint = PackRgba8(slot.color);
        g.flags = kDrawFlagStraightAlpha | (di.blend_mode == SpineBlendMode::kAdditive ? kDrawFlagAdditive : 0u);
        g.weighted_base = a.weighted_segment_offset / 4u;
        ApplyTintBlack(g, slot, tint_black_on, di.blend_mode == SpineBlendMode::kAdditive, tint_black_draws);
        if (deform_region[item] >= 0)
        {
            // uint 回绕偏置：shader 用段内绝对下标（rigid）/ 全局影响下标（weighted）直接加
            const uint32_t region = static_cast<uint32_t>(deform_region[item]);
            g.deform_base = di.vertex_format == SpineVertexFormat::kRigid
                                ? region - 2u * static_cast<uint32_t>(di.vertex_base)
                                : region - 2u * ld.infl_base[item];
            g.flags |= kDrawFlagDeform;
            ++deformed_draws;
        }
        draws.push_back(g);
        cmds_out.push_back({item, false, di.index_offset, di.index_count});
    }

    // ---- 3) 上传：骨骼 / 绘制项 / 实例（本帧槽位）----
    const uint32_t slot_index = static_cast<uint32_t>(rhi->GetCurrentFrameIndex()) % static_cast<uint32_t>(ld.frames.size());
    Impl::FrameSlot& fs = ld.frames[slot_index];
    {
        void* dst = nullptr;
        if (rhi->MapMemory(fs.bones.memory, 0, fs.bones.size, 0, &dst) && dst != nullptr)
        {
            static_assert(sizeof(SpineGpuBoneSample) == sizeof(SpineBoneTransform), "bone sample is the GPU bone");
            std::memcpy(dst, pose.bones.data(), pose.bones.size() * sizeof(SpineGpuBoneSample));
            rhi->UnmapMemory(fs.bones.memory);
        }
        if (!draws.empty() && rhi->MapMemory(fs.draws.memory, 0, fs.draws.size, 0, &dst) && dst != nullptr)
        {
            std::memcpy(dst, draws.data(), draws.size() * sizeof(GpuDraw));
            rhi->UnmapMemory(fs.draws.memory);
        }
        if (!deform_values.empty() && rhi->MapMemory(fs.deform.memory, 0, fs.deform.size, 0, &dst) && dst != nullptr)
        {
            std::memcpy(dst, deform_values.data(), deform_values.size() * sizeof(float));
            rhi->UnmapMemory(fs.deform.memory);
        }
        if (clipped_draws > 0)
        {
            const size_t nv = clip.xy.size() / 2u;
            if (!d.EnsureFrameBuffer(fs, fs.cpu_verts, static_cast<RHIDeviceSize>(nv * sizeof(GpuCpuVert)),
                                     RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT, static_cast<int>(kCpuVertBinding)) ||
                !d.EnsureFrameBuffer(fs, fs.cpu_index, static_cast<RHIDeviceSize>(clip.indices.size() * sizeof(uint16_t)),
                                     RHI_BUFFER_USAGE_INDEX_BUFFER_BIT, -1))
            {
                skip("could not grow the clipping buffers");
                return;
            }
            if (rhi->MapMemory(fs.cpu_verts.memory, 0, fs.cpu_verts.size, 0, &dst) && dst != nullptr)
            {
                GpuCpuVert* cv = static_cast<GpuCpuVert*>(dst);
                for (size_t v = 0; v < nv; ++v)
                {
                    cv[v] = {clip.xy[v * 2u], clip.xy[v * 2u + 1u], clip.uv[v * 2u], clip.uv[v * 2u + 1u]};
                }
                rhi->UnmapMemory(fs.cpu_verts.memory);
            }
            if (rhi->MapMemory(fs.cpu_index.memory, 0, fs.cpu_index.size, 0, &dst) && dst != nullptr)
            {
                std::memcpy(dst, clip.indices.data(), clip.indices.size() * sizeof(uint16_t));
                rhi->UnmapMemory(fs.cpu_index.memory);
            }
        }
        // 骨架空间（y 向上，1 单位 = 1 像素）→ 视口：setup 包围盒居中、占视口 80%
        GpuInst inst {};
        const float bw = std::max(ld.bounds[2] - ld.bounds[0], 1.0f);
        const float bh = std::max(ld.bounds[3] - ld.bounds[1], 1.0f);
        const float fit = 0.8f * std::min(viewport.width / bw, viewport.height / bh);   // 像素 / 单位
        const float sx = 2.0f * fit / viewport.width;
        const float sy = ClipSpaceYSign(rhi) * 2.0f * fit / viewport.height;   // Vulkan NDC y 向下，DX12 / Metal 向上
        const float cx = 0.5f * (ld.bounds[0] + ld.bounds[2]);
        const float cy = 0.5f * (ld.bounds[1] + ld.bounds[3]);
        inst.clip_from_world[0] = sx;
        inst.clip_from_world[5] = sy;
        inst.clip_from_world[10] = 1.0f;
        inst.clip_from_world[12] = -cx * sx;
        inst.clip_from_world[13] = -cy * sy;
        inst.clip_from_world[15] = 1.0f;
        inst.root_abcd[0] = 1.0f;   // 根变换：单位阵（UI 摆放位置交给 clip_from_world）
        inst.root_abcd[3] = 1.0f;
        if (rhi->MapMemory(fs.insts.memory, 0, fs.insts.size, 0, &dst) && dst != nullptr)
        {
            std::memcpy(dst, &inst, sizeof(inst));
            rhi->UnmapMemory(fs.insts.memory);
        }
    }

    // ---- 4) 录制：一项一次 DrawIndexed，firstInstance = 绘制项在 b5 里的下标 ----
    d.RefreshDescriptors(ld, fs);
    RHICommandBuffer* cmd = rhi->GetCurrentCommandBuffer();
    float color[4] = {0.9f, 0.6f, 1.0f, 1.0f};
    rhi->PushEvent(cmd, "SpineGpu", color);
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
    int bound_index = -1;   // 0 = 静态索引缓冲，1 = 本帧 CPU 裁剪的动态索引缓冲
    int bound_mode = -1;
    int bound_page = -1;
    const bool dx12 = d.IsDx12();
    for (uint32_t i = 0; i < draws.size(); ++i)
    {
        const DrawCmd& dc = cmds_out[i];
        const SpineDrawItem& di = a.draw_items[dc.item];
        const int want_index = dc.cpu ? 1 : 0;
        if (want_index != bound_index)
        {
            rhi->CmdBindIndexBufferPFN(cmd, dc.cpu ? fs.cpu_index.buffer : ld.index.buffer, 0, RHI_INDEX_TYPE_UINT16);
            bound_index = want_index;
        }
        const int mode = static_cast<int>(di.blend_mode) & 3;
        if (mode != bound_mode)
        {
            rhi->CmdBindPipelinePFN(cmd, RHI_PIPELINE_BIND_POINT_GRAPHICS, d.pipelines[mode]);
            if (dx12)
            {
                // DX12RHI 的顶点缓冲 stride 取自**当前**管线 ⇒ 必须在绑管线之后绑
                RHIBuffer* id_buf = ld.draw_ids.buffer;
                const RHIDeviceSize zero = 0;
                rhi->CmdBindVertexBuffersPFN(cmd, 0, 1, &id_buf, &zero);
            }
            bound_mode = mode;
            bound_page = -1;
        }
        const int page = di.atlas_page < ld.pages.size() ? static_cast<int>(di.atlas_page) : 0;
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
    rhi->PopEvent(cmd);

    std::lock_guard<std::mutex> lock(d.mutex);
    if (draws.empty())
    {
        if (d.skip_reason != "no visible draw items")
        {
            d.skip_reason = "no visible draw items";
            LOG_WARNING(ZRender, "SpineGpuRenderer: skipping frame: no visible draw items ({})", d.DescribeTracks());
        }
    }
    else if (d.last_draw_count == 0)
    {
        // 每次加载后第一帧真正录了东西时记一行：视口 / 包围盒 / 项数，排查"画了但看不见"
        LOG_INFO(ZRender,
                 "SpineGpuRenderer: first frame {} draw items ({} deformed, {} deform floats, capacity {}), "
                 "viewport ({}, {}) {}x{}, bounds ({}, {})-({}, {})",
                 draws.size(), deformed_draws, deform_values.size(), ld.deform_capacity, viewport.x, viewport.y,
                 viewport.width, viewport.height, ld.bounds[0], ld.bounds[1], ld.bounds[2], ld.bounds[3]);
        d.skip_reason.clear();
    }
    d.last_draw_count = static_cast<uint32_t>(draws.size());
    d.track_summary = d.DescribeTracks();
    d.last_deform_draws = deformed_draws;
    d.last_deform_floats = static_cast<uint32_t>(deform_values.size());
    if (clipped_draws > 0 && d.last_clipped_draws == 0)
    {
        // 裁剪从"没有"变成"有"时记一行（每个资产 / 每段动画一次，不刷屏）
        LOG_INFO(ZRender, "SpineGpuRenderer: clipping active: {} clipped draw items, {} clip vertices, {} indices, "
                          "{} clipStart / {} convex pieces",
                 clipped_draws, clip.xy.size() / 2u, clip.indices.size(), clip.clip_starts, clip.clip_pieces);
    }
    d.last_clipped_draws = clipped_draws;
    if (tint_black_draws > 0 && !d.tint_black_logged)
    {
        // 每次加载 / 拨开关后第一次出现暗色项时记一行（动画循环里暗色时有时无，按"0→非 0"记会刷屏）
        d.tint_black_logged = true;
        LOG_INFO(ZRender, "SpineGpuRenderer: tint black active: {} of {} draw items carry a dark color",
                 tint_black_draws, draws.size());
    }
    d.last_tint_black_draws = tint_black_draws;
    d.last_clip_vertices = static_cast<uint32_t>(clip.xy.size() / 2u);
}

void SpineGpuRenderer::DrawInEditorViewport(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass)
{
    if (rhi == nullptr)
    {
        return;
    }
    const RHIViewport* scene = rhi->GetViewport(ViewportType::scene);
    const RHIViewport* vp =
        (scene != nullptr && scene->width > 0.0f && scene->height > 0.0f) ? scene : rhi->GetViewport(ViewportType::game);
    if (vp == nullptr)
    {
        return;
    }
    Draw(rhi, render_pass, subpass, *vp);
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
void SpineGpuRenderer::RequestClear() {}
void SpineGpuRenderer::RequestSetAnimation(int, const std::string&, bool) {}
void SpineGpuRenderer::RequestAddAnimation(int, const std::string&, bool, float) {}
void SpineGpuRenderer::RequestSetEmptyAnimation(int, float) {}
void SpineGpuRenderer::RequestDefaultMix(float) {}
void SpineGpuRenderer::RequestTimeScale(float) {}
void SpineGpuRenderer::RequestTrackAlpha(int, float) {}
void SpineGpuRenderer::RequestTintBlack(bool) {}
void SpineGpuRenderer::Draw(RHI*, RHIRenderPass*, uint32_t, const RHIViewport&) {}
void SpineGpuRenderer::DrawInEditorViewport(RHI*, RHIRenderPass*, uint32_t) {}
uint32_t SpineGpuRenderer::LastDrawCount() const { return 0; }
std::string SpineGpuRenderer::Status() const { return "GPU-Spine rendering is not available on WebGL2"; }

#endif
