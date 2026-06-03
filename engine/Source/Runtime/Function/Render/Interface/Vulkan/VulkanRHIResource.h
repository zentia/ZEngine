#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"

#include <optional>
#include <vulkan/vulkan.h>

class VulkanBuffer : public RHIBuffer
{
public:
    void setResource(VkBuffer res) { m_Resource = res; }
    VkBuffer getResource() const { return m_Resource; }

private:
    VkBuffer m_Resource;
};
class VulkanBufferView : public RHIBufferView
{
public:
    void setResource(VkBufferView res) { m_Resource = res; }
    VkBufferView getResource() const { return m_Resource; }

private:
    VkBufferView m_Resource;
};
class VulkanCommandBuffer : public RHICommandBuffer
{
public:
    void setResource(VkCommandBuffer res) { m_Resource = res; }
    const VkCommandBuffer getResource() const { return m_Resource; }

private:
    VkCommandBuffer m_Resource;
};
class VulkanCommandPool : public RHICommandPool
{
public:
    void setResource(VkCommandPool res) { m_Resource = res; }
    VkCommandPool getResource() const { return m_Resource; }

private:
    VkCommandPool m_Resource;
};
class VulkanDescriptorPool : public RHIDescriptorPool
{
public:
    void setResource(VkDescriptorPool res) { m_Resource = res; }
    VkDescriptorPool getResource() const { return m_Resource; }

private:
    VkDescriptorPool m_Resource;
};
class VulkanDescriptorSet : public RHIDescriptorSet
{
public:
    void setResource(VkDescriptorSet res) { m_Resource = res; }
    VkDescriptorSet getResource() const { return m_Resource; }

private:
    VkDescriptorSet m_Resource;
};
class VulkanDescriptorSetLayout : public RHIDescriptorSetLayout
{
public:
    void setResource(VkDescriptorSetLayout res) { m_Resource = res; }
    VkDescriptorSetLayout getResource() const { return m_Resource; }

private:
    VkDescriptorSetLayout m_Resource;
};
class VulkanDevice : public RHIDevice
{
public:
    void setResource(VkDevice res) { m_Resource = res; }
    VkDevice getResource() const { return m_Resource; }

private:
    VkDevice m_Resource;
};
class VulkanDeviceMemory : public RHIDeviceMemory
{
public:
    void setResource(VkDeviceMemory res) { m_Resource = res; }
    VkDeviceMemory getResource() const { return m_Resource; }

private:
    VkDeviceMemory m_Resource;
};
class VulkanEvent : public RHIEvent
{
public:
    void setResource(VkEvent res) { m_Resource = res; }
    VkEvent getResource() const { return m_Resource; }

private:
    VkEvent m_Resource;
};
class VulkanFence : public RHIFence
{
public:
    void setResource(VkFence res) { m_Resource = res; }
    VkFence getResource() const { return m_Resource; }

private:
    VkFence m_Resource;
};
class VulkanFramebuffer : public RHIFramebuffer
{
public:
    void setResource(VkFramebuffer res) { m_Resource = res; }
    VkFramebuffer getResource() const { return m_Resource; }

private:
    VkFramebuffer m_Resource;
};
class VulkanImage : public RHIImage
{
public:
    void setResource(VkImage res) { m_Resource = res; }
    VkImage& getResource() { return m_Resource; }

private:
    VkImage m_Resource;
};
class VulkanImageView : public RHIImageView
{
public:
    void setResource(VkImageView res) { m_Resource = res; }
    VkImageView getResource() const { return m_Resource; }

private:
    VkImageView m_Resource;
};
class VulkanInstance : public RHIInstance
{
public:
    void setResource(VkInstance res) { m_Resource = res; }
    VkInstance getResource() const { return m_Resource; }

private:
    VkInstance m_Resource;
};
class VulkanQueue : public RHIQueue
{
public:
    void setResource(VkQueue res) { m_Resource = res; }
    VkQueue getResource() const { return m_Resource; }

private:
    VkQueue m_Resource;
};
class VulkanPhysicalDevice : public RHIPhysicalDevice
{
public:
    void setResource(VkPhysicalDevice res) { m_Resource = res; }
    VkPhysicalDevice getResource() const { return m_Resource; }

private:
    VkPhysicalDevice m_Resource;
};
class VulkanPipeline : public RHIPipeline
{
public:
    void setResource(VkPipeline res) { m_Resource = res; }
    VkPipeline getResource() const { return m_Resource; }

private:
    VkPipeline m_Resource;
};
class VulkanPipelineCache : public RHIPipelineCache
{
public:
    void setResource(VkPipelineCache res) { m_Resource = res; }
    VkPipelineCache getResource() const { return m_Resource; }

private:
    VkPipelineCache m_Resource;
};
class VulkanPipelineLayout : public RHIPipelineLayout
{
public:
    void setResource(VkPipelineLayout res) { m_Resource = res; }
    VkPipelineLayout getResource() const { return m_Resource; }

private:
    VkPipelineLayout m_Resource;
};
class VulkanRenderPass : public RHIRenderPass
{
public:
    void setResource(VkRenderPass res) { m_Resource = res; }
    VkRenderPass getResource() const { return m_Resource; }

private:
    VkRenderPass m_Resource;
};
class VulkanSampler : public RHISampler
{
public:
    void setResource(VkSampler res) { m_Resource = res; }
    VkSampler getResource() const { return m_Resource; }

private:
    VkSampler m_Resource;
};
class VulkanSemaphore : public RHISemaphore
{
public:
    void setResource(VkSemaphore res) { m_Resource = res; }
    VkSemaphore& getResource() { return m_Resource; }

private:
    VkSemaphore m_Resource;
};
class VulkanShader : public RHIShader
{
public:
    void setResource(VkShaderModule res) { m_Resource = res; }
    VkShaderModule getResource() const { return m_Resource; }

private:
    VkShaderModule m_Resource;
};