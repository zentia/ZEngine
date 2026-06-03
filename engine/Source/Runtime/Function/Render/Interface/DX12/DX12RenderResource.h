#pragma once

#include "Runtime/Function/Render/RenderGPUResource.h"

struct DX12Mesh : RenderMeshGPUResource
{
    DX12Mesh()
        : RenderMeshGPUResource(RenderResourceBackend::DirectX12) {}
};

struct DX12PBRMaterial : RenderMaterialGPUResource
{
    DX12PBRMaterial()
        : RenderMaterialGPUResource(RenderResourceBackend::DirectX12) {}
};
