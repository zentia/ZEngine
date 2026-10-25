#pragma once

class ConsoleManager;

// Virtual Texture 的 CVar 层（r.VirtualTexture.*），照 NaniteSettings 的模式：CVar
// 直接写进文件内的 int 存储，IsEnabled() 读回。
//
// 为什么需要这一层：VT 子系统（VirtualTextureManager）此前是**完全休眠的孤岛**——
// 6 个源文件能编译，但没有任何渲染路径调用 Update()/RequestPages()，CMakeLists 在
// WebGL/Windows/Metal 三条路径还把它整个排除了。step 2.6 的"接线"第一步就是给它一个
// 控制台开关，让运维/调试能显式打开，同时默认关闭保证零回归。
//
// 这个头**不引用任何 VT 类型**（只暴露一个 bool），所以它在所有平台都能编译，不受 VT
// 源文件只在 Vulkan/Linux 路径参与编译的限制。
namespace VirtualTextureSettings
{
    // r.VirtualTexture.Enable：0=off（默认，休眠），1=on。
    bool IsEnabled();

    // 注册 r.VirtualTexture.*（从 RegisterRuntimeConsoleCommands 调用）
    void RegisterConsoleVariables(ConsoleManager& console);
}  // namespace VirtualTextureSettings
