#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"

#include <cstdint>

enum class RenderResourceBackend
{
    Unknown,
    Vulkan,
    DirectX12,
    Metal
};

struct RenderMeshGPUResource
{
    explicit RenderMeshGPUResource(RenderResourceBackend in_backend)
        : backend(in_backend) {}
    virtual ~RenderMeshGPUResource() = default;

    RenderResourceBackend backend {RenderResourceBackend::Unknown};
    bool enable_vertex_blending {false};
    uint32_t mesh_vertex_count {0};
    uint32_t mesh_index_count {0};
};

struct RenderMaterialGPUResource
{
    explicit RenderMaterialGPUResource(RenderResourceBackend in_backend)
        : backend(in_backend) {}
    virtual ~RenderMaterialGPUResource() = default;

    RenderResourceBackend backend {RenderResourceBackend::Unknown};
};
