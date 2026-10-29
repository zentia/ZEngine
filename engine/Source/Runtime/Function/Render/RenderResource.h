#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderGpuResources.h"
#include "Runtime/Function/Render/RenderCommon.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/MegaLights/MegaLightsSystem.h"
#include "Runtime/Function/Render/Texture/TextureStreamingManager.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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
    GpuPBRMaterial* now_material;
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

    void CreateAndMapStorageBuffer(RHI* rhi);

    virtual void UploadGlobalRenderResource(RHI* rhi,
                                            LevelResourceDesc level_resource_desc) override final;

    /// 只上传 IBL（6 张 irradiance + 6 张 specular HDR cubemap + BRDF LUT）。
    ///
    /// Apple 的 `RenderSystem::Initialize` 是精简版，不走
    /// `UploadGlobalRenderResource`，于是 IBL 在 Apple 上**从来没上过 GPU** ——
    /// RP1 只能绑 `EnsureFallbackIblTextures` 造的 4x4 中性灰 cubemap，环境光恒为
    /// 一个常数。这个入口让 Apple 只补 IBL，不用把 texture streaming manager /
    /// MegaLights / storage buffer 那套也拖进来。
    ///
    /// 必须在 `RenderPipeline::Initialize` **之前**调：RP1 的 `SetupDescriptorSets`
    /// 在那里就把 descriptor 写死了，晚一步要额外走
    /// `MainCameraRp1Pass::RefreshMeshGlobalIblDescriptors()` 重绑。
    void UploadIblResources(RHI* rhi, const LevelIBLResourceDesc& ibl_desc);

    /// 只上传颜色分级 LUT（不碰 IBL 的 6 张 HDR cubemap）。
    ///
    /// Apple 的 `RenderSystem::Initialize` 是精简版，不走
    /// `UploadGlobalRenderResource`（那条路要先把整个 GlobalRenderingRes 读出来），
    /// 于是 LUT 在 Apple 上**从来没上过 GPU** —— RP2 的第二个描述符槽只能绑到
    /// 1x1 兜底 texel。这个入口让 Apple 只补 LUT 这一件。
    void UploadColorGradingLut(RHI* rhi, const eastl::string& color_grading_path);

    virtual void UploadGameObjectRenderResource(RHI* rhi,
                                                RenderEntity render_entity,
                                                RenderMeshData mesh_data,
                                                RenderMaterialData material_data) override final;

    virtual void UploadGameObjectRenderResource(RHI* rhi,
                                                RenderEntity render_entity,
                                                RenderMeshData mesh_data) override final;

    virtual void UploadGameObjectRenderResource(RHI* rhi,
                                                RenderEntity render_entity,
                                                RenderMaterialData material_data) override final;

    /// 见 RenderResourceBase 上的注释。函数体在文件末尾（inline），因为
    /// RenderResource.cpp / RenderResourceDx12.cpp 各自被平台宏守着，而这段逻辑
    /// 与后端无关，必须两个平台都编到。
    void UploadGameObjectMaterialInstance(RHI* rhi, RenderEntity render_entity) override;

    virtual void UpdatePerFrameBuffer(std::shared_ptr<RenderScene> render_scene,
                                      std::shared_ptr<RenderCamera> camera) override final;

    RenderMeshGPUResource& GetEntityMesh(RenderEntity entity);

    RenderMaterialGPUResource& GetEntityMaterial(RenderEntity entity);

#if defined(_WIN32)
    // DX12 mesh upload used when Win+Vulkan build runs the DX12 RHI at runtime.
    void UploadGpuMeshDx12(RHI* rhi, GpuMesh& now_mesh, RenderMeshData& mesh_data);
#endif

    bool HasValidMesh(size_t mesh_asset_id) const override
    {
        const auto it = m_Meshes.find(mesh_asset_id);
        return it != m_Meshes.end() && meshDrawDataIsValid(&it->second);
    }

    void ResetRingBufferOffset(uint8_t current_frame_index);

    // global rendering resource, include IBL data, global storage buffer
    GlobalRenderResource m_GlobalRenderResource;

    // storage buffer objects
    std::array<MainCameraPerFrame, 2> m_MainCameraPerFrameByViewport;
    MainCameraPerFrame m_MainCameraPerFrame;
    PointLightShadowPerFrame m_PointLightShadowPerFrame;
    DirectionalLightShadowPerFrame
        m_DirectionalLightShadowPerFrame;
    // CSM：方向光阴影每一级的 light proj-view + 生效级数。shadow pass 靠它做
    // N 次 draw（一次一级，各画到 array 的一层）。
    //
    // 之所以单独放一份而不是让 pass 去读 m_MainCameraPerFrame 里的级联矩阵：
    // 那份是"采样侧"的数据（按 viewport 分、给主相机 shader 用），而这里是
    // "生成侧"的数据（给 shadow pass 上传 per-frame SSBO 用）。混用会在
    // 多个 viewport 各自更新时互相踩。
    Matrix4x4 m_DirectionalLightCascadeProjView[s_MaxCascadeCount];
    uint32_t m_DirectionalLightCascadeCount {1};
    AxisDrawStorage m_AxisDrawStorage;
    PickPassPerFrame m_PickPassPerFrame;
    std::array<ParticleBillboardPerFrame, 2>
        m_ParticleBillboardPerFrameByViewport;
    ParticleBillboardPerFrame m_ParticleBillboardPerFrame;
    std::array<ParticleCollisionPerFrame, 2>
        m_ParticleCollisionPerFrameByViewport;
    ParticleCollisionPerFrame m_ParticleCollisionPerFrame;

    // cached mesh and material (shared GpuMesh/GpuPBRMaterial storage for Vulkan and DX12).
    std::map<size_t, GpuMesh> m_Meshes;
    std::map<size_t, GpuPBRMaterial> m_Materials;

    /// 运行时材质实例，键是 `RenderEntity::m_InstanceId`（一个 part 一份）。
    ///
    /// 与 `m_Materials` 分开是必须的：那边按材质资产 id 索引，两个 allocator 的 id
    /// 空间会撞。这里的 id 由 RenderScene 的 `m_InstanceIdAllocator` 回收复用，所以
    /// map 规模的上界是"同时带 override 的 part 数"。
    std::map<uint32_t, GpuPBRMaterial> m_MaterialInstances;

    // descriptor set layout in main camera pass will be used when uploading resource
    RHIDescriptorSetLayout* const* m_MeshDescriptorSetLayout {nullptr};
    RHIDescriptorSetLayout* const* m_MaterialDescriptorSetLayout {nullptr};

    // Texture streaming manager
    std::shared_ptr<TextureStreamingManager> m_TextureStreamingManager;

    MegaLights::MegaLightsSystem m_MegaLights;

    // Get texture streaming manager
    std::shared_ptr<TextureStreamingManager> getTextureStreamingManager() const { return m_TextureStreamingManager; }

    // ---- 材质里的 Virtual Texture 槽位（base color 经 VT 采样；仅 DX12 的网格材质布局有 binding 6/7）----
    //
    /// RHI 线程。每帧在 VT 的 GPU 半帧之后调用一次。`desired.enabled == false` 表示「现在不该有 VT 材质」。
    ///   · 槽位内容变化（开/关、纹理换了、参数变了）：所有内置布局的材质（共享 + 实例）改 UBO 字节并整块重传；
    ///     之后创建的材质也带着这个槽位（见 GetOrCreateMaterial）。
    ///   · 启用状态下**每帧**重写 binding 6（页表）/ 7（页池 + LINEAR/clamp 采样器）——页池/页表可能重建，
    ///     DX12 的 SRV 又是按帧循环的，写一次不够。
    /// ShaderLab 自定义布局的材质不动（它们的 UBO 是 shader 自己的布局）。
    void SyncMaterialVirtualTexture(RHI* rhi,
                                    const MaterialVirtualTextureSlot& desired,
                                    RHIImageView* page_table_view,
                                    RHIImageView* pool_view);
    const MaterialVirtualTextureSlot& GetMaterialVirtualTextureSlot() const { return m_MaterialVtSlot; }
    /// 观测：槽位变化导致的材质 UBO 重传次数 / 每帧 binding 重写的材质描述符集次数（测试和日志用）。
    uint64_t GetMaterialVtUniformReuploads() const { return m_MaterialVtUniformReuploads; }
    uint64_t GetMaterialVtBindingWrites() const { return m_MaterialVtBindingWrites; }

    MegaLights::MegaLightsSystem& GetMegaLightsSystem() { return m_MegaLights; }
    const MegaLights::MegaLightsSystem& GetMegaLightsSystem() const { return m_MegaLights; }

private:
    MaterialVirtualTextureSlot m_MaterialVtSlot {};
    RHISampler* m_MaterialVtSampler {nullptr};
    uint64_t m_MaterialVtUniformReuploads {0};
    uint64_t m_MaterialVtBindingWrites {0};
    /// 把 material.uniform_bytes 整块传到 material_uniform_buffer（staging + 立即拷贝）。
    void ReuploadMaterialUniform(RHI* rhi, GpuPBRMaterial& material);
    void WriteMaterialVtBindings(RHI* rhi, GpuPBRMaterial& material, RHIImageView* page_table_view, RHIImageView* pool_view);

    void CreateIBLSamplers(RHI* rhi);
    void CreateIBLTextures(RHI* rhi,
                           std::array<std::shared_ptr<TextureData>, 6> irradiance_maps,
                           std::array<std::shared_ptr<TextureData>, 6> specular_maps);

    GpuMesh& GetOrCreateMesh(RHI* rhi, RenderEntity entity, RenderMeshData mesh_data);
    GpuPBRMaterial&
    GetOrCreateMaterial(RHI* rhi, RenderEntity entity, RenderMaterialData material_data);

    /// 把 `instance`（已克隆好 CPU 字段 / 纹理视图 / 采样器）变成一份能上 GPU 的材质：
    /// uniform buffer 和描述符集为空就建、已有就只重写内容，然后把 entity 的 8 个标量
    /// 写进 UBO、把 5 个纹理槽 + UBO 写进描述符集。
    ///
    /// 后端相关（Vulkan 用 Vma 分配 + staging copy，DX12 用 CopyBufferImmediate），
    /// 两个 RenderResource 实现各写一份。**必须幂等**：脚本每帧改动 override 时
    /// UploadGameObjectMaterialInstance 会反复调到它，重建 buffer 就是漏显存。
    void MaterializeMaterialInstance(RHI* rhi, RenderEntity entity, GpuPBRMaterial& instance);

    void UpdateMeshData(RHI* rhi,
                        bool enable_vertex_blending,
                        uint32_t index_buffer_size,
                        void* index_buffer_data,
                        uint32_t vertex_buffer_size,
                        struct MeshVertexDataDefinition const* vertex_buffer_data,
                        uint32_t joint_binding_buffer_size,
                        struct MeshVertexBindingDataDefinition const* joint_binding_buffer_data,
                        GpuMesh& now_mesh);
    void UpdateVertexBuffer(RHI* rhi,
                            bool enable_vertex_blending,
                            uint32_t vertex_buffer_size,
                            struct MeshVertexDataDefinition const* vertex_buffer_data,
                            uint32_t joint_binding_buffer_size,
                            struct MeshVertexBindingDataDefinition const* joint_binding_buffer_data,
                            uint32_t index_buffer_size,
                            uint16_t* index_buffer_data,
                            GpuMesh& now_mesh);
    void UpdateIndexBuffer(RHI* rhi,
                           uint32_t index_buffer_size,
                           void* index_buffer_data,
                           GpuMesh& now_mesh);
    void UpdateTextureImageData(RHI* rhi, const TextureDataToUpdate& texture_data);
};

// ---- 运行时材质实例 -------------------------------------------------------
//
// 这段刻意内联在头文件里：RenderResource.cpp（Vulkan / Metal / WebGL2）和
// RenderResourceDx12.cpp 各自被平台宏守卫着，而"要不要建实例、建完怎么复用"这段
// 逻辑与后端无关，必须两个平台都编到。真正与后端相关的只有下面两个私有方法。

/// 材质 UBO 只有 128 字节，ShaderLab 的具名属性就落在这同一块里 —— 改
/// MeshMaterialUniform 必须同步改 k_ShaderPropertyBlobBytes，否则反射算出来的
/// offset 会指到 buffer 外面去。
static_assert(sizeof(MeshMaterialUniform) == ZEngine::ShaderLab::k_ShaderPropertyBlobBytes,
              "MeshMaterialUniform and the ShaderLab property blob must be the same size");

/// 把材质 UBO 的 128 个字节填进 `out_bytes`。
///
/// 两种布局二选一，由 `material.shaderlab_property_layout` 决定：
///   · false（内置 StandardLit）= MeshMaterialUniform 那套固定字段；
///   · true（自定义 ShaderLab）= shader 自己 CBUFFER_START(UnityPerMaterial) 声明
///     出来的布局，属性按导入期反射出的 offset 落位，资产默认值打底、per-renderer
///     的 override 盖在上面。
///
/// 同一块字节按两套布局解释会**静默**读错（比如把 `_Amount` 当成 metallicFactor
/// 读走），所以这个开关只能由材质数据给，绝不能猜。
inline void FillMaterialUniformBytes(const GpuPBRMaterial& material, const RenderEntity& entity, uint8_t* out_bytes)
{
    std::memset(out_bytes, 0, ZEngine::ShaderLab::k_ShaderPropertyBlobBytes);

    if (material.shaderlab_property_layout)
    {
        ZEngine::ShaderLab::ShaderPropertyBlob final_blob = material.property_defaults;
        ZEngine::ShaderLab::ApplyOverride(entity.m_PropertyBlob, final_blob);
        std::memcpy(out_bytes, final_blob.bytes, ZEngine::ShaderLab::k_ShaderPropertyBlobBytes);
        return;
    }

    MeshMaterialUniform& ubo = *reinterpret_cast<MeshMaterialUniform*>(out_bytes);
    ubo.is_blend = entity.m_Blend;
    ubo.is_double_sided = entity.m_DoubleSided;
    ubo.baseColorFactor = entity.m_BaseColorFactor;
    ubo.metallicFactor = entity.m_MetallicFactor;
    ubo.roughnessFactor = entity.m_RoughnessFactor;
    ubo.normalScale = entity.m_NormalScale;
    ubo.occlusionStrength = entity.m_OcclusionStrength;
    ubo.emissiveFactor = entity.m_EmissiveFactor;
    for (uint32_t param_index = 0; param_index < k_MaterialCustomParamCount; ++param_index)
    {
        ubo.customParams[param_index] = entity.m_CustomParams[param_index];
    }
    // VT 槽位盖在最后：启用时它占用 vt_flags 和 customParams[2..3]，覆盖 entity 在那两个自留槽里写的值。
    ApplyMaterialVirtualTextureSlot(material.vt_slot, out_bytes);
}

/// 一个贴图槽的上传源：像素 + 尺寸 + 格式。
struct MaterialSlotTextureSource
{
    void* pixels {nullptr};
    uint32_t width {1};
    uint32_t height {1};
    RHIFormat format {RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM};
};

/// ShaderLab 2D 属性逐槽盖在 PBR 五张贴图之上。
///
/// 槽位是**复用** binding 1..5 的（材质描述符集只有 6 个 binding，没有为自定义
/// shader 另开槽，见 ShaderTextureSlot.h），所以覆盖必须发生在"上传源"这一层：
/// 描述符集的 layout、绑定顺序、池大小全都不用动。
///
/// 空的槽不动 —— 那个槽继续用对应的 PBR 贴图。于是"没有 2D 属性的材质"五个槽
/// 全空，行为和加这个功能之前一模一样。
inline void ApplyShaderLabTextureSlots(const RenderMaterialData& material_data,
                                       MaterialSlotTextureSource (&slots)[ZEngine::ShaderLab::k_MaterialTextureSlotCount])
{
    for (uint32_t slot = 0; slot < ZEngine::ShaderLab::k_MaterialTextureSlotCount; ++slot)
    {
        const std::shared_ptr<TextureData>& texture = material_data.m_TextureSlotTextures[slot];
        if (texture == nullptr || texture->m_Pixels == nullptr)
        {
            continue;
        }
        slots[slot].pixels = texture->m_Pixels;
        slots[slot].width = static_cast<uint32_t>(texture->m_Width);
        slots[slot].height = static_cast<uint32_t>(texture->m_Height);
        slots[slot].format = texture->m_Format;
    }
}


inline void RenderResource::UploadGameObjectMaterialInstance(RHI* rhi, RenderEntity render_entity)
{
    if (rhi == nullptr || render_entity.m_MaterialInstanceId == 0)
    {
        return;
    }

    // 共享材质必须先存在：实例是它的克隆 —— 纹理视图、采样器、shader 全从它来。
    const auto base_it = m_Materials.find(render_entity.m_MaterialAssetId);
    if (base_it == m_Materials.end())
    {
        return;
    }

    const uint32_t instance_id = render_entity.m_MaterialInstanceId;
    GpuPBRMaterial& instance = m_MaterialInstances[instance_id];

    // UBO 和描述符集是实例自己的，绝不能被下面那一赋值盖成共享材质的 —— 否则
    // 写入实例标量会直接改到共享 UBO 上，所有用这个资产的 part 一起变色。
    const RHIBuffer* own_uniform_buffer = instance.material_uniform_buffer;
    const VmaAllocation own_uniform_allocation = instance.material_uniform_buffer_allocation;
    const RHIDescriptorSet* own_descriptor_set = instance.material_descriptor_set;

    instance = base_it->second;

    instance.material_uniform_buffer = const_cast<RHIBuffer*>(own_uniform_buffer);
    instance.material_uniform_buffer_allocation = const_cast<VmaAllocation>(own_uniform_allocation);
    instance.material_descriptor_set = const_cast<RHIDescriptorSet*>(own_descriptor_set);

    // 幂等：buffer / 描述符集为空就建，已有就只重写内容。基础资产换了（instance id
    // 被回收后重新分配给另一个 part）也走同一条路 —— 6 个绑定每次全量重写。
    MaterializeMaterialInstance(rhi, render_entity, instance);
}