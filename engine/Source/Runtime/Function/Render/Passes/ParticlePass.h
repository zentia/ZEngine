#pragma once

#include "Runtime/Function/Particle/ParticleCommon.h"
#include "Runtime/Function/Particle/ParticleManager.h"
#include "Runtime/Function/Render/RenderPass.h"
#include "Runtime/Function/Render/RenderResource.h"

#include <array>

struct ParticlePassInitInfo : RenderPassInitInfo
{
    std::shared_ptr<ParticleManager> m_ParticleManager;
};

class ParticleEmitterBufferBatch
{
public:
    RHIBuffer* m_PositionRenderBuffer = nullptr;
    RHIBuffer* m_PositionDeviceBuffer = nullptr;
    RHIBuffer* m_PositionHostBuffer = nullptr;
    RHIBuffer* m_CounterDeviceBuffer = nullptr;
    RHIBuffer* m_CounterHostBuffer = nullptr;
    RHIBuffer* m_IndirectDispatchArgumentBuffer = nullptr;
    RHIBuffer* m_AliveListBuffer = nullptr;
    RHIBuffer* m_AliveListNextBuffer = nullptr;
    RHIBuffer* m_DeadListBuffer = nullptr;
    RHIBuffer* m_ParticleComponentResBuffer = nullptr;

    RHIDeviceMemory* m_CounterHostMemory = nullptr;
    RHIDeviceMemory* m_PositionHostMemory = nullptr;
    RHIDeviceMemory* m_PositionDeviceMemory = nullptr;
    RHIDeviceMemory* m_CounterDeviceMemory = nullptr;
    RHIDeviceMemory* m_IndirectDispatchArgumentMemory = nullptr;
    RHIDeviceMemory* m_AliveListMemory = nullptr;
    RHIDeviceMemory* m_AliveListNextMemory = nullptr;
    RHIDeviceMemory* m_DeadListMemory = nullptr;
    RHIDeviceMemory* m_ParticleComponentResMemory = nullptr;
    RHIDeviceMemory* m_PositionRenderMemory = nullptr;

    void* m_EmitterDescMapped {nullptr};

    ParticleEmitterDesc m_EmitterDesc;

    uint32_t m_NumParticle {0};
    void FreeUpBatch(std::shared_ptr<RHI> rhi);
};

class ParticlePass : public RenderPass
{
public:
    void Initialize(const RenderPassInitInfo* init_info) override final;

    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override final;

    void Draw() override final;

    void Simulate();

    void CopyNormalAndDepthImage();

    void SetDepthAndNormalImage(RHIImage* depth_image, RHIImage* normal_image);

    void SetupParticlePass();

    void SetRenderCommandBufferHandle(RHICommandBuffer* command_buffer);

    void SetRenderPassHandle(RHIRenderPass* render_pass);

    void UpdateAfterFramebufferRecreate();

    void SetEmitterCount(int count);

    void CreateEmitter(int id, const ParticleEmitterDesc& desc);

    void InitializeEmitters();

    void SetTickIndices(const std::vector<ParticleEmitterID>& tick_indices);

    void SetTransformIndices(const std::vector<ParticleEmitterTransformDesc>& transform_indices);

private:
    void UpdateUniformBuffer(ViewportType viewport_type);

    void UpdateEmitterTransform();

    void SetupAttachments();

    // SRP-style lazy initialization: ensure attachments are loaded when ParticleManager is ready
    void EnsureAttachmentsInitialized();

    void SetupDescriptorSetLayout();

    void PrepareUniformBuffer();

    void SetupPipelines();

    void AllocateDescriptorSet();

    void UpdateDescriptorSet();

    void SetupParticleDescriptorSet();

    RHIPipeline* m_KickoffPipeline = nullptr;
    RHIPipeline* m_EmitPipeline = nullptr;
    RHIPipeline* m_SimulatePipeline = nullptr;

    RHICommandBuffer* m_ComputeCommandBuffer = nullptr;
    RHICommandBuffer* m_RenderCommandBuffer = nullptr;
    RHICommandBuffer* m_CopyCommandBuffer = nullptr;

    RHIBuffer* m_SceneUniformBuffer = nullptr;
    RHIBuffer* m_ComputeUniformBuffer = nullptr;
    RHIBuffer* m_ParticleBillboardUniformBuffer = nullptr;

    RHIViewport m_ViewportParams;

    RHIFence* m_Fence = nullptr;

    RHIImage* m_SrcDepthImage = nullptr;
    RHIImage* m_DstNormalImage = nullptr;
    RHIImage* m_SrcNormalImage = nullptr;
    RHIImage* m_DstDepthImage = nullptr;
    RHIImageView* m_SrcDepthImageView = nullptr;
    RHIImageView* m_SrcNormalImageView = nullptr;
    RHIDeviceMemory* m_DstNormalImageMemory = nullptr;
    RHIDeviceMemory* m_DstDepthImageMemory = nullptr;

    /*
     * particle rendering
     */
    RHIImage* m_ParticleBillboardTextureImage = nullptr;
    RHIImageView* m_ParticleBillboardTextureImageView = nullptr;
    VmaAllocation m_ParticleBillboardTextureVmaAllocation;

    RHIImage* m_ZengineLogoTextureImage = nullptr;
    RHIImageView* m_ZengineLogoTextureImageView = nullptr;
    VmaAllocation m_ZengineLogoTextureVmaAllocation;

    RHIRenderPass* m_RenderPass = nullptr;

    std::array<ParticleBillboardPerframeStorageBufferObject, 2>
        m_ParticlebillboardPerframeStorageBufferObjects;
    ParticleBillboardPerframeStorageBufferObject m_ParticlebillboardPerframeStorageBufferObject;
    std::array<ParticleCollisionPerframeStorageBufferObject, 2>
        m_ParticleCollisionPerframeStorageBufferObjects;
    ParticleCollisionPerframeStorageBufferObject m_ParticleCollisionPerframeStorageBufferObject;

    void* m_ParticleComputeBufferMapped {nullptr};
    void* m_ParticleBillboardUniformBufferMapped {nullptr};
    void* m_SceneUniformBufferMapped {nullptr};

    struct uvec4
    {
        uint32_t x;
        uint32_t y;
        uint32_t z;
        uint32_t w;
    };

    struct computeUniformBufferObject
    {
        int emit_gap;
        int xemit_count;
        float max_life;
        float time_step;
        Vector4 pack;  // randomness 3 | frame index 1
        Vector3 gravity;
        float padding;
        uvec4 viewport;  // x, y, width, height
        Vector4 extent;  // width, height, near, far
    } m_Ubo;

    struct Particle
    {
        Vector3 pos;
        float life;
        Vector3 vel;
        float size_x;
        Vector3 acc;
        float size_y;
        Vector4 color;
    };

    // indirect dispath parameter offset
    static const uint32_t s_ArgumentOffsetEmit = 0;
    static const uint32_t s_ArgumentOffsetSimulate = s_ArgumentOffsetEmit + sizeof(uvec4);
    struct IndirectArgumemt
    {
        uvec4 emit_argument;
        uvec4 simulate_argument;
        int alive_flap_bit;
    };

    struct ParticleCounter
    {
        int dead_count;
        int alive_count;
        int alive_count_after_sim;
        int emit_count;
    };

    std::vector<ParticleEmitterBufferBatch> m_EmitterBufferBatches;
    std::shared_ptr<ParticleManager> m_ParticleManager;

    DefaultRNG m_RandomEngine;

    int m_EmitterCount;

    static constexpr bool s_VerboseParticleAliveInfo {false};

    std::vector<ParticleEmitterID> m_EmitterTickIndices;

    std::vector<ParticleEmitterTransformDesc> m_EmitterTransformIndices;

    // Track if attachments (textures) have been initialized
    // This allows lazy initialization when ParticleManager becomes available
    bool m_AttachmentsInitialized = false;
};