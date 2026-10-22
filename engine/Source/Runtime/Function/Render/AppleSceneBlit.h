// =====================================================================
// AppleSceneBlit —— Metal 上把离屏场景结果搬到 swapchain 的那一次全屏采样。
// ---------------------------------------------------------------------
// 为什么必须存在：Metal **没有持久的 swapchain image**（drawable 每帧换一张，
// MetalRHI.h 里 `GetSwapchainRenderPass/Framebuffer` 的注释就是这么写的），
// 所以 `CmdBlitImage(scene_color -> swapchain)` 这条路在 Metal 上根本没有目标
// 图像可写。RP2 那一套（BindlessTonemap / ColorGrading / CombineUI）也都没有
// Apple 版本。于是剩下唯一能走的路就是：在 swapchain pass 里画一个全屏三角形，
// 采样 RP1 的 backup_odd。
//
// 只在 Apple 上编译。非 Apple 平台整个文件是空的 —— 那边有真正的 RP2。
// =====================================================================

#pragma once

#include "Runtime/Function/Render/Interface/RHIStruct.h"

#if defined(__APPLE__)

class RHI;

class AppleSceneBlit
{
public:
    AppleSceneBlit() = default;
    ~AppleSceneBlit() { Shutdown(); }

    AppleSceneBlit(const AppleSceneBlit&)            = delete;
    AppleSceneBlit& operator=(const AppleSceneBlit&) = delete;

    // swapchain_render_pass 必须就是 swapchain 的那个 pass：管线要按它的附件格式
    // 建（见 MetalRHI::CreateGraphicsPipelines —— 格式是从 render pass 反推的）。
    bool Initialize(RHI* rhi, RHIRenderPass* swapchain_render_pass);
    void Shutdown();

    bool IsReady() const { return m_Pipeline != nullptr && m_DescriptorSet != nullptr; }

    // 在 swapchain pass **内部**调用（begin 之后、end 之前）。source 可以每帧换
    // （resize 会重建 framebuffer），所以每次都重新写 descriptor。
    void Record(RHICommandBuffer* command_buffer, RHIImageView* source);

private:
    RHI*                    m_Rhi {nullptr};
    RHIDescriptorSetLayout* m_DescriptorSetLayout {nullptr};
    RHIPipelineLayout*      m_PipelineLayout {nullptr};
    RHIDescriptorSet*       m_DescriptorSet {nullptr};
    RHIPipeline*            m_Pipeline {nullptr};
    RHISampler*             m_Sampler {nullptr};
};

#endif  // __APPLE__
