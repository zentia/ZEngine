#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/MaterialCustomParamSlots.h"

#include <cstdint>

// 网格材质描述符集（set 2）的 binding 布局，单一来源：MainCameraRp1Pass 用它建真正的 layout，
// VT 材质 A/B 测试用它建同一份 layout（测试验证的是「真实布局 + 真实 gbuffer 着色器」，不是测试自己写的一份近似）。
//
//   binding 0          UBO（MeshMaterialUniform，128 字节）
//   binding 1..5       combined image sampler：base color / metallic-roughness / normal / occlusion / emissive
//   binding 6, 7       仅 DX12：VT 页表 / VT 页池（见 MaterialCustomParamSlots.h「材质里的 Virtual Texture 槽位」）
//
// 返回 binding 个数（6 或 8）。`out` 至少要放得下 8 个。
inline uint32_t BuildMeshMaterialLayoutBindings(bool with_virtual_texture, RHIDescriptorSetLayoutBinding (&out)[8])
{
    for (auto& binding : out)
    {
        binding = {};
    }
    out[0].binding = 0;
    out[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    out[0].descriptorCount = 1;
    out[0].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

    out[1].binding = 1;
    out[1].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    out[1].descriptorCount = 1;
    out[1].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 2; i < 6; ++i)
    {
        out[i] = out[1];
        out[i].binding = i;
    }
    if (!with_virtual_texture)
    {
        return 6;
    }
    out[6] = out[1];
    out[6].binding = k_MaterialVtBinding_PageTable;
    out[7] = out[1];
    out[7].binding = k_MaterialVtBinding_Pool;
    return 8;
}
