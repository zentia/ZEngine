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

    // Backend-agnostic RHI mesh buffers (Vulkan and DX12 upload into these fields).
    RHIBuffer* mesh_vertex_position_buffer {RHI_NULL_HANDLE};
    RHIBuffer* mesh_vertex_varying_enable_blending_buffer {RHI_NULL_HANDLE};
    RHIBuffer* mesh_vertex_varying_buffer {RHI_NULL_HANDLE};
    // DX12 axis gizmo: raw MeshVertexDataDefinition stream (matches Axis.cpp authoring layout).
    RHIBuffer* mesh_vertex_interleaved_buffer {RHI_NULL_HANDLE};
    RHIBuffer* mesh_index_buffer {RHI_NULL_HANDLE};
};

// Draw-ready view of a RenderMeshGPUResource; use instead of backend-specific casts in passes.
struct MeshDrawData
{
    RHIBuffer* position_buffer {RHI_NULL_HANDLE};
    RHIBuffer* varying_blending_buffer {RHI_NULL_HANDLE};
    RHIBuffer* varying_buffer {RHI_NULL_HANDLE};
    RHIBuffer* interleaved_buffer {RHI_NULL_HANDLE};
    RHIBuffer* index_buffer {RHI_NULL_HANDLE};
    uint32_t index_count {0};

    explicit operator bool() const
    {
        return position_buffer != nullptr && varying_blending_buffer != nullptr && varying_buffer != nullptr &&
               index_buffer != nullptr && index_count > 0;
    }

    bool HasInterleavedStream() const
    {
        return interleaved_buffer != nullptr && index_buffer != nullptr && index_count > 0;
    }
};

inline MeshDrawData getMeshDrawData(const RenderMeshGPUResource* mesh)
{
    if (mesh == nullptr)
    {
        return {};
    }

    return {mesh->mesh_vertex_position_buffer,
            mesh->mesh_vertex_varying_enable_blending_buffer,
            mesh->mesh_vertex_varying_buffer,
            mesh->mesh_vertex_interleaved_buffer,
            mesh->mesh_index_buffer,
            mesh->mesh_index_count};
}

inline bool meshDrawDataIsValid(const RenderMeshGPUResource* mesh)
{
    return static_cast<bool>(getMeshDrawData(mesh));
}

struct RenderMaterialGPUResource
{
    explicit RenderMaterialGPUResource(RenderResourceBackend in_backend)
        : backend(in_backend) {}
    virtual ~RenderMaterialGPUResource() = default;

    RenderResourceBackend backend {RenderResourceBackend::Unknown};
};
