#include "Runtime/Function/Render/Passes/ShadowPassShared.h"

namespace ShadowPassShared
{
    namespace
    {
        RHIDescriptorSetLayout* g_PerMeshLayout = nullptr;
    }  // namespace

    RHIDescriptorSetLayout*& GetPerMeshLayoutPtr()
    {
        return g_PerMeshLayout;
    }

    bool CreatePerMeshDescriptorSetLayout(RHI* rhi, RHIDescriptorSetLayout*& out_layout)
    {
        out_layout = nullptr;
        if (rhi == nullptr)
        {
            return false;
        }

        RHIDescriptorSetLayoutBinding binding {};
        binding.binding = 0;
        binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;

        RHIDescriptorSetLayoutCreateInfo create_info {};
        create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        create_info.bindingCount = 1;
        create_info.pBindings = &binding;

        if (rhi->CreateDescriptorSetLayout(&create_info, out_layout) != RHI_SUCCESS)
        {
            return false;
        }

        g_PerMeshLayout = out_layout;
        return true;
    }
}  // namespace ShadowPassShared
