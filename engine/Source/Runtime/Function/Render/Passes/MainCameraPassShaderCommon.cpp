#include "Runtime/Function/Render/Passes/MainCameraPassShaderCommon.h"

#include <algorithm>
#include <cctype>
#include <tuple>

namespace MainCameraPassShaderCommon
{
    namespace
    {
        bool ContainsIgnoreCase(const std::string& text, const std::string& token)
        {
            return ToUpperCopy(text).find(ToUpperCopy(token)) != std::string::npos;
        }

        const VulkanShaderPassData* FindForwardShaderPass(const VulkanPBRMaterial& material)
        {
            if (const VulkanShaderPassData* shader_pass = FindShaderPassByLightMode(material, "ForwardBase"))
            {
                return shader_pass;
            }
            return FindShaderPassByLightMode(material, "Forward");
        }
    }  // namespace

    std::string ToUpperCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        return value;
    }

    bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
    {
        return ToUpperCopy(lhs) == ToUpperCopy(rhs);
    }

    bool IsBlendModeEnabled(const std::string& blend)
    {
        const std::string normalized_blend = ToUpperCopy(blend);
        return !(normalized_blend.empty() || normalized_blend == "OFF" || normalized_blend == "OPAQUE");
    }

    RHICullModeFlags ParseCullMode(const std::string& cull)
    {
        if (EqualsIgnoreCase(cull, "OFF") || EqualsIgnoreCase(cull, "NONE"))
        {
            return RHI_CULL_MODE_NONE;
        }
        if (EqualsIgnoreCase(cull, "FRONT"))
        {
            return RHI_CULL_MODE_FRONT_BIT;
        }
        return RHI_CULL_MODE_BACK_BIT;
    }

    RHICompareOp ParseCompareOp(const std::string& ztest)
    {
        if (EqualsIgnoreCase(ztest, "NEVER"))
        {
            return RHI_COMPARE_OP_NEVER;
        }
        if (EqualsIgnoreCase(ztest, "LESS"))
        {
            return RHI_COMPARE_OP_LESS;
        }
        if (EqualsIgnoreCase(ztest, "EQUAL"))
        {
            return RHI_COMPARE_OP_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "LEQUAL"))
        {
            return RHI_COMPARE_OP_LESS_OR_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "GREATER"))
        {
            return RHI_COMPARE_OP_GREATER;
        }
        if (EqualsIgnoreCase(ztest, "NOTEQUAL"))
        {
            return RHI_COMPARE_OP_NOT_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "GEQUAL"))
        {
            return RHI_COMPARE_OP_GREATER_OR_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "ALWAYS"))
        {
            return RHI_COMPARE_OP_ALWAYS;
        }
        return RHI_COMPARE_OP_LESS_OR_EQUAL;
    }

    template<size_t AttachmentCount>
    void ApplyBlendMode(const std::string& blend, std::array<RHIPipelineColorBlendAttachmentState, AttachmentCount>& attachments)
    {
        const std::string normalized_blend = ToUpperCopy(blend);
        const bool enable_alpha_blend = normalized_blend == "ALPHA" || normalized_blend == "TRANSPARENT" ||
                                        normalized_blend == "BLEND";
        const bool enable_additive_blend = normalized_blend == "ADDITIVE" || normalized_blend == "ADD";
        const bool enable_premultiply_blend = normalized_blend == "PREMULTIPLY" || normalized_blend == "PREMULTIPLIED";

        for (RHIPipelineColorBlendAttachmentState& attachment : attachments)
        {
            attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT |
                                        RHI_COLOR_COMPONENT_A_BIT;
            attachment.blendEnable = RHI_FALSE;
            attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
            attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
            attachment.colorBlendOp = RHI_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
            attachment.alphaBlendOp = RHI_BLEND_OP_ADD;

            if (enable_alpha_blend)
            {
                attachment.blendEnable = RHI_TRUE;
                attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_SRC_ALPHA;
                attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            }
            else if (enable_additive_blend)
            {
                attachment.blendEnable = RHI_TRUE;
                attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_SRC_ALPHA;
                attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
            }
            else if (enable_premultiply_blend)
            {
                attachment.blendEnable = RHI_TRUE;
                attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            }
        }
    }

    template void ApplyBlendMode<1>(const std::string& blend, std::array<RHIPipelineColorBlendAttachmentState, 1>& attachments);
    template void ApplyBlendMode<3>(const std::string& blend, std::array<RHIPipelineColorBlendAttachmentState, 3>& attachments);

    const VulkanShaderPassData* FindShaderPassByLightMode(const VulkanPBRMaterial& material, const char* desired_light_mode)
    {
        for (const VulkanShaderPassData& shader_pass : material.shader_passes)
        {
            if (EqualsIgnoreCase(shader_pass.light_mode, desired_light_mode != nullptr ? desired_light_mode : ""))
            {
                return &shader_pass;
            }
        }
        return nullptr;
    }

    const VulkanShaderPassData* FindTransparentShaderPass(const VulkanPBRMaterial& material)
    {
        if (const VulkanShaderPassData* shader_pass = FindShaderPassByLightMode(material, "Transparent"))
        {
            return shader_pass;
        }

        for (const VulkanShaderPassData& shader_pass : material.shader_passes)
        {
            if (IsBlendModeEnabled(shader_pass.blend))
            {
                return &shader_pass;
            }
        }

        return nullptr;
    }

    bool CanUseRuntimePrimaryShaderPass(const std::shared_ptr<RHI>& rhi, const VulkanPBRMaterial& material)
    {
        if (material.vertex_shader_file.empty() || material.fragment_shader_file.empty())
        {
            return false;
        }

        if (!material.light_mode.empty() && !EqualsIgnoreCase(material.light_mode, "GBUFFER"))
        {
            return false;
        }

        if (rhi == nullptr)
        {
            return false;
        }

        const GraphicsAPI graphics_api = rhi->getGraphicsAPI();
        if (graphics_api == GraphicsAPI::Vulkan)
        {
            if (!material.enable_vulkan || ContainsIgnoreCase(material.source_language, "HLSL"))
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::DirectX12)
        {
            if (!material.enable_dx12)
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::Metal)
        {
            if (!material.enable_metal)
            {
                return false;
            }
        }

        return true;
    }

    bool CanUseRuntimeShaderPass(const std::shared_ptr<RHI>& rhi,
                                 const VulkanPBRMaterial& material,
                                 const VulkanShaderPassData* shader_pass)
    {
        if (shader_pass == nullptr || shader_pass->vertex_shader_file.empty() || shader_pass->fragment_shader_file.empty())
        {
            return false;
        }

        if (rhi == nullptr)
        {
            return false;
        }

        const GraphicsAPI graphics_api = rhi->getGraphicsAPI();
        if (graphics_api == GraphicsAPI::Vulkan)
        {
            if (!material.enable_vulkan || ContainsIgnoreCase(material.source_language, "HLSL"))
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::DirectX12)
        {
            if (!material.enable_dx12)
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::Metal)
        {
            if (!material.enable_metal)
            {
                return false;
            }
        }

        return true;
    }

    MeshPipelineKey BuildPipelineKey(const VulkanPBRMaterial& material, const VulkanShaderPassData* shader_pass)
    {
        if (shader_pass == nullptr)
        {
            MeshPipelineKey key;
            key.vertex_shader_file = material.vertex_shader_file;
            key.fragment_shader_file = material.fragment_shader_file;
            key.vertex_entry = material.vertex_entry.empty() ? "main" : material.vertex_entry;
            key.fragment_entry = material.fragment_entry.empty() ? "main" : material.fragment_entry;
            key.include_directory = material.include_directory;
            key.source_language = material.source_language;
            key.render_pipeline = material.render_pipeline;
            key.light_mode = material.light_mode;
            key.cull = material.cull;
            key.ztest = material.ztest;
            key.blend = material.blend;
            key.zwrite = material.zwrite;
            key.shader_macros = material.shader_macros;
            return key;
        }

        MeshPipelineKey key;
        key.vertex_shader_file = shader_pass->vertex_shader_file;
        key.fragment_shader_file = shader_pass->fragment_shader_file;
        key.vertex_entry = shader_pass->vertex_entry.empty() ? "main" : shader_pass->vertex_entry;
        key.fragment_entry = shader_pass->fragment_entry.empty() ? "main" : shader_pass->fragment_entry;
        key.include_directory = material.include_directory;
        key.source_language = material.source_language;
        key.render_pipeline =
            shader_pass->render_pipeline.empty() ? material.render_pipeline : shader_pass->render_pipeline;
        key.light_mode = shader_pass->light_mode;
        key.cull = shader_pass->cull.empty() ? material.cull : shader_pass->cull;
        key.ztest = shader_pass->ztest.empty() ? material.ztest : shader_pass->ztest;
        key.blend = shader_pass->blend.empty() ? material.blend : shader_pass->blend;
        key.zwrite = shader_pass->zwrite;

        if (EqualsIgnoreCase(key.light_mode, "Transparent"))
        {
            if (!IsBlendModeEnabled(key.blend))
            {
                key.blend = "Transparent";
            }
            key.zwrite = false;
        }

        key.shader_macros = material.shader_macros;
        return key;
    }

    bool MeshPipelineKey::operator<(const MeshPipelineKey& rhs) const
    {
        return std::tie(vertex_shader_file,
                        fragment_shader_file,
                        vertex_entry,
                        fragment_entry,
                        include_directory,
                        source_language,
                        render_pipeline,
                        light_mode,
                        cull,
                        ztest,
                        blend,
                        zwrite,
                        shader_macros) <
               std::tie(rhs.vertex_shader_file,
                        rhs.fragment_shader_file,
                        rhs.vertex_entry,
                        rhs.fragment_entry,
                        rhs.include_directory,
                        rhs.source_language,
                        rhs.render_pipeline,
                        rhs.light_mode,
                        rhs.cull,
                        rhs.ztest,
                        rhs.blend,
                        rhs.zwrite,
                        rhs.shader_macros);
    }

}  // namespace MainCameraPassShaderCommon
