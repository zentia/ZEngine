#pragma once

#include "DebugDrawPrimitive.h"
#include "Runtime/Function/Render/Interface/RHI.h"

struct DebugDrawFrameBufferAttachment
{
    RHIImage* image = nullptr;
    RHIDeviceMemory* mem = nullptr;
    RHIImageView* view = nullptr;
    RHIFormat format;
};

struct DebugDrawFramebuffer
{
    int width;
    int height;
    RHIRenderPass* render_pass = nullptr;

    std::vector<RHIFramebuffer*> framebuffers;
    std::vector<DebugDrawFrameBufferAttachment> attachments;
};

struct DebugDrawPipelineBase
{
    RHIPipelineLayout* layout = nullptr;
    RHIPipeline* pipeline = nullptr;
};

enum DebugDrawPipelineType : uint8_t
{
    _debug_draw_pipeline_type_point = 0,
    _debug_draw_pipeline_type_line,
    _debug_draw_pipeline_type_triangle,
    _debug_draw_pipeline_type_point_no_depth_test,
    _debug_draw_pipeline_type_line_no_depth_test,
    _debug_draw_pipeline_type_triangle_no_depth_test,
    _debug_draw_pipeline_type_count,
};

class DebugDrawPipeline
{
public:
    DebugDrawPipelineType m_PipelineType;
    DebugDrawPipeline(DebugDrawPipelineType pipelineType) { m_PipelineType = pipelineType; }
    void Initialize();
    void Destory();
    const DebugDrawPipelineBase& GetPipeline() const;
    const DebugDrawFramebuffer& GetFramebuffer() const;

    void RecreateAfterSwapchain();

private:
    void SetupAttachments();
    void SetupFramebuffer();
    void SetupRenderPass();
    void SetupDescriptorLayout();
    void SetupPipelines();

    RHIDescriptorSetLayout* m_DescriptorLayout;
    std::vector<DebugDrawPipelineBase> m_RenderPipelines;
    DebugDrawFramebuffer m_Framebuffer;
    std::shared_ptr<RHI> m_Rhi;
};