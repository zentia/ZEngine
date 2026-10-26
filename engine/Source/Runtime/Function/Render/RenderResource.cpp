#include "Runtime/Function/Render/RenderResource.h"

#include "Runtime/Core/Math/LargeWorldCoordinates.h"

// 这个 TU 是 **RenderResource 的非 DX12 实现**：Vulkan 用它，Apple（Metal）也用它。
//
// 以前整段包在 `#if defined(Z_HAS_VULKAN)` 里，于是 Apple 上一个 RenderResource 的
// 虚函数都不存在 —— 只剩文件末尾那个专门补 vtable 的 clear() 桩。那样 Apple 永远
// 不可能有场景渲染：UploadGameObjectRenderResource / GetOrCreateMaterial
// / CreateIBLTextures 全都没有定义。
//
// 注意守卫**不含 Emscripten**：WebGL2 后端的覆盖面和 Metal 差得远，而本机没法验证
// 它，所以那里继续走文件末尾的 clear() 桩。想把 Web 也拉进来，得先把 WebGL2 的
// buffer / descriptor 路径补齐再说。
//
// 【更新】WebGL2 的命令录制/回放 + buffer/descriptor 路径已补齐（见 WebGL2RHI.cpp），
// 于是把 Emscripten 也纳入本守卫：Web 现在走与 Metal 相同的**非 Vulkan 通用 RHI 路径**
// （下面的 #else 分支），真正把我的 WebGL2 后端编进来跑，而不是只留 clear() 空壳。
// Vulkan 专属代码仍被内部的 #if defined(Z_HAS_VULKAN) 二次排除，Emscripten 一律走
// 通用 CreateBuffer 回退——这与 Metal 完全一致。
#if defined(Z_HAS_VULKAN) || defined(__APPLE__) || defined(__EMSCRIPTEN__)
    #include "Runtime/Core/Base/Macro.h"
    #if defined(Z_HAS_VULKAN)
        #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
        #include "Runtime/Function/Render/Interface/Vulkan/VulkanUtil.h"
    #endif
    #include "Runtime/Function/Render/Passes/MainCameraPass.h"
    #include "Runtime/Function/Render/RenderCamera.h"
    #include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
    #include "Runtime/Function/Render/RenderHelper.h"
    #include "Runtime/Function/Render/RenderMesh.h"
    #include "Runtime/Function/Render/RenderSystem.h"
    #include "Runtime/Profiler/Profiler.h"

    #include <stdexcept>

namespace
{
    // 资产 buffer（顶点 / index / uniform）在两个世界里长得不一样：
    //   · Vulkan 走 VMA：allocator 是 VmaAllocator，返回的句柄是 VmaAllocation；
    //   · Metal / WebGL2 没有 VMA，RHI::CreateBufferVMA 直接返回 false（基类默认实现）。
    //
    // 所以统一走这两个 helper：先试 VMA，失败就退回 RHI 的通用 CreateBuffer。
    // 退回时把 RHIDeviceMemory* 塞回同一个 void* 槽位 —— 那一栏本来就是只写不读的
    // 账本（GpuMesh::*_allocation 在 Apple 上就是 void*），谁都不会去释放它，所以
    // 换个句柄类型放进去没有语义损失，也没有泄漏变化。
    //
    // minAlignment 只有 Vulkan 用（minUniformBufferOffsetAlignment）；Metal 的
    // CreateBuffer 没有这个参数，对齐由 Metal 自己按 usage 定。
    void CreateAssetBuffer(RHI* rhi,
                           const RHIBufferCreateInfo& buffer_info,
                           RHIBuffer*& buffer,
                           void** allocation)
    {
#if defined(Z_HAS_VULKAN)
        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (rhi->CreateBufferVMA(static_cast<VulkanRHI*>(rhi)->m_AssetsAllocator,
                                 &buffer_info,
                                 &alloc_info,
                                 buffer,
                                 allocation,
                                 nullptr))
        {
            return;
        }
#else
        (void)rhi;
#endif
        RHIDeviceMemory* memory = RHI_NULL_HANDLE;
        rhi->CreateBuffer(buffer_info.size,
                          buffer_info.usage,
                          RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          buffer,
                          memory);
        *allocation = memory;
    }

    void CreateAssetBufferWithAlignment(RHI* rhi,
                                        const RHIBufferCreateInfo& buffer_info,
                                        RHIDeviceSize min_alignment,
                                        RHIBuffer*& buffer,
                                        void** allocation)
    {
#if defined(Z_HAS_VULKAN)
        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (rhi->CreateBufferWithAlignmentVMA(static_cast<VulkanRHI*>(rhi)->m_AssetsAllocator,
                                              &buffer_info,
                                              &alloc_info,
                                              min_alignment,
                                              buffer,
                                              allocation,
                                              nullptr))
        {
            return;
        }
#else
        (void)rhi;
        (void)min_alignment;
#endif
        CreateAssetBuffer(rhi, buffer_info, buffer, allocation);
    }
}  // namespace

void RenderResource::clear()
{
    ClearMeshGeometryCache();

    // 运行时材质实例跟着共享材质一起失效。RHI 没有单套描述符集的释放接口，这里的
    // UBO / 描述符集随进程回收；map 的规模上界是"同时带 override 的 part 数"
    // （instance id 由 RenderScene 的 allocator 回收复用），不会随关卡加载次数涨。
    m_MaterialInstances.clear();

    if (m_TextureStreamingManager)
    {
        m_TextureStreamingManager->Shutdown();
        m_TextureStreamingManager.reset();
    }
}

void RenderResource::UploadGlobalRenderResource(RHI* rhi, LevelResourceDesc level_resource_desc)
{
    Z_PROFILE_SCOPE("RenderResource::uploadGlobalRenderResource");
    // Initialize texture streaming manager
    if (!m_TextureStreamingManager)
    {
        m_TextureStreamingManager = std::make_shared<TextureStreamingManager>();
        // Note: We pass a raw pointer since RenderResourceBase doesn't inherit from enable_shared_from_this
        // The manager will only use it for loading textures, not storing it
        m_TextureStreamingManager->Initialize(rhi, this);
    }

    // create and map global storage buffer (skip if RenderSystem already created it for DX12 pipeline init)
    if (m_GlobalRenderResource.m_StorageBuffer.m_GlobalUploadRingbuffer == nullptr)
    {
        CreateAndMapStorageBuffer(rhi);
    }

    m_MegaLights.Initialize(rhi);

    // IBL（6 张 irradiance + 6 张 specular HDR cubemap + BRDF LUT），细节见 UploadIblResources
    UploadIblResources(rhi, level_resource_desc.m_IblResourceDesc);

    // color grading（细节见 UploadColorGradingLut）
    UploadColorGradingLut(rhi, level_resource_desc.m_ColorGradingResourceDesc.m_ColorGradingMap);
}

void RenderResource::UploadIblResources(RHI* rhi, const LevelIBLResourceDesc& ibl_desc)
{
    // 这一段以前只活在 `UploadGlobalRenderResource` 里，而 Apple 的
    // `RenderSystem::Initialize` 是精简版、**从不调用**它 —— 于是 Apple 上 IBL 从来
    // 没上过 GPU，RP1 的 `SetupDescriptorSets` 只能绑 `EnsureFallbackIblTextures`
    // 造的那张 4x4 中性灰 cubemap + 1x1 BRDF。画面上是"环境光永远是一个常数灰"，
    // 而且日志里还挂着一条 `[E] IBL textures unavailable (device removed?)`，看
    // 起来像设备掉了，其实是"没上传"。
    //
    // 抽成独立入口后 Apple 可以只补 IBL，不用顺带把 texture streaming manager /
    // MegaLights / storage buffer 那套也拖进来。
    //
    // sky box irradiance
    const SkyBoxIrradianceMap& skybox_irradiance_map = ibl_desc.m_SkyboxIrradianceMap;
    LOG_INFO(ZRender, "UploadIblResources: loading irradiance maps from {}", skybox_irradiance_map.m_PositiveXMap);
    std::shared_ptr<TextureData> irradiace_pos_x_map = LoadTextureHDR(skybox_irradiance_map.m_PositiveXMap);
    std::shared_ptr<TextureData> irradiace_neg_x_map = LoadTextureHDR(skybox_irradiance_map.m_NegativeXMap);
    std::shared_ptr<TextureData> irradiace_pos_y_map = LoadTextureHDR(skybox_irradiance_map.m_PositiveYMap);
    std::shared_ptr<TextureData> irradiace_neg_y_map = LoadTextureHDR(skybox_irradiance_map.m_NegativeYMap);
    std::shared_ptr<TextureData> irradiace_pos_z_map = LoadTextureHDR(skybox_irradiance_map.m_PositiveZMap);
    std::shared_ptr<TextureData> irradiace_neg_z_map = LoadTextureHDR(skybox_irradiance_map.m_NegativeZMap);
    LOG_INFO(ZRender, "UploadIblResources: irradiance loaded: +X={}, -X={}, +Y={}, -Y={}, +Z={}, -Z={}",
             irradiace_pos_x_map != nullptr, irradiace_neg_x_map != nullptr,
             irradiace_pos_y_map != nullptr, irradiace_neg_y_map != nullptr,
             irradiace_pos_z_map != nullptr, irradiace_neg_z_map != nullptr);

    // sky box specular
    const SkyBoxSpecularMap& skybox_specular_map = ibl_desc.m_SkyboxSpecularMap;
    LOG_INFO(ZRender, "UploadIblResources: loading specular maps from {}", skybox_specular_map.m_PositiveXMap);
    std::shared_ptr<TextureData> specular_pos_x_map = LoadTextureHDR(skybox_specular_map.m_PositiveXMap);
    std::shared_ptr<TextureData> specular_neg_x_map = LoadTextureHDR(skybox_specular_map.m_NegativeXMap);
    std::shared_ptr<TextureData> specular_pos_y_map = LoadTextureHDR(skybox_specular_map.m_PositiveYMap);
    std::shared_ptr<TextureData> specular_neg_y_map = LoadTextureHDR(skybox_specular_map.m_NegativeYMap);
    std::shared_ptr<TextureData> specular_pos_z_map = LoadTextureHDR(skybox_specular_map.m_PositiveZMap);
    std::shared_ptr<TextureData> specular_neg_z_map = LoadTextureHDR(skybox_specular_map.m_NegativeZMap);
    LOG_INFO(ZRender, "UploadIblResources: specular loaded: +X={}, -X={}, +Y={}, -Y={}, +Z={}, -Z={}",
             specular_pos_x_map != nullptr, specular_neg_x_map != nullptr,
             specular_pos_y_map != nullptr, specular_neg_y_map != nullptr,
             specular_pos_z_map != nullptr, specular_neg_z_map != nullptr);

    // brdf
    LOG_INFO(ZRender, "UploadIblResources: loading BRDF LUT from {}", ibl_desc.m_BrdfMap);
    std::shared_ptr<TextureData> brdf_map = LoadTextureHDR(ibl_desc.m_BrdfMap);
    LOG_INFO(ZRender, "UploadIblResources: BRDF LUT loaded: {}", brdf_map != nullptr);

    const auto ibl_face_loaded = [](const std::shared_ptr<TextureData>& map) {
        return map != nullptr && map->m_Pixels != nullptr && map->m_Width > 0 && map->m_Height > 0;
    };

    if (ibl_face_loaded(irradiace_pos_x_map) && ibl_face_loaded(specular_pos_x_map) && ibl_face_loaded(brdf_map))
    {
        try
        {
            CreateIBLSamplers(rhi);

            std::array<std::shared_ptr<TextureData>, 6> irradiance_maps = {irradiace_pos_x_map,
                                                                           irradiace_neg_x_map,
                                                                           irradiace_pos_z_map,
                                                                           irradiace_neg_z_map,
                                                                           irradiace_pos_y_map,
                                                                           irradiace_neg_y_map};
            std::array<std::shared_ptr<TextureData>, 6> specular_maps = {specular_pos_x_map,
                                                                         specular_neg_x_map,
                                                                         specular_pos_z_map,
                                                                         specular_neg_z_map,
                                                                         specular_pos_y_map,
                                                                         specular_neg_y_map};
            CreateIBLTextures(rhi, irradiance_maps, specular_maps);

            rhi->CreateGlobalImage(m_GlobalRenderResource.m_IblResource.m_BrdflutTextureImage,
                                   m_GlobalRenderResource.m_IblResource.m_BrdflutTextureImageView,
                                   m_GlobalRenderResource.m_IblResource.m_BrdflutTextureImageAllocation,
                                   brdf_map->m_Width,
                                   brdf_map->m_Height,
                                   brdf_map->m_Pixels,
                                   brdf_map->m_Format);
            LOG_INFO(ZRender,
                     "RenderResource: IBL HDR upload complete (irradiance {}x{}, specular {}x{}, BRDF {}x{}, "
                     "api={})",
                     irradiance_maps[0]->m_Width,
                     irradiance_maps[0]->m_Height,
                     specular_maps[0]->m_Width,
                     specular_maps[0]->m_Height,
                     brdf_map->m_Width,
                     brdf_map->m_Height,
                     static_cast<int>(rhi->getGraphicsAPI()));
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR(ZRender, "RenderResource: IBL HDR upload failed: {}", ex.what());
        }
    }
    else
    {
        LOG_WARNING(ZRender, "RenderResource: skipping IBL upload (HDR face or BRDF load failed)");
    }
}

void RenderResource::UploadColorGradingLut(RHI* rhi, const eastl::string& color_grading_path)
{
    // 这一段以前**没有任何守卫**：配置里没写 color_grading_map 时 LoadTexture
    // 返回 nullptr，紧接着就 `color_grading_map->m_Width` —— 直接崩；加载失败
    // （文件不存在 / 解码失败）时同样静默。RenderResourceDx12.cpp 那份 twin 有
    // empty 检查和失败警告，这份没有 ⇒ Apple 上一旦出问题只能靠"画面颜色不对"
    // 反推，而 LUT 恰恰是最容易被当成"美术风格"的东西。
    //
    // 三个出口都留了日志：没配 / 加载失败是 WARNING（画面照旧，只是没有分级），
    // 成功是 INFO 并把尺寸打出来 —— 真机上靠这一行确认 LUT 真的到了 GPU，
    // 以及它是 256x16（条带）还是别的尺寸（条带采样的切片数是从高度推的）。
    if (color_grading_path.empty())
    {
        LOG_WARNING(ZRender,
                    "RenderResource: no color grading map configured -- "
                    "the RP2 LUT sample falls back to a neutral texel");
        return;
    }

    // `allow_cooked=false`：LUT 是逐 texel 精确的查表，绝不能走 4x4 块压缩的
    // `.zasset`（BC7 / ASTC）。工程里 `asset/_Generated/Textures/texture/lut/`
    // 下确实有烤好的产物，不跳过的话拿到的就是一张被压缩过的表 —— 分级结果出现
    // 色带，从日志到画面都不会报错。
    std::shared_ptr<TextureData> color_grading_map = LoadTexture(color_grading_path, false, false);
    if (color_grading_map == nullptr || color_grading_map->m_Pixels == nullptr ||
        color_grading_map->m_Width == 0 || color_grading_map->m_Height == 0)
    {
        LOG_WARNING(ZRender,
                    "RenderResource: failed to load color grading LUT '{}' -- "
                    "the RP2 LUT sample falls back to a neutral texel",
                    color_grading_path.c_str());
        return;
    }

    // create color grading texture
    rhi->CreateGlobalImage(m_GlobalRenderResource.m_ColorGradingResource.m_ColorGradingLutTextureImage,
                           m_GlobalRenderResource.m_ColorGradingResource.m_ColorGradingLutTextureImageView,
                           m_GlobalRenderResource.m_ColorGradingResource.m_ColorGradingLutTextureImageAllocation,
                           color_grading_map->m_Width,
                           color_grading_map->m_Height,
                           color_grading_map->m_Pixels,
                           color_grading_map->m_Format);

    LOG_INFO(ZRender,
             "RenderResource: color grading LUT '{}' uploaded ({}x{}, format={})",
             color_grading_path.c_str(),
             color_grading_map->m_Width,
             color_grading_map->m_Height,
             static_cast<int>(color_grading_map->m_Format));
}

void RenderResource::UploadGameObjectRenderResource(RHI* rhi,
                                                    RenderEntity render_entity,
                                                    RenderMeshData mesh_data,
                                                    RenderMaterialData material_data)
{
    GetOrCreateMesh(rhi, render_entity, mesh_data);
    GetOrCreateMaterial(rhi, render_entity, material_data);
}

void RenderResource::UploadGameObjectRenderResource(RHI* rhi,
                                                    RenderEntity render_entity,
                                                    RenderMeshData mesh_data)
{
    GetOrCreateMesh(rhi, render_entity, mesh_data);
}

void RenderResource::UploadGameObjectRenderResource(RHI* rhi,
                                                    RenderEntity render_entity,
                                                    RenderMaterialData material_data)
{
    GetOrCreateMaterial(rhi, render_entity, material_data);
}

void RenderResource::CreateIBLSamplers(RHI* rhi)
{
    if (rhi == nullptr)
    {
        throw std::runtime_error("RenderResource::CreateIBLSamplers requires a valid RHI");
    }

    RHIPhysicalDeviceProperties physical_device_properties {};
    rhi->GetPhysicalDeviceProperties(&physical_device_properties);
    if (physical_device_properties.limits.maxSamplerAnisotropy <= 0.0f)
    {
        physical_device_properties.limits.maxSamplerAnisotropy = 1.0f;
    }

    RHISamplerCreateInfo samplerInfo {};
    samplerInfo.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = RHI_FILTER_LINEAR;
    samplerInfo.minFilter = RHI_FILTER_LINEAR;
    samplerInfo.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = RHI_TRUE;                                             // close:false
    samplerInfo.maxAnisotropy = physical_device_properties.limits.maxSamplerAnisotropy;  // close :1.0f
    samplerInfo.borderColor = RHI_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = RHI_FALSE;
    samplerInfo.compareEnable = RHI_FALSE;
    samplerInfo.compareOp = RHI_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = 0.0f;

    if (m_GlobalRenderResource.m_IblResource.m_BrdflutTextureSampler != RHI_NULL_HANDLE)
    {
        rhi->DestroySampler(m_GlobalRenderResource.m_IblResource.m_BrdflutTextureSampler);
    }

    if (rhi->CreateSampler(&samplerInfo, m_GlobalRenderResource.m_IblResource.m_BrdflutTextureSampler) !=
        RHI_SUCCESS)
    {
        throw std::runtime_error("vk create sampler");
    }

    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 8.0f;  // TODO: irradiance_texture_miplevels
    samplerInfo.mipLodBias = 0.0f;

    if (m_GlobalRenderResource.m_IblResource.m_IrradianceTextureSampler != RHI_NULL_HANDLE)
    {
        rhi->DestroySampler(m_GlobalRenderResource.m_IblResource.m_IrradianceTextureSampler);
    }

    if (rhi->CreateSampler(&samplerInfo, m_GlobalRenderResource.m_IblResource.m_IrradianceTextureSampler) !=
        RHI_SUCCESS)
    {
        throw std::runtime_error("vk create sampler");
    }

    if (m_GlobalRenderResource.m_IblResource.m_SpecularTextureSampler != RHI_NULL_HANDLE)
    {
        rhi->DestroySampler(m_GlobalRenderResource.m_IblResource.m_SpecularTextureSampler);
    }

    if (rhi->CreateSampler(&samplerInfo, m_GlobalRenderResource.m_IblResource.m_SpecularTextureSampler) !=
        RHI_SUCCESS)
    {
        throw std::runtime_error("vk create sampler");
    }
}

void RenderResource::CreateIBLTextures(RHI* rhi,
                                       std::array<std::shared_ptr<TextureData>, 6> irradiance_maps,
                                       std::array<std::shared_ptr<TextureData>, 6> specular_maps)
{
    // assume all textures have same width, height and format
    uint32_t irradiance_cubemap_miplevels =
        static_cast<uint32_t>(std::floor(log2(std::max(irradiance_maps[0]->m_Width, irradiance_maps[0]->m_Height)))) +
        1;
    uint32_t specular_cubemap_miplevels =
        static_cast<uint32_t>(std::floor(log2(std::max(specular_maps[0]->m_Width, specular_maps[0]->m_Height)))) + 1;

    // DX12CubemapMipGenerator runs a fullscreen pass per mip x 6 faces. Uploading
    // two 256^2 HDR cubemaps back-to-back during editor startup can remove the
    // device before the first frame presents. Mip0 is sufficient for skybox +
    // RP1 fallback IBL until GPU mipgen is hardened.
    if (rhi != nullptr && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        irradiance_cubemap_miplevels = 1;
        specular_cubemap_miplevels = 1;
    }

    rhi->CreateCubeMap(m_GlobalRenderResource.m_IblResource.m_IrradianceTextureImage,
                       m_GlobalRenderResource.m_IblResource.m_IrradianceTextureImageView,
                       m_GlobalRenderResource.m_IblResource.m_IrradianceTextureImageAllocation,
                       irradiance_maps[0]->m_Width,
                       irradiance_maps[0]->m_Height,
                       {irradiance_maps[0]->m_Pixels,
                        irradiance_maps[1]->m_Pixels,
                        irradiance_maps[2]->m_Pixels,
                        irradiance_maps[3]->m_Pixels,
                        irradiance_maps[4]->m_Pixels,
                        irradiance_maps[5]->m_Pixels},
                       irradiance_maps[0]->m_Format,
                       irradiance_cubemap_miplevels);

    rhi->CreateCubeMap(m_GlobalRenderResource.m_IblResource.m_SpecularTextureImage,
                       m_GlobalRenderResource.m_IblResource.m_SpecularTextureImageView,
                       m_GlobalRenderResource.m_IblResource.m_SpecularTextureImageAllocation,
                       specular_maps[0]->m_Width,
                       specular_maps[0]->m_Height,
                       {specular_maps[0]->m_Pixels,
                        specular_maps[1]->m_Pixels,
                        specular_maps[2]->m_Pixels,
                        specular_maps[3]->m_Pixels,
                        specular_maps[4]->m_Pixels,
                        specular_maps[5]->m_Pixels},
                       specular_maps[0]->m_Format,
                       specular_cubemap_miplevels);
}

GpuMesh&
RenderResource::GetOrCreateMesh(RHI* rhi, RenderEntity entity, RenderMeshData mesh_data)
{
    size_t assetid = entity.m_MeshAssetId;

    // Keep a CPU copy of the geometry while we still have it: after this function
    // returns, mesh_data is gone and nothing can read vertex positions back from the
    // GPU buffers. Lumen's distance field is the consumer today.
    CacheMeshGeometry(assetid, mesh_data, entity.m_BoundingBox);

    auto it = m_Meshes.find(assetid);
    if (it != m_Meshes.end() && meshDrawDataIsValid(&it->second))
    {
#if defined(_WIN32)
        if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 && it->second.mesh_vertex_interleaved_buffer == nullptr)
        {
            LOG_WARNING(ZRender,
                        "RenderResource: re-uploading mesh asset id {} (DX12 interleaved vertex buffer missing)",
                        assetid);
            m_Meshes.erase(it);
        }
        else
#endif
        {
            return it->second;
        }
    }
    else if (it != m_Meshes.end())
    {
        LOG_WARNING(ZRender, "RenderResource: re-uploading mesh asset id {} (GPU buffers were invalid)", assetid);
        m_Meshes.erase(it);
    }

    GpuMesh temp;
    auto res = m_Meshes.insert(std::make_pair(assetid, std::move(temp)));
    assert(res.second);

    uint32_t index_buffer_size = static_cast<uint32_t>(mesh_data.m_StaticMeshData.m_IndexBuffer->m_Size);
    void* index_buffer_data = mesh_data.m_StaticMeshData.m_IndexBuffer->m_Data;

    uint32_t vertex_buffer_size = static_cast<uint32_t>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Size);
    MeshVertexDataDefinition* vertex_buffer_data =
        reinterpret_cast<MeshVertexDataDefinition*>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data);

    GpuMesh& now_mesh = res.first->second;

#if defined(_WIN32)
    if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        UploadGpuMeshDx12(rhi, now_mesh, mesh_data);
        return now_mesh;
    }
#endif

    // Metal 上不能谎报 Vulkan：backend 是 GpuMesh 的运行时标签，后面的代码会拿它
    // 判断这块 buffer 是谁建的、该按哪套规则去绑。
    now_mesh.backend = rhi->getGraphicsAPI() == GraphicsAPI::Metal ? RenderResourceBackend::Metal
                                                                   : RenderResourceBackend::Vulkan;

    if (mesh_data.m_SkeletonBindingBuffer)
    {
        uint32_t joint_binding_buffer_size = (uint32_t)mesh_data.m_SkeletonBindingBuffer->m_Size;
        MeshVertexBindingDataDefinition* joint_binding_buffer_data =
            reinterpret_cast<MeshVertexBindingDataDefinition*>(mesh_data.m_SkeletonBindingBuffer->m_Data);
        UpdateMeshData(rhi,
                       true,
                       index_buffer_size,
                       index_buffer_data,
                       vertex_buffer_size,
                       vertex_buffer_data,
                       joint_binding_buffer_size,
                       joint_binding_buffer_data,
                       now_mesh);
    }
    else
    {
        UpdateMeshData(rhi,
                       false,
                       index_buffer_size,
                       index_buffer_data,
                       vertex_buffer_size,
                       vertex_buffer_data,
                       0,
                       NULL,
                       now_mesh);
    }

    return now_mesh;
}

GpuPBRMaterial& RenderResource::GetOrCreateMaterial(RHI* rhi,
                                                             RenderEntity entity,
                                                             RenderMaterialData material_data)
{
    size_t assetid = entity.m_MaterialAssetId;

    auto it = m_Materials.find(assetid);
    if (it != m_Materials.end())
    {
        if (entity.m_DoubleSided)
        {
            it->second.cull = "None";
        }
        return it->second;
    }
    else
    {
        GpuPBRMaterial temp;
        auto res = m_Materials.insert(std::make_pair(assetid, std::move(temp)));
        assert(res.second);

        float empty_image[] = {0.5f, 0.5f, 0.5f, 0.5f};

        void* base_color_image_pixels = empty_image;
        uint32_t base_color_image_width = 1;
        uint32_t base_color_image_height = 1;
        RHIFormat base_color_image_format = RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB;
        if (material_data.m_BaseColorTexture)
        {
            base_color_image_pixels = material_data.m_BaseColorTexture->m_Pixels;
            base_color_image_width = static_cast<uint32_t>(material_data.m_BaseColorTexture->m_Width);
            base_color_image_height = static_cast<uint32_t>(material_data.m_BaseColorTexture->m_Height);
            base_color_image_format = material_data.m_BaseColorTexture->m_Format;
        }

        void* metallic_roughness_image_pixels = empty_image;
        uint32_t metallic_roughness_width = 1;
        uint32_t metallic_roughness_height = 1;
        RHIFormat metallic_roughness_format = RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;
        if (material_data.m_MetallicRoughnessTexture)
        {
            metallic_roughness_image_pixels = material_data.m_MetallicRoughnessTexture->m_Pixels;
            metallic_roughness_width = static_cast<uint32_t>(material_data.m_MetallicRoughnessTexture->m_Width);
            metallic_roughness_height = static_cast<uint32_t>(material_data.m_MetallicRoughnessTexture->m_Height);
            metallic_roughness_format = material_data.m_MetallicRoughnessTexture->m_Format;
        }

        void* normal_roughness_image_pixels = empty_image;
        uint32_t normal_roughness_width = 1;
        uint32_t normal_roughness_height = 1;
        RHIFormat normal_roughness_format = RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;
        if (material_data.m_NormalTexture)
        {
            normal_roughness_image_pixels = material_data.m_NormalTexture->m_Pixels;
            normal_roughness_width = static_cast<uint32_t>(material_data.m_NormalTexture->m_Width);
            normal_roughness_height = static_cast<uint32_t>(material_data.m_NormalTexture->m_Height);
            normal_roughness_format = material_data.m_NormalTexture->m_Format;
        }

        void* occlusion_image_pixels = empty_image;
        uint32_t occlusion_image_width = 1;
        uint32_t occlusion_image_height = 1;
        RHIFormat occlusion_image_format = RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;
        if (material_data.m_OcclusionTexture)
        {
            occlusion_image_pixels = material_data.m_OcclusionTexture->m_Pixels;
            occlusion_image_width = static_cast<uint32_t>(material_data.m_OcclusionTexture->m_Width);
            occlusion_image_height = static_cast<uint32_t>(material_data.m_OcclusionTexture->m_Height);
            occlusion_image_format = material_data.m_OcclusionTexture->m_Format;
        }

        void* emissive_image_pixels = empty_image;
        uint32_t emissive_image_width = 1;
        uint32_t emissive_image_height = 1;
        RHIFormat emissive_image_format = RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;
        if (material_data.m_EmissiveTexture)
        {
            emissive_image_pixels = material_data.m_EmissiveTexture->m_Pixels;
            emissive_image_width = static_cast<uint32_t>(material_data.m_EmissiveTexture->m_Width);
            emissive_image_height = static_cast<uint32_t>(material_data.m_EmissiveTexture->m_Height);
            emissive_image_format = material_data.m_EmissiveTexture->m_Format;
        }

        GpuPBRMaterial& now_material = res.first->second;
        now_material.shader_name = material_data.m_Shader.c_str();
        now_material.shader_asset_file = material_data.m_ShaderAssetFile.c_str();
        now_material.vertex_shader_file = material_data.m_VertexShaderFile.c_str();
        now_material.fragment_shader_file = material_data.m_FragmentShaderFile.c_str();
        now_material.render_pipeline = material_data.m_RenderPipeline.c_str();
        now_material.light_mode = material_data.m_LightMode.c_str();
        now_material.source_language = material_data.m_SourceLanguage.c_str();
        now_material.vertex_entry = material_data.m_VertexEntry.c_str();
        now_material.fragment_entry = material_data.m_FragmentEntry.c_str();
        now_material.include_directory = material_data.m_IncludeDirectory.c_str();
        now_material.cull = material_data.m_Cull.c_str();
        now_material.ztest = material_data.m_Ztest.c_str();
        now_material.blend = material_data.m_Blend.c_str();
        now_material.zwrite = material_data.m_Zwrite;
        if (entity.m_DoubleSided)
        {
            now_material.cull = "None";
        }
        now_material.shader_macros.clear();
        for (const eastl::string& keyword : material_data.m_EnabledShaderKeywords)
        {
            if (!keyword.empty())
            {
                now_material.shader_macros[keyword.c_str()] = "1";
            }
        }
        now_material.enable_dx12 = material_data.m_EnableDx12;
        now_material.enable_vulkan = material_data.m_EnableVulkan;
        now_material.enable_metal = material_data.m_EnableMetal;
        now_material.shader_passes.clear();
        now_material.shader_passes.reserve(material_data.m_ShaderPasses.size());
        for (const RenderShaderPassData& shader_pass : material_data.m_ShaderPasses)
        {
            GpuShaderPassData runtime_shader_pass;
            runtime_shader_pass.name = shader_pass.m_Name.c_str();
            runtime_shader_pass.light_mode = shader_pass.m_LightMode.c_str();
            runtime_shader_pass.vertex_shader_file = shader_pass.m_VertexShaderFile.c_str();
            runtime_shader_pass.fragment_shader_file = shader_pass.m_FragmentShaderFile.c_str();
            runtime_shader_pass.render_pipeline = shader_pass.m_RenderPipeline.c_str();
            runtime_shader_pass.vertex_entry = shader_pass.m_VertexEntry.c_str();
            runtime_shader_pass.fragment_entry = shader_pass.m_FragmentEntry.c_str();
            runtime_shader_pass.cull = shader_pass.m_Cull.c_str();
            runtime_shader_pass.ztest = shader_pass.m_Ztest.c_str();
            runtime_shader_pass.blend = shader_pass.m_Blend.c_str();
            runtime_shader_pass.zwrite = shader_pass.m_Zwrite;
            now_material.shader_passes.emplace_back(std::move(runtime_shader_pass));
        }

        // similiarly to the vertex/index buffer, we should allocate the uniform

        // buffer in DEVICE_LOCAL memory and use the temp stage buffer to copy the
        // data
        {
            // temporary staging buffer

            RHIDeviceSize buffer_size = sizeof(MeshMaterialUniform);

            RHIBuffer* inefficient_staging_buffer = RHI_NULL_HANDLE;
            RHIDeviceMemory* inefficient_staging_buffer_memory = RHI_NULL_HANDLE;
            rhi->CreateBuffer(buffer_size,
                              RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              inefficient_staging_buffer,
                              inefficient_staging_buffer_memory);
            // RHI_BUFFER_USAGE_TRANSFER_SRC_BIT: buffer can be used as source in a
            // memory transfer operation

            void* staging_buffer_data = nullptr;
            rhi->MapMemory(inefficient_staging_buffer_memory, 0, buffer_size, 0, &staging_buffer_data);

            MeshMaterialUniform& material_uniform_buffer_info =
                (*static_cast<MeshMaterialUniform*>(staging_buffer_data));
            material_uniform_buffer_info.is_blend = entity.m_Blend;
            material_uniform_buffer_info.is_double_sided = entity.m_DoubleSided;
            material_uniform_buffer_info.baseColorFactor = entity.m_BaseColorFactor;
            material_uniform_buffer_info.metallicFactor = entity.m_MetallicFactor;
            material_uniform_buffer_info.roughnessFactor = entity.m_RoughnessFactor;
            material_uniform_buffer_info.normalScale = entity.m_NormalScale;
            material_uniform_buffer_info.occlusionStrength = entity.m_OcclusionStrength;
            material_uniform_buffer_info.emissiveFactor = entity.m_EmissiveFactor;

            rhi->UnmapMemory(inefficient_staging_buffer_memory);

            // use the vmaAllocator to allocate asset uniform buffer
            RHIBufferCreateInfo bufferInfo = {RHI_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufferInfo.size = buffer_size;
            bufferInfo.usage = RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;

            CreateAssetBufferWithAlignment(rhi,
                                           bufferInfo,
                                           m_GlobalRenderResource.m_StorageBuffer.m_MinUniformBufferOffsetAlignment,
                                           now_material.material_uniform_buffer,
                                           (void**)&now_material.material_uniform_buffer_allocation);

            // use the data from staging buffer
            rhi->CopyBuffer(inefficient_staging_buffer, now_material.material_uniform_buffer, 0, 0, buffer_size);

            // release staging buffer
            rhi->DestroyBuffer(inefficient_staging_buffer);
            rhi->FreeMemory(inefficient_staging_buffer_memory);
        }

        // Mip counts come from the cooked TextureData (cooked Texture2D carries a
        // full compressed mip chain; uncompressed stb-decoded textures stay at 1
        // and rely on the RHI's GPU-mipgen contract).
        auto mip_count_of = [](const std::shared_ptr<TextureData>& tex) -> uint32_t {
            return (tex && tex->m_MipLevels > 0) ? tex->m_MipLevels : 1u;
        };

        TextureDataToUpdate update_texture_data;
        update_texture_data.base_color_image_pixels = base_color_image_pixels;
        update_texture_data.base_color_image_width = base_color_image_width;
        update_texture_data.base_color_image_height = base_color_image_height;
        update_texture_data.base_color_image_format = base_color_image_format;
        update_texture_data.base_color_image_miplevels = mip_count_of(material_data.m_BaseColorTexture);
        update_texture_data.metallic_roughness_image_pixels = metallic_roughness_image_pixels;
        update_texture_data.metallic_roughness_image_width = metallic_roughness_width;
        update_texture_data.metallic_roughness_image_height = metallic_roughness_height;
        update_texture_data.metallic_roughness_image_format = metallic_roughness_format;
        update_texture_data.metallic_roughness_image_miplevels = mip_count_of(material_data.m_MetallicRoughnessTexture);
        update_texture_data.normal_roughness_image_pixels = normal_roughness_image_pixels;
        update_texture_data.normal_roughness_image_width = normal_roughness_width;
        update_texture_data.normal_roughness_image_height = normal_roughness_height;
        update_texture_data.normal_roughness_image_format = normal_roughness_format;
        update_texture_data.normal_roughness_image_miplevels = mip_count_of(material_data.m_NormalTexture);
        update_texture_data.occlusion_image_pixels = occlusion_image_pixels;
        update_texture_data.occlusion_image_width = occlusion_image_width;
        update_texture_data.occlusion_image_height = occlusion_image_height;
        update_texture_data.occlusion_image_format = occlusion_image_format;
        update_texture_data.occlusion_image_miplevels = mip_count_of(material_data.m_OcclusionTexture);
        update_texture_data.emissive_image_pixels = emissive_image_pixels;
        update_texture_data.emissive_image_width = emissive_image_width;
        update_texture_data.emissive_image_height = emissive_image_height;
        update_texture_data.emissive_image_format = emissive_image_format;
        update_texture_data.emissive_image_miplevels = mip_count_of(material_data.m_EmissiveTexture);
        update_texture_data.now_material = &now_material;

        UpdateTextureImageData(rhi, update_texture_data);

        RHIDescriptorSetAllocateInfo material_descriptor_set_alloc_info;
        material_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        material_descriptor_set_alloc_info.pNext = NULL;
        material_descriptor_set_alloc_info.descriptorPool = rhi->GetDescriptorPoor();
        material_descriptor_set_alloc_info.descriptorSetCount = 1;
        material_descriptor_set_alloc_info.pSetLayouts = m_MaterialDescriptorSetLayout;

        if (RHI_SUCCESS !=
            rhi->AllocateDescriptorSets(&material_descriptor_set_alloc_info, now_material.material_descriptor_set))
        {
            throw std::runtime_error("allocate material descriptor set");
        }

        RHIDescriptorBufferInfo material_uniform_buffer_info = {};
        material_uniform_buffer_info.offset = 0;
        material_uniform_buffer_info.range = sizeof(MeshMaterialUniform);
        material_uniform_buffer_info.buffer = now_material.material_uniform_buffer;

        RHIDescriptorImageInfo base_color_image_info = {};
        base_color_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        base_color_image_info.imageView = now_material.base_color_image_view;
        base_color_image_info.sampler = rhi->GetOrCreateMipmapSampler(base_color_image_width, base_color_image_height);
        now_material.base_color_sampler = base_color_image_info.sampler;

        RHIDescriptorImageInfo metallic_roughness_image_info = {};
        metallic_roughness_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        metallic_roughness_image_info.imageView = now_material.metallic_roughness_image_view;
        metallic_roughness_image_info.sampler =
            rhi->GetOrCreateMipmapSampler(metallic_roughness_width, metallic_roughness_height);
        now_material.metallic_roughness_sampler = metallic_roughness_image_info.sampler;

        RHIDescriptorImageInfo normal_roughness_image_info = {};
        normal_roughness_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        normal_roughness_image_info.imageView = now_material.normal_image_view;
        normal_roughness_image_info.sampler =
            rhi->GetOrCreateMipmapSampler(normal_roughness_width, normal_roughness_height);
        now_material.normal_sampler = normal_roughness_image_info.sampler;

        RHIDescriptorImageInfo occlusion_image_info = {};
        occlusion_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        occlusion_image_info.imageView = now_material.occlusion_image_view;
        occlusion_image_info.sampler = rhi->GetOrCreateMipmapSampler(occlusion_image_width, occlusion_image_height);
        now_material.occlusion_sampler = occlusion_image_info.sampler;

        RHIDescriptorImageInfo emissive_image_info = {};
        emissive_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        emissive_image_info.imageView = now_material.emissive_image_view;
        emissive_image_info.sampler = rhi->GetOrCreateMipmapSampler(emissive_image_width, emissive_image_height);
        now_material.emissive_sampler = emissive_image_info.sampler;

        RHIWriteDescriptorSet mesh_descriptor_writes_info[6];

        mesh_descriptor_writes_info[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_descriptor_writes_info[0].pNext = NULL;
        mesh_descriptor_writes_info[0].dstSet = now_material.material_descriptor_set;
        mesh_descriptor_writes_info[0].dstBinding = 0;
        mesh_descriptor_writes_info[0].dstArrayElement = 0;
        mesh_descriptor_writes_info[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mesh_descriptor_writes_info[0].descriptorCount = 1;
        mesh_descriptor_writes_info[0].pBufferInfo = &material_uniform_buffer_info;

        mesh_descriptor_writes_info[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_descriptor_writes_info[1].pNext = NULL;
        mesh_descriptor_writes_info[1].dstSet = now_material.material_descriptor_set;
        mesh_descriptor_writes_info[1].dstBinding = 1;
        mesh_descriptor_writes_info[1].dstArrayElement = 0;
        mesh_descriptor_writes_info[1].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_descriptor_writes_info[1].descriptorCount = 1;
        mesh_descriptor_writes_info[1].pImageInfo = &base_color_image_info;

        mesh_descriptor_writes_info[2] = mesh_descriptor_writes_info[1];
        mesh_descriptor_writes_info[2].dstBinding = 2;
        mesh_descriptor_writes_info[2].pImageInfo = &metallic_roughness_image_info;

        mesh_descriptor_writes_info[3] = mesh_descriptor_writes_info[1];
        mesh_descriptor_writes_info[3].dstBinding = 3;
        mesh_descriptor_writes_info[3].pImageInfo = &normal_roughness_image_info;

        mesh_descriptor_writes_info[4] = mesh_descriptor_writes_info[1];
        mesh_descriptor_writes_info[4].dstBinding = 4;
        mesh_descriptor_writes_info[4].pImageInfo = &occlusion_image_info;

        mesh_descriptor_writes_info[5] = mesh_descriptor_writes_info[1];
        mesh_descriptor_writes_info[5].dstBinding = 5;
        mesh_descriptor_writes_info[5].pImageInfo = &emissive_image_info;

        rhi->UpdateDescriptorSets(6, mesh_descriptor_writes_info, 0, nullptr);

        return now_material;
    }
}

void RenderResource::MaterializeMaterialInstance(RHI* rhi, RenderEntity entity, GpuPBRMaterial& instance)
{
    if (rhi == nullptr || m_MaterialDescriptorSetLayout == nullptr)
    {
        return;
    }

    // ---- 1. uniform buffer：没有就建，有就复用（重建 = 漏显存） ----
    const RHIDeviceSize buffer_size = sizeof(MeshMaterialUniform);

    if (instance.material_uniform_buffer == RHI_NULL_HANDLE)
    {
        RHIBufferCreateInfo buffer_info = {RHI_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = buffer_size;
        buffer_info.usage = RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;

        CreateAssetBufferWithAlignment(rhi,
                                       buffer_info,
                                       m_GlobalRenderResource.m_StorageBuffer.m_MinUniformBufferOffsetAlignment,
                                       instance.material_uniform_buffer,
                                       (void**)&instance.material_uniform_buffer_allocation);
    }

    if (instance.material_uniform_buffer == RHI_NULL_HANDLE)
    {
        return;
    }

    {
        RHIBuffer* staging_buffer = RHI_NULL_HANDLE;
        RHIDeviceMemory* staging_buffer_memory = RHI_NULL_HANDLE;
        rhi->CreateBuffer(buffer_size,
                          RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          staging_buffer,
                          staging_buffer_memory);

        void* staging_data = nullptr;
        rhi->MapMemory(staging_buffer_memory, 0, buffer_size, 0, &staging_data);
        if (staging_data != nullptr)
        {
            MeshMaterialUniform& uniform = *static_cast<MeshMaterialUniform*>(staging_data);
            uniform.is_blend = entity.m_Blend;
            uniform.is_double_sided = entity.m_DoubleSided;
            uniform.baseColorFactor = entity.m_BaseColorFactor;
            uniform.metallicFactor = entity.m_MetallicFactor;
            uniform.roughnessFactor = entity.m_RoughnessFactor;
            uniform.normalScale = entity.m_NormalScale;
            uniform.occlusionStrength = entity.m_OcclusionStrength;
            uniform.emissiveFactor = entity.m_EmissiveFactor;
            for (uint32_t param_index = 0; param_index < k_MaterialCustomParamCount; ++param_index)
            {
                uniform.customParams[param_index] = entity.m_CustomParams[param_index];
            }
        }
        rhi->UnmapMemory(staging_buffer_memory);

        rhi->CopyBuffer(staging_buffer, instance.material_uniform_buffer, 0, 0, buffer_size);

        rhi->DestroyBuffer(staging_buffer);
        rhi->FreeMemory(staging_buffer_memory);
    }

    // ---- 2. 描述符集：没有就分配，有就复用 ----
    if (instance.material_descriptor_set == RHI_NULL_HANDLE)
    {
        RHIDescriptorSetAllocateInfo alloc_info;
        alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.pNext = NULL;
        alloc_info.descriptorPool = rhi->GetDescriptorPoor();
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = m_MaterialDescriptorSetLayout;

        if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&alloc_info, instance.material_descriptor_set))
        {
            instance.material_descriptor_set = RHI_NULL_HANDLE;
            return;
        }
    }

    // 6 个绑定**每次全量重写**：instance id 被回收后可能换了一个 part，纹理视图和
    // 采样器也就换了；只写 UBO 会把上一任的纹理留在描述符集里。
    RHIDescriptorBufferInfo uniform_buffer_info = {};
    uniform_buffer_info.offset = 0;
    uniform_buffer_info.range = buffer_size;
    uniform_buffer_info.buffer = instance.material_uniform_buffer;

    RHIDescriptorImageInfo image_infos[5] = {};
    RHIImageView* views[5] = {instance.base_color_image_view,
                              instance.metallic_roughness_image_view,
                              instance.normal_image_view,
                              instance.occlusion_image_view,
                              instance.emissive_image_view};
    RHISampler* samplers[5] = {instance.base_color_sampler,
                               instance.metallic_roughness_sampler,
                               instance.normal_sampler,
                               instance.occlusion_sampler,
                               instance.emissive_sampler};
    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        image_infos[slot].imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_infos[slot].imageView = views[slot];
        image_infos[slot].sampler = samplers[slot];
    }

    RHIWriteDescriptorSet writes[6];

    writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = NULL;
    writes[0].dstSet = instance.material_descriptor_set;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &uniform_buffer_info;

    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        RHIWriteDescriptorSet& write = writes[slot + 1];
        write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext = NULL;
        write.dstSet = instance.material_descriptor_set;
        write.dstBinding = slot + 1;
        write.dstArrayElement = 0;
        write.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_infos[slot];
    }

    rhi->UpdateDescriptorSets(6, writes, 0, nullptr);
}

void RenderResource::UpdateMeshData(RHI* rhi,
                                    bool enable_vertex_blending,
                                    uint32_t index_buffer_size,
                                    void* index_buffer_data,
                                    uint32_t vertex_buffer_size,
                                    MeshVertexDataDefinition const* vertex_buffer_data,
                                    uint32_t joint_binding_buffer_size,
                                    MeshVertexBindingDataDefinition const* joint_binding_buffer_data,
                                    GpuMesh& now_mesh)
{
    now_mesh.enable_vertex_blending = enable_vertex_blending;
    assert(0 == (vertex_buffer_size % sizeof(MeshVertexDataDefinition)));
    now_mesh.mesh_vertex_count = vertex_buffer_size / sizeof(MeshVertexDataDefinition);
    UpdateVertexBuffer(rhi,
                       enable_vertex_blending,
                       vertex_buffer_size,
                       vertex_buffer_data,
                       joint_binding_buffer_size,
                       joint_binding_buffer_data,
                       index_buffer_size,
                       reinterpret_cast<uint16_t*>(index_buffer_data),
                       now_mesh);
    assert(0 == (index_buffer_size % sizeof(uint16_t)));
    now_mesh.mesh_index_count = index_buffer_size / sizeof(uint16_t);
    UpdateIndexBuffer(rhi, index_buffer_size, index_buffer_data, now_mesh);
}

void RenderResource::UpdateVertexBuffer(RHI* rhi,
                                        bool enable_vertex_blending,
                                        uint32_t vertex_buffer_size,
                                        MeshVertexDataDefinition const* vertex_buffer_data,
                                        uint32_t joint_binding_buffer_size,
                                        MeshVertexBindingDataDefinition const* joint_binding_buffer_data,
                                        uint32_t index_buffer_size,
                                        uint16_t* index_buffer_data,
                                        GpuMesh& now_mesh)
{
    if (enable_vertex_blending)
    {
        assert(0 == (vertex_buffer_size % sizeof(MeshVertexDataDefinition)));
        uint32_t vertex_count = vertex_buffer_size / sizeof(MeshVertexDataDefinition);
        assert(0 == (index_buffer_size % sizeof(uint16_t)));
        uint32_t index_count = index_buffer_size / sizeof(uint16_t);

        RHIDeviceSize vertex_position_buffer_size = sizeof(MeshVertex::Position) * vertex_count;
        RHIDeviceSize vertex_varying_enable_blending_buffer_size =
            sizeof(MeshVertex::VaryingBlending) * vertex_count;
        RHIDeviceSize vertex_varying_buffer_size = sizeof(MeshVertex::Varying) * vertex_count;
        RHIDeviceSize vertex_joint_binding_buffer_size = sizeof(MeshVertex::JointBinding) * index_count;

        RHIDeviceSize vertex_position_buffer_offset = 0;
        RHIDeviceSize vertex_varying_enable_blending_buffer_offset =
            vertex_position_buffer_offset + vertex_position_buffer_size;
        RHIDeviceSize vertex_varying_buffer_offset =
            vertex_varying_enable_blending_buffer_offset + vertex_varying_enable_blending_buffer_size;
        RHIDeviceSize vertex_joint_binding_buffer_offset = vertex_varying_buffer_offset + vertex_varying_buffer_size;

        // temporary staging buffer
        RHIDeviceSize inefficient_staging_buffer_size = vertex_position_buffer_size +
                                                        vertex_varying_enable_blending_buffer_size +
                                                        vertex_varying_buffer_size + vertex_joint_binding_buffer_size;
        RHIBuffer* inefficient_staging_buffer = RHI_NULL_HANDLE;
        RHIDeviceMemory* inefficient_staging_buffer_memory = RHI_NULL_HANDLE;
        rhi->CreateBuffer(inefficient_staging_buffer_size,
                          RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          inefficient_staging_buffer,
                          inefficient_staging_buffer_memory);

        void* inefficient_staging_buffer_data;
        rhi->MapMemory(inefficient_staging_buffer_memory, 0, RHI_WHOLE_SIZE, 0, &inefficient_staging_buffer_data);

        MeshVertex::Position* mesh_vertex_positions =
            reinterpret_cast<MeshVertex::Position*>(
                reinterpret_cast<uintptr_t>(inefficient_staging_buffer_data) + vertex_position_buffer_offset);
        MeshVertex::VaryingBlending* mesh_vertex_blending_varyings =
            reinterpret_cast<MeshVertex::VaryingBlending*>(
                reinterpret_cast<uintptr_t>(inefficient_staging_buffer_data) +
                vertex_varying_enable_blending_buffer_offset);
        MeshVertex::Varying* mesh_vertex_varyings =
            reinterpret_cast<MeshVertex::Varying*>(
                reinterpret_cast<uintptr_t>(inefficient_staging_buffer_data) + vertex_varying_buffer_offset);
        MeshVertex::JointBinding* mesh_vertex_joint_binding =
            reinterpret_cast<MeshVertex::JointBinding*>(
                reinterpret_cast<uintptr_t>(inefficient_staging_buffer_data) + vertex_joint_binding_buffer_offset);

        for (uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index)
        {
            Vector3 normal = Vector3(vertex_buffer_data[vertex_index].nx,
                                     vertex_buffer_data[vertex_index].ny,
                                     vertex_buffer_data[vertex_index].nz);
            Vector3 tangent = Vector3(vertex_buffer_data[vertex_index].tx,
                                      vertex_buffer_data[vertex_index].ty,
                                      vertex_buffer_data[vertex_index].tz);

            mesh_vertex_positions[vertex_index].position = Vector3(vertex_buffer_data[vertex_index].x,
                                                                   vertex_buffer_data[vertex_index].y,
                                                                   vertex_buffer_data[vertex_index].z);

            mesh_vertex_blending_varyings[vertex_index].normal = normal;
            mesh_vertex_blending_varyings[vertex_index].tangent = tangent;

            mesh_vertex_varyings[vertex_index].texcoord =
                Vector2(vertex_buffer_data[vertex_index].u, vertex_buffer_data[vertex_index].v);
        }

        for (uint32_t index_index = 0; index_index < index_count; ++index_index)
        {
            uint32_t vertex_buffer_index = index_buffer_data[index_index];

            // TODO: move to assets loading process

            mesh_vertex_joint_binding[index_index].indices[0] = joint_binding_buffer_data[vertex_buffer_index].m_Index0;
            mesh_vertex_joint_binding[index_index].indices[1] = joint_binding_buffer_data[vertex_buffer_index].m_Index1;
            mesh_vertex_joint_binding[index_index].indices[2] = joint_binding_buffer_data[vertex_buffer_index].m_Index2;
            mesh_vertex_joint_binding[index_index].indices[3] = joint_binding_buffer_data[vertex_buffer_index].m_Index3;

            float inv_total_weight = joint_binding_buffer_data[vertex_buffer_index].m_Weight0 +
                                     joint_binding_buffer_data[vertex_buffer_index].m_Weight1 +
                                     joint_binding_buffer_data[vertex_buffer_index].m_Weight2 +
                                     joint_binding_buffer_data[vertex_buffer_index].m_Weight3;

            inv_total_weight = (inv_total_weight != 0.0) ? 1 / inv_total_weight : 1.0;

            mesh_vertex_joint_binding[index_index].weights =
                Vector4(joint_binding_buffer_data[vertex_buffer_index].m_Weight0 * inv_total_weight,
                        joint_binding_buffer_data[vertex_buffer_index].m_Weight1 * inv_total_weight,
                        joint_binding_buffer_data[vertex_buffer_index].m_Weight2 * inv_total_weight,
                        joint_binding_buffer_data[vertex_buffer_index].m_Weight3 * inv_total_weight);
        }

        rhi->UnmapMemory(inefficient_staging_buffer_memory);

        // use the vmaAllocator to allocate asset vertex buffer
        RHIBufferCreateInfo bufferInfo = {RHI_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

        bufferInfo.usage = RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.size = vertex_position_buffer_size;
        CreateAssetBuffer(rhi,
                          bufferInfo,
                          now_mesh.mesh_vertex_position_buffer,
                          (void**)&now_mesh.mesh_vertex_position_buffer_allocation);
        bufferInfo.size = vertex_varying_enable_blending_buffer_size;
        CreateAssetBuffer(rhi,
                          bufferInfo,
                          now_mesh.mesh_vertex_varying_enable_blending_buffer,
                          (void**)&now_mesh.mesh_vertex_varying_enable_blending_buffer_allocation);
        bufferInfo.size = vertex_varying_buffer_size;
        CreateAssetBuffer(rhi,
                          bufferInfo,
                          now_mesh.mesh_vertex_varying_buffer,
                          (void**)&now_mesh.mesh_vertex_varying_buffer_allocation);

        bufferInfo.usage = RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.size = vertex_joint_binding_buffer_size;
        CreateAssetBuffer(rhi,
                          bufferInfo,
                          now_mesh.mesh_vertex_joint_binding_buffer,
                          (void**)&now_mesh.mesh_vertex_joint_binding_buffer_allocation);

        // use the data from staging buffer
        rhi->CopyBuffer(inefficient_staging_buffer,
                        now_mesh.mesh_vertex_position_buffer,
                        vertex_position_buffer_offset,
                        0,
                        vertex_position_buffer_size);
        rhi->CopyBuffer(inefficient_staging_buffer,
                        now_mesh.mesh_vertex_varying_enable_blending_buffer,
                        vertex_varying_enable_blending_buffer_offset,
                        0,
                        vertex_varying_enable_blending_buffer_size);
        rhi->CopyBuffer(inefficient_staging_buffer,
                        now_mesh.mesh_vertex_varying_buffer,
                        vertex_varying_buffer_offset,
                        0,
                        vertex_varying_buffer_size);
        rhi->CopyBuffer(inefficient_staging_buffer,
                        now_mesh.mesh_vertex_joint_binding_buffer,
                        vertex_joint_binding_buffer_offset,
                        0,
                        vertex_joint_binding_buffer_size);

        // release staging buffer
        rhi->DestroyBuffer(inefficient_staging_buffer);
        rhi->FreeMemory(inefficient_staging_buffer_memory);

        // update descriptor set
        RHIDescriptorSetAllocateInfo mesh_vertex_blending_per_mesh_descriptor_set_alloc_info;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.pNext = NULL;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.descriptorPool = rhi->GetDescriptorPoor();
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.descriptorSetCount = 1;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.pSetLayouts = m_MeshDescriptorSetLayout;

        if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&mesh_vertex_blending_per_mesh_descriptor_set_alloc_info,
                                                       now_mesh.mesh_vertex_blending_descriptor_set))
        {
            throw std::runtime_error("allocate mesh vertex blending per mesh descriptor set");
        }

        RHIDescriptorBufferInfo mesh_vertex_Joint_binding_storage_buffer_info = {};
        mesh_vertex_Joint_binding_storage_buffer_info.offset = 0;
        mesh_vertex_Joint_binding_storage_buffer_info.range = vertex_joint_binding_buffer_size;
        mesh_vertex_Joint_binding_storage_buffer_info.buffer = now_mesh.mesh_vertex_joint_binding_buffer;
        assert(mesh_vertex_Joint_binding_storage_buffer_info.range <
               m_GlobalRenderResource.m_StorageBuffer.m_MaxStorageBufferRange);

        RHIDescriptorSet* descriptor_set_to_write = now_mesh.mesh_vertex_blending_descriptor_set;

        RHIWriteDescriptorSet descriptor_writes[1];

        RHIWriteDescriptorSet& mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info =
            descriptor_writes[0];
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.sType =
            RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.pNext = NULL;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.dstSet = descriptor_set_to_write;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.dstBinding = 0;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.dstArrayElement = 0;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.descriptorType =
            RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.descriptorCount = 1;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.pBufferInfo =
            &mesh_vertex_Joint_binding_storage_buffer_info;

        rhi->UpdateDescriptorSets(
            (sizeof(descriptor_writes) / sizeof(descriptor_writes[0])), descriptor_writes, 0, NULL);
    }
    else
    {
        assert(0 == (vertex_buffer_size % sizeof(MeshVertexDataDefinition)));
        uint32_t vertex_count = vertex_buffer_size / sizeof(MeshVertexDataDefinition);

        RHIDeviceSize vertex_position_buffer_size = sizeof(MeshVertex::Position) * vertex_count;
        RHIDeviceSize vertex_varying_enable_blending_buffer_size =
            sizeof(MeshVertex::VaryingBlending) * vertex_count;
        RHIDeviceSize vertex_varying_buffer_size = sizeof(MeshVertex::Varying) * vertex_count;

        RHIDeviceSize vertex_position_buffer_offset = 0;
        RHIDeviceSize vertex_varying_enable_blending_buffer_offset =
            vertex_position_buffer_offset + vertex_position_buffer_size;
        RHIDeviceSize vertex_varying_buffer_offset =
            vertex_varying_enable_blending_buffer_offset + vertex_varying_enable_blending_buffer_size;

        // temporary staging buffer
        RHIDeviceSize inefficient_staging_buffer_size =
            vertex_position_buffer_size + vertex_varying_enable_blending_buffer_size + vertex_varying_buffer_size;
        RHIBuffer* inefficient_staging_buffer = RHI_NULL_HANDLE;
        RHIDeviceMemory* inefficient_staging_buffer_memory = RHI_NULL_HANDLE;
        rhi->CreateBuffer(inefficient_staging_buffer_size,
                          RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          inefficient_staging_buffer,
                          inefficient_staging_buffer_memory);

        void* inefficient_staging_buffer_data;
        rhi->MapMemory(inefficient_staging_buffer_memory, 0, RHI_WHOLE_SIZE, 0, &inefficient_staging_buffer_data);

        MeshVertex::Position* mesh_vertex_positions =
            reinterpret_cast<MeshVertex::Position*>(
                reinterpret_cast<uintptr_t>(inefficient_staging_buffer_data) + vertex_position_buffer_offset);
        MeshVertex::VaryingBlending* mesh_vertex_blending_varyings =
            reinterpret_cast<MeshVertex::VaryingBlending*>(
                reinterpret_cast<uintptr_t>(inefficient_staging_buffer_data) +
                vertex_varying_enable_blending_buffer_offset);
        MeshVertex::Varying* mesh_vertex_varyings =
            reinterpret_cast<MeshVertex::Varying*>(
                reinterpret_cast<uintptr_t>(inefficient_staging_buffer_data) + vertex_varying_buffer_offset);

        for (uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index)
        {
            Vector3 normal = Vector3(vertex_buffer_data[vertex_index].nx,
                                     vertex_buffer_data[vertex_index].ny,
                                     vertex_buffer_data[vertex_index].nz);
            Vector3 tangent = Vector3(vertex_buffer_data[vertex_index].tx,
                                      vertex_buffer_data[vertex_index].ty,
                                      vertex_buffer_data[vertex_index].tz);

            mesh_vertex_positions[vertex_index].position = Vector3(vertex_buffer_data[vertex_index].x,
                                                                   vertex_buffer_data[vertex_index].y,
                                                                   vertex_buffer_data[vertex_index].z);

            mesh_vertex_blending_varyings[vertex_index].normal = normal;
            mesh_vertex_blending_varyings[vertex_index].tangent = tangent;

            mesh_vertex_varyings[vertex_index].texcoord =
                Vector2(vertex_buffer_data[vertex_index].u, vertex_buffer_data[vertex_index].v);
        }

        rhi->UnmapMemory(inefficient_staging_buffer_memory);

        // use the vmaAllocator to allocate asset vertex buffer
        RHIBufferCreateInfo bufferInfo = {RHI_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.usage = RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;

        bufferInfo.size = vertex_position_buffer_size;
        CreateAssetBuffer(rhi,
                          bufferInfo,
                          now_mesh.mesh_vertex_position_buffer,
                          (void**)&now_mesh.mesh_vertex_position_buffer_allocation);
        bufferInfo.size = vertex_varying_enable_blending_buffer_size;
        CreateAssetBuffer(rhi,
                          bufferInfo,
                          now_mesh.mesh_vertex_varying_enable_blending_buffer,
                          (void**)&now_mesh.mesh_vertex_varying_enable_blending_buffer_allocation);
        bufferInfo.size = vertex_varying_buffer_size;
        CreateAssetBuffer(rhi,
                          bufferInfo,
                          now_mesh.mesh_vertex_varying_buffer,
                          (void**)&now_mesh.mesh_vertex_varying_buffer_allocation);

        // use the data from staging buffer
        rhi->CopyBuffer(inefficient_staging_buffer,
                        now_mesh.mesh_vertex_position_buffer,
                        vertex_position_buffer_offset,
                        0,
                        vertex_position_buffer_size);
        rhi->CopyBuffer(inefficient_staging_buffer,
                        now_mesh.mesh_vertex_varying_enable_blending_buffer,
                        vertex_varying_enable_blending_buffer_offset,
                        0,
                        vertex_varying_enable_blending_buffer_size);
        rhi->CopyBuffer(inefficient_staging_buffer,
                        now_mesh.mesh_vertex_varying_buffer,
                        vertex_varying_buffer_offset,
                        0,
                        vertex_varying_buffer_size);

        // release staging buffer
        rhi->DestroyBuffer(inefficient_staging_buffer);
        rhi->FreeMemory(inefficient_staging_buffer_memory);

        // update descriptor set
        RHIDescriptorSetAllocateInfo mesh_vertex_blending_per_mesh_descriptor_set_alloc_info;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.pNext = NULL;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.descriptorPool = rhi->GetDescriptorPoor();
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.descriptorSetCount = 1;
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.pSetLayouts = m_MeshDescriptorSetLayout;

        if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&mesh_vertex_blending_per_mesh_descriptor_set_alloc_info,
                                                       now_mesh.mesh_vertex_blending_descriptor_set))
        {
            throw std::runtime_error("allocate mesh vertex blending per mesh descriptor set");
        }

        RHIDescriptorBufferInfo mesh_vertex_Joint_binding_storage_buffer_info = {};
        mesh_vertex_Joint_binding_storage_buffer_info.offset = 0;
        mesh_vertex_Joint_binding_storage_buffer_info.range = 1;
        mesh_vertex_Joint_binding_storage_buffer_info.buffer =
            m_GlobalRenderResource.m_StorageBuffer.m_GlobalNullDescriptorStorageBuffer;
        assert(mesh_vertex_Joint_binding_storage_buffer_info.range <
               m_GlobalRenderResource.m_StorageBuffer.m_MaxStorageBufferRange);

        RHIDescriptorSet* descriptor_set_to_write = now_mesh.mesh_vertex_blending_descriptor_set;

        RHIWriteDescriptorSet descriptor_writes[1];

        RHIWriteDescriptorSet& mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info =
            descriptor_writes[0];
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.sType =
            RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.pNext = NULL;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.dstSet = descriptor_set_to_write;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.dstBinding = 0;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.dstArrayElement = 0;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.descriptorType =
            RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.descriptorCount = 1;
        mesh_vertex_blending_vertex_Joint_binding_storage_buffer_write_info.pBufferInfo =
            &mesh_vertex_Joint_binding_storage_buffer_info;

        rhi->UpdateDescriptorSets(
            (sizeof(descriptor_writes) / sizeof(descriptor_writes[0])), descriptor_writes, 0, NULL);
    }
}

void RenderResource::UpdateIndexBuffer(RHI* rhi,
                                       uint32_t index_buffer_size,
                                       void* index_buffer_data,
                                       GpuMesh& now_mesh)
{
    // temp staging buffer
    RHIDeviceSize buffer_size = index_buffer_size;

    RHIBuffer* inefficient_staging_buffer;
    RHIDeviceMemory* inefficient_staging_buffer_memory;
    rhi->CreateBuffer(buffer_size,
                      RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      inefficient_staging_buffer,
                      inefficient_staging_buffer_memory);

    void* staging_buffer_data;
    rhi->MapMemory(inefficient_staging_buffer_memory, 0, buffer_size, 0, &staging_buffer_data);
    memcpy(staging_buffer_data, index_buffer_data, (size_t)buffer_size);
    rhi->UnmapMemory(inefficient_staging_buffer_memory);

    // use the vmaAllocator to allocate asset index buffer
    RHIBufferCreateInfo bufferInfo = {RHI_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = buffer_size;
    bufferInfo.usage = RHI_BUFFER_USAGE_INDEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;

    CreateAssetBuffer(rhi,
                      bufferInfo,
                      now_mesh.mesh_index_buffer,
                      (void**)&now_mesh.mesh_index_buffer_allocation);

    // use the data from staging buffer
    rhi->CopyBuffer(inefficient_staging_buffer, now_mesh.mesh_index_buffer, 0, 0, buffer_size);

    // release temp staging buffer
    rhi->DestroyBuffer(inefficient_staging_buffer);
    rhi->FreeMemory(inefficient_staging_buffer_memory);
}

void RenderResource::UpdateTextureImageData(RHI* rhi, const TextureDataToUpdate& texture_data)
{
    // miplevels passthrough: preserve the legacy sentinel 0 for single-mip
    // (uncompressed) uploads -- DX12 treats 0 as 1, Vulkan treats 0 as
    // "GPU-generate the full chain", exactly the pre-cook behaviour. Only a
    // cooked chain (>1) passes an explicit pre-supplied mip count.
    auto upload_mips = [](uint32_t n) -> uint32_t { return n > 1u ? n : 0u; };

    rhi->CreateGlobalImage(texture_data.now_material->base_color_texture_image,
                           texture_data.now_material->base_color_image_view,
                           texture_data.now_material->base_color_image_allocation,
                           texture_data.base_color_image_width,
                           texture_data.base_color_image_height,
                           texture_data.base_color_image_pixels,
                           texture_data.base_color_image_format,
                           upload_mips(texture_data.base_color_image_miplevels));

    rhi->CreateGlobalImage(texture_data.now_material->metallic_roughness_texture_image,
                           texture_data.now_material->metallic_roughness_image_view,
                           texture_data.now_material->metallic_roughness_image_allocation,
                           texture_data.metallic_roughness_image_width,
                           texture_data.metallic_roughness_image_height,
                           texture_data.metallic_roughness_image_pixels,
                           texture_data.metallic_roughness_image_format,
                           upload_mips(texture_data.metallic_roughness_image_miplevels));

    rhi->CreateGlobalImage(texture_data.now_material->normal_texture_image,
                           texture_data.now_material->normal_image_view,
                           texture_data.now_material->normal_image_allocation,
                           texture_data.normal_roughness_image_width,
                           texture_data.normal_roughness_image_height,
                           texture_data.normal_roughness_image_pixels,
                           texture_data.normal_roughness_image_format,
                           upload_mips(texture_data.normal_roughness_image_miplevels));

    rhi->CreateGlobalImage(texture_data.now_material->occlusion_texture_image,
                           texture_data.now_material->occlusion_image_view,
                           texture_data.now_material->occlusion_image_allocation,
                           texture_data.occlusion_image_width,
                           texture_data.occlusion_image_height,
                           texture_data.occlusion_image_pixels,
                           texture_data.occlusion_image_format,
                           upload_mips(texture_data.occlusion_image_miplevels));

    rhi->CreateGlobalImage(texture_data.now_material->emissive_texture_image,
                           texture_data.now_material->emissive_image_view,
                           texture_data.now_material->emissive_image_allocation,
                           texture_data.emissive_image_width,
                           texture_data.emissive_image_height,
                           texture_data.emissive_image_pixels,
                           texture_data.emissive_image_format,
                           upload_mips(texture_data.emissive_image_miplevels));
}

RenderMeshGPUResource& RenderResource::GetEntityMesh(RenderEntity entity)
{
    size_t assetid = entity.m_MeshAssetId;

    auto it = m_Meshes.find(assetid);
    if (it != m_Meshes.end())
    {
        return it->second;
    }
    else
    {
        throw std::runtime_error("failed to get entity mesh");
    }
}

RenderMaterialGPUResource& RenderResource::GetEntityMaterial(RenderEntity entity)
{
    // 运行时材质实例优先。拿不到就回退到共享资产材质 —— 绝不能在这里抛：
    // 这条路径在可见性剔除里每帧跑，抛一次整个帧就崩了。回退的代价只是"override
    // 不生效"，与加这个功能之前的行为一致。
    if (entity.m_MaterialInstanceId != 0)
    {
        auto instance_it = m_MaterialInstances.find(entity.m_MaterialInstanceId);
        if (instance_it != m_MaterialInstances.end())
        {
            return instance_it->second;
        }
    }

    size_t assetid = entity.m_MaterialAssetId;

    auto it = m_Materials.find(assetid);
    if (it != m_Materials.end())
    {
        return it->second;
    }
    else
    {
        throw std::runtime_error("failed to get entity material");
    }
}

void RenderResource::ResetRingBufferOffset(uint8_t current_frame_index)
{
    m_GlobalRenderResource.m_StorageBuffer.m_GlobalUploadRingbuffersEnd[current_frame_index] =
        m_GlobalRenderResource.m_StorageBuffer.m_GlobalUploadRingbuffersBegin[current_frame_index];
}

void RenderResource::CreateAndMapStorageBuffer(RHI* rhi)
{
    if (rhi == nullptr)
    {
        throw std::runtime_error("RenderResource::CreateAndMapStorageBuffer requires a valid RHI");
    }

    StorageBuffer& m_StorageBuffer = m_GlobalRenderResource.m_StorageBuffer;
    uint32_t frames_in_flight = std::max<uint8_t>(rhi->GetMaxFramesInFlight(), 1);

    RHIPhysicalDeviceProperties properties {};
    rhi->GetPhysicalDeviceProperties(&properties);

    m_StorageBuffer.m_MinUniformBufferOffsetAlignment =
        static_cast<uint32_t>(properties.limits.minUniformBufferOffsetAlignment != 0 ? properties.limits.minUniformBufferOffsetAlignment : 256);
    m_StorageBuffer.m_MinStorageBufferOffsetAlignment =
        static_cast<uint32_t>(properties.limits.minStorageBufferOffsetAlignment != 0 ? properties.limits.minStorageBufferOffsetAlignment : 256);
    m_StorageBuffer.m_MaxStorageBufferRange = properties.limits.maxStorageBufferRange != 0 ? properties.limits.maxStorageBufferRange : (1 << 27);
    m_StorageBuffer.m_NonCoherentAtomSize = properties.limits.nonCoherentAtomSize != 0 ? properties.limits.nonCoherentAtomSize : 256;

    // In Vulkan, the storage buffer should be pre-allocated.
    // The size is 128MB in NVIDIA D3D11
    // driver(https://developer.nvidia.com/content/constant-buffers-without-constant-pain-0).
    uint32_t global_storage_buffer_size = 1024 * 1024 * 128;
    rhi->CreateBuffer(global_storage_buffer_size,
                      RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_StorageBuffer.m_GlobalUploadRingbuffer,
                      m_StorageBuffer.m_GlobalUploadRingbufferMemory);

    m_StorageBuffer.m_GlobalUploadRingbuffersBegin.resize(frames_in_flight);
    m_StorageBuffer.m_GlobalUploadRingbuffersEnd.resize(frames_in_flight);
    m_StorageBuffer.m_GlobalUploadRingbuffersSize.resize(frames_in_flight);
    for (uint32_t i = 0; i < frames_in_flight; ++i)
    {
        m_StorageBuffer.m_GlobalUploadRingbuffersBegin[i] = (global_storage_buffer_size * i) / frames_in_flight;
        m_StorageBuffer.m_GlobalUploadRingbuffersSize[i] = (global_storage_buffer_size * (i + 1)) / frames_in_flight -
                                                           (global_storage_buffer_size * i) / frames_in_flight;
        m_StorageBuffer.m_GlobalUploadRingbuffersEnd[i] = m_StorageBuffer.m_GlobalUploadRingbuffersBegin[i];
    }

    // axis
    rhi->CreateBuffer(sizeof(AxisDrawStorage),
                      RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_StorageBuffer.m_AxisInefficientStorageBuffer,
                      m_StorageBuffer.m_AxisInefficientStorageBufferMemory);

    // null descriptor
    rhi->CreateBuffer(64,
                      RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      0,
                      m_StorageBuffer.m_GlobalNullDescriptorStorageBuffer,
                      m_StorageBuffer.m_GlobalNullDescriptorStorageBufferMemory);

    // TODO: Unmap when program terminates
    rhi->MapMemory(m_StorageBuffer.m_GlobalUploadRingbufferMemory,
                   0,
                   RHI_WHOLE_SIZE,
                   0,
                   &m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer);

    rhi->MapMemory(m_StorageBuffer.m_AxisInefficientStorageBufferMemory,
                   0,
                   RHI_WHOLE_SIZE,
                   0,
                   &m_StorageBuffer.m_AxisInefficientStorageBufferMemoryPointer);

    static_assert(64 >= sizeof(MeshVertex::JointBinding), "");
}
#endif  // Z_HAS_VULKAN || __APPLE__

// 【更新】主体守卫现已纳入 Emscripten（见文件头），clear() 已在上面的主体里定义，
// 故本块的 clear() 桩不再需要——否则与主体重复定义。保留注释以记录这段历史：
// 过去 Web 只走这里（key function 给 vtable/typeinfo 一个 home TU），现在主体
// 的非 Vulkan 通用 RHI 路径已把 Web 编进来，clear() 随之产出，无需单独桩。
//
// 不含 Windows：RenderResourceDx12.cpp 在 Windows 上编，并且已经在自己的
// !Z_HAS_VULKAN 块里定义了 clear()，这里再定义一次就是重复符号。
// #if defined(__EMSCRIPTEN__)   // 已并入主体守卫，桩移除
// void RenderResource::clear()
// {
//     ClearMeshGeometryCache();
//
//     if (m_TextureStreamingManager)
//     {
//         m_TextureStreamingManager->Shutdown();
//         m_TextureStreamingManager.reset();
//     }
// }
// #endif
