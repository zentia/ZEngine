// DX12 / non-Vulkan RenderResource implementations (mesh upload, global ring buffer, per-frame UBOs).

#include "Runtime/Function/Render/RenderResource.h"

#if defined(_WIN32)

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Core/Math/LargeWorldCoordinates.h"
    #include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
    #include "Runtime/Function/Render/RenderCamera.h"
    #include "Runtime/Function/Render/RenderMesh.h"
    #include "Runtime/Function/Render/RenderScene.h"
    #include "Runtime/Function/Render/RenderSystem.h"
    #include "Runtime/Function/Render/Interface/RHI.h"
    #include "Runtime/Utility/Utility.h"

    #include <algorithm>
    #include <array>
    #include <cassert>
    #include <cmath>
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

        rhi->CopyBufferImmediate(staging_buffer, out_buffer, 0, 0, size);
        rhi->DestroyBuffer(staging_buffer);
        rhi->FreeMemory(staging_memory);
        if (gpu_memory != nullptr)
        {
            rhi->FreeMemory(gpu_memory);
        }
    }

    /// 把 `src_data` 写进一个**已经存在**的 GPU buffer。材质实例的 UBO 要能被脚本
    /// 反复改（受击闪白可能每帧写一次），每帧重建 buffer 会一直漏显存，所以这里只
    /// 做 staging -> 既有 buffer 的拷贝。
    void Dx12UpdateGpuBufferFromStaging(RHI* rhi,
                                        RHIDeviceSize size,
                                        const void* src_data,
                                        RHIBuffer* dst_buffer)
    {
        if (rhi == nullptr || size == 0 || dst_buffer == nullptr || src_data == nullptr)
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
        if (mapped != nullptr)
        {
            std::memcpy(mapped, src_data, static_cast<size_t>(size));
        }
        rhi->UnmapMemory(staging_memory);

        rhi->CopyBufferImmediate(staging_buffer, dst_buffer, 0, 0, size);
        rhi->DestroyBuffer(staging_buffer);
        rhi->FreeMemory(staging_memory);
    }

    /// Uploads the three vertex streams (position / normal+tangent / texcoord)
    /// plus the index buffer. Shared by the rigid and the skinned path.
    bool Dx12UploadMeshVertexStreams(RHI* rhi, GpuMesh& now_mesh, RenderMeshData& mesh_data)
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
            return false;
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
        rhi->CopyBufferImmediate(staging_buffer, now_mesh.mesh_vertex_position_buffer, 0, 0, position_buffer_size);

        RHIDeviceMemory* blending_memory = nullptr;
        rhi->CreateBuffer(varying_blending_size,
                          RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          now_mesh.mesh_vertex_varying_enable_blending_buffer,
                          blending_memory);
        rhi->CopyBufferImmediate(
            staging_buffer, now_mesh.mesh_vertex_varying_enable_blending_buffer, position_buffer_size, 0, varying_blending_size);

        RHIDeviceMemory* varying_memory = nullptr;
        rhi->CreateBuffer(varying_size,
                          RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          now_mesh.mesh_vertex_varying_buffer,
                          varying_memory);
        rhi->CopyBufferImmediate(staging_buffer,
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
        now_mesh.mesh_vertex_count = vertex_count;
        now_mesh.mesh_index_count = index_buffer_size / sizeof(uint16_t);
        Dx12CreateGpuBufferFromStaging(rhi,
                                       vertex_buffer_size,
                                       RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       vertex_buffer_data,
                                       now_mesh.mesh_vertex_interleaved_buffer);
        Dx12CreateGpuBufferFromStaging(rhi,
                                       index_buffer_size,
                                       RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       index_buffer_data,
                                       now_mesh.mesh_index_buffer);

        return true;
    }

    void Dx12UploadMeshNonSkinned(RHI* rhi, RenderResource* resource, GpuMesh& now_mesh, RenderMeshData& mesh_data)
    {
        if (!Dx12UploadMeshVertexStreams(rhi, now_mesh, mesh_data))
        {
            return;
        }

        now_mesh.enable_vertex_blending = false;

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

    void Dx12UploadMeshSkinned(RHI* rhi, RenderResource* resource, GpuMesh& now_mesh, RenderMeshData& mesh_data)
    {
        if (!Dx12UploadMeshVertexStreams(rhi, now_mesh, mesh_data))
        {
            return;
        }

        now_mesh.enable_vertex_blending = true;

        const BufferData* binding_data = mesh_data.m_SkeletonBindingBuffer.get();
        if (binding_data == nullptr || binding_data->m_Data == nullptr || binding_data->m_Size == 0)
        {
            // A skinned mesh with no skin weights would collapse every vertex
            // into the origin, so flag it loudly and fall back to the rigid
            // descriptor (which the shader treats as "no influences").
            LOG_WARNING(ZRender, "RenderResource(DX12): skinned mesh has an empty joint-binding buffer");
            now_mesh.enable_vertex_blending = false;
            return;
        }

        if (binding_data->m_Size % sizeof(MeshVertex::JointBinding) != 0)
        {
            LOG_ERROR(ZRender,
                      "RenderResource(DX12): joint-binding buffer size {} is not a multiple of {}",
                      static_cast<uint64_t>(binding_data->m_Size),
                      static_cast<uint32_t>(sizeof(MeshVertex::JointBinding)));
            now_mesh.enable_vertex_blending = false;
            return;
        }

        Dx12CreateGpuBufferFromStaging(rhi,
                                       binding_data->m_Size,
                                       RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       binding_data->m_Data,
                                       now_mesh.mesh_vertex_joint_binding_buffer);

        if (now_mesh.mesh_vertex_joint_binding_buffer == nullptr)
        {
            LOG_ERROR(ZRender, "RenderResource(DX12): failed to create the joint-binding buffer");
            now_mesh.enable_vertex_blending = false;
            return;
        }
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
                buffer_info.range = binding_data->m_Size;
                buffer_info.buffer = now_mesh.mesh_vertex_joint_binding_buffer;
                // Explicit stride: MeshVertex::JointBinding is 32 bytes, so the
                // backend's 80-byte heuristic would build a mis-sized SRV for any
                // vertex count that is a multiple of 5.
                buffer_info.stride = static_cast<uint32_t>(sizeof(MeshVertex::JointBinding));

                RHIWriteDescriptorSet write {};
                write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = now_mesh.mesh_vertex_blending_descriptor_set;
                write.dstBinding = 0;
                write.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.descriptorCount = 1;
                write.pBufferInfo = &buffer_info;
                rhi->UpdateDescriptorSets(1, &write, 0, nullptr);
            }
            else
            {
                LOG_ERROR(ZRender, "RenderResource(DX12): failed to allocate the joint-binding descriptor set");
                now_mesh.enable_vertex_blending = false;
            }
        }

        LOG_INFO(ZRender,
                 "RenderResource(DX12): skinned mesh uploaded ({} vertices, {} bytes of joint bindings)",
                 now_mesh.mesh_vertex_count,
                 static_cast<uint64_t>(binding_data->m_Size));
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
                                     GpuPBRMaterial& material,
                                     const RenderMaterialData& material_data)
    {
        if (rhi == nullptr || resource == nullptr || resource->m_MaterialDescriptorSetLayout == nullptr)
        {
            return;
        }

        // 与 Vulkan 侧同一条规则：有 ShaderLab 具名属性就用反射出来的布局，没有就
        // 用 MeshMaterialUniform 那套固定布局。后端不同不能导致 UBO 解释不同。
        BuildShaderPropertySlots(material_data.m_Properties, material.property_slots, material.property_defaults);
        material.shaderlab_property_layout = !material.property_slots.empty();

        MeshMaterialUniform ubo {};
        FillMaterialUniformBytes(material, entity, reinterpret_cast<uint8_t*>(&ubo));

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
        material.base_color_sampler = base_color_info.sampler;

        RHIDescriptorImageInfo metallic_info = base_color_info;
        metallic_info.imageView = material.metallic_roughness_image_view;
        material.metallic_roughness_sampler = metallic_info.sampler;
        RHIDescriptorImageInfo normal_info = base_color_info;
        normal_info.imageView = material.normal_image_view;
        material.normal_sampler = normal_info.sampler;
        RHIDescriptorImageInfo occlusion_info = base_color_info;
        occlusion_info.imageView = material.occlusion_image_view;
        material.occlusion_sampler = occlusion_info.sampler;
        RHIDescriptorImageInfo emissive_info = base_color_info;
        emissive_info.imageView = material.emissive_image_view;
        material.emissive_sampler = emissive_info.sampler;

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

    // PR-SK1: DX12 finally has a real skinned path (three vertex streams +
    // a per-vertex joint-binding SRV + the per-drawcall joint palette). The
    // shader skins in mesh.vert.hlsl; rigid meshes keep the old single path.
    if (mesh_data.m_SkeletonBindingBuffer != nullptr)
    {
        Dx12UploadMeshSkinned(rhi, this, now_mesh, mesh_data);
    }
    else
    {
        Dx12UploadMeshNonSkinned(rhi, this, now_mesh, mesh_data);
    }
}

#if !defined(Z_HAS_VULKAN)

void RenderResource::clear()
{
    ClearMeshGeometryCache();

    // 运行时材质实例跟着共享材质一起失效（理由同 RenderResource.cpp 那份 clear）。
    m_MaterialInstances.clear();

    if (m_TextureStreamingManager)
    {
        m_TextureStreamingManager->Shutdown();
        m_TextureStreamingManager.reset();
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

    RHISamplerCreateInfo sampler_info {};
    sampler_info.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = RHI_FILTER_LINEAR;
    sampler_info.minFilter = RHI_FILTER_LINEAR;
    sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = RHI_TRUE;
    sampler_info.maxAnisotropy = physical_device_properties.limits.maxSamplerAnisotropy;
    sampler_info.borderColor = RHI_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = RHI_FALSE;
    sampler_info.compareEnable = RHI_FALSE;
    sampler_info.compareOp = RHI_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod = 0.0f;

    if (m_GlobalRenderResource.m_IblResource.m_BrdflutTextureSampler != RHI_NULL_HANDLE)
    {
        rhi->DestroySampler(m_GlobalRenderResource.m_IblResource.m_BrdflutTextureSampler);
    }

    if (rhi->CreateSampler(&sampler_info, m_GlobalRenderResource.m_IblResource.m_BrdflutTextureSampler) !=
        RHI_SUCCESS)
    {
        throw std::runtime_error("RenderResource::CreateIBLSamplers: BRDF sampler create failed");
    }

    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 8.0f;
    sampler_info.mipLodBias = 0.0f;

    if (m_GlobalRenderResource.m_IblResource.m_IrradianceTextureSampler != RHI_NULL_HANDLE)
    {
        rhi->DestroySampler(m_GlobalRenderResource.m_IblResource.m_IrradianceTextureSampler);
    }

    if (rhi->CreateSampler(&sampler_info, m_GlobalRenderResource.m_IblResource.m_IrradianceTextureSampler) !=
        RHI_SUCCESS)
    {
        throw std::runtime_error("RenderResource::CreateIBLSamplers: irradiance sampler create failed");
    }

    if (m_GlobalRenderResource.m_IblResource.m_SpecularTextureSampler != RHI_NULL_HANDLE)
    {
        rhi->DestroySampler(m_GlobalRenderResource.m_IblResource.m_SpecularTextureSampler);
    }

    if (rhi->CreateSampler(&sampler_info, m_GlobalRenderResource.m_IblResource.m_SpecularTextureSampler) !=
        RHI_SUCCESS)
    {
        throw std::runtime_error("RenderResource::CreateIBLSamplers: specular sampler create failed");
    }
}

void RenderResource::CreateIBLTextures(RHI* rhi,
                                       std::array<std::shared_ptr<TextureData>, 6> irradiance_maps,
                                       std::array<std::shared_ptr<TextureData>, 6> specular_maps)
{
    uint32_t irradiance_cubemap_miplevels =
        static_cast<uint32_t>(std::floor(log2(std::max(irradiance_maps[0]->m_Width, irradiance_maps[0]->m_Height)))) +
        1;
    uint32_t specular_cubemap_miplevels =
        static_cast<uint32_t>(std::floor(log2(std::max(specular_maps[0]->m_Width, specular_maps[0]->m_Height)))) + 1;

    // Mip0-only on DX12: full GPU mipgen during startup has removed the device in the past.
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

namespace
{
    bool AllCubemapFacesLoaded(const std::array<std::shared_ptr<TextureData>, 6>& maps)
    {
        for (const std::shared_ptr<TextureData>& map : maps)
        {
            if (map == nullptr || map->m_Pixels == nullptr || map->m_Width == 0 || map->m_Height == 0)
            {
                return false;
            }
        }
        return true;
    }
}  // namespace

void RenderResource::UploadGlobalRenderResource(RHI* rhi, LevelResourceDesc level_resource_desc)
{
    if (rhi == nullptr)
    {
        return;
    }

    if (m_GlobalRenderResource.m_StorageBuffer.m_GlobalUploadRingbuffer == nullptr)
    {
        CreateAndMapStorageBuffer(rhi);
    }

    m_MegaLights.Initialize(rhi);

    const SkyBoxIrradianceMap& skybox_irradiance_map = level_resource_desc.m_IblResourceDesc.m_SkyboxIrradianceMap;
    const SkyBoxSpecularMap& skybox_specular_map = level_resource_desc.m_IblResourceDesc.m_SkyboxSpecularMap;

    std::array<std::shared_ptr<TextureData>, 6> irradiance_maps = {
        LoadTextureHDR(skybox_irradiance_map.m_PositiveXMap),
        LoadTextureHDR(skybox_irradiance_map.m_NegativeXMap),
        LoadTextureHDR(skybox_irradiance_map.m_PositiveZMap),
        LoadTextureHDR(skybox_irradiance_map.m_NegativeZMap),
        LoadTextureHDR(skybox_irradiance_map.m_PositiveYMap),
        LoadTextureHDR(skybox_irradiance_map.m_NegativeYMap)};

    std::array<std::shared_ptr<TextureData>, 6> specular_maps = {
        LoadTextureHDR(skybox_specular_map.m_PositiveXMap),
        LoadTextureHDR(skybox_specular_map.m_NegativeXMap),
        LoadTextureHDR(skybox_specular_map.m_PositiveZMap),
        LoadTextureHDR(skybox_specular_map.m_NegativeZMap),
        LoadTextureHDR(skybox_specular_map.m_PositiveYMap),
        LoadTextureHDR(skybox_specular_map.m_NegativeYMap)};

    std::shared_ptr<TextureData> brdf_map = LoadTextureHDR(level_resource_desc.m_IblResourceDesc.m_BrdfMap);

    if (AllCubemapFacesLoaded(irradiance_maps) && AllCubemapFacesLoaded(specular_maps) && brdf_map != nullptr &&
        brdf_map->m_Pixels != nullptr)
    {
        try
        {
            CreateIBLSamplers(rhi);

            // Diagnostic escape hatch: set ZENGINE_DISABLE_IBL_CUBEMAP=1 to skip the
            // cubemap upload (BRDF LUT still uploads).  Used to A/B whether the IBL
            // cubemap upload is what removes the D3D12 device during bring-up.
            const char* disable_cubemap_env = std::getenv("ZENGINE_DISABLE_IBL_CUBEMAP");
            const bool disable_cubemap =
                disable_cubemap_env != nullptr && std::string(disable_cubemap_env) != "0";
            if (disable_cubemap)
            {
                LOG_WARNING(ZRender,
                            "RenderResource(DX12): ZENGINE_DISABLE_IBL_CUBEMAP set — skipping IBL cubemap upload");
            }
            else
            {
                CreateIBLTextures(rhi, irradiance_maps, specular_maps);
            }

            rhi->CreateGlobalImage(m_GlobalRenderResource.m_IblResource.m_BrdflutTextureImage,
                                   m_GlobalRenderResource.m_IblResource.m_BrdflutTextureImageView,
                                   m_GlobalRenderResource.m_IblResource.m_BrdflutTextureImageAllocation,
                                   brdf_map->m_Width,
                                   brdf_map->m_Height,
                                   brdf_map->m_Pixels,
                                   brdf_map->m_Format);
            LOG_INFO(ZRender, "RenderResource(DX12): IBL HDR upload complete (mip0 cubemaps + BRDF LUT)");
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR(ZRender, "RenderResource(DX12): IBL HDR upload failed: {}", ex.what());
        }
    }
    else
    {
        LOG_WARNING(ZRender, "RenderResource(DX12): skipping IBL upload (HDR face or BRDF load failed)");
    }

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
    // 运行时材质实例优先。拿不到就回退到共享资产材质 —— 绝不能在这里抛：这条路径
    // 在可见性剔除里每帧跑，抛一次整个帧就崩了。回退的代价只是"override 不生效"，
    // 与加这个功能之前的行为一致。
    if (entity.m_MaterialInstanceId != 0)
    {
        auto instance_it = m_MaterialInstances.find(entity.m_MaterialInstanceId);
        if (instance_it != m_MaterialInstances.end())
        {
            return instance_it->second;
        }
    }

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
        storage_buffer.m_GlobalUploadRingbuffersBegin[i] =
            RoundUp((global_storage_buffer_size * i) / frames_in_flight, 256u);
        storage_buffer.m_GlobalUploadRingbuffersSize[i] = (global_storage_buffer_size * (i + 1)) / frames_in_flight -
                                                          storage_buffer.m_GlobalUploadRingbuffersBegin[i];
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

    // Keep a CPU copy of the geometry while we still have it: after this function
    // returns, mesh_data is gone (BufferData frees its malloc) and nothing can read
    // vertex positions back from the GPU buffers.
    CacheMeshGeometry(assetid, mesh_data, entity.m_BoundingBox);

    auto it = m_Meshes.find(assetid);
    if (it != m_Meshes.end() && meshDrawDataIsValid(&it->second) &&
        it->second.mesh_vertex_interleaved_buffer != nullptr)
    {
        return it->second;
    }
    if (it != m_Meshes.end())
    {
        LOG_WARNING(ZRender,
                    "RenderResource(DX12): re-uploading mesh asset id {} (GPU buffers were invalid or stale)",
                    assetid);
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

    Dx12UploadMaterialources(rhi, this, entity, material, material_data);
    return material;
}

void RenderResource::MaterializeMaterialInstance(RHI* rhi, RenderEntity entity, GpuPBRMaterial& instance)
{
    if (rhi == nullptr || m_MaterialDescriptorSetLayout == nullptr)
    {
        return;
    }

    // ---- 1. uniform buffer：没有就建，有就只重写（重建 = 漏显存） ----
    // 共享材质的默认值 + 这个 part 的 override，与 Vulkan 侧同一个 helper。
    MeshMaterialUniform ubo {};
    FillMaterialUniformBytes(instance, entity, reinterpret_cast<uint8_t*>(&ubo));

    const RHIDeviceSize buffer_size = sizeof(MeshMaterialUniform);

    if (instance.material_uniform_buffer == RHI_NULL_HANDLE)
    {
        Dx12CreateGpuBufferFromStaging(rhi,
                                       buffer_size,
                                       RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       &ubo,
                                       instance.material_uniform_buffer);
    }
    else
    {
        Dx12UpdateGpuBufferFromStaging(rhi, buffer_size, &ubo, instance.material_uniform_buffer);
    }

    if (instance.material_uniform_buffer == RHI_NULL_HANDLE)
    {
        return;
    }

    // ---- 2. 描述符集：没有就分配，有就复用 ----
    if (instance.material_descriptor_set == RHI_NULL_HANDLE)
    {
        RHIDescriptorSetAllocateInfo alloc_info {};
        alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = rhi->GetDescriptorPoor();
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = m_MaterialDescriptorSetLayout;

        if (rhi->AllocateDescriptorSets(&alloc_info, instance.material_descriptor_set) != RHI_SUCCESS)
        {
            instance.material_descriptor_set = RHI_NULL_HANDLE;
            return;
        }
    }

    // 6 个绑定**每次全量重写**：instance id 被回收后可能换了一个 part，纹理视图也
    // 就换了；只写 UBO 会把上一任的纹理留在描述符集里。
    //
    // 纹理（DX12 下目前是 5 张 1x1 白图）和采样器都从共享材质那里共享 —— 实例只有
    // UBO 和描述符集是自己的。
    RHIDescriptorBufferInfo ubo_info {};
    ubo_info.offset = 0;
    ubo_info.range = buffer_size;
    ubo_info.buffer = instance.material_uniform_buffer;

    RHIDescriptorImageInfo image_info {};
    image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.sampler = rhi->GetOrCreateMipmapSampler(1, 1);

    RHIDescriptorImageInfo image_infos[5] {};
    RHIImageView* views[5] = {instance.base_color_image_view,
                              instance.metallic_roughness_image_view,
                              instance.normal_image_view,
                              instance.occlusion_image_view,
                              instance.emissive_image_view};
    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        image_infos[slot] = image_info;
        image_infos[slot].imageView = views[slot];
    }

    RHIWriteDescriptorSet writes[6] {};
    writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = instance.material_descriptor_set;
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
        writes[i].pImageInfo = &image_infos[i - 1];
    }

    rhi->UpdateDescriptorSets(6, writes, 0, nullptr);
}

#endif  // !Z_HAS_VULKAN

#endif  // _WIN32
