#pragma once

#include "Runtime/Function/Render/RenderPass.h"

#include <map>
#include <string>

class RenderResourceBase;
struct VulkanPBRMaterial;

class DirectionalLightShadowPass : public RenderPass
{
public:
    struct ShadowPipelineKey
    {
        std::string vertex_shader_file;
        std::string fragment_shader_file;
        std::string vertex_entry {"main"};
        std::string fragment_entry {"main"};
        std::string include_directory;
        std::string source_language {"HLSL"};
        std::string cull {"Back"};
        std::string ztest {"Less"};
        bool zwrite {true};
        std::map<std::string, std::string> shader_macros;

        bool operator<(const ShadowPipelineKey& rhs) const;
    };

public:
    void Initialize(const RenderPassInitInfo* init_info) override final;
    void PostInitialize() override final;
    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override final;
    void Draw() override final;

    void setPerMeshLayout(RHIDescriptorSetLayout* layout) { m_PerMeshLayout = layout; }

    // DX12: descriptor bind is deferred until RenderResource::UploadGlobalRenderResource.
    void FinishDescriptorSetup();

private:
    void SetupAttachments();
    void SetupRenderPass();
    void SetupFramebuffer();
    void SetupDescriptorSetLayout();
    void SetupPipelines();
    void SetupDescriptorSet();
    void DrawModel();
    RHIPipeline* GetOrCreateShadowPipeline(const VulkanPBRMaterial& material);

private:
    RHIDescriptorSetLayout* m_PerMeshLayout {nullptr};
    MeshDirectionalLightShadowPerframeStorageBufferObject
        m_MeshDirectionalLightShadowPerframeStorageBufferObject;
    std::map<ShadowPipelineKey, RHIPipeline*> m_MaterialPipelines;
};