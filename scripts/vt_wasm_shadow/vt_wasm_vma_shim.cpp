// Shadow TU providing out-of-line definitions for the vmaCreateImage /
// vmaDestroyImage symbols declared in the shadow VulkanRHI.h. The real
// PhysicalTexturePool::Shutdown() references vmaDestroyImage in its non-Apple
// branch (compiled on Emscripten), and the pool's #else Initialize body
// references vmaCreateImage. The harness never actually runs either (it uses
// InitializeLogical() and never destroys a GPU pool), so these are trivial
// no-op stubs that just satisfy the linker. Lives only in the throwaway shadow
// tree; never linked into the real engine.

#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"

VkResult vmaCreateImage(VmaAllocator /*allocator*/, const VkImageCreateInfo* /*pCreateInfo*/,
                        const VmaAllocationCreateInfo* /*pAllocCreateInfo*/, VkImage* pImage,
                        VmaAllocation* pAllocation, VmaAllocationInfo* /*pAllocInfo*/)
{
    if (pImage) *pImage = nullptr;
    if (pAllocation) *pAllocation = nullptr;
    return VK_SUCCESS;
}

void vmaDestroyImage(VmaAllocator /*allocator*/, VkImage /*image*/, VmaAllocation /*allocation*/)
{
}
