#include "Runtime/Function/Render/AppleSsaoPass.h"

#include <cstring>

// ---------------------------------------------------------------------------
// MSL 源码的拼装（平台无关的纯字符串，非 Apple 平台上测试也能做文本检查）。
//
// 每个 library 只放**一个**入口函数：MetalRHI 的 CreateShaderModuleFromSource 找
// "main" 落空时退回 `library.functionNames.firstObject`，放两个入口必然取错。
// 公共部分（参数结构、重建世界坐标、噪声）以 static 函数的形式拼在每份源码前面，
// static 函数不是入口，不进 functionNames。
//
// 槽位（MetalRHI：texture = set*8 + binding；buffer 再 +8）：
//   set 0 / binding 0 ⇒ [[buffer(8)]]    SsaoParams（UNIFORM_BUFFER_DYNAMIC，每视口一份）
//   set 0 / binding 1 ⇒ [[texture(1)]]   场景深度（D32）
//   set 0 / binding 2 ⇒ [[texture(2)]]   GBufferA（世界法线）
//   set 0 / binding 3 ⇒ [[texture(3)]]   上一趟的 AO（只有模糊两趟用）
// 全程 `read()`（逐像素精确取值），不用 sampler —— AO 的输入必须和像素一一对应，
// 双线性会把深度跨边缘插值成一个不存在的表面。
// ---------------------------------------------------------------------------
namespace
{
    const char* kAppleSsaoVertMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SsaoVertexOut
{
    float4 position [[position]];
    float2 ndc;
};

vertex SsaoVertexOut SsaoVertexMain(uint vertex_id [[vertex_id]])
{
    const float2 xy = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);

    SsaoVertexOut out;
    out.position = float4(xy, 0.0, 1.0);
    // 直接把 NDC 传下去：fragment 里它就是"这个像素中心在这台相机下的 NDC"，
    // 和网格光栅化走的是同一套视口变换（不需要、也不许自己猜 y 轴翻不翻）。
    out.ndc = xy;
    return out;
}
)MSL";

    // 公共部分。**SsaoParams 的字段顺序必须和 C++ 的 AppleSsaoGpuParams 一致**。
    const char* kAppleSsaoCommonMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SsaoVertexOut
{
    float4 position [[position]];
    float2 ndc;
};

struct SsaoParams
{
    float4x4 proj_view;
    float4x4 inv_proj_view;
    float4   camera_position;
    float4   viewport;
    float4   settings;
    float4   blur;
};

// 和 deferred_lighting.frag 反推世界坐标是同一条式子：NDC(xy, depth) 过 proj_view 的逆。
static float3 SsaoReconstruct(constant SsaoParams& p, float2 ndc, float depth)
{
    const float4 world = p.inv_proj_view * float4(ndc, depth, 1.0);
    return world.xyz / world.w;
}

static bool SsaoInsideViewport(constant SsaoParams& p, float2 px)
{
    return px.x >= p.viewport.x && px.y >= p.viewport.y &&
           px.x < p.viewport.x + p.viewport.z && px.y < p.viewport.y + p.viewport.w;
}

// GBufferA：EncodeNormal(N) = N * 0.5 + 0.5（engine/shader/include/gbuffer.h）。
static float3 SsaoDecodeNormal(float4 encoded)
{
    return normalize(encoded.xyz * 2.0 - 1.0);
}
)MSL";

    // ---- 原始 AO（Alchemy，逐项对齐 URP SSAO.hlsl 的 SSAO()）----
    //
    //   ao = Σ max(dot(v, N) - kBeta * dist, 0) / (dot(v, v) + kEpsilon)
    //   ao = pow(saturate(ao * radius * intensity / count), kContrast)
    //   visibility = 1 - ao
    //
    // v = 样本点投影回屏幕后、那个像素上真实表面点 Q 与当前点 P 的差。
    // 只统计"沿视线方向离 P 不超过 radius"的 Q（URP 的 isInsideRadius）——
    // 否则远处挡在前面的大物体会让整片像素发黑（光环）。
    const char* kAppleSsaoAoBodyMsl = R"MSL(
static float SsaoUVRandom(float u, float v)
{
    const float f = dot(float2(12.9898, 78.233), float2(u, v));
    return fract(43758.5453 * sin(f));
}

// Jimenez 2014 Interleaved Gradient Noise（URP 同款系数）。
static float SsaoGradientNoise(float2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, float2(0.06711056, 0.00583715))));
}

// URP PickSamplePoint：单位球面上的点，按像素噪声旋转。
static float3 SsaoSamplePoint(float noise, int index)
{
    const float u     = fract(SsaoUVRandom(0.0, float(index)) + noise) * 2.0 - 1.0;
    const float theta = (SsaoUVRandom(1.0, float(index)) + noise) * 6.28318530718;
    return float3(float2(cos(theta), sin(theta)) * sqrt(max(1.0 - u * u, 0.0)), u);
}

fragment float4 SsaoAoMain(SsaoVertexOut       in         [[stage_in]],
                           constant SsaoParams& p          [[buffer(8)]],
                           texture2d<float>     depth_tex  [[texture(1)]],
                           texture2d<float>     normal_tex [[texture(2)]])
{
    // 导数必须在任何分支之前取（分支里的 dfdx/dfdy 是未定义的）。
    // 全屏三角形上 ndc 是线性的 ⇒ 这两个数就是"每像素多少 NDC"，带符号。
    const float2 ndc_per_px = float2(dfdx(in.ndc.x), dfdy(in.ndc.y));

    const uint2 pix   = uint2(in.position.xy);
    const float depth = depth_tex.read(pix).r;
    // 什么都没画（天空 / 清屏）：不遮蔽。
    if (depth >= 1.0 || abs(ndc_per_px.x) < 1e-8 || abs(ndc_per_px.y) < 1e-8)
    {
        return float4(1.0, 1.0, 0.0, 1.0);
    }

    const float3 P        = SsaoReconstruct(p, in.ndc, depth);
    const float3 N        = SsaoDecodeNormal(normal_tex.read(pix));
    const float3 to_p     = P - p.camera_position.xyz;
    const float  dist_o   = length(to_p);
    const float3 view_dir = to_p / max(dist_o, 1e-6);

    const float radius    = max(p.settings.x, 1e-4);
    const int   count     = clamp(int(p.settings.w), 1, 64);
    const float inv_count = 1.0 / float(count);
    const float noise     = SsaoGradientNoise(in.position.xy);

    const float kBeta    = 0.002;
    const float kEpsilon = 0.0001;

    float ao = 0.0;
    for (int s = 0; s < count; ++s)
    {
        // [0, radius] 之间按 sqrt 分布（越靠后的样本离 P 越远），再翻进法线半球。
        float3 v_s1 = SsaoSamplePoint(noise, s) * (sqrt((float(s) + 1.0) * inv_count) * radius);
        v_s1        = dot(v_s1, N) < 0.0 ? -v_s1 : v_s1;

        const float4 clip = p.proj_view * float4(P + v_s1, 1.0);
        if (clip.w <= 1e-6)
        {
            continue;
        }
        const float2 ndc_s = clip.xy / clip.w;
        const float2 px    = in.position.xy + (ndc_s - in.ndc) / ndc_per_px;
        if (!SsaoInsideViewport(p, px))
        {
            continue;
        }
        // 深度是在**整像素中心**取的，重建 Q 也必须用那个像素中心的 NDC，不能用
        // 样本点自己的亚像素 NDC —— 否则斜面上 Q 会偏离真实表面最多一个像素的
        // 深度斜率，平面自己遮蔽自己（斜坡上一层灰）。
        const float2 px_center  = floor(px) + 0.5;
        const float2 ndc_center = in.ndc + (px_center - in.position.xy) * ndc_per_px;
        const float  depth_s    = depth_tex.read(uint2(px_center)).r;
        if (depth_s >= 1.0)
        {
            continue;  // 天空不遮蔽
        }

        const float3 v_s2   = SsaoReconstruct(p, ndc_center, depth_s) - P;
        const float  inside = abs(dot(v_s2, view_dir)) < radius ? 1.0 : 0.0;
        const float  a1     = max(dot(v_s2, N) - kBeta * dist_o, 0.0);
        const float  a2     = dot(v_s2, v_s2) + kEpsilon;
        ao += a1 / a2 * inside;
    }

    ao *= radius;
    ao = pow(saturate(ao * p.settings.y * inv_count), 0.6);

    const float visibility = 1.0 - ao;
    // g = 直接光乘子。模糊关掉时这一趟就是最终结果，所以这里也得给对。
    return float4(visibility, mix(1.0, visibility, saturate(p.settings.z)), 0.0, 1.0);
}
)MSL";

    // ---- 双边模糊（URP 的 HorizontalBlur / VerticalBlur 思路）----
    //
    // 9 tap 高斯（σ≈2）。每个 tap 乘两个几何权重，否则 AO 会从物体边缘"漏"到背景上：
    //   法线：smoothstep(0.8, 1.0, dot(N, Nt))（URP 的 kGeometryCoeff = 0.8）
    //   平面距离：tap 的世界坐标离"当前像素所在切平面"多远，超过 radius * 容差就不要。
    // SSAO_BLUR_DIRECTION / SSAO_BLUR_FINAL 由 BuildFragmentSource 按趟定义。
    const char* kAppleSsaoBlurBodyMsl = R"MSL(
fragment float4 SsaoBlurMain(SsaoVertexOut       in         [[stage_in]],
                             constant SsaoParams& p          [[buffer(8)]],
                             texture2d<float>     depth_tex  [[texture(1)]],
                             texture2d<float>     normal_tex [[texture(2)]],
                             texture2d<float>     ao_tex     [[texture(3)]])
{
    const float2 ndc_per_px = float2(dfdx(in.ndc.x), dfdy(in.ndc.y));

    const uint2 pix       = uint2(in.position.xy);
    const float depth     = depth_tex.read(pix).r;
    const float center_ao = ao_tex.read(pix).r;
    if (depth >= 1.0)
    {
        return float4(1.0, 1.0, 0.0, 1.0);
    }

    const float3 P         = SsaoReconstruct(p, in.ndc, depth);
    const float3 N         = SsaoDecodeNormal(normal_tex.read(pix));
    const float  tolerance = max(p.settings.x * p.blur.x, 1e-4);

    const float weights[5] = {0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};

    float sum  = center_ao * weights[0];
    float wsum = weights[0];
    for (int i = 1; i < 5; ++i)
    {
        for (int side = -1; side <= 1; side += 2)
        {
            const float2 offset = SSAO_BLUR_DIRECTION * float(i * side);
            const float2 px     = in.position.xy + offset;
            if (!SsaoInsideViewport(p, px))
            {
                continue;
            }
            const uint2 tap       = uint2(px);
            const float tap_depth = depth_tex.read(tap).r;
            if (tap_depth >= 1.0)
            {
                continue;
            }
            const float3 Q  = SsaoReconstruct(p, in.ndc + offset * ndc_per_px, tap_depth);
            const float3 Nt = SsaoDecodeNormal(normal_tex.read(tap));

            const float w_normal = smoothstep(0.8, 1.0, dot(N, Nt));
            const float w_plane  = saturate(1.0 - abs(dot(Q - P, N)) / tolerance);
            const float w        = weights[i] * w_normal * w_plane;

            sum += ao_tex.read(tap).r * w;
            wsum += w;
        }
    }

    const float ao = sum / max(wsum, 1e-6);
#if SSAO_BLUR_FINAL
    return float4(ao, mix(1.0, ao, saturate(p.settings.z)), 0.0, 1.0);
#else
    return float4(ao, ao, 0.0, 1.0);
#endif
}
)MSL";
}  // namespace

std::string AppleSsaoPass::BuildVertexSource()
{
    return kAppleSsaoVertMsl;
}

std::string AppleSsaoPass::BuildFragmentSource(Stage stage)
{
    std::string source = kAppleSsaoCommonMsl;
    switch (stage)
    {
        case Stage::Ao:
            source += kAppleSsaoAoBodyMsl;
            break;
        case Stage::BlurH:
            source += "\n#define SSAO_BLUR_DIRECTION float2(1.0, 0.0)\n#define SSAO_BLUR_FINAL 0\n";
            source += kAppleSsaoBlurBodyMsl;
            break;
        case Stage::BlurV:
            source += "\n#define SSAO_BLUR_DIRECTION float2(0.0, 1.0)\n#define SSAO_BLUR_FINAL 1\n";
            source += kAppleSsaoBlurBodyMsl;
            break;
        default:
            return std::string();
    }
    return source;
}

AppleSsaoGpuParams AppleSsaoPass::MakeGpuParams(const AppleSsaoViewport& viewport, const AppleSsaoSettings& settings)
{
    AppleSsaoGpuParams params {};

    // 逐字节照搬 Matrix4x4：MainCameraPerFrame 也是这么把它塞进 GPU 的，GLSL / MSL
    // 两边按列主序读同一段字节 ⇒ shader 里 `proj_view * v` 与 deferred_lighting
    // 的语义一致。逆矩阵用**同一种存法**的逆：inv(Mᵀ) = inv(M)ᵀ，所以不管 C++ 侧
    // 存的是 M 还是 Mᵀ，GPU 侧拿到的都恰好是 proj_view 的逆。
    static_assert(sizeof(Matrix4x4) == sizeof(params.proj_view), "Matrix4x4 must be 16 tightly packed floats");
    const Matrix4x4 inverse = viewport.proj_view.inverse();
    std::memcpy(params.proj_view, &viewport.proj_view, sizeof(params.proj_view));
    std::memcpy(params.inv_proj_view, &inverse, sizeof(params.inv_proj_view));

    params.camera_position[0] = viewport.camera_position.x;
    params.camera_position[1] = viewport.camera_position.y;
    params.camera_position[2] = viewport.camera_position.z;
    params.camera_position[3] = 1.0f;

    params.viewport[0] = viewport.viewport.x;
    params.viewport[1] = viewport.viewport.y;
    params.viewport[2] = viewport.viewport.width;
    params.viewport[3] = viewport.viewport.height;

    params.settings[0] = settings.radius;
    params.settings[1] = settings.intensity;
    params.settings[2] = settings.direct_lighting_strength;
    params.settings[3] = static_cast<float>(settings.sample_count);

    // 模糊的平面距离容差：radius 的 1/4（离切平面超过这个距离的 tap 不算同一个表面）。
    params.blur[0] = 0.25f;
    return params;
}

#if defined(__APPLE__)

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/Interface/RHI.h"

bool AppleSsaoPass::Initialize(RHI* rhi)
{
    Shutdown();
    m_Rhi = rhi;
    if (rhi == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSsaoPass: rhi is null");
        return false;
    }

    // ---- 描述符布局：0 = 参数（dynamic，每视口一个偏移），1/2/3 = 三张输入图 ----
    RHIDescriptorSetLayoutBinding bindings[4] {};
    bindings[0].binding         = kParamsBinding;
    bindings[0].descriptorType  = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = RHI_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < 4; ++i)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = RHI_SHADER_STAGE_FRAGMENT_BIT;
    }

    RHIDescriptorSetLayoutCreateInfo layout_info {};
    layout_info.sType        = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 4;
    layout_info.pBindings    = bindings;
    if (!rhi->CreateDescriptorSetLayout(&layout_info, m_DescriptorSetLayout) || m_DescriptorSetLayout == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSsaoPass: descriptor set layout creation failed");
        return false;
    }

    RHIPipelineLayoutCreateInfo pipeline_layout_info {};
    pipeline_layout_info.sType          = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts    = &m_DescriptorSetLayout;
    if (!rhi->CreatePipelineLayout(&pipeline_layout_info, m_PipelineLayout) || m_PipelineLayout == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSsaoPass: pipeline layout creation failed");
        return false;
    }

    // 三趟各一个 set：binding 3 指向的上一趟结果不同（AO 趟不读它）。
    for (uint32_t i = 0; i < static_cast<uint32_t>(Stage::Count); ++i)
    {
        RHIDescriptorSetAllocateInfo alloc_info {};
        alloc_info.sType              = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool     = rhi->GetDescriptorPoor();
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &m_DescriptorSetLayout;
        if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&alloc_info, m_DescriptorSets[i]) ||
            m_DescriptorSets[i] == nullptr)
        {
            LOG_ERROR(ZRender, "AppleSsaoPass: descriptor set allocation failed (stage {})", i);
            return false;
        }
    }

    RHISamplerCreateInfo sampler_info {};
    sampler_info.sType        = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter    = RHI_FILTER_NEAREST;
    sampler_info.minFilter    = RHI_FILTER_NEAREST;
    sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!rhi->CreateSampler(&sampler_info, m_Sampler) || m_Sampler == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSsaoPass: sampler creation failed");
        return false;
    }

    // ---- 参数 buffer：每个 frame-in-flight × 每个视口一个 256 字节槽 ----
    //
    // 不能所有帧共用一个槽：CPU 写下一帧的时候，GPU 可能还在读上一帧的。
    m_FramesInFlight = rhi->GetMaxFramesInFlight() > 0 ? rhi->GetMaxFramesInFlight() : 1;
    const RHIDeviceSize params_bytes =
        static_cast<RHIDeviceSize>(m_FramesInFlight) * kMaxViewports * kParamsStride;
    rhi->CreateBuffer(params_bytes,
                      RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_ParamsBuffer,
                      m_ParamsMemory);
    void* mapped = nullptr;
    if (m_ParamsBuffer == nullptr || m_ParamsMemory == nullptr ||
        !rhi->MapMemory(m_ParamsMemory, 0, params_bytes, 0, &mapped) || mapped == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSsaoPass: parameter buffer creation / mapping failed");
        return false;
    }
    m_ParamsMapped = static_cast<uint8_t*>(mapped);
    std::memset(m_ParamsMapped, 0, static_cast<size_t>(params_bytes));

    // 参数 binding 只写一次：buffer 不变，每次绘制的差别全在 dynamic offset 上。
    {
        RHIDescriptorBufferInfo buffer_info {};
        buffer_info.buffer = m_ParamsBuffer;
        buffer_info.offset = 0;
        buffer_info.range  = sizeof(AppleSsaoGpuParams);

        RHIWriteDescriptorSet writes[static_cast<uint32_t>(Stage::Count)] {};
        for (uint32_t i = 0; i < static_cast<uint32_t>(Stage::Count); ++i)
        {
            writes[i].sType           = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_DescriptorSets[i];
            writes[i].dstBinding      = kParamsBinding;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[i].pBufferInfo     = &buffer_info;
        }
        rhi->UpdateDescriptorSets(static_cast<uint32_t>(Stage::Count), writes, 0, nullptr);
    }

    // ---- render pass：一张 RG8 颜色附件，每趟先清成 (1,1) = 不遮蔽 ----
    //
    // 清成 1 而不是 0：两个视口之外的像素（以及某个视口这一帧没画）都必须是"没有
    // 遮蔽"，否则延迟光照在那些地方读到 0，环境光整片消失。
    {
        RHIAttachmentDescription attachment {};
        attachment.format        = kTargetFormat;
        attachment.samples       = RHI_SAMPLE_COUNT_1_BIT;
        attachment.loadOp        = RHI_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp       = RHI_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout   = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        RHIAttachmentReference color_reference {};
        color_reference.attachment = 0;
        color_reference.layout     = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        RHISubpassDescription subpass {};
        subpass.pipelineBindPoint    = RHI_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &color_reference;

        RHIRenderPassCreateInfo render_pass_info {};
        render_pass_info.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments    = &attachment;
        render_pass_info.subpassCount    = 1;
        render_pass_info.pSubpasses      = &subpass;
        if (!rhi->CreateRenderPass(&render_pass_info, m_RenderPass) || m_RenderPass == nullptr)
        {
            LOG_ERROR(ZRender, "AppleSsaoPass: render pass creation failed");
            return false;
        }
    }

    // ---- 三条管线：同一个顶点函数 + 各自的 fragment ----
    for (uint32_t i = 0; i < static_cast<uint32_t>(Stage::Count); ++i)
    {
        const std::string vert_source = BuildVertexSource();
        const std::string frag_source = BuildFragmentSource(static_cast<Stage>(i));

        RHIShader* vert = rhi->CreateShaderModuleFromSource(vert_source, ShaderStage::Vertex, "apple_ssao.vert");
        RHIShader* frag = rhi->CreateShaderModuleFromSource(frag_source, ShaderStage::Fragment, "apple_ssao.frag");
        if (vert == nullptr || frag == nullptr)
        {
            LOG_ERROR(ZRender, "AppleSsaoPass: MSL compilation failed (stage {})", i);
            if (vert != nullptr)
            {
                rhi->DestroyShaderModule(vert);
            }
            if (frag != nullptr)
            {
                rhi->DestroyShaderModule(frag);
            }
            return false;
        }

        RHIPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = RHI_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = RHI_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        RHIPipelineVertexInputStateCreateInfo vertex_input {};
        vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
        input_assembly.sType                  = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology               = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state {};
        viewport_state.sType         = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;

        RHIPipelineRasterizationStateCreateInfo rasterization {};
        rasterization.sType       = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = RHI_POLYGON_MODE_FILL;
        // 全屏三角形按 vertex_id 直接给 NDC，绕序和场景网格相反 ⇒ 关掉剔除。
        rasterization.cullMode  = RHI_CULL_MODE_NONE;
        rasterization.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        RHIPipelineMultisampleStateCreateInfo multisample {};
        multisample.sType                = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState blend {};
        blend.blendEnable    = RHI_FALSE;
        blend.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                               RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo blend_state {};
        blend_state.sType           = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend_state.attachmentCount = 1;
        blend_state.pAttachments    = &blend;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state {};
        dynamic_state.sType             = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates    = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipeline_info {};
        pipeline_info.sType               = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount          = 2;
        pipeline_info.pStages             = stages;
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState   = &multisample;
        pipeline_info.pColorBlendState    = &blend_state;
        pipeline_info.pDynamicState       = &dynamic_state;
        pipeline_info.layout              = m_PipelineLayout;
        pipeline_info.renderPass          = m_RenderPass;
        pipeline_info.subpass             = 0;

        const bool ok = rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_Pipelines[i]);
        rhi->DestroyShaderModule(vert);
        rhi->DestroyShaderModule(frag);
        if (!ok || m_Pipelines[i] == nullptr)
        {
            LOG_ERROR(ZRender, "AppleSsaoPass: graphics pipeline creation failed (stage {})", i);
            return false;
        }
    }

    m_Ready = true;
    LOG_INFO(ZRender, "AppleSsaoPass: ready (frames_in_flight={}, target=RG8, stages=AO+blurH+blurV)", m_FramesInFlight);
    return true;
}

void AppleSsaoPass::Shutdown()
{
    // 和 AppleRp2Chain 一样：RHI 层的 Destroy* 在 Metal 上大多是空实现，这里尽量走
    // 正规接口，剩下的置空。
    if (m_Rhi != nullptr)
    {
        for (uint32_t i = 0; i < 2; ++i)
        {
            if (m_Framebuffers[i] != nullptr)
            {
                m_Rhi->DestroyFramebuffer(m_Framebuffers[i]);
            }
            if (m_TargetViews[i] != nullptr)
            {
                m_Rhi->DestroyImageView(m_TargetViews[i]);
            }
            if (m_TargetImages[i] != nullptr)
            {
                m_Rhi->DestroyImage(m_TargetImages[i]);
            }
            if (m_TargetMemories[i] != nullptr)
            {
                m_Rhi->FreeMemory(m_TargetMemories[i]);
            }
        }
        if (m_ParamsMemory != nullptr)
        {
            m_Rhi->UnmapMemory(m_ParamsMemory);
            m_Rhi->FreeMemory(m_ParamsMemory);
        }
        if (m_ParamsBuffer != nullptr)
        {
            m_Rhi->DestroyBuffer(m_ParamsBuffer);
        }
    }

    for (uint32_t i = 0; i < 2; ++i)
    {
        m_Framebuffers[i]   = nullptr;
        m_TargetViews[i]    = nullptr;
        m_TargetImages[i]   = nullptr;
        m_TargetMemories[i] = nullptr;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(Stage::Count); ++i)
    {
        m_Pipelines[i]      = nullptr;
        m_DescriptorSets[i] = nullptr;
    }
    m_TargetExtent        = {0, 0};
    m_ParamsMapped        = nullptr;
    m_ParamsMemory        = nullptr;
    m_ParamsBuffer        = nullptr;
    m_Sampler             = nullptr;
    m_RenderPass          = nullptr;
    m_PipelineLayout      = nullptr;
    m_DescriptorSetLayout = nullptr;
    m_Ready               = false;
    m_OutputValid         = false;
    m_LastViewportCount   = 0;
    m_Rhi                 = nullptr;
}

bool AppleSsaoPass::EnsureTargets(const RHIExtent2D& extent)
{
    if (m_TargetViews[0] != nullptr && m_TargetViews[1] != nullptr && m_Framebuffers[0] != nullptr &&
        m_Framebuffers[1] != nullptr && m_TargetExtent.width == extent.width &&
        m_TargetExtent.height == extent.height)
    {
        return true;
    }

    // 尺寸变了（含第一次）：整套重建。旧的走 RHI 的正规销毁接口。
    for (uint32_t i = 0; i < 2; ++i)
    {
        if (m_Framebuffers[i] != nullptr)
        {
            m_Rhi->DestroyFramebuffer(m_Framebuffers[i]);
            m_Framebuffers[i] = nullptr;
        }
        if (m_TargetViews[i] != nullptr)
        {
            m_Rhi->DestroyImageView(m_TargetViews[i]);
            m_TargetViews[i] = nullptr;
        }
        if (m_TargetImages[i] != nullptr)
        {
            m_Rhi->DestroyImage(m_TargetImages[i]);
            m_TargetImages[i] = nullptr;
        }
        if (m_TargetMemories[i] != nullptr)
        {
            m_Rhi->FreeMemory(m_TargetMemories[i]);
            m_TargetMemories[i] = nullptr;
        }
    }
    m_TargetExtent = {0, 0};

    for (uint32_t i = 0; i < 2; ++i)
    {
        // TRANSFER_SRC 只是给冒烟测试读回用的，不影响运行时。
        m_Rhi->CreateImage(extent.width,
                           extent.height,
                           kTargetFormat,
                           RHI_IMAGE_TILING_OPTIMAL,
                           RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT |
                               RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           m_TargetImages[i],
                           m_TargetMemories[i],
                           0,
                           1,
                           1);
        if (m_TargetImages[i] == nullptr)
        {
            LOG_ERROR(ZRender, "AppleSsaoPass: target image creation failed ({}x{})", extent.width, extent.height);
            return false;
        }
        m_Rhi->CreateImageView(m_TargetImages[i],
                               kTargetFormat,
                               RHI_IMAGE_ASPECT_COLOR_BIT,
                               RHI_IMAGE_VIEW_TYPE_2D,
                               1,
                               1,
                               m_TargetViews[i]);
        if (m_TargetViews[i] == nullptr)
        {
            LOG_ERROR(ZRender, "AppleSsaoPass: target image view creation failed");
            return false;
        }

        RHIFramebufferCreateInfo framebuffer_info {};
        framebuffer_info.sType           = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass      = m_RenderPass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments    = &m_TargetViews[i];
        framebuffer_info.width           = extent.width;
        framebuffer_info.height          = extent.height;
        framebuffer_info.layers          = 1;
        if (!m_Rhi->CreateFramebuffer(&framebuffer_info, m_Framebuffers[i]) || m_Framebuffers[i] == nullptr)
        {
            LOG_ERROR(ZRender, "AppleSsaoPass: framebuffer creation failed");
            return false;
        }
    }

    m_TargetExtent = extent;
    LOG_INFO(ZRender, "AppleSsaoPass: targets (re)built at {}x{}", extent.width, extent.height);
    return true;
}

void AppleSsaoPass::WriteDescriptors(RHIImageView* depth_view, RHIImageView* normal_view)
{
    // 每帧都写：深度图 / GBuffer / 自己的目标在 resize 时都会换句柄。
    //
    //   AO    趟：depth, normal
    //   BlurH 趟：depth, normal, A
    //   BlurV 趟：depth, normal, B
    RHIDescriptorImageInfo depth_info {};
    depth_info.sampler     = m_Sampler;
    depth_info.imageView   = depth_view;
    depth_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo normal_info = depth_info;
    normal_info.imageView              = normal_view;

    RHIDescriptorImageInfo ao_a_info = depth_info;
    ao_a_info.imageView              = m_TargetViews[0];
    RHIDescriptorImageInfo ao_b_info = depth_info;
    ao_b_info.imageView              = m_TargetViews[1];

    RHIWriteDescriptorSet writes[8] {};
    uint32_t              count = 0;
    auto add = [&](RHIDescriptorSet* set, uint32_t binding, RHIDescriptorImageInfo* info) {
        RHIWriteDescriptorSet& w = writes[count++];
        w.sType                  = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet                 = set;
        w.dstBinding             = binding;
        w.dstArrayElement        = 0;
        w.descriptorCount        = 1;
        w.descriptorType         = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo             = info;
    };
    for (uint32_t i = 0; i < static_cast<uint32_t>(Stage::Count); ++i)
    {
        add(m_DescriptorSets[i], kDepthBinding, &depth_info);
        add(m_DescriptorSets[i], kNormalBinding, &normal_info);
    }
    add(m_DescriptorSets[static_cast<uint32_t>(Stage::BlurH)], kAoInputBinding, &ao_a_info);
    add(m_DescriptorSets[static_cast<uint32_t>(Stage::BlurV)], kAoInputBinding, &ao_b_info);
    m_Rhi->UpdateDescriptorSets(count, writes, 0, nullptr);
}

void AppleSsaoPass::RecordStage(RHICommandBuffer*        command_buffer,
                                Stage                    stage,
                                uint32_t                 target_index,
                                const RHIExtent2D&       extent,
                                const AppleSsaoViewport* viewports,
                                uint32_t                 viewport_count,
                                uint32_t                 params_base_offset)
{
    RHIClearValue clear_value {};
    clear_value.color.float32[0] = 1.0f;
    clear_value.color.float32[1] = 1.0f;
    clear_value.color.float32[2] = 0.0f;
    clear_value.color.float32[3] = 1.0f;

    RHIRenderPassBeginInfo begin_info {};
    begin_info.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass      = m_RenderPass;
    begin_info.framebuffer     = m_Framebuffers[target_index];
    begin_info.renderArea      = {{0, 0}, extent};
    begin_info.clearValueCount = 1;
    begin_info.pClearValues    = &clear_value;

    m_Rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);
    m_Rhi->CmdBindPipelinePFN(command_buffer,
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_Pipelines[static_cast<uint32_t>(stage)]);

    for (uint32_t v = 0; v < viewport_count; ++v)
    {
        m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewports[v].viewport);
        m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &viewports[v].scissor);

        // 每个视口自己的参数槽：Metal 没有 dynamic descriptor，offset 在绑定这一刻
        // 算进 setFragmentBuffer:offset:（见 MetalRHI::CmdBindDescriptorSetsPFN）。
        const uint32_t dynamic_offset = params_base_offset + v * kParamsStride;
        m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                        RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_PipelineLayout,
                                        0,
                                        1,
                                        &m_DescriptorSets[static_cast<uint32_t>(stage)],
                                        1,
                                        &dynamic_offset);
        m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
    }

    m_Rhi->CmdEndRenderPassPFN(command_buffer);
}

bool AppleSsaoPass::Record(RHICommandBuffer*        command_buffer,
                           RHIImageView*            depth_view,
                           RHIImageView*            normal_view,
                           const RHIExtent2D&       extent,
                           const AppleSsaoViewport* viewports,
                           uint32_t                 viewport_count,
                           const AppleSsaoSettings& settings)
{
    // 先清掉：这一帧任何一个出口提前返回，GetOutputView() 都必须是 nullptr。
    m_OutputValid       = false;
    m_LastViewportCount = 0;

    if (!m_Ready || m_Rhi == nullptr || command_buffer == nullptr || depth_view == nullptr ||
        normal_view == nullptr || viewports == nullptr || viewport_count == 0 || extent.width == 0 ||
        extent.height == 0 || extent.width > 16384 || extent.height > 16384)
    {
        return false;
    }
    if (!EnsureTargets(extent))
    {
        return false;
    }

    const uint32_t used_viewports = viewport_count < kMaxViewports ? viewport_count : kMaxViewports;
    const uint32_t frame_index    = static_cast<uint32_t>(m_Rhi->GetCurrentFrameIndex()) % m_FramesInFlight;
    const uint32_t params_base    = frame_index * kMaxViewports * kParamsStride;
    for (uint32_t v = 0; v < used_viewports; ++v)
    {
        const AppleSsaoGpuParams params = MakeGpuParams(viewports[v], settings);
        std::memcpy(m_ParamsMapped + params_base + v * kParamsStride, &params, sizeof(params));
    }

    WriteDescriptors(depth_view, normal_view);

    // A ← AO；开模糊时 B ← blurH(A)，A ← blurV(B)。最终结果恒在 A。
    RecordStage(command_buffer, Stage::Ao, 0, extent, viewports, used_viewports, params_base);
    if (settings.blur)
    {
        RecordStage(command_buffer, Stage::BlurH, 1, extent, viewports, used_viewports, params_base);
        RecordStage(command_buffer, Stage::BlurV, 0, extent, viewports, used_viewports, params_base);
    }

    m_OutputValid       = true;
    m_LastViewportCount = used_viewports;
    return true;
}

#else  // !__APPLE__

// 非 Apple 平台：没有 Metal，没有 MSL。保留空实现让头文件在任何平台都能被引用
// （RP1 / 管线模块的调用点本身也包在 __APPLE__ 里）。
bool AppleSsaoPass::Initialize(RHI*) { return false; }
void AppleSsaoPass::Shutdown() {}
bool AppleSsaoPass::Record(RHICommandBuffer*,
                           RHIImageView*,
                           RHIImageView*,
                           const RHIExtent2D&,
                           const AppleSsaoViewport*,
                           uint32_t,
                           const AppleSsaoSettings&)
{
    return false;
}

#endif  // __APPLE__
