#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Input/InputEnums.h"

// =============================================================================
// Unity 的 `Touch` —— 一次触摸在某一帧的快照
//
// 值类型：跨脚本边界是一个**快照**（与 `Color` / `Bounds` / `RaycastHit` 同
// 类），声明成 `declare interface Touch`，所以脚本只能通过 `Input.GetTouch(i)`
// 拿，不能 `new`。这是刻意的 —— 触摸是引擎每帧采出来的，脚本造一个假的没有
// 任何意义，反而会让"这根手指真的在屏幕上吗"这个问题变得答不上来。
//
// 单位：像素，窗口坐标（与 `Input.mousePosition` 同一空间）。
//
// ⚠️ 几个"不支持"字段的取值**照抄 Unity 的规定**，不是随便填的哨兵：
//   * `pressure` = 1.0f、`maximumPossiblePressure` = 1.0f —— Unity 明确说设备
//     不报压力时返回 1.0（不是 0）。填 0 会让"按压力度控制笔刷粗细"在支持压力
//     的设备和不支持的设备上表现完全相反。
//   * `radius` / `radiusVariance` / `altitudeAngle` / `azimuthAngle` = 0 ——
//     Unity 也是 0（"平台不支持"）。
// =============================================================================
struct Touch
{
    /// 手指 id。同一根手指在 Began..Ended 之间保持不变，跨帧稳定。
    int fingerId {0};

    /// 当前位置（像素）。
    Vector2 position {};

    /// 未做死区/平滑处理的位置。ZEngine 不做触摸平滑，与 `position` 同值 ——
    /// 保留这个字段是为了和 Unity 的字段集对齐，省掉脚本移植时的改名。
    Vector2 rawPosition {};

    /// 相对**上一帧**的位移（像素）。Began 帧为 0。
    Vector2 deltaPosition {};

    /// 距上一次相位变化的秒数。Began 帧为 0。
    float deltaTime {0.0f};

    /// 连击数（双击 = 2）。见 EnhancedInputSystem 里的判定窗口。
    int tapCount {0};

    /// 本帧的相位。
    TouchPhase phase {TouchPhase::Canceled};

    /// 压力。不支持压力的设备恒 1.0f（Unity 的规定，不是哨兵）。
    float pressure {1.0f};

    /// 该设备能报出的最大压力。不支持时恒 1.0f。
    float maximumPossiblePressure {1.0f};

    /// 触摸类型。移动端宿主不区分，恒 Direct。
    TouchType type {TouchType::Direct};

    /// 触摸半径（像素）。不支持时 0。
    float radius {0.0f};

    /// 半径的抖动量。不支持时 0。
    float radiusVariance {0.0f};

    /// 触控笔的仰角（弧度）。不支持时 0。
    float altitudeAngle {0.0f};

    /// 触控笔的方位角（弧度）。不支持时 0。
    float azimuthAngle {0.0f};
};
