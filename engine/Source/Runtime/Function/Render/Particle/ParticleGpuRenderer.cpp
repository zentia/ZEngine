#include "ParticleGpuRenderer.h"

#include "GPUParticle/ParticleGpuEmitter.h"

#include "core/Log/LogSystem.h"   // LOG_ERROR(ZRender, ...) —— 与 SpineGpuRenderer.cpp:14 同一条路径
#include "Runtime/Core/Base/Macro.h"             // LOG_ERROR（与 SpineGpuRenderer.cpp:5 同款）
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderSystem.h"   // ZRender 日志分类
#include "Runtime/Function/Render/RenderType.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// =====================================================================
// 局部类型与常量
// =====================================================================
namespace
{
// 与 SpineGpuRenderer.cpp:256 同款（RHI 里没有公共的 Buffer 类型，各渲染器自己持有一份）。
struct Buffer
{
    RHIBuffer* buffer {nullptr};
    RHIDeviceMemory* memory {nullptr};
    RHIDeviceSize size {0};
};

// 共享静态几何的上限：64 K quad = 256 K 顶点 / 384 K 索引，够所有发射器共用。
constexpr uint32_t kMaxQuads = 65536u;
constexpr uint32_t kQuadVerts = 4u;
constexpr uint32_t kQuadIndices = 6u;
constexpr RHIDeviceSize kQuadVertexSize = 8u;   // float2 角标

// set 0：b0 = 出生记录（环形，整条上传），b1 = 本绘制的常量。
constexpr uint32_t kSet0Bindings = 3u;
constexpr uint32_t kBindingSpawns = 0u;   // StructuredBuffer<Spawn>
constexpr uint32_t kBindingConsts = 1u;   // StructuredBuffer<DrawConsts>
constexpr uint32_t kBindingEmitter = 2u;  // ByteAddressBuffer：desc + 曲线 + 渐变

bool IsDx12()
{
#if defined(Z_GRAPHICS_API_DX12)
    return true;
#else
    return false;
#endif
}

std::string ShaderRoot()
{
    // 与 SpineGpuRenderer.cpp 的 ShaderRoot() 同款：着色器源码在工程里的根。
    return "engine/shader";
}
}   // namespace

// ---- 每个发射器一份的常量（== 着色器里读的那一块）----
// 改这里必须同步改 engine/shader/hlsl/rp2/particle_gpu.vert.hlsl 与（将来）
// engine/shader/glsl/particle_gpu.vert，以及公式层 particle_sim.h。
struct ParticleGpuDrawConsts
{
    float view_proj[16];   // 观察投影
    float model[16];       // 本地 → 世界
    float cam_right[4];    // 相机右（xyz + 占位）
    float cam_up[4];       // 相机上（xyz + 占位）
    float time;            // 与核心 tick 同一个 time
    uint32_t window_first;
    uint32_t window_count;
    uint32_t capacity;
    // b2（ByteAddressBuffer）里各段的**字节偏移**与元素数。着色器里的 DrawConsts
    // 与此逐字段对应（hlsl/rp2/particle_gpu.vert.hlsl）—— 改一边必须改另一边。
    uint32_t curve_range_off;
    uint32_t curve_key_off;
    uint32_t grad_range_off;
    uint32_t grad_color_off;
    uint32_t grad_alpha_off;
    uint32_t curve_range_count;
    uint32_t curve_key_count;
    uint32_t grad_range_count;
    uint32_t grad_color_count;
    uint32_t grad_alpha_count;
    uint32_t pad0;
    uint32_t pad1;
};

// ---- 游戏线程 → 渲染线程 ----
enum class ParticleGpuCmd : uint8_t
{
    kAddEmitter,
    kRemoveEmitter,
    kTransform,
    kVisible,
};

struct ParticleGpuPendingCmd
{
    ParticleGpuCmd cmd;
    uint32_t emitter_id {0};
    float time_delta {0.0f};
    bool visible {true};
    float local_to_world[16] {};
    ParticleGpuEmitterSource source {};
};

// =====================================================================
// Impl
// =====================================================================
struct ParticleGpuRenderer::Impl
{
    struct Emitter
    {
        uint32_t id {0};
        ParticleGpuEmitterDesc desc {};
        ParticleGpuEmitterSource source {};   // 曲线 / 渐变（只用来打 b2 的数据块）
        ParticleGpuEmitterState state {};
        float model[16] {};
        float time {0.0f};
        Buffer spawns;
        Buffer consts;
        Buffer emitter;               // b2：desc + 曲线 + 渐变打包的字节块
        bool emitter_uploaded {false};
        bool visible {true};          // 组件 disabled / 父级停用 ⇒ 不画（但仍保留缓冲）
        // b2 里各段的位置（与 DrawConsts 里的 *_off / *_count 一一对应）
        uint32_t off_curve_range {0};
        uint32_t off_curve_key {0};
        uint32_t off_grad_range {0};
        uint32_t off_grad_color {0};
        uint32_t off_grad_alpha {0};
        uint32_t n_curve_range {0};
        uint32_t n_curve_key {0};
        uint32_t n_grad_range {0};
        uint32_t n_grad_color {0};
        uint32_t n_grad_alpha {0};
        RHIDescriptorSet* set {nullptr};
    };

    // ---- 游戏线程 ----
    std::atomic<uint32_t> next_id {1u};

    // ---- 共享（渲染线程）----
    std::mutex mutex;
    std::vector<ParticleGpuPendingCmd> pending;
    std::vector<Emitter> emitters;
    std::unordered_map<uint32_t, size_t> id_to_index;
    Buffer quad_vb;
    Buffer index_buffer;
    RHIDescriptorSetLayout* set0_layout {nullptr};
    RHIPipelineLayout* pipeline_layout {nullptr};
    RHIPipeline* pipeline {nullptr};
    RHIRenderPass* pipeline_render_pass {nullptr};   // 管线与 render pass 绑定：换了就重建
    uint32_t pipeline_subpass {0};
    uint32_t last_draw_count {0};
    bool backend_warned {false};

    // ---- 工具 ----
    void PushCmd(ParticleGpuPendingCmd cmd)
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending.push_back(cmd);
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending.clear();
    }

    static void DestroyBuffer(RHI* rhi, Buffer& b)
    {
        if (b.buffer != nullptr)
        {
            rhi->DestroyBuffer(b.buffer);
            b.buffer = nullptr;
        }
        if (b.memory != nullptr)
        {
            rhi->FreeMemory(b.memory);
            b.memory = nullptr;
        }
        b.size = 0;
    }

    // 需要多大就多大：容量变了重建（旧缓冲立刻释放 —— 见头注释"第一版不接 pass"，
    // 真正的延迟销毁等接了多视口 / 多帧轮转再说）。
    bool EnsureBuffer(RHI* rhi, Buffer& b, RHIDeviceSize size, RHIBufferUsageFlags usage)
    {
        if (b.size == size && b.buffer != nullptr)
        {
            return true;
        }
        DestroyBuffer(rhi, b);
        rhi->CreateBufferAndInitialize(usage,
                                       RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       b.buffer,
                                       b.memory,
                                       size,
                                       nullptr);
        if (b.buffer == nullptr || b.memory == nullptr)
        {
            LOG_ERROR(ZRender, "ParticleGpuRenderer: CreateBufferAndInitialize failed (size={})",
                      static_cast<uint64_t>(size));
            return false;
        }
        b.size = size;
        return true;
    }

    bool Upload(Buffer& b, RHI* rhi, const void* data, RHIDeviceSize size)
    {
        void* dst = nullptr;
        if (!rhi->MapMemory(b.memory, 0, size, 0, &dst) || dst == nullptr)
        {
            LOG_ERROR(ZRender, "ParticleGpuRenderer: MapMemory failed");
            return false;
        }
        std::memcpy(dst, data, static_cast<size_t>(size));
        rhi->UnmapMemory(b.memory);
        return true;
    }

    // 把一个发射器的 desc + 曲线 + 渐变打成 b2 的字节块（定义在本文件下方）
    bool BuildEmitterBlob(Emitter& e, std::vector<uint8_t>& bytes);

    // 共享静态几何：quad 角标 + 索引。角序取自核心（ParticleGpuQuadCorner），
    // 与 Unity 侧的 GPUParticleDrawResources、CPU 镜像 ParticleGpuEmulateVertex
    // 是同一份契约 —— 三处改一处必须改全部。
    bool EnsureShared(RHI* rhi)
    {
        if (quad_vb.buffer != nullptr && index_buffer.buffer != nullptr)
        {
            return true;
        }
        std::vector<float> corners(static_cast<size_t>(kMaxQuads) * kQuadVerts * 2);
        for (uint32_t q = 0; q < kMaxQuads; ++q)
        {
            for (uint32_t c = 0; c < kQuadVerts; ++c)
            {
                float cx = 0.0f;
                float cy = 0.0f;
                ParticleGpuQuadCorner(c, cx, cy);   // 核心（全局命名空间）的唯一角序
                const size_t at = (static_cast<size_t>(q) * kQuadVerts + c) * 2;
                corners[at + 0] = cx;
                corners[at + 1] = cy;
            }
        }
        std::vector<uint32_t> indices(static_cast<size_t>(kMaxQuads) * kQuadIndices);
        for (uint32_t q = 0; q < kMaxQuads; ++q)
        {
            const uint32_t v0 = q * kQuadVerts;
            const size_t at = static_cast<size_t>(q) * kQuadIndices;
            // 两个三角形：0,1,2 + 0,2,3（与 Unity 侧共享静态 IB 的绕序一致）
            indices[at + 0] = v0 + 0;
            indices[at + 1] = v0 + 1;
            indices[at + 2] = v0 + 2;
            indices[at + 3] = v0 + 0;
            indices[at + 4] = v0 + 2;
            indices[at + 5] = v0 + 3;
        }
        if (!EnsureBuffer(rhi, quad_vb, static_cast<RHIDeviceSize>(corners.size() * sizeof(float)),
                          RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !Upload(quad_vb, rhi, corners.data(), quad_vb.size))
        {
            return false;
        }
        if (!EnsureBuffer(rhi, index_buffer, static_cast<RHIDeviceSize>(indices.size() * sizeof(uint32_t)),
                          RHI_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
            !Upload(index_buffer, rhi, indices.data(), index_buffer.size))
        {
            return false;
        }
        return true;
    }

    bool EnsureLayouts(RHI* rhi)
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
            RHIDescriptorSetLayoutCreateInfo info {};
            info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = kSet0Bindings;
            info.pBindings = bindings;
            if (!rhi->CreateDescriptorSetLayout(&info, set0_layout) || set0_layout == nullptr)
            {
                LOG_ERROR(ZRender, "ParticleGpuRenderer: CreateDescriptorSetLayout failed");
                return false;
            }
        }
        if (pipeline_layout == nullptr)
        {
            RHIDescriptorSetLayout* layouts[1] = {set0_layout};
            RHIPipelineLayoutCreateInfo info {};
            info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.setLayoutCount = 1;
            info.pSetLayouts = layouts;
            if (!rhi->CreatePipelineLayout(&info, pipeline_layout) || pipeline_layout == nullptr)
            {
                LOG_ERROR(ZRender, "ParticleGpuRenderer: CreatePipelineLayout failed");
                return false;
            }
        }
        return true;
    }

    bool AllocSet(RHI* rhi, RHIDescriptorSet*& out)
    {
        RHIDescriptorSetAllocateInfo alloc {};
        alloc.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = rhi->GetDescriptorPoor();
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &set0_layout;
        out = nullptr;
        return rhi->AllocateDescriptorSets(&alloc, out) && out != nullptr;
    }

    void WriteSetBinding(RHI* rhi, RHIDescriptorSet* set, uint32_t binding, const Buffer& b)
    {
        RHIDescriptorBufferInfo info {};
        info.buffer = b.buffer;
        info.offset = 0;
        info.range = b.size;
        // b0 / b1 是带类型的 StructuredBuffer ⇒ 给 stride；b2 是 ByteAddressBuffer ⇒ 0。
        if (binding == kBindingSpawns)
        {
            info.stride = static_cast<uint32_t>(sizeof(ParticleGpuSpawn));
        }
        else if (binding == kBindingConsts)
        {
            info.stride = static_cast<uint32_t>(sizeof(ParticleGpuDrawConsts));
        }
        else
        {
            info.stride = 0u;
        }
        RHIWriteDescriptorSet w {};
        w.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &info;
        rhi->UpdateDescriptorSets(1, &w, 0, nullptr);
    }

    // 第一版只有 DX12（运行时编 HLSL）。Vulkan / Metal 要等 SPIR-V 那份（着色器 +
    // 构建侧编译清单，下一步）。WebGL2 不画（没有 SSBO）—— 与 GPU-Spine 的边界一致。
    bool EnsurePipeline(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass)
    {
        if (pipeline != nullptr && pipeline_render_pass == render_pass && pipeline_subpass == subpass)
        {
            return true;
        }
        if (!IsDx12())
        {
            if (!backend_warned)
            {
                backend_warned = true;
                LOG_ERROR(ZRender, "ParticleGpuRenderer: 当前后端还没有 SPIR-V 着色器，GPU 粒子不画");
            }
            return false;
        }
        // DX12：运行时从 .hlsl 源编（RHI.h:239 CreateShaderModuleFromFile）。
        // #include "particle_sim.h" 走 `include` 目录下的转发头。
        const std::string root = ShaderRoot();
        const std::vector<std::string> includes = {root + "/include", root + "/hlsl/rp2"};
        std::vector<uint8_t> vs_binary;   // RHI.h:243 的输出参数（DXIL 落在这里）
        std::vector<uint8_t> fs_binary;
        RHIShader* vs = rhi->CreateShaderModuleFromFile(root + "/hlsl/rp2/particle_gpu.vert.hlsl",
                                                        ShaderStage::Vertex,
                                                        includes,
                                                        {},
                                                        vs_binary);
        RHIShader* fs = rhi->CreateShaderModuleFromFile(root + "/hlsl/rp2/particle_gpu.frag.hlsl",
                                                        ShaderStage::Fragment,
                                                        includes,
                                                        {},
                                                        fs_binary);
        if (vs == nullptr || fs == nullptr)
        {
            LOG_ERROR(ZRender, "ParticleGpuRenderer: 着色器编译失败（{}）", root + "/hlsl/rp2/particle_gpu.*.hlsl");
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

        // 顶点只有一个 float2 角标（共享静态 VB）；quad 下标 = vertexID >> 2。
        RHIPipelineVertexInputStateCreateInfo vertex_input {};
        vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        RHIVertexInputBindingDescription binding {};
        binding.binding = 0;
        binding.stride = static_cast<uint32_t>(kQuadVertexSize);
        binding.inputRate = RHI_VERTEX_INPUT_RATE_VERTEX;
        RHIVertexInputAttributeDescription attr {};
        attr.location = 0;
        attr.binding = 0;
        attr.format = RHI_FORMAT_R32G32_SFLOAT;
        attr.offset = 0;
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = 1;
        vertex_input.pVertexAttributeDescriptions = &attr;

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
        raster.cullMode = RHI_CULL_MODE_NONE;   // 公告板可以镜像（负缩放），不剔除
        raster.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        RHIPipelineMultisampleStateCreateInfo multisample {};
        multisample.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;
        RHIPipelineDepthStencilStateCreateInfo depth {};
        depth.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        // 透明物体的标准做法：测深度（被场景挡住）、不写深度（粒子之间不互相挡）
        depth.depthTestEnable = RHI_TRUE;
        depth.depthWriteEnable = RHI_FALSE;
        depth.depthCompareOp = RHI_COMPARE_OP_LESS_OR_EQUAL;
        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state {};
        dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates = dynamic_states;

        // 预乘 alpha 的合成：src=ONE, dst=ONE_MINUS_SRC_ALPHA（与 Spine 的 normal 同款）
        RHIPipelineColorBlendAttachmentState blend {};
        blend.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                               RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = RHI_TRUE;
        blend.colorBlendOp = RHI_BLEND_OP_ADD;
        blend.alphaBlendOp = RHI_BLEND_OP_ADD;
        blend.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
        RHIPipeline* created = nullptr;
        const bool ok = rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pi, created) && created != nullptr;
        rhi->DestroyShaderModule(vs);
        rhi->DestroyShaderModule(fs);
        if (!ok)
        {
            LOG_ERROR(ZRender, "ParticleGpuRenderer: CreateGraphicsPipelines failed");
            return false;
        }
        pipeline = created;
        pipeline_render_pass = render_pass;
        pipeline_subpass = subpass;
        return true;
    }

    void Flush()
    {
        std::vector<ParticleGpuPendingCmd> cmds;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending.empty())
            {
                return;
            }
            cmds.swap(pending);
        }
        for (const ParticleGpuPendingCmd& cmd : cmds)
        {
            switch (cmd.cmd)
            {
                case ParticleGpuCmd::kAddEmitter:
                {
                    Emitter e;
                    e.id = cmd.emitter_id;
                    e.source = cmd.source;
                    e.desc = cmd.source.desc;
                    e.state = ParticleGpuEmitterState{};
                    std::memcpy(e.model, cmd.local_to_world, sizeof(float) * 16);
                    id_to_index[e.id] = emitters.size();
                    emitters.push_back(e);
                    break;
                }
                case ParticleGpuCmd::kRemoveEmitter:
                {
                    auto it = id_to_index.find(cmd.emitter_id);
                    if (it == id_to_index.end())
                    {
                        break;
                    }
                    const size_t index = it->second;
                    if (index < emitters.size())
                    {
                        emitters[index].desc = ParticleGpuEmitterDesc{};
                        emitters[index].state = ParticleGpuEmitterState{};
                    }
                    id_to_index.erase(it);
                    break;
                }
                case ParticleGpuCmd::kTransform:
                {
                    auto it = id_to_index.find(cmd.emitter_id);
                    if (it == id_to_index.end() || it->second >= emitters.size())
                    {
                        break;
                    }
                    Emitter& e = emitters[it->second];
                    std::memcpy(e.model, cmd.local_to_world, sizeof(float) * 16);
                    e.time += cmd.time_delta;
                    break;
                }
                case ParticleGpuCmd::kVisible:
                {
                    auto it = id_to_index.find(cmd.emitter_id);
                    if (it != id_to_index.end() && it->second < emitters.size())
                    {
                        emitters[it->second].visible = cmd.visible;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
};

// =====================================================================
// 对外接口
// =====================================================================
ParticleGpuRenderer& ParticleGpuRenderer::Get()
{
    static ParticleGpuRenderer renderer;
    return renderer;
}

ParticleGpuRenderer::ParticleGpuRenderer() : m_Impl(new ParticleGpuRenderer::Impl())
{
}

ParticleGpuRenderer::Impl& ParticleGpuRenderer::impl()
{
    return *m_Impl;
}

uint32_t ParticleGpuRenderer::RequestAddEmitter(const ParticleGpuEmitterSource& source)
{
    const ParticleGpuEmitterDesc& desc = source.desc;
    uint32_t required = 0;   // ParticleGpuEmitter.h:51 的出参：最少需要多少槽位
    if (!ParticleGpuEmitterCheckCapacity(desc, required))
    {
        LOG_ERROR(ZRender,
                  "ParticleGpuRenderer::RequestAddEmitter: 容量不合法：desc.capacity={}，至少需要 {}（rate={}, "
                  "lifetime={}）",
                  desc.capacity, required, desc.rate, desc.lifetime);
        return 0;
    }
    ParticleGpuPendingCmd cmd;
    cmd.cmd = ParticleGpuCmd::kAddEmitter;
    cmd.source = source;
    cmd.emitter_id = m_Impl->next_id.fetch_add(1u);
    m_Impl->PushCmd(cmd);
    return cmd.emitter_id;
}

bool ParticleGpuRenderer::RequestSetVisible(uint32_t id, bool visible)
{
    if (id == 0)
    {
        return false;
    }
    ParticleGpuPendingCmd cmd;
    cmd.cmd = ParticleGpuCmd::kVisible;
    cmd.emitter_id = id;
    cmd.visible = visible;
    m_Impl->PushCmd(cmd);
    return true;
}

bool ParticleGpuRenderer::RequestRemoveEmitter(uint32_t id)
{
    if (id == 0)
    {
        return false;
    }
    ParticleGpuPendingCmd cmd;
    cmd.cmd = ParticleGpuCmd::kRemoveEmitter;
    cmd.emitter_id = id;
    m_Impl->PushCmd(cmd);
    return true;
}

bool ParticleGpuRenderer::RequestSetTransform(uint32_t id, const float local_to_world[16], float dt)
{
    if (id == 0)
    {
        return false;
    }
    ParticleGpuPendingCmd cmd;
    cmd.cmd = ParticleGpuCmd::kTransform;
    cmd.emitter_id = id;
    cmd.time_delta = dt;
    std::memcpy(cmd.local_to_world, local_to_world, sizeof(float) * 16);
    m_Impl->PushCmd(cmd);
    return true;
}

void ParticleGpuRenderer::RequestClear()
{
    m_Impl->Clear();
}

uint32_t ParticleGpuRenderer::LastDrawCount() const
{
    return m_Impl->last_draw_count;
}

std::string ParticleGpuRenderer::Status() const
{
    return "ParticleGpuRenderer: " + std::to_string(m_Impl->emitters.size()) + " emitters, last draw " +
           std::to_string(LastDrawCount()) + " quads";
}

// b2 的数据块：[desc(96 B)] [curve_ranges] [curve_keys] [gradient_ranges] [color_keys] [alpha_keys]
// 每段起始都对齐到 16 B：着色器对 color_keys 用 ByteAddressBuffer.Load4，偏移必须 16 B 对齐。
namespace
{
void BlobAppend(std::vector<uint8_t>& dst, const void* data, size_t size)
{
    while ((dst.size() % 16u) != 0u)
    {
        dst.push_back(0u);
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    dst.insert(dst.end(), p, p + size);
}
}   // namespace

bool ParticleGpuRenderer::Impl::BuildEmitterBlob(Emitter& e, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    BlobAppend(bytes, &e.desc, sizeof(ParticleGpuEmitterDesc));
    if (!e.source.curve_ranges.empty())
    {
        e.off_curve_range = static_cast<uint32_t>(bytes.size());
        e.n_curve_range = static_cast<uint32_t>(e.source.curve_ranges.size());
        BlobAppend(bytes, e.source.curve_ranges.data(), e.source.curve_ranges.size() * sizeof(ParticleGpuCurveRange));
    }
    if (!e.source.curve_keys.empty())
    {
        e.off_curve_key = static_cast<uint32_t>(bytes.size());
        e.n_curve_key = static_cast<uint32_t>(e.source.curve_keys.size());
        BlobAppend(bytes, e.source.curve_keys.data(), e.source.curve_keys.size() * sizeof(ParticleGpuCurveKey));
    }
    if (!e.source.gradient_ranges.empty())
    {
        e.off_grad_range = static_cast<uint32_t>(bytes.size());
        e.n_grad_range = static_cast<uint32_t>(e.source.gradient_ranges.size());
        BlobAppend(bytes, e.source.gradient_ranges.data(),
                   e.source.gradient_ranges.size() * sizeof(ParticleGpuGradientRange));
    }
    if (!e.source.gradient_color_keys.empty())
    {
        e.off_grad_color = static_cast<uint32_t>(bytes.size());
        e.n_grad_color = static_cast<uint32_t>(e.source.gradient_color_keys.size());
        BlobAppend(bytes, e.source.gradient_color_keys.data(),
                   e.source.gradient_color_keys.size() * sizeof(ParticleGpuGradientColorKey));
    }
    if (!e.source.gradient_alpha_keys.empty())
    {
        e.off_grad_alpha = static_cast<uint32_t>(bytes.size());
        e.n_grad_alpha = static_cast<uint32_t>(e.source.gradient_alpha_keys.size());
        BlobAppend(bytes, e.source.gradient_alpha_keys.data(),
                   e.source.gradient_alpha_keys.size() * sizeof(ParticleGpuGradientAlphaKey));
    }
    return !bytes.empty();
}

void ParticleGpuRenderer::Draw(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
                               ViewportType viewport_type, const float view_proj[16], const float cam_right[3],
                               const float cam_up[3])
{
    (void)viewport_type;   // 第一版不区分视口类型：粒子是纯公告板，正交 / 透视都能画
    ParticleGpuRenderer::Impl& impl = this->impl();
    impl.last_draw_count = 0;
    if (rhi == nullptr || render_pass == nullptr)
    {
        return;
    }
    impl.Flush();
    if (impl.emitters.empty())
    {
        return;
    }
    if (!impl.EnsureShared(rhi) || !impl.EnsureLayouts(rhi) || !impl.EnsurePipeline(rhi, render_pass, subpass))
    {
        return;
    }

    RHICommandBuffer* cmd = rhi->GetCurrentCommandBuffer();
    if (cmd == nullptr)
    {
        return;
    }
    RHIViewport vp = viewport;
    // Vulkan 要求 scissor offset ≥ 0（视口可能部分移出窗口）—— 与 Spine 同款
    const float sx0 = std::max(viewport.x, 0.0f);
    const float sy0 = std::max(viewport.y, 0.0f);
    const float sx1 = std::max(viewport.x + viewport.width, sx0);
    const float sy1 = std::max(viewport.y + viewport.height, sy0);
    RHIRect2D scissor = {{static_cast<int32_t>(sx0), static_cast<int32_t>(sy0)},
                         {static_cast<uint32_t>(sx1 - sx0), static_cast<uint32_t>(sy1 - sy0)}};
    rhi->CmdSetViewportPFN(cmd, 0, 1, &vp);
    rhi->CmdSetScissorPFN(cmd, 0, 1, &scissor);
    rhi->CmdBindPipelinePFN(cmd, RHI_PIPELINE_BIND_POINT_GRAPHICS, impl.pipeline);
    RHIBuffer* vbs[1] = {impl.quad_vb.buffer};
    RHIDeviceSize offsets[1] = {0};
    rhi->CmdBindVertexBuffersPFN(cmd, 0, 1, vbs, offsets);
    // RHI_INDEX_TYPE_UINT32 是 RenderType.h:1670 的无作用域枚举，不写限定。
    rhi->CmdBindIndexBufferPFN(cmd, impl.index_buffer.buffer, 0, RHI_INDEX_TYPE_UINT32);

    for (Impl::Emitter& e : impl.emitters)
    {
        if (e.desc.capacity == 0 || !e.visible)
        {
            continue;   // 已被移除 / 组件不激活
        }
        // ---- 1) 核心推进：写入本帧出生记录、给出存活窗口 ----
        const ParticleGpuWindow window = ParticleGpuEmitterTick(e.state, e.desc, e.time);
        if (window.count == 0)
        {
            continue;
        }
        // ---- 2) 上传出生记录（第一版整条环形重写，见头注释）----
        const RHIDeviceSize spawn_bytes = static_cast<RHIDeviceSize>(e.desc.capacity) * sizeof(ParticleGpuSpawn);
        if (!impl.EnsureBuffer(rhi, e.spawns, spawn_bytes, RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !impl.Upload(e.spawns, rhi, e.state.spawns.data(), spawn_bytes))
        {
            continue;
        }
        // ---- 2b) 发射器数据块（b2）：只在加进来时打一次，之后不动 ----
        if (!e.emitter_uploaded)
        {
            std::vector<uint8_t> blob;
            if (!impl.BuildEmitterBlob(e, blob) ||
                !impl.EnsureBuffer(rhi, e.emitter, static_cast<RHIDeviceSize>(blob.size()),
                                   RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
                !impl.Upload(e.emitter, rhi, blob.data(), static_cast<RHIDeviceSize>(blob.size())))
            {
                continue;
            }
            e.emitter_uploaded = true;
        }

        // ---- 3) 本绘制常量 ----
        ParticleGpuDrawConsts consts {};
        std::memcpy(consts.view_proj, view_proj, sizeof(float) * 16);
        std::memcpy(consts.model, e.model, sizeof(float) * 16);
        consts.cam_right[0] = cam_right[0];
        consts.cam_right[1] = cam_right[1];
        consts.cam_right[2] = cam_right[2];
        consts.cam_up[0] = cam_up[0];
        consts.cam_up[1] = cam_up[1];
        consts.cam_up[2] = cam_up[2];
        consts.time = e.time;
        consts.window_first = window.first;
        consts.window_count = window.count;
        consts.capacity = e.desc.capacity;
        // b2 各段位置（与着色器 DrawConsts 逐字段对应）
        consts.curve_range_off = e.off_curve_range;
        consts.curve_key_off = e.off_curve_key;
        consts.grad_range_off = e.off_grad_range;
        consts.grad_color_off = e.off_grad_color;
        consts.grad_alpha_off = e.off_grad_alpha;
        consts.curve_range_count = e.n_curve_range;
        consts.curve_key_count = e.n_curve_key;
        consts.grad_range_count = e.n_grad_range;
        consts.grad_color_count = e.n_grad_color;
        consts.grad_alpha_count = e.n_grad_alpha;
        consts.pad0 = 0;
        consts.pad1 = 0;
        if (!impl.EnsureBuffer(rhi, e.consts, static_cast<RHIDeviceSize>(sizeof(ParticleGpuDrawConsts)),
                               RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !impl.Upload(e.consts, rhi, &consts, sizeof(ParticleGpuDrawConsts)))
        {
            continue;
        }
        // ---- 4) 描述符 ----
        if (e.set == nullptr && !impl.AllocSet(rhi, e.set))
        {
            continue;
        }
        impl.WriteSetBinding(rhi, e.set, kBindingSpawns, e.spawns);
        impl.WriteSetBinding(rhi, e.set, kBindingConsts, e.consts);
        impl.WriteSetBinding(rhi, e.set, kBindingEmitter, e.emitter);
        RHIDescriptorSet* sets[1] = {e.set};
        rhi->CmdBindDescriptorSetsPFN(cmd, RHI_PIPELINE_BIND_POINT_GRAPHICS, impl.pipeline_layout, 0, 1, sets, 0,
                                      nullptr);
        // ---- 5) 画：count 个 quad，从共享 VB / IB 的第 0 个 quad 起 ----
        // 顶点着色器用 vertexID >> 2 拿到 quad 下标，再 + window_first 求环槽位。
        const uint32_t index_count = window.count * kQuadIndices;
        rhi->CmdDrawIndexedPFN(cmd, index_count, 1, 0, 0, 0);
        impl.last_draw_count += window.count;
    }
}
