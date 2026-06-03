#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"

#include <array>
#include <unordered_map>
#include <vector>
#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

class VulkanUtil
{
public:
    static uint32_t
    FindMemoryType(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties_flag);
    static VkShaderModule CreateShaderModule(VkDevice device, const std::vector<unsigned char>& shader_code);
    static void CreateBuffer(VkPhysicalDevice physical_device,
                             VkDevice device,
                             VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags properties,
                             VkBuffer& buffer,
                             VkDeviceMemory& buffer_memory);
    static void CreateBufferAndInitialize(VkDevice device,
                                          VkPhysicalDevice physicalDevice,
                                          VkBufferUsageFlags usageFlags,
                                          VkMemoryPropertyFlags memoryPropertyFlags,
                                          VkBuffer* buffer,
                                          VkDeviceMemory* memory,
                                          VkDeviceSize size,
                                          void* data = nullptr,
                                          int datasize = 0);
    static void CopyBuffer(RHI* rhi,
                           VkBuffer srcBuffer,
                           VkBuffer dstBuffer,
                           VkDeviceSize srcOffset,
                           VkDeviceSize dstOffset,
                           VkDeviceSize size);
    static void CreateImage(VkPhysicalDevice physical_device,
                            VkDevice device,
                            uint32_t image_width,
                            uint32_t image_height,
                            VkFormat format,
                            VkImageTiling image_tiling,
                            VkImageUsageFlags image_usage_flags,
                            VkMemoryPropertyFlags memory_property_flags,
                            VkImage& image,
                            VkDeviceMemory& memory,
                            VkImageCreateFlags image_create_flags,
                            uint32_t array_layers,
                            uint32_t miplevels);
    static VkImageView CreateImageView(VkDevice device,
                                       VkImage& image,
                                       VkFormat format,
                                       VkImageAspectFlags image_aspect_flags,
                                       VkImageViewType view_type,
                                       uint32_t layout_count,
                                       uint32_t miplevels);
    static void CreateGlobalImage(RHI* rhi,
                                  VkImage& image,
                                  VkImageView& image_view,
                                  VmaAllocation image_allocation,
                                  uint32_t texture_image_width,
                                  uint32_t texture_image_height,
                                  void* texture_image_pixels,
                                  RHIFormat texture_image_format,
                                  uint32_t miplevels = 0);
    static void CreateCubeMap(RHI* rhi,
                              VkImage& image,
                              VkImageView& image_view,
                              VmaAllocation image_allocation,
                              uint32_t texture_image_width,
                              uint32_t texture_image_height,
                              std::array<void*, 6> texture_image_pixels,
                              RHIFormat texture_image_format,
                              uint32_t miplevels);
    static void GenerateTextureMipMaps(RHI* rhi,
                                       VkImage image,
                                       VkFormat image_format,
                                       uint32_t texture_width,
                                       uint32_t texture_height,
                                       uint32_t layers,
                                       uint32_t miplevels);
    static void TransitionImageLayout(RHI* rhi,
                                      VkImage image,
                                      VkImageLayout old_layout,
                                      VkImageLayout new_layout,
                                      uint32_t layer_count,
                                      uint32_t miplevels,
                                      VkImageAspectFlags aspect_mask_bits);
    static void
    CopyBufferToImage(RHI* rhi, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height, uint32_t layer_count);
    static void GenMipmappedImage(RHI* rhi, VkImage image, uint32_t width, uint32_t height, uint32_t mip_levels);

    static VkSampler
    GetOrCreateMipmapSampler(VkPhysicalDevice physical_device, VkDevice device, uint32_t width, uint32_t height);
    static void DestroyMipmappedSampler(VkDevice device);
    static VkSampler GetOrCreateNearestSampler(VkPhysicalDevice physical_device, VkDevice device);
    static VkSampler GetOrCreateLinearSampler(VkPhysicalDevice physical_device, VkDevice device);
    static void DestroyNearestSampler(VkDevice device);
    static void DestroyLinearSampler(VkDevice device);

private:
    static std::unordered_map<uint32_t, VkSampler> m_MipmapSamplerMap;
    static VkSampler m_NearestSampler;
    static VkSampler m_LinearSampler;
};