#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Render/RenderPass.h"

class RenderResourceBase;

struct PickPassInitInfo : RenderPassInitInfo
{
    RHIDescriptorSetLayout* per_mesh_layout;
};

class PickPass : public RenderPass
{
public:
    void Initialize(const RenderPassInitInfo* init_info) override final;
    void PostInitialize() override final;
    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override final;
    void Draw() override final;

    uint32_t Pick(const Vector2& picked_uv);
    void RecreateFramebuffer();

    MeshInefficientPickPerframeStorageBufferObject m_MeshInefficientPickPerframeStorageBufferObject;

private:
    void SetupAttachments();
    void SetupRenderPass();
    void SetupFramebuffer();
    void SetupDescriptorSetLayout();
    void SetupPipelines();
    void SetupDescriptorSet();

private:
    RHIImage* m_ObjectIdImage = nullptr;
    RHIDeviceMemory* m_ObjectIdImageMemory = nullptr;
    RHIImageView* m_ObjectIdImageView = nullptr;

    RHIDescriptorSetLayout* _per_mesh_layout = nullptr;
};