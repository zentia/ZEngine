// =====================================================================
// AppleSceneBlit —— Metal 上把离屏场景结果搬到 swapchain 的那一次全屏采样。
// ---------------------------------------------------------------------
// 为什么必须存在：Metal **没有持久的 swapchain image**（drawable 每帧换一张，
// MetalRHI.h 里 `GetSwapchainRenderPass/Framebuffer` 的注释就是这么写的），
// 所以 `CmdBlitImage(scene_color -> swapchain)` 这条路在 Metal 上根本没有目标
// 图像可写。于是剩下唯一能走的路就是：在 swapchain pass 里画一个全屏三角形，
// 采样离屏那张场景色。
//
// 采样哪张、要不要顺手做显示变换，取决于 AppleRp2Chain 起没起来：
//   RP2 起来了 —— 源是 backup_even（已 tonemap），这里**直通**；
//   RP2 没起来 —— 源是 backup_odd（线性 HDR），这里自己做 tonemap + gamma。
// 两条管线都在 Initialize 里建好，Record 每帧按 apply_tonemap 挑一条。
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

    bool IsReady() const
    {
        return m_Pipeline != nullptr && m_PipelinePassthrough != nullptr && m_DescriptorSet != nullptr;
    }

    // 在 swapchain pass **内部**调用（begin 之后、end 之前）。source 可以每帧换
    // （resize 会重建 framebuffer），所以每次都重新写 descriptor。
    //
    // `apply_tonemap`：
    //   true  —— source 是 RP1 的线性 HDR（backup_odd），这一步要自己做显示变换。
    //            只有 AppleRp2Chain 没起来时才走这条。
    //   false —— source 是 RP2 的输出，已经是显示值，再变换一次画面就发灰。
    //
    // 每帧决定、不是初始化时决定：blit 是惰性初始化的，而 RP2 是另一个模块建的，
    // 谁先谁后没有保证（RP2 会因为 swapchain 一开始是 0x0 晚一帧才好）。
    void Record(RHICommandBuffer* command_buffer, RHIImageView* source, bool apply_tonemap);

private:
    RHI*                    m_Rhi {nullptr};
    RHIDescriptorSetLayout* m_DescriptorSetLayout {nullptr};
    RHIPipelineLayout*      m_PipelineLayout {nullptr};
    RHIDescriptorSet*       m_DescriptorSet {nullptr};
    RHIPipeline*            m_Pipeline {nullptr};             // tonemap（RP2 缺席时的兜底）
    RHIPipeline*            m_PipelinePassthrough {nullptr};  // 直通（RP2 已变换过）
    RHISampler*             m_Sampler {nullptr};
};

#endif  // __APPLE__
