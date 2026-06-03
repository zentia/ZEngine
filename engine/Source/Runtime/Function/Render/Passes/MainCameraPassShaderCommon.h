#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderGpuResources.h"

#include <array>
#include <map>
#include <string>

namespace MainCameraPassShaderCommon
{
    std::string ToUpperCopy(std::string value);
    bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs);
    bool IsBlendModeEnabled(const std::string& blend);

    RHICullModeFlags ParseCullMode(const std::string& cull);
    RHICompareOp ParseCompareOp(const std::string& ztest);

    template<size_t AttachmentCount>
    void ApplyBlendMode(const std::string& blend, std::array<RHIPipelineColorBlendAttachmentState, AttachmentCount>& attachments);

    const GpuShaderPassData* FindShaderPassByLightMode(const GpuPBRMaterial& material, const char* desired_light_mode);
    const GpuShaderPassData* FindTransparentShaderPass(const GpuPBRMaterial& material);
    bool CanUseRuntimePrimaryShaderPass(RHI* rhi, const GpuPBRMaterial& material);
    bool CanUseRuntimeShaderPass(RHI* rhi,
                                 const GpuPBRMaterial& material,
                                 const GpuShaderPassData* shader_pass);

    struct MeshPipelineKey
    {
        std::string vertex_shader_file;
        std::string fragment_shader_file;
        std::string vertex_entry {"main"};
        std::string fragment_entry {"main"};
        std::string include_directory;
        std::string source_language {"HLSL"};
        std::string render_pipeline {"StandardLit"};
        std::string light_mode {"GBuffer"};
        std::string cull {"Back"};
        std::string ztest {"LEqual"};
        std::string blend {"Off"};
        bool zwrite {true};
        std::map<std::string, std::string> shader_macros;

        bool operator<(const MeshPipelineKey& rhs) const;
    };

    MeshPipelineKey BuildPipelineKey(const GpuPBRMaterial& material, const GpuShaderPassData* shader_pass = nullptr);

}  // namespace MainCameraPassShaderCommon
