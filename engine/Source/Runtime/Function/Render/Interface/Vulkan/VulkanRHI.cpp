#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/ShaderCompiler.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanUtil.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Profiler/Profiler.h"
#include "Runtime/Project/ProjectInfo.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>

// https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html
#define ZENGINE_XSTR(s) ZENGINE_STR(s)
#define ZENGINE_STR(s)  #s

#if defined(__GNUC__)
    // https://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
    #if defined(__linux__)
        #include <stdlib.h>
    #elif defined(__MACH__)
    // https://developer.apple.com/library/archive/documentation/Porting/Conceptual/PortingUnix/compiling/compiling.html
        #include <stdlib.h>
    #else
        #error Unknown Platform
    #endif
#elif defined(_MSC_VER)
// https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros
    #include <sdkddkver.h>
    #define WIN32_LEAN_AND_MEAN 1
    #define NOGDICAPMASKS       1
    #define NOVIRTUALKEYCODES   1
    #define NOWINMESSAGES       1
    #define NOWINSTYLES         1
    #define NOSYSMETRICS        1
    #define NOMENUS             1
    #define NOICONS             1
    #define NOKEYSTATES         1
    #define NOSYSCOMMANDS       1
    #define NORASTEROPS         1
    #define NOSHOWWINDOW        1
    #define NOATOM              1
    #define NOCLIPBOARD         1
    #define NOCOLOR             1
    #define NOCTLMGR            1
    #define NODRAWTEXT          1
    #define NOGDI               1
    #define NOKERNEL            1
    #define NOUSER              1
    #define NONLS               1
    #define NOMB                1
    #define NOMEMMGR            1
    #define NOMETAFILE          1
    #define NOMINMAX            1
    #define NOMSG               1
    #define NOOPENFILE          1
    #define NOSCROLL            1
    #define NOSERVICE           1
    #define NOSOUND             1
    #define NOTEXTMETRIC        1
    #define NOWH                1
    #define NOWINOFFSETS        1
    #define NOCOMM              1
    #define NOKANJI             1
    #define NOHELP              1
    #define NOPROFILER          1
    #define NODEFERWINDOWPOS    1
    #define NOMCX               1
    #include <Windows.h>
#else
    #error Unknown Compiler
#endif

#include "Runtime/Core/Thread/ThreadManager.h"

#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

REGISTER_FACTORY(RHI, VulkanRHI, "VulkanRHI");
std::vector<std::type_index> VulkanRHI::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(WindowSystem), GET_SYSTEM_TYPE(ThreadManager)};
}
VulkanRHI::~VulkanRHI()
{
    // TODO
}

bool VulkanRHI::Initialize()
{
    // Wire up the engine-wide default SPIR-V on-disk cache directory
    // before any ShaderCompiler instance is constructed (VulkanRHI's
    // own m_ShaderCompiler is lazy-constructed in
    // createShaderModuleFromFile/Source; ShaderLab also owns one
    // internally for its SPIR-V emit stage). Pointing the default at
    // <project>/Intermediate/Shaders/ means every default-constructed
    // compiler picks up the same cache root automatically -- no
    // call-site changes needed. Mirrors DX12RHI::Initialize.
    //
    // ProjectInfo may be unavailable on the launcher / "no project
    // loaded" screen; in that case we leave caching disabled, which is
    // the pre-existing behaviour.
    if (auto project_info = GET_SYSTEM(ProjectInfo))
    {
        const std::filesystem::path interm_shaders = project_info->GetIntermediateShadersRoot();
        if (!interm_shaders.empty())
        {
            ShaderCompiler::SetDefaultCacheDirectory(interm_shaders);
        }
    }

    std::array<int, 2> window_size = GET_SYSTEM(WindowSystem)->GetWindowSize();
    for (int i = 0; i < std::size(m_Viewports); i++)
    {
        m_Viewports[i] = {0.0f, 0.0f, (float)window_size[0], (float)window_size[1], 0.0f, 1.0f};
    }

    for (int i = 0; i < std::size(m_Scissors); i++)
    {
        m_Scissors[i] = {{0, 0}, {(uint32_t)window_size[0], (uint32_t)window_size[1]}};
    }

#ifndef NDEBUG
    m_EnableValidationLayers = true;
    m_EnableDebugUtilsLabel = true;
#else
    // [TEMP-VALIDATION] forced ON for bindless tonemap RP1/RP2 split smoke run; revert before commit.
    m_EnableValidationLayers = true;
    m_EnableDebugUtilsLabel = true;
#endif

#if defined(__GNUC__) && defined(__MACH__)
    m_EnablePointLightShadow = false;
#else
    m_EnablePointLightShadow = true;
#endif

#if defined(__GNUC__)
    // https://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
    #if defined(__linux__)
    char const* vk_layer_path = ZENGINE_XSTR(ZENGINE_VK_LAYER_PATH);
    setenv("VK_LAYER_PATH", vk_layer_path, 1);
    #elif defined(__OHOS__)
    // OHOS uses the system Vulkan loader from the native SDK/sysroot.
    #elif defined(__MACH__)
    // https://developer.apple.com/library/archive/documentation/Porting/Conceptual/PortingUnix/compiling/compiling.html
    char const* vk_layer_path = ZENGINE_XSTR(ZENGINE_VK_LAYER_PATH);
    char const* vk_icd_filenames = ZENGINE_XSTR(ZENGINE_VK_ICD_FILENAMES);
    setenv("VK_LAYER_PATH", vk_layer_path, 1);
    setenv("VK_ICD_FILENAMES", vk_icd_filenames, 1);
    #else
        #error Unknown Platform
    #endif
#elif defined(_MSC_VER)
    // https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros
    // On Windows the Vulkan loader can discover layers from the system registration.
    // Avoid overriding VK_LAYER_PATH here because a stale SDK path can hide valid system layers.
    SetEnvironmentVariableA("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", "1");
#else
    #error Unknown Compiler
#endif

    if (m_EnableValidationLayers && !CheckValidationLayerSupport())
    {
        LOG_ERROR(ZVulkan,
                  "Validation layer '{}' is unavailable. Falling back to Vulkan without validation layers.",
                  m_ValidationLayers.front());

        m_EnableValidationLayers = false;
    }

    CreateInstance();

    InitializeDebugMessenger();

    CreateWindowSurface();

    InitializePhysicalDevice();

    CreateLogicalDevice();

    CreateCommandPool();

    CreateCommandBuffers();

    CreateDescriptorPool();

    CreateSyncPrimitives();

    CreateSwapchain();

    CreateSwapchainImageViews();

    CreateFramebufferImageAndView();

    CreateAssetAllocator();

    // -----------------------------------------------------------------
    // PR3: bring up the global bindless texture descriptor table once
    // the device is ready. Skipped silently if PR2 decided the platform
    // / driver cannot satisfy the descriptor-indexing feature bits;
    // higher-level code is expected to gate on
    // supportsBindlessTextures() before requesting a slot.
    // (Earlier patch for this block did not land -- restored here.)
    // -----------------------------------------------------------------
    if (m_BindlessSupported && m_MaxBindlessSampledImages > 0)
    {
        m_BindlessTextureManager = std::make_unique<VulkanBindlessTextureManager>();
        // PR5a: manager now owns its own slot-0 placeholder upload
        // (1x1 white), so it needs the physical device + graphics
        // queue + queue family on top of the logical device.
        VkQueue vk_graphics_queue = static_cast<VulkanQueue*>(m_GraphicsQueue)->getResource();
        const uint32_t graphics_family = m_QueueIndices.graphics_family.value();
        if (!m_BindlessTextureManager->Initialize(m_Device,
                                                  m_PhysicalDevice,
                                                  vk_graphics_queue,
                                                  graphics_family,
                                                  m_MaxBindlessSampledImages))
        {
            // Init failed at runtime (e.g. pool/set allocation rejected
            // by the driver). Demote: drop the manager and clear the
            // capability flag so callers fall back to the per-material
            // descriptor path.
            m_BindlessTextureManager.reset();
            m_BindlessSupported = false;
            LOG_WARNING(ZVulkan,
                        "Bindless texture manager init failed -- demoting bindless support to OFF");
        }
        else
        {
            LOG_INFO(ZVulkan,
                     "VulkanBindlessTextureManager: initialized (capacity = {}, slot 0 = white placeholder)",
                     m_MaxBindlessSampledImages);
        }
    }

    return true;
}

void VulkanRHI::PrepareContext()
{
    Z_PROFILE_SCOPE("VulkanRHI::prepareContext");
    m_VkCurrentCommandBuffer = m_VkCommandBuffers[m_CurrentFrameIndex];
    ((VulkanCommandBuffer*)m_CurrentCommandBuffer)->setResource(m_VkCurrentCommandBuffer);
}

void VulkanRHI::clear()
{
    // 等待设备空闲，确保所有正在进行的 Vulkan 操作都已完成
    // 这在线程关闭后调用，确保没有线程在使用 Vulkan 资源
    if (m_Device)
    {
        vkDeviceWaitIdle(m_Device);
    }

    // PR3: tear down the bindless table BEFORE the logical device is
    // destroyed -- the manager owns a VkDescriptorPool / Layout that
    // are children of m_Device.
    if (m_BindlessTextureManager)
    {
        m_BindlessTextureManager->Shutdown();
        m_BindlessTextureManager.reset();
    }

    if (m_EnableValidationLayers)
    {
        DestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
    }
}

void VulkanRHI::WaitForFences()
{
    VkResult res_wait_for_fences =
        _vkWaitForFences(m_Device, 1, &m_IsFrameInFlightFences[m_CurrentFrameIndex], VK_TRUE, UINT64_MAX);
    if (VK_SUCCESS != res_wait_for_fences)
    {
        LOG_ERROR(ZVulkan, "failed to synchronize!");
    }
}

bool VulkanRHI::WaitForFences(uint32_t fenceCount, const RHIFence* const* pFences, RHIBool32 waitAll, uint64_t timeout)
{
    // fence
    int fence_size = fenceCount;
    std::vector<VkFence> vk_fence_list(fence_size);
    for (int i = 0; i < fence_size; ++i)
    {
        const auto& rhi_fence_element = pFences[i];
        auto& vk_fence_element = vk_fence_list[i];

        vk_fence_element = ((VulkanFence*)rhi_fence_element)->getResource();
    };

    VkResult result = vkWaitForFences(m_Device, fenceCount, vk_fence_list.data(), waitAll, timeout);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "waitForFences failed");
        return false;
    }
}

void VulkanRHI::GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties* pProperties)
{
    VkPhysicalDeviceProperties vk_physical_device_properties;
    vkGetPhysicalDeviceProperties(m_PhysicalDevice, &vk_physical_device_properties);

    pProperties->apiVersion = vk_physical_device_properties.apiVersion;
    pProperties->driverVersion = vk_physical_device_properties.driverVersion;
    pProperties->vendorID = vk_physical_device_properties.vendorID;
    pProperties->deviceID = vk_physical_device_properties.deviceID;
    pProperties->deviceType = (RHIPhysicalDeviceType)vk_physical_device_properties.deviceType;
    for (uint32_t i = 0; i < RHI_MAX_PHYSICAL_DEVICE_NAME_SIZE; i++)
    {
        pProperties->deviceName[i] = vk_physical_device_properties.deviceName[i];
    }
    for (uint32_t i = 0; i < RHI_UUID_SIZE; i++)
    {
        pProperties->pipelineCacheUUID[i] = vk_physical_device_properties.pipelineCacheUUID[i];
    }
    pProperties->sparseProperties.residencyStandard2DBlockShape =
        (VkBool32)vk_physical_device_properties.sparseProperties.residencyStandard2DBlockShape;
    pProperties->sparseProperties.residencyStandard2DMultisampleBlockShape =
        (VkBool32)vk_physical_device_properties.sparseProperties.residencyStandard2DMultisampleBlockShape;
    pProperties->sparseProperties.residencyStandard3DBlockShape =
        (VkBool32)vk_physical_device_properties.sparseProperties.residencyStandard3DBlockShape;
    pProperties->sparseProperties.residencyAlignedMipSize =
        (VkBool32)vk_physical_device_properties.sparseProperties.residencyAlignedMipSize;
    pProperties->sparseProperties.residencyNonResidentStrict =
        (VkBool32)vk_physical_device_properties.sparseProperties.residencyNonResidentStrict;

    pProperties->limits.maxImageDimension1D = vk_physical_device_properties.limits.maxImageDimension1D;
    pProperties->limits.maxImageDimension2D = vk_physical_device_properties.limits.maxImageDimension2D;
    pProperties->limits.maxImageDimension3D = vk_physical_device_properties.limits.maxImageDimension3D;
    pProperties->limits.maxImageDimensionCube = vk_physical_device_properties.limits.maxImageDimensionCube;
    pProperties->limits.maxImageArrayLayers = vk_physical_device_properties.limits.maxImageArrayLayers;
    pProperties->limits.maxTexelBufferElements = vk_physical_device_properties.limits.maxTexelBufferElements;
    pProperties->limits.maxUniformBufferRange = vk_physical_device_properties.limits.maxUniformBufferRange;
    pProperties->limits.maxStorageBufferRange = vk_physical_device_properties.limits.maxStorageBufferRange;
    pProperties->limits.maxPushConstantsSize = vk_physical_device_properties.limits.maxPushConstantsSize;
    pProperties->limits.maxMemoryAllocationCount = vk_physical_device_properties.limits.maxMemoryAllocationCount;
    pProperties->limits.maxSamplerAllocationCount = vk_physical_device_properties.limits.maxSamplerAllocationCount;
    pProperties->limits.bufferImageGranularity =
        (VkDeviceSize)vk_physical_device_properties.limits.bufferImageGranularity;
    pProperties->limits.sparseAddressSpaceSize =
        (VkDeviceSize)vk_physical_device_properties.limits.sparseAddressSpaceSize;
    pProperties->limits.maxBoundDescriptorSets = vk_physical_device_properties.limits.maxBoundDescriptorSets;
    pProperties->limits.maxPerStageDescriptorSamplers =
        vk_physical_device_properties.limits.maxPerStageDescriptorSamplers;
    pProperties->limits.maxPerStageDescriptorUniformBuffers =
        vk_physical_device_properties.limits.maxPerStageDescriptorUniformBuffers;
    pProperties->limits.maxPerStageDescriptorStorageBuffers =
        vk_physical_device_properties.limits.maxPerStageDescriptorStorageBuffers;
    pProperties->limits.maxPerStageDescriptorSampledImages =
        vk_physical_device_properties.limits.maxPerStageDescriptorSampledImages;
    pProperties->limits.maxPerStageDescriptorStorageImages =
        vk_physical_device_properties.limits.maxPerStageDescriptorStorageImages;
    pProperties->limits.maxPerStageDescriptorInputAttachments =
        vk_physical_device_properties.limits.maxPerStageDescriptorInputAttachments;
    pProperties->limits.maxPerStageResources = vk_physical_device_properties.limits.maxPerStageResources;
    pProperties->limits.maxDescriptorSetSamplers = vk_physical_device_properties.limits.maxDescriptorSetSamplers;
    pProperties->limits.maxDescriptorSetUniformBuffers =
        vk_physical_device_properties.limits.maxDescriptorSetUniformBuffers;
    pProperties->limits.maxDescriptorSetUniformBuffersDynamic =
        vk_physical_device_properties.limits.maxDescriptorSetUniformBuffersDynamic;
    pProperties->limits.maxDescriptorSetStorageBuffers =
        vk_physical_device_properties.limits.maxDescriptorSetStorageBuffers;
    pProperties->limits.maxDescriptorSetStorageBuffersDynamic =
        vk_physical_device_properties.limits.maxDescriptorSetStorageBuffersDynamic;
    pProperties->limits.maxDescriptorSetSampledImages =
        vk_physical_device_properties.limits.maxDescriptorSetSampledImages;
    pProperties->limits.maxDescriptorSetStorageImages =
        vk_physical_device_properties.limits.maxDescriptorSetStorageImages;
    pProperties->limits.maxDescriptorSetInputAttachments =
        vk_physical_device_properties.limits.maxDescriptorSetInputAttachments;
    pProperties->limits.maxVertexInputAttributes = vk_physical_device_properties.limits.maxVertexInputAttributes;
    pProperties->limits.maxVertexInputBindings = vk_physical_device_properties.limits.maxVertexInputBindings;
    pProperties->limits.maxVertexInputAttributeOffset =
        vk_physical_device_properties.limits.maxVertexInputAttributeOffset;
    pProperties->limits.maxVertexInputBindingStride = vk_physical_device_properties.limits.maxVertexInputBindingStride;
    pProperties->limits.maxVertexOutputComponents = vk_physical_device_properties.limits.maxVertexOutputComponents;
    pProperties->limits.maxTessellationGenerationLevel =
        vk_physical_device_properties.limits.maxTessellationGenerationLevel;
    pProperties->limits.maxTessellationPatchSize = vk_physical_device_properties.limits.maxTessellationPatchSize;
    pProperties->limits.maxTessellationControlPerVertexInputComponents =
        vk_physical_device_properties.limits.maxTessellationControlPerVertexInputComponents;
    pProperties->limits.maxTessellationControlPerVertexOutputComponents =
        vk_physical_device_properties.limits.maxTessellationControlPerVertexOutputComponents;
    pProperties->limits.maxTessellationControlPerPatchOutputComponents =
        vk_physical_device_properties.limits.maxTessellationControlPerPatchOutputComponents;
    pProperties->limits.maxTessellationControlTotalOutputComponents =
        vk_physical_device_properties.limits.maxTessellationControlTotalOutputComponents;
    pProperties->limits.maxTessellationEvaluationInputComponents =
        vk_physical_device_properties.limits.maxTessellationEvaluationInputComponents;
    pProperties->limits.maxTessellationEvaluationOutputComponents =
        vk_physical_device_properties.limits.maxTessellationEvaluationOutputComponents;
    pProperties->limits.maxGeometryShaderInvocations =
        vk_physical_device_properties.limits.maxGeometryShaderInvocations;
    pProperties->limits.maxGeometryInputComponents = vk_physical_device_properties.limits.maxGeometryInputComponents;
    pProperties->limits.maxGeometryOutputComponents = vk_physical_device_properties.limits.maxGeometryOutputComponents;
    pProperties->limits.maxGeometryOutputVertices = vk_physical_device_properties.limits.maxGeometryOutputVertices;
    pProperties->limits.maxGeometryTotalOutputComponents =
        vk_physical_device_properties.limits.maxGeometryTotalOutputComponents;
    pProperties->limits.maxFragmentInputComponents = vk_physical_device_properties.limits.maxFragmentInputComponents;
    pProperties->limits.maxFragmentOutputAttachments =
        vk_physical_device_properties.limits.maxFragmentOutputAttachments;
    pProperties->limits.maxFragmentDualSrcAttachments =
        vk_physical_device_properties.limits.maxFragmentDualSrcAttachments;
    pProperties->limits.maxFragmentCombinedOutputResources =
        vk_physical_device_properties.limits.maxFragmentCombinedOutputResources;
    pProperties->limits.maxComputeSharedMemorySize = vk_physical_device_properties.limits.maxComputeSharedMemorySize;
    for (uint32_t i = 0; i < 3; i++)
    {
        pProperties->limits.maxComputeWorkGroupCount[i] =
            vk_physical_device_properties.limits.maxComputeWorkGroupCount[i];
    }
    pProperties->limits.maxComputeWorkGroupInvocations =
        vk_physical_device_properties.limits.maxComputeWorkGroupInvocations;
    for (uint32_t i = 0; i < 3; i++)
    {
        pProperties->limits.maxComputeWorkGroupSize[i] =
            vk_physical_device_properties.limits.maxComputeWorkGroupSize[i];
    }
    pProperties->limits.subPixelPrecisionBits = vk_physical_device_properties.limits.subPixelPrecisionBits;
    pProperties->limits.subTexelPrecisionBits = vk_physical_device_properties.limits.subTexelPrecisionBits;
    pProperties->limits.mipmapPrecisionBits = vk_physical_device_properties.limits.mipmapPrecisionBits;
    pProperties->limits.maxDrawIndexedIndexValue = vk_physical_device_properties.limits.maxDrawIndexedIndexValue;
    pProperties->limits.maxDrawIndirectCount = vk_physical_device_properties.limits.maxDrawIndirectCount;
    pProperties->limits.maxSamplerLodBias = vk_physical_device_properties.limits.maxSamplerLodBias;
    pProperties->limits.maxSamplerAnisotropy = vk_physical_device_properties.limits.maxSamplerAnisotropy;
    pProperties->limits.maxViewports = vk_physical_device_properties.limits.maxViewports;
    for (uint32_t i = 0; i < 2; i++)
    {
        pProperties->limits.maxViewportDimensions[i] = vk_physical_device_properties.limits.maxViewportDimensions[i];
    }
    for (uint32_t i = 0; i < 2; i++)
    {
        pProperties->limits.viewportBoundsRange[i] = vk_physical_device_properties.limits.viewportBoundsRange[i];
    }
    pProperties->limits.viewportSubPixelBits = vk_physical_device_properties.limits.viewportSubPixelBits;
    pProperties->limits.minMemoryMapAlignment = vk_physical_device_properties.limits.minMemoryMapAlignment;
    pProperties->limits.minTexelBufferOffsetAlignment =
        (VkDeviceSize)vk_physical_device_properties.limits.minTexelBufferOffsetAlignment;
    pProperties->limits.minUniformBufferOffsetAlignment =
        (VkDeviceSize)vk_physical_device_properties.limits.minUniformBufferOffsetAlignment;
    pProperties->limits.minStorageBufferOffsetAlignment =
        (VkDeviceSize)vk_physical_device_properties.limits.minStorageBufferOffsetAlignment;
    pProperties->limits.minTexelOffset = vk_physical_device_properties.limits.minTexelOffset;
    pProperties->limits.maxTexelOffset = vk_physical_device_properties.limits.maxTexelOffset;
    pProperties->limits.minTexelGatherOffset = vk_physical_device_properties.limits.minTexelGatherOffset;
    pProperties->limits.maxTexelGatherOffset = vk_physical_device_properties.limits.maxTexelGatherOffset;
    pProperties->limits.minInterpolationOffset = vk_physical_device_properties.limits.minInterpolationOffset;
    pProperties->limits.maxInterpolationOffset = vk_physical_device_properties.limits.maxInterpolationOffset;
    pProperties->limits.subPixelInterpolationOffsetBits =
        vk_physical_device_properties.limits.subPixelInterpolationOffsetBits;
    pProperties->limits.maxFramebufferWidth = vk_physical_device_properties.limits.maxFramebufferWidth;
    pProperties->limits.maxFramebufferHeight = vk_physical_device_properties.limits.maxFramebufferHeight;
    pProperties->limits.maxFramebufferLayers = vk_physical_device_properties.limits.maxFramebufferLayers;
    pProperties->limits.framebufferColorSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.framebufferColorSampleCounts;
    pProperties->limits.framebufferDepthSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.framebufferDepthSampleCounts;
    pProperties->limits.framebufferStencilSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.framebufferStencilSampleCounts;
    pProperties->limits.framebufferNoAttachmentsSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.framebufferNoAttachmentsSampleCounts;
    pProperties->limits.maxColorAttachments = vk_physical_device_properties.limits.maxColorAttachments;
    pProperties->limits.sampledImageColorSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.sampledImageColorSampleCounts;
    pProperties->limits.sampledImageIntegerSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.sampledImageIntegerSampleCounts;
    pProperties->limits.sampledImageDepthSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.sampledImageDepthSampleCounts;
    pProperties->limits.sampledImageStencilSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.sampledImageStencilSampleCounts;
    pProperties->limits.storageImageSampleCounts =
        (VkSampleCountFlags)vk_physical_device_properties.limits.storageImageSampleCounts;
    pProperties->limits.maxSampleMaskWords = vk_physical_device_properties.limits.maxSampleMaskWords;
    pProperties->limits.timestampComputeAndGraphics =
        (VkBool32)vk_physical_device_properties.limits.timestampComputeAndGraphics;
    pProperties->limits.timestampPeriod = vk_physical_device_properties.limits.timestampPeriod;
    pProperties->limits.maxClipDistances = vk_physical_device_properties.limits.maxClipDistances;
    pProperties->limits.maxCullDistances = vk_physical_device_properties.limits.maxCullDistances;
    pProperties->limits.maxCombinedClipAndCullDistances =
        vk_physical_device_properties.limits.maxCombinedClipAndCullDistances;
    pProperties->limits.discreteQueuePriorities = vk_physical_device_properties.limits.discreteQueuePriorities;
    for (uint32_t i = 0; i < 2; i++)
    {
        pProperties->limits.pointSizeRange[i] = vk_physical_device_properties.limits.pointSizeRange[i];
    }
    for (uint32_t i = 0; i < 2; i++)
    {
        pProperties->limits.lineWidthRange[i] = vk_physical_device_properties.limits.lineWidthRange[i];
    }
    pProperties->limits.pointSizeGranularity = vk_physical_device_properties.limits.pointSizeGranularity;
    pProperties->limits.lineWidthGranularity = vk_physical_device_properties.limits.lineWidthGranularity;
    pProperties->limits.strictLines = (VkBool32)vk_physical_device_properties.limits.strictLines;
    pProperties->limits.standardSampleLocations =
        (VkBool32)vk_physical_device_properties.limits.standardSampleLocations;
    pProperties->limits.optimalBufferCopyOffsetAlignment =
        (VkDeviceSize)vk_physical_device_properties.limits.optimalBufferCopyOffsetAlignment;
    pProperties->limits.optimalBufferCopyRowPitchAlignment =
        (VkDeviceSize)vk_physical_device_properties.limits.optimalBufferCopyRowPitchAlignment;
    pProperties->limits.nonCoherentAtomSize = (VkDeviceSize)vk_physical_device_properties.limits.nonCoherentAtomSize;
}

void VulkanRHI::ResetCommandPool()
{
    VkResult res_reset_command_pool = _vkResetCommandPool(m_Device, m_CommandPools[m_CurrentFrameIndex], 0);
    if (VK_SUCCESS != res_reset_command_pool)
    {
        LOG_ERROR(ZVulkan, "failed to synchronize");
    }
}

bool VulkanRHI::PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain)
{
    VkResult acquire_image_result =
        vkAcquireNextImageKHR(m_Device,
                              m_Swapchain,
                              UINT64_MAX,
                              m_ImageAvailableForRenderSemaphores[m_CurrentFrameIndex],
                              VK_NULL_HANDLE,
                              &m_CurrentSwapchainImageIndex);

    if (VK_ERROR_OUT_OF_DATE_KHR == acquire_image_result)
    {
        RecreateSwapchain();
        passUpdateAfterRecreateSwapchain();
        return RHI_SUCCESS;
    }
    else if (VK_SUBOPTIMAL_KHR == acquire_image_result)
    {
        RecreateSwapchain();
        passUpdateAfterRecreateSwapchain();

        // NULL submit to wait semaphore
        VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &m_ImageAvailableForRenderSemaphores[m_CurrentFrameIndex];
        submit_info.pWaitDstStageMask = wait_stages;
        submit_info.commandBufferCount = 0;
        submit_info.pCommandBuffers = NULL;
        submit_info.signalSemaphoreCount = 0;
        submit_info.pSignalSemaphores = NULL;

        VkResult res_reset_fences = _vkResetFences(m_Device, 1, &m_IsFrameInFlightFences[m_CurrentFrameIndex]);
        if (VK_SUCCESS != res_reset_fences)
        {
            LOG_ERROR(ZVulkan, "_vkResetFences failed!");
            return false;
        }

        VkResult res_queue_submit = vkQueueSubmit(((VulkanQueue*)m_GraphicsQueue)->getResource(),
                                                  1,
                                                  &submit_info,
                                                  m_IsFrameInFlightFences[m_CurrentFrameIndex]);
        if (VK_SUCCESS != res_queue_submit)
        {
            LOG_ERROR(ZVulkan, "vkQueueSubmit failed!");
            return false;
        }
        m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % k_max_frames_in_flight;
        return RHI_SUCCESS;
    }
    else
    {
        if (VK_SUCCESS != acquire_image_result)
        {
            LOG_ERROR(ZVulkan, "vkAcquireNextImageKHR failed!");
            return false;
        }
    }

    // begin command buffer
    VkCommandBufferBeginInfo command_buffer_begin_info {};
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.flags = 0;
    command_buffer_begin_info.pInheritanceInfo = nullptr;

    VkResult res_begin_command_buffer =
        _vkBeginCommandBuffer(m_VkCommandBuffers[m_CurrentFrameIndex], &command_buffer_begin_info);

    if (VK_SUCCESS != res_begin_command_buffer)
    {
        LOG_ERROR(ZVulkan, "_vkBeginCommandBuffer failed!");
        return false;
    }
    return false;
}

void VulkanRHI::SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain)
{
    // end command buffer
    VkResult res_end_command_buffer = _vkEndCommandBuffer(m_VkCommandBuffers[m_CurrentFrameIndex]);
    if (VK_SUCCESS != res_end_command_buffer)
    {
        LOG_ERROR(ZVulkan, "_vkEndCommandBuffer failed!");
        return;
    }

    VkSemaphore semaphores[2] = {
        ((VulkanSemaphore*)m_ImageAvailableForTexturescopySemaphores[m_CurrentFrameIndex])->getResource(),
        m_ImageFinishedForPresentationSemaphores[m_CurrentFrameIndex]};

    // submit command buffer
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &m_ImageAvailableForRenderSemaphores[m_CurrentFrameIndex];
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_VkCommandBuffers[m_CurrentFrameIndex];
    submit_info.signalSemaphoreCount = 2;
    submit_info.pSignalSemaphores = semaphores;

    VkResult res_reset_fences = _vkResetFences(m_Device, 1, &m_IsFrameInFlightFences[m_CurrentFrameIndex]);

    if (VK_SUCCESS != res_reset_fences)
    {
        LOG_ERROR(ZVulkan, "_vkResetFences failed!");
        return;
    }
    VkResult res_queue_submit = vkQueueSubmit(((VulkanQueue*)m_GraphicsQueue)->getResource(),
                                              1,
                                              &submit_info,
                                              m_IsFrameInFlightFences[m_CurrentFrameIndex]);

    if (VK_SUCCESS != res_queue_submit)
    {
        LOG_ERROR(ZVulkan, "vkQueueSubmit failed!");
        return;
    }

    // present swapchain
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &m_ImageFinishedForPresentationSemaphores[m_CurrentFrameIndex];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &m_Swapchain;
    present_info.pImageIndices = &m_CurrentSwapchainImageIndex;

    VkResult present_result = vkQueuePresentKHR(m_PresentQueue, &present_info);
    if (VK_ERROR_OUT_OF_DATE_KHR == present_result || VK_SUBOPTIMAL_KHR == present_result)
    {
        RecreateSwapchain();
        passUpdateAfterRecreateSwapchain();
    }
    else
    {
        if (VK_SUCCESS != present_result)
        {
            LOG_ERROR(ZVulkan, "vkQueuePresentKHR failed!");
            return;
        }
    }

    m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % k_max_frames_in_flight;
}

RHICommandBuffer* VulkanRHI::BeginSingleTimeCommands()
{
    VkCommandBufferAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = ((VulkanCommandPool*)m_RhiCommandPool)->getResource();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer command_buffer;
    vkAllocateCommandBuffers(m_Device, &allocInfo, &command_buffer);

    VkCommandBufferBeginInfo beginInfo {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    _vkBeginCommandBuffer(command_buffer, &beginInfo);

    RHICommandBuffer* rhi_command_buffer = new VulkanCommandBuffer();
    ((VulkanCommandBuffer*)rhi_command_buffer)->setResource(command_buffer);
    return rhi_command_buffer;
}

void VulkanRHI::EndSingleTimeCommands(RHICommandBuffer* command_buffer)
{
    VkCommandBuffer vk_command_buffer = ((VulkanCommandBuffer*)command_buffer)->getResource();
    _vkEndCommandBuffer(vk_command_buffer);

    VkSubmitInfo submitInfo {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vk_command_buffer;

    vkQueueSubmit(((VulkanQueue*)m_GraphicsQueue)->getResource(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(((VulkanQueue*)m_GraphicsQueue)->getResource());

    vkFreeCommandBuffers(m_Device, ((VulkanCommandPool*)m_RhiCommandPool)->getResource(), 1, &vk_command_buffer);
    delete (command_buffer);
}

// validation layers
bool VulkanRHI::CheckValidationLayerSupport()
{
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : m_ValidationLayers)
    {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers)
        {
            if (strcmp(layerName, layerProperties.layerName) == 0)
            {
                layerFound = true;
                break;
            }
        }

        if (!layerFound)
        {
            return false;
        }
    }

    return RHI_SUCCESS;
}

std::vector<const char*> VulkanRHI::GetRequiredExtensions()
{
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions;
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (m_EnableValidationLayers || m_EnableDebugUtilsLabel)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // VK_KHR_get_physical_device_properties2 is required to legally call
    // vkGetPhysicalDeviceFeatures2KHR / chain VkPhysicalDeviceDescriptorIndexingFeatures
    // through pNext on a Vulkan 1.0 instance. It was promoted to core in 1.1, but we
    // request it unconditionally here -- on 1.1+ loaders/drivers it's a harmless no-op
    // (the extension is always reported as available), on 1.0 it is mandatory.
    // Without this, vkGetPhysicalDeviceFeatures2KHR returns silently with the chained
    // struct untouched, which is exactly the "runtime=0 nonUniform=0 ..." symptom we
    // were hitting on the bindless path.
    extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    return extensions;
}

// debug callback
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                    void*)
{
    // [TEMP-VALIDATION] also dump to file because ZEditor is a /SUBSYSTEM:WINDOWS app and stderr is not connected.
    static FILE* s_ValidationLog = nullptr;
    if (s_ValidationLog == nullptr)
    {
        fopen_s(&s_ValidationLog, "e:/Engine/ZEngine/vk_validation.log", "w");
    }
    const char* sev = "INFO";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        sev = "ERROR";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        sev = "WARN";
    if (s_ValidationLog)
    {
        fprintf(s_ValidationLog, "[%s] %s\n", sev, pCallbackData->pMessage);
        fflush(s_ValidationLog);
    }
    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

void VulkanRHI::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo)
{
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
}

// Helper function to convert VkResult to string
static const char* VkResultToString(VkResult result)
{
    switch (result)
    {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_EVENT_SET:
            return "VK_EVENT_SET";
        case VK_EVENT_RESET:
            return "VK_EVENT_RESET";
        case VK_INCOMPLETE:
            return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:
            return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        default:
            return "UNKNOWN_VK_RESULT";
    }
}

void VulkanRHI::CreateInstance()
{
    LOG_INFO(ZVulkan, "========== Vulkan Instance Creation Diagnostics ==========");

    // ==================== Step 1: Check Vulkan API availability ====================
    LOG_INFO(ZVulkan, "[Step 1] Checking Vulkan API availability...");

    uint32_t apiVersion = VK_API_VERSION_1_0;
    auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));

    if (enumerateInstanceVersion)
    {
        VkResult vkEnumerateResult = enumerateInstanceVersion(&apiVersion);
        if (vkEnumerateResult != VK_SUCCESS)
        {
            LOG_ERROR(ZVulkan,
                      "  vkEnumerateInstanceVersion failed! VkResult = {}. Falling back to Vulkan 1.0.",
                      static_cast<int>(vkEnumerateResult));
            apiVersion = VK_API_VERSION_1_0;
        }
    }
    else
    {
        LOG_INFO(ZVulkan, "  vkEnumerateInstanceVersion unavailable, assuming Vulkan 1.0 loader");
    }

    // Cache the loader-supported instance API version. Used below when populating
    // VkApplicationInfo::apiVersion so we don't accidentally downgrade a 1.1+
    // loader to 1.0 (which would silently neuter VkPhysicalDeviceFeatures2 and
    // the descriptor-indexing pNext chain on the device-create path -- this was
    // exactly the "runtime=0 nonUniform=0 ..." symptom on the bindless path).
    m_VulkanApiVersion = apiVersion;

    LOG_INFO(ZVulkan,
             "  Vulkan API Version: {}.{}.{}",
             VK_VERSION_MAJOR(apiVersion),
             VK_VERSION_MINOR(apiVersion),
             VK_VERSION_PATCH(apiVersion));

    // ==================== Step 2: Check validation layers ====================
    LOG_INFO(ZVulkan, "[Step 2] Checking validation layers...");
    LOG_INFO(ZVulkan, "  Validation layers enabled: {}", m_EnableValidationLayers ? "YES" : "NO");

    if (m_EnableValidationLayers)
    {
        // Enumerate available layers
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        LOG_INFO(ZVulkan, "  Available layers ({}):", layerCount);
        for (const auto& layer : availableLayers)
        {
            LOG_INFO(ZVulkan,
                     "    - {} (spec version: {}.{}.{})",
                     layer.layerName,
                     VK_VERSION_MAJOR(layer.specVersion),
                     VK_VERSION_MINOR(layer.specVersion),
                     VK_VERSION_PATCH(layer.specVersion));
        }

        // Check requested layers
        LOG_INFO(ZVulkan, "  Requested layers ({}):", m_ValidationLayers.size());
        for (const char* layerName : m_ValidationLayers)
        {
            bool found = false;
            for (const auto& availableLayer : availableLayers)
            {
                if (strcmp(layerName, availableLayer.layerName) == 0)
                {
                    found = true;
                    break;
                }
            }
            if (found)
            {
                LOG_INFO(ZVulkan, "    - {} [OK]", layerName);
            }
            else
            {
                LOG_ERROR(ZVulkan, "    - {} [NOT FOUND] <-- This will cause VK_ERROR_LAYER_NOT_PRESENT!", layerName);
            }
        }
    }

    // ==================== Step 3: Check extensions ====================
    LOG_INFO(ZVulkan, "[Step 3] Checking instance extensions...");

    // Enumerate available extensions
    uint32_t availableExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availableExtCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(availableExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availableExtCount, availableExtensions.data());

    LOG_INFO(ZVulkan, "  Available extensions ({}):", availableExtCount);
    for (const auto& ext : availableExtensions)
    {
        LOG_INFO(ZVulkan, "    - {} (version: {})", ext.extensionName, ext.specVersion);
    }

    // Get required extensions
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    LOG_INFO(ZVulkan, "  GLFW required extensions ({}):", glfwExtensionCount);
    if (glfwExtensions == nullptr || glfwExtensionCount == 0)
    {
        LOG_ERROR(ZVulkan, "    glfwGetRequiredInstanceExtensions returned NULL or 0!");
        LOG_ERROR(ZVulkan, "    Possible causes:");
        LOG_ERROR(ZVulkan, "      - GLFW not initialized (glfwInit not called)");
        LOG_ERROR(ZVulkan, "      - GLFW compiled without Vulkan support");
        LOG_ERROR(ZVulkan, "      - Vulkan loader not found on system");
    }
    else
    {
        for (uint32_t i = 0; i < glfwExtensionCount; i++)
        {
            LOG_INFO(ZVulkan, "    - {}", glfwExtensions[i]);
        }
    }

    auto extensions = GetRequiredExtensions();
    LOG_INFO(ZVulkan, "  Total requested extensions ({}):", extensions.size());

    bool allExtensionsAvailable = true;
    for (const auto& reqExt : extensions)
    {
        bool found = false;
        for (const auto& availExt : availableExtensions)
        {
            if (strcmp(reqExt, availExt.extensionName) == 0)
            {
                found = true;
                break;
            }
        }
        if (found)
        {
            LOG_INFO(ZVulkan, "    - {} [OK]", reqExt);
        }
        else
        {
            LOG_ERROR(ZVulkan, "    - {} [NOT FOUND] <-- This will cause VK_ERROR_EXTENSION_NOT_PRESENT!", reqExt);
            allExtensionsAvailable = false;
        }
    }

    // ==================== Step 4: Create Instance ====================
    LOG_INFO(ZVulkan, "[Step 4] Creating Vulkan instance...");

    LOG_INFO(ZVulkan,
             "  Requesting API version: {}.{}.{}",
             VK_VERSION_MAJOR(m_VulkanApiVersion),
             VK_VERSION_MINOR(m_VulkanApiVersion),
             VK_VERSION_PATCH(m_VulkanApiVersion));

    // app info
    VkApplicationInfo appInfo {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = GET_SYSTEM(PlayerSettings)->m_ProjectName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "ZEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = m_VulkanApiVersion;

    LOG_INFO(ZVulkan, "  Application name: {}", appInfo.pApplicationName);

    // create info
    VkInstanceCreateInfo instance_create_info {};
    instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pApplicationInfo = &appInfo;
    instance_create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_create_info.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo {};
    if (m_EnableValidationLayers)
    {
        instance_create_info.enabledLayerCount = static_cast<uint32_t>(m_ValidationLayers.size());
        instance_create_info.ppEnabledLayerNames = m_ValidationLayers.data();

        PopulateDebugMessengerCreateInfo(debugCreateInfo);
        instance_create_info.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;

        LOG_INFO(ZVulkan, "  Enabled {} validation layer(s)", instance_create_info.enabledLayerCount);
    }
    else
    {
        instance_create_info.enabledLayerCount = 0;
        instance_create_info.pNext = nullptr;
    }

    LOG_INFO(ZVulkan, "  Enabled {} extension(s)", instance_create_info.enabledExtensionCount);

    // create m_VulkanContext._instance
    VkResult result = vkCreateInstance(&instance_create_info, nullptr, &m_Instance);

    if (result != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "========== vkCreateInstance FAILED ==========");
        LOG_ERROR(ZVulkan, "  VkResult = {} ({})", static_cast<int>(result), VkResultToString(result));
        LOG_ERROR(ZVulkan, "");
        LOG_ERROR(ZVulkan, "  Troubleshooting hints:");

        switch (result)
        {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                LOG_ERROR(ZVulkan, "  - System is out of host memory");
                break;
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                LOG_ERROR(ZVulkan, "  - GPU is out of device memory");
                break;
            case VK_ERROR_INITIALIZATION_FAILED:
                LOG_ERROR(ZVulkan, "  - Vulkan initialization failed");
                LOG_ERROR(ZVulkan, "  - Check if Vulkan runtime is properly installed");
                LOG_ERROR(ZVulkan, "  - Try updating your GPU drivers");
                break;
            case VK_ERROR_LAYER_NOT_PRESENT:
                LOG_ERROR(ZVulkan, "  - One or more requested layers are not available");
                LOG_ERROR(ZVulkan, "  - Install Vulkan SDK or disable validation layers");
                break;
            case VK_ERROR_EXTENSION_NOT_PRESENT:
                LOG_ERROR(ZVulkan, "  - One or more requested extensions are not available");
                LOG_ERROR(ZVulkan, "  - Check the extension list above for [NOT FOUND] items");
                break;
            case VK_ERROR_INCOMPATIBLE_DRIVER:
                LOG_ERROR(ZVulkan, "  - GPU driver is incompatible with Vulkan");
                LOG_ERROR(ZVulkan, "  - Update your GPU drivers to the latest version");
                LOG_ERROR(ZVulkan, "  - Ensure your GPU supports Vulkan");
                break;
            default:
                LOG_ERROR(ZVulkan, "  - Unknown error, please check Vulkan documentation");
                break;
        }

        LOG_FATAL(ZVulkan, "vk create instance failed! VkResult = {}", static_cast<int>(result));
    }
    else
    {
        LOG_INFO(ZVulkan, "========== vkCreateInstance SUCCESS ==========");
    }
}

void VulkanRHI::InitializeDebugMessenger()
{
    if (m_EnableValidationLayers)
    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        PopulateDebugMessengerCreateInfo(createInfo);
        if (VK_SUCCESS != CreateDebugUtilsMessengerEXT(m_Instance, &createInfo, nullptr, &m_DebugMessenger))
        {
            LOG_ERROR(ZVulkan, "failed to set up debug messenger!");
        }
    }

    if (m_EnableDebugUtilsLabel)
    {
        _vkCmdBeginDebugUtilsLabelEXT =
            (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(m_Instance, "vkCmdBeginDebugUtilsLabelEXT");
        _vkCmdEndDebugUtilsLabelEXT =
            (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(m_Instance, "vkCmdEndDebugUtilsLabelEXT");
    }
}

void VulkanRHI::CreateWindowSurface()
{
    if (glfwCreateWindowSurface(m_Instance, GET_SYSTEM(WindowSystem)->GetWindow(), nullptr, &m_Surface) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "glfwCreateWindowSurface failed!");
    }
}

void VulkanRHI::InitializePhysicalDevice()
{
    uint32_t physical_device_count;
    vkEnumeratePhysicalDevices(m_Instance, &physical_device_count, nullptr);
    if (physical_device_count == 0)
    {
        LOG_ERROR(ZVulkan, "enumerate physical devices failed!");
    }
    else
    {
        // find one device that matches our requirement
        // or find which is the best
        std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
        vkEnumeratePhysicalDevices(m_Instance, &physical_device_count, physical_devices.data());

        std::vector<std::pair<int, VkPhysicalDevice>> ranked_physical_devices;
        for (const auto& device : physical_devices)
        {
            VkPhysicalDeviceProperties physical_device_properties;
            vkGetPhysicalDeviceProperties(device, &physical_device_properties);
            int score = 0;

            if (physical_device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                score += 1000;
            }
            else if (physical_device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            {
                score += 100;
            }

            ranked_physical_devices.push_back({score, device});
        }

        std::sort(ranked_physical_devices.begin(),
                  ranked_physical_devices.end(),
                  [](const std::pair<int, VkPhysicalDevice>& p1, const std::pair<int, VkPhysicalDevice>& p2) {
                      return p1 > p2;
                  });

        for (const auto& device : ranked_physical_devices)
        {
            if (IsDeviceSuitable(device.second))
            {
                m_PhysicalDevice = device.second;
                break;
            }
        }

        if (m_PhysicalDevice == VK_NULL_HANDLE)
        {
            LOG_FATAL(ZVulkan, "failed to find suitable physical device");
        }
    }
}

// logical device (m_VulkanContext._device : graphic queue, present queue,
// feature:samplerAnisotropy)
void VulkanRHI::CreateLogicalDevice()
{
    m_QueueIndices = FindQueueFamilies(m_PhysicalDevice);

    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;  // all queues that need to be created
    std::set<uint32_t> queue_families = {m_QueueIndices.graphics_family.value(),
                                         m_QueueIndices.present_family.value(),
                                         m_QueueIndices.m_ComputeFamily.value()};

    float queue_priority = 1.0f;
    for (uint32_t queue_family : queue_families)  // for every queue family
    {
        // queue create info
        VkDeviceQueueCreateInfo queue_create_info {};
        queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = queue_family;
        queue_create_info.queueCount = 1;
        queue_create_info.pQueuePriorities = &queue_priority;
        queue_create_infos.push_back(queue_create_info);
    }

    // physical device features
    VkPhysicalDeviceFeatures physical_device_features = {};

    physical_device_features.samplerAnisotropy = VK_TRUE;

    // support inefficient readback storage buffer
    physical_device_features.fragmentStoresAndAtomics = VK_TRUE;

    // support independent blending
    physical_device_features.independentBlend = VK_TRUE;
    physical_device_features.multiViewport = VK_TRUE;

    // support geometry shader
    if (m_EnablePointLightShadow)
    {
        physical_device_features.geometryShader = VK_TRUE;
    }

    // -----------------------------------------------------------------
    // Optional: VK_EXT_descriptor_indexing (bindless).
    //
    // Strategy for Android / HarmonyOS:
    //   1. probe the extension list of the chosen physical device;
    //   2. if available, query VkPhysicalDeviceDescriptorIndexingFeatures
    //      via vkGetPhysicalDeviceFeatures2 (also exposed via the
    //      VK_KHR_get_physical_device_properties2 instance extension on
    //      Vulkan 1.0 contexts);
    //   3. only enable the device extension AND set m_BindlessSupported
    //      when the minimum required feature bits are reported.
    //
    // This keeps the engine working on legacy drivers where the
    // extension is missing or partially implemented.
    // -----------------------------------------------------------------
    {
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> available_exts(ext_count);
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &ext_count, available_exts.data());

        const std::string descriptor_indexing_name = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
        const std::string maintenance3_name = VK_KHR_MAINTENANCE3_EXTENSION_NAME;

        bool has_descriptor_indexing = false;
        bool has_maintenance3 = false;
        for (const auto& ext : available_exts)
        {
            if (descriptor_indexing_name == ext.extensionName)
            {
                has_descriptor_indexing = true;
            }
            if (maintenance3_name == ext.extensionName)
            {
                has_maintenance3 = true;
            }
        }

        // VK_EXT_descriptor_indexing depends on VK_KHR_maintenance3.
        m_DescriptorIndexingExtensionAvailable = has_descriptor_indexing && has_maintenance3;
    }

    VkPhysicalDeviceDescriptorIndexingFeaturesEXT indexing_features {};
    indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;

    if (m_DescriptorIndexingExtensionAvailable)
    {
        // query supported feature bits via Features2
        auto vkGetPhysicalDeviceFeatures2KHR_fn =
            (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceFeatures2KHR");
        if (vkGetPhysicalDeviceFeatures2KHR_fn == nullptr)
        {
            // try the core 1.1+ entry point name
            vkGetPhysicalDeviceFeatures2KHR_fn =
                (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceFeatures2");
        }

        if (vkGetPhysicalDeviceFeatures2KHR_fn)
        {
            VkPhysicalDeviceFeatures2 feats2 {};
            feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            feats2.pNext = &indexing_features;
            vkGetPhysicalDeviceFeatures2KHR_fn(m_PhysicalDevice, &feats2);

            // Minimum subset we require for a bindless texture array:
            //   - runtimeDescriptorArray
            //   - shaderSampledImageArrayNonUniformIndexing
            //   - descriptorBindingPartiallyBound
            //   - descriptorBindingSampledImageUpdateAfterBind
            //   - descriptorBindingVariableDescriptorCount
            const bool min_required = indexing_features.runtimeDescriptorArray &&
                                      indexing_features.shaderSampledImageArrayNonUniformIndexing &&
                                      indexing_features.descriptorBindingPartiallyBound &&
                                      indexing_features.descriptorBindingSampledImageUpdateAfterBind &&
                                      indexing_features.descriptorBindingVariableDescriptorCount;

            if (min_required)
            {
                m_BindlessSupported = true;

                // keep only the bits we actually need; clear everything else
                // to avoid validation warnings about features the driver
                // technically reports but we never asked for.
                VkPhysicalDeviceDescriptorIndexingFeaturesEXT enabled {};
                enabled.sType = indexing_features.sType;
                enabled.runtimeDescriptorArray = VK_TRUE;
                enabled.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
                enabled.descriptorBindingPartiallyBound = VK_TRUE;
                enabled.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                enabled.descriptorBindingVariableDescriptorCount = VK_TRUE;
                // these are nice-to-have, only enable if the driver supports them
                if (indexing_features.descriptorBindingUpdateUnusedWhilePending)
                {
                    enabled.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
                }
                if (indexing_features.shaderStorageBufferArrayNonUniformIndexing)
                {
                    enabled.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
                }
                indexing_features = enabled;

                // append the two extensions (maintenance3 first; order does
                // not strictly matter but keeps drivers happy).
                m_DeviceExtensions.push_back(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
                m_DeviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);

                // Reasonable default cap; the bindless table size is later
                // chosen by the high-level texture manager and clamped to
                // maxPerStageDescriptorUpdateAfterBindSampledImages.
                m_MaxBindlessSampledImages = 4096;
                m_MaxBindlessStorageBuffers = 256;

                LOG_INFO(ZVulkan, "Bindless descriptor indexing: ENABLED (max sampled images = {})", m_MaxBindlessSampledImages);
            }
            else
            {
                LOG_WARNING(ZVulkan,
                            "VK_EXT_descriptor_indexing present but required feature bits "
                            "missing: runtime={} nonUniform={} partial={} uab={} varCount={} -> bindless DISABLED",
                            (int)indexing_features.runtimeDescriptorArray,
                            (int)indexing_features.shaderSampledImageArrayNonUniformIndexing,
                            (int)indexing_features.descriptorBindingPartiallyBound,
                            (int)indexing_features.descriptorBindingSampledImageUpdateAfterBind,
                            (int)indexing_features.descriptorBindingVariableDescriptorCount);
            }
        }
        else
        {
            LOG_WARNING(ZVulkan,
                        "VK_EXT_descriptor_indexing extension present but vkGetPhysicalDeviceFeatures2 "
                        "is unavailable -> bindless DISABLED");
        }
    }
    else
    {
        LOG_INFO(ZVulkan, "VK_EXT_descriptor_indexing not available on this device -> bindless DISABLED");
    }

    // device create info
    VkDeviceCreateInfo device_create_info {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.pQueueCreateInfos = queue_create_infos.data();
    device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    device_create_info.pEnabledFeatures = &physical_device_features;
    device_create_info.enabledExtensionCount = static_cast<uint32_t>(m_DeviceExtensions.size());
    device_create_info.ppEnabledExtensionNames = m_DeviceExtensions.data();
    device_create_info.enabledLayerCount = 0;
    if (m_BindlessSupported)
    {
        // Chain descriptor-indexing features into the Device create chain.
        // pEnabledFeatures stays non-null (legacy core features); the
        // indexing struct goes through pNext only.
        device_create_info.pNext = &indexing_features;
    }

    if (vkCreateDevice(m_PhysicalDevice, &device_create_info, nullptr, &m_Device) != VK_SUCCESS)
    {
        LOG_FATAL(ZVulkan, "vk create device");
    }

    // initialize queues of this device
    VkQueue vk_graphics_queue;
    vkGetDeviceQueue(m_Device, m_QueueIndices.graphics_family.value(), 0, &vk_graphics_queue);
    m_GraphicsQueue = new VulkanQueue();
    ((VulkanQueue*)m_GraphicsQueue)->setResource(vk_graphics_queue);

    vkGetDeviceQueue(m_Device, m_QueueIndices.present_family.value(), 0, &m_PresentQueue);

    VkQueue vk_compute_queue;
    vkGetDeviceQueue(m_Device, m_QueueIndices.m_ComputeFamily.value(), 0, &vk_compute_queue);
    m_ComputeQueue = new VulkanQueue();
    ((VulkanQueue*)m_ComputeQueue)->setResource(vk_compute_queue);

    // more efficient pointer
    _vkResetCommandPool = (PFN_vkResetCommandPool)vkGetDeviceProcAddr(m_Device, "vkResetCommandPool");
    _vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)vkGetDeviceProcAddr(m_Device, "vkBeginCommandBuffer");
    _vkEndCommandBuffer = (PFN_vkEndCommandBuffer)vkGetDeviceProcAddr(m_Device, "vkEndCommandBuffer");
    _vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)vkGetDeviceProcAddr(m_Device, "vkCmdBeginRenderPass");
    _vkCmdNextSubpass = (PFN_vkCmdNextSubpass)vkGetDeviceProcAddr(m_Device, "vkCmdNextSubpass");
    _vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass)vkGetDeviceProcAddr(m_Device, "vkCmdEndRenderPass");
    _vkCmdBindPipeline = (PFN_vkCmdBindPipeline)vkGetDeviceProcAddr(m_Device, "vkCmdBindPipeline");
    _vkCmdSetScissor = (PFN_vkCmdSetScissor)vkGetDeviceProcAddr(m_Device, "vkCmdSetScissor");
    _vkWaitForFences = (PFN_vkWaitForFences)vkGetDeviceProcAddr(m_Device, "vkWaitForFences");
    _vkResetFences = (PFN_vkResetFences)vkGetDeviceProcAddr(m_Device, "vkResetFences");
    _vkCmdDrawIndexed = (PFN_vkCmdDrawIndexed)vkGetDeviceProcAddr(m_Device, "vkCmdDrawIndexed");
    _vkCmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers)vkGetDeviceProcAddr(m_Device, "vkCmdBindVertexBuffers");
    _vkCmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer)vkGetDeviceProcAddr(m_Device, "vkCmdBindIndexBuffer");
    _vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)vkGetDeviceProcAddr(m_Device, "vkCmdBindDescriptorSets");
    _vkCmdClearAttachments = (PFN_vkCmdClearAttachments)vkGetDeviceProcAddr(m_Device, "vkCmdClearAttachments");

    m_DepthImageFormat = (RHIFormat)FindDepthFormat();
}

void VulkanRHI::CreateCommandPool()
{
    // default graphics command pool
    {
        m_RhiCommandPool = new VulkanCommandPool();
        VkCommandPool vk_command_pool;
        VkCommandPoolCreateInfo command_pool_create_info {};
        command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_create_info.pNext = NULL;
        command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_create_info.queueFamilyIndex = m_QueueIndices.graphics_family.value();

        if (vkCreateCommandPool(m_Device, &command_pool_create_info, nullptr, &vk_command_pool) != VK_SUCCESS)
        {
            LOG_ERROR(ZVulkan, "vk create command pool");
        }

        ((VulkanCommandPool*)m_RhiCommandPool)->setResource(vk_command_pool);
    }

    // other command pools
    {
        VkCommandPoolCreateInfo command_pool_create_info;
        command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_create_info.pNext = NULL;
        command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        command_pool_create_info.queueFamilyIndex = m_QueueIndices.graphics_family.value();

        for (uint32_t i = 0; i < k_max_frames_in_flight; ++i)
        {
            if (vkCreateCommandPool(m_Device, &command_pool_create_info, NULL, &m_CommandPools[i]) != VK_SUCCESS)
            {
                LOG_ERROR(ZVulkan, "vk create command pool");
            }
        }
    }
}

bool VulkanRHI::CreateCommandPool(const RHICommandPoolCreateInfo* pCreateInfo, RHICommandPool*& pCommandPool)
{
    VkCommandPoolCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkCommandPoolCreateFlags)pCreateInfo->flags;
    create_info.queueFamilyIndex = pCreateInfo->queueFamilyIndex;

    pCommandPool = new VulkanCommandPool();
    VkCommandPool vk_commandPool;
    VkResult result = vkCreateCommandPool(m_Device, &create_info, nullptr, &vk_commandPool);
    ((VulkanCommandPool*)pCommandPool)->setResource(vk_commandPool);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateCommandPool is failed!");
        return false;
    }
}

bool VulkanRHI::CreateDescriptorPool(const RHIDescriptorPoolCreateInfo* pCreateInfo,
                                     RHIDescriptorPool*& pDescriptorPool)
{
    int size = pCreateInfo->poolSizeCount;
    std::vector<VkDescriptorPoolSize> descriptor_pool_size(size);
    for (int i = 0; i < size; ++i)
    {
        const auto& rhi_desc = pCreateInfo->pPoolSizes[i];
        auto& vk_desc = descriptor_pool_size[i];

        vk_desc.type = (VkDescriptorType)rhi_desc.type;
        vk_desc.descriptorCount = rhi_desc.descriptorCount;
    };

    VkDescriptorPoolCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkDescriptorPoolCreateFlags)pCreateInfo->flags;
    create_info.maxSets = pCreateInfo->maxSets;
    create_info.poolSizeCount = pCreateInfo->poolSizeCount;
    create_info.pPoolSizes = descriptor_pool_size.data();

    // Bindless safety: silently strip UPDATE_AFTER_BIND_BIT on devices
    // without descriptor_indexing, so callers asking for bindless pools on
    // legacy hardware get a working (bindful) pool instead of VK_ERROR.
    if ((create_info.flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) && !m_BindlessSupported)
    {
        LOG_WARNING(ZVulkan,
                    "Descriptor pool requested UPDATE_AFTER_BIND_BIT but bindless is not supported; "
                    "the bit is being stripped to keep pool creation compatible.");
        create_info.flags &= ~VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    }

    pDescriptorPool = new VulkanDescriptorPool();
    VkDescriptorPool vk_descriptorPool;
    VkResult result = vkCreateDescriptorPool(m_Device, &create_info, nullptr, &vk_descriptorPool);
    ((VulkanDescriptorPool*)pDescriptorPool)->setResource(vk_descriptorPool);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateDescriptorPool is failed!");
        return false;
    }
}

bool VulkanRHI::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo* pCreateInfo,
                                          RHIDescriptorSetLayout*& pSetLayout)
{
    // descriptor_set_layout_binding
    int descriptor_set_layout_binding_size = pCreateInfo->bindingCount;
    std::vector<VkDescriptorSetLayoutBinding> vk_descriptor_set_layout_binding_list(descriptor_set_layout_binding_size);

    int sampler_count = 0;
    for (int i = 0; i < descriptor_set_layout_binding_size; ++i)
    {
        const auto& rhi_descriptor_set_layout_binding_element = pCreateInfo->pBindings[i];
        if (rhi_descriptor_set_layout_binding_element.pImmutableSamplers != nullptr)
        {
            sampler_count += rhi_descriptor_set_layout_binding_element.descriptorCount;
        }
    }
    std::vector<VkSampler> sampler_list(sampler_count);
    int sampler_current = 0;

    for (int i = 0; i < descriptor_set_layout_binding_size; ++i)
    {
        const auto& rhi_descriptor_set_layout_binding_element = pCreateInfo->pBindings[i];
        auto& vk_descriptor_set_layout_binding_element = vk_descriptor_set_layout_binding_list[i];

        // sampler
        vk_descriptor_set_layout_binding_element.pImmutableSamplers = nullptr;
        if (rhi_descriptor_set_layout_binding_element.pImmutableSamplers)
        {
            vk_descriptor_set_layout_binding_element.pImmutableSamplers = &sampler_list[sampler_current];
            for (int i = 0; i < rhi_descriptor_set_layout_binding_element.descriptorCount; ++i)
            {
                const auto& rhi_sampler_element = rhi_descriptor_set_layout_binding_element.pImmutableSamplers[i];
                auto& vk_sampler_element = sampler_list[sampler_current];

                vk_sampler_element = ((VulkanSampler*)rhi_sampler_element)->getResource();

                sampler_current++;
            };
        }
        vk_descriptor_set_layout_binding_element.binding = rhi_descriptor_set_layout_binding_element.binding;
        vk_descriptor_set_layout_binding_element.descriptorType =
            (VkDescriptorType)rhi_descriptor_set_layout_binding_element.descriptorType;
        vk_descriptor_set_layout_binding_element.descriptorCount =
            rhi_descriptor_set_layout_binding_element.descriptorCount;
        vk_descriptor_set_layout_binding_element.stageFlags = rhi_descriptor_set_layout_binding_element.stageFlags;
    };

    if (sampler_count != sampler_current)
    {
        LOG_ERROR(ZVulkan, "sampler_count != sampller_current");
        return false;
    }

    // -----------------------------------------------------------------
    // Bindless: gather per-binding flags. If any binding requested any
    // RHI_DESCRIPTOR_BINDING_* bit, build a
    // VkDescriptorSetLayoutBindingFlagsCreateInfo and chain it through
    // pNext. Vectors must outlive the vkCreateDescriptorSetLayout call.
    // -----------------------------------------------------------------
    std::vector<VkDescriptorBindingFlags> vk_binding_flags;
    bool any_binding_flag_set = false;
    for (int i = 0; i < descriptor_set_layout_binding_size; ++i)
    {
        if (pCreateInfo->pBindings[i].bindingFlags != 0)
        {
            any_binding_flag_set = true;
            break;
        }
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT binding_flags_info {};
    if (any_binding_flag_set)
    {
        if (!m_BindlessSupported)
        {
            LOG_WARNING(ZVulkan,
                        "Descriptor set layout requested bindless flags but device does not support "
                        "VK_EXT_descriptor_indexing; flags will be ignored.");
        }
        else
        {
            vk_binding_flags.resize(descriptor_set_layout_binding_size);
            for (int i = 0; i < descriptor_set_layout_binding_size; ++i)
            {
                vk_binding_flags[i] =
                    static_cast<VkDescriptorBindingFlags>(pCreateInfo->pBindings[i].bindingFlags);
            }

            binding_flags_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
            binding_flags_info.bindingCount = static_cast<uint32_t>(vk_binding_flags.size());
            binding_flags_info.pBindingFlags = vk_binding_flags.data();
        }
    }

    VkDescriptorSetLayoutCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkDescriptorSetLayoutCreateFlags)pCreateInfo->flags;
    create_info.bindingCount = pCreateInfo->bindingCount;
    create_info.pBindings = vk_descriptor_set_layout_binding_list.data();

    if (!vk_binding_flags.empty())
    {
        // splice the bindless extension struct into the head of the chain;
        // the user-supplied pNext (usually nullptr) is preserved after it.
        binding_flags_info.pNext = create_info.pNext;
        create_info.pNext = &binding_flags_info;
    }

    // Strip UPDATE_AFTER_BIND_POOL_BIT on devices without bindless support,
    // mirroring what we do in CreateDescriptorPool().
    if ((create_info.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT) &&
        !m_BindlessSupported)
    {
        create_info.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT;
    }

    pSetLayout = new VulkanDescriptorSetLayout();
    VkDescriptorSetLayout vk_descriptorSetLayout;
    VkResult result = vkCreateDescriptorSetLayout(m_Device, &create_info, nullptr, &vk_descriptorSetLayout);
    ((VulkanDescriptorSetLayout*)pSetLayout)->setResource(vk_descriptorSetLayout);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateDescriptorSetLayout failed!");
        return false;
    }
}

bool VulkanRHI::CreateFence(const RHIFenceCreateInfo* pCreateInfo, RHIFence*& pFence)
{
    VkFenceCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkFenceCreateFlags)pCreateInfo->flags;

    pFence = new VulkanFence();
    VkFence vk_fence;
    VkResult result = vkCreateFence(m_Device, &create_info, nullptr, &vk_fence);
    ((VulkanFence*)pFence)->setResource(vk_fence);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateFence failed!");
        return false;
    }
}

bool VulkanRHI::CreateFramebuffer(const RHIFramebufferCreateInfo* pCreateInfo, RHIFramebuffer*& pFramebuffer)
{
    // image_view
    int image_view_size = pCreateInfo->attachmentCount;
    std::vector<VkImageView> vk_image_view_list(image_view_size);
    for (int i = 0; i < image_view_size; ++i)
    {
        const auto& rhi_image_view_element = pCreateInfo->pAttachments[i];
        auto& vk_image_view_element = vk_image_view_list[i];

        vk_image_view_element = ((VulkanImageView*)rhi_image_view_element)->getResource();
    };

    VkFramebufferCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkFramebufferCreateFlags)pCreateInfo->flags;
    create_info.renderPass = ((VulkanRenderPass*)pCreateInfo->renderPass)->getResource();
    create_info.attachmentCount = pCreateInfo->attachmentCount;
    create_info.pAttachments = vk_image_view_list.data();
    create_info.width = pCreateInfo->width;
    create_info.height = pCreateInfo->height;
    create_info.layers = pCreateInfo->layers;

    pFramebuffer = new VulkanFramebuffer();
    VkFramebuffer vk_framebuffer;
    VkResult result = vkCreateFramebuffer(m_Device, &create_info, nullptr, &vk_framebuffer);
    ((VulkanFramebuffer*)pFramebuffer)->setResource(vk_framebuffer);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateFramebuffer failed!");
        return false;
    }
}

bool VulkanRHI::CreateGraphicsPipelines(RHIPipelineCache* pipelineCache,
                                        uint32_t createInfoCount,
                                        const RHIGraphicsPipelineCreateInfo* pCreateInfo,
                                        RHIPipeline*& pPipelines)
{
    // pipeline_shader_stage_create_info
    int pipeline_shader_stage_create_info_size = pCreateInfo->stageCount;
    std::vector<VkPipelineShaderStageCreateInfo> vk_pipeline_shader_stage_create_info_list(
        pipeline_shader_stage_create_info_size);

    int specialization_map_entry_size_total = 0;
    int specialization_info_total = 0;
    for (int i = 0; i < pipeline_shader_stage_create_info_size; ++i)
    {
        const auto& rhi_pipeline_shader_stage_create_info_element = pCreateInfo->pStages[i];
        if (rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo != nullptr)
        {
            specialization_info_total++;
            specialization_map_entry_size_total +=
                rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo->mapEntryCount;
        }
    }
    std::vector<VkSpecializationInfo> vk_specialization_info_list(specialization_info_total);
    std::vector<VkSpecializationMapEntry> vk_specialization_map_entry_list(specialization_map_entry_size_total);
    int specialization_map_entry_current = 0;
    int specialization_info_current = 0;

    for (int i = 0; i < pipeline_shader_stage_create_info_size; ++i)
    {
        const auto& rhi_pipeline_shader_stage_create_info_element = pCreateInfo->pStages[i];
        auto& vk_pipeline_shader_stage_create_info_element = vk_pipeline_shader_stage_create_info_list[i];

        if (rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo != nullptr)
        {
            vk_pipeline_shader_stage_create_info_element.pSpecializationInfo =
                &vk_specialization_info_list[specialization_info_current];

            VkSpecializationInfo vk_specialization_info {};
            vk_specialization_info.mapEntryCount =
                rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo->mapEntryCount;
            vk_specialization_info.pMapEntries = &vk_specialization_map_entry_list[specialization_map_entry_current];
            vk_specialization_info.dataSize =
                rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo->dataSize;
            vk_specialization_info.pData =
                (const void*)rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo->pData;

            // specialization_map_entry
            for (int i = 0; i < rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo->mapEntryCount; ++i)
            {
                const auto& rhi_specialization_map_entry_element =
                    rhi_pipeline_shader_stage_create_info_element.pSpecializationInfo->pMapEntries[i];
                auto& vk_specialization_map_entry_element =
                    vk_specialization_map_entry_list[specialization_map_entry_current];

                vk_specialization_map_entry_element.constantID = rhi_specialization_map_entry_element->constantID;
                vk_specialization_map_entry_element.offset = rhi_specialization_map_entry_element->offset;
                vk_specialization_map_entry_element.size = rhi_specialization_map_entry_element->size;

                specialization_map_entry_current++;
            };

            specialization_info_current++;
        }
        else
        {
            vk_pipeline_shader_stage_create_info_element.pSpecializationInfo = nullptr;
        }
        vk_pipeline_shader_stage_create_info_element.sType =
            (VkStructureType)rhi_pipeline_shader_stage_create_info_element.sType;
        vk_pipeline_shader_stage_create_info_element.pNext =
            (const void*)rhi_pipeline_shader_stage_create_info_element.pNext;
        vk_pipeline_shader_stage_create_info_element.flags =
            (VkPipelineShaderStageCreateFlags)rhi_pipeline_shader_stage_create_info_element.flags;
        vk_pipeline_shader_stage_create_info_element.stage =
            (VkShaderStageFlagBits)rhi_pipeline_shader_stage_create_info_element.stage;
        vk_pipeline_shader_stage_create_info_element.module =
            ((VulkanShader*)rhi_pipeline_shader_stage_create_info_element.module)->getResource();
        vk_pipeline_shader_stage_create_info_element.pName = rhi_pipeline_shader_stage_create_info_element.pName;
    };

    if (!((specialization_map_entry_size_total == specialization_map_entry_current) &&
          (specialization_info_total == specialization_info_current)))
    {
        LOG_ERROR(ZVulkan,
                  "(specialization_map_entry_size_total == specialization_map_entry_current)&& "
                  "(specialization_info_total == specialization_info_current)");
        return false;
    }

    // vertex_input_binding_description
    int vertex_input_binding_description_size = pCreateInfo->pVertexInputState->vertexBindingDescriptionCount;
    std::vector<VkVertexInputBindingDescription> vk_vertex_input_binding_description_list(
        vertex_input_binding_description_size);
    for (int i = 0; i < vertex_input_binding_description_size; ++i)
    {
        const auto& rhi_vertex_input_binding_description_element =
            pCreateInfo->pVertexInputState->pVertexBindingDescriptions[i];
        auto& vk_vertex_input_binding_description_element = vk_vertex_input_binding_description_list[i];

        vk_vertex_input_binding_description_element.binding = rhi_vertex_input_binding_description_element.binding;
        vk_vertex_input_binding_description_element.stride = rhi_vertex_input_binding_description_element.stride;
        vk_vertex_input_binding_description_element.inputRate =
            (VkVertexInputRate)rhi_vertex_input_binding_description_element.inputRate;
    };

    // vertex_input_attribute_description
    int vertex_input_attribute_description_size = pCreateInfo->pVertexInputState->vertexAttributeDescriptionCount;
    std::vector<VkVertexInputAttributeDescription> vk_vertex_input_attribute_description_list(
        vertex_input_attribute_description_size);
    for (int i = 0; i < vertex_input_attribute_description_size; ++i)
    {
        const auto& rhi_vertex_input_attribute_description_element =
            pCreateInfo->pVertexInputState->pVertexAttributeDescriptions[i];
        auto& vk_vertex_input_attribute_description_element = vk_vertex_input_attribute_description_list[i];

        vk_vertex_input_attribute_description_element.location =
            rhi_vertex_input_attribute_description_element.location;
        vk_vertex_input_attribute_description_element.binding = rhi_vertex_input_attribute_description_element.binding;
        vk_vertex_input_attribute_description_element.format =
            (VkFormat)rhi_vertex_input_attribute_description_element.format;
        vk_vertex_input_attribute_description_element.offset = rhi_vertex_input_attribute_description_element.offset;
    };

    VkPipelineVertexInputStateCreateInfo vk_pipeline_vertex_input_state_create_info {};
    vk_pipeline_vertex_input_state_create_info.sType = (VkStructureType)pCreateInfo->pVertexInputState->sType;
    vk_pipeline_vertex_input_state_create_info.pNext = (const void*)pCreateInfo->pVertexInputState->pNext;
    vk_pipeline_vertex_input_state_create_info.flags =
        (VkPipelineVertexInputStateCreateFlags)pCreateInfo->pVertexInputState->flags;
    vk_pipeline_vertex_input_state_create_info.vertexBindingDescriptionCount =
        pCreateInfo->pVertexInputState->vertexBindingDescriptionCount;
    vk_pipeline_vertex_input_state_create_info.pVertexBindingDescriptions =
        vk_vertex_input_binding_description_list.data();
    vk_pipeline_vertex_input_state_create_info.vertexAttributeDescriptionCount =
        pCreateInfo->pVertexInputState->vertexAttributeDescriptionCount;
    vk_pipeline_vertex_input_state_create_info.pVertexAttributeDescriptions =
        vk_vertex_input_attribute_description_list.data();

    VkPipelineInputAssemblyStateCreateInfo vk_pipeline_input_assembly_state_create_info {};
    vk_pipeline_input_assembly_state_create_info.sType = (VkStructureType)pCreateInfo->pInputAssemblyState->sType;
    vk_pipeline_input_assembly_state_create_info.pNext = (const void*)pCreateInfo->pInputAssemblyState->pNext;
    vk_pipeline_input_assembly_state_create_info.flags =
        (VkPipelineInputAssemblyStateCreateFlags)pCreateInfo->pInputAssemblyState->flags;
    vk_pipeline_input_assembly_state_create_info.topology =
        (VkPrimitiveTopology)pCreateInfo->pInputAssemblyState->topology;
    vk_pipeline_input_assembly_state_create_info.primitiveRestartEnable =
        (VkBool32)pCreateInfo->pInputAssemblyState->primitiveRestartEnable;

    const VkPipelineTessellationStateCreateInfo* vk_pipeline_tessellation_state_create_info_ptr = nullptr;
    VkPipelineTessellationStateCreateInfo vk_pipeline_tessellation_state_create_info {};
    if (pCreateInfo->pTessellationState != nullptr)
    {
        vk_pipeline_tessellation_state_create_info.sType = (VkStructureType)pCreateInfo->pTessellationState->sType;
        vk_pipeline_tessellation_state_create_info.pNext = (const void*)pCreateInfo->pTessellationState->pNext;
        vk_pipeline_tessellation_state_create_info.flags =
            (VkPipelineTessellationStateCreateFlags)pCreateInfo->pTessellationState->flags;
        vk_pipeline_tessellation_state_create_info.patchControlPoints =
            pCreateInfo->pTessellationState->patchControlPoints;

        vk_pipeline_tessellation_state_create_info_ptr = &vk_pipeline_tessellation_state_create_info;
    }

    // viewport
    int viewport_size = pCreateInfo->pViewportState->viewportCount;
    std::vector<VkViewport> vk_viewport_list(viewport_size);
    for (int i = 0; i < viewport_size; ++i)
    {
        const auto& rhi_viewport_element = pCreateInfo->pViewportState->pViewports[i];
        auto& vk_viewport_element = vk_viewport_list[i];

        vk_viewport_element.x = rhi_viewport_element.x;
        vk_viewport_element.y = rhi_viewport_element.y;
        vk_viewport_element.width = rhi_viewport_element.width;
        vk_viewport_element.height = rhi_viewport_element.height;
        vk_viewport_element.minDepth = rhi_viewport_element.minDepth;
        vk_viewport_element.maxDepth = rhi_viewport_element.maxDepth;
    };

    // rect_2d
    int rect_2d_size = pCreateInfo->pViewportState->scissorCount;
    std::vector<VkRect2D> vk_rect_2d_list(rect_2d_size);
    for (int i = 0; i < rect_2d_size; ++i)
    {
        const auto& rhi_rect_2d_element = pCreateInfo->pViewportState->pScissors[i];
        auto& vk_rect_2d_element = vk_rect_2d_list[i];

        VkOffset2D offset2d {};
        offset2d.x = rhi_rect_2d_element.offset.x;
        offset2d.y = rhi_rect_2d_element.offset.y;

        VkExtent2D extend2d {};
        extend2d.width = rhi_rect_2d_element.extent.width;
        extend2d.height = rhi_rect_2d_element.extent.height;

        vk_rect_2d_element.offset = offset2d;
        vk_rect_2d_element.extent = extend2d;
    };

    VkPipelineViewportStateCreateInfo vk_pipeline_viewport_state_create_info {};
    vk_pipeline_viewport_state_create_info.sType = (VkStructureType)pCreateInfo->pViewportState->sType;
    vk_pipeline_viewport_state_create_info.pNext = (const void*)pCreateInfo->pViewportState->pNext;
    vk_pipeline_viewport_state_create_info.flags =
        (VkPipelineViewportStateCreateFlags)pCreateInfo->pViewportState->flags;
    vk_pipeline_viewport_state_create_info.viewportCount = pCreateInfo->pViewportState->viewportCount;
    vk_pipeline_viewport_state_create_info.pViewports = vk_viewport_list.data();
    vk_pipeline_viewport_state_create_info.scissorCount = pCreateInfo->pViewportState->scissorCount;
    vk_pipeline_viewport_state_create_info.pScissors = vk_rect_2d_list.data();

    VkPipelineRasterizationStateCreateInfo vk_pipeline_rasterization_state_create_info {};
    vk_pipeline_rasterization_state_create_info.sType = (VkStructureType)pCreateInfo->pRasterizationState->sType;
    vk_pipeline_rasterization_state_create_info.pNext = (const void*)pCreateInfo->pRasterizationState->pNext;
    vk_pipeline_rasterization_state_create_info.flags =
        (VkPipelineRasterizationStateCreateFlags)pCreateInfo->pRasterizationState->flags;
    vk_pipeline_rasterization_state_create_info.depthClampEnable =
        (VkBool32)pCreateInfo->pRasterizationState->depthClampEnable;
    vk_pipeline_rasterization_state_create_info.rasterizerDiscardEnable =
        (VkBool32)pCreateInfo->pRasterizationState->rasterizerDiscardEnable;
    vk_pipeline_rasterization_state_create_info.polygonMode =
        (VkPolygonMode)pCreateInfo->pRasterizationState->polygonMode;
    vk_pipeline_rasterization_state_create_info.cullMode = (VkCullModeFlags)pCreateInfo->pRasterizationState->cullMode;
    vk_pipeline_rasterization_state_create_info.frontFace = (VkFrontFace)pCreateInfo->pRasterizationState->frontFace;
    vk_pipeline_rasterization_state_create_info.depthBiasEnable =
        (VkBool32)pCreateInfo->pRasterizationState->depthBiasEnable;
    vk_pipeline_rasterization_state_create_info.depthBiasConstantFactor =
        pCreateInfo->pRasterizationState->depthBiasConstantFactor;
    vk_pipeline_rasterization_state_create_info.depthBiasClamp = pCreateInfo->pRasterizationState->depthBiasClamp;
    vk_pipeline_rasterization_state_create_info.depthBiasSlopeFactor =
        pCreateInfo->pRasterizationState->depthBiasSlopeFactor;
    vk_pipeline_rasterization_state_create_info.lineWidth = pCreateInfo->pRasterizationState->lineWidth;

    VkPipelineMultisampleStateCreateInfo vk_pipeline_multisample_state_create_info {};
    vk_pipeline_multisample_state_create_info.sType = (VkStructureType)pCreateInfo->pMultisampleState->sType;
    vk_pipeline_multisample_state_create_info.pNext = (const void*)pCreateInfo->pMultisampleState->pNext;
    vk_pipeline_multisample_state_create_info.flags =
        (VkPipelineMultisampleStateCreateFlags)pCreateInfo->pMultisampleState->flags;
    vk_pipeline_multisample_state_create_info.rasterizationSamples =
        (VkSampleCountFlagBits)pCreateInfo->pMultisampleState->rasterizationSamples;
    vk_pipeline_multisample_state_create_info.sampleShadingEnable =
        (VkBool32)pCreateInfo->pMultisampleState->sampleShadingEnable;
    vk_pipeline_multisample_state_create_info.minSampleShading = pCreateInfo->pMultisampleState->minSampleShading;
    vk_pipeline_multisample_state_create_info.pSampleMask =
        (const RHISampleMask*)pCreateInfo->pMultisampleState->pSampleMask;
    vk_pipeline_multisample_state_create_info.alphaToCoverageEnable =
        (VkBool32)pCreateInfo->pMultisampleState->alphaToCoverageEnable;
    vk_pipeline_multisample_state_create_info.alphaToOneEnable =
        (VkBool32)pCreateInfo->pMultisampleState->alphaToOneEnable;

    VkStencilOpState stencil_op_state_front {};
    stencil_op_state_front.failOp = (VkStencilOp)pCreateInfo->pDepthStencilState->front.failOp;
    stencil_op_state_front.passOp = (VkStencilOp)pCreateInfo->pDepthStencilState->front.passOp;
    stencil_op_state_front.depthFailOp = (VkStencilOp)pCreateInfo->pDepthStencilState->front.depthFailOp;
    stencil_op_state_front.compareOp = (VkCompareOp)pCreateInfo->pDepthStencilState->front.compareOp;
    stencil_op_state_front.compareMask = pCreateInfo->pDepthStencilState->front.compareMask;
    stencil_op_state_front.writeMask = pCreateInfo->pDepthStencilState->front.writeMask;
    stencil_op_state_front.reference = pCreateInfo->pDepthStencilState->front.reference;

    VkStencilOpState stencil_op_state_back {};
    stencil_op_state_back.failOp = (VkStencilOp)pCreateInfo->pDepthStencilState->back.failOp;
    stencil_op_state_back.passOp = (VkStencilOp)pCreateInfo->pDepthStencilState->back.passOp;
    stencil_op_state_back.depthFailOp = (VkStencilOp)pCreateInfo->pDepthStencilState->back.depthFailOp;
    stencil_op_state_back.compareOp = (VkCompareOp)pCreateInfo->pDepthStencilState->back.compareOp;
    stencil_op_state_back.compareMask = pCreateInfo->pDepthStencilState->back.compareMask;
    stencil_op_state_back.writeMask = pCreateInfo->pDepthStencilState->back.writeMask;
    stencil_op_state_back.reference = pCreateInfo->pDepthStencilState->back.reference;

    VkPipelineDepthStencilStateCreateInfo vk_pipeline_depth_stencil_state_create_info {};
    vk_pipeline_depth_stencil_state_create_info.sType = (VkStructureType)pCreateInfo->pDepthStencilState->sType;
    vk_pipeline_depth_stencil_state_create_info.pNext = (const void*)pCreateInfo->pDepthStencilState->pNext;
    vk_pipeline_depth_stencil_state_create_info.flags =
        (VkPipelineDepthStencilStateCreateFlags)pCreateInfo->pDepthStencilState->flags;
    vk_pipeline_depth_stencil_state_create_info.depthTestEnable =
        (VkBool32)pCreateInfo->pDepthStencilState->depthTestEnable;
    vk_pipeline_depth_stencil_state_create_info.depthWriteEnable =
        (VkBool32)pCreateInfo->pDepthStencilState->depthWriteEnable;
    vk_pipeline_depth_stencil_state_create_info.depthCompareOp =
        (VkCompareOp)pCreateInfo->pDepthStencilState->depthCompareOp;
    vk_pipeline_depth_stencil_state_create_info.depthBoundsTestEnable =
        (VkBool32)pCreateInfo->pDepthStencilState->depthBoundsTestEnable;
    vk_pipeline_depth_stencil_state_create_info.stencilTestEnable =
        (VkBool32)pCreateInfo->pDepthStencilState->stencilTestEnable;
    vk_pipeline_depth_stencil_state_create_info.front = stencil_op_state_front;
    vk_pipeline_depth_stencil_state_create_info.back = stencil_op_state_back;
    vk_pipeline_depth_stencil_state_create_info.minDepthBounds = pCreateInfo->pDepthStencilState->minDepthBounds;
    vk_pipeline_depth_stencil_state_create_info.maxDepthBounds = pCreateInfo->pDepthStencilState->maxDepthBounds;

    // pipeline_color_blend_attachment_state
    int pipeline_color_blend_attachment_state_size = pCreateInfo->pColorBlendState->attachmentCount;
    std::vector<VkPipelineColorBlendAttachmentState> vk_pipeline_color_blend_attachment_state_list(
        pipeline_color_blend_attachment_state_size);
    for (int i = 0; i < pipeline_color_blend_attachment_state_size; ++i)
    {
        const auto& rhi_pipeline_color_blend_attachment_state_element = pCreateInfo->pColorBlendState->pAttachments[i];
        auto& vk_pipeline_color_blend_attachment_state_element = vk_pipeline_color_blend_attachment_state_list[i];

        vk_pipeline_color_blend_attachment_state_element.blendEnable =
            (VkBool32)rhi_pipeline_color_blend_attachment_state_element.blendEnable;
        vk_pipeline_color_blend_attachment_state_element.srcColorBlendFactor =
            (VkBlendFactor)rhi_pipeline_color_blend_attachment_state_element.srcColorBlendFactor;
        vk_pipeline_color_blend_attachment_state_element.dstColorBlendFactor =
            (VkBlendFactor)rhi_pipeline_color_blend_attachment_state_element.dstColorBlendFactor;
        vk_pipeline_color_blend_attachment_state_element.colorBlendOp =
            (VkBlendOp)rhi_pipeline_color_blend_attachment_state_element.colorBlendOp;
        vk_pipeline_color_blend_attachment_state_element.srcAlphaBlendFactor =
            (VkBlendFactor)rhi_pipeline_color_blend_attachment_state_element.srcAlphaBlendFactor;
        vk_pipeline_color_blend_attachment_state_element.dstAlphaBlendFactor =
            (VkBlendFactor)rhi_pipeline_color_blend_attachment_state_element.dstAlphaBlendFactor;
        vk_pipeline_color_blend_attachment_state_element.alphaBlendOp =
            (VkBlendOp)rhi_pipeline_color_blend_attachment_state_element.alphaBlendOp;
        vk_pipeline_color_blend_attachment_state_element.colorWriteMask =
            (VkColorComponentFlags)rhi_pipeline_color_blend_attachment_state_element.colorWriteMask;
    };

    VkPipelineColorBlendStateCreateInfo vk_pipeline_color_blend_state_create_info {};
    vk_pipeline_color_blend_state_create_info.sType = (VkStructureType)pCreateInfo->pColorBlendState->sType;
    vk_pipeline_color_blend_state_create_info.pNext = pCreateInfo->pColorBlendState->pNext;
    vk_pipeline_color_blend_state_create_info.flags = pCreateInfo->pColorBlendState->flags;
    vk_pipeline_color_blend_state_create_info.logicOpEnable = pCreateInfo->pColorBlendState->logicOpEnable;
    vk_pipeline_color_blend_state_create_info.logicOp = (VkLogicOp)pCreateInfo->pColorBlendState->logicOp;
    vk_pipeline_color_blend_state_create_info.attachmentCount = pCreateInfo->pColorBlendState->attachmentCount;
    vk_pipeline_color_blend_state_create_info.pAttachments = vk_pipeline_color_blend_attachment_state_list.data();
    for (int i = 0; i < 4; ++i)
    {
        vk_pipeline_color_blend_state_create_info.blendConstants[i] = pCreateInfo->pColorBlendState->blendConstants[i];
    };

    // dynamic_state
    int dynamic_state_size = pCreateInfo->pDynamicState->dynamicStateCount;
    std::vector<VkDynamicState> vk_dynamic_state_list(dynamic_state_size);
    for (int i = 0; i < dynamic_state_size; ++i)
    {
        const auto& rhi_dynamic_state_element = pCreateInfo->pDynamicState->pDynamicStates[i];
        auto& vk_dynamic_state_element = vk_dynamic_state_list[i];

        vk_dynamic_state_element = (VkDynamicState)rhi_dynamic_state_element;
    };

    VkPipelineDynamicStateCreateInfo vk_pipeline_dynamic_state_create_info {};
    vk_pipeline_dynamic_state_create_info.sType = (VkStructureType)pCreateInfo->pDynamicState->sType;
    vk_pipeline_dynamic_state_create_info.pNext = pCreateInfo->pDynamicState->pNext;
    vk_pipeline_dynamic_state_create_info.flags = (VkPipelineDynamicStateCreateFlags)pCreateInfo->pDynamicState->flags;
    vk_pipeline_dynamic_state_create_info.dynamicStateCount = pCreateInfo->pDynamicState->dynamicStateCount;
    vk_pipeline_dynamic_state_create_info.pDynamicStates = vk_dynamic_state_list.data();

    VkGraphicsPipelineCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkPipelineCreateFlags)pCreateInfo->flags;
    create_info.stageCount = pCreateInfo->stageCount;
    create_info.pStages = vk_pipeline_shader_stage_create_info_list.data();
    create_info.pVertexInputState = &vk_pipeline_vertex_input_state_create_info;
    create_info.pInputAssemblyState = &vk_pipeline_input_assembly_state_create_info;
    create_info.pTessellationState = vk_pipeline_tessellation_state_create_info_ptr;
    create_info.pViewportState = &vk_pipeline_viewport_state_create_info;
    create_info.pRasterizationState = &vk_pipeline_rasterization_state_create_info;
    create_info.pMultisampleState = &vk_pipeline_multisample_state_create_info;
    create_info.pDepthStencilState = &vk_pipeline_depth_stencil_state_create_info;
    create_info.pColorBlendState = &vk_pipeline_color_blend_state_create_info;
    create_info.pDynamicState = &vk_pipeline_dynamic_state_create_info;
    create_info.layout = ((VulkanPipelineLayout*)pCreateInfo->layout)->getResource();
    create_info.renderPass = ((VulkanRenderPass*)pCreateInfo->renderPass)->getResource();
    create_info.subpass = pCreateInfo->subpass;
    if (pCreateInfo->basePipelineHandle != nullptr)
    {
        create_info.basePipelineHandle = ((VulkanPipeline*)pCreateInfo->basePipelineHandle)->getResource();
    }
    else
    {
        create_info.basePipelineHandle = VK_NULL_HANDLE;
    }
    create_info.basePipelineIndex = pCreateInfo->basePipelineIndex;

    pPipelines = new VulkanPipeline();
    VkPipeline vk_pipelines;
    VkPipelineCache vk_pipeline_cache = VK_NULL_HANDLE;
    if (pipelineCache != nullptr)
    {
        vk_pipeline_cache = ((VulkanPipelineCache*)pipelineCache)->getResource();
    }
    VkResult result =
        vkCreateGraphicsPipelines(m_Device, vk_pipeline_cache, createInfoCount, &create_info, nullptr, &vk_pipelines);
    ((VulkanPipeline*)pPipelines)->setResource(vk_pipelines);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateGraphicsPipelines failed!");
        return false;
    }
}

bool VulkanRHI::CreateComputePipelines(RHIPipelineCache* pipelineCache,
                                       uint32_t createInfoCount,
                                       const RHIComputePipelineCreateInfo* pCreateInfos,
                                       RHIPipeline*& pPipelines)
{
    VkPipelineShaderStageCreateInfo shader_stage_create_info {};
    if (pCreateInfos->pStages->pSpecializationInfo != nullptr)
    {
        // will be complete soon if needed.
        shader_stage_create_info.pSpecializationInfo = nullptr;
    }
    else
    {
        shader_stage_create_info.pSpecializationInfo = nullptr;
    }
    shader_stage_create_info.sType = (VkStructureType)pCreateInfos->pStages->sType;
    shader_stage_create_info.pNext = (const void*)pCreateInfos->pStages->pNext;
    shader_stage_create_info.flags = (VkPipelineShaderStageCreateFlags)pCreateInfos->pStages->flags;
    shader_stage_create_info.stage = (VkShaderStageFlagBits)pCreateInfos->pStages->stage;
    shader_stage_create_info.module = ((VulkanShader*)pCreateInfos->pStages->module)->getResource();
    shader_stage_create_info.pName = pCreateInfos->pStages->pName;

    VkComputePipelineCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfos->sType;
    create_info.pNext = (const void*)pCreateInfos->pNext;
    create_info.flags = (VkPipelineCreateFlags)pCreateInfos->flags;
    create_info.stage = shader_stage_create_info;
    create_info.layout = ((VulkanPipelineLayout*)pCreateInfos->layout)->getResource();
    ;
    if (pCreateInfos->basePipelineHandle != nullptr)
    {
        create_info.basePipelineHandle = ((VulkanPipeline*)pCreateInfos->basePipelineHandle)->getResource();
    }
    else
    {
        create_info.basePipelineHandle = VK_NULL_HANDLE;
    }
    create_info.basePipelineIndex = pCreateInfos->basePipelineIndex;

    pPipelines = new VulkanPipeline();
    VkPipeline vk_pipelines;
    VkPipelineCache vk_pipeline_cache = VK_NULL_HANDLE;
    if (pipelineCache != nullptr)
    {
        vk_pipeline_cache = ((VulkanPipelineCache*)pipelineCache)->getResource();
    }
    VkResult result =
        vkCreateComputePipelines(m_Device, vk_pipeline_cache, createInfoCount, &create_info, nullptr, &vk_pipelines);
    ((VulkanPipeline*)pPipelines)->setResource(vk_pipelines);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateComputePipelines failed!");
        return false;
    }
}

bool VulkanRHI::CreatePipelineLayout(const RHIPipelineLayoutCreateInfo* pCreateInfo,
                                     RHIPipelineLayout*& pPipelineLayout)
{
    // descriptor_set_layout
    int descriptor_set_layout_size = pCreateInfo->setLayoutCount;
    std::vector<VkDescriptorSetLayout> vk_descriptor_set_layout_list(descriptor_set_layout_size);
    for (int i = 0; i < descriptor_set_layout_size; ++i)
    {
        const auto& rhi_descriptor_set_layout_element = pCreateInfo->pSetLayouts[i];
        auto& vk_descriptor_set_layout_element = vk_descriptor_set_layout_list[i];

        vk_descriptor_set_layout_element =
            ((VulkanDescriptorSetLayout*)rhi_descriptor_set_layout_element)->getResource();
    };

    // PR-V1: forward push-constant ranges so bindless / material code
    // can publish a `vkCmdPushConstants`-compatible target through the
    // RHI-neutral RHIPipelineLayoutCreateInfo. RHIShaderStageFlags is
    // typedef'd uint32_t and the RHI_SHADER_STAGE_*_BIT enum values are
    // bit-identical to VkShaderStageFlagBits (see render_type.h
    // RHI_SHADER_STAGE_VERTEX_BIT = 0x1, RHI_SHADER_STAGE_ALL =
    // 0x7FFFFFFF), so a flat memcpy/cast is the contract -- the same
    // shape every other stageFlags->VkShaderStageFlags hop in this
    // backend uses. DX12 ignores pPushConstantRanges entirely (its
    // bindless path uses a root 32-bit constant reserved inside
    // DX12RHI::CreatePipelineLayout); the field is therefore Vulkan-
    // flavoured but lives in the shared RHI struct because no neutral
    // alternative exists.
    std::vector<VkPushConstantRange> vk_push_constant_ranges;
    vk_push_constant_ranges.reserve(pCreateInfo->pushConstantRangeCount);
    for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; ++i)
    {
        const RHIPushConstantRange& src = pCreateInfo->pPushConstantRanges[i];
        VkPushConstantRange dst {};
        dst.stageFlags = static_cast<VkShaderStageFlags>(src.stageFlags);
        dst.offset = src.offset;
        dst.size = src.size;
        vk_push_constant_ranges.push_back(dst);
    }

    VkPipelineLayoutCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkPipelineLayoutCreateFlags)pCreateInfo->flags;
    create_info.setLayoutCount = pCreateInfo->setLayoutCount;
    create_info.pSetLayouts = vk_descriptor_set_layout_list.data();
    create_info.pushConstantRangeCount = pCreateInfo->pushConstantRangeCount;
    create_info.pPushConstantRanges =
        vk_push_constant_ranges.empty() ? nullptr : vk_push_constant_ranges.data();

    pPipelineLayout = new VulkanPipelineLayout();
    VkPipelineLayout vk_pipeline_layout;
    VkResult result = vkCreatePipelineLayout(m_Device, &create_info, nullptr, &vk_pipeline_layout);
    ((VulkanPipelineLayout*)pPipelineLayout)->setResource(vk_pipeline_layout);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreatePipelineLayout failed!");
        return false;
    }
}

bool VulkanRHI::CreateRenderPass(const RHIRenderPassCreateInfo* pCreateInfo, RHIRenderPass*& pRenderPass)
{
    // attachment convert
    std::vector<VkAttachmentDescription> vk_attachments(pCreateInfo->attachmentCount);
    for (int i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        const auto& rhi_desc = pCreateInfo->pAttachments[i];
        auto& vk_desc = vk_attachments[i];

        vk_desc.flags = (VkAttachmentDescriptionFlags)(rhi_desc).flags;
        vk_desc.format = (VkFormat)(rhi_desc).format;
        vk_desc.samples = (VkSampleCountFlagBits)(rhi_desc).samples;
        vk_desc.loadOp = (VkAttachmentLoadOp)(rhi_desc).loadOp;
        vk_desc.storeOp = (VkAttachmentStoreOp)(rhi_desc).storeOp;
        vk_desc.stencilLoadOp = (VkAttachmentLoadOp)(rhi_desc).stencilLoadOp;
        vk_desc.stencilStoreOp = (VkAttachmentStoreOp)(rhi_desc).stencilStoreOp;
        vk_desc.initialLayout = (VkImageLayout)(rhi_desc).initialLayout;
        vk_desc.finalLayout = (VkImageLayout)(rhi_desc).finalLayout;
    };

    // subpass convert
    int totalAttachmentRefenrence = 0;
    for (int i = 0; i < pCreateInfo->subpassCount; i++)
    {
        const auto& rhi_desc = pCreateInfo->pSubpasses[i];
        totalAttachmentRefenrence += rhi_desc.inputAttachmentCount;  // pInputAttachments
        totalAttachmentRefenrence += rhi_desc.colorAttachmentCount;  // pColorAttachments
        if (rhi_desc.pDepthStencilAttachment != nullptr)
        {
            totalAttachmentRefenrence += rhi_desc.colorAttachmentCount;  // pDepthStencilAttachment
        }
        if (rhi_desc.pResolveAttachments != nullptr)
        {
            totalAttachmentRefenrence += rhi_desc.colorAttachmentCount;  // pResolveAttachments
        }
    }
    std::vector<VkSubpassDescription> vk_subpass_description(pCreateInfo->subpassCount);
    std::vector<VkAttachmentReference> vk_attachment_reference(totalAttachmentRefenrence);
    int currentAttachmentRefence = 0;
    for (int i = 0; i < pCreateInfo->subpassCount; ++i)
    {
        const auto& rhi_desc = pCreateInfo->pSubpasses[i];
        auto& vk_desc = vk_subpass_description[i];

        vk_desc.flags = (VkSubpassDescriptionFlags)(rhi_desc).flags;
        vk_desc.pipelineBindPoint = (VkPipelineBindPoint)(rhi_desc).pipelineBindPoint;
        vk_desc.preserveAttachmentCount = (rhi_desc).preserveAttachmentCount;
        vk_desc.pPreserveAttachments = (const uint32_t*)(rhi_desc).pPreserveAttachments;

        vk_desc.inputAttachmentCount = (rhi_desc).inputAttachmentCount;
        vk_desc.pInputAttachments = &vk_attachment_reference[currentAttachmentRefence];
        for (int i = 0; i < (rhi_desc).inputAttachmentCount; i++)
        {
            const auto& rhi_attachment_refence_input = (rhi_desc).pInputAttachments[i];
            auto& vk_attachment_refence_input = vk_attachment_reference[currentAttachmentRefence];

            vk_attachment_refence_input.attachment = rhi_attachment_refence_input.attachment;
            vk_attachment_refence_input.layout = (VkImageLayout)(rhi_attachment_refence_input.layout);

            currentAttachmentRefence += 1;
        };

        vk_desc.colorAttachmentCount = (rhi_desc).colorAttachmentCount;
        vk_desc.pColorAttachments = &vk_attachment_reference[currentAttachmentRefence];
        for (int i = 0; i < (rhi_desc).colorAttachmentCount; ++i)
        {
            const auto& rhi_attachment_refence_color = (rhi_desc).pColorAttachments[i];
            auto& vk_attachment_refence_color = vk_attachment_reference[currentAttachmentRefence];

            vk_attachment_refence_color.attachment = rhi_attachment_refence_color.attachment;
            vk_attachment_refence_color.layout = (VkImageLayout)(rhi_attachment_refence_color.layout);

            currentAttachmentRefence += 1;
        };

        if (rhi_desc.pResolveAttachments != nullptr)
        {
            vk_desc.pResolveAttachments = &vk_attachment_reference[currentAttachmentRefence];
            for (int i = 0; i < (rhi_desc).colorAttachmentCount; ++i)
            {
                const auto& rhi_attachment_refence_resolve = (rhi_desc).pResolveAttachments[i];
                auto& vk_attachment_refence_resolve = vk_attachment_reference[currentAttachmentRefence];

                vk_attachment_refence_resolve.attachment = rhi_attachment_refence_resolve.attachment;
                vk_attachment_refence_resolve.layout = (VkImageLayout)(rhi_attachment_refence_resolve.layout);

                currentAttachmentRefence += 1;
            };
        }

        if (rhi_desc.pDepthStencilAttachment != nullptr)
        {
            vk_desc.pDepthStencilAttachment = &vk_attachment_reference[currentAttachmentRefence];
            for (int i = 0; i < (rhi_desc).colorAttachmentCount; ++i)
            {
                const auto& rhi_attachment_refence_depth = (rhi_desc).pDepthStencilAttachment[i];
                auto& vk_attachment_refence_depth = vk_attachment_reference[currentAttachmentRefence];

                vk_attachment_refence_depth.attachment = rhi_attachment_refence_depth.attachment;
                vk_attachment_refence_depth.layout = (VkImageLayout)(rhi_attachment_refence_depth.layout);

                currentAttachmentRefence += 1;
            };
        };
    };
    if (currentAttachmentRefence != totalAttachmentRefenrence)
    {
        LOG_ERROR(ZVulkan, "currentAttachmentRefence != totalAttachmentRefenrence");
        return false;
    }

    std::vector<VkSubpassDependency> vk_subpass_depandecy(pCreateInfo->dependencyCount);
    for (int i = 0; i < pCreateInfo->dependencyCount; ++i)
    {
        const auto& rhi_desc = pCreateInfo->pDependencies[i];
        auto& vk_desc = vk_subpass_depandecy[i];

        vk_desc.srcSubpass = rhi_desc.srcSubpass;
        vk_desc.dstSubpass = rhi_desc.dstSubpass;
        vk_desc.srcStageMask = (VkPipelineStageFlags)(rhi_desc).srcStageMask;
        vk_desc.dstStageMask = (VkPipelineStageFlags)(rhi_desc).dstStageMask;
        vk_desc.srcAccessMask = (VkAccessFlags)(rhi_desc).srcAccessMask;
        vk_desc.dstAccessMask = (VkAccessFlags)(rhi_desc).dstAccessMask;
        vk_desc.dependencyFlags = (VkDependencyFlags)(rhi_desc).dependencyFlags;
    };

    VkRenderPassCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkRenderPassCreateFlags)pCreateInfo->flags;
    create_info.attachmentCount = pCreateInfo->attachmentCount;
    create_info.pAttachments = vk_attachments.data();
    create_info.subpassCount = pCreateInfo->subpassCount;
    create_info.pSubpasses = vk_subpass_description.data();
    create_info.dependencyCount = pCreateInfo->dependencyCount;
    create_info.pDependencies = vk_subpass_depandecy.data();

    pRenderPass = new VulkanRenderPass();
    VkRenderPass vk_render_pass;
    VkResult result = vkCreateRenderPass(m_Device, &create_info, nullptr, &vk_render_pass);
    ((VulkanRenderPass*)pRenderPass)->setResource(vk_render_pass);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateRenderPass failed!");
        return false;
    }
}

void VulkanRHI::DestroyRenderPass(RHIRenderPass* renderPass)
{
    if (renderPass == nullptr)
    {
        return;
    }
    VulkanRenderPass* vk_render_pass = static_cast<VulkanRenderPass*>(renderPass);
    VkRenderPass vk_handle = vk_render_pass->getResource();
    if (vk_handle != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_Device, vk_handle, nullptr);
    }
    delete vk_render_pass;
}

bool VulkanRHI::CreateSampler(const RHISamplerCreateInfo* pCreateInfo, RHISampler*& pSampler)
{
    VkSamplerCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = (const void*)pCreateInfo->pNext;
    create_info.flags = (VkSamplerCreateFlags)pCreateInfo->flags;
    create_info.magFilter = (VkFilter)pCreateInfo->magFilter;
    create_info.minFilter = (VkFilter)pCreateInfo->minFilter;
    create_info.mipmapMode = (VkSamplerMipmapMode)pCreateInfo->mipmapMode;
    create_info.addressModeU = (VkSamplerAddressMode)pCreateInfo->addressModeU;
    create_info.addressModeV = (VkSamplerAddressMode)pCreateInfo->addressModeV;
    create_info.addressModeW = (VkSamplerAddressMode)pCreateInfo->addressModeW;
    create_info.mipLodBias = pCreateInfo->mipLodBias;
    create_info.anisotropyEnable = (VkBool32)pCreateInfo->anisotropyEnable;
    create_info.maxAnisotropy = pCreateInfo->maxAnisotropy;
    create_info.compareEnable = (VkBool32)pCreateInfo->compareEnable;
    create_info.compareOp = (VkCompareOp)pCreateInfo->compareOp;
    create_info.minLod = pCreateInfo->minLod;
    create_info.maxLod = pCreateInfo->maxLod;
    create_info.borderColor = (VkBorderColor)pCreateInfo->borderColor;
    create_info.unnormalizedCoordinates = (VkBool32)pCreateInfo->unnormalizedCoordinates;

    pSampler = new VulkanSampler();
    VkSampler vk_sampler;
    VkResult result = vkCreateSampler(m_Device, &create_info, nullptr, &vk_sampler);
    ((VulkanSampler*)pSampler)->setResource(vk_sampler);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateSampler failed!");
        return false;
    }
}

bool VulkanRHI::CreateSemaphore(const RHISemaphoreCreateInfo* pCreateInfo, RHISemaphore*& pSemaphore)
{
    VkSemaphoreCreateInfo create_info {};
    create_info.sType = (VkStructureType)pCreateInfo->sType;
    create_info.pNext = pCreateInfo->pNext;
    create_info.flags = (VkSemaphoreCreateFlags)pCreateInfo->flags;

    pSemaphore = new VulkanSemaphore();
    VkSemaphore vk_semaphore;
    VkResult result = vkCreateSemaphore(m_Device, &create_info, nullptr, &vk_semaphore);
    ((VulkanSemaphore*)pSemaphore)->setResource(vk_semaphore);

    if (result == VK_SUCCESS)
    {
        return RHI_SUCCESS;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkCreateSemaphore failed!");
        return false;
    }
}

bool VulkanRHI::WaitForFencesPFN(uint32_t fenceCount, RHIFence* const* pFences, RHIBool32 waitAll, uint64_t timeout)
{
    // fence
    int fence_size = fenceCount;
    std::vector<VkFence> vk_fence_list(fence_size);
    for (int i = 0; i < fence_size; ++i)
    {
        const auto& rhi_fence_element = pFences[i];
        auto& vk_fence_element = vk_fence_list[i];

        vk_fence_element = ((VulkanFence*)rhi_fence_element)->getResource();
    };

    VkResult result = _vkWaitForFences(m_Device, fenceCount, vk_fence_list.data(), waitAll, timeout);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "_vkWaitForFences failed!");
        return false;
    }
}

bool VulkanRHI::ResetFencesPFN(uint32_t fenceCount, RHIFence* const* pFences)
{
    // fence
    int fence_size = fenceCount;
    std::vector<VkFence> vk_fence_list(fence_size);
    for (int i = 0; i < fence_size; ++i)
    {
        const auto& rhi_fence_element = pFences[i];
        auto& vk_fence_element = vk_fence_list[i];

        vk_fence_element = ((VulkanFence*)rhi_fence_element)->getResource();
    };

    VkResult result = _vkResetFences(m_Device, fenceCount, vk_fence_list.data());

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "_vkResetFences failed!");
        return false;
    }
}

bool VulkanRHI::ResetCommandPoolPFN(RHICommandPool* commandPool, RHICommandPoolResetFlags flags)
{
    VkResult result =
        _vkResetCommandPool(m_Device, ((VulkanCommandPool*)commandPool)->getResource(), (VkCommandPoolResetFlags)flags);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "_vkResetCommandPool failed!");
        return false;
    }
}

bool VulkanRHI::BeginCommandBufferPFN(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo* pBeginInfo)
{
    VkCommandBufferInheritanceInfo* command_buffer_inheritance_info_ptr = nullptr;
    VkCommandBufferInheritanceInfo command_buffer_inheritance_info {};
    if (pBeginInfo->pInheritanceInfo != nullptr)
    {
        command_buffer_inheritance_info.sType = (VkStructureType)pBeginInfo->pInheritanceInfo->sType;
        command_buffer_inheritance_info.pNext = (const void*)pBeginInfo->pInheritanceInfo->pNext;
        command_buffer_inheritance_info.renderPass =
            ((VulkanRenderPass*)pBeginInfo->pInheritanceInfo->renderPass)->getResource();
        command_buffer_inheritance_info.subpass = pBeginInfo->pInheritanceInfo->subpass;
        command_buffer_inheritance_info.framebuffer =
            ((VulkanFramebuffer*)pBeginInfo->pInheritanceInfo->framebuffer)->getResource();
        command_buffer_inheritance_info.occlusionQueryEnable =
            (VkBool32)pBeginInfo->pInheritanceInfo->occlusionQueryEnable;
        command_buffer_inheritance_info.queryFlags = (VkQueryControlFlags)pBeginInfo->pInheritanceInfo->queryFlags;
        command_buffer_inheritance_info.pipelineStatistics =
            (VkQueryPipelineStatisticFlags)pBeginInfo->pInheritanceInfo->pipelineStatistics;

        command_buffer_inheritance_info_ptr = &command_buffer_inheritance_info;
    }

    VkCommandBufferBeginInfo command_buffer_begin_info {};
    command_buffer_begin_info.sType = (VkStructureType)pBeginInfo->sType;
    command_buffer_begin_info.pNext = (const void*)pBeginInfo->pNext;
    command_buffer_begin_info.flags = (VkCommandBufferUsageFlags)pBeginInfo->flags;
    command_buffer_begin_info.pInheritanceInfo = command_buffer_inheritance_info_ptr;
    VkResult result =
        _vkBeginCommandBuffer(((VulkanCommandBuffer*)commandBuffer)->getResource(), &command_buffer_begin_info);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "_vkBeginCommandBuffer failed!");
        return false;
    }
}

bool VulkanRHI::EndCommandBufferPFN(RHICommandBuffer* commandBuffer)
{
    VkResult result = _vkEndCommandBuffer(((VulkanCommandBuffer*)commandBuffer)->getResource());

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "_vkEndCommandBuffer failed!");
        return false;
    }
}

void VulkanRHI::CmdBeginRenderPassPFN(RHICommandBuffer* commandBuffer,
                                      const RHIRenderPassBeginInfo* pRenderPassBegin,
                                      RHISubpassContents contents)
{
    VkOffset2D offset_2d {};
    offset_2d.x = pRenderPassBegin->renderArea.offset.x;
    offset_2d.y = pRenderPassBegin->renderArea.offset.y;

    VkExtent2D extent_2d {};
    extent_2d.width = pRenderPassBegin->renderArea.extent.width;
    extent_2d.height = pRenderPassBegin->renderArea.extent.height;

    VkRect2D rect_2d {};
    rect_2d.offset = offset_2d;
    rect_2d.extent = extent_2d;

    // clear_values
    int clear_value_size = pRenderPassBegin->clearValueCount;
    std::vector<VkClearValue> vk_clear_value_list(clear_value_size);
    for (int i = 0; i < clear_value_size; ++i)
    {
        const auto& rhi_clear_value_element = pRenderPassBegin->pClearValues[i];
        auto& vk_clear_value_element = vk_clear_value_list[i];

        VkClearColorValue vk_clear_color_value;
        vk_clear_color_value.float32[0] = rhi_clear_value_element.color.float32[0];
        vk_clear_color_value.float32[1] = rhi_clear_value_element.color.float32[1];
        vk_clear_color_value.float32[2] = rhi_clear_value_element.color.float32[2];
        vk_clear_color_value.float32[3] = rhi_clear_value_element.color.float32[3];
        vk_clear_color_value.int32[0] = rhi_clear_value_element.color.int32[0];
        vk_clear_color_value.int32[1] = rhi_clear_value_element.color.int32[1];
        vk_clear_color_value.int32[2] = rhi_clear_value_element.color.int32[2];
        vk_clear_color_value.int32[3] = rhi_clear_value_element.color.int32[3];
        vk_clear_color_value.uint32[0] = rhi_clear_value_element.color.uint32[0];
        vk_clear_color_value.uint32[1] = rhi_clear_value_element.color.uint32[1];
        vk_clear_color_value.uint32[2] = rhi_clear_value_element.color.uint32[2];
        vk_clear_color_value.uint32[3] = rhi_clear_value_element.color.uint32[3];

        VkClearDepthStencilValue vk_clear_depth_stencil_value;
        vk_clear_depth_stencil_value.depth = rhi_clear_value_element.depthStencil.depth;
        vk_clear_depth_stencil_value.stencil = rhi_clear_value_element.depthStencil.stencil;

        vk_clear_value_element.color = vk_clear_color_value;
        vk_clear_value_element.depthStencil = vk_clear_depth_stencil_value;
    };

    VkRenderPassBeginInfo vk_render_pass_begin_info {};
    vk_render_pass_begin_info.sType = (VkStructureType)pRenderPassBegin->sType;
    vk_render_pass_begin_info.pNext = pRenderPassBegin->pNext;
    vk_render_pass_begin_info.renderPass = ((VulkanRenderPass*)pRenderPassBegin->renderPass)->getResource();
    vk_render_pass_begin_info.framebuffer = ((VulkanFramebuffer*)pRenderPassBegin->framebuffer)->getResource();
    vk_render_pass_begin_info.renderArea = rect_2d;
    vk_render_pass_begin_info.clearValueCount = pRenderPassBegin->clearValueCount;
    vk_render_pass_begin_info.pClearValues = vk_clear_value_list.data();

    return _vkCmdBeginRenderPass(
        ((VulkanCommandBuffer*)commandBuffer)->getResource(), &vk_render_pass_begin_info, (VkSubpassContents)contents);
}

void VulkanRHI::CmdNextSubpassPFN(RHICommandBuffer* commandBuffer, RHISubpassContents contents)
{
    return _vkCmdNextSubpass(((VulkanCommandBuffer*)commandBuffer)->getResource(), ((VkSubpassContents)contents));
}

void VulkanRHI::CmdEndRenderPassPFN(RHICommandBuffer* commandBuffer)
{
    return _vkCmdEndRenderPass(((VulkanCommandBuffer*)commandBuffer)->getResource());
}

void VulkanRHI::CmdBindPipelinePFN(RHICommandBuffer* commandBuffer,
                                   RHIPipelineBindPoint pipelineBindPoint,
                                   RHIPipeline* pipeline)
{
    return _vkCmdBindPipeline(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                              (VkPipelineBindPoint)pipelineBindPoint,
                              ((VulkanPipeline*)pipeline)->getResource());
}

void VulkanRHI::CmdSetViewportPFN(RHICommandBuffer* commandBuffer,
                                  uint32_t firstViewport,
                                  uint32_t viewportCount,
                                  const RHIViewport* pViewports)
{
    // viewport
    int viewport_size = viewportCount;
    std::vector<VkViewport> vk_viewport_list(viewport_size);
    for (int i = 0; i < viewport_size; ++i)
    {
        const auto& rhi_viewport_element = pViewports[i];
        auto& vk_viewport_element = vk_viewport_list[i];

        vk_viewport_element.x = rhi_viewport_element.x;
        vk_viewport_element.y = rhi_viewport_element.y;
        vk_viewport_element.width = rhi_viewport_element.width;
        vk_viewport_element.height = rhi_viewport_element.height;
        vk_viewport_element.minDepth = rhi_viewport_element.minDepth;
        vk_viewport_element.maxDepth = rhi_viewport_element.maxDepth;
    };
    /*if (viewport_size > 1)
    {
        return vkCmdSetViewport(
            ((VulkanCommandBuffer*)commandBuffer)->getResource(), 1, viewportCount, vk_viewport_list.data());
    }*/
    return vkCmdSetViewport(
        ((VulkanCommandBuffer*)commandBuffer)->getResource(), firstViewport, viewportCount, vk_viewport_list.data());
}

void VulkanRHI::CmdSetScissorPFN(RHICommandBuffer* commandBuffer,
                                 uint32_t firstScissor,
                                 uint32_t scissorCount,
                                 const RHIRect2D* pScissors)
{
    // rect_2d
    int rect_2d_size = scissorCount;
    std::vector<VkRect2D> vk_rect_2d_list(rect_2d_size);
    for (int i = 0; i < rect_2d_size; ++i)
    {
        const auto& rhi_rect_2d_element = pScissors[i];
        auto& vk_rect_2d_element = vk_rect_2d_list[i];

        VkOffset2D offset_2d {};
        offset_2d.x = rhi_rect_2d_element.offset.x;
        offset_2d.y = rhi_rect_2d_element.offset.y;

        VkExtent2D extent_2d {};
        extent_2d.width = rhi_rect_2d_element.extent.width;
        extent_2d.height = rhi_rect_2d_element.extent.height;

        vk_rect_2d_element.offset = (VkOffset2D)offset_2d;
        vk_rect_2d_element.extent = (VkExtent2D)extent_2d;
    };
    /*if (rect_2d_size > 1)
    {
        return vkCmdSetScissor(
            ((VulkanCommandBuffer*)commandBuffer)->getResource(), 1, scissorCount, vk_rect_2d_list.data());
    }*/
    return vkCmdSetScissor(
        ((VulkanCommandBuffer*)commandBuffer)->getResource(), firstScissor, scissorCount, vk_rect_2d_list.data());
}

void VulkanRHI::CmdBindVertexBuffersPFN(RHICommandBuffer* commandBuffer,
                                        uint32_t firstBinding,
                                        uint32_t bindingCount,
                                        RHIBuffer* const* pBuffers,
                                        const RHIDeviceSize* pOffsets)
{
    // buffer
    int buffer_size = bindingCount;
    std::vector<VkBuffer> vk_buffer_list(buffer_size);
    for (int i = 0; i < buffer_size; ++i)
    {
        const auto& rhi_buffer_element = pBuffers[i];
        auto& vk_buffer_element = vk_buffer_list[i];

        vk_buffer_element = ((VulkanBuffer*)rhi_buffer_element)->getResource();
    };

    // offset
    int offset_size = bindingCount;
    std::vector<VkDeviceSize> vk_device_size_list(offset_size);
    for (int i = 0; i < offset_size; ++i)
    {
        const auto& rhi_offset_element = pOffsets[i];
        auto& vk_offset_element = vk_device_size_list[i];

        vk_offset_element = rhi_offset_element;
    };

    return _vkCmdBindVertexBuffers(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                                   firstBinding,
                                   bindingCount,
                                   vk_buffer_list.data(),
                                   vk_device_size_list.data());
}

void VulkanRHI::CmdBindIndexBufferPFN(RHICommandBuffer* commandBuffer,
                                      RHIBuffer* buffer,
                                      RHIDeviceSize offset,
                                      RHIIndexType indexType)
{
    return _vkCmdBindIndexBuffer(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                                 ((VulkanBuffer*)buffer)->getResource(),
                                 (VkDeviceSize)offset,
                                 (VkIndexType)indexType);
}

void VulkanRHI::CmdBindDescriptorSetsPFN(RHICommandBuffer* commandBuffer,
                                         RHIPipelineBindPoint pipelineBindPoint,
                                         RHIPipelineLayout* layout,
                                         uint32_t firstSet,
                                         uint32_t descriptorSetCount,
                                         const RHIDescriptorSet* const* pDescriptorSets,
                                         uint32_t dynamicOffsetCount,
                                         const uint32_t* pDynamicOffsets)
{
    // descriptor_set
    int descriptor_set_size = descriptorSetCount;
    std::vector<VkDescriptorSet> vk_descriptor_set_list(descriptor_set_size);
    for (int i = 0; i < descriptor_set_size; ++i)
    {
        const auto& rhi_descriptor_set_element = pDescriptorSets[i];
        auto& vk_descriptor_set_element = vk_descriptor_set_list[i];

        vk_descriptor_set_element = ((VulkanDescriptorSet*)rhi_descriptor_set_element)->getResource();
    };

    // offset
    int offset_size = dynamicOffsetCount;
    std::vector<uint32_t> vk_offset_list(offset_size);
    for (int i = 0; i < offset_size; ++i)
    {
        const auto& rhi_offset_element = pDynamicOffsets[i];
        auto& vk_offset_element = vk_offset_list[i];

        vk_offset_element = rhi_offset_element;
    };

    return vkCmdBindDescriptorSets(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                                   (VkPipelineBindPoint)pipelineBindPoint,
                                   ((VulkanPipelineLayout*)layout)->getResource(),
                                   firstSet,
                                   descriptorSetCount,
                                   vk_descriptor_set_list.data(),
                                   dynamicOffsetCount,
                                   vk_offset_list.data());
}

void VulkanRHI::CmdSetBindlessIndexPFN(RHICommandBuffer* commandBuffer,
                                       RHIPipelineBindPoint /*pipelineBindPoint*/,
                                       RHIPipelineLayout* layout,
                                       uint32_t packed_index)
{
    // PR-V1: bindless-index push.
    //
    // Contract (mirrors DX12RHI::CmdSetBindlessIndexPFN):
    // - 'layout' MUST be the same VulkanPipelineLayout that the
    //   currently-bound pipeline was built against; the push-constant
    //   write targets that layout's reserved 4-byte range. Vulkan
    //   tolerates pushing into a layout the bound pipeline shares
    //   (push constants are layout-scoped, not pipeline-scoped), but
    //   the validation layer flags a mismatch loudly so any drift is
    //   caught at the first frame.
    // - We push to VK_SHADER_STAGE_ALL at offset 0 because that is
    //   the canonical bindless slot reserved by the bindless texture
    //   manager helper; if the layout did not declare such a range,
    //   the validation layer will fire VUID-vkCmdPushConstants-offset.
    //   We do not silently no-op like DX12 does (which can probe
    //   layout->usesBindless()) because Vulkan has no host-visible
    //   bindless flag on the layout -- failing loud at debug time is
    //   the better trade-off than swallowing a misuse.
    // - 'pipelineBindPoint' is unused: vkCmdPushConstants does not
    //   take a bind point (push constants are pipeline-bind-point
    //   agnostic, unlike DX12's split SetGraphics/Compute root32).
    if (commandBuffer == nullptr || layout == nullptr)
    {
        return;
    }
    VkCommandBuffer vk_command_buffer = static_cast<VulkanCommandBuffer*>(commandBuffer)->getResource();
    VkPipelineLayout vk_pipeline_layout = static_cast<VulkanPipelineLayout*>(layout)->getResource();
    if (vk_command_buffer == VK_NULL_HANDLE || vk_pipeline_layout == VK_NULL_HANDLE)
    {
        return;
    }
    vkCmdPushConstants(vk_command_buffer,
                       vk_pipeline_layout,
                       VK_SHADER_STAGE_ALL,
                       /*offset*/ 0,
                       /*size*/ sizeof(uint32_t),
                       &packed_index);
}

void VulkanRHI::CmdDrawIndexedPFN(RHICommandBuffer* commandBuffer,
                                  uint32_t indexCount,
                                  uint32_t instanceCount,
                                  uint32_t firstIndex,
                                  int32_t vertexOffset,
                                  uint32_t firstInstance)
{
    return _vkCmdDrawIndexed(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                             indexCount,
                             instanceCount,
                             firstIndex,
                             vertexOffset,
                             firstInstance);
}

void VulkanRHI::CmdClearAttachmentsPFN(RHICommandBuffer* commandBuffer,
                                       uint32_t attachmentCount,
                                       const RHIClearAttachment* pAttachments,
                                       uint32_t rectCount,
                                       const RHIClearRect* pRects)
{
    // clear_attachment
    int clear_attachment_size = attachmentCount;
    std::vector<VkClearAttachment> vk_clear_attachment_list(clear_attachment_size);
    for (int i = 0; i < clear_attachment_size; ++i)
    {
        const auto& rhi_clear_attachment_element = pAttachments[i];
        auto& vk_clear_attachment_element = vk_clear_attachment_list[i];

        VkClearColorValue vk_clear_color_value;
        vk_clear_color_value.float32[0] = rhi_clear_attachment_element.clearValue.color.float32[0];
        vk_clear_color_value.float32[1] = rhi_clear_attachment_element.clearValue.color.float32[1];
        vk_clear_color_value.float32[2] = rhi_clear_attachment_element.clearValue.color.float32[2];
        vk_clear_color_value.float32[3] = rhi_clear_attachment_element.clearValue.color.float32[3];
        vk_clear_color_value.int32[0] = rhi_clear_attachment_element.clearValue.color.int32[0];
        vk_clear_color_value.int32[1] = rhi_clear_attachment_element.clearValue.color.int32[1];
        vk_clear_color_value.int32[2] = rhi_clear_attachment_element.clearValue.color.int32[2];
        vk_clear_color_value.int32[3] = rhi_clear_attachment_element.clearValue.color.int32[3];
        vk_clear_color_value.uint32[0] = rhi_clear_attachment_element.clearValue.color.uint32[0];
        vk_clear_color_value.uint32[1] = rhi_clear_attachment_element.clearValue.color.uint32[1];
        vk_clear_color_value.uint32[2] = rhi_clear_attachment_element.clearValue.color.uint32[2];
        vk_clear_color_value.uint32[3] = rhi_clear_attachment_element.clearValue.color.uint32[3];

        VkClearDepthStencilValue vk_clear_depth_stencil_value;
        vk_clear_depth_stencil_value.depth = rhi_clear_attachment_element.clearValue.depthStencil.depth;
        vk_clear_depth_stencil_value.stencil = rhi_clear_attachment_element.clearValue.depthStencil.stencil;

        vk_clear_attachment_element.clearValue.color = vk_clear_color_value;
        vk_clear_attachment_element.clearValue.depthStencil = vk_clear_depth_stencil_value;
        vk_clear_attachment_element.aspectMask = rhi_clear_attachment_element.aspectMask;
        vk_clear_attachment_element.colorAttachment = rhi_clear_attachment_element.colorAttachment;
    };

    // clear_rect
    int clear_rect_size = rectCount;
    std::vector<VkClearRect> vk_clear_rect_list(clear_rect_size);
    for (int i = 0; i < clear_rect_size; ++i)
    {
        const auto& rhi_clear_rect_element = pRects[i];
        auto& vk_clear_rect_element = vk_clear_rect_list[i];

        VkOffset2D offset_2d {};
        offset_2d.x = rhi_clear_rect_element.rect.offset.x;
        offset_2d.y = rhi_clear_rect_element.rect.offset.y;

        VkExtent2D extent_2d {};
        extent_2d.width = rhi_clear_rect_element.rect.extent.width;
        extent_2d.height = rhi_clear_rect_element.rect.extent.height;

        vk_clear_rect_element.rect.offset = (VkOffset2D)offset_2d;
        vk_clear_rect_element.rect.extent = (VkExtent2D)extent_2d;
        vk_clear_rect_element.baseArrayLayer = rhi_clear_rect_element.baseArrayLayer;
        vk_clear_rect_element.layerCount = rhi_clear_rect_element.layerCount;
    };

    return _vkCmdClearAttachments(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                                  attachmentCount,
                                  vk_clear_attachment_list.data(),
                                  rectCount,
                                  vk_clear_rect_list.data());
}

bool VulkanRHI::BeginCommandBuffer(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo* pBeginInfo)
{
    VkCommandBufferInheritanceInfo command_buffer_inheritance_info {};
    const VkCommandBufferInheritanceInfo* command_buffer_inheritance_info_ptr = nullptr;
    if (pBeginInfo->pInheritanceInfo != nullptr)
    {
        command_buffer_inheritance_info.sType = (VkStructureType)(pBeginInfo->pInheritanceInfo->sType);
        command_buffer_inheritance_info.pNext = (const void*)pBeginInfo->pInheritanceInfo->pNext;
        command_buffer_inheritance_info.renderPass =
            ((VulkanRenderPass*)pBeginInfo->pInheritanceInfo->renderPass)->getResource();
        command_buffer_inheritance_info.subpass = pBeginInfo->pInheritanceInfo->subpass;
        command_buffer_inheritance_info.framebuffer =
            ((VulkanFramebuffer*)(pBeginInfo->pInheritanceInfo->framebuffer))->getResource();
        command_buffer_inheritance_info.occlusionQueryEnable =
            (VkBool32)pBeginInfo->pInheritanceInfo->occlusionQueryEnable;
        command_buffer_inheritance_info.queryFlags = (VkQueryControlFlags)pBeginInfo->pInheritanceInfo->queryFlags;
        command_buffer_inheritance_info.pipelineStatistics =
            (VkQueryPipelineStatisticFlags)pBeginInfo->pInheritanceInfo->pipelineStatistics;

        command_buffer_inheritance_info_ptr = &command_buffer_inheritance_info;
    }

    VkCommandBufferBeginInfo command_buffer_begin_info {};
    command_buffer_begin_info.sType = (VkStructureType)pBeginInfo->sType;
    command_buffer_begin_info.pNext = (const void*)pBeginInfo->pNext;
    command_buffer_begin_info.flags = (VkCommandBufferUsageFlags)pBeginInfo->flags;
    command_buffer_begin_info.pInheritanceInfo = command_buffer_inheritance_info_ptr;

    VkResult result =
        vkBeginCommandBuffer(((VulkanCommandBuffer*)commandBuffer)->getResource(), &command_buffer_begin_info);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkBeginCommandBuffer failed!");
        return false;
    }
}

bool VulkanRHI::EndCommandBuffer(RHICommandBuffer* commandBuffer)
{
    VkResult result = vkEndCommandBuffer(((VulkanCommandBuffer*)commandBuffer)->getResource());

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkEndCommandBuffer failed!");
        return false;
    }
}

void VulkanRHI::UpdateDescriptorSets(uint32_t descriptorWriteCount,
                                     const RHIWriteDescriptorSet* pDescriptorWrites,
                                     uint32_t descriptorCopyCount,
                                     const RHICopyDescriptorSet* pDescriptorCopies)
{
    // write_descriptor_set
    int write_descriptor_set_size = descriptorWriteCount;
    std::vector<VkWriteDescriptorSet> vk_write_descriptor_set_list(write_descriptor_set_size);
    int image_info_count = 0;
    int buffer_info_count = 0;
    for (int i = 0; i < write_descriptor_set_size; ++i)
    {
        const auto& rhi_write_descriptor_set_element = pDescriptorWrites[i];
        if (rhi_write_descriptor_set_element.pImageInfo != nullptr)
        {
            image_info_count++;
        }
        if (rhi_write_descriptor_set_element.pBufferInfo != nullptr)
        {
            buffer_info_count++;
        }
    }
    std::vector<VkDescriptorImageInfo> vk_descriptor_image_info_list(image_info_count);
    std::vector<VkDescriptorBufferInfo> vk_descriptor_buffer_info_list(buffer_info_count);
    int image_info_current = 0;
    int buffer_info_current = 0;

    for (int i = 0; i < write_descriptor_set_size; ++i)
    {
        const auto& rhi_write_descriptor_set_element = pDescriptorWrites[i];
        auto& vk_write_descriptor_set_element = vk_write_descriptor_set_list[i];

        const VkDescriptorImageInfo* vk_descriptor_image_info_ptr = nullptr;
        if (rhi_write_descriptor_set_element.pImageInfo != nullptr)
        {
            auto& vk_descriptor_image_info = vk_descriptor_image_info_list[image_info_current];
            if (rhi_write_descriptor_set_element.pImageInfo->sampler == nullptr)
            {
                vk_descriptor_image_info.sampler = nullptr;
            }
            else
            {
                vk_descriptor_image_info.sampler =
                    ((VulkanSampler*)rhi_write_descriptor_set_element.pImageInfo->sampler)->getResource();
            }
            vk_descriptor_image_info.imageView =
                ((VulkanImageView*)rhi_write_descriptor_set_element.pImageInfo->imageView)->getResource();
            vk_descriptor_image_info.imageLayout =
                (VkImageLayout)rhi_write_descriptor_set_element.pImageInfo->imageLayout;

            vk_descriptor_image_info_ptr = &vk_descriptor_image_info;
            image_info_current++;
        }

        const VkDescriptorBufferInfo* vk_descriptor_buffer_info_ptr = nullptr;
        if (rhi_write_descriptor_set_element.pBufferInfo != nullptr)
        {
            auto& vk_descriptor_buffer_info = vk_descriptor_buffer_info_list[buffer_info_current];
            vk_descriptor_buffer_info.buffer =
                ((VulkanBuffer*)rhi_write_descriptor_set_element.pBufferInfo->buffer)->getResource();
            vk_descriptor_buffer_info.offset = (VkDeviceSize)rhi_write_descriptor_set_element.pBufferInfo->offset;
            vk_descriptor_buffer_info.range = (VkDeviceSize)rhi_write_descriptor_set_element.pBufferInfo->range;

            vk_descriptor_buffer_info_ptr = &vk_descriptor_buffer_info;
            buffer_info_current++;
        }

        vk_write_descriptor_set_element.sType = (VkStructureType)rhi_write_descriptor_set_element.sType;
        vk_write_descriptor_set_element.pNext = (const void*)rhi_write_descriptor_set_element.pNext;
        vk_write_descriptor_set_element.dstSet =
            ((VulkanDescriptorSet*)rhi_write_descriptor_set_element.dstSet)->getResource();
        vk_write_descriptor_set_element.dstBinding = rhi_write_descriptor_set_element.dstBinding;
        vk_write_descriptor_set_element.dstArrayElement = rhi_write_descriptor_set_element.dstArrayElement;
        vk_write_descriptor_set_element.descriptorCount = rhi_write_descriptor_set_element.descriptorCount;
        vk_write_descriptor_set_element.descriptorType =
            (VkDescriptorType)rhi_write_descriptor_set_element.descriptorType;
        vk_write_descriptor_set_element.pImageInfo = vk_descriptor_image_info_ptr;
        vk_write_descriptor_set_element.pBufferInfo = vk_descriptor_buffer_info_ptr;
        // vk_write_descriptor_set_element.pTexelBufferView =
        // &((VulkanBufferView*)rhi_write_descriptor_set_element.pTexelBufferView)->getResource();
    };

    if (image_info_current != image_info_count || buffer_info_current != buffer_info_count)
    {
        LOG_ERROR(ZVulkan, "image_info_current != image_info_count || buffer_info_current != buffer_info_count");
        return;
    }

    // copy_descriptor_set
    int copy_descriptor_set_size = descriptorCopyCount;
    std::vector<VkCopyDescriptorSet> vk_copy_descriptor_set_list(copy_descriptor_set_size);
    for (int i = 0; i < copy_descriptor_set_size; ++i)
    {
        const auto& rhi_copy_descriptor_set_element = pDescriptorCopies[i];
        auto& vk_copy_descriptor_set_element = vk_copy_descriptor_set_list[i];

        vk_copy_descriptor_set_element.sType = (VkStructureType)rhi_copy_descriptor_set_element.sType;
        vk_copy_descriptor_set_element.pNext = (const void*)rhi_copy_descriptor_set_element.pNext;
        vk_copy_descriptor_set_element.srcSet =
            ((VulkanDescriptorSet*)rhi_copy_descriptor_set_element.srcSet)->getResource();
        vk_copy_descriptor_set_element.srcBinding = rhi_copy_descriptor_set_element.srcBinding;
        vk_copy_descriptor_set_element.srcArrayElement = rhi_copy_descriptor_set_element.srcArrayElement;
        vk_copy_descriptor_set_element.dstSet =
            ((VulkanDescriptorSet*)rhi_copy_descriptor_set_element.dstSet)->getResource();
        vk_copy_descriptor_set_element.dstBinding = rhi_copy_descriptor_set_element.dstBinding;
        vk_copy_descriptor_set_element.dstArrayElement = rhi_copy_descriptor_set_element.dstArrayElement;
        vk_copy_descriptor_set_element.descriptorCount = rhi_copy_descriptor_set_element.descriptorCount;
    };

    vkUpdateDescriptorSets(m_Device,
                           descriptorWriteCount,
                           vk_write_descriptor_set_list.data(),
                           descriptorCopyCount,
                           vk_copy_descriptor_set_list.data());
}

bool VulkanRHI::QueueSubmit(RHIQueue* queue, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence* fence)
{
    // submit_info
    int command_buffer_size_total = 0;
    int semaphore_size_total = 0;
    int signal_semaphore_size_total = 0;
    int pipeline_stage_flags_size_total = 0;

    int submit_info_size = submitCount;
    for (int i = 0; i < submit_info_size; ++i)
    {
        const auto& rhi_submit_info_element = pSubmits[i];
        command_buffer_size_total += rhi_submit_info_element.commandBufferCount;
        semaphore_size_total += rhi_submit_info_element.waitSemaphoreCount;
        signal_semaphore_size_total += rhi_submit_info_element.signalSemaphoreCount;
        pipeline_stage_flags_size_total += rhi_submit_info_element.waitSemaphoreCount;
    }
    std::vector<VkCommandBuffer> vk_command_buffer_list_external(command_buffer_size_total);
    std::vector<VkSemaphore> vk_semaphore_list_external(semaphore_size_total);
    std::vector<VkSemaphore> vk_signal_semaphore_list_external(signal_semaphore_size_total);
    std::vector<VkPipelineStageFlags> vk_pipeline_stage_flags_list_external(pipeline_stage_flags_size_total);

    int command_buffer_size_current = 0;
    int semaphore_size_current = 0;
    int signal_semaphore_size_current = 0;
    int pipeline_stage_flags_size_current = 0;

    std::vector<VkSubmitInfo> vk_submit_info_list(submit_info_size);
    for (int i = 0; i < submit_info_size; ++i)
    {
        const auto& rhi_submit_info_element = pSubmits[i];
        auto& vk_submit_info_element = vk_submit_info_list[i];

        vk_submit_info_element.sType = (VkStructureType)rhi_submit_info_element.sType;
        vk_submit_info_element.pNext = (const void*)rhi_submit_info_element.pNext;

        // command_buffer
        if (rhi_submit_info_element.commandBufferCount > 0)
        {
            vk_submit_info_element.commandBufferCount = rhi_submit_info_element.commandBufferCount;
            vk_submit_info_element.pCommandBuffers = &vk_command_buffer_list_external[command_buffer_size_current];
            int command_buffer_size = rhi_submit_info_element.commandBufferCount;
            for (int i = 0; i < command_buffer_size; ++i)
            {
                const auto& rhi_command_buffer_element = rhi_submit_info_element.pCommandBuffers[i];
                auto& vk_command_buffer_element = vk_command_buffer_list_external[command_buffer_size_current];

                vk_command_buffer_element = ((VulkanCommandBuffer*)rhi_command_buffer_element)->getResource();

                command_buffer_size_current++;
            };
        }

        // semaphore
        if (rhi_submit_info_element.waitSemaphoreCount > 0)
        {
            vk_submit_info_element.waitSemaphoreCount = rhi_submit_info_element.waitSemaphoreCount;
            vk_submit_info_element.pWaitSemaphores = &vk_semaphore_list_external[semaphore_size_current];
            int semaphore_size = rhi_submit_info_element.waitSemaphoreCount;
            for (int i = 0; i < semaphore_size; ++i)
            {
                const auto& rhi_semaphore_element = rhi_submit_info_element.pWaitSemaphores[i];
                auto& vk_semaphore_element = vk_semaphore_list_external[semaphore_size_current];

                vk_semaphore_element = ((VulkanSemaphore*)rhi_semaphore_element)->getResource();

                semaphore_size_current++;
            };
        }

        // signal_semaphore
        if (rhi_submit_info_element.signalSemaphoreCount > 0)
        {
            vk_submit_info_element.signalSemaphoreCount = rhi_submit_info_element.signalSemaphoreCount;
            vk_submit_info_element.pSignalSemaphores =
                &vk_signal_semaphore_list_external[signal_semaphore_size_current];
            int signal_semaphore_size = rhi_submit_info_element.signalSemaphoreCount;
            for (int i = 0; i < signal_semaphore_size; ++i)
            {
                const auto& rhi_signal_semaphore_element = rhi_submit_info_element.pSignalSemaphores[i];
                auto& vk_signal_semaphore_element = vk_signal_semaphore_list_external[signal_semaphore_size_current];

                vk_signal_semaphore_element = ((VulkanSemaphore*)rhi_signal_semaphore_element)->getResource();

                signal_semaphore_size_current++;
            };
        }

        // pipeline_stage_flags
        if (rhi_submit_info_element.waitSemaphoreCount > 0)
        {
            vk_submit_info_element.pWaitDstStageMask =
                &vk_pipeline_stage_flags_list_external[pipeline_stage_flags_size_current];
            int pipeline_stage_flags_size = rhi_submit_info_element.waitSemaphoreCount;
            for (int i = 0; i < pipeline_stage_flags_size; ++i)
            {
                const auto& rhi_pipeline_stage_flags_element = rhi_submit_info_element.pWaitDstStageMask[i];
                auto& vk_pipeline_stage_flags_element =
                    vk_pipeline_stage_flags_list_external[pipeline_stage_flags_size_current];

                vk_pipeline_stage_flags_element = (VkPipelineStageFlags)rhi_pipeline_stage_flags_element;

                pipeline_stage_flags_size_current++;
            };
        }
    };

    if ((command_buffer_size_total != command_buffer_size_current) ||
        (semaphore_size_total != semaphore_size_current) ||
        (signal_semaphore_size_total != signal_semaphore_size_current) ||
        (pipeline_stage_flags_size_total != pipeline_stage_flags_size_current))
    {
        LOG_ERROR(ZVulkan, "submit info is not right!");
        return false;
    }

    VkFence vk_fence = VK_NULL_HANDLE;
    if (fence != nullptr)
    {
        vk_fence = ((VulkanFence*)fence)->getResource();
    }

    VkResult result =
        vkQueueSubmit(((VulkanQueue*)queue)->getResource(), submitCount, vk_submit_info_list.data(), vk_fence);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        switch (result)
        {
            case VK_ERROR_DEVICE_LOST:
                LOG_FATAL(ZVulkan,
                          "vkQueueSubmit failed with VK_ERROR_DEVICE_LOST - The logical or physical device has "
                          "been lost. This may indicate a driver crash or hardware failure.");
                // Device lost is a fatal error - the application should handle this appropriately
                // Consider implementing device recovery or graceful shutdown
                break;
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                LOG_FATAL(ZVulkan,
                          "vkQueueSubmit failed with VK_ERROR_OUT_OF_HOST_MEMORY - Host memory allocation failed");
                break;
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                LOG_FATAL(ZVulkan,
                          "vkQueueSubmit failed with VK_ERROR_OUT_OF_DEVICE_MEMORY - Device memory allocation failed");
                break;
            case VK_ERROR_INITIALIZATION_FAILED:
                LOG_FATAL(ZVulkan, "vkQueueSubmit failed with VK_ERROR_INITIALIZATION_FAILED - Initialization failed");
                break;
            default:
                LOG_FATAL(ZVulkan, "vkQueueSubmit failed with VkResult: Unknown error {}", (int)result);
                break;
        }
        return false;
    }
}

bool VulkanRHI::QueueWaitIdle(RHIQueue* queue)
{
    VkResult result = vkQueueWaitIdle(((VulkanQueue*)queue)->getResource());

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkQueueWaitIdle failed!");
        return false;
    }
}

void VulkanRHI::CmdPipelineBarrier(RHICommandBuffer* commandBuffer,
                                   RHIPipelineStageFlags srcStageMask,
                                   RHIPipelineStageFlags dstStageMask,
                                   RHIDependencyFlags dependencyFlags,
                                   uint32_t memoryBarrierCount,
                                   const RHIMemoryBarrier* pMemoryBarriers,
                                   uint32_t bufferMemoryBarrierCount,
                                   const RHIBufferMemoryBarrier* pBufferMemoryBarriers,
                                   uint32_t imageMemoryBarrierCount,
                                   const RHIImageMemoryBarrier* pImageMemoryBarriers)
{
    // memory_barrier
    int memory_barrier_size = memoryBarrierCount;
    std::vector<VkMemoryBarrier> vk_memory_barrier_list(memory_barrier_size);
    for (int i = 0; i < memory_barrier_size; ++i)
    {
        const auto& rhi_memory_barrier_element = pMemoryBarriers[i];
        auto& vk_memory_barrier_element = vk_memory_barrier_list[i];

        vk_memory_barrier_element.sType = (VkStructureType)rhi_memory_barrier_element.sType;
        vk_memory_barrier_element.pNext = (const void*)rhi_memory_barrier_element.pNext;
        vk_memory_barrier_element.srcAccessMask = (VkAccessFlags)rhi_memory_barrier_element.srcAccessMask;
        vk_memory_barrier_element.dstAccessMask = (VkAccessFlags)rhi_memory_barrier_element.dstAccessMask;
    };

    // buffer_memory_barrier
    int buffer_memory_barrier_size = bufferMemoryBarrierCount;
    std::vector<VkBufferMemoryBarrier> vk_buffer_memory_barrier_list(buffer_memory_barrier_size);
    for (int i = 0; i < buffer_memory_barrier_size; ++i)
    {
        const auto& rhi_buffer_memory_barrier_element = pBufferMemoryBarriers[i];
        auto& vk_buffer_memory_barrier_element = vk_buffer_memory_barrier_list[i];

        vk_buffer_memory_barrier_element.sType = (VkStructureType)rhi_buffer_memory_barrier_element.sType;
        vk_buffer_memory_barrier_element.pNext = (const void*)rhi_buffer_memory_barrier_element.pNext;
        vk_buffer_memory_barrier_element.srcAccessMask = (VkAccessFlags)rhi_buffer_memory_barrier_element.srcAccessMask;
        vk_buffer_memory_barrier_element.dstAccessMask = (VkAccessFlags)rhi_buffer_memory_barrier_element.dstAccessMask;
        vk_buffer_memory_barrier_element.srcQueueFamilyIndex = rhi_buffer_memory_barrier_element.srcQueueFamilyIndex;
        vk_buffer_memory_barrier_element.dstQueueFamilyIndex = rhi_buffer_memory_barrier_element.dstQueueFamilyIndex;
        vk_buffer_memory_barrier_element.buffer =
            ((VulkanBuffer*)rhi_buffer_memory_barrier_element.buffer)->getResource();
        vk_buffer_memory_barrier_element.offset = (VkDeviceSize)rhi_buffer_memory_barrier_element.offset;
        vk_buffer_memory_barrier_element.size = (VkDeviceSize)rhi_buffer_memory_barrier_element.size;
    };

    // image_memory_barrier
    int image_memory_barrier_size = imageMemoryBarrierCount;
    std::vector<VkImageMemoryBarrier> vk_image_memory_barrier_list(image_memory_barrier_size);
    for (int i = 0; i < image_memory_barrier_size; ++i)
    {
        const auto& rhi_image_memory_barrier_element = pImageMemoryBarriers[i];
        auto& vk_image_memory_barrier_element = vk_image_memory_barrier_list[i];

        VkImageSubresourceRange image_subresource_range {};
        image_subresource_range.aspectMask =
            (VkImageAspectFlags)rhi_image_memory_barrier_element.subresourceRange.aspectMask;
        image_subresource_range.baseMipLevel = rhi_image_memory_barrier_element.subresourceRange.baseMipLevel;
        image_subresource_range.levelCount = rhi_image_memory_barrier_element.subresourceRange.levelCount;
        image_subresource_range.baseArrayLayer = rhi_image_memory_barrier_element.subresourceRange.baseArrayLayer;
        image_subresource_range.layerCount = rhi_image_memory_barrier_element.subresourceRange.layerCount;

        vk_image_memory_barrier_element.sType = (VkStructureType)rhi_image_memory_barrier_element.sType;
        vk_image_memory_barrier_element.pNext = (const void*)rhi_image_memory_barrier_element.pNext;
        vk_image_memory_barrier_element.srcAccessMask = (VkAccessFlags)rhi_image_memory_barrier_element.srcAccessMask;
        vk_image_memory_barrier_element.dstAccessMask = (VkAccessFlags)rhi_image_memory_barrier_element.dstAccessMask;
        vk_image_memory_barrier_element.oldLayout = (VkImageLayout)rhi_image_memory_barrier_element.oldLayout;
        vk_image_memory_barrier_element.newLayout = (VkImageLayout)rhi_image_memory_barrier_element.newLayout;
        vk_image_memory_barrier_element.srcQueueFamilyIndex = rhi_image_memory_barrier_element.srcQueueFamilyIndex;
        vk_image_memory_barrier_element.dstQueueFamilyIndex = rhi_image_memory_barrier_element.dstQueueFamilyIndex;
        vk_image_memory_barrier_element.image = ((VulkanImage*)rhi_image_memory_barrier_element.image)->getResource();
        vk_image_memory_barrier_element.subresourceRange = image_subresource_range;
    };

    vkCmdPipelineBarrier(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                         (RHIPipelineStageFlags)srcStageMask,
                         (RHIPipelineStageFlags)dstStageMask,
                         (RHIDependencyFlags)dependencyFlags,
                         memoryBarrierCount,
                         vk_memory_barrier_list.data(),
                         bufferMemoryBarrierCount,
                         vk_buffer_memory_barrier_list.data(),
                         imageMemoryBarrierCount,
                         vk_image_memory_barrier_list.data());
}

void VulkanRHI::CmdDraw(RHICommandBuffer* commandBuffer,
                        uint32_t vertexCount,
                        uint32_t instanceCount,
                        uint32_t firstVertex,
                        uint32_t firstInstance)
{
    vkCmdDraw(
        ((VulkanCommandBuffer*)commandBuffer)->getResource(), vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanRHI::CmdDispatch(RHICommandBuffer* commandBuffer,
                            uint32_t groupCountX,
                            uint32_t groupCountY,
                            uint32_t groupCountZ)
{
    vkCmdDispatch(((VulkanCommandBuffer*)commandBuffer)->getResource(), groupCountX, groupCountY, groupCountZ);
}

void VulkanRHI::CmdDispatchIndirect(RHICommandBuffer* commandBuffer, RHIBuffer* buffer, RHIDeviceSize offset)
{
    vkCmdDispatchIndirect(
        ((VulkanCommandBuffer*)commandBuffer)->getResource(), ((VulkanBuffer*)buffer)->getResource(), offset);
}

void VulkanRHI::CmdCopyImageToBuffer(RHICommandBuffer* commandBuffer,
                                     RHIImage* srcImage,
                                     RHIImageLayout srcImageLayout,
                                     RHIBuffer* dstBuffer,
                                     uint32_t regionCount,
                                     const RHIBufferImageCopy* pRegions)
{
    // buffer_image_copy
    int buffer_image_copy_size = regionCount;
    std::vector<VkBufferImageCopy> vk_buffer_image_copy_list(buffer_image_copy_size);
    for (int i = 0; i < buffer_image_copy_size; ++i)
    {
        const auto& rhi_buffer_image_copy_element = pRegions[i];
        auto& vk_buffer_image_copy_element = vk_buffer_image_copy_list[i];

        VkImageSubresourceLayers image_subresource_layers {};
        image_subresource_layers.aspectMask =
            (VkImageAspectFlags)rhi_buffer_image_copy_element.imageSubresource.aspectMask;
        image_subresource_layers.mipLevel = rhi_buffer_image_copy_element.imageSubresource.mipLevel;
        image_subresource_layers.baseArrayLayer = rhi_buffer_image_copy_element.imageSubresource.baseArrayLayer;
        image_subresource_layers.layerCount = rhi_buffer_image_copy_element.imageSubresource.layerCount;

        VkOffset3D offset_3d {};
        offset_3d.x = rhi_buffer_image_copy_element.imageOffset.x;
        offset_3d.y = rhi_buffer_image_copy_element.imageOffset.y;
        offset_3d.z = rhi_buffer_image_copy_element.imageOffset.z;

        VkExtent3D extent_3d {};
        extent_3d.width = rhi_buffer_image_copy_element.imageExtent.width;
        extent_3d.height = rhi_buffer_image_copy_element.imageExtent.height;
        extent_3d.depth = rhi_buffer_image_copy_element.imageExtent.depth;

        VkBufferImageCopy buffer_image_copy {};
        buffer_image_copy.bufferOffset = (VkDeviceSize)rhi_buffer_image_copy_element.bufferOffset;
        buffer_image_copy.bufferRowLength = rhi_buffer_image_copy_element.bufferRowLength;
        buffer_image_copy.bufferImageHeight = rhi_buffer_image_copy_element.bufferImageHeight;
        buffer_image_copy.imageSubresource = image_subresource_layers;
        buffer_image_copy.imageOffset = offset_3d;
        buffer_image_copy.imageExtent = extent_3d;

        vk_buffer_image_copy_element.bufferOffset = (VkDeviceSize)rhi_buffer_image_copy_element.bufferOffset;
        vk_buffer_image_copy_element.bufferRowLength = rhi_buffer_image_copy_element.bufferRowLength;
        vk_buffer_image_copy_element.bufferImageHeight = rhi_buffer_image_copy_element.bufferImageHeight;
        vk_buffer_image_copy_element.imageSubresource = image_subresource_layers;
        vk_buffer_image_copy_element.imageOffset = offset_3d;
        vk_buffer_image_copy_element.imageExtent = extent_3d;
    };

    vkCmdCopyImageToBuffer(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                           ((VulkanImage*)srcImage)->getResource(),
                           (VkImageLayout)srcImageLayout,
                           ((VulkanBuffer*)dstBuffer)->getResource(),
                           regionCount,
                           vk_buffer_image_copy_list.data());
}

void VulkanRHI::CmdCopyImageToImage(RHICommandBuffer* commandBuffer,
                                    RHIImage* srcImage,
                                    RHIImageAspectFlagBits srcFlag,
                                    RHIImage* dstImage,
                                    RHIImageAspectFlagBits dstFlag,
                                    uint32_t width,
                                    uint32_t height)
{
    VkImageCopy imagecopyRegion = {};
    imagecopyRegion.srcSubresource = {(VkImageAspectFlags)srcFlag, 0, 0, 1};
    imagecopyRegion.srcOffset = {0, 0, 0};
    imagecopyRegion.dstSubresource = {(VkImageAspectFlags)dstFlag, 0, 0, 1};
    imagecopyRegion.dstOffset = {0, 0, 0};
    imagecopyRegion.extent = {width, height, 1};

    vkCmdCopyImage(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                   ((VulkanImage*)srcImage)->getResource(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   ((VulkanImage*)dstImage)->getResource(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &imagecopyRegion);
}

void VulkanRHI::CmdBlitImage(RHICommandBuffer* commandBuffer,
                             RHIImage* srcImage,
                             RHIImageLayout srcImageLayout,
                             RHIImage* dstImage,
                             RHIImageLayout dstImageLayout,
                             uint32_t srcX0,
                             uint32_t srcY0,
                             uint32_t srcX1,
                             uint32_t srcY1,
                             uint32_t dstX0,
                             uint32_t dstY0,
                             uint32_t dstX1,
                             uint32_t dstY1,
                             RHIFilter filter)
{
    VkImageBlit blitRegion {};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.mipLevel = 0;
    blitRegion.srcSubresource.baseArrayLayer = 0;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcOffsets[0] = {static_cast<int32_t>(srcX0), static_cast<int32_t>(srcY0), 0};
    blitRegion.srcOffsets[1] = {static_cast<int32_t>(srcX1), static_cast<int32_t>(srcY1), 1};

    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.mipLevel = 0;
    blitRegion.dstSubresource.baseArrayLayer = 0;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstOffsets[0] = {static_cast<int32_t>(dstX0), static_cast<int32_t>(dstY0), 0};
    blitRegion.dstOffsets[1] = {static_cast<int32_t>(dstX1), static_cast<int32_t>(dstY1), 1};

    vkCmdBlitImage(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                   ((VulkanImage*)srcImage)->getResource(),
                   (VkImageLayout)srcImageLayout,
                   ((VulkanImage*)dstImage)->getResource(),
                   (VkImageLayout)dstImageLayout,
                   1,
                   &blitRegion,
                   (VkFilter)filter);
}

void VulkanRHI::CmdCopyBuffer(RHICommandBuffer* commandBuffer,
                              RHIBuffer* srcBuffer,
                              RHIBuffer* dstBuffer,
                              uint32_t regionCount,
                              RHIBufferCopy* pRegions)
{
    VkBufferCopy copyRegion {};
    copyRegion.srcOffset = pRegions->srcOffset;
    copyRegion.dstOffset = pRegions->dstOffset;
    copyRegion.size = pRegions->size;

    vkCmdCopyBuffer(((VulkanCommandBuffer*)commandBuffer)->getResource(),
                    ((VulkanBuffer*)srcBuffer)->getResource(),
                    ((VulkanBuffer*)dstBuffer)->getResource(),
                    regionCount,
                    &copyRegion);
}

void VulkanRHI::CreateCommandBuffers()
{
    VkCommandBufferAllocateInfo command_buffer_allocate_info {};
    command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandBufferCount = 1U;

    for (uint32_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        command_buffer_allocate_info.commandPool = m_CommandPools[i];
        VkCommandBuffer vk_command_buffer;
        if (vkAllocateCommandBuffers(m_Device, &command_buffer_allocate_info, &vk_command_buffer) != VK_SUCCESS)
        {
            LOG_ERROR(ZVulkan, "vk allocate command buffers");
        }
        m_VkCommandBuffers[i] = vk_command_buffer;
        m_CommandBuffers[i] = new VulkanCommandBuffer();
        ((VulkanCommandBuffer*)m_CommandBuffers[i])->setResource(vk_command_buffer);
    }
}

void VulkanRHI::CreateDescriptorPool()
{
    // Since DescriptorSet should be treated as asset in Vulkan, DescriptorPool
    // should be big enough, and thus we can sub-allocate DescriptorSet from
    // DescriptorPool merely as we sub-allocate Buffer/Image from DeviceMemory.

    // ImGui Vulkan backend minimums (imgui_impl_vulkan.h): 8 SAMPLED_IMAGE + 2 SAMPLER,
    // plus headroom for dynamic font/user textures (bindless preview, etc.).
    static constexpr uint32_t kImGuiSampledImageDescriptors = 16;
    static constexpr uint32_t kImGuiSamplerDescriptors = 2;

    VkDescriptorPoolSize pool_sizes[9];
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    pool_sizes[0].descriptorCount = 3 + 2 + 2 + 2 + 1 + 1 + 3 + 3;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 1 + 1 + 1 * m_MaxVertexBlendingMeshCount;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[2].descriptorCount = 1 * m_MaxMaterialCount;
    pool_sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[3].descriptorCount = 3 + 5 * m_MaxMaterialCount;
    pool_sizes[4].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    pool_sizes[4].descriptorCount = 4 + 1 + 1 + 2;
    pool_sizes[5].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    pool_sizes[5].descriptorCount = 3;
    pool_sizes[6].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[6].descriptorCount = 1;
    pool_sizes[7].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[7].descriptorCount = kImGuiSampledImageDescriptors;
    pool_sizes[8].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[8].descriptorCount = kImGuiSamplerDescriptors;

    VkDescriptorPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]);
    pool_info.pPoolSizes = pool_sizes;
    pool_info.maxSets = 1 + 1 + 1 + m_MaxMaterialCount + m_MaxVertexBlendingMeshCount + 1 + 1 +
                        kImGuiSampledImageDescriptors + kImGuiSamplerDescriptors;  // +skybox + axis + ImGui
    // ImGui_ImplVulkan_AddTexture / font atlas need vkFreeDescriptorSets (see imgui_impl_vulkan.h).
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (vkCreateDescriptorPool(m_Device, &pool_info, nullptr, &m_VkDescriptorPool) != VK_SUCCESS)
    {
        LOG_ERROR(ZVulkan, "create descriptor pool");
    }

    m_DescriptorPool = new VulkanDescriptorPool();
    ((VulkanDescriptorPool*)m_DescriptorPool)->setResource(m_VkDescriptorPool);
}

// semaphore : signal an image is ready for rendering // ready for presentation
// (m_VulkanContext._swapchain_images --> semaphores, fences)
void VulkanRHI::CreateSyncPrimitives()
{
    VkSemaphoreCreateInfo semaphore_create_info {};
    semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_create_info {};
    fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // the fence is initialized as signaled

    for (uint32_t i = 0; i < k_max_frames_in_flight; i++)
    {
        m_ImageAvailableForTexturescopySemaphores[i] = new VulkanSemaphore();
        if (vkCreateSemaphore(m_Device, &semaphore_create_info, nullptr, &m_ImageAvailableForRenderSemaphores[i]) !=
                VK_SUCCESS ||
            vkCreateSemaphore(
                m_Device, &semaphore_create_info, nullptr, &m_ImageFinishedForPresentationSemaphores[i]) !=
                VK_SUCCESS ||
            vkCreateSemaphore(m_Device,
                              &semaphore_create_info,
                              nullptr,
                              &(((VulkanSemaphore*)m_ImageAvailableForTexturescopySemaphores[i])->getResource())) !=
                VK_SUCCESS ||
            vkCreateFence(m_Device, &fence_create_info, nullptr, &m_IsFrameInFlightFences[i]) != VK_SUCCESS)
        {
            LOG_ERROR(ZVulkan, "vk create semaphore & fence");
        }

        m_RhiIsFrameInFlightFences[i] = new VulkanFence();
        ((VulkanFence*)m_RhiIsFrameInFlightFences[i])->setResource(m_IsFrameInFlightFences[i]);
    }
}

void VulkanRHI::CreateFramebufferImageAndView()
{
    VulkanUtil::CreateImage(m_PhysicalDevice,
                            m_Device,
                            m_SwapchainExtent.width,
                            m_SwapchainExtent.height,
                            (VkFormat)m_DepthImageFormat,
                            VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                            ((VulkanImage*)m_DepthImage)->getResource(),
                            m_DepthImageMemory,
                            0,
                            1,
                            1);

    ((VulkanImageView*)m_DepthImageView)
        ->setResource(VulkanUtil::CreateImageView(m_Device,
                                                  ((VulkanImage*)m_DepthImage)->getResource(),
                                                  (VkFormat)m_DepthImageFormat,
                                                  VK_IMAGE_ASPECT_DEPTH_BIT,
                                                  VK_IMAGE_VIEW_TYPE_2D,
                                                  1,
                                                  1));
}

RHISampler* VulkanRHI::GetOrCreateDefaultSampler(RHIDefaultSamplerType type)
{
    switch (type)
    {
        case Default_Sampler_Linear:
            if (m_LinearSampler == nullptr)
            {
                m_LinearSampler = new VulkanSampler();
                ((VulkanSampler*)m_LinearSampler)
                    ->setResource(VulkanUtil::GetOrCreateLinearSampler(m_PhysicalDevice, m_Device));
            }
            return m_LinearSampler;
            break;

        case Default_Sampler_Nearest:
            if (m_NearestSampler == nullptr)
            {
                m_NearestSampler = new VulkanSampler();
                ((VulkanSampler*)m_NearestSampler)
                    ->setResource(VulkanUtil::GetOrCreateNearestSampler(m_PhysicalDevice, m_Device));
            }
            return m_NearestSampler;
            break;

        default:
            return nullptr;
            break;
    }
}

RHISampler* VulkanRHI::GetOrCreateMipmapSampler(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        LOG_ERROR(ZVulkan, "width == 0 || height == 0");
        return nullptr;
    }
    RHISampler* sampler;
    uint32_t mip_levels = floor(log2(std::max(width, height))) + 1;
    auto find_sampler = m_MipmapSamplerMap.find(mip_levels);
    if (find_sampler != m_MipmapSamplerMap.end())
    {
        return find_sampler->second;
    }
    else
    {
        sampler = new VulkanSampler();

        VkSampler vk_sampler = VulkanUtil::GetOrCreateMipmapSampler(m_PhysicalDevice, m_Device, width, height);

        ((VulkanSampler*)sampler)->setResource(vk_sampler);

        m_MipmapSamplerMap.insert(std::make_pair(mip_levels, sampler));

        return sampler;
    }
}

RHIShader* VulkanRHI::CreateShaderModule(const std::vector<unsigned char>& shader_code)
{
    RHIShader* shahder = new VulkanShader();

    VkShaderModule vk_shader = VulkanUtil::CreateShaderModule(m_Device, shader_code);

    ((VulkanShader*)shahder)->setResource(vk_shader);

    return shahder;
}

RHIShader* VulkanRHI::CreateShaderModuleFromFile(const std::string& file_path,
                                                 ShaderStage shader_stage,
                                                 const std::vector<std::string>& include_paths,
                                                 const ShaderMacros& macros,
                                                 std::vector<uint8_t>& output_binary,
                                                 const std::string& entry_point,
                                                 bool /*embed_debug*/)
{
    // Initialize shader compiler if not already initialized
    if (!m_ShaderCompiler)
    {
        m_ShaderCompiler = std::make_unique<ShaderCompiler>();
    }

    // Compile shader from file
    // Note: RHI::ShaderMacros and ShaderCompiler::ShaderMacros are both std::map<std::string, std::string>
    // so we can pass macros directly after converting to the compiler's type
    ShaderMacros compiler_macros(macros.begin(), macros.end());

    (void)entry_point;
    // TODO: pass embed_debug to ShaderCompiler::CompileFromFile to enable
    //       -g flag (SPIR-V debug info) when embed_debug == true.
    ShaderCompileResult result =
        m_ShaderCompiler->CompileFromFile(file_path, shader_stage, include_paths, compiler_macros);

    if (!result.success)
    {
        LOG_ERROR(ZShader, "Failed to compile shader from file: {}", file_path);
        LOG_ERROR(ZShader, "Error: {}", result.error_message);
        return nullptr;
    }

    // Optionally copy SPIR-V bytecode to caller.
    if (!result.spirv_code.empty())
    {
        output_binary = result.spirv_code;
    }

    // Create shader module from compiled SPIR-V
    return CreateShaderModule(result.spirv_code);
}

RHIShader* VulkanRHI::CreateShaderModuleFromSource(const std::string& source_code,
                                                   ShaderStage shader_stage,
                                                   const std::string& shader_name,
                                                   const std::vector<std::string>& include_paths,
                                                   const ShaderMacros& macros)
{
    // Initialize shader compiler if not already initialized
    if (!m_ShaderCompiler)
    {
        m_ShaderCompiler = std::make_unique<ShaderCompiler>();
    }

    // Compile shader from source
    // Note: RHI::ShaderMacros and ShaderCompiler::ShaderMacros are both std::map<std::string, std::string>
    ShaderMacros compiler_macros(macros.begin(), macros.end());

    ShaderCompileResult result = m_ShaderCompiler->CompileFromSource(
        source_code, shader_stage, shader_name, include_paths, compiler_macros);

    if (!result.success)
    {
        // Route the diagnostic through BqLog (LOG_ERROR), not std::cerr --
        // ZEditor is built /SUBSYSTEM:WINDOWS so stderr is not connected,
        // and the actual glslang diagnostic was being swallowed silently.
        LOG_ERROR(ZVulkan,
                  "Failed to compile shader from source. shader_name='{}', stage={}\n"
                  "glslang error:\n{}",
                  shader_name.empty() ? "<unnamed>" : shader_name,
                  static_cast<int>(shader_stage),
                  result.error_message);
        return nullptr;
    }

    // Create shader module from compiled SPIR-V
    return CreateShaderModule(result.spirv_code);
}

void VulkanRHI::CreateBuffer(RHIDeviceSize size,
                             RHIBufferUsageFlags usage,
                             RHIMemoryPropertyFlags properties,
                             RHIBuffer*& buffer,
                             RHIDeviceMemory*& buffer_memory)
{
    VkBuffer vk_buffer;
    VkDeviceMemory vk_device_memory;

    VulkanUtil::CreateBuffer(m_PhysicalDevice, m_Device, size, usage, properties, vk_buffer, vk_device_memory);

    buffer = new VulkanBuffer();
    buffer_memory = new VulkanDeviceMemory();
    ((VulkanBuffer*)buffer)->setResource(vk_buffer);
    ((VulkanDeviceMemory*)buffer_memory)->setResource(vk_device_memory);
}

void VulkanRHI::CreateBufferAndInitialize(RHIBufferUsageFlags usage,
                                          RHIMemoryPropertyFlags properties,
                                          RHIBuffer*& buffer,
                                          RHIDeviceMemory*& buffer_memory,
                                          RHIDeviceSize size,
                                          void* data,
                                          int datasize)
{
    VkBuffer vk_buffer;
    VkDeviceMemory vk_device_memory;

    VulkanUtil::CreateBufferAndInitialize(
        m_Device, m_PhysicalDevice, usage, properties, &vk_buffer, &vk_device_memory, size, data, datasize);

    buffer = new VulkanBuffer();
    buffer_memory = new VulkanDeviceMemory();
    ((VulkanBuffer*)buffer)->setResource(vk_buffer);
    ((VulkanDeviceMemory*)buffer_memory)->setResource(vk_device_memory);
}

bool VulkanRHI::CreateBufferVMA(void* allocator,
                                const RHIBufferCreateInfo* pBufferCreateInfo,
                                void* pAllocationCreateInfo,
                                RHIBuffer*& pBuffer,
                                void** pAllocation,
                                void* pAllocationInfo)
{
    VkBuffer vk_buffer;
    VkBufferCreateInfo buffer_create_info {};
    buffer_create_info.sType = (VkStructureType)pBufferCreateInfo->sType;
    buffer_create_info.pNext = (const void*)pBufferCreateInfo->pNext;
    buffer_create_info.flags = (VkBufferCreateFlags)pBufferCreateInfo->flags;
    buffer_create_info.size = (VkDeviceSize)pBufferCreateInfo->size;
    buffer_create_info.usage = (VkBufferUsageFlags)pBufferCreateInfo->usage;
    buffer_create_info.sharingMode = (VkSharingMode)pBufferCreateInfo->sharingMode;
    buffer_create_info.queueFamilyIndexCount = pBufferCreateInfo->queueFamilyIndexCount;
    buffer_create_info.pQueueFamilyIndices = (const uint32_t*)pBufferCreateInfo->pQueueFamilyIndices;

    pBuffer = new VulkanBuffer();
    VkResult result = vmaCreateBuffer((VmaAllocator)allocator,
                                      &buffer_create_info,
                                      (VmaAllocationCreateInfo*)pAllocationCreateInfo,
                                      &vk_buffer,
                                      (VmaAllocation*)pAllocation,
                                      (VmaAllocationInfo*)pAllocationInfo);

    ((VulkanBuffer*)pBuffer)->setResource(vk_buffer);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool VulkanRHI::CreateBufferWithAlignmentVMA(void* allocator,
                                             const RHIBufferCreateInfo* pBufferCreateInfo,
                                             void* pAllocationCreateInfo,
                                             RHIDeviceSize minAlignment,
                                             RHIBuffer*& pBuffer,
                                             void** pAllocation,
                                             void* pAllocationInfo)
{
    VkBuffer vk_buffer;
    VkBufferCreateInfo buffer_create_info {};
    buffer_create_info.sType = (VkStructureType)pBufferCreateInfo->sType;
    buffer_create_info.pNext = (const void*)pBufferCreateInfo->pNext;
    buffer_create_info.flags = (VkBufferCreateFlags)pBufferCreateInfo->flags;
    buffer_create_info.size = (VkDeviceSize)pBufferCreateInfo->size;
    buffer_create_info.usage = (VkBufferUsageFlags)pBufferCreateInfo->usage;
    buffer_create_info.sharingMode = (VkSharingMode)pBufferCreateInfo->sharingMode;
    buffer_create_info.queueFamilyIndexCount = pBufferCreateInfo->queueFamilyIndexCount;
    buffer_create_info.pQueueFamilyIndices = (const uint32_t*)pBufferCreateInfo->pQueueFamilyIndices;

    pBuffer = new VulkanBuffer();
    VkResult result = vmaCreateBufferWithAlignment((VmaAllocator)allocator,
                                                   &buffer_create_info,
                                                   (VmaAllocationCreateInfo*)pAllocationCreateInfo,
                                                   minAlignment,
                                                   &vk_buffer,
                                                   (VmaAllocation*)pAllocation,
                                                   (VmaAllocationInfo*)pAllocationInfo);

    ((VulkanBuffer*)pBuffer)->setResource(vk_buffer);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vmaCreateBufferWithAlignment failed!");
        return false;
    }
}

void VulkanRHI::CopyBuffer(RHIBuffer* srcBuffer,
                           RHIBuffer* dstBuffer,
                           RHIDeviceSize srcOffset,
                           RHIDeviceSize dstOffset,
                           RHIDeviceSize size)
{
    VkBuffer vk_src_buffer = ((VulkanBuffer*)srcBuffer)->getResource();
    VkBuffer vk_dst_buffer = ((VulkanBuffer*)dstBuffer)->getResource();
    VulkanUtil::CopyBuffer(this, vk_src_buffer, vk_dst_buffer, srcOffset, dstOffset, size);
}

void VulkanRHI::CreateImage(uint32_t image_width,
                            uint32_t image_height,
                            RHIFormat format,
                            RHIImageTiling image_tiling,
                            RHIImageUsageFlags image_usage_flags,
                            RHIMemoryPropertyFlags memory_property_flags,
                            RHIImage*& image,
                            RHIDeviceMemory*& memory,
                            RHIImageCreateFlags image_create_flags,
                            uint32_t array_layers,
                            uint32_t miplevels)
{
    VkImage vk_image;
    VkDeviceMemory vk_device_memory;
    VulkanUtil::CreateImage(m_PhysicalDevice,
                            m_Device,
                            image_width,
                            image_height,
                            (VkFormat)format,
                            (VkImageTiling)image_tiling,
                            (VkImageUsageFlags)image_usage_flags,
                            (VkMemoryPropertyFlags)memory_property_flags,
                            vk_image,
                            vk_device_memory,
                            (VkImageCreateFlags)image_create_flags,
                            array_layers,
                            miplevels);

    image = new VulkanImage();
    memory = new VulkanDeviceMemory();
    ((VulkanImage*)image)->setResource(vk_image);
    ((VulkanDeviceMemory*)memory)->setResource(vk_device_memory);
}

void VulkanRHI::CreateImageView(RHIImage* image,
                                RHIFormat format,
                                RHIImageAspectFlags image_aspect_flags,
                                RHIImageViewType view_type,
                                uint32_t layout_count,
                                uint32_t miplevels,
                                RHIImageView*& image_view)
{
    image_view = new VulkanImageView();
    VkImage vk_image = ((VulkanImage*)image)->getResource();
    VkImageView vk_image_view;
    vk_image_view = VulkanUtil::CreateImageView(
        m_Device, vk_image, (VkFormat)format, image_aspect_flags, (VkImageViewType)view_type, layout_count, miplevels);
    ((VulkanImageView*)image_view)->setResource(vk_image_view);
}

void VulkanRHI::CreateGlobalImage(RHIImage*& image,
                                  RHIImageView*& image_view,
                                  void* image_allocation,
                                  uint32_t texture_image_width,
                                  uint32_t texture_image_height,
                                  void* texture_image_pixels,
                                  RHIFormat texture_image_format,
                                  uint32_t miplevels)
{
    VkImage vk_image;
    VkImageView vk_image_view;

    VulkanUtil::CreateGlobalImage(this,
                                  vk_image,
                                  vk_image_view,
                                  (VmaAllocation)image_allocation,
                                  texture_image_width,
                                  texture_image_height,
                                  texture_image_pixels,
                                  texture_image_format,
                                  miplevels);

    image = new VulkanImage();
    image_view = new VulkanImageView();
    ((VulkanImage*)image)->setResource(vk_image);
    ((VulkanImageView*)image_view)->setResource(vk_image_view);
}

void VulkanRHI::CreateCubeMap(RHIImage*& image,
                              RHIImageView*& image_view,
                              void* image_allocation,
                              uint32_t texture_image_width,
                              uint32_t texture_image_height,
                              std::array<void*, 6> texture_image_pixels,
                              RHIFormat texture_image_format,
                              uint32_t miplevels)
{
    VkImage vk_image;
    VkImageView vk_image_view;

    VulkanUtil::CreateCubeMap(this,
                              vk_image,
                              vk_image_view,
                              (VmaAllocation)image_allocation,
                              texture_image_width,
                              texture_image_height,
                              texture_image_pixels,
                              texture_image_format,
                              miplevels);

    image = new VulkanImage();
    image_view = new VulkanImageView();
    ((VulkanImage*)image)->setResource(vk_image);
    ((VulkanImageView*)image_view)->setResource(vk_image_view);
}

void VulkanRHI::CreateSwapchainImageViews()
{
    m_SwapchainImageviews.resize(m_SwapchainImages.size());

    // create imageview (one for each this time) for all swapchain images
    for (size_t i = 0; i < m_SwapchainImages.size(); i++)
    {
        VkImageView vk_image_view;
        vk_image_view = VulkanUtil::CreateImageView(m_Device,
                                                    m_SwapchainImages[i],
                                                    (VkFormat)m_SwapchainImageFormat,
                                                    VK_IMAGE_ASPECT_COLOR_BIT,
                                                    VK_IMAGE_VIEW_TYPE_2D,
                                                    1,
                                                    1);
        m_SwapchainImageviews[i] = new VulkanImageView();
        ((VulkanImageView*)m_SwapchainImageviews[i])->setResource(vk_image_view);
    }
}

void VulkanRHI::CreateAssetAllocator()
{
    VmaVulkanFunctions vulkanFunctions = {};
    vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorCreateInfo = {};
    allocatorCreateInfo.vulkanApiVersion = m_VulkanApiVersion;
    allocatorCreateInfo.physicalDevice = m_PhysicalDevice;
    allocatorCreateInfo.device = m_Device;
    allocatorCreateInfo.instance = m_Instance;
    allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

    vmaCreateAllocator(&allocatorCreateInfo, &m_AssetsAllocator);
}

// todo : more descriptorSet
bool VulkanRHI::AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo* pAllocateInfo,
                                       RHIDescriptorSet*& pDescriptorSets)
{
    // descriptor_set_layout
    int descriptor_set_layout_size = pAllocateInfo->descriptorSetCount;
    std::vector<VkDescriptorSetLayout> vk_descriptor_set_layout_list(descriptor_set_layout_size);
    for (int i = 0; i < descriptor_set_layout_size; ++i)
    {
        const auto& rhi_descriptor_set_layout_element = pAllocateInfo->pSetLayouts[i];
        auto& vk_descriptor_set_layout_element = vk_descriptor_set_layout_list[i];

        vk_descriptor_set_layout_element =
            ((VulkanDescriptorSetLayout*)rhi_descriptor_set_layout_element)->getResource();

        VulkanDescriptorSetLayout* test = ((VulkanDescriptorSetLayout*)rhi_descriptor_set_layout_element);

        test = nullptr;
    };

    VkDescriptorSetAllocateInfo descriptorset_allocate_info {};
    descriptorset_allocate_info.sType = (VkStructureType)pAllocateInfo->sType;
    descriptorset_allocate_info.pNext = (const void*)pAllocateInfo->pNext;
    descriptorset_allocate_info.descriptorPool =
        ((VulkanDescriptorPool*)(pAllocateInfo->descriptorPool))->getResource();
    descriptorset_allocate_info.descriptorSetCount = pAllocateInfo->descriptorSetCount;
    descriptorset_allocate_info.pSetLayouts = vk_descriptor_set_layout_list.data();

    VkDescriptorSet vk_descriptor_set;
    pDescriptorSets = new VulkanDescriptorSet;
    VkResult result = vkAllocateDescriptorSets(m_Device, &descriptorset_allocate_info, &vk_descriptor_set);
    ((VulkanDescriptorSet*)pDescriptorSets)->setResource(vk_descriptor_set);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkAllocateDescriptorSets failed!");
        return false;
    }
}

bool VulkanRHI::AllocateCommandBuffers(const RHICommandBufferAllocateInfo* pAllocateInfo,
                                       RHICommandBuffer*& pCommandBuffers)
{
    VkCommandBufferAllocateInfo command_buffer_allocate_info {};
    command_buffer_allocate_info.sType = (VkStructureType)pAllocateInfo->sType;
    command_buffer_allocate_info.pNext = (const void*)pAllocateInfo->pNext;
    command_buffer_allocate_info.commandPool = ((VulkanCommandPool*)(pAllocateInfo->commandPool))->getResource();
    command_buffer_allocate_info.level = (VkCommandBufferLevel)pAllocateInfo->level;
    command_buffer_allocate_info.commandBufferCount = pAllocateInfo->commandBufferCount;

    VkCommandBuffer vk_command_buffer;
    pCommandBuffers = new RHICommandBuffer();
    VkResult result = vkAllocateCommandBuffers(m_Device, &command_buffer_allocate_info, &vk_command_buffer);
    ((VulkanCommandBuffer*)pCommandBuffers)->setResource(vk_command_buffer);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkAllocateCommandBuffers failed!");
        return false;
    }
}

void VulkanRHI::CreateSwapchain()
{
    // query all supports of this physical device
    SwapChainSupportDetails swapchain_support_details = QuerySwapChainSupport(m_PhysicalDevice);

    // choose the best or fitting format
    VkSurfaceFormatKHR chosen_surface_format =
        ChooseSwapchainSurfaceFormatFromDetails(swapchain_support_details.formats);
    // choose the best or fitting present mode
    VkPresentModeKHR chosen_presentMode = ChooseSwapchainPresentModeFromDetails(swapchain_support_details.presentModes);
    // choose the best or fitting extent
    VkExtent2D chosen_extent = ChooseSwapchainExtentFromDetails(swapchain_support_details.capabilities);

    uint32_t image_count = swapchain_support_details.capabilities.minImageCount + 1;
    if (swapchain_support_details.capabilities.maxImageCount > 0 &&
        image_count > swapchain_support_details.capabilities.maxImageCount)
    {
        image_count = swapchain_support_details.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_Surface;

    createInfo.minImageCount = image_count;
    createInfo.imageFormat = chosen_surface_format.format;
    createInfo.imageColorSpace = chosen_surface_format.colorSpace;
    createInfo.imageExtent = chosen_extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilyIndices[] = {m_QueueIndices.graphics_family.value(), m_QueueIndices.present_family.value()};

    if (m_QueueIndices.graphics_family != m_QueueIndices.present_family)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapchain_support_details.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = chosen_presentMode;
    createInfo.clipped = VK_TRUE;

    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS)
    {
        LOG_FATAL(ZVulkan, "vk create swapchain khr");
    }

    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &image_count, nullptr);
    m_SwapchainImages.resize(image_count);
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &image_count, m_SwapchainImages.data());

    m_SwapchainImageFormat = (RHIFormat)chosen_surface_format.format;
    m_SwapchainExtent.height = chosen_extent.height;
    m_SwapchainExtent.width = chosen_extent.width;

    for (auto& element : m_Scissors)
    {
        element = {{0, 0}, {m_SwapchainExtent.width, m_SwapchainExtent.height}};
    }
}

void VulkanRHI::ClearSwapchain()
{
    for (auto imageview : m_SwapchainImageviews)
    {
        vkDestroyImageView(m_Device, ((VulkanImageView*)imageview)->getResource(), NULL);
    }
    vkDestroySwapchainKHR(m_Device, m_Swapchain, NULL);  // also swapchain images
}

void VulkanRHI::DestroyDefaultSampler(RHIDefaultSamplerType type)
{
    switch (type)
    {
        case Default_Sampler_Linear:
            VulkanUtil::DestroyLinearSampler(m_Device);
            delete (m_LinearSampler);
            break;
        case Default_Sampler_Nearest:
            VulkanUtil::DestroyNearestSampler(m_Device);
            delete (m_NearestSampler);
            break;
        default:
            break;
    }
}

void VulkanRHI::DestroyMipmappedSampler()
{
    VulkanUtil::DestroyMipmappedSampler(m_Device);

    for (auto sampler : m_MipmapSamplerMap)
    {
        delete sampler.second;
    }
    m_MipmapSamplerMap.clear();
}

void VulkanRHI::DestroyShaderModule(RHIShader* shaderModule)
{
    vkDestroyShaderModule(m_Device, ((VulkanShader*)shaderModule)->getResource(), nullptr);

    delete (shaderModule);
}

void VulkanRHI::DestroySemaphore(RHISemaphore* semaphore)
{
    vkDestroySemaphore(m_Device, ((VulkanSemaphore*)semaphore)->getResource(), nullptr);
}

void VulkanRHI::DestroySampler(RHISampler* sampler)
{
    vkDestroySampler(m_Device, ((VulkanSampler*)sampler)->getResource(), nullptr);
}

void VulkanRHI::DestroyInstance(RHIInstance* instance)
{
    vkDestroyInstance(((VulkanInstance*)instance)->getResource(), nullptr);
}

void VulkanRHI::DestroyImageView(RHIImageView* imageView)
{
    vkDestroyImageView(m_Device, ((VulkanImageView*)imageView)->getResource(), nullptr);
}

void VulkanRHI::DestroyImage(RHIImage* image)
{
    vkDestroyImage(m_Device, ((VulkanImage*)image)->getResource(), nullptr);
}

void VulkanRHI::DestroyFramebuffer(RHIFramebuffer* framebuffer)
{
    vkDestroyFramebuffer(m_Device, ((VulkanFramebuffer*)framebuffer)->getResource(), nullptr);
}

void VulkanRHI::DestroyFence(RHIFence* fence)
{
    vkDestroyFence(m_Device, ((VulkanFence*)fence)->getResource(), nullptr);
}

void VulkanRHI::DestroyDevice()
{
    vkDestroyDevice(m_Device, nullptr);
}

void VulkanRHI::DestroyCommandPool(RHICommandPool* commandPool)
{
    vkDestroyCommandPool(m_Device, ((VulkanCommandPool*)commandPool)->getResource(), nullptr);
}

void VulkanRHI::DestroyBuffer(RHIBuffer*& buffer)
{
    vkDestroyBuffer(m_Device, ((VulkanBuffer*)buffer)->getResource(), nullptr);
    RHI_DELETE_PTR(buffer);
}

void VulkanRHI::FreeCommandBuffers(RHICommandPool* commandPool,
                                   uint32_t commandBufferCount,
                                   RHICommandBuffer* pCommandBuffers)
{
    VkCommandBuffer vk_command_buffer = ((VulkanCommandBuffer*)pCommandBuffers)->getResource();
    vkFreeCommandBuffers(
        m_Device, ((VulkanCommandPool*)commandPool)->getResource(), commandBufferCount, &vk_command_buffer);
}

void VulkanRHI::FreeMemory(RHIDeviceMemory*& memory)
{
    vkFreeMemory(m_Device, ((VulkanDeviceMemory*)memory)->getResource(), nullptr);
    RHI_DELETE_PTR(memory);
}

bool VulkanRHI::MapMemory(RHIDeviceMemory* memory,
                          RHIDeviceSize offset,
                          RHIDeviceSize size,
                          RHIMemoryMapFlags flags,
                          void** ppData)
{
    VkResult result = vkMapMemory(
        m_Device, ((VulkanDeviceMemory*)memory)->getResource(), offset, size, (VkMemoryMapFlags)flags, ppData);

    if (result == VK_SUCCESS)
    {
        return true;
    }
    else
    {
        LOG_ERROR(ZVulkan, "vkMapMemory failed!");
        return false;
    }
}

void VulkanRHI::UnmapMemory(RHIDeviceMemory* memory)
{
    vkUnmapMemory(m_Device, ((VulkanDeviceMemory*)memory)->getResource());
}

void VulkanRHI::InvalidateMappedMemoryRanges(void* pNext,
                                             RHIDeviceMemory* memory,
                                             RHIDeviceSize offset,
                                             RHIDeviceSize size)
{
    VkMappedMemoryRange mappedRange {};
    mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    mappedRange.memory = ((VulkanDeviceMemory*)memory)->getResource();
    mappedRange.offset = offset;
    mappedRange.size = size;
    vkInvalidateMappedMemoryRanges(m_Device, 1, &mappedRange);
}

void VulkanRHI::FlushMappedMemoryRanges(void* pNext, RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size)
{
    VkMappedMemoryRange mappedRange {};
    mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    mappedRange.memory = ((VulkanDeviceMemory*)memory)->getResource();
    mappedRange.offset = offset;
    mappedRange.size = size;
    vkFlushMappedMemoryRanges(m_Device, 1, &mappedRange);
}

RHISemaphore*& VulkanRHI::GetTextureCopySemaphore(uint32_t index)
{
    return m_ImageAvailableForTexturescopySemaphores[index];
}

void VulkanRHI::RegisterViewport(const int viewport_id, const RHIViewport& viewport)
{
    m_Viewports[viewport_id] = viewport;
    // Also create corresponding scissor
    RHIRect2D scissor;
    scissor.offset = {static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y)};
    scissor.extent = {static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height)};
    m_Scissors[viewport_id] = scissor;
}

void VulkanRHI::UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport)
{
    m_Viewports[(int)viewport_id] = viewport;
    RHIRect2D scissor;
    scissor.offset = {static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y)};
    scissor.extent = {static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height)};
    m_Scissors[(int)viewport_id] = scissor;
}

RHIViewport* VulkanRHI::GetViewport(ViewportType viewport_id)
{
    return &m_Viewports[(int)viewport_id];
}

void VulkanRHI::CreateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height)
{
    // Destroy existing if any
    DestroyViewportRenderTexture(viewport_id);

    ViewportRenderTexture& rt = m_ViewportRenderTextures[viewport_id];
    rt.width = width;
    rt.height = height;

    // Create color image
    CreateImage(width,
                height,
                m_SwapchainImageFormat,
                RHI_IMAGE_TILING_OPTIMAL,
                RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                rt.color_image,
                rt.color_memory,
                0,
                1,
                1);

    CreateImageView(rt.color_image,
                    m_SwapchainImageFormat,
                    RHI_IMAGE_ASPECT_COLOR_BIT,
                    RHI_IMAGE_VIEW_TYPE_2D,
                    1,
                    1,
                    rt.color_image_view);

    // Create depth image
    CreateImage(width,
                height,
                m_DepthImageFormat,
                RHI_IMAGE_TILING_OPTIMAL,
                RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT,
                RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                rt.depth_image,
                rt.depth_memory,
                0,
                1,
                1);

    CreateImageView(rt.depth_image,
                    m_DepthImageFormat,
                    RHI_IMAGE_ASPECT_DEPTH_BIT,
                    RHI_IMAGE_VIEW_TYPE_2D,
                    1,
                    1,
                    rt.depth_image_view);
}

void VulkanRHI::UpdateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height)
{
    auto it = m_ViewportRenderTextures.find(viewport_id);
    if (it != m_ViewportRenderTextures.end())
    {
        // If size changed, recreate
        if (it->second.width != width || it->second.height != height)
        {
            DestroyViewportRenderTexture(viewport_id);
            CreateViewportRenderTexture(viewport_id, width, height);
        }
    }
    else
    {
        // Create new if doesn't exist
        CreateViewportRenderTexture(viewport_id, width, height);
    }
}

void VulkanRHI::DestroyViewportRenderTexture(const std::string& viewport_id)
{
    auto it = m_ViewportRenderTextures.find(viewport_id);
    if (it != m_ViewportRenderTextures.end())
    {
        ViewportRenderTexture& rt = it->second;

        if (rt.framebuffer)
        {
            DestroyFramebuffer(rt.framebuffer);
            rt.framebuffer = nullptr;
        }

        if (rt.color_image_view)
        {
            DestroyImageView(rt.color_image_view);
            rt.color_image_view = nullptr;
        }

        if (rt.color_image)
        {
            DestroyImage(rt.color_image);
            rt.color_image = nullptr;
        }

        if (rt.color_memory)
        {
            FreeMemory(rt.color_memory);
            rt.color_memory = nullptr;
        }

        if (rt.depth_image_view)
        {
            DestroyImageView(rt.depth_image_view);
            rt.depth_image_view = nullptr;
        }

        if (rt.depth_image)
        {
            DestroyImage(rt.depth_image);
            rt.depth_image = nullptr;
        }

        if (rt.depth_memory)
        {
            FreeMemory(rt.depth_memory);
            rt.depth_memory = nullptr;
        }

        m_ViewportRenderTextures.erase(it);
    }
}

RHI::ViewportRenderTexture* VulkanRHI::GetViewportRenderTexture(const std::string& viewport_id)
{
    auto it = m_ViewportRenderTextures.find(viewport_id);
    if (it != m_ViewportRenderTextures.end())
    {
        return &(it->second);
    }
    return nullptr;
}

void VulkanRHI::RecreateSwapchain()
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(GET_SYSTEM(WindowSystem)->GetWindow(), &width, &height);
    while (width == 0 || height == 0)  // minimized 0,0, pause for now
    {
        glfwGetFramebufferSize(GET_SYSTEM(WindowSystem)->GetWindow(), &width, &height);
        glfwWaitEvents();
    }

    VkResult res_wait_for_fences =
        _vkWaitForFences(m_Device, k_max_frames_in_flight, m_IsFrameInFlightFences, VK_TRUE, UINT64_MAX);
    if (VK_SUCCESS != res_wait_for_fences)
    {
        LOG_ERROR(ZVulkan, "_vkWaitForFences failed");
        return;
    }

    DestroyImageView(m_DepthImageView);
    vkDestroyImage(m_Device, ((VulkanImage*)m_DepthImage)->getResource(), NULL);
    vkFreeMemory(m_Device, m_DepthImageMemory, NULL);

    for (auto imageview : m_SwapchainImageviews)
    {
        vkDestroyImageView(m_Device, ((VulkanImageView*)imageview)->getResource(), NULL);
    }
    vkDestroySwapchainKHR(m_Device, m_Swapchain, NULL);

    CreateSwapchain();
    CreateSwapchainImageViews();
    CreateFramebufferImageAndView();
}

VkResult VulkanRHI::CreateDebugUtilsMessengerEXT(VkInstance instance,
                                                 const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDebugUtilsMessengerEXT* pDebugMessenger)
{
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr)
    {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    }
    else
    {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void VulkanRHI::DestroyDebugUtilsMessengerEXT(VkInstance instance,
                                              VkDebugUtilsMessengerEXT debugMessenger,
                                              const VkAllocationCallbacks* pAllocator)
{
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr)
    {
        func(instance, debugMessenger, pAllocator);
    }
}

QueueFamilyIndices VulkanRHI::FindQueueFamilies(VkPhysicalDevice physicalm_device)  // for device and surface
{
    QueueFamilyIndices indices;
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalm_device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalm_device, &queue_family_count, queue_families.data());

    int i = 0;
    for (const auto& queue_family : queue_families)
    {
        if (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT)  // if support graphics command queue
        {
            indices.graphics_family = i;
        }

        if (queue_family.queueFlags & VK_QUEUE_COMPUTE_BIT)  // if support compute command queue
        {
            indices.m_ComputeFamily = i;
        }

        VkBool32 is_present_support = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalm_device,
                                             i,
                                             m_Surface,
                                             &is_present_support);  // if support surface presentation
        if (is_present_support)
        {
            indices.present_family = i;
        }

        if (indices.IsComplete())
        {
            break;
        }
        i++;
    }
    return indices;
}

bool VulkanRHI::CheckDeviceExtensionSupport(VkPhysicalDevice physicalm_device)
{
    uint32_t extension_count;
    vkEnumerateDeviceExtensionProperties(physicalm_device, nullptr, &extension_count, nullptr);

    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(physicalm_device, nullptr, &extension_count, available_extensions.data());

    std::set<std::string> required_extensions(m_DeviceExtensions.begin(), m_DeviceExtensions.end());
    for (const auto& extension : available_extensions)
    {
        required_extensions.erase(extension.extensionName);
    }

    return required_extensions.empty();
}

bool VulkanRHI::IsDeviceSuitable(VkPhysicalDevice physicalm_device)
{
    auto queue_indices = FindQueueFamilies(physicalm_device);
    bool is_extensions_supported = CheckDeviceExtensionSupport(physicalm_device);
    bool is_swapchain_adequate = false;
    if (is_extensions_supported)
    {
        SwapChainSupportDetails swapchain_support_details = QuerySwapChainSupport(physicalm_device);
        is_swapchain_adequate =
            !swapchain_support_details.formats.empty() && !swapchain_support_details.presentModes.empty();
    }

    VkPhysicalDeviceFeatures physicalm_device_features;
    vkGetPhysicalDeviceFeatures(physicalm_device, &physicalm_device_features);

    if (!queue_indices.IsComplete() || !is_swapchain_adequate || !physicalm_device_features.samplerAnisotropy)
    {
        return false;
    }

    return true;
}

SwapChainSupportDetails VulkanRHI::QuerySwapChainSupport(VkPhysicalDevice physicalm_device)
{
    SwapChainSupportDetails details_result;

    // capabilities
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalm_device, m_Surface, &details_result.capabilities);

    // formats
    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalm_device, m_Surface, &format_count, nullptr);
    if (format_count != 0)
    {
        details_result.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalm_device, m_Surface, &format_count, details_result.formats.data());
    }

    // present modes
    uint32_t presentmode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalm_device, m_Surface, &presentmode_count, nullptr);
    if (presentmode_count != 0)
    {
        details_result.presentModes.resize(presentmode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalm_device, m_Surface, &presentmode_count, details_result.presentModes.data());
    }

    return details_result;
}

VkFormat VulkanRHI::FindDepthFormat()
{
    return FindSupportedFormat({VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                               VK_IMAGE_TILING_OPTIMAL,
                               VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

VkFormat VulkanRHI::FindSupportedFormat(const std::vector<VkFormat>& candidates,
                                        VkImageTiling tiling,
                                        VkFormatFeatureFlags features)
{
    for (VkFormat format : candidates)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
        {
            return format;
        }
        else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
        {
            return format;
        }
    }

    LOG_ERROR(ZVulkan, "findSupportedFormat failed");
    return VkFormat();
}

VkSurfaceFormatKHR
VulkanRHI::ChooseSwapchainSurfaceFormatFromDetails(const std::vector<VkSurfaceFormatKHR>& available_surface_formats)
{
    for (const auto& surface_format : available_surface_formats)
    {
        // TODO: select the VK_FORMAT_B8G8R8A8_SRGB surface format,
        // there is no need to do gamma correction in the fragment shader
        if (surface_format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            surface_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return surface_format;
        }
    }
    return available_surface_formats[0];
}

VkPresentModeKHR
VulkanRHI::ChooseSwapchainPresentModeFromDetails(const std::vector<VkPresentModeKHR>& available_present_modes)
{
    for (VkPresentModeKHR present_mode : available_present_modes)
    {
        if (VK_PRESENT_MODE_MAILBOX_KHR == present_mode)
        {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRHI::ChooseSwapchainExtentFromDetails(const VkSurfaceCapabilitiesKHR& capabilities)
{
    if (capabilities.currentExtent.width != UINT32_MAX)
    {
        return capabilities.currentExtent;
    }
    else
    {
        int width, height;
        glfwGetFramebufferSize(GET_SYSTEM(WindowSystem)->GetWindow(), &width, &height);

        VkExtent2D actualExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

        actualExtent.width =
            std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height =
            std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return actualExtent;
    }
}

void VulkanRHI::PushEvent(RHICommandBuffer* commond_buffer, const char* name, const float* color)
{
    if (m_EnableDebugUtilsLabel)
    {
        VkDebugUtilsLabelEXT label_info;
        label_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label_info.pNext = nullptr;
        label_info.pLabelName = name;
        for (int i = 0; i < 4; ++i)
            label_info.color[i] = color[i];
        _vkCmdBeginDebugUtilsLabelEXT(((VulkanCommandBuffer*)commond_buffer)->getResource(), &label_info);
    }
}

void VulkanRHI::PopEvent(RHICommandBuffer* commond_buffer)
{
    if (m_EnableDebugUtilsLabel)
    {
        _vkCmdEndDebugUtilsLabelEXT(((VulkanCommandBuffer*)commond_buffer)->getResource());
    }
}
bool VulkanRHI::IsPointLightShadowEnabled()
{
    return m_EnablePointLightShadow;
}

RHICommandBuffer* VulkanRHI::GetCurrentCommandBuffer() const
{
    return m_CurrentCommandBuffer;
}
RHICommandBuffer* const* VulkanRHI::GetCommandBufferList() const
{
    return m_CommandBuffers;
}
RHICommandPool* VulkanRHI::GetCommandPoor() const
{
    return m_RhiCommandPool;
}
RHIDescriptorPool* VulkanRHI::GetDescriptorPoor() const
{
    return m_DescriptorPool;
}
RHIFence* const* VulkanRHI::GetFenceList() const
{
    return m_RhiIsFrameInFlightFences;
}
QueueFamilyIndices VulkanRHI::GetQueueFamilyIndices() const
{
    return m_QueueIndices;
}
RHIQueue* VulkanRHI::GetGraphicsQueue() const
{
    return m_GraphicsQueue;
}
RHIQueue* VulkanRHI::GetComputeQueue() const
{
    return m_ComputeQueue;
}
RHISwapChainDesc VulkanRHI::GetSwapchainInfo()
{
    RHISwapChainDesc desc;
    desc.image_format = m_SwapchainImageFormat;
    desc.extent = m_SwapchainExtent;
    desc.viewport = m_Viewports;
    desc.viewport_count = m_ViewportCount;
    desc.scissor = m_Scissors;
    desc.imageViews = m_SwapchainImageviews;
    return desc;
}
RHIDepthImageDesc VulkanRHI::GetDepthImageInfo() const
{
    RHIDepthImageDesc desc;
    desc.depth_image_format = m_DepthImageFormat;
    desc.depth_image_view = m_DepthImageView;
    desc.depth_image = m_DepthImage;
    return desc;
}
uint8_t VulkanRHI::GetMaxFramesInFlight() const
{
    return k_max_frames_in_flight;
}
uint8_t VulkanRHI::GetCurrentFrameIndex() const
{
    return m_CurrentFrameIndex;
}
void VulkanRHI::SetCurrentFrameIndex(uint8_t index)
{
    m_CurrentFrameIndex = index;
}