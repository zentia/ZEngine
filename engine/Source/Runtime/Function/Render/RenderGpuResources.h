#pragma once

#include "Runtime/Function/Render/MaterialCustomParamSlots.h"
#include "Runtime/Function/Render/RenderGPUResource.h"
#include "Runtime/Function/ShaderLab/ShaderPropertyBlob.h"

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

    /// 材质 UBO 当前内容的 CPU 副本（128 字节）。共享材质的 UBO 是用「第一个用到它的 part」的 entity 填的，
    /// 之后没有 entity 可用；VT 槽位要在运行时开/关，只能在这份副本上改几个字段再整块重传。每次写 UBO 时同步更新。
    uint8_t uniform_bytes[128] {};

    /// 这个材质当前的 VT 槽位（enabled = false 就是没有）。由 RenderResource::SyncMaterialVirtualTexture 维护。
    MaterialVirtualTextureSlot vt_slot {};

    RHIDescriptorSet* material_descriptor_set {RHI_NULL_HANDLE};

    /// 五个槽位的采样器。材质实例要复用共享材质的纹理视图，而视图本身不带尺寸
    /// 信息 —— 之前是拿上传时的 `width/height` 现算 `GetOrCreateMipmapSampler(w,h)`，
    /// 实例手里没有那两个数，所以把采样器存下来。RHI 侧按 (w,h) 缓存，存指针和
    /// 重新算拿到的是同一个对象。
    RHISampler* base_color_sampler {RHI_NULL_HANDLE};
    RHISampler* metallic_roughness_sampler {RHI_NULL_HANDLE};
    RHISampler* normal_sampler {RHI_NULL_HANDLE};
    RHISampler* occlusion_sampler {RHI_NULL_HANDLE};
    RHISampler* emissive_sampler {RHI_NULL_HANDLE};

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

    // ---- ShaderLab 具名属性 ------------------------------------------------
    //
    // 内置 StandardLit 材质的 UBO 是 `MeshMaterialUniform` 那套固定布局（基础色 /
    // 金属度 / ... / customParams[4]）；自定义 ShaderLab 材质的 UBO 则是
    // **shader 自己 CBUFFER_START(UnityPerMaterial) 声明出来的布局**。两者互斥，
    // 由 `shaderlab_property_layout` 区分 —— 同一块 128 字节按两套布局解释会静默
    // 读错，所以这个开关必须由材质数据决定，不能猜。
    //
    // `property_defaults` 是"资产值打底"用的：材质实例每帧的 UBO = 它 + per-renderer
    // override 盖上去（见 RenderResource::MaterializeMaterialInstance）。
    bool shaderlab_property_layout {false};
    std::vector<ZEngine::ShaderLab::ShaderPropertySlot> property_slots;
    ZEngine::ShaderLab::ShaderPropertyBlob property_defaults;
};

inline GpuMesh* AsGpuMesh(RenderMeshGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12 ||
           // Metal 走的是同一套 GpuMesh / GpuPBRMaterial 布局（只是 VmaAllocation
           // 那几个字段恒为 nullptr），所以这里必须放行；以前漏了 Metal，Apple 上
           // 一有网格进场景就 assert 崩。
           resource->backend == RenderResourceBackend::Metal);
    return static_cast<GpuMesh*>(resource);
}

inline const GpuMesh* AsGpuMesh(const RenderMeshGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12 ||
           // Metal 走的是同一套 GpuMesh / GpuPBRMaterial 布局（只是 VmaAllocation
           // 那几个字段恒为 nullptr），所以这里必须放行；以前漏了 Metal，Apple 上
           // 一有网格进场景就 assert 崩。
           resource->backend == RenderResourceBackend::Metal);
    return static_cast<const GpuMesh*>(resource);
}

inline GpuPBRMaterial* AsGpuMaterial(RenderMaterialGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12 ||
           // Metal 走的是同一套 GpuMesh / GpuPBRMaterial 布局（只是 VmaAllocation
           // 那几个字段恒为 nullptr），所以这里必须放行；以前漏了 Metal，Apple 上
           // 一有网格进场景就 assert 崩。
           resource->backend == RenderResourceBackend::Metal);
    return static_cast<GpuPBRMaterial*>(resource);
}

inline const GpuPBRMaterial* AsGpuMaterial(const RenderMaterialGPUResource* resource)
{
    assert(resource == nullptr || resource->backend == RenderResourceBackend::Vulkan ||
           resource->backend == RenderResourceBackend::DirectX12 ||
           // Metal 走的是同一套 GpuMesh / GpuPBRMaterial 布局（只是 VmaAllocation
           // 那几个字段恒为 nullptr），所以这里必须放行；以前漏了 Metal，Apple 上
           // 一有网格进场景就 assert 崩。
           resource->backend == RenderResourceBackend::Metal);
    return static_cast<const GpuPBRMaterial*>(resource);
}
