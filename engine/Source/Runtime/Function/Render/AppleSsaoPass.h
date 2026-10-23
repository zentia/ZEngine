#pragma once

// AppleSsaoPass —— Apple / Metal 上的屏幕空间环境光遮蔽（SSAO）。
//
// 帧内位置（RP1 内部，见 DesktopRenderPipelineModule 的 MainCamera 那一站）：
//
//     GBuffer pass ──► [AppleSsaoPass: AO → 横向模糊 → 纵向模糊] ──► 延迟光照 ──► forward
//                         读 depth + GBufferA(法线)        结果 RG8：r=间接光可见度
//                                                                  g=直接光乘子
//
// 算法：Alchemy AO（McGuire 2011），逐项对齐 Unity URP 的 SSAO.hlsl（同样的采样
// 点生成 / Interleaved Gradient Noise / kBeta / kContrast / 半径归一化 / 法线几何
// 权重的双边模糊）。和 URP 不同的一点：**全程在世界空间算**，不在视空间 —— 引擎的
// per-frame 数据里只有 proj_view 而没有单独的 view / proj，世界空间版只需要
// proj_view 和它的逆，与 deferred_lighting.frag 反推世界坐标是同一条路。
//
// 遮蔽怎么用（deferred_lighting_ssao.frag → mesh_lighting.inl 的
// Z_DEFERRED_SCREEN_SPACE_AO 块）：
//   * 环境光 / IBL 漫反射 × r；
//   * IBL 镜面反射 × Lagarde 的 specular occlusion（UE 的 GetSpecularOcclusion）；
//   * 直接光 × g = lerp(1, r, DirectLightingStrength)（URP 的做法，默认 0.25）。
//
// 为什么是手写 MSL、而不是 GLSL → SPIR-V：和 AppleRp2Chain 同一个理由 —— 这是
// Apple 专属的一段后处理，SPIR-V 路线要同时在 Vulkan 的 MainCameraPass 里有一份
// 对应的描述符布局才算"接上"，而那条路径本机编不到、验不了。
//
// ⚠️ 屏幕坐标的约定**不靠猜**：样本点投影回屏幕时，NDC→像素的映射（包括 y 轴朝向）
// 是在 fragment 里用 `dfdx(ndc.x) / dfdy(ndc.y)` 现场求出来的 —— 全屏三角形输出的
// ndc 和网格光栅化用的是同一套视口变换，所以导数天然带对了符号。写死
// `v = 0.5 - y*0.5` 这种翻转在 Metal 上恰好对、换个后端或者换个 viewport 约定就
// 整张 AO 上下错位，而且画面上只是"遮蔽位置怪怪的"，不报任何错。
//
// 和 AppleSceneBlit / AppleRp2Chain 一样：MSL 是写死在 .cpp 里的字符串，只在
// 运行时编译。BuildVertexSource()/BuildFragmentSource() 是**公开**的 —— 冒烟测试
// 拿的就是真身拼出来的那份源码，不自己复制一份。

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <cstdint>
#include <string>

class RHI;

/// 一个视口一份：视口/裁剪矩形（像素）+ 这台相机的 proj_view。
struct AppleSsaoViewport
{
    RHIViewport viewport {};
    RHIRect2D   scissor {};
    Matrix4x4   proj_view {Matrix4x4::IDENTITY};
    Vector3     camera_position {0.0f, 0.0f, 0.0f};
};

/// 这一帧的参数（调用方从 SsaoSettings 取，测试直接给）。
struct AppleSsaoSettings
{
    float    radius {0.5f};
    float    intensity {1.0f};
    float    direct_lighting_strength {0.25f};
    uint32_t sample_count {12};
    bool     blur {true};
};

/// GPU 侧的参数块。**必须和 MSL 里的 `struct SsaoParams` 逐字段一致**（冒烟测试
/// 有一条文本契约盯着字段顺序）。每个视口一份，按 256 字节对齐放在同一个 buffer
/// 里（macOS 上 constant 地址空间的 buffer offset 必须是 256 的倍数）。
struct AppleSsaoGpuParams
{
    float proj_view[16];
    float inv_proj_view[16];
    float camera_position[4];  // xyz
    float viewport[4];         // x, y, width, height（像素）
    float settings[4];         // radius, intensity, direct_lighting_strength, sample_count
    float blur[4];             // 平面距离容差（以 radius 为单位）, 0, 0, 0
};
static_assert(sizeof(AppleSsaoGpuParams) == 192, "AppleSsaoGpuParams must mirror the MSL SsaoParams (192 bytes)");

class AppleSsaoPass
{
public:
    enum class Stage : uint32_t
    {
        Ao    = 0,  // 原始 AO：读 depth + normal，写 A
        BlurH = 1,  // 横向双边模糊：读 A，写 B
        BlurV = 2,  // 纵向双边模糊：读 B，写 A（并算出 g = 直接光乘子）
        Count = 3,
    };

    static constexpr uint32_t   kMaxViewports      = 2;    // game + scene
    static constexpr uint32_t   kParamsStride      = 256;  // constant buffer offset 对齐
    static constexpr RHIFormat  kTargetFormat      = RHI_FORMAT_R8G8_UNORM;
    // MSL 的手工槽位（MetalRHI：texture = set*8 + binding，buffer = 那个数 + 8）。
    static constexpr uint32_t   kParamsBinding     = 0;  // [[buffer(8)]]
    static constexpr uint32_t   kDepthBinding      = 1;  // [[texture(1)]]
    static constexpr uint32_t   kNormalBinding     = 2;  // [[texture(2)]]
    static constexpr uint32_t   kAoInputBinding    = 3;  // [[texture(3)]]

    AppleSsaoPass() = default;
    ~AppleSsaoPass() { Shutdown(); }

    AppleSsaoPass(const AppleSsaoPass&)            = delete;
    AppleSsaoPass& operator=(const AppleSsaoPass&) = delete;

    /// 只需要一个 RHI（render pass / 附件全是自己的），所以测试可以无头建。
    bool Initialize(RHI* rhi);
    void Shutdown();

    bool IsReady() const { return m_Ready; }

    /// 录 AO（+ 两趟模糊）。必须在**没有** render pass 开着的时候调（它自己开关
    /// 三个离屏 pass）。`depth_view` 是场景深度（D32，带 SAMPLED），`normal_view`
    /// 是 GBufferA（世界法线，RGBA8 编码 n*0.5+0.5）。
    ///
    /// 返回值 == 这一帧 GetOutputView() 是否非空：两者是**同一个**判据（Record
    /// 每帧都会先把它清掉），不存在"Record 说没跑、Output 却给了一张旧图"。
    bool Record(RHICommandBuffer*         command_buffer,
                RHIImageView*             depth_view,
                RHIImageView*             normal_view,
                const RHIExtent2D&        extent,
                const AppleSsaoViewport*  viewports,
                uint32_t                  viewport_count,
                const AppleSsaoSettings&  settings);

    /// 这一帧的 AO 结果（RG8）。这一帧没跑（关掉 / 没准备好 / 输入缺失）就是
    /// nullptr —— 调用方必须退回 1x1 白图，**不能**拿上一帧（或 resize 前）的图。
    RHIImageView* GetOutputView() const { return m_OutputValid ? m_TargetViews[0] : nullptr; }
    RHIImage*     GetOutputImage() const { return m_OutputValid ? m_TargetImages[0] : nullptr; }

    uint32_t GetLastViewportCount() const { return m_LastViewportCount; }

    static std::string BuildVertexSource();
    static std::string BuildFragmentSource(Stage stage);

    /// 把一个视口的输入组装成 GPU 参数块（纯 CPU，测试也直接调它对拍）。
    static AppleSsaoGpuParams MakeGpuParams(const AppleSsaoViewport& viewport, const AppleSsaoSettings& settings);

private:
    bool EnsureTargets(const RHIExtent2D& extent);
    void WriteDescriptors(RHIImageView* depth_view, RHIImageView* normal_view);
    void RecordStage(RHICommandBuffer*        command_buffer,
                     Stage                    stage,
                     uint32_t                 target_index,
                     const RHIExtent2D&       extent,
                     const AppleSsaoViewport* viewports,
                     uint32_t                 viewport_count,
                     uint32_t                 params_base_offset);

    RHI* m_Rhi {nullptr};

    RHIDescriptorSetLayout* m_DescriptorSetLayout {nullptr};
    RHIPipelineLayout*      m_PipelineLayout {nullptr};
    RHIRenderPass*          m_RenderPass {nullptr};
    RHISampler*             m_Sampler {nullptr};
    RHIPipeline*            m_Pipelines[static_cast<uint32_t>(Stage::Count)] {};
    RHIDescriptorSet*       m_DescriptorSets[static_cast<uint32_t>(Stage::Count)] {};

    // 参数：frames_in_flight × kMaxViewports 个 256 字节槽，常驻映射。
    RHIBuffer*       m_ParamsBuffer {nullptr};
    RHIDeviceMemory* m_ParamsMemory {nullptr};
    uint8_t*         m_ParamsMapped {nullptr};
    uint32_t         m_FramesInFlight {1};

    // 两张 RG8 目标：A（最终结果）/ B（横向模糊的中间结果）。
    RHIImage*        m_TargetImages[2] {};
    RHIDeviceMemory* m_TargetMemories[2] {};
    RHIImageView*    m_TargetViews[2] {};
    RHIFramebuffer*  m_Framebuffers[2] {};
    RHIExtent2D      m_TargetExtent {0, 0};

    bool     m_Ready {false};
    bool     m_OutputValid {false};
    uint32_t m_LastViewportCount {0};
};
