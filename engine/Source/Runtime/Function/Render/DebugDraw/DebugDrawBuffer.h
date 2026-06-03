#pragma once

#include "DebugDrawFont.h"
#include "DebugDrawPrimitive.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <queue>
class DebugDrawAllocator
{
public:
    DebugDrawAllocator() {};
    void Initialize(DebugDrawFont* font);
    void Destory();
    void Tick();
    void clear();
    void ClearBuffer();

    size_t CacheVertexs(const std::vector<DebugDrawVertex>& vertexs);
    void CacheUniformObject(Matrix4x4 proj_view_matrix);
    size_t CacheUniformDynamicObject(const std::vector<std::pair<Matrix4x4, Vector4>>& model_colors);

    size_t GetVertexCacheOffset() const;
    size_t GetUniformDynamicCacheOffset() const;
    void Allocator();

    RHIBuffer* GetVertexBuffer();
    RHIDescriptorSet*& GetDescriptorSet();

    RHIBuffer* GetSphereVertexBuffer();
    RHIBuffer* GetCylinderVertexBuffer();
    RHIBuffer* GetCapsuleVertexBuffer();

    const size_t GetSphereVertexBufferSize() const;
    const size_t GetCylinderVertexBufferSize() const;
    const size_t GetCapsuleVertexBufferSize() const;
    const size_t GetCapsuleVertexBufferUpSize() const;
    const size_t GetCapsuleVertexBufferMidSize() const;
    const size_t GetCapsuleVertexBufferDownSize() const;

    const size_t GetSizeOfUniformBufferObject() const;

private:
    RHI* m_Rhi;
    struct UniformBufferObject
    {
        Matrix4x4 proj_view_matrix;
    };

    struct alignas(256) UniformBufferDynamicObject
    {
        Matrix4x4 model_matrix;
        Vector4 color;
    };

    struct Resource
    {
        RHIBuffer* buffer = nullptr;
        RHIDeviceMemory* memory = nullptr;
    };
    struct Descriptor
    {
        RHIDescriptorSetLayout* layout = nullptr;
        std::vector<RHIDescriptorSet*> descriptor_set;
    };

    // descriptor
    Descriptor m_Descriptor;

    // changeable resource
    Resource m_VertexResource;
    std::vector<DebugDrawVertex> m_VertexCache;

    Resource m_UniformResource;
    UniformBufferObject m_UniformBufferObject;

    Resource m_UniformDynamicResource;
    std::vector<UniformBufferDynamicObject> m_UniformBufferDynamicObjectCache;

    // static mesh resource
    Resource m_SphereResource;
    Resource m_CylinderResource;
    Resource m_CapsuleResource;

    // font resource
    DebugDrawFont* m_Font = nullptr;

    // resource deleter
    static const uint32_t k_deferred_delete_resource_frame_count =
        5;  // the count means after count-1 frame will be delete
    uint32_t m_CurrentFrame = 0;
    std::queue<Resource> m_DefferDeleteQueue[k_deferred_delete_resource_frame_count];

private:
    void SetupDescriptorSet();
    void PrepareDescriptorSet();
    void UpdateDescriptorSet();
    void FlushPendingDelete();
    void UnloadMeshBuffer();
    void LoadSphereMeshBuffer();
    void LoadCylinderMeshBuffer();
    void LoadCapsuleMeshBuffer();

    const int m_CircleSampleCount = 10;
};