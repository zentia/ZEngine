#pragma once

#include "Runtime/BaseClasses/ObjectMacros.h"

// =============================================================================
// 输入相关的枚举
//
// ⚠️ 必须在**全局命名空间**。ZHeaderTool 为 ZENUM 生成的 .gen.cpp 在全局作用域
// 引用枚举名且**不加限定**，放进 namespace 会得到 C2061（`ParticlePlayState`
// 那次就是这么挂的）。`KeyCodes::Key` 能待在 namespace 里，是 ZENUM 对"恰好一个
// 具名 namespace"的特例 —— 别把那个特例当成规则用。
//
// 取值与 Unity 逐一对应，脚本里写的数字（以及存档里可能存下的数字）才有意义。
// =============================================================================

/// Unity 的 `TouchPhase`。一次触摸的生命周期：Began → (Moved | Stationary)* →
/// Ended | Canceled。
ZENUM()
enum class TouchPhase : int
{
    Began      = 0,
    Moved      = 1,
    Stationary = 2,
    Ended      = 3,
    Canceled   = 4,
};

/// Unity 的 `DeviceOrientation`。
///
/// ⚠️ 桌面上恒 `Unknown`：没有方向传感器，也没有"窗口旋转"这回事。返回
/// `Portrait` 之类的值会让`根据横竖屏切布局`的代码在桌面上挑错一套 UI。
ZENUM()
enum class DeviceOrientation : int
{
    Unknown            = 0,
    Portrait           = 1,
    PortraitUpsideDown = 2,
    LandscapeLeft      = 3,
    LandscapeRight     = 4,
    FaceUp             = 5,
    FaceDown           = 6,
};

/// Unity 的 `IMECompositionMode`（输入法组合串的处理方式）。
ZENUM()
enum class IMECompositionMode : int
{
    Auto = 0,
    On   = 1,
    Off  = 2,
};

/// Unity 的 `TouchType`：手指 / 触控笔 / 间接（触控板映射成触摸）。
///
/// ⚠️ **刻意不标 ZENUM**。TS 的 lib.dom 里已经有一个全局
/// `type TouchType = "direct" | "stylus"`（`Touch.touchType` 用的）。再声明一个
/// `declare enum TouchType` 会撞全局名 —— 结果是脚本里写 `TouchType.Direct`
/// 得到 TS2693 "only refers to a type"，而开了 skipLibCheck 的工程连报错都没有。
/// 所以脚本侧 `Touch.type` 是 `number`，取值与 Unity 一致（0/1/2），写在
/// Touch.h 的注释里。
enum class TouchType : int
{
    Direct   = 0,
    Indirect = 1,
    Stylus   = 2,
};
