#pragma once

#include "Runtime/Function/Render/RenderPass.h"

struct CombineUIPassInitInfo : RenderPassInitInfo
{
    RHIRenderPass* render_pass;
    RHIImageView* scene_input_attachment;
    RHIImageView* ui_input_attachment;
};

class CombineUIPass : public RenderPass
{
public:
    void Initialize(const RenderPassInitInfo* init_info) override final;
    void Draw() override final;

    void UpdateAfterFramebufferRecreate(RHIImageView* scene_input_attachment, RHIImageView* ui_input_attachment);

private:
    void SetupDescriptorSetLayout();
    void SetupPipelines();
    void SetupDescriptorSet();
};