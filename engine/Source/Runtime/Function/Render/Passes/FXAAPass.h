#pragma once

#include "Runtime/Function/Render/RenderPass.h"

class WindowUI;

struct FXAAPassInitInfo : RenderPassInitInfo
{
    RHIRenderPass* render_pass;
    RHIImageView* input_attachment;
};

class FXAAPass : public RenderPass
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