#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/Platform.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Time/TimeState.h"
#include "Runtime/Function/Input/InputEnums.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Input/KeyCodes.h"
#include "Runtime/Function/Input/Touch.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "ScriptInput.generated.h"

// 脚本侧的输入查询。
//
// 之前脚本一个输入 API 都没有：InputSystem 是 IEngineSystem（不是 ZCLASS），
// 而真正的状态在 EnhancedInputSystem 里 —— 它由 InputSystem::Initialize()
// new 出来、**不注册进 SystemRegistry**，所以 GET_SYSTEM(EnhancedInputSystem)
// 永远是 nullptr。脚本要读输入只能走 InputSystem::GetEnhancedInput()。
//
// 键码从哪来
// ----------
// `KeyCodes::Key` 直接用 ZENUM 导出（ZENUM 支持"恰好一个具名 namespace"，
// KeyCodes 正好是一个），所以脚本写：
//
//     if (Input.GetKeyDown(Key.Space)) { jump(); }
//
// 不用为了导出而把几百个键码复制一份。注意 d.ts 里的名字是 `Key`（namespace
// 名不进脚本），不是 Unity 的 `KeyCode` —— ZENUM 目前不支持 Name= specifier
// （parser: "ZENUM takes no specifiers yet"）。
//
// 为什么是 GetKeyDown() 而不是 GetKeyDown 属性
// ---------------------------------------------
// 同 Time：ZSCRIPTPROPERTY 的 getter 不能是 static（zht/parser.py:540），
// 静态 API 一律做成方法。
//
// 三态（GetKey / GetKeyDown / GetKeyUp）
// --------------------------------------
// Unity 的老 Input 靠这三个区分"持续按住"和"本帧刚发生"。没有边沿态的话，
// "按一次空格跳一下"会变成"按住空格每帧都跳"。边沿集合由
// EnhancedInputSystem::Tick 的 Step 4 清空，一次按下只活一帧。
//
// 没有输入系统时
// --------------
// 每个查询在 InputSystem 缺失、或它的 EnhancedInput 还没建起来（未
// Initialize、headless 冒烟测试）时返回**安全默认值**（false / 0 / 零向量），
// 不崩。脚本拿到的就是"没有输入"。
//
// `Input` 不该被实例化 —— 它只是脚本侧的命名空间。header-only（同
// BasicShape.h 的 Box / Sphere / Capsule，无对应 .cpp）。

ZCLASS()
class Input : public Object
{
    ZGENERATED_BODY(Input)

public:
    // ---- 键盘 ----

    /// 键现在是否按着（持续态）。Unity 的 `Input.GetKey`。
    ZFUNCTION()
    static bool GetKey(int key_code)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->IsKeyDown(key_code);
    }

    /// 键是否在本帧刚按下（边沿）。Unity 的 `Input.GetKeyDown`。
    /// 长按自动重复（REPEAT）不算 —— 否则按住不放会每帧为真。
    ZFUNCTION()
    static bool GetKeyDown(int key_code)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->WasKeyPressedThisFrame(key_code);
    }

    /// 键是否在本帧刚抬起（边沿）。Unity 的 `Input.GetKeyUp`。
    ZFUNCTION()
    static bool GetKeyUp(int key_code)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->WasKeyReleasedThisFrame(key_code);
    }

    // ---- 鼠标键 ----

    /// button 是 0-based：0 左键、1 右键、2 中键。
    ZFUNCTION()
    static bool GetMouseButton(int button)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->IsMouseButtonDown(button);
    }

    ZFUNCTION()
    static bool GetMouseButtonDown(int button)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->WasMouseButtonPressedThisFrame(button);
    }

    ZFUNCTION()
    static bool GetMouseButtonUp(int button)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->WasMouseButtonReleasedThisFrame(button);
    }

    // ---- 鼠标位置 / 位移 / 滚轮 ----

    /// 光标位置（像素，窗口坐标）。Unity 的 `Input.mousePosition` 是 Vector3，
    /// 这里没有 z。
    ZFUNCTION()
    static Vector2 GetMousePosition()
    {
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return Vector2::ZERO;
        }
        return Vector2(static_cast<float>(input->GetCursorX()), static_cast<float>(input->GetCursorY()));
    }
    ZSCRIPTPROPERTY(Name = "mousePosition", Getter = "GetMousePosition")

    /// 上一帧的光标位移（像素）。与鼠标 delta 同语义。
    ///
    /// 为什么是"上一帧"：输入事件在帧末尾才投递，帧内量由
    /// EnhancedInputSystem::EndFrame() 在脚本 tick 之后滚动，所以脚本读到的
    /// 是"到本帧为止最后一次滚动出来的位移"。
    ZFUNCTION()
    static Vector2 GetMouseDelta()
    {
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return Vector2::ZERO;
        }
        return Vector2(static_cast<float>(input->GetLastTickCursorDeltaX()),
                       static_cast<float>(input->GetLastTickCursorDeltaY()));
    }

    /// 上一帧的滚轮量。Unity 的 `Input.mouseScrollDelta`。
    ZFUNCTION()
    static Vector2 GetMouseScrollDelta()
    {
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return Vector2::ZERO;
        }
        return Vector2(input->GetScrollDeltaX(), input->GetScrollDeltaY());
    }
    ZSCRIPTPROPERTY(Name = "mouseScrollDelta", Getter = "GetMouseScrollDelta")

    // ---- 动作（mapping context 那一层）----

    /// 动作当前是否被触发。名字是 InputAction 的 m_Name
    /// （引擎默认建好的有 "Move" / "Look" / "Jump" / "Sprint" / "Crouch" /
    /// "Fire" / "ToggleFreeCamera"）。
    ZFUNCTION()
    static bool IsActionActive(const std::string& action_name)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->IsActionActive(action_name);
    }

    /// 动作的一维值。布尔型动作按下时是 1，否则 0。
    ZFUNCTION()
    static float GetActionAxis1D(const std::string& action_name)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr ? input->GetActionValue(action_name).GetAxis1D() : 0.0f;
    }

    /// 动作的二维值。"Move" 的 y 是前后、x 是左右；"Look" 是本帧视角位移。
    ZFUNCTION()
    static Vector2 GetActionAxis2D(const std::string& action_name)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr ? input->GetActionValue(action_name).GetAxis2D() : Vector2::ZERO;
    }

    // ---- 触摸（移动端）----

    ZFUNCTION()
    static int GetTouchCount()
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr ? input->GetTouchCount() : 0;
    }

    /// 第 index 根手指的 pointer_id（按 id 升序，索引稳定）。越界返回 -1。
    /// 遍历手指时先拿 id 再按 id 查位置 —— m_Touches 是 unordered_map，
    /// 直接按下标取没有顺序保证。
    ZFUNCTION()
    static int GetTouchIdAt(int index)
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr ? input->GetTouchIdAt(index) : -1;
    }

    /// 手指当前位置（像素）。手指不在屏幕上返回零向量。
    ZFUNCTION()
    static Vector2 GetTouchPosition(int pointer_id)
    {
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return Vector2::ZERO;
        }
        float x = 0.0f;
        float y = 0.0f;
        return input->GetTouchPosition(pointer_id, x, y) ? Vector2(x, y) : Vector2::ZERO;
    }

    /// 手指相对**按下点**的位移（像素）—— 虚拟摇杆的摇杆量。
    ZFUNCTION()
    static Vector2 GetTouchDelta(int pointer_id)
    {
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return Vector2::ZERO;
        }
        float dx = 0.0f;
        float dy = 0.0f;
        return input->GetTouchDelta(pointer_id, dx, dy) ? Vector2(dx, dy) : Vector2::ZERO;
    }

    /// 手指**本帧**的位移（像素）—— 转视角要的是这个，与鼠标 delta 同语义。
    ZFUNCTION()
    static Vector2 GetTouchFrameDelta(int pointer_id)
    {
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return Vector2::ZERO;
        }
        float dx = 0.0f;
        float dy = 0.0f;
        return input->GetTouchFrameDelta(pointer_id, dx, dy) ? Vector2(dx, dy) : Vector2::ZERO;
    }

    // =========================================================================
    // Unity 老 Input 那一层
    //
    // 上面那组是"ZEngine 的动作系统"（IsActionActive / GetActionAxis1D），这套
    // 是 Unity 的 `Input.GetAxis` / `GetButton` / `anyKey` / `touches`。两者不是
    // 重复：Unity 项目的脚本写的是后者，没有这一层就得全改。
    //
    // 轴名映射：Unity 的 Input Manager 是工程里的配置表，ZEngine 没有那张表，
    // 所以这里是一张**固定的**老轴名 → 动作名映射。好处是开箱可用，代价是
    // 不能改键 —— 要改键请直接用动作系统（或以后补一张可配的表）。
    // =========================================================================

    // ---- 任意键 ----

    /// 现在是否有任意键（**含鼠标键**）按着。Unity 的 `Input.anyKey`。
    ZFUNCTION()
    static bool GetAnyKey()
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->IsAnyKeyDown();
    }
    ZSCRIPTPROPERTY(Name = "anyKey", Getter = "GetAnyKey")

    /// 本帧是否有任意键刚按下。Unity 的 `Input.anyKeyDown`。
    ZFUNCTION()
    static bool GetAnyKeyDown()
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->WasAnyKeyPressedThisFrame();
    }
    ZSCRIPTPROPERTY(Name = "anyKeyDown", Getter = "GetAnyKeyDown")

    // ---- 文本输入 ----

    /// 本帧由键盘/输入法输入的字符（UTF-8）。Unity 的 `Input.inputString`。
    ///
    /// ⚠️ 与 `GetKeyDown(Key.A)` 不是一回事：那是"物理键"，这是"打出来的字"。
    /// 做输入框必须用这一条 —— 中文输入法提交时 `GetKeyDown` 只有一个回车。
    ZFUNCTION()
    static std::string GetInputString()
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr ? input->GetInputString() : std::string();
    }
    ZSCRIPTPROPERTY(Name = "inputString", Getter = "GetInputString")

    // ---- 轴 ----

    /// Unity 的 `Input.GetAxis`：带死区与"重力/灵敏度"斜坡的轴值。
    ///
    /// 与 `GetAxisRaw` 的差别就是那个斜坡：按一下 W，`GetAxisRaw("Vertical")`
    /// 立刻是 1，而 `GetAxis` 要用约 1/3 秒爬上去（默认 Gravity=3、
    /// Sensitivity=3）。角色移动用 Raw 手感更跟手，载具/镜头用 GetAxis 更顺。
    ///
    /// ⚠️ 名字不认识时返回 0，不抛异常 —— 拼错轴名在 Unity 里是运行时警告，
    /// 在这里让玩法每帧抛一次只会把问题藏起来。
    ZFUNCTION()
    static float GetAxis(const std::string& axis_name)
    {
        return SmoothAxis(axis_name, ReadAxisRaw(axis_name));
    }

    /// Unity 的 `Input.GetAxisRaw`：不做平滑，直接给 -1 / 0 / 1。
    ZFUNCTION()
    static float GetAxisRaw(const std::string& axis_name)
    {
        return ReadAxisRaw(axis_name);
    }

    // ---- 按钮 ----

    /// Unity 的 `Input.GetButton`。名字映射见 AxisActionName / ButtonActionName。
    ZFUNCTION()
    static bool GetButton(const std::string& button_name)
    {
        const std::string action = ButtonActionName(button_name);
        if (action.empty())
        {
            return false;
        }
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr && input->IsActionActive(action);
    }

    /// Unity 的 `Input.GetButtonDown`：本帧刚按下（边沿）。
    ZFUNCTION()
    static bool GetButtonDown(const std::string& button_name)
    {
        bool down = false;
        bool up   = false;
        SampleButton(button_name, down, up);
        return down;
    }

    /// Unity 的 `Input.GetButtonUp`：本帧刚抬起（边沿）。
    ZFUNCTION()
    static bool GetButtonUp(const std::string& button_name)
    {
        bool down = false;
        bool up   = false;
        SampleButton(button_name, down, up);
        return up;
    }

    /// 轴值推进一帧（纯函数，**不读任何状态**）。
    ///
    /// 抽出来是为了能单测：斜坡 + 死区 + 夹紧这三条规则在没有真实输入系统的
    /// 进程里没办法通过 `GetAxis` 走一遍（那条路要先有 InputSystem，而它要
    /// WindowSystem）。把规则做成显式入参显式出参，测试就能直接喂数。
    ///
    /// Unity 的数字轴语义：
    ///   * 有输入时朝目标按 **Sensitivity** 爬（不是立刻到位）
    ///   * 没输入时按 **Gravity** 衰减回 0（松手后还剩一段滑行）
    ///   * |值| < Dead 直接归 0（摇杆回中时的零点抖动）
    static float AdvanceAxis(float current, float raw, float dt)
    {
        float next = current;
        if (raw != 0.0f)
        {
            if (next < raw)
            {
                next = std::min(raw, next + kAxisSensitivity * dt);
            }
            else
            {
                next = std::max(raw, next - kAxisSensitivity * dt);
            }
        }
        else
        {
            if (next > 0.0f)
            {
                next = std::max(0.0f, next - kAxisGravity * dt);
            }
            else
            {
                next = std::min(0.0f, next + kAxisGravity * dt);
            }
        }
        if (std::fabs(next) < kAxisDead)
        {
            next = 0.0f;
        }
        return ClampAxis(next);
    }

    /// 清掉本帧的轴累积。Unity 的 `Input.ResetInputAxes()`。
    ZFUNCTION()
    static void ResetInputAxes()
    {
        if (EnhancedInputSystem* input = GetMutableInput())
        {
            input->ResetInputAxes();
        }
        AxisStates().clear();
    }

    // ---- 触摸（Unity 的 `Input.touches` 形状）----

    /// 本帧可枚举的手指数（含本帧刚抬起的那几根）。Unity 的 `Input.touchCount`。
    ZFUNCTION()
    static int GetFrameTouchCount()
    {
        const EnhancedInputSystem* input = GetInput();
        return input != nullptr ? input->GetFrameTouchCount() : 0;
    }
    ZSCRIPTPROPERTY(Name = "touchCount", Getter = "GetFrameTouchCount")

    /// 第 index 根手指的快照。越界返回一个"什么都没有"的 Touch（phase 为
    /// Canceled、fingerId 为 -1）—— Unity 里越界是抛异常，TS 层没有廉价的方式
    /// 表达"读了个不存在的手指"，给出可判的空值更好用。
    ZFUNCTION()
    static Touch GetTouch(int index)
    {
        Touch touch;
        touch.fingerId = -1;
        touch.phase    = TouchPhase::Canceled;
        if (const EnhancedInputSystem* input = GetInput())
        {
            input->GetTouchAt(index, touch);
        }
        return touch;
    }

    /// 本帧所有手指的快照。Unity 的 `Input.touches`。
    ZFUNCTION()
    static std::vector<Touch> GetTouches()
    {
        std::vector<Touch> touches;
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return touches;
        }
        const int count = input->GetFrameTouchCount();
        touches.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            Touch touch;
            if (input->GetTouchAt(i, touch))
            {
                touches.push_back(touch);
            }
        }
        return touches;
    }
    ZSCRIPTPROPERTY(Name = "touches", Getter = "GetTouches")

    // ---- 设备能力 ----

    /// 是否有鼠标。桌面恒 true，移动端恒 false。
    ///
    /// 按平台推而不是"试着查一下"：没有 API 能可靠地问出"这台机器插着鼠标吗"
    /// （Windows 上 `GetSystemMetrics(SM_MOUSEPRESENT)` 在开了平板模式的笔记本上
    /// 会撒谎）。移动端一刀切 false，因为那里根本没有光标语义。
    ZFUNCTION()
    static bool GetMousePresent()
    {
#if defined(Z_PLATFORM_ANDROID) || defined(Z_PLATFORM_IOS) || defined(Z_PLATFORM_OHOS)
        return false;
#else
        return true;
#endif
    }
    ZSCRIPTPROPERTY(Name = "mousePresent", Getter = "GetMousePresent")

    /// 是否支持触摸。移动端为 true。
    ///
    /// ⚠️ 桌面恒 false，即使是一台带触摸屏的 Windows 笔记本 —— 桌面后端没有
    /// 接 WM_TOUCH / WM_POINTER，说 true 会让脚本走进一条永远收不到事件的路。
    ZFUNCTION()
    static bool GetTouchSupported()
    {
#if defined(Z_PLATFORM_ANDROID) || defined(Z_PLATFORM_IOS) || defined(Z_PLATFORM_OHOS)
        return true;
#else
        return false;
#endif
    }
    ZSCRIPTPROPERTY(Name = "touchSupported", Getter = "GetTouchSupported")

    /// 是否支持触控笔。没有后端 ⇒ false（不猜）。
    ZFUNCTION()
    static bool GetStylusTouchSupported() { return false; }
    ZSCRIPTPROPERTY(Name = "stylusTouchSupported", Getter = "GetStylusTouchSupported")

    /// 设备是否上报压力。没有后端 ⇒ false。
    ///
    /// ⚠️ 与 `Touch.pressure` 的取值不冲突：这里说"设备不上报"，所以
    /// `Touch.pressure` 取 Unity 规定的 1.0f。
    ZFUNCTION()
    static bool GetTouchPressureSupported() { return false; }
    ZSCRIPTPROPERTY(Name = "touchPressureSupported", Getter = "GetTouchPressureSupported")

    /// 手柄（joystick）名字列表。**恒空**：引擎没有手柄后端。
    ///
    /// 返回一个编出来的 "Player 1" 会让"等手柄连上"的逻辑永远以为连上了。
    ZFUNCTION()
    static std::vector<std::string> GetJoystickNames()
    {
        return {};
    }

    /// 设备方向。没有方向传感器 ⇒ 恒 `Unknown`（见 InputEnums.h 那条注释）。
    ZFUNCTION()
    static DeviceOrientation GetDeviceOrientation()
    {
        return DeviceOrientation::Unknown;
    }
    ZSCRIPTPROPERTY(Name = "deviceOrientation", Getter = "GetDeviceOrientation")

    /// 加速度（m/s²）。没有加速度计 ⇒ 零向量。
    ///
    /// 零向量而不是"上一次的值"：拿着设备不动时读到的确实接近重力，但我们没有
    /// 传感器，给零是唯一诚实的选择。
    ZFUNCTION()
    static Vector2 GetAcceleration() { return Vector2::ZERO; }
    ZSCRIPTPROPERTY(Name = "acceleration", Getter = "GetAcceleration")

    /// 重力向量。同上，没有传感器 ⇒ 零向量。
    ZFUNCTION()
    static Vector2 GetGravity() { return Vector2::ZERO; }
    ZSCRIPTPROPERTY(Name = "gravity", Getter = "GetGravity")

    // ---- 输入设置（进程级，与 Cursor / QualitySettings 同一类）----

    /// 是否启用多点触控。Unity 的 `Input.multiTouchEnabled`（默认 true）。
    ZFUNCTION()
    static bool GetMultiTouchEnabled() { return Settings().multi_touch_enabled; }

    ZFUNCTION()
    static void SetMultiTouchEnabled(bool enabled) { Settings().multi_touch_enabled = enabled; }
    ZSCRIPTPROPERTY(Name = "multiTouchEnabled", Getter = "GetMultiTouchEnabled",
                    Setter = "SetMultiTouchEnabled")

    /// 安卓返回键是否退出应用。Unity 的 `Input.backButtonLeavesApp`。
    ZFUNCTION()
    static bool GetBackButtonLeavesApp() { return Settings().back_button_leaves_app; }

    ZFUNCTION()
    static void SetBackButtonLeavesApp(bool leaves) { Settings().back_button_leaves_app = leaves; }
    ZSCRIPTPROPERTY(Name = "backButtonLeavesApp", Getter = "GetBackButtonLeavesApp",
                    Setter = "SetBackButtonLeavesApp")

    /// 是否用触摸模拟鼠标。Unity 的 `Input.simulateMouseWithTouches`。
    ///
    /// ⚠️ 目前**只记状态，不改行为**：ZEngine 的 primary 手指**总是**被翻译成
    /// 鼠标（见 EnhancedInputSystem::OnTouch 的注释，UI 与相机都靠这条）。做成
    /// 可关的开关要先把那条路径改成可旁路，那是功能改动不是补绑定 —— 所以这里
    /// 先给读写，注释写清楚它现在还不生效。
    ZFUNCTION()
    static bool GetSimulateMouseWithTouches() { return Settings().simulate_mouse_with_touches; }

    ZFUNCTION()
    static void SetSimulateMouseWithTouches(bool simulate)
    {
        Settings().simulate_mouse_with_touches = simulate;
    }
    ZSCRIPTPROPERTY(Name = "simulateMouseWithTouches", Getter = "GetSimulateMouseWithTouches",
                    Setter = "SetSimulateMouseWithTouches")

    /// 输入法组合串的处理方式。Unity 的 `Input.imeCompositionMode`。
    ZFUNCTION()
    static IMECompositionMode GetImeCompositionMode() { return Settings().ime_composition_mode; }

    ZFUNCTION()
    static void SetImeCompositionMode(IMECompositionMode mode)
    {
        Settings().ime_composition_mode = mode;
    }
    ZSCRIPTPROPERTY(Name = "imeCompositionMode", Getter = "GetImeCompositionMode",
                    Setter = "SetImeCompositionMode")

    /// 输入法是否正在组合（未提交）。没有 IME 后端 ⇒ 恒 false。
    ZFUNCTION()
    static bool GetImeIsSelected() { return false; }
    ZSCRIPTPROPERTY(Name = "imeIsSelected", Getter = "GetImeIsSelected")

    /// 输入法正在组合的串。没有 IME 后端 ⇒ 恒空串。
    ZFUNCTION()
    static std::string GetCompositionString() { return std::string(); }
    ZSCRIPTPROPERTY(Name = "compositionString", Getter = "GetCompositionString")

private:
    // ---- 老轴名 → 动作名 / 直接读数 ----------------------------------------

    /// 轴名是否是"鼠标派生"的。这类轴在 Unity 里 Gravity=0，**不做斜坡** ——
    /// 否则每帧的一小段鼠标位移会被累加器拖成一条长尾（松手后视角还在飘）。
    static bool IsMouseAxis(const std::string& axis_name)
    {
        return axis_name == "Mouse X" || axis_name == "Mouse Y" ||
               axis_name == "Mouse ScrollWheel";
    }

    /// 读一次轴的原始值。
    static float ReadAxisRaw(const std::string& axis_name)
    {
        const EnhancedInputSystem* input = GetInput();
        if (input == nullptr)
        {
            return 0.0f;
        }

        if (axis_name == "Mouse X")
        {
            return static_cast<float>(input->GetLastTickCursorDeltaX());
        }
        if (axis_name == "Mouse Y")
        {
            return static_cast<float>(input->GetLastTickCursorDeltaY());
        }
        if (axis_name == "Mouse ScrollWheel")
        {
            return input->GetScrollDeltaY();
        }

        const std::string action = AxisActionName(axis_name);
        if (action.empty())
        {
            return 0.0f;
        }

        if (action == "Move.x")
        {
            return input->GetActionValue("Move").GetAxis2D().x;
        }
        if (action == "Move.y")
        {
            return input->GetActionValue("Move").GetAxis2D().y;
        }
        return input->GetActionValue(action).GetAxis1D();
    }

    /// 老轴名 → 动作名。"Move.x" / "Move.y" 是 Axis2D 的两个分量。
    static std::string AxisActionName(const std::string& axis_name)
    {
        if (axis_name == "Horizontal")
        {
            return "Move.x";
        }
        if (axis_name == "Vertical")
        {
            return "Move.y";
        }
        if (axis_name == "Fire1")
        {
            return "Fire";
        }
        if (axis_name == "Jump")
        {
            return "Jump";
        }
        if (axis_name == "Sprint")
        {
            return "Sprint";
        }
        if (axis_name == "Crouch")
        {
            return "Crouch";
        }
        return std::string();
    }

    /// 按钮名 → 动作名。空 = 没这个按钮。
    ///
    /// "Submit" / "Cancel" / "Fire2" / "Fire3" 这些 Unity 默认轴在 ZEngine 里
    /// 没有对应动作，不给它们编一个 —— 返回 false 比"绑到 Fire 上"好（后者会
    /// 让"右键开镜"和"左键开火"变成同一个键）。
    static std::string ButtonActionName(const std::string& button_name)
    {
        if (button_name == "Fire1")
        {
            return "Fire";
        }
        if (button_name == "Jump")
        {
            return "Jump";
        }
        if (button_name == "Sprint")
        {
            return "Sprint";
        }
        if (button_name == "Crouch")
        {
            return "Crouch";
        }
        return std::string();
    }

    // ---- 轴的重力/灵敏度斜坡 ------------------------------------------------

    struct AxisState
    {
        float current {0.0f};
        int   last_frame {-1};
    };

    static std::unordered_map<std::string, AxisState>& AxisStates()
    {
        static std::unordered_map<std::string, AxisState> s_States;
        return s_States;
    }

    /// Unity Input Manager 里数字轴的默认值。写死在这里，因为 ZEngine 没有那张
    /// 可配的表（见上面"轴名映射"那节注释）。
    static constexpr float kAxisGravity     = 3.0f;
    static constexpr float kAxisSensitivity = 3.0f;
    static constexpr float kAxisDead        = 0.001f;

    static float SmoothAxis(const std::string& axis_name, float raw)
    {
        // 鼠标派生的轴不做斜坡（见 IsMouseAxis 的注释）。
        if (IsMouseAxis(axis_name))
        {
            return ClampAxis(raw);
        }

        const TimeState& time  = TimeState::Get();
        const float      dt    = time.GetUnscaledDeltaTime();
        const int        frame = time.GetFrameCount();

        AxisState& state = AxisStates()[axis_name];

        // ⚠️ 一帧只推进一次。GetAxis 常常同一帧被好几个系统各调一遍（移动 +
        // 动画 + UI），按调用次数推进会让"按住 3 秒"这件事依赖于有多少个读者。
        if (state.last_frame != frame)
        {
            state.current = AdvanceAxis(state.current, raw, dt);
            state.last_frame = frame;
        }

        return state.current;
    }

    static float ClampAxis(float v)
    {
        if (v > 1.0f)
        {
            return 1.0f;
        }
        if (v < -1.0f)
        {
            return -1.0f;
        }
        return v;
    }

    // ---- 按钮边沿 ------------------------------------------------------------

    struct ButtonState
    {
        bool value {false};
        bool previous {false};
        int  last_frame {-1};
    };

    static std::unordered_map<std::string, ButtonState>& ButtonStates()
    {
        static std::unordered_map<std::string, ButtonState> s_States;
        return s_States;
    }

    /// 采一次按钮状态并给出本帧的两个边沿。
    ///
    /// ⚠️ 与 Unity 一样，边沿只在"每帧都查"时准确。跳帧不查的话 previous 停在上
    /// 一次采样的值上，会漏掉中间那次按下 —— 这是轮询式边沿检测的固有代价。
    static void SampleButton(const std::string& button_name, bool& out_down, bool& out_up)
    {
        out_down = false;
        out_up   = false;

        const std::string action = ButtonActionName(button_name);
        if (action.empty())
        {
            return;
        }
        const EnhancedInputSystem* input = GetInput();
        const bool now = (input != nullptr) && input->IsActionActive(action);

        ButtonState& state = ButtonStates()[button_name];
        const int    frame = TimeState::Get().GetFrameCount();
        if (state.last_frame != frame)
        {
            state.previous   = state.value;
            state.value      = now;
            state.last_frame = frame;
        }
        out_down = state.value && !state.previous;
        out_up   = !state.value && state.previous;
    }

    // ---- 进程级输入设置 ------------------------------------------------------

    struct InputSettings
    {
        bool               multi_touch_enabled {true};
        bool               back_button_leaves_app {false};
        bool               simulate_mouse_with_touches {true};
        IMECompositionMode ime_composition_mode {IMECompositionMode::Auto};
    };

    /// 函数内静态：与 Cursor / QualitySettings 同一类进程级状态，避开静态初始化
    /// 顺序（这些开关会被 TypeContainer 注册路径上的代码读，而那条路径跑在
    /// main() 之前）。
    static InputSettings& Settings()
    {
        static InputSettings s_Settings;
        return s_Settings;
    }

    /// 拿到可写的输入状态（ResetInputAxes 用）。
    static EnhancedInputSystem* GetMutableInput()
    {
        InputSystem* system = GET_SYSTEM(InputSystem);
        if (system == nullptr)
        {
            return nullptr;
        }
        return system->GetEnhancedInput();
    }

    /// 拿到真正的输入状态。nullptr = 这台机器/这个时刻没有输入系统。
    ///
    /// 为什么不 GET_SYSTEM(EnhancedInputSystem)：它是 InputSystem::Initialize()
    /// new 出来的，**没有注册进 SystemRegistry**，那样查永远是 nullptr。
    ///
    /// 为什么不 GET_SYSTEM(EnhancedInputSystem)：它是 InputSystem::Initialize()
    /// new 出来的，**没有注册进 SystemRegistry**，那样查永远是 nullptr。
    static const EnhancedInputSystem* GetInput()
    {
        const InputSystem* system = GET_SYSTEM(InputSystem);
        if (system == nullptr)
        {
            return nullptr;
        }
        return system->GetEnhancedInput();
    }
};
