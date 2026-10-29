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

    // r.VirtualTexture.Source：要作为虚拟纹理流送的图片资产路径（空 = 不流送任何东西）。
    // 这是**最小生产者**的输入：没有它，Enable=1 也什么都不会发生，因为引擎里没有别的东西
    // 会注册虚拟纹理或请求页。返回值按值，因为 CVar 可能在别的线程被改写。
    const char* GetSource();

    // r.VirtualTexture.Overlay：0=off（默认），1=在编辑器 Scene 视口左下角画一个 VT 调试窗口——
    // **第一个真正在帧内、经页表间接寻址采样 VT 的消费者**（DX12 主相机 pass，见
    // DX12MainCameraPass::DrawVirtualTextureOverlay）。需要同时 Enable 1 + Source。
    bool IsOverlayEnabled();

    // r.VirtualTexture.OverlaySelfTest：1 = 叠加层画满若干帧后，把它在后备缓冲里的像素读回，逐像素和 CPU
    // 参考（源纹理）比对，结果写日志。默认 0。用来在没人盯屏幕时证明"屏幕上画出来的是对的"。
    bool IsOverlaySelfTestEnabled();

    // r.VirtualTexture.MaterialSlot：1 = 所有内置布局（StandardLit）的网格材质把 base color 改为经 VT 采样（DX12 的
    // 延迟 gbuffer 路径）。需要同时 Enable 1 + Source。默认 0：不开时材质逐字节与过去一致。
    // 现在是引擎级总开关（调试 / 演示用）；按材质资产逐个指定 VT 纹理需要材质资产格式支持，尚未做。
    bool IsMaterialSlotEnabled();

    // r.VirtualTexture.MaterialSrgb：材质里的 VT 采样结果是否按 sRGB 解码成线性（默认 1：页池存的是原始字节，
    // 彩色图片是 sRGB 编码的）。
    bool IsMaterialSrgbEnabled();

    // 注册 r.VirtualTexture.*（从 RegisterRuntimeConsoleCommands 调用）
    void RegisterConsoleVariables(ConsoleManager& console);
}  // namespace VirtualTextureSettings
