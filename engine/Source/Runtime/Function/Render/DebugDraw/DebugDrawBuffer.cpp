#include "DebugDrawBuffer.h"

#include "Runtime/Function/Render/RenderSystem.h"

#include <stdexcept>

void DebugDrawAllocator::Initialize(DebugDrawFont* font)
{
    m_Rhi = GET_SYSTEM(RHI);
    m_Font = font;
    SetupDescriptorSet();
}
void DebugDrawAllocator::Destory()
{
    clear();
    UnloadMeshBuffer();
}

void DebugDrawAllocator::Tick()
{
    FlushPendingDelete();
    m_CurrentFrame = (m_CurrentFrame + 1) % k_deferred_delete_resource_frame_count;
}

RHIBuffer* DebugDrawAllocator::GetVertexBuffer()
{
    return m_VertexResource.buffer;
}
RHIDescriptorSet*& DebugDrawAllocator::GetDescriptorSet()
{
    return m_Descriptor.descriptor_set[m_Rhi->GetCurrentFrameIndex()];
}

size_t DebugDrawAllocator::CacheVertexs(const std::vector<DebugDrawVertex>& vertexs)
{
    size_t offset = m_VertexCache.size();
    m_VertexCache.resize(offset + vertexs.size());
    for (size_t i = 0; i < vertexs.size(); i++)
    {
        m_VertexCache[i + offset] = vertexs[i];
    }
    return offset;
}
void DebugDrawAllocator::CacheUniformObject(Matrix4x4 proj_view_matrix)
{
    m_UniformBufferObject.proj_view_matrix = proj_view_matrix;
}
size_t DebugDrawAllocator::CacheUniformDynamicObject(const std::vector<std::pair<Matrix4x4, Vector4>>& model_colors)
{
    size_t offset = m_UniformBufferDynamicObjectCache.size();
    m_UniformBufferDynamicObjectCache.resize(offset + model_colors.size());
    for (size_t i = 0; i < model_colors.size(); i++)
    {
        m_UniformBufferDynamicObjectCache[i + offset].model_matrix = model_colors[i].first;
        m_UniformBufferDynamicObjectCache[i + offset].color = model_colors[i].second;
    }
    return offset;
}

size_t DebugDrawAllocator::GetVertexCacheOffset() const
{
    return m_VertexCache.size();
}
size_t DebugDrawAllocator::GetUniformDynamicCacheOffset() const
{
    return m_UniformBufferDynamicObjectCache.size();
}

void DebugDrawAllocator::Allocator()
{
    ClearBuffer();
    uint64_t vertex_bufferSize = static_cast<uint64_t>(m_VertexCache.size() * sizeof(DebugDrawVertex));
    if (vertex_bufferSize > 0)
    {
        m_Rhi->CreateBuffer(vertex_bufferSize,
                            RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            m_VertexResource.buffer,
                            m_VertexResource.memory);

        void* data;
        m_Rhi->MapMemory(m_VertexResource.memory, 0, vertex_bufferSize, 0, &data);
        memcpy(data, m_VertexCache.data(), vertex_bufferSize);
        m_Rhi->UnmapMemory(m_VertexResource.memory);
    }

    uint64_t uniform_BufferSize = static_cast<uint64_t>(sizeof(UniformBufferObject));
    if (uniform_BufferSize > 0)
    {
        m_Rhi->CreateBuffer(uniform_BufferSize,
                            RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            m_UniformResource.buffer,
                            m_UniformResource.memory);

        void* data;
        m_Rhi->MapMemory(m_UniformResource.memory, 0, uniform_BufferSize, 0, &data);
        memcpy(data, &m_UniformBufferObject.proj_view_matrix, uniform_BufferSize);
        m_Rhi->UnmapMemory(m_UniformResource.memory);
    }

    uint64_t uniform_dynamic_BufferSize =
        static_cast<uint64_t>(sizeof(UniformBufferDynamicObject) * m_UniformBufferDynamicObjectCache.size());
    if (uniform_dynamic_BufferSize > 0)
    {
        m_Rhi->CreateBuffer(uniform_dynamic_BufferSize,
                            RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            m_UniformDynamicResource.buffer,
                            m_UniformDynamicResource.memory);

        void* data;
        m_Rhi->MapMemory(m_UniformDynamicResource.memory, 0, uniform_dynamic_BufferSize, 0, &data);
        memcpy(data, m_UniformBufferDynamicObjectCache.data(), uniform_dynamic_BufferSize);
        m_Rhi->UnmapMemory(m_UniformDynamicResource.memory);
    }

    UpdateDescriptorSet();
}

void DebugDrawAllocator::clear()
{
    ClearBuffer();
    m_VertexCache.clear();
    m_UniformBufferObject.proj_view_matrix = Matrix4x4::IDENTITY;
    m_UniformBufferDynamicObjectCache.clear();
}

void DebugDrawAllocator::ClearBuffer()
{
    if (m_VertexResource.buffer)
    {
        m_DefferDeleteQueue[m_CurrentFrame].push(m_VertexResource);
        m_VertexResource.buffer = nullptr;
        m_VertexResource.memory = nullptr;
    }
    if (m_UniformResource.buffer)
    {
        m_DefferDeleteQueue[m_CurrentFrame].push(m_UniformResource);
        m_UniformResource.buffer = nullptr;
        m_UniformResource.memory = nullptr;
    }
    if (m_UniformDynamicResource.buffer)
    {
        m_DefferDeleteQueue[m_CurrentFrame].push(m_UniformDynamicResource);
        m_UniformDynamicResource.buffer = nullptr;
        m_UniformDynamicResource.memory = nullptr;
    }
}

void DebugDrawAllocator::FlushPendingDelete()
{
    uint32_t current_frame_to_delete = (m_CurrentFrame + 1) % k_deferred_delete_resource_frame_count;
    while (!m_DefferDeleteQueue[current_frame_to_delete].empty())
    {
        Resource resource_to_delete = m_DefferDeleteQueue[current_frame_to_delete].front();
        m_DefferDeleteQueue[current_frame_to_delete].pop();
        if (resource_to_delete.buffer == nullptr)
            continue;
        m_Rhi->FreeMemory(resource_to_delete.memory);
        m_Rhi->DestroyBuffer(resource_to_delete.buffer);
    }
}

void DebugDrawAllocator::SetupDescriptorSet()
{
    RHIDescriptorSetLayoutBinding uboLayoutBinding[3];
    uboLayoutBinding[0].binding = 0;
    uboLayoutBinding[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding[0].descriptorCount = 1;
    uboLayoutBinding[0].stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding[0].pImmutableSamplers = nullptr;

    uboLayoutBinding[1].binding = 1;
    uboLayoutBinding[1].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uboLayoutBinding[1].descriptorCount = 1;
    uboLayoutBinding[1].stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding[1].pImmutableSamplers = nullptr;

    uboLayoutBinding[2].binding = 2;
    uboLayoutBinding[2].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    uboLayoutBinding[2].descriptorCount = 1;
    uboLayoutBinding[2].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
    uboLayoutBinding[2].pImmutableSamplers = nullptr;

    RHIDescriptorSetLayoutCreateInfo layoutInfo {};
    layoutInfo.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = uboLayoutBinding;

    if (m_Rhi->CreateDescriptorSetLayout(&layoutInfo, m_Descriptor.layout) != RHI_SUCCESS)
    {
        throw std::runtime_error("create debug draw layout");
    }

    m_Descriptor.descriptor_set.resize(m_Rhi->GetMaxFramesInFlight());
    for (size_t i = 0; i < m_Rhi->GetMaxFramesInFlight(); i++)
    {
        RHIDescriptorSetAllocateInfo allocInfo {};
        allocInfo.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.pNext = NULL;
        allocInfo.descriptorPool = m_Rhi->GetDescriptorPoor();
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_Descriptor.layout;

        if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&allocInfo, m_Descriptor.descriptor_set[i]))
        {
            throw std::runtime_error("debug draw descriptor set");
        }
    }

    PrepareDescriptorSet();
}

// prepare at the start tick
void DebugDrawAllocator::PrepareDescriptorSet()
{
    RHIDescriptorImageInfo image_info[1];
    image_info[0].imageView = m_Font->GetImageView();
    image_info[0].sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
    image_info[0].imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    for (size_t i = 0; i < m_Rhi->GetMaxFramesInFlight(); i++)
    {
        RHIWriteDescriptorSet descriptor_write[1];
        descriptor_write[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_write[0].dstSet = m_Descriptor.descriptor_set[i];
        descriptor_write[0].dstBinding = 2;
        descriptor_write[0].dstArrayElement = 0;
        descriptor_write[0].pNext = nullptr;
        descriptor_write[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_write[0].descriptorCount = 1;
        descriptor_write[0].pBufferInfo = nullptr;
        descriptor_write[0].pImageInfo = &image_info[0];
        descriptor_write[0].pTexelBufferView = nullptr;

        m_Rhi->UpdateDescriptorSets(1, descriptor_write, 0, nullptr);
    }
}

// update every tick
void DebugDrawAllocator::UpdateDescriptorSet()
{
    RHIDescriptorBufferInfo buffer_info[2];
    buffer_info[0].buffer = m_UniformResource.buffer;
    buffer_info[0].offset = 0;
    buffer_info[0].range = sizeof(UniformBufferObject);

    buffer_info[1].buffer = m_UniformDynamicResource.buffer;
    buffer_info[1].offset = 0;
    buffer_info[1].range = sizeof(UniformBufferDynamicObject);

    RHIWriteDescriptorSet descriptor_write[2];
    descriptor_write[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write[0].dstSet = m_Descriptor.descriptor_set[m_Rhi->GetCurrentFrameIndex()];
    descriptor_write[0].dstBinding = 0;
    descriptor_write[0].dstArrayElement = 0;
    descriptor_write[0].pNext = nullptr;
    descriptor_write[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_write[0].descriptorCount = 1;
    descriptor_write[0].pBufferInfo = &buffer_info[0];
    descriptor_write[0].pImageInfo = nullptr;
    descriptor_write[0].pTexelBufferView = nullptr;

    descriptor_write[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write[1].dstSet = m_Descriptor.descriptor_set[m_Rhi->GetCurrentFrameIndex()];
    descriptor_write[1].dstBinding = 1;
    descriptor_write[1].dstArrayElement = 0;
    descriptor_write[1].pNext = nullptr;
    descriptor_write[1].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptor_write[1].descriptorCount = 1;
    descriptor_write[1].pBufferInfo = &buffer_info[1];
    descriptor_write[1].pImageInfo = nullptr;
    descriptor_write[1].pTexelBufferView = nullptr;

    m_Rhi->UpdateDescriptorSets(2, descriptor_write, 0, nullptr);
}

void DebugDrawAllocator::UnloadMeshBuffer()
{
    m_DefferDeleteQueue[m_CurrentFrame].push(m_SphereResource);
    m_SphereResource.buffer = nullptr;
    m_SphereResource.memory = nullptr;
    m_DefferDeleteQueue[m_CurrentFrame].push(m_CylinderResource);
    m_CylinderResource.buffer = nullptr;
    m_CylinderResource.memory = nullptr;
    m_DefferDeleteQueue[m_CurrentFrame].push(m_CapsuleResource);
    m_CapsuleResource.buffer = nullptr;
    m_CapsuleResource.memory = nullptr;
}

void DebugDrawAllocator::LoadSphereMeshBuffer()
{
    int32_t param = m_CircleSampleCount;
    // radios is 1
    float _2pi = 2.0f * Math_PI;
    std::vector<DebugDrawVertex> vertexs((param * 2 + 2) * (param * 2) * 2 + (param * 2 + 1) * (param * 2) * 2);

    int32_t current_index = 0;
    for (int32_t i = -param - 1; i < param + 1; i++)
    {
        float h = Math::sin(_2pi / 4.0f * i / (param + 1.0f));
        float h1 = Math::sin(_2pi / 4.0f * (i + 1) / (param + 1.0f));
        float r = Math::sqrt(1.0f - h * h);
        float r1 = Math::sqrt(1.0f - h1 * h1);
        for (int32_t j = 0; j < 2 * param; j++)
        {
            Vector3 p(Math::cos(_2pi / (2.0f * param) * j) * r, Math::sin(_2pi / (2.0f * param) * j) * r, h);
            Vector3 p1(Math::cos(_2pi / (2.0f * param) * j) * r1, Math::sin(_2pi / (2.0f * param) * j) * r1, h1);
            vertexs[current_index].pos = p;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

            vertexs[current_index].pos = p1;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        }
        if (i != -param - 1)
        {
            for (int32_t j = 0; j < 2 * param; j++)
            {
                Vector3 p(Math::cos(_2pi / (2.0f * param) * j) * r, Math::sin(_2pi / (2.0f * param) * j) * r, h);
                Vector3 p1(
                    Math::cos(_2pi / (2.0f * param) * (j + 1)) * r, Math::sin(_2pi / (2.0f * param) * (j + 1)) * r, h);
                vertexs[current_index].pos = p;
                vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

                vertexs[current_index].pos = p1;
                vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
            }
        }
    }

    uint64_t bufferSize = static_cast<uint64_t>(vertexs.size() * sizeof(DebugDrawVertex));

    m_Rhi->CreateBuffer(bufferSize,
                        RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY,
                        m_SphereResource.buffer,
                        m_SphereResource.memory);

    Resource stagingBuffer;
    m_Rhi->CreateBuffer(bufferSize,
                        RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        stagingBuffer.buffer,
                        stagingBuffer.memory);
    void* data;
    m_Rhi->MapMemory(stagingBuffer.memory, 0, bufferSize, 0, &data);
    memcpy(data, vertexs.data(), bufferSize);
    m_Rhi->UnmapMemory(stagingBuffer.memory);

    m_Rhi->CopyBuffer(stagingBuffer.buffer, m_SphereResource.buffer, 0, 0, bufferSize);

    m_Rhi->DestroyBuffer(stagingBuffer.buffer);
    m_Rhi->FreeMemory(stagingBuffer.memory);
}

void DebugDrawAllocator::LoadCylinderMeshBuffer()
{
    int param = m_CircleSampleCount;
    // radios is 1 , height is 2
    float _2pi = 2.0f * Math_PI;
    std::vector<DebugDrawVertex> vertexs(2 * param * 5 * 2);

    size_t current_index = 0;
    for (int32_t i = 0; i < 2 * param; i++)
    {
        Vector3 p(Math::cos(_2pi / (2.0f * param) * i), Math::sin(_2pi / (2.0f * param) * i), 1.0f);
        Vector3 p_(Math::cos(_2pi / (2.0f * param) * (i + 1)), Math::sin(_2pi / (2.0f * param) * (i + 1)), 1.0f);
        Vector3 p1(Math::cos(_2pi / (2.0f * param) * i), Math::sin(_2pi / (2.0f * param) * i), -1.0f);
        Vector3 p1_(Math::cos(_2pi / (2.0f * param) * (i + 1)), Math::sin(_2pi / (2.0f * param) * (i + 1)), -1.0f);

        vertexs[current_index].pos = p;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        vertexs[current_index].pos = p_;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

        vertexs[current_index].pos = p1;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        vertexs[current_index].pos = p1_;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

        vertexs[current_index].pos = p;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        vertexs[current_index].pos = p1;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

        vertexs[current_index].pos = p;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        vertexs[current_index].pos = Vector3(0.0f, 0.0f, 1.0f);
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

        vertexs[current_index].pos = p1;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        vertexs[current_index].pos = Vector3(0.0f, 0.0f, -1.0f);
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    uint64_t bufferSize = static_cast<uint64_t>(vertexs.size() * sizeof(DebugDrawVertex));

    m_Rhi->CreateBuffer(bufferSize,
                        RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY,
                        m_CylinderResource.buffer,
                        m_CylinderResource.memory);

    Resource stagingBuffer;
    m_Rhi->CreateBuffer(bufferSize,
                        RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        stagingBuffer.buffer,
                        stagingBuffer.memory);
    void* data;
    m_Rhi->MapMemory(stagingBuffer.memory, 0, bufferSize, 0, &data);
    memcpy(data, vertexs.data(), bufferSize);
    m_Rhi->UnmapMemory(stagingBuffer.memory);

    m_Rhi->CopyBuffer(stagingBuffer.buffer, m_CylinderResource.buffer, 0, 0, bufferSize);

    m_Rhi->DestroyBuffer(stagingBuffer.buffer);
    m_Rhi->FreeMemory(stagingBuffer.memory);
}

void DebugDrawAllocator::LoadCapsuleMeshBuffer()
{
    int param = m_CircleSampleCount;
    // radios is 1,height is 4
    float _2pi = 2.0f * Math_PI;
    std::vector<DebugDrawVertex> vertexs(2 * param * param * 4 + 2 * param * param * 4 + 2 * param * 2);

    size_t current_index = 0;
    for (int32_t i = 0; i < param; i++)
    {
        float h = Math::sin(_2pi / 4.0 / param * i);
        float h1 = Math::sin(_2pi / 4.0 / param * (i + 1));
        float r = Math::sqrt(1 - h * h);
        float r1 = Math::sqrt(1 - h1 * h1);
        for (int32_t j = 0; j < 2 * param; j++)
        {
            Vector3 p(Math::cos(_2pi / (2.0f * param) * j) * r, Math::sin(_2pi / (2.0f * param) * j) * r, h + 1.0f);
            Vector3 p_(Math::cos(_2pi / (2.0f * param) * (j + 1)) * r,
                       Math::sin(_2pi / (2.0f * param) * (j + 1)) * r,
                       h + 1.0f);
            Vector3 p1(Math::cos(_2pi / (2.0f * param) * j) * r1, Math::sin(_2pi / (2.0f * param) * j) * r1, h1 + 1.0f);
            vertexs[current_index].pos = p;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
            vertexs[current_index].pos = p1;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

            vertexs[current_index].pos = p;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
            vertexs[current_index].pos = p_;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    for (int32_t j = 0; j < 2 * param; j++)
    {
        Vector3 p(Math::cos(_2pi / (2.0f * param) * j), Math::sin(_2pi / (2.0f * param) * j), 1.0f);
        Vector3 p1(Math::cos(_2pi / (2.0f * param) * j), Math::sin(_2pi / (2.0f * param) * j), -1.0f);
        vertexs[current_index].pos = p;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        vertexs[current_index].pos = p1;
        vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    for (int32_t i = 0; i > -param; i--)
    {
        float h = Math::sin(_2pi / 4.0f / param * i);
        float h1 = Math::sin(_2pi / 4.0f / param * (i - 1));
        float r = Math::sqrt(1 - h * h);
        float r1 = Math::sqrt(1 - h1 * h1);
        for (int32_t j = 0; j < (2 * param); j++)
        {
            Vector3 p(Math::cos(_2pi / (2.0f * param) * j) * r, Math::sin(_2pi / (2.0f * param) * j) * r, h - 1.0f);
            Vector3 p_(Math::cos(_2pi / (2.0f * param) * (j + 1)) * r,
                       Math::sin(_2pi / (2.0f * param) * (j + 1)) * r,
                       h - 1.0f);
            Vector3 p1(Math::cos(_2pi / (2.0f * param) * j) * r1, Math::sin(_2pi / (2.0f * param) * j) * r1, h1 - 1.0f);
            vertexs[current_index].pos = p;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
            vertexs[current_index].pos = p1;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

            vertexs[current_index].pos = p;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
            vertexs[current_index].pos = p_;
            vertexs[current_index++].color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    uint64_t bufferSize = static_cast<uint64_t>(vertexs.size() * sizeof(DebugDrawVertex));

    m_Rhi->CreateBuffer(bufferSize,
                        RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY,
                        m_CapsuleResource.buffer,
                        m_CapsuleResource.memory);

    Resource stagingBuffer;
    m_Rhi->CreateBuffer(bufferSize,
                        RHI_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        stagingBuffer.buffer,
                        stagingBuffer.memory);
    void* data;
    m_Rhi->MapMemory(stagingBuffer.memory, 0, bufferSize, 0, &data);
    memcpy(data, vertexs.data(), bufferSize);
    m_Rhi->UnmapMemory(stagingBuffer.memory);

    m_Rhi->CopyBuffer(stagingBuffer.buffer, m_CapsuleResource.buffer, 0, 0, bufferSize);

    m_Rhi->DestroyBuffer(stagingBuffer.buffer);
    m_Rhi->FreeMemory(stagingBuffer.memory);
}

RHIBuffer* DebugDrawAllocator::GetSphereVertexBuffer()
{
    if (m_SphereResource.buffer == nullptr)
    {
        LoadSphereMeshBuffer();
    }
    return m_SphereResource.buffer;
}
RHIBuffer* DebugDrawAllocator::GetCylinderVertexBuffer()
{
    if (m_CylinderResource.buffer == nullptr)
    {
        LoadCylinderMeshBuffer();
    }
    return m_CylinderResource.buffer;
}
RHIBuffer* DebugDrawAllocator::GetCapsuleVertexBuffer()
{
    if (m_CapsuleResource.buffer == nullptr)
    {
        LoadCapsuleMeshBuffer();
    }
    return m_CapsuleResource.buffer;
}

const size_t DebugDrawAllocator::GetSphereVertexBufferSize() const
{
    return ((m_CircleSampleCount * 2 + 2) * (m_CircleSampleCount * 2) * 2 +
            (m_CircleSampleCount * 2 + 1) * (m_CircleSampleCount * 2) * 2);
}
const size_t DebugDrawAllocator::GetCylinderVertexBufferSize() const
{
    return (m_CircleSampleCount * 2) * 5 * 2;
}
const size_t DebugDrawAllocator::GetCapsuleVertexBufferSize() const
{
    return (m_CircleSampleCount * 2) * m_CircleSampleCount * 4 + (2 * m_CircleSampleCount) * 2 +
           (2 * m_CircleSampleCount) * m_CircleSampleCount * 4;
}
const size_t DebugDrawAllocator::GetCapsuleVertexBufferUpSize() const
{
    return (m_CircleSampleCount * 2) * m_CircleSampleCount * 4;
}
const size_t DebugDrawAllocator::GetCapsuleVertexBufferMidSize() const
{
    return 2 * m_CircleSampleCount * 2;
}
const size_t DebugDrawAllocator::GetCapsuleVertexBufferDownSize() const
{
    return 2 * m_CircleSampleCount * m_CircleSampleCount * 4;
}

const size_t DebugDrawAllocator::GetSizeOfUniformBufferObject() const
{
    return sizeof(UniformBufferDynamicObject);
}