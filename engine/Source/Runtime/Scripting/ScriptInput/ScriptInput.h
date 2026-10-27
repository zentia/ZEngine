#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Input/KeyCodes.h"

#include <string>

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

private:
    /// 拿到真正的输入状态。nullptr = 这台机器/这个时刻没有输入系统。
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
