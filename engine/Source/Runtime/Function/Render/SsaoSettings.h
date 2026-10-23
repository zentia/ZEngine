#pragma once

#include <cstdint>

class ConsoleManager;

// SSAO（屏幕空间环境光遮蔽）的 CVar 层（r.SSAO.*），照 NaniteSettings 的模式：CVar
// 直接写进文件内的存储，渲染侧每帧重新取一次（全部"热改即生效"）。
//
// 参数含义对齐 Unity URP 的 ScreenSpaceAmbientOcclusion（同一个 Alchemy 估计式）：
//
//   Radius                  采样半球半径（世界单位，米）。
//   Intensity               遮蔽强度（乘在归一化之后、对比度曲线之前）。
//   DirectLightingStrength  遮蔽作用到**直接光**上的比例。0 = 只压间接光（UE 的
//                           做法，物理上更对），1 = 直接光也被完整压暗（URP
//                           "After Opaque" 的效果）。URP 默认 0.25。
//   SampleCount             每像素采样数。
//   Blur                    是否做两趟双边模糊（关掉就是带噪声的原始结果，仅调试用）。
//
// 这一层**平台无关**：CVar 在所有平台都注册，真正消费它的目前只有 Apple/Metal 的
// AppleSsaoPass（见那个文件的头注释）。别的后端读到也不会出错，只是没人用。
namespace SsaoSettings
{
    bool     IsEnabled();
    float    GetRadius();
    float    GetIntensity();
    float    GetDirectLightingStrength();
    uint32_t GetSampleCount();
    bool     IsBlurEnabled();

    // 注册 r.SSAO.*（从 RegisterRuntimeConsoleCommands 调用）
    void RegisterConsoleVariables(ConsoleManager& console);
}  // namespace SsaoSettings

// 心跳探针用的每帧统计（RenderSystem 的 AppleRender 心跳打出来）。
//
// 为什么光报"开关"不够：开关=1、pass 建好了，但延迟光照那边绑的仍然是 1x1 兜底图
// 时，画面上和"没有 SSAO"一模一样、而且不报任何错 ⇒ 必须把**生效那一步**
// （`bound`：这一帧延迟光照的 binding 4 绑的是不是真 AO 图）单独报出来。
struct SsaoFrameStats
{
    uint32_t enabled {0};    // r.SSAO.Enable
    uint32_t ready {0};      // 管线 / 描述符都建好了
    uint32_t ran {0};        // 这一帧真的录了 AO（+ 模糊）
    uint32_t viewports {0};  // 这一帧录了几个视口
    uint32_t bound {0};      // 延迟光照这一帧采样的是真 AO 图（不是 1x1 兜底）
};
