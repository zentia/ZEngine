#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#include <stdexcept>
#include <vector>
#if defined(Z_HAS_VULKAN)
    #include <vma/vk_mem_alloc.h>
#elif defined(__APPLE__) || defined(__EMSCRIPTEN__)
// macOS uses Metal (no VMA); Emscripten/WebGL2 has no Vulkan at all and we
// strip vk_mem_alloc.h on Web. Provide a minimal placeholder so the rest of
// the header still compiles. The font path that *uses* this allocation lives
// in debug_draw_buffer.cpp which is excluded from the Web build.
using VmaAllocation = void*;
#else
// Windows DX12-only build (ZENGINE_USE_VULKAN=OFF): the Vulkan SDK include
// dir (which provides vma/vk_mem_alloc.h) is not on the path. Forward-declare
// the opaque handle exactly as VMA does so the type matches RenderGpuResources.h.
struct VmaAllocation_T;
typedef struct VmaAllocation_T* VmaAllocation;
#endif

class DebugDrawFont
{
public:
    void GetCharacterTextureRect(const unsigned char character, float& x1, float& y1, float& x2, float& y2);
    RHIImageView* GetImageView() const;
    void Inialize();
    void Destroy();

private:
    const unsigned char m_RangeL = 32, m_RangeR = 126;
    const int m_Singlecharacterwidth = 32;
    const int m_Singlecharacterheight = 64;
    const int m_Numofcharacterinoneline = 16;
    const int m_Numofcharacter = (m_RangeR - m_RangeL + 1);
    const int m_BitmapW = m_Singlecharacterwidth * m_Numofcharacterinoneline;
    const int m_BitmapH =
        m_Singlecharacterheight * ((m_Numofcharacter + m_Numofcharacterinoneline - 1) / m_Numofcharacterinoneline);

    RHIImage* m_FontImage = nullptr;
    RHIImageView* m_FontImageview = nullptr;
    RHIDeviceMemory* m_FontImagememory = nullptr;
    VmaAllocation m_Allocation;

    void LoadFont();
};