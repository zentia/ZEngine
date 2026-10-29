#include "VirtualTextureSettings.h"

#include "Runtime/Function/Console/ConsoleManager.h"

#include <string>

namespace VirtualTextureSettings
{
namespace
{
    // CVar 存储。默认 0（休眠）——VT 此前从未被任何渲染路径调用，默认关闭保证
    // 零回归；只有显式 r.VirtualTexture.Enable 1 才推进流送逻辑。
    int g_enable = 0;
    std::string g_source;
    int g_overlay = 0;
    int g_overlay_selftest = 0;
    int g_material_slot = 0;
    int g_material_srgb = 1;
}  // namespace

bool IsEnabled()
{
    return g_enable != 0;
}

bool IsOverlayEnabled()
{
    return g_overlay != 0;
}

bool IsOverlaySelfTestEnabled()
{
    return g_overlay_selftest != 0;
}

bool IsMaterialSlotEnabled()
{
    return g_material_slot != 0;
}

bool IsMaterialSrgbEnabled()
{
    return g_material_srgb != 0;
}

const char* GetSource()
{
    return g_source.c_str();
}

void RegisterConsoleVariables(ConsoleManager& console)
{
    console.RegisterIntVariable("r.VirtualTexture.Enable",
                                "Enable the virtual texture streaming manager (page request / priority / "
                                "LRU eviction). Off by default: the VT subsystem was previously a fully "
                                "dormant island (no render path called Update/RequestPages), so this is "
                                "opt-in for step 2.6 wiring. 0=off, 1=on.",
                                g_enable,
                                &g_enable);

    console.RegisterStringVariable("r.VirtualTexture.Source",
                                   "Asset path of an image to stream as a virtual texture (the minimal VT "
                                   "producer). Needs r.VirtualTexture.Enable 1. Empty = stream nothing. The "
                                   "texture is anchored in front of the camera where it was when the path was "
                                   "set; nothing draws it yet, residency is logged when streaming settles.",
                                   "",
                                   &g_source);

    console.RegisterIntVariable("r.VirtualTexture.Overlay",
                                "Draw the streamed virtual texture in the lower-left corner of the editor Scene "
                                "viewport, sampled through the page table inside the frame (DX12). Needs "
                                "r.VirtualTexture.Enable 1 and r.VirtualTexture.Source. 0=off (default), 1=on.",
                                g_overlay,
                                &g_overlay);

    console.RegisterIntVariable("r.VirtualTexture.OverlaySelfTest",
                                "With r.VirtualTexture.Overlay 1: read the overlay's pixels back from the back "
                                "buffer after a few frames and compare them with the CPU reference (the source "
                                "texture), logging the result. 0=off (default), 1=on.",
                                g_overlay_selftest,
                                &g_overlay_selftest);

    console.RegisterIntVariable("r.VirtualTexture.MaterialSlot",
                                "Make the base color of every built-in-layout mesh material sample the streamed "
                                "virtual texture (DX12 deferred gbuffer path: mip from uv derivatives, bilinear, "
                                "coarser-mip fallback; non-resident falls back to the plain base color). Needs "
                                "r.VirtualTexture.Enable 1 and r.VirtualTexture.Source. 0=off (default), 1=on.",
                                g_material_slot,
                                &g_material_slot);

    console.RegisterIntVariable("r.VirtualTexture.MaterialSrgb",
                                "With r.VirtualTexture.MaterialSlot 1: decode the sampled VT colour from sRGB to "
                                "linear (the page pool stores raw bytes). 1=decode (default), 0=use as is.",
                                g_material_srgb,
                                &g_material_srgb);
}
}  // namespace VirtualTextureSettings
