#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRenderResource.h"
#include "Runtime/Function/Render/RenderCommon.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/MegaLights/MegaLightsSystem.h"
#include "Runtime/Function/Render/Texture/TextureStreamingManager.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

class RHI;
class RenderPassBase;
class RenderCamera;

struct IBLResource
{
    RHIImage* m_BrdflutTextureImage;
    RHIImageView* m_BrdflutTextureImageView;
    RHISampler* m_BrdflutTextureSampler;
    VmaAllocation m_BrdflutTextureImageAllocation;

    RHIImage* m_IrradianceTextureImage;
    RHIImageView* m_IrradianceTextureImageView;
    RHISampler* m_IrradianceTextureSampler;
    VmaAllocation m_IrradianceTextureImageAllocation;

    RHIImage* m_SpecularTextureImage;
    RHIImageView* m_SpecularTextureImageView;
    RHISampler* m_SpecularTextureSampler;
    VmaAllocation m_SpecularTextureImageAllocation;
};

struct IBLResourceData
{
    void* m_BrdflutTextureImagePixels;
    uint32_t m_BrdflutTextureImageWidth;
    uint32_t m_BrdflutTextureImageHeight;
    RHIFormat m_BrdflutTextureImageFormat;
    std::array<void*, 6> m_IrradianceTextureImagePixels;
    uint32_t m_IrradianceTextureImageWidth;
    uint32_t m_IrradianceTextureImageHeight;
    RHIFormat m_IrradianceTextureImageFormat;
    std::array<void*, 6> m_SpecularTextureImagePixels;
    uint32_t m_SpecularTextureImageWidth;
    uint32_t m_SpecularTextureImageHeight;
    RHIFormat m_SpecularTextureImageFormat;
};

struct ColorGradingResource
{
    RHIImage* m_ColorGradingLutTextureImage;
    RHIImageView* m_ColorGradingLutTextureImageView;
    VmaAllocation m_ColorGradingLutTextureImageAllocation;
};

struct ColorGradingResourceData
{
    void* m_ColorGradingLutTextureImagePixels;
    uint32_t m_ColorGradingLutTextureImageWidth;
    uint32_t m_ColorGradingLutTextureImageHeight;
    RHIFormat m_ColorGradingLutTextureImageFormat;
};

struct TextureDataToUpdate
{
    void* base_color_image_pixels;
    uint32_t base_color_image_width;
    uint32_t base_color_image_height;
    RHIFormat base_color_image_format;
    void* metallic_roughness_image_pixels;
    uint32_t metallic_roughness_image_width;
    uint32_t metallic_roughness_image_height;
    RHIFormat metallic_roughness_image_format;
    void* normal_roughness_image_pixels;
    uint32_t normal_roughness_image_width;
    uint32_t normal_roughness_image_height;
    RHIFormat normal_roughness_image_format;
    void* occlusion_image_pixels;
    uint32_t occlusion_image_width;
    uint32_t occlusion_image_height;
    RHIFormat occlusion_image_format;
    void* emissive_image_pixels;
    uint32_t emissive_image_width;
    uint32_t emissive_image_height;
    RHIFormat emissive_image_format;
    // Mip level counts per slot (texture cook). 1 = single mip / GPU-mipgen via
    // the RHI's miplevels=0 contract for legacy uncompressed uploads; >1 = a
    // pre-supplied compressed+mipped chain (cooked Texture2D) packed in *pixels.
    uint32_t base_color_image_miplevels {1};
    uint32_t metallic_roughness_image_miplevels {1};
    uint32_t normal_roughness_image_miplevels {1};
    uint32_t occlusion_image_miplevels {1};
    uint32_t emissive_image_miplevels {1};
    VulkanPBRMaterial* now_material;
};

struct StorageBuffer

{
    // limits
    uint32_t m_MinUniformBufferOffsetAlignment {256};
    uint32_t m_MinStorageBufferOffsetAlignment {256};
    uint32_t m_MaxStorageBufferRange {1 << 27};
    uint32_t m_NonCoherentAtomSize {256};

    RHIBuffer* m_GlobalUploadRingbuffer;
    RHIDeviceMemory* m_GlobalUploadRingbufferMemory;
    void* m_GlobalUploadRingbufferMemoryPointer;
    std::vector<uint32_t> m_GlobalUploadRingbuffersBegin;
    std::vector<uint32_t> m_GlobalUploadRingbuffersEnd;
    std::vector<uint32_t> m_GlobalUploadRingbuffersSize;

    RHIBuffer* m_GlobalNullDescriptorStorageBuffer;
    RHIDeviceMemory* m_GlobalNullDescriptorStorageBufferMemory;

    // axis
    RHIBuffer* m_AxisInefficientStorageBuffer;
    RHIDeviceMemory* m_AxisInefficientStorageBufferMemory;
    void* m_AxisInefficientStorageBufferMemoryPointer;
};

struct GlobalRenderResource
{
    IBLResource m_IblResource;
    ColorGradingResource m_ColorGradingResource;
    StorageBuffer m_StorageBuffer;
};

class RenderResource : public RenderResourceBase
{
public:
    void clear() override final;

    void CreateAndMapStorageBuffer(std::shared_ptr<RHI> rhi);

    virtual void UploadGlobalRenderResource(std::shared_ptr<RHI> rhi,
                                            LevelResourceDesc level_resource_desc) override final;

    virtual void UploadGameObjectRenderResource(std::shared_ptr<RHI> rhi,
                                                RenderEntity render_entity,
                                                RenderMeshData mesh_data,
                                                RenderMaterialData material_data) override final;

    virtual void UploadGameObjectRenderResource(std::shared_ptr<RHI> rhi,
                                                RenderEntity render_entity,
                                                RenderMeshData mesh_data) override final;

    virtual void UploadGameObjectRenderResource(std::shared_ptr<RHI> rhi,
                                                RenderEntity render_entity,
                                                RenderMaterialData material_data) override final;

    virtual void UpdatePerFrameBuffer(std::shared_ptr<RenderScene> render_scene,
                                      std::shared_ptr<RenderCamera> camera) override final;

    RenderMeshGPUResource& GetEntityMesh(RenderEntity entity);

    RenderMaterialGPUResource& GetEntityMaterial(RenderEntity entity);

    void ResetRingBufferOffset(uint8_t current_frame_index);

    // global rendering resource, include IBL data, global storage buffer
    GlobalRenderResource m_GlobalRenderResource;

    // storage buffer objects
    std::array<MeshPerframeStorageBufferObject, 2> m_MeshPerframeStorageBufferObjects;
    MeshPerframeStorageBufferObject m_MeshPerframeStorageBufferObject;
    MeshPointLightShadowPerframeStorageBufferObject m_MeshPointLightShadowPerframeStorageBufferObject;
    MeshDirectionalLightShadowPerframeStorageBufferObject
        m_MeshDirectionalLightShadowPerframeStorageBufferObject;
    AxisStorageBufferObject m_AxisStorageBufferObject;
    MeshInefficientPickPerframeStorageBufferObject m_MeshInefficientPickPerframeStorageBufferObject;
    std::array<ParticleBillboardPerframeStorageBufferObject, 2>
        m_ParticlebillboardPerframeStorageBufferObjects;
    ParticleBillboardPerframeStorageBufferObject m_ParticlebillboardPerframeStorageBufferObject;
    std::array<ParticleCollisionPerframeStorageBufferObject, 2>
        m_ParticleCollisionPerframeStorageBufferObjects;
    ParticleCollisionPerframeStorageBufferObject m_ParticleCollisionPerframeStorageBufferObject;

    // cached mesh and material
    std::map<size_t, VulkanMesh> m_VulkanMeshes;
    std::map<size_t, VulkanPBRMaterial> m_VulkanPbrMaterials;

    // descriptor set layout in main camera pass will be used when uploading resource
    RHIDescriptorSetLayout* const* m_MeshDescriptorSetLayout {nullptr};
    RHIDescriptorSetLayout* const* m_MaterialDescriptorSetLayout {nullptr};

    // Texture streaming manager
    std::shared_ptr<TextureStreamingManager> m_TextureStreamingManager;

    MegaLights::MegaLightsSystem m_MegaLights;

    // Get texture streaming manager
    std::shared_ptr<TextureStreamingManager> getTextureStreamingManager() const { return m_TextureStreamingManager; }

    MegaLights::MegaLightsSystem& GetMegaLightsSystem() { return m_MegaLights; }
    const MegaLights::MegaLightsSystem& GetMegaLightsSystem() const { return m_MegaLights; }

private:
    void CreateIBLSamplers(std::shared_ptr<RHI> rhi);
    void CreateIBLTextures(std::shared_ptr<RHI> rhi,
                           std::array<std::shared_ptr<TextureData>, 6> irradiance_maps,
                           std::array<std::shared_ptr<TextureData>, 6> specular_maps);

    VulkanMesh& GetOrCreateVulkanMesh(std::shared_ptr<RHI> rhi, RenderEntity entity, RenderMeshData mesh_data);
    VulkanPBRMaterial&
    GetOrCreateVulkanMaterial(std::shared_ptr<RHI> rhi, RenderEntity entity, RenderMaterialData material_data);

    void UpdateMeshData(std::shared_ptr<RHI> rhi,
                        bool enable_vertex_blending,
                        uint32_t index_buffer_size,
                        void* index_buffer_data,
                        uint32_t vertex_buffer_size,
                        struct MeshVertexDataDefinition const* vertex_buffer_data,
                        uint32_t joint_binding_buffer_size,
                        struct MeshVertexBindingDataDefinition const* joint_binding_buffer_data,
                        VulkanMesh& now_mesh);
    void UpdateVertexBuffer(std::shared_ptr<RHI> rhi,
                            bool enable_vertex_blending,
                            uint32_t vertex_buffer_size,
                            struct MeshVertexDataDefinition const* vertex_buffer_data,
                            uint32_t joint_binding_buffer_size,
                            struct MeshVertexBindingDataDefinition const* joint_binding_buffer_data,
                            uint32_t index_buffer_size,
                            uint16_t* index_buffer_data,
                            VulkanMesh& now_mesh);
    void UpdateIndexBuffer(std::shared_ptr<RHI> rhi,
                           uint32_t index_buffer_size,
                           void* index_buffer_data,
                           VulkanMesh& now_mesh);
    void UpdateTextureImageData(std::shared_ptr<RHI> rhi, const TextureDataToUpdate& texture_data);
};