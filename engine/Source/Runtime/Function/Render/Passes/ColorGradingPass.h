#pragma once

#include "Runtime/Function/Render/RenderPass.h"

struct ColorGradingPassInitInfo : RenderPassInitInfo
{
    RHIRenderPass* render_pass;
    RHIImageView* input_attachment;
};

class ColorGradingPass : public RenderPass
{
public:
    void Initialize(const RenderPassInitInfo* init_info) override final;
    void Draw() override final;

    void UpdateAfterFramebufferRecreate(RHIImageView* input_attachment);

private:
    void SetupDescriptorSetLayout();
    void SetupPipelines();
    void SetupDescriptorSet();
};