// DX12 / non-Vulkan RenderResource implementations (mesh upload, global ring buffer, per-frame UBOs).

#include "Runtime/Function/Render/RenderResource.h"

#if defined(_WIN32)

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
    #include "Runtime/Function/Render/RenderCamera.h"
    #include "Runtime/Function/Render/RenderMesh.h"
    #include "Runtime/Function/Render/RenderScene.h"

    #include <algorithm>
    #include <cassert>
    #include <cstring>
    #include <stdexcept>

namespace
{
    void Dx12CreateGpuBufferFromStaging(RHI* rhi,
                                        RHIDeviceSize size,
                                        RHIBufferUsageFlags usage,
                                        const void* src_data,
                                        RHIBuffer*& out_buffer)
    {
        out_buffer = nullptr;
        if (rhi == nullptr || size == 0)
        {
            return;
        }

        RHIBuffer* staging_buffer = nullptr;
        RHIDeviceMemory* staging_memory = nullptr;
        rhi->CreateBuffer(size,
                          RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          staging_buffer,
                          staging_memory);

        void* mapped = nullptr;
        rhi->MapMemory(staging_memory, 0, size, 0, &mapped);
        if (mapped != nullptr && src_data != nullptr)
        {
            std::memcpy(mapped, src_data, static_cast<size_t>(size));
        }
        rhi->UnmapMemory(staging_memory);

        RHIDeviceMemory* gpu_memory = nullptr;
        rhi->CreateBuffer(size,
                          usage | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          out_buffer,
                          gpu_memory);

        rhi->CopyBuffer(staging_buffer, out_buffer, 0, 0, size);
        rhi->DestroyBuffer(staging_buffer);
        rhi->FreeMemory(staging_memory);
        if (gpu_memory != nullptr)
        {
            rhi->FreeMemory(gpu_memory);
        }
    }

    void Dx12UploadMeshNonSkinned(RHI* rhi, RenderResource* resource, GpuMesh& now_mesh, RenderMeshData& mesh_data)
    {
        const uint32_t index_buffer_size = static_cast<uint32_t>(mesh_data.m_StaticMeshData.m_IndexBuffer->m_Size);
        void* index_buffer_data = mesh_data.m_StaticMeshData.m_IndexBuffer->m_Data;

        const uint32_t vertex_buffer_size = static_cast<uint32_t>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Size);
        auto* vertex_buffer_data =
            reinterpret_cast<MeshVertexDataDefinition*>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data);

        assert(vertex_buffer_size % sizeof(MeshVertexDataDefinition) == 0);
        const uint32_t vertex_count = vertex_buffer_size / sizeof(MeshVertexDataDefinition);

        const RHIDeviceSize position_buffer_size = sizeof(MeshVertex::Position) * vertex_count;
        const RHIDeviceSize varying_blending_size =
            sizeof(MeshVertex::VaryingBlending) * vertex_count;
        const RHIDeviceSize varying_size = sizeof(MeshVertex::Varying) * vertex_count;

        const RHIDeviceSize staging_size = position_buffer_size + varying_blending_size + varying_size;
        RHIBuffer* staging_buffer = nullptr;
        RHIDeviceMemory* staging_memory = nullptr;
        rhi->CreateBuffer(staging_size,
                          RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          staging_buffer,
                          staging_memory);
        if (staging_buffer == nullptr || staging_memory == nullptr)
        {
            LOG_ERROR(ZRender, "RenderResource(DX12): failed to create mesh staging buffer (size={})",
                      static_cast<uint64_t>(staging_size));
            return;
        }

        void* staging_ptr = nullptr;
        rhi->MapMemory(staging_memory, 0, staging_size, 0, &staging_ptr);

        auto* positions = reinterpret_cast<MeshVertex::Position*>(staging_ptr);
        auto* blending_varyings =
            reinterpret_cast<MeshVertex::VaryingBlending*>(reinterpret_cast<uintptr_t>(staging_ptr) +
                                                                                 position_buffer_size);
        auto* varyings = reinterpret_cast<MeshVertex::Varying*>(
            reinterpret_cast<uintptr_t>(staging_ptr) + position_buffer_size + varying_blending_size);

        for (uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index)
        {
            Vector3 normal = Vector3(vertex_buffer_data[vertex_index].nx,
                                     vertex_buffer_data[vertex_index].ny,
                                     vertex_buffer_data[vertex_index].nz);
            Vector3 tangent = Vector3(vertex_buffer_data[vertex_index].tx,
                                      vertex_buffer_data[vertex_index].ty,
                                      vertex_buffer_data[vertex_index].tz);

            positions[vertex_index].position = Vector3(vertex_buffer_data[vertex_index].x,
                                                       vertex_buffer_data[vertex_index].y,
                                                       vertex_buffer_data[vertex_index].z);

            blending_varyings[vertex_index].normal = normal;
            blending_varyings[vertex_index].tangent = tangent;

            varyings[vertex_index].texcoord =
                Vector2(vertex_buffer_data[vertex_index].u, vertex_buffer_data[vertex_index].v);
        }

        rhi->UnmapMemory(staging_memory);

        RHIDeviceMemory* position_memory = nullptr;
        rhi->CreateBuffer(position_buffer_size,
                          RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          now_mesh.mesh_vertex_position_buffer,
                          position_memory);
        rhi->CopyBuffer(staging_buffer, now_mesh.mesh_vertex_position_buffer, 0, 0, position_buffer_size);

        RHIDeviceMemory* blending_memory = nullptr;
        rhi->CreateBuffer(varying_blending_size,
                          RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          now_mesh.mesh_vertex_varying_enable_blending_buffer,
                          blending_memory);
        rhi->CopyBuffer(
            staging_buffer, now_mesh.mesh_vertex_varying_enable_blending_buffer, position_buffer_size, 0, varying_blending_size);

        RHIDeviceMemory* varying_memory = nullptr;
        rhi->CreateBuffer(varying_size,
                          RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          now_mesh.mesh_vertex_varying_buffer,
                          varying_memory);
        rhi->CopyBuffer(staging_buffer,
                        now_mesh.mesh_vertex_varying_buffer,
                        position_buffer_size + varying_blending_size,
                        0,
                        varying_size);

        rhi->DestroyBuffer(staging_buffer);
        rhi->FreeMemory(staging_memory);
        if (position_memory != nullptr)
        {
            rhi->FreeMemory(position_memory);
        }
        if (blending_memory != nullptr)
        {
            rhi->FreeMemory(blending_memory);
        }
        if (varying_memory != nullptr)
        {
            rhi->FreeMemory(varying_memory);
        }

        assert(index_buffer_size % sizeof(uint16_t) == 0);
        now_mesh.mesh_index_count = index_buffer_size / sizeof(uint16_t);
        Dx12CreateGpuBufferFromStaging(rhi,
                                       index_buffer_size,
                                       RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       index_buffer_data,
                                       now_mesh.mesh_index_buffer);

        if (resource->m_MeshDescriptorSetLayout != nullptr)
        {
            RHIDescriptorSetAllocateInfo alloc_info {};
            alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = rhi->GetDescriptorPoor();
            alloc_info.descriptorSetCount = 1;
            alloc_info.pSetLayouts = resource->m_MeshDescriptorSetLayout;

            if (rhi->AllocateDescriptorSets(&alloc_info, now_mesh.mesh_vertex_blending_descriptor_set) == RHI_SUCCESS)
            {
                RHIDescriptorBufferInfo buffer_info {};
                buffer_info.offset = 0;
                buffer_info.range = 1;
                buffer_info.buffer = resource->m_GlobalRenderResource.m_StorageBuffer.m_GlobalNullDescriptorStorageBuffer;

                RHIWriteDescriptorSet write {};
                write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = now_mesh.mesh_vertex_blending_descriptor_set;
                write.dstBinding = 0;
                write.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.descriptorCount = 1;
                write.pBufferInfo = &buffer_info;
                rhi->UpdateDescriptorSets(1, &write, 0, nullptr);
            }
        }
    }

    void Dx12CreateWhiteTexture(RHI* rhi, RHIImage*& image, RHIImageView*& view)
    {
        image = nullptr;
        view = nullptr;
        if (rhi == nullptr)
        {
            return;
        }

        const uint8_t white_rgba[4] = {255, 255, 255, 255};
        rhi->CreateGlobalImage(image,
                               view,
                               nullptr,
                               1,
                               1,
                               const_cast<uint8_t*>(white_rgba),
                               RHI_FORMAT_R8G8B8A8_UNORM,
                               1);
    }

    void Dx12UploadMaterialources(RHI* rhi,
                                     RenderResource* resource,
                                     RenderEntity entity,
                                     GpuPBRMaterial& material)
    {
        if (rhi == nullptr || resource == nullptr || resource->m_MaterialDescriptorSetLayout == nullptr)
        {
            return;
        }

        MeshMaterialUniform ubo {};
        ubo.is_blend = entity.m_Blend;
        ubo.is_double_sided = entity.m_DoubleSided;
        ubo.baseColorFactor = entity.m_BaseColorFactor;
        ubo.metallicFactor = entity.m_MetallicFactor;
        ubo.roughnessFactor = entity.m_RoughnessFactor;
        ubo.normalScale = entity.m_NormalScale;
        ubo.occlusionStrength = entity.m_OcclusionStrength;
        ubo.emissiveFactor = entity.m_EmissiveFactor;

        Dx12CreateGpuBufferFromStaging(rhi,
                                       sizeof(MeshMaterialUniform),
                                       RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       &ubo,
                                       material.material_uniform_buffer);

        Dx12CreateWhiteTexture(rhi, material.base_color_texture_image, material.base_color_image_view);
        Dx12CreateWhiteTexture(rhi, material.metallic_roughness_texture_image, material.metallic_roughness_image_view);
        Dx12CreateWhiteTexture(rhi, material.normal_texture_image, material.normal_image_view);
        Dx12CreateWhiteTexture(rhi, material.occlusion_texture_image, material.occlusion_image_view);
        Dx12CreateWhiteTexture(rhi, material.emissive_texture_image, material.emissive_image_view);

        RHIDescriptorSetAllocateInfo alloc_info {};
        alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = rhi->GetDescriptorPoor();
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = resource->m_MaterialDescriptorSetLayout;

        if (rhi->AllocateDescriptorSets(&alloc_info, material.material_descriptor_set) != RHI_SUCCESS)
        {
            return;
        }

        RHIDescriptorBufferInfo ubo_info {};
        ubo_info.offset = 0;
        ubo_info.range = sizeof(MeshMaterialUniform);
        ubo_info.buffer = material.material_uniform_buffer;

        RHIDescriptorImageInfo base_color_info {};
        base_color_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        base_color_info.imageView = material.base_color_image_view;
        base_color_info.sampler = rhi->GetOrCreateMipmapSampler(1, 1);

        RHIDescriptorImageInfo metallic_info = base_color_info;
        metallic_info.imageView = material.metallic_roughness_image_view;
        RHIDescriptorImageInfo normal_info = base_color_info;
        normal_info.imageView = material.normal_image_view;
        RHIDescriptorImageInfo occlusion_info = base_color_info;
        occlusion_info.imageView = material.occlusion_image_view;
        RHIDescriptorImageInfo emissive_info = base_color_info;
        emissive_info.imageView = material.emissive_image_view;

        RHIWriteDescriptorSet writes[6] {};
        writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = material.material_descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &ubo_info;

        for (uint32_t i = 1; i < 6; ++i)
        {
            writes[i] = writes[0];
            writes[i].dstBinding = i;
            writes[i].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pBufferInfo = nullptr;
            writes[i].pImageInfo = nullptr;
        }
        writes[1].pImageInfo = &base_color_info;
        writes[2].pImageInfo = &metallic_info;
        writes[3].pImageInfo = &normal_info;
        writes[4].pImageInfo = &occlusion_info;
        writes[5].pImageInfo = &emissive_info;

        rhi->UpdateDescriptorSets(6, writes, 0, nullptr);
    }
}  // namespace

void RenderResource::UploadGpuMeshDx12(RHI* rhi, GpuMesh& now_mesh, RenderMeshData& mesh_data)
{
    now_mesh.backend = RenderResourceBackend::DirectX12;
    if (mesh_data.m_SkeletonBindingBuffer != nullptr)
    {
        LOG_WARNING(ZRender, "RenderResource(DX12): skinned mesh uses rigid upload path for now");
    }

    Dx12UploadMeshNonSkinned(rhi, this, now_mesh, mesh_data);
}

#if !defined(Z_HAS_VULKAN)

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
    if (m_GlobalRenderResource.m_StorageBuffer.m_GlobalUploadRingbuffer == nullptr)
    {
        CreateAndMapStorageBuffer(rhi);
    }

    m_MegaLights.Initialize(rhi);

    if (level_resource_desc.m_ColorGradingResourceDesc.m_ColorGradingMap.empty())
    {
        return;
    }

    std::shared_ptr<TextureData> color_grading_map =
        LoadTexture(level_resource_desc.m_ColorGradingResourceDesc.m_ColorGradingMap);
    if (color_grading_map == nullptr)
    {
        LOG_WARNING(ZRender,
                    "RenderResource(DX12): failed to load color grading LUT '{}'",
                    level_resource_desc.m_ColorGradingResourceDesc.m_ColorGradingMap.c_str());
        return;
    }

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
    if (render_scene == nullptr || camera == nullptr)
    {
        return;
    }

    Matrix4x4 view_matrix = camera->GetViewMatrix();
    Matrix4x4 proj_matrix = camera->GetPersProjMatrix();
    Vector3 camera_position = camera->position();
    Matrix4x4 proj_view_matrix = proj_matrix * view_matrix;

    ViewportType viewport_type =
        (camera->m_CurrentCameraType == RenderCameraType::Game) ? ViewportType::game : ViewportType::scene;
    const size_t viewport_index = static_cast<size_t>(viewport_type);

    auto& mesh_perframe_storage_buffer_object = m_MainCameraPerFrameByViewport[viewport_index];

    const Vector3 ambient_light = render_scene->m_AmbientLight.m_Irradiance;
    const uint32_t point_light_num = static_cast<uint32_t>(render_scene->m_PointLightList.m_Lights.size());

    mesh_perframe_storage_buffer_object.proj_view_matrix = proj_view_matrix;
    mesh_perframe_storage_buffer_object.camera_position = camera_position;
    mesh_perframe_storage_buffer_object.ambient_light = ambient_light;
    mesh_perframe_storage_buffer_object.point_light_num = point_light_num;
    mesh_perframe_storage_buffer_object.show_skybox = 1U;

    m_PointLightShadowPerFrame.point_light_num = point_light_num;
    for (uint32_t i = 0; i < point_light_num; ++i)
    {
        const Vector3 point_light_position = render_scene->m_PointLightList.m_Lights[i].m_Position;
        const Vector3 point_light_intensity =
            render_scene->m_PointLightList.m_Lights[i].m_Flux / (4.0f * Math_PI);
        const float radius = render_scene->m_PointLightList.m_Lights[i].calculateRadius();

        mesh_perframe_storage_buffer_object.scene_point_lights[i].position = point_light_position;
        mesh_perframe_storage_buffer_object.scene_point_lights[i].radius = radius;
        mesh_perframe_storage_buffer_object.scene_point_lights[i].intensity = point_light_intensity;

        m_PointLightShadowPerFrame.point_lights_position_and_radius[i] =
            Vector4(point_light_position, radius);
    }

    mesh_perframe_storage_buffer_object.scene_directional_light.direction =
        render_scene->m_DirectionalLight.m_Direction.normalisedCopy();
    mesh_perframe_storage_buffer_object.scene_directional_light.color = render_scene->m_DirectionalLight.m_Color;

    if (MegaLights::IsEnabled())
    {
        m_MegaLights.Update(*render_scene, camera, viewport_type);
        mesh_perframe_storage_buffer_object.point_light_num = 0;
    }

    m_MainCameraPerFrame = mesh_perframe_storage_buffer_object;
}

RenderMeshGPUResource& RenderResource::GetEntityMesh(RenderEntity entity)
{
    const size_t assetid = entity.m_MeshAssetId;
    auto it = m_Meshes.find(assetid);
    if (it != m_Meshes.end())
    {
        return it->second;
    }
    throw std::runtime_error("RenderResource::GetEntityMesh: mesh not uploaded");
}

RenderMaterialGPUResource& RenderResource::GetEntityMaterial(RenderEntity entity)
{
    const size_t assetid = entity.m_MaterialAssetId;
    auto it = m_Materials.find(assetid);
    if (it != m_Materials.end())
    {
        return it->second;
    }
    throw std::runtime_error("RenderResource::GetEntityMaterial: material not uploaded");
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

    StorageBuffer& storage_buffer = m_GlobalRenderResource.m_StorageBuffer;
    const uint32_t frames_in_flight = std::max<uint8_t>(rhi->GetMaxFramesInFlight(), 1);

    RHIPhysicalDeviceProperties properties {};
    rhi->GetPhysicalDeviceProperties(&properties);

    storage_buffer.m_MinUniformBufferOffsetAlignment =
        static_cast<uint32_t>(properties.limits.minUniformBufferOffsetAlignment != 0 ? properties.limits.minUniformBufferOffsetAlignment : 256);
    storage_buffer.m_MinStorageBufferOffsetAlignment =
        static_cast<uint32_t>(properties.limits.minStorageBufferOffsetAlignment != 0 ? properties.limits.minStorageBufferOffsetAlignment : 256);
    storage_buffer.m_MaxStorageBufferRange = properties.limits.maxStorageBufferRange != 0 ? properties.limits.maxStorageBufferRange : (1u << 27);
    storage_buffer.m_NonCoherentAtomSize =
        properties.limits.nonCoherentAtomSize != 0 ? properties.limits.nonCoherentAtomSize : 256;

    const uint32_t global_storage_buffer_size = 1024u * 1024u * 128u;
    rhi->CreateBuffer(global_storage_buffer_size,
                      RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      storage_buffer.m_GlobalUploadRingbuffer,
                      storage_buffer.m_GlobalUploadRingbufferMemory);

    storage_buffer.m_GlobalUploadRingbuffersBegin.resize(frames_in_flight);
    storage_buffer.m_GlobalUploadRingbuffersEnd.resize(frames_in_flight);
    storage_buffer.m_GlobalUploadRingbuffersSize.resize(frames_in_flight);
    for (uint32_t i = 0; i < frames_in_flight; ++i)
    {
        storage_buffer.m_GlobalUploadRingbuffersBegin[i] = (global_storage_buffer_size * i) / frames_in_flight;
        storage_buffer.m_GlobalUploadRingbuffersSize[i] = (global_storage_buffer_size * (i + 1)) / frames_in_flight -
                                                          (global_storage_buffer_size * i) / frames_in_flight;
        storage_buffer.m_GlobalUploadRingbuffersEnd[i] = storage_buffer.m_GlobalUploadRingbuffersBegin[i];
    }

    rhi->CreateBuffer(sizeof(AxisDrawStorage),
                      RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      storage_buffer.m_AxisInefficientStorageBuffer,
                      storage_buffer.m_AxisInefficientStorageBufferMemory);

    rhi->CreateBuffer(64,
                      RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      storage_buffer.m_GlobalNullDescriptorStorageBuffer,
                      storage_buffer.m_GlobalNullDescriptorStorageBufferMemory);

    rhi->MapMemory(storage_buffer.m_GlobalUploadRingbufferMemory,
                   0,
                   RHI_WHOLE_SIZE,
                   0,
                   &storage_buffer.m_GlobalUploadRingbufferMemoryPointer);

    rhi->MapMemory(storage_buffer.m_AxisInefficientStorageBufferMemory,
                   0,
                   RHI_WHOLE_SIZE,
                   0,
                   &storage_buffer.m_AxisInefficientStorageBufferMemoryPointer);
}

GpuMesh& RenderResource::GetOrCreateMesh(RHI* rhi,
                                                  RenderEntity entity,
                                                  RenderMeshData mesh_data)
{
    const size_t assetid = entity.m_MeshAssetId;
    auto it = m_Meshes.find(assetid);
    if (it != m_Meshes.end() && meshDrawDataIsValid(&it->second))
    {
        return it->second;
    }
    if (it != m_Meshes.end())
    {
        LOG_WARNING(ZRender, "RenderResource(DX12): re-uploading mesh asset id {} (GPU buffers were invalid)", assetid);
        m_Meshes.erase(it);
    }

    auto res = m_Meshes.insert(std::make_pair(assetid, GpuMesh()));
    assert(res.second);
    GpuMesh& now_mesh = res.first->second;
    now_mesh.backend = RenderResourceBackend::DirectX12;

    if (mesh_data.m_SkeletonBindingBuffer != nullptr)
    {
        LOG_WARNING(ZRender, "RenderResource(DX12): skinned mesh uses rigid upload path for now");
    }

    UploadGpuMeshDx12(rhi, now_mesh, mesh_data);
    return now_mesh;
}

GpuPBRMaterial& RenderResource::GetOrCreateMaterial(RHI* rhi,
                                                             RenderEntity entity,
                                                             RenderMaterialData material_data)
{
    (void)rhi;
    const size_t assetid = entity.m_MaterialAssetId;
    auto it = m_Materials.find(assetid);
    if (it != m_Materials.end())
    {
        if (entity.m_DoubleSided)
        {
            it->second.cull = "None";
        }
        return it->second;
    }

    auto res = m_Materials.insert(std::make_pair(assetid, GpuPBRMaterial()));
    assert(res.second);
    GpuPBRMaterial& material = res.first->second;

    material.shader_name = material_data.m_Shader.c_str();
    material.shader_asset_file = material_data.m_ShaderAssetFile.c_str();
    material.vertex_shader_file = material_data.m_VertexShaderFile.c_str();
    material.fragment_shader_file = material_data.m_FragmentShaderFile.c_str();
    material.render_pipeline = material_data.m_RenderPipeline.c_str();
    material.light_mode = material_data.m_LightMode.c_str();
    material.source_language = material_data.m_SourceLanguage.c_str();
    material.vertex_entry = material_data.m_VertexEntry.c_str();
    material.fragment_entry = material_data.m_FragmentEntry.c_str();
    material.include_directory = material_data.m_IncludeDirectory.c_str();
    material.cull = material_data.m_Cull.c_str();
    material.ztest = material_data.m_Ztest.c_str();
    material.blend = material_data.m_Blend.c_str();
    material.zwrite = material_data.m_Zwrite;
    if (entity.m_DoubleSided)
    {
        material.cull = "None";
    }
    material.shader_macros.clear();
    for (const eastl::string& keyword : material_data.m_EnabledShaderKeywords)
    {
        if (!keyword.empty())
        {
            material.shader_macros[keyword.c_str()] = "1";
        }
    }
    material.enable_dx12 = material_data.m_EnableDx12;
    material.enable_vulkan = material_data.m_EnableVulkan;
    material.enable_metal = material_data.m_EnableMetal;
    material.shader_passes.clear();
    material.shader_passes.reserve(material_data.m_ShaderPasses.size());
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
        material.shader_passes.emplace_back(std::move(runtime_shader_pass));
    }

    Dx12UploadMaterialources(rhi, this, entity, material);
    return material;
}

#endif  // !Z_HAS_VULKAN

#endif  // _WIN32
