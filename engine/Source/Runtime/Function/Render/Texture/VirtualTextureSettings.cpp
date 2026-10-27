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
}  // namespace

bool IsEnabled()
{
    return g_enable != 0;
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
}
}  // namespace VirtualTextureSettings
