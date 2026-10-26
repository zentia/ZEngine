#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Input/InputAction.h"
#include "Runtime/Function/Input/InputMappingContext.h"
#include "Runtime/Function/Input/InputTriggers.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// EnhancedInputSystem — replaces the old InputSystem.
//
// Architecture (UE EnhancedInput-inspired):
//
//   Raw input (GLFW) → EnhancedInputSystem::OnKey/OnCursorPos/...
//     → Accumulate raw values per InputAction (via mapping contexts)
//     → Apply modifiers
//     → Evaluate triggers
//     → Fire bound delegates
//
// The system owns a priority stack of InputMappingContexts.
// Higher-priority contexts process input first and can consume
// actions (preventing lower-priority contexts from seeing them).
//
// No 3D dependencies: cursor-to-angle conversion is now the
// consumer's responsibility (CameraComponent applies its own
// sensitivity). The system only provides raw delta values.
// ============================================================
class EnhancedInputSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "EnhancedInputSystem"; }
    std::vector<std::type_index> GetDependencies() const override;
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Platform; }

    bool Initialize() override;
    void Shutdown() override;
    void Tick();

    // ---- Frame-delta computation (pure, unit-tested) ---------------------
    //
    // Computes the clamped wall-clock delta for one frame. `last_time_s` is
    // the epoch-second stamp from the previous Tick (0.0 before the first),
    // `has_ticked` whether a Tick has already happened, and `now_s` the current
    // epoch-second reading. Returns the delta to feed the trigger evaluator,
    // clamped to [0, k_MaxFrameDeltaS] so a stall can't fast-forward a Hold
    // trigger, and 0 on the first frame / backwards-clock edge.
    //
    // Static + pure (no member access) so the smoke test can exercise the
    // exact logic Tick() uses without spinning up WindowSystem. Tick() passes
    // its own m_LastTickTimeS / m_HasTicked through here.
    static constexpr float k_MaxFrameDeltaS = 0.25f; // 250ms cap
    static inline float ComputeFrameDelta(double last_time_s, bool has_ticked, double now_s)
    {
        if (!has_ticked)
        {
            // First frame: report 0 so a long idle gap before the first tick
            // can't spike a Hold trigger.
            return 0.0f;
        }

        const double raw = now_s - last_time_s;
        if (raw < 0.0)
        {
            // Monotonic clock should never go backwards; treat a backwards edge
            // as a fresh frame rather than feeding a negative delta into Hold.
            return 0.0f;
        }
        const float delta = static_cast<float>(raw);
        return delta > k_MaxFrameDeltaS ? k_MaxFrameDeltaS : delta;
    }

    // ---- Raw input callbacks (registered with WindowSystem) ----
    void OnKey(int key, int scancode, int action, int mods);
    void OnCursorPos(double current_cursor_x, double current_cursor_y);
    void OnMouseButton(int button, int action, int mods);
    void OnScroll(double xoffset, double yoffset);

    // ---- Touch (mobile hosts) --------------------------------------------
    //
    // 移动端没有 GenericWindow，触摸由宿主（Android 的 AInputEvent / iOS 的
    // UITouch）经 WindowSystem::PushTouchEvent 推到这里。
    //
    // 映射策略：只有"第一根按下的手指"（primary）被翻译成鼠标语义
    // （光标位置 + 左键按下/抬起）。这样 UI 命中测试、按钮、相机拖拽全部复用
    // 现有的鼠标路径，移动端不需要任何改动就能点能拖。
    //
    // 其余手指只维护状态供查询（GetTouchPosition / GetTouchDelta），游戏层拿
    // 去做虚拟摇杆：把位移归一化后写进一个 InputAction 的 Axis2D，
    // InputSystem::SyncGameCommands() 会把它翻译成 forward/backward/left/right。
    void OnTouch(int pointer_id, int action, float x, float y);

    /// 当前按在屏幕上的手指数。
    int GetTouchCount() const { return static_cast<int>(m_Touches.size()); }

    /// 某根手指的当前位置。返回 false = 这根手指不在屏幕上。
    bool GetTouchPosition(int pointer_id, float& out_x, float& out_y) const;

    /// 某根手指从按下点起的位移（像素）—— 虚拟摇杆的摇杆量。
    bool GetTouchDelta(int pointer_id, float& out_dx, float& out_dy) const;

    /// 某根手指本帧的位移（像素，Tick 末尾清零）—— 转视角要的是这个。
    bool GetTouchFrameDelta(int pointer_id, float& out_dx, float& out_dy) const;

    // ---- 虚拟摇杆 / 触摸视角（移动端）------------------------------------
    //
    // 键盘 WASD 把值写进 "Move" 的 Axis2D，鼠标 delta 写进 "Look"，
    // InputSystem::SyncGameCommands() 再把它们翻译成 GameCommand。移动端没有
    // 键盘鼠标，这条链路得在引擎里补上。
    //
    // 为什么必须是引擎内部写而不是游戏层注入：m_AccumulatedValues 在 Tick 的
    // 第一步就被 clear()，而 InputSystem::Tick() 紧接着就要读它 —— 游戏层的
    // Tick 排在后面，注入的值下一帧开头就被清掉了，永远读不到。
    //
    // 默认全关：摇杆的触发区域（左半屏？一个 UI 控件？）是玩法决策，不该由
    // 引擎硬编码。游戏层判断"这根手指按在摇杆上"之后调下面的 setter。

    void SetTouchStickEnabled(bool enabled) { m_TouchStickEnabled = enabled; }
    bool IsTouchStickEnabled() const { return m_TouchStickEnabled; }

    /// 指定哪根手指是摇杆（通常是"按在摇杆控件上"的那根）。-1 = 没有。
    void SetTouchStickPointer(int pointer_id) { m_TouchStickPointerId = pointer_id; }
    int  GetTouchStickPointer() const { return m_TouchStickPointerId; }

    /// 摇杆半径（像素）：手指偏离按下点这么多即满舵。0 = 屏幕短边的 1/4。
    void SetTouchStickRadius(float radius_pixels) { m_TouchStickRadiusPixels = radius_pixels; }

    /// 指定哪根手指用来转视角（本帧位移 → "Look"）。-1 = 没有。
    void SetTouchLookPointer(int pointer_id) { m_TouchLookPointerId = pointer_id; }
    int  GetTouchLookPointer() const { return m_TouchLookPointerId; }

    // ---- Mapping context stack (UE IEnhancedInputSubsystemInterface) ----

    // Add a mapping context at the given priority. If a context with the
    // same name already exists, this replaces it.
    void AddMappingContext(std::shared_ptr<InputMappingContext> context, int priority);

    // Remove a mapping context by name.
    void RemoveMappingContext(const std::string& name);

    // Remove all mapping contexts.
    void ClearMappingContexts();

    // ---- Action registration ----

    // Register an action so the system knows about it. Not strictly required
    // but useful for iteration / debugging.
    void RegisterAction(InputAction* action);

    // ---- Query helpers (for legacy migration) ----

    // Get the current value of a named action (if any context maps it).
    InputActionValue GetActionValue(const std::string& action_name) const;

    // Quick boolean check: is the action currently actuated?
    bool IsActionActive(const std::string& action_name) const;

    // Raw cursor position (for UI hit-testing, etc.)
    double GetCursorX() const { return m_CursorX; }
    double GetCursorY() const { return m_CursorY; }

    // Raw cursor delta in pixels (cleared each frame after Tick).
    double GetCursorDeltaX() const { return m_CursorDeltaX; }
    double GetCursorDeltaY() const { return m_CursorDeltaY; }

    // Focus mode (cursor locked/hidden for FPS camera control).
    // Kept for backward compatibility; new code should use a
    // mapping context for UI vs gameplay instead.
    bool IsFocusMode() const { return m_FocusMode; }
    void SetFocusMode(bool mode);

private:
    // Rebuild the internal lookup tables when the context stack changes.
    void RebuildMappingTable();

    /// 一根手指的状态。start_* 是按下点 —— 虚拟摇杆的圆心，位移都相对它算。
    struct TouchPoint
    {
        float x {0.0f};
        float y {0.0f};
        float start_x {0.0f};
        float start_y {0.0f};
        /// 本帧累积的位移（Tick 末尾清零）。"Look" 要的是帧内增量（与鼠标
        /// delta 同语义），而摇杆要的是相对按下点的累计位移 —— 两者不同。
        float frame_dx {0.0f};
        float frame_dy {0.0f};
    };

    /// 把 primary 手指的位移当成鼠标光标移动累积进去。
    /// 与 OnCursorPos() 的区别：触摸的"按住拖"天然等价于桌面 focus mode 下的
    /// 鼠标拖拽，所以这里无条件累积，不看 m_FocusMode。
    void FeedCursorFromPrimaryTouch(float x, float y, bool reset_anchor);

    /// 把触摸摇杆 / 触摸视角写进 m_AccumulatedValues（Tick 内调用，见那组
    /// setter 的注释：这是唯一能写入的时机）。
    void ApplyTouchAxes();

    /// 摇杆半径：没显式给就是用屏幕短边的 1/4。
    float ResolveTouchStickRadius() const;

    /// 把一次"连续型指针输入"（鼠标移动 / 滚轮）按绑定写进 m_AccumulatedValues。
    /// 这类输入没有按下/抬起，所以不走 m_KeyToMappings（那张表靠 m_PressedKeys
    /// 查询），而是走 RebuildMappingTable() 填出来的伪键表。
    void ApplyPointerMapping(const FKeyMapping& mapping, const Vector2& raw_delta);

    // Apply modifiers from a key mapping to a value.
    InputActionValue ApplyModifiers(const FKeyMapping& mapping, const InputActionValue& value) const;

    // Get or create the default trigger for an action type.
    std::shared_ptr<InputTrigger> GetDefaultTrigger(InputAction* action);

    // ---- Raw input state ----
    std::unordered_set<int> m_PressedKeys;  // currently-held GLFW key codes
    double m_CursorX = 0.0;
    double m_CursorY = 0.0;
    double m_CursorDeltaX = 0.0;
    double m_CursorDeltaY = 0.0;
    double m_LastCursorX = 0.0;
    double m_LastCursorY = 0.0;
    float  m_ScrollAccumX = 0.0f;
    float  m_ScrollAccumY = 0.0f;
    bool   m_FocusMode = false;
    bool   m_CursorInitialized = false;

    // ---- Touch state (mobile) ----
    // pointer_id -> 状态。pointer_id 由宿主分配，同一根手指的 down/move/up 一致。
    std::unordered_map<int, TouchPoint> m_Touches;
    int m_PrimaryTouchId {-1};  // 第一根按下的手指，被翻译成鼠标语义的那根

    bool  m_TouchStickEnabled {false};
    int   m_TouchStickPointerId {-1};
    float m_TouchStickRadiusPixels {0.0f};
    int   m_TouchLookPointerId {-1};

    // ---- Mapping context stack ----
    std::vector<FMappingContextStackEntry> m_ContextStack;
    bool m_ContextDirty = true; // need RebuildMappingTable

    // ---- Rebuilt lookup tables ----
    // key_code -> list of FKeyMapping pointers (from all active contexts,
    // ordered by context priority).
    // 注意：下面三张表缓存的是 FKeyMapping 的裸指针，指向 context 自己的
    // m_Mappings。所以 AddMappingContext() 之后不要再对同一个 context 调
    // MapKey() —— vector 重分配会让这些指针全部悬空（且 m_ContextDirty 不会被
    // 置位，表不会重建）。想改绑定就先 Remove 再 Add。
    std::unordered_map<int, std::vector<FKeyMapping*>> m_KeyToMappings;
    // "mouse_move" and "mouse_scroll" are special pseudo-keys.
    //
    // 值必须引用 KeyCodes::MouseMoveXY / MouseScrollXY，不要在这里另写 -1 / -2：
    // -1 是 KeyCodes::Unknown（也就是 FKey::Invalid()），RebuildMappingTable()
    // 会把 code == Unknown 的绑定当成"没绑按键"直接 continue 掉。历史版本正是
    // 取了 -1 / -2，结果两张伪键表永远为空，鼠标移动与滚轮的映射从未生效。
    static constexpr int kKeyMouseMove   = KeyCodes::MouseMoveXY;
    static constexpr int kKeyMouseScroll = KeyCodes::MouseScrollXY;
    std::vector<FKeyMapping*> m_MouseMoveMappings;
    std::vector<FKeyMapping*> m_MouseScrollMappings;

    // ---- Action state ----
    // Per-action accumulated value this frame (before trigger eval).
    // Action name -> accumulated value.
    std::unordered_map<std::string, InputActionValue> m_AccumulatedValues;

    // ---- Frame timing ----
    // Anchor for the real wall-clock delta between successive Tick() calls.
    // Stores epoch-seconds from a monotonic steady_clock; 0.0 until the first
    // Tick(). m_HasTicked=false makes the first frame report 0 delta so a long
    // idle gap before the first tick can't spike a Hold trigger.
    double m_LastTickTimeS = 0.0;
    bool m_HasTicked = false;

    // Per-action trigger state (for default triggers).
    // Action name -> trigger instance.
    std::unordered_map<std::string, std::shared_ptr<InputTrigger>> m_DefaultTriggers;

    // Registered actions (for query/debug).
    std::vector<InputAction*> m_RegisteredActions;
};
