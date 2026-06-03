// =====================================================================
// VulkanBindlessTextureManager implementation (PR3)
// =====================================================================

#include "Runtime/Function/Render/Interface/Vulkan/VulkanBindlessTextureManager.h"

#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHIResource.h"

#include <algorithm>
#include <cstring>

bool VulkanBindlessTextureManager::Initialize(VkDevice device,
                                              VkPhysicalDevice physical_device,
                                              VkQueue graphics_queue,
                                              uint32_t graphics_queue_family,
                                              uint32_t capacity)
{
    if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE ||
        graphics_queue == VK_NULL_HANDLE || capacity == 0)
    {
        LOG_WARNING(ZVulkan,
                    "VulkanBindlessTextureManager::Initialize called with invalid args "
                    "(device={}, physical={}, queue={}, capacity={})",
                    (void*)device,
                    (void*)physical_device,
                    (void*)graphics_queue,
                    capacity);
        return false;
    }

    m_Device = device;
    m_Capacity = capacity;
    m_HighWaterMark = 0;
    m_FreeList.clear();
    m_FreeList.reserve(64);

    // -----------------------------------------------------------------
    // 1. Descriptor pool sized for the full capacity.
    //    UPDATE_AFTER_BIND_BIT_EXT lets us write to the set while a
    //    command buffer that already bound it is in flight.
    // -----------------------------------------------------------------
    VkDescriptorPoolSize pool_size {};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = m_Capacity;

    VkDescriptorPoolCreateInfo pool_ci {};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;

    if (vkCreateDescriptorPool(m_Device, &pool_ci, nullptr, &m_Pool) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "VulkanBindlessTextureManager: vkCreateDescriptorPool failed");
        m_Pool = VK_NULL_HANDLE;
        return false;
    }

    // -----------------------------------------------------------------
    // 2. Set layout with a single binding flagged for descriptor
    //    indexing. VARIABLE_DESCRIPTOR_COUNT is mandatory on the last
    //    binding -- it lets us tell the driver "the actual count we
    //    will allocate is N <= capacity" at allocate time.
    // -----------------------------------------------------------------
    VkDescriptorSetLayoutBinding binding {};
    binding.binding = kBindlessTextureBinding;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = m_Capacity;
    binding.stageFlags = VK_SHADER_STAGE_ALL;  // accessible from any pipeline stage
    binding.pImmutableSamplers = nullptr;

    VkDescriptorBindingFlagsEXT binding_flags =
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT |
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT;

    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT binding_flags_ci {};
    binding_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
    binding_flags_ci.bindingCount = 1;
    binding_flags_ci.pBindingFlags = &binding_flags;

    VkDescriptorSetLayoutCreateInfo layout_ci {};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &binding_flags_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(m_Device, &layout_ci, nullptr, &m_SetLayout) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "VulkanBindlessTextureManager: vkCreateDescriptorSetLayout failed");
        vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
        m_Pool = VK_NULL_HANDLE;
        m_SetLayout = VK_NULL_HANDLE;
        return false;
    }

    // -----------------------------------------------------------------
    // 3. Allocate the single descriptor set; the variable-count tail
    //    is set to capacity, matching the binding's descriptorCount.
    //    (We do not save memory by under-allocating here -- the pool
    //    already holds capacity descriptors -- this just satisfies
    //    the validation contract of VARIABLE_DESCRIPTOR_COUNT.)
    // -----------------------------------------------------------------
    uint32_t variable_count = m_Capacity;

    VkDescriptorSetVariableDescriptorCountAllocateInfoEXT variable_alloc_ci {};
    variable_alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT;
    variable_alloc_ci.descriptorSetCount = 1;
    variable_alloc_ci.pDescriptorCounts = &variable_count;

    VkDescriptorSetAllocateInfo set_ai {};
    set_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_ai.pNext = &variable_alloc_ci;
    set_ai.descriptorPool = m_Pool;
    set_ai.descriptorSetCount = 1;
    set_ai.pSetLayouts = &m_SetLayout;

    if (vkAllocateDescriptorSets(m_Device, &set_ai, &m_Set) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "VulkanBindlessTextureManager: vkAllocateDescriptorSets failed");
        vkDestroyDescriptorSetLayout(m_Device, m_SetLayout, nullptr);
        vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
        m_Pool = VK_NULL_HANDLE;
        m_SetLayout = VK_NULL_HANDLE;
        m_Set = VK_NULL_HANDLE;
        return false;
    }

    // Reserve slot 0 as the "default / missing texture" placeholder.
    // PR5a: we now actually populate that slot with a 1x1 opaque
    // white image + a default linear-clamp sampler, owned by the
    // manager itself. This guarantees that any draw which samples an
    // unbound bindless index sees defined data (white) instead of
    // whatever stale memory the descriptor would otherwise point at,
    // which is required behaviour under PARTIALLY_BOUND.
    m_HighWaterMark = 1;

    if (!CreateAndUploadPlaceholderLocked(physical_device, graphics_queue, graphics_queue_family))
    {
        LOG_ERROR(ZVulkan,
                  "VulkanBindlessTextureManager: failed to create slot-0 placeholder; aborting init");
        // Tear down what we already built so the caller can demote
        // bindless support cleanly.
        DestroyPlaceholderLocked();
        if (m_Pool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
            m_Pool = VK_NULL_HANDLE;
        }
        if (m_SetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_Device, m_SetLayout, nullptr);
            m_SetLayout = VK_NULL_HANDLE;
        }
        m_Set = VK_NULL_HANDLE;
        m_Capacity = 0;
        return false;
    }

    LOG_INFO(ZVulkan,
             "VulkanBindlessTextureManager: initialized (capacity = {}, slot 0 = 1x1 white placeholder)",
             m_Capacity);
    return true;
}

void VulkanBindlessTextureManager::Shutdown()
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return;
    }

    // PR5a: placeholder resources first (they reference the device
    // and were created against this manager's lifetime). Order vs.
    // pool destruction is irrelevant -- the descriptor set's writes
    // become invalid the moment the pool dies anyway -- but we keep
    // it before the pool for symmetry with creation order.
    DestroyPlaceholderLocked();

    // The set is freed implicitly when the pool is destroyed; no
    // separate vkFreeDescriptorSets call is needed (and the pool
    // wasn't created with FREE_DESCRIPTOR_SET_BIT anyway).
    if (m_Pool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
        m_Pool = VK_NULL_HANDLE;
    }
    if (m_SetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_Device, m_SetLayout, nullptr);
        m_SetLayout = VK_NULL_HANDLE;
    }
    m_Set = VK_NULL_HANDLE;
    m_Device = VK_NULL_HANDLE;
    m_Capacity = 0;
    m_HighWaterMark = 0;
    m_FreeList.clear();
}

uint32_t VulkanBindlessTextureManager::allocate(RHIImageView* image_view, RHISampler* sampler)
{
    if (m_Set == VK_NULL_HANDLE || image_view == nullptr || sampler == nullptr)
    {
        return kInvalidBindlessIndex;
    }

    VkImageView vk_view = static_cast<VulkanImageView*>(image_view)->getResource();
    VkSampler vk_sampler = static_cast<VulkanSampler*>(sampler)->getResource();
    if (vk_view == VK_NULL_HANDLE || vk_sampler == VK_NULL_HANDLE)
    {
        return kInvalidBindlessIndex;
    }

    uint32_t slot = kInvalidBindlessIndex;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_FreeList.empty())
        {
            slot = m_FreeList.back();
            m_FreeList.pop_back();
        }
        else if (m_HighWaterMark < m_Capacity)
        {
            slot = m_HighWaterMark++;
        }
        else
        {
            // Table exhausted. Higher-level code is expected to log
            // its own context (which texture / material is to blame),
            // we just signal failure here.
            return kInvalidBindlessIndex;
        }

        WriteDescriptorLocked(slot, vk_view, vk_sampler);
    }
    return slot;
}

void VulkanBindlessTextureManager::free(uint32_t index)
{
    if (index == kInvalidBindlessIndex || index >= m_Capacity || index == 0)
    {
        // Slot 0 is the reserved placeholder and must not be returned
        // to the free list, otherwise a later allocate() could hand
        // it out and overwrite the placeholder.
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    // We deliberately do NOT clear the descriptor entry here; it
    // remains "stale" until reused, which is legal under
    // PARTIALLY_BOUND: only slots actually sampled by a draw need to
    // hold a valid view+sampler pair.
    m_FreeList.push_back(index);
}

void VulkanBindlessTextureManager::Update(uint32_t index, RHIImageView* image_view, RHISampler* sampler)
{
    if (m_Set == VK_NULL_HANDLE || index >= m_Capacity || image_view == nullptr || sampler == nullptr)
    {
        return;
    }
    VkImageView vk_view = static_cast<VulkanImageView*>(image_view)->getResource();
    VkSampler vk_sampler = static_cast<VulkanSampler*>(sampler)->getResource();
    if (vk_view == VK_NULL_HANDLE || vk_sampler == VK_NULL_HANDLE)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    WriteDescriptorLocked(index, vk_view, vk_sampler);
}

void VulkanBindlessTextureManager::WriteDescriptorLocked(uint32_t index, VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo image_info {};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = view;
    image_info.sampler = sampler;

    VkWriteDescriptorSet write {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_Set;
    write.dstBinding = kBindlessTextureBinding;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

// =====================================================================
// PR5a: slot-0 default-white placeholder
// ---------------------------------------------------------------------
// We deliberately keep this self-contained -- no dependency on the
// engine's higher-level texture / staging buffer abstractions -- so
// that the bindless manager can come up before any of those systems
// exist. The cost is ~30 lines of plain Vulkan boilerplate.
//
// Steps:
//   1. Create a 1x1 RGBA8_UNORM VkImage (TRANSFER_DST | SAMPLED).
//   2. Allocate device-local memory and bind it.
//   3. Create a host-visible staging buffer holding a single
//      0xFFFFFFFF (opaque white) texel.
//   4. Record a one-shot command buffer:
//        UNDEFINED -> TRANSFER_DST_OPTIMAL,
//        vkCmdCopyBufferToImage,
//        TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL.
//   5. Submit on the graphics queue with a fence; wait. Tear down
//      staging resources.
//   6. Create the VkImageView (2D, RGBA8) and a default linear-clamp
//      VkSampler (no anisotropy -- we don't enable it on the device
//      yet).
//   7. Write descriptor at slot 0.
// =====================================================================

namespace
{
    // Picks a memory type from the physical device's memory properties
    // that satisfies (typeBits & (1<<i)) and contains all of `required`.
    // Returns UINT32_MAX on failure.
    uint32_t FindMemoryType(VkPhysicalDevice physical_device,
                            uint32_t type_bits,
                            VkMemoryPropertyFlags required)
    {
        VkPhysicalDeviceMemoryProperties props {};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        {
            if ((type_bits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & required) == required)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }
}  // namespace

bool VulkanBindlessTextureManager::CreateAndUploadPlaceholderLocked(VkPhysicalDevice physical_device,
                                                                    VkQueue graphics_queue,
                                                                    uint32_t graphics_queue_family)
{
    // ---- 1. Image -----------------------------------------------------
    VkImageCreateInfo image_ci {};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent = {1, 1, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_Device, &image_ci, nullptr, &m_PlaceholderImage) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements mem_req {};
    vkGetImageMemoryRequirements(m_Device, m_PlaceholderImage, &mem_req);

    const uint32_t device_local_type = FindMemoryType(physical_device,
                                                      mem_req.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (device_local_type == UINT32_MAX)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: no DEVICE_LOCAL memory type for image");
        return false;
    }

    VkMemoryAllocateInfo image_alloc_ai {};
    image_alloc_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_alloc_ai.allocationSize = mem_req.size;
    image_alloc_ai.memoryTypeIndex = device_local_type;
    if (vkAllocateMemory(m_Device, &image_alloc_ai, nullptr, &m_PlaceholderMemory) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkAllocateMemory(image) failed");
        return false;
    }
    vkBindImageMemory(m_Device, m_PlaceholderImage, m_PlaceholderMemory, 0);

    // ---- 2. Staging buffer with one opaque-white texel ---------------
    VkBuffer staging_buffer {VK_NULL_HANDLE};
    VkDeviceMemory staging_memory {VK_NULL_HANDLE};

    VkBufferCreateInfo buffer_ci {};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = 4;  // 1 RGBA8 texel
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_Device, &buffer_ci, nullptr, &staging_buffer) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkCreateBuffer(staging) failed");
        return false;
    }

    VkMemoryRequirements staging_req {};
    vkGetBufferMemoryRequirements(m_Device, staging_buffer, &staging_req);
    const uint32_t host_visible_type =
        FindMemoryType(physical_device,
                       staging_req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (host_visible_type == UINT32_MAX)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: no HOST_VISIBLE|COHERENT memory type");
        vkDestroyBuffer(m_Device, staging_buffer, nullptr);
        return false;
    }

    VkMemoryAllocateInfo staging_ai {};
    staging_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    staging_ai.allocationSize = staging_req.size;
    staging_ai.memoryTypeIndex = host_visible_type;
    if (vkAllocateMemory(m_Device, &staging_ai, nullptr, &staging_memory) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkAllocateMemory(staging) failed");
        vkDestroyBuffer(m_Device, staging_buffer, nullptr);
        return false;
    }
    vkBindBufferMemory(m_Device, staging_buffer, staging_memory, 0);

    void* mapped = nullptr;
    if (vkMapMemory(m_Device, staging_memory, 0, staging_req.size, 0, &mapped) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkMapMemory(staging) failed");
        vkDestroyBuffer(m_Device, staging_buffer, nullptr);
        vkFreeMemory(m_Device, staging_memory, nullptr);
        return false;
    }
    const uint32_t white = 0xFFFFFFFFu;  // R=G=B=A=0xFF (opaque white)
    std::memcpy(mapped, &white, sizeof(white));
    vkUnmapMemory(m_Device, staging_memory);

    // ---- 3. One-shot command buffer ----------------------------------
    VkCommandPool upload_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_ci {};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_ci.queueFamilyIndex = graphics_queue_family;
    if (vkCreateCommandPool(m_Device, &pool_ci, nullptr, &upload_pool) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkCreateCommandPool failed");
        vkDestroyBuffer(m_Device, staging_buffer, nullptr);
        vkFreeMemory(m_Device, staging_memory, nullptr);
        return false;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmd_ai {};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = upload_pool;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_Device, &cmd_ai, &cmd) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkAllocateCommandBuffers failed");
        vkDestroyCommandPool(m_Device, upload_pool, nullptr);
        vkDestroyBuffer(m_Device, staging_buffer, nullptr);
        vkFreeMemory(m_Device, staging_memory, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier to_dst {};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = m_PlaceholderImage;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.baseMipLevel = 0;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.baseArrayLayer = 0;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &to_dst);

    VkBufferImageCopy region {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd,
                           staging_buffer,
                           m_PlaceholderImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);

    // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &to_read);

    vkEndCommandBuffer(cmd);

    // Submit + wait via dedicated fence (no dependency on the
    // RHI-wide frame-in-flight fences; this happens before the
    // first frame).
    VkFence upload_fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_ci {};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(m_Device, &fence_ci, nullptr, &upload_fence);

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vkQueueSubmit(graphics_queue, 1, &submit, upload_fence) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkQueueSubmit failed");
        vkDestroyFence(m_Device, upload_fence, nullptr);
        vkDestroyCommandPool(m_Device, upload_pool, nullptr);
        vkDestroyBuffer(m_Device, staging_buffer, nullptr);
        vkFreeMemory(m_Device, staging_memory, nullptr);
        return false;
    }
    vkWaitForFences(m_Device, 1, &upload_fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(m_Device, upload_fence, nullptr);
    vkDestroyCommandPool(m_Device, upload_pool, nullptr);
    vkDestroyBuffer(m_Device, staging_buffer, nullptr);
    vkFreeMemory(m_Device, staging_memory, nullptr);

    // ---- 4. Image view -----------------------------------------------
    VkImageViewCreateInfo view_ci {};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = m_PlaceholderImage;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.baseMipLevel = 0;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.baseArrayLayer = 0;
    view_ci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_Device, &view_ci, nullptr, &m_PlaceholderView) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkCreateImageView failed");
        return false;
    }

    // ---- 5. Sampler --------------------------------------------------
    // Linear-clamp, no anisotropy. Anisotropy needs the device
    // feature flag and we don't want to silently demand it; clamp +
    // linear is fine for a 1x1 texture anyway.
    VkSamplerCreateInfo sampler_ci {};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.mipLodBias = 0.0f;
    sampler_ci.anisotropyEnable = VK_FALSE;
    sampler_ci.maxAnisotropy = 1.0f;
    sampler_ci.compareEnable = VK_FALSE;
    sampler_ci.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_ci.minLod = 0.0f;
    sampler_ci.maxLod = 0.0f;
    sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler_ci.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(m_Device, &sampler_ci, nullptr, &m_PlaceholderSampler) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "Bindless placeholder: vkCreateSampler failed");
        return false;
    }

    // ---- 6. Write descriptor at slot 0 -------------------------------
    WriteDescriptorLocked(0, m_PlaceholderView, m_PlaceholderSampler);
    return true;
}

void VulkanBindlessTextureManager::DestroyPlaceholderLocked()
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return;
    }
    if (m_PlaceholderSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_Device, m_PlaceholderSampler, nullptr);
        m_PlaceholderSampler = VK_NULL_HANDLE;
    }
    if (m_PlaceholderView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_Device, m_PlaceholderView, nullptr);
        m_PlaceholderView = VK_NULL_HANDLE;
    }
    if (m_PlaceholderImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_Device, m_PlaceholderImage, nullptr);
        m_PlaceholderImage = VK_NULL_HANDLE;
    }
    if (m_PlaceholderMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_Device, m_PlaceholderMemory, nullptr);
        m_PlaceholderMemory = VK_NULL_HANDLE;
    }
}
