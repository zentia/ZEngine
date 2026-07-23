#pragma once

#include "Runtime/Function/Render/RenderGPUResource.h"

#if defined(Z_HAS_VULKAN)
    #include <vma/vk_mem_alloc.h>
    #include <vulkan/vulkan.h>
#elif defined(__APPLE__) || defined(__EMSCRIPTEN__)
using VmaAllocation = void*;
#else
struct VmaAllocation_T;
typedef struct VmaAllocation_T* VmaAllocation;
#endif

#include <cassert>
#include <map>
#include <string>
#include <vector>

// Concrete GPU mesh/material storage for all RHI backends. RHI buffer/image fields live on
// RenderMeshGPUResource / RenderMaterialGPUResource; backend-specific bookkeeping (Vma, etc.)
// lives on these types. DX12 and Vulkan upload paths both populate m_Meshes / m_Materials.

struct GpuMesh : RenderMeshGPUResource
{
    GpuMesh()
        : RenderMeshGPUResource(RenderResourceBackend::Vulkan) {}

    VmaAllocation mesh_vertex_position_buffer_allocation {nullptr};
    VmaAllocation mesh_vertex_varying_enable_blending_buffer_allocation {nullptr};

    RHIBuffer* mesh_vertex_joint_binding_buffer {RHI_NULL_HANDLE};
    VmaAllocation mesh_vertex_joint_binding_buffer_allocation {nullptr};

    RHIDescriptorSet* mesh_vertex_blending_descriptor_set {RHI_NULL_HANDLE};

    VmaAllocation mesh_vertex_varying_buffer_allocation {nullptr};
    VmaAllocation mesh_index_buffer_allocation {nullptr};
};

struct GpuShaderPassData
{
    std::string name {"GBuffer"};
    std::string light_mode {"GBuffer"};
    std::string vertex_shader_file;
    std::string fragment_shader_file;
    std::string render_pipeline {"StandardLit"};
    std::string vertex_entry {"main"};
    std::string fragment_entry {"main"};
    std::string cull {"Back"};
    std::string ztest {"LEqual"};
    std::string blend {"Off"};
    bool zwrite {true};
};

struct GpuPBRMaterial : RenderMaterialGPUResource
{
    GpuPBRMaterial()
        : RenderMaterialGPUResource(RenderResourceBackend::Vulkan) {}

    RHIImage* base_color_texture_image {RHI_NULL_HANDLE};
    RHIImageView* base_color_image_view {RHI_NULL_HANDLE};
    VmaAllocation base_color_image_allocation {nullptr};

    RHIImage* metallic_roughness_texture_image {RHI_NULL_HANDLE};
    RHIImageView* metallic_roughness_image_view {RHI_NULL_HANDLE};
    VmaAllocation metallic_roughness_image_allocation {nullptr};

    RHIImage* normal_texture_image {RHI_NULL_HANDLE};
    RHIImageView* normal_image_view {RHI_NULL_HANDLE};
    VmaAllocation normal_image_allocation {nullptr};

    RHIImage* occlusion_texture_image {RHI_NULL_HANDLE};
    RHIImageView* occlusion_image_view {RHI_NULL_HANDLE};
    VmaAllocation occlusion_image_allocation {nullptr};

    RHIImage* emissive_texture_image {RHI_NULL_HANDLE};
    RHIImageView* emissive_image_view {RHI_NULL_HANDLE};
    VmaAllocation emissive_image_allocation {nullptr};

    RHIBuffer* material_uniform_buffer {RHI_NULL_HANDLE};
    VmaAllocation material_uniform_buffer_allocation {nullptr};

    RHIDescriptorSet* material_descriptor_set {RHI_NULL_HANDLE};

    std::string shader_name {"StandardLit"};
    std::string shader_asset_file;
    std::string vertex_shader_file;
    std::string fragment_shader_file;
    std::string render_pipeline {"StandardLit"};
    std::string light_mode {"GBuffer"};
    std::string source_language {"HLSL"};
    std::string vertex_entry {"main"};
    std::string fragment_entry {"main"};
    std::string include_directory;
    std::string cull {"Back"};
    std::string ztest {"LEqual"};
    std::string blend {"Off"};
    bool zwrite {true};
    bool enable_dx12 {true};
    bool enable_vulkan {true};
    bool enable_metal {false};

    std::map<std::string, std::string> shader_macros;
    std::vector<GpuShaderPassData> shader_passes;
};

inline GpuMesh* AsGpuMesh(RenderMeshGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12);
    return static_cast<GpuMesh*>(resource);
}

inline const GpuMesh* AsGpuMesh(const RenderMeshGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12);
    return static_cast<const GpuMesh*>(resource);
}

inline GpuPBRMaterial* AsGpuMaterial(RenderMaterialGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12);
    return static_cast<GpuPBRMaterial*>(resource);
}

inline const GpuPBRMaterial* AsGpuMaterial(const RenderMaterialGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12);
    return static_cast<const GpuPBRMaterial*>(resource);
}
