#pragma once

#include "Runtime/Function/Render/RenderCommon.h"
#include "Runtime/Function/Render/RenderPassBase.h"
#include "Runtime/Function/Render/RenderResource.h"

#include <memory>
#include <vector>
#if defined(Z_HAS_VULKAN)
    #include <vulkan/vulkan.h>
#endif

#if defined(Z_HAS_VULKAN)
class VulkanRHI;
#endif

enum
{
    _main_camera_pass_gbuffer_a = 0,
    _main_camera_pass_gbuffer_b = 1,
    _main_camera_pass_gbuffer_c = 2,
    _main_camera_pass_backup_buffer_odd = 3,
    _main_camera_pass_backup_buffer_even = 4,
    _main_camera_pass_post_process_buffer_odd = 5,
    _main_camera_pass_post_process_buffer_even = 6,
    _main_camera_pass_depth = 7,
    _main_camera_pass_swap_chain_image = 8,
    _main_camera_pass_custom_attachment_count = 5,
    _main_camera_pass_post_process_attachment_count = 2,
    _main_camera_pass_attachment_count = 9,
};

// PR-V4 part 2: Split the original 8-subpass main_camera render pass into:
//   RP1 (gbuffer + lighting): basepass / deferred_lighting / forward_lighting
//   <bindless tonemap, runs as a stand-alone RenderPass between RP1 and RP2>
//   RP2 (post-FX + UI): color_grading / fxaa / ui / combine_ui
// The legacy `_main_camera_subpass_tone_mapping` value is removed entirely;
// any code path that referenced it has been migrated to BindlessTonemapPass.
enum
{
    // RP1 subpass indices (0..2)
    _main_camera_subpass_basepass = 0,
    _main_camera_subpass_deferred_lighting,
    _main_camera_subpass_forward_lighting,
    _main_camera_rp1_subpass_count,

    // RP2 subpass indices (0..3) -- defined as a SEPARATE numbering space.
    // To keep the original symbolic names usable by downstream passes
    // (color_grading_pass.cpp / fxaa_pass.cpp / ui_pass.cpp /
    // combine_ui_pass.cpp), we redefine them as small integers starting at 0
    // so each pipeline's pipelineInfo.subpass matches its index inside RP2.
    _main_camera_subpass_color_grading = 0,
    _main_camera_subpass_fxaa = 1,
    _main_camera_subpass_ui = 2,
    _main_camera_subpass_combine_ui = 3,
    _main_camera_rp2_subpass_count = 4,
};

struct VisiableNodes
{
    std::vector<RenderMeshNode>* p_directional_light_visible_mesh_nodes {nullptr};
    std::vector<RenderMeshNode>* p_point_lights_visible_mesh_nodes {nullptr};
    std::vector<RenderMeshNode>* p_main_camera_visible_mesh_nodes {nullptr};
    RenderAxisNode* p_axis_node {nullptr};
};

class RenderPass : public RenderPassBase
{
public:
    struct FrameBufferAttachment
    {
        RHIImage* image;
        RHIDeviceMemory* mem;
        RHIImageView* view;
        RHIFormat format;
    };

    struct Framebuffer
    {
        int width;
        int height;
        RHIFramebuffer* framebuffer;
        RHIRenderPass* render_pass;

        std::vector<FrameBufferAttachment> attachments;
    };

    struct Descriptor
    {
        RHIDescriptorSetLayout* layout;
        RHIDescriptorSet* descriptor_set;
    };

    struct RenderPipelineBase
    {
        RHIPipelineLayout* layout;
        RHIPipeline* pipeline;
    };

    GlobalRenderResource* m_GlobalRenderResource {nullptr};

    std::vector<Descriptor> m_DescriptorInfos;
    std::vector<RenderPipelineBase> m_RenderPipelines;
    Framebuffer m_Framebuffer;

    void Initialize(const RenderPassInitInfo* init_info) override;
    void PostInitialize() override;

    // Re-bind m_GlobalRenderResource after m_RenderResource is attached or the
    // upload ringbuffer is lazily created (DX12 defers CreateAndMapStorageBuffer).
    void RefreshGlobalRenderResourcePointer();

    // Refresh pointer and, on DX12, lazily create the upload ringbuffer if needed.
    bool EnsureGlobalRenderResourceReady();

    virtual void Draw();

    virtual RHIRenderPass* GetRenderPass() const;
    virtual std::vector<RHIImageView*> GetFramebufferImageViews() const;
    virtual std::vector<RHIDescriptorSetLayout*> GetDescriptorSetLayouts() const;

    static VisiableNodes m_VisiableNodes;

private:
};