#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Function/Render/Platform/Generic/GenericWindow.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include "ScriptCursor.generated.h"

// 脚本侧的光标控制（Unity 的 `UnityEngine.Cursor` 静态类）。
//
// Unity 的 `Cursor` 有三样东西：
//   * `visible`   —— 光标画不画
//   * `lockState` —— None / Locked / Confined
//   * `SetCursor(texture, hotSpot, mode)` —— 换光标位图
//
// 这里给了前两样，**没有** `SetCursor`
// -----------------------------------------------
// Unity 的 `SetCursor` 吃一张 `Texture2D` 当位图。ZEngine 的 `Texture2D` 是
// **资产**（一张 `.zasset` 里的像素 blob，见 `Texture2D.h` 的文件头），它身上
// 没有 OS 光标句柄，也没有"从资产造 HCURSOR / NSCursor"这一层。要补得先有
// "运行时把 Texture2D 上传成 GUI 资源"的能力 —— 那是一件独立的事，不是这个
// 门面该顺手塞进去的。先不导出：`Cursor.SetCursor` 编译期报错，好过传进去
// 之后什么都没发生。
//
// 跨平台生效情况（读这段再决定要不要依赖）
// -----------------------------------------
// `visible` / `lockState` 在 **Windows** 上真的会改 OS 光标（`ShowCursor` /
// `ClipCursor`，见 `WindowsWindow::SetCursorVisible`）。其余后端目前只走
// GenericWindow 的默认实现 —— 记状态，不产生 OS 效果。
//
// 也就是说：**读回来永远是你设的值**，但"屏幕上光标是否真的消失了"在非
// Windows 上不保证。这不是静默失败 —— 它是"上层策略已下达、后端还没接"，
// 与 `QualitySettings` 那批开关同一性质。
//
// headless 下（没有 WindowSystem / 没有窗口）状态仍然存在进程里，脚本照样能
// 设能读 —— 少了这一层的话，"进游戏锁光标"的代码在没有窗口的测试里会炸。

// Unity 的 `UnityEngine.CursorLockMode`。取值与之一致。
ZENUM()
enum class CursorLockMode : int
{
    None     = 0,
    Locked   = 1,
    Confined = 2,
};

ZCLASS()
class Cursor : public Object
{
    ZGENERATED_BODY(Cursor)

public:
    /// 光标是否可见。Unity 的 `Cursor.visible`。
    ZFUNCTION()
    static bool GetVisible();
    ZSCRIPTPROPERTY(Name = "visible", Getter = "GetVisible", Setter = "SetVisible")

    /// 设置光标可见性。
    ZFUNCTION()
    static void SetVisible(bool visible);

    /// 光标锁定模式。Unity 的 `Cursor.lockState`。
    ZFUNCTION()
    static CursorLockMode GetLockState();
    ZSCRIPTPROPERTY(Name = "lockState", Getter = "GetLockState", Setter = "SetLockState")

    /// 设置锁定模式。
    ///
    /// `Locked` 会把光标钉在窗口中心（Windows 上是 `ClipCursor` 到中心 1x1），
    /// 这是 FPS / 第三人称相机"鼠标转视角"的标准前提。退出这类玩法时记得还回
    /// `None` —— 否则用户会得到一个动不了的鼠标。
    ZFUNCTION()
    static void SetLockState(CursorLockMode mode);

    /// Unity 里最常用的一对写法的快捷方式：`Cursor.visible = v; Cursor.lockState
    /// = v ? None : Locked;`。
    ///
    /// 两件事**必须一起做**（只设 visible=false 的话光标还能移出窗口点到别的
    /// 应用上；只设 Locked 的话光标还画在中心），所以给一个入口。
    ZFUNCTION()
    static void SetCursorState(bool visible, CursorLockMode mode);

    /// 恢复到出厂状态（可见 + 不锁定）。
    ///
    /// 与 `QualitySettings.ResetToDefaults()` 同源的考虑：这些是进程内状态，
    /// 没有"工程资产"可以回退，得有个显式的还原入口（退出玩法 / 测试收尾）。
    ZFUNCTION()
    static void ResetToDefaults();
};
