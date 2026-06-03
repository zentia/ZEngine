#pragma once

// =====================================================================
// VulkanBindlessTextureManager (PR3)
// ---------------------------------------------------------------------
// Concrete Vulkan implementation of the cross-backend
// RHIBindlessTextureManager interface. Owns:
//
//   - one VkDescriptorPool created with
//     VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
//   - one VkDescriptorSetLayout with a single binding (binding=0,
//     COMBINED_IMAGE_SAMPLER, count=capacity), flagged with
//     UPDATE_AFTER_BIND | PARTIALLY_BOUND | VARIABLE_DESCRIPTOR_COUNT;
//   - one VkDescriptorSet allocated from the pool with the variable
//     count tail set to capacity-1 (the high-water mark; the pool is
//     sized for the full capacity to keep things simple).
//
// Slot allocation strategy:
//   - free-list LIFO (std::vector<uint32_t>) for cheap reuse;
//   - monotonically increasing high-water mark when the free list is
//     empty;
//   - slot 0 is reserved on construction so callers can use it as
//     the "default white" placeholder.
//
// Thread-safety:
//   - allocate / free / update guarded by std::mutex. Vulkan
//     UPDATE_AFTER_BIND lets us call vkUpdateDescriptorSets while
//     command buffers that already bound the set are in flight, as
//     long as nobody is *reading* the slot we are writing.
//
// References (drawn from):
//   - UnrealEngine: FVulkanBindlessDescriptorManager
//     (Engine/Source/Runtime/VulkanRHI/Private/VulkanBindless*.cpp)
//   - Unity 2023.1 Vulkan backend:
//     Modules/Vulkan/VKDescriptorSetManager.cpp (the
//     UPDATE_AFTER_BIND_POOL section).
// We do NOT mirror them line-for-line; we only borrow the layout
// shape (single-set, single-binding, partial-bound + variable-count).
// =====================================================================

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderType.h"

#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>

class VulkanBindlessTextureManager final : public RHIBindlessTextureManager
{
public:
    // Set index reserved for the bindless table when binding in
    // pipeline layouts. Materials / passes that consume the bindless
    // path MUST use this same set index, kept here as the single
    // source of truth.
    static constexpr uint32_t kBindlessDescriptorSet = 0;

    // Binding index inside that set.
    static constexpr uint32_t kBindlessTextureBinding = 0;

    // PR-V2: shape of the push-constant range that every Vulkan
    // pipeline layout consuming the bindless path MUST declare.
    //
    // Why these exact values:
    //   - stageFlags = RHI_SHADER_STAGE_ALL: VulkanRHI::CmdSetBindlessIndexPFN
    //     issues vkCmdPushConstants with VK_SHADER_STAGE_ALL (see the
    //     contract comment at vulkan_rhi.cpp ~line 2806). The layout
    //     range MUST cover at least every stage that touches the index;
    //     declaring ALL is the cheapest way to keep the validation
    //     layer quiet across vertex/fragment/compute pipelines without
    //     forcing each pass to enumerate its stage mask.
    //   - offset = 0: hard-coded by VulkanRHI::CmdSetBindlessIndexPFN
    //     and by every shader's `layout(push_constant) uniform { uint
    //     g_packed_indices; }` declaration. Anchoring at 0 leaves the
    //     128-byte push-constant minGuarantee window free for non-
    //     bindless data after byte 4 if a pass needs it.
    //   - size = 4: the bindless payload is a single 32-bit
    //     BindlessIndex::Pack(...) value (see rhi.h's BindlessIndex
    //     namespace -- 16 bits texture | 16 bits sampler).
    //
    // This helper is the SINGLE SOURCE OF TRUTH for the layout's
    // bindless push range across all Vulkan-side pass code. Pass
    // authors should call it directly when populating
    // RHIPipelineLayoutCreateInfo::pPushConstantRanges instead of
    // open-coding `{RHI_SHADER_STAGE_ALL, 0, 4}` -- so that any future
    // contract change (e.g. growing the payload to 8 bytes for a
    // material-id half) is a one-line edit here.
    //
    // DX12 has no analogue because DX12RHI::CreatePipelineLayout
    // synthesises the equivalent root32 inline; the field is
    // intentionally Vulkan-flavoured. See AGENTS.md 2.9 PR-V2 for the
    // cross-backend rationale.
    [[nodiscard]] static constexpr RHIPushConstantRange GetBindlessPushConstantRange() noexcept
    {
        RHIPushConstantRange r {};
        r.stageFlags = static_cast<RHIShaderStageFlags>(RHI_SHADER_STAGE_ALL);
        r.offset = 0;
        r.size = sizeof(uint32_t);
        return r;
    }

    VulkanBindlessTextureManager() = default;
    ~VulkanBindlessTextureManager() override = default;

    // Two-phase init so we never throw out of a constructor and we
    // can fail gracefully (e.g. driver lies about feature support).
    // Returns false on any Vulkan error; the manager is unusable in
    // that case and the owning RHI should not publish it.
    //
    // PR5a: physical_device + graphics_queue + graphics_queue_family
    // are required because the manager now also creates and uploads
    // a 1x1 white placeholder image into slot 0 (so any shader that
    // samples an unbound bindless index sees defined data instead of
    // garbage). The upload uses a one-shot command buffer on the
    // graphics queue and synchronises with a fence before returning.
    bool Initialize(VkDevice device,
                    VkPhysicalDevice physical_device,
                    VkQueue graphics_queue,
                    uint32_t graphics_queue_family,
                    uint32_t capacity);

    // Releases the pool / layout / set. Safe to call multiple times.
    void Shutdown();

    // ------- RHIBindlessTextureManager overrides -------------------
    uint32_t capacity() const override { return m_Capacity; }
    uint32_t allocate(RHIImageView* image_view, RHISampler* sampler) override;
    void free(uint32_t index) override;
    void Update(uint32_t index, RHIImageView* image_view, RHISampler* sampler) override;
    // ---------------------------------------------------------------

    // Vulkan-specific accessors used by the rest of the Vulkan
    // backend when wiring pipelines and binding descriptor sets.
    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_SetLayout; }
    VkDescriptorSet GetDescriptorSet() const { return m_Set; }
    VkDescriptorPool getDescriptorPool() const { return m_Pool; }

private:
    // Internal write helper. Caller must already hold m_Mutex.
    void WriteDescriptorLocked(uint32_t index, VkImageView view, VkSampler sampler);

    // PR5a: builds the 1x1 white placeholder (image + memory + view +
    // sampler), uploads `0xFFFFFFFF` to it via a one-shot command
    // buffer on the graphics queue, transitions to
    // SHADER_READ_ONLY_OPTIMAL, then writes the descriptor at slot 0.
    // Owns the resources for its lifetime; freed in Shutdown().
    // Returns false on any failure (caller treats this as a fatal
    // init error -- without slot 0 the contract is broken).
    bool CreateAndUploadPlaceholderLocked(VkPhysicalDevice physical_device,
                                          VkQueue graphics_queue,
                                          uint32_t graphics_queue_family);
    void DestroyPlaceholderLocked();

    VkDevice m_Device {VK_NULL_HANDLE};
    VkDescriptorPool m_Pool {VK_NULL_HANDLE};
    VkDescriptorSetLayout m_SetLayout {VK_NULL_HANDLE};
    VkDescriptorSet m_Set {VK_NULL_HANDLE};

    // PR5a: slot-0 placeholder ("default white"). Owned by the
    // manager so its lifetime is tied to the bindless table itself.
    VkImage m_PlaceholderImage {VK_NULL_HANDLE};
    VkDeviceMemory m_PlaceholderMemory {VK_NULL_HANDLE};
    VkImageView m_PlaceholderView {VK_NULL_HANDLE};
    VkSampler m_PlaceholderSampler {VK_NULL_HANDLE};

    uint32_t m_Capacity {0};

    // Slot bookkeeping (guarded by m_Mutex):
    //   - m_HighWaterMark : next never-allocated slot
    //   - m_FreeList       : LIFO of slots returned via free()
    std::mutex m_Mutex;
    uint32_t m_HighWaterMark {0};
    std::vector<uint32_t> m_FreeList;
};
