#pragma once

// AppleRp2Chain —— Apple / Metal 上的 RP2（后处理链）。
//
// 为什么不是复用 Passes/ 里那份 MainCameraRp2Pass：
//
//   1. 那份只认 HLSL（`CreateShaderModuleFromFile` 到 `.hlsl`），而 MetalRHI 的
//      `CreateShaderModuleFromFile` **只接受 `.metal`**，HLSL 直接返回 nullptr。
//      把 shader 换成 SPIR-V blob 也不行：`engine/shader/glsl/color_grading.frag`
//      和 `combine_ui.frag` 用的是 `subpassInput`（Vulkan subpass 的 input
//      attachment），Metal 根本没有这个概念 —— 那份 SPIR-V 在 Metal 上编译不了。
//
//   2. RP2 的 step 3（combine_ui）写 swapchain framebuffer，而 Metal 没有持久
//      swapchain image（`GetSwapchainInfo().imageViews` 是空的），那一步在 Apple
//      上永远没有可绑定的目标。Apple 的 UI 本来就是直接画进 swapchain 的
//      （RenderSystem::Tick 的 post-UI 回调），所以也不需要那一步。
//
// 所以 Apple 上的 RP2 只跑**离屏那一段**：step 0（显示变换）读 backup_odd、写
// backup_even，结果再由 AppleSceneBlit 直通搬上 swapchain。附件和 render pass
// 一整套都是现成的 —— MainCameraFramebufferResources 已经建好了
// m_Rp2HdrRenderPass / m_Rp2ColorGradingFramebuffer，之前只是没人用。
//
// 颜色分级（LUT）：
//
//   set 0 / binding 1 绑的是 `RenderResource::m_GlobalRenderResource.
//   m_ColorGradingResource.m_ColorGradingLutTextureImageView` —— 就是
//   rendering.global.json 里 `color_grading_map` 指的那张 256x16 条带
//   （16 个 16x16 切片横向排开：x = 切片(b) + r，y = g）。
//
//   这个仓库里**没有任何一处真的采样过它**：Vulkan 的 glsl/color_grading.frag
//   算出了 `_COLORS` 然后把 `texture(...)` 那行注释掉了，DX12 的
//   hlsl/rp2/color_grading.frag.hlsl 写明 "LUT is bound but unused"。所以采样
//   序（LUT 在 tonemap 前还是后、在 gamma 前还是后）没有既有实现可抄，是本文件
//   自己定的：**tonemap → LUT → gamma**，理由见 .cpp 里 SampleColorGradingLut
//   上方的注释。
//
//   视图为空时用一个 1x1 中性 texel 兜底（和 MainCameraRp2Pass::
//   EnsureFallbackLutTexture 一个思路），这样"没配 LUT"是画面没有分级，而不是
//   采样到一张空贴图导致整屏黑。
//
// 和 AppleSceneBlit 一样：MSL 是写死在 .cpp 里的字符串，只在运行时编译，
// 构建期看不到语法错，所以冒烟测试里有专门的断言盯着。

#include "Runtime/Function/Render/Interface/RHIStruct.h"

class RHI;
class MainCameraFramebufferResources;
class RenderResource;

class AppleRp2Chain
{
public:
    AppleRp2Chain() = default;
    ~AppleRp2Chain() { Shutdown(); }

    AppleRp2Chain(const AppleRp2Chain&)            = delete;
    AppleRp2Chain& operator=(const AppleRp2Chain&) = delete;

    /// `framebuffers` 是借用的（归 DesktopRenderPipelineModule 所有），不能为空。
    /// `render_resource` 只用来每帧取 LUT 视图，可以是 nullptr（那就走兜底 texel）。
    bool Initialize(RHI* rhi, MainCameraFramebufferResources* framebuffers, RenderResource* render_resource);
    void Shutdown();

    bool IsReady() const { return m_Ready; }

    /// 这一帧 RP2 到底能不能跑：管线建好了，且 framebuffer / 源图 / swapchain 尺寸
    /// 都到位。头几帧 swapchain 还是 0x0，附件还不存在，这时必须返回 false。
    bool CanRender() const;

    /// RP2 的产出（backup_even）。resize 会重建 attachment，所以**每帧重取**，别缓存。
    /// 返回 nullptr 表示这一帧 RP2 没跑，调用方必须退回 RP1 的 backup_odd
    /// （那时 blit 得自己做 tonemap）。
    ///
    /// 判据和 CanRender() **同一个**，不是一个近似：只要这里返回了非空，Record()
    /// 那一帧就一定真的写了这张图。两套判据不一致的后果是某一帧 blit 直通读到一张
    /// 从没被写过的图 —— 整屏黑，而且只在 resize 那一帧出现，极难复现。
    RHIImageView* GetOutputView() const;

    /// 把 RP2 那一串录进命令缓冲。必须在 swapchain render pass **打开之前**调：
    /// 它自己会 Begin/End 一个离屏 render pass，两个 pass 套着开是非法的。
    void Record(RHICommandBuffer* command_buffer);

private:
    void WriteSceneDescriptor(RHIImageView* source_view);
    void WriteLutDescriptor(RHIImageView* lut_view);

    /// LUT 视图**每帧重取**：关卡热重载会重新 UploadGlobalRenderResource、换掉
    /// 这张 view，缓存下来就是"换了 LUT 但画面不变"。换掉了才重写描述符。
    RHIImageView* FindLutView() const;
    RHIImageView* ResolveLutView();
    void          EnsureFallbackLutTexture();

    RHI* m_Rhi {nullptr};
    MainCameraFramebufferResources* m_Framebuffers {nullptr};
    RenderResource* m_RenderResource {nullptr};

    RHIDescriptorSetLayout* m_DescriptorSetLayout {nullptr};
    RHIPipelineLayout* m_PipelineLayout {nullptr};
    RHIDescriptorSet* m_DescriptorSet {nullptr};
    RHISampler* m_Sampler {nullptr};
    RHIPipeline* m_Pipeline {nullptr};

    // LUT 没加载成功时的 1x1 中性兜底图，以及"上一次写进描述符的是谁"。
    RHIImage* m_FallbackLutImage {nullptr};
    RHIImageView* m_FallbackLutView {nullptr};
    RHIImageView* m_BoundLutView {nullptr};

    bool m_Ready {false};
};
