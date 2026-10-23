#pragma once

#include "Runtime/Function/Render/RenderPass.h"
#include "Runtime/Function/Render/RenderCommon.h"

#include <map>
#include <string>

class RenderResourceBase;
struct GpuPBRMaterial;

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
    RHIPipeline* GetOrCreateShadowPipeline(const GpuPBRMaterial& material);

private:
    RHIDescriptorSetLayout* m_PerMeshLayout {nullptr};
    DirectionalLightShadowPerFrame
        m_DirectionalLightShadowPerFrame;
    std::map<ShadowPipelineKey, RHIPipeline*> m_MaterialPipelines;

    // ---- CSM ----
    //
    // shadow map 是一张 4 层的 texture2DArray，一层一级。Metal 没有 geometry
    // shader（ Vulkan 那边靠 gl_Layer 选层），所以"画到哪一层"只能在 CPU 侧
    // 显式指定：每一级一个 framebuffer，framebuffer 上带 array_slice。
    //
    // m_Framebuffer.framebuffer 保留（slice 0，等价老行为）供只画一级时用。
    RHIFramebuffer* m_CascadeFramebuffers[s_MaxCascadeCount] {nullptr, nullptr, nullptr, nullptr};
    uint32_t m_CascadeCount {1};
    // 每一级一份 per-frame：里面只有 light_proj_view，必须各级不同。
    DirectionalLightShadowPerFrame m_CascadeShadowPerFrame[s_MaxCascadeCount];
};