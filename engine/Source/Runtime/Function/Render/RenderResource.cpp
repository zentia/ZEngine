#include "Runtime/Function/Render/RenderResource.h"

#if defined(Z_HAS_VULKAN)
    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanUtil.h"
    #include "Runtime/Function/Render/Passes/MainCameraPass.h"
    #include "Runtime/Function/Render/RenderCamera.h"
    #include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
    #include "Runtime/Function/Render/RenderHelper.h"
    #include "Runtime/Function/Render/RenderMesh.h"
    #include "Runtime/Profiler/Profiler.h"

    #include <stdexcept>

void RenderResource::clear()
{
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

    // sky box irradiance
    SkyBoxIrradianceMap skybox_irradiance_map = level_resource_desc.m_IblResourceDesc.m_SkyboxIrradianceMap;
    std::shared_ptr<TextureData> irradiace_pos_x_map = LoadTextureHDR(skybox_irradiance_map.m_PositiveXMap);
    std::shared_ptr<TextureData> irradiace_neg_x_map = LoadTextureHDR(skybox_irradiance_map.m_NegativeXMap);
    std::shared_ptr<TextureData> irradiace_pos_y_map = LoadTextureHDR(skybox_irradiance_map.m_PositiveYMap);
    std::shared_ptr<TextureData> irradiace_neg_y_map = LoadTextureHDR(skybox_irradiance_map.m_NegativeYMap);
    std::shared_ptr<TextureData> irradiace_pos_z_map = LoadTextureHDR(skybox_irradiance_map.m_PositiveZMap);
    std::shared_ptr<TextureData> irradiace_neg_z_map = LoadTextureHDR(skybox_irradiance_map.m_NegativeZMap);

    // sky box specular
    SkyBoxSpecularMap skybox_specular_map = level_resource_desc.m_IblResourceDesc.m_SkyboxSpecularMap;
    std::shared_ptr<TextureData> specular_pos_x_map = LoadTextureHDR(skybox_specular_map.m_PositiveXMap);
    std::shared_ptr<TextureData> specular_neg_x_map = LoadTextureHDR(skybox_specular_map.m_NegativeXMap);
    std::shared_ptr<TextureData> specular_pos_y_map = LoadTextureHDR(skybox_specular_map.m_PositiveYMap);
    std::shared_ptr<TextureData> specular_neg_y_map = LoadTextureHDR(skybox_specular_map.m_NegativeYMap);
    std::shared_ptr<TextureData> specular_pos_z_map = LoadTextureHDR(skybox_specular_map.m_PositiveZMap);
    std::shared_ptr<TextureData> specular_neg_z_map = LoadTextureHDR(skybox_specular_map.m_NegativeZMap);

    // brdf
    std::shared_ptr<TextureData> brdf_map = LoadTextureHDR(level_resource_desc.m_IblResourceDesc.m_BrdfMap);

    // HDR IBL cubemap + BRDF upload can remove the DX12 device during editor startup
    // (even with mip0-only uploads). RP1/RP2 already have 1x1 fallback textures on DX12.
    if (rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
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
    }
    else
    {
        LOG_INFO(ZRender,
                 "RenderResource(DX12): skipping startup IBL HDR GPU upload; MainCamera RP1 fallbacks are used");
    }

    // color grading
    std::shared_ptr<TextureData> color_grading_map =
        LoadTexture(level_resource_desc.m_ColorGradingResourceDesc.m_ColorGradingMap);

    // create color grading texture
    rhi->CreateGlobalImage(m_GlobalRenderResource.m_ColorGradingResource.m_ColorGradingLutTextureImage,
                           m_GlobalRenderResource.m_ColorGradingResource.m_ColorGradingLutTextureImageView,
                           m_GlobalRenderResource.m_ColorGradingResource.m_ColorGradingLutTextureImageAllocation,
                           color_grading_map->m_Width,
                           color_grading_map->m_Height,
                           color_grading_map->m_Pixels,
                           color_grading_map->m_Format);
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

void RenderResource::UpdatePerFrameBuffer(std::shared_ptr<RenderScene> render_scene,
                                          std::shared_ptr<RenderCamera> camera)
{
    Z_PROFILE_SCOPE("RenderResource::updatePerFrameBuffer");
    Matrix4x4 view_matrix = camera->GetViewMatrix();
    Matrix4x4 proj_matrix = camera->GetProjectionMatrix();
    Vector3 camera_position = camera->position();
    Matrix4x4 proj_view_matrix = proj_matrix * view_matrix;

    ViewportType viewport_type =
        (camera->m_CurrentCameraType == RenderCameraType::Game) ? ViewportType::game : ViewportType::scene;
    size_t viewport_index = static_cast<size_t>(viewport_type);

    auto& mesh_perframe_storage_buffer_object = m_MainCameraPerFrameByViewport[viewport_index];
    auto& particle_collision_perframe_storage_buffer_object =
        m_ParticleCollisionPerFrameByViewport[viewport_index];
    auto& particlebillboard_perframe_storage_buffer_object =
        m_ParticleBillboardPerFrameByViewport[viewport_index];

    // ambient light
    Vector3 ambient_light = render_scene->m_AmbientLight.m_Irradiance;
    uint32_t point_light_num = static_cast<uint32_t>(render_scene->m_PointLightList.m_Lights.size());

    // set ubo data
    particle_collision_perframe_storage_buffer_object.view_matrix = view_matrix;
    particle_collision_perframe_storage_buffer_object.proj_view_matrix = proj_view_matrix;
    particle_collision_perframe_storage_buffer_object.proj_inv_matrix = proj_matrix.inverse();

    mesh_perframe_storage_buffer_object.proj_view_matrix = proj_view_matrix;
    mesh_perframe_storage_buffer_object.camera_position = camera_position;
    mesh_perframe_storage_buffer_object.ambient_light = ambient_light;
    mesh_perframe_storage_buffer_object.point_light_num = point_light_num;
    mesh_perframe_storage_buffer_object.show_skybox = 1U;

    m_PointLightShadowPerFrame.point_light_num = point_light_num;
    // point lights
    for (uint32_t i = 0; i < point_light_num; i++)
    {
        Vector3 point_light_position = render_scene->m_PointLightList.m_Lights[i].m_Position;
        Vector3 point_light_intensity = render_scene->m_PointLightList.m_Lights[i].m_Flux / (4.0f * Math_PI);

        float radius = render_scene->m_PointLightList.m_Lights[i].calculateRadius();

        mesh_perframe_storage_buffer_object.scene_point_lights[i].position = point_light_position;
        mesh_perframe_storage_buffer_object.scene_point_lights[i].radius = radius;
        mesh_perframe_storage_buffer_object.scene_point_lights[i].intensity = point_light_intensity;

        m_PointLightShadowPerFrame.point_lights_position_and_radius[i] =
            Vector4(point_light_position, radius);
    }

    // directional light
    mesh_perframe_storage_buffer_object.scene_directional_light.direction =
        render_scene->m_DirectionalLight.m_Direction.normalisedCopy();
    mesh_perframe_storage_buffer_object.scene_directional_light.color = render_scene->m_DirectionalLight.m_Color;

    // pick pass view projection matrix
    m_PickPassPerFrame.proj_view_matrix = proj_view_matrix;

    particlebillboard_perframe_storage_buffer_object.proj_view_matrix = proj_view_matrix;
    particlebillboard_perframe_storage_buffer_object.right_direction = camera->right();
    particlebillboard_perframe_storage_buffer_object.forward_direction = camera->forward();
    particlebillboard_perframe_storage_buffer_object.up_direction = camera->up();

    m_ParticleCollisionPerFrame = particle_collision_perframe_storage_buffer_object;
    m_MainCameraPerFrame = mesh_perframe_storage_buffer_object;
    m_ParticleBillboardPerFrame = particlebillboard_perframe_storage_buffer_object;

    if (MegaLights::IsEnabled())
    {
        m_MegaLights.Update(*render_scene, camera, viewport_type);
        mesh_perframe_storage_buffer_object.point_light_num = 0;
        m_MainCameraPerFrame.point_light_num = 0;
    }
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

    auto it = m_Meshes.find(assetid);
    if (it != m_Meshes.end() && meshDrawDataIsValid(&it->second))
    {
        return it->second;
    }
    if (it != m_Meshes.end())
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

    now_mesh.backend = RenderResourceBackend::Vulkan;

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
    VulkanRHI* vulkan_context = static_cast<VulkanRHI*>(rhi);

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

            VmaAllocationCreateInfo allocInfo = {};
            allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            rhi->CreateBufferWithAlignmentVMA(
                vulkan_context->m_AssetsAllocator,
                &bufferInfo,
                &allocInfo,
                m_GlobalRenderResource.m_StorageBuffer.m_MinUniformBufferOffsetAlignment,
                now_material.material_uniform_buffer,
                (void**)&now_material.material_uniform_buffer_allocation,
                NULL);

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
        material_descriptor_set_alloc_info.descriptorPool = vulkan_context->m_DescriptorPool;
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

        RHIDescriptorImageInfo metallic_roughness_image_info = {};
        metallic_roughness_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        metallic_roughness_image_info.imageView = now_material.metallic_roughness_image_view;
        metallic_roughness_image_info.sampler =
            rhi->GetOrCreateMipmapSampler(metallic_roughness_width, metallic_roughness_height);

        RHIDescriptorImageInfo normal_roughness_image_info = {};
        normal_roughness_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        normal_roughness_image_info.imageView = now_material.normal_image_view;
        normal_roughness_image_info.sampler =
            rhi->GetOrCreateMipmapSampler(normal_roughness_width, normal_roughness_height);

        RHIDescriptorImageInfo occlusion_image_info = {};
        occlusion_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        occlusion_image_info.imageView = now_material.occlusion_image_view;
        occlusion_image_info.sampler = rhi->GetOrCreateMipmapSampler(occlusion_image_width, occlusion_image_height);

        RHIDescriptorImageInfo emissive_image_info = {};
        emissive_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        emissive_image_info.imageView = now_material.emissive_image_view;
        emissive_image_info.sampler = rhi->GetOrCreateMipmapSampler(emissive_image_width, emissive_image_height);

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
    VulkanRHI* vulkan_context = static_cast<VulkanRHI*>(rhi);

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

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        bufferInfo.usage = RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.size = vertex_position_buffer_size;
        rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                             &bufferInfo,
                             &allocInfo,
                             now_mesh.mesh_vertex_position_buffer,
                             (void**)&now_mesh.mesh_vertex_position_buffer_allocation,
                             NULL);
        bufferInfo.size = vertex_varying_enable_blending_buffer_size;
        rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                             &bufferInfo,
                             &allocInfo,
                             now_mesh.mesh_vertex_varying_enable_blending_buffer,
                             (void**)&now_mesh.mesh_vertex_varying_enable_blending_buffer_allocation,
                             NULL);
        bufferInfo.size = vertex_varying_buffer_size;
        rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                             &bufferInfo,
                             &allocInfo,
                             now_mesh.mesh_vertex_varying_buffer,
                             (void**)&now_mesh.mesh_vertex_varying_buffer_allocation,
                             NULL);

        bufferInfo.usage = RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.size = vertex_joint_binding_buffer_size;
        rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                             &bufferInfo,
                             &allocInfo,
                             now_mesh.mesh_vertex_joint_binding_buffer,
                             (void**)&now_mesh.mesh_vertex_joint_binding_buffer_allocation,
                             NULL);

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
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.descriptorPool = vulkan_context->m_DescriptorPool;
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

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        bufferInfo.size = vertex_position_buffer_size;
        rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                             &bufferInfo,
                             &allocInfo,
                             now_mesh.mesh_vertex_position_buffer,
                             (void**)&now_mesh.mesh_vertex_position_buffer_allocation,
                             NULL);
        bufferInfo.size = vertex_varying_enable_blending_buffer_size;
        rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                             &bufferInfo,
                             &allocInfo,
                             now_mesh.mesh_vertex_varying_enable_blending_buffer,
                             (void**)&now_mesh.mesh_vertex_varying_enable_blending_buffer_allocation,
                             NULL);
        bufferInfo.size = vertex_varying_buffer_size;
        rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                             &bufferInfo,
                             &allocInfo,
                             now_mesh.mesh_vertex_varying_buffer,
                             (void**)&now_mesh.mesh_vertex_varying_buffer_allocation,
                             NULL);

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
        mesh_vertex_blending_per_mesh_descriptor_set_alloc_info.descriptorPool = vulkan_context->m_DescriptorPool;
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
    VulkanRHI* vulkan_context = static_cast<VulkanRHI*>(rhi);

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

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    rhi->CreateBufferVMA(vulkan_context->m_AssetsAllocator,
                         &bufferInfo,
                         &allocInfo,
                         now_mesh.mesh_index_buffer,
                         (void**)&now_mesh.mesh_index_buffer_allocation,
                         NULL);

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
#endif  // Z_HAS_VULKAN
