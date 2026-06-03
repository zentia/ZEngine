#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"

// Shared helpers for directional / point-light shadow passes (DX12 + Vulkan).

namespace ShadowPassShared
{
    // MainCameraPass::_per_mesh equivalent: one storage buffer for optional skinning indices.
    bool CreatePerMeshDescriptorSetLayout(RHI* rhi, RHIDescriptorSetLayout*& out_layout);

    // Pointer stable for RenderResource::m_MeshDescriptorSetLayout (set during pipeline init).
    RHIDescriptorSetLayout*& GetPerMeshLayoutPtr();
}  // namespace ShadowPassShared
