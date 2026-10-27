#include "Runtime/Function/Input/EnhancedInputSystem.h"

#include "Runtime/Function/Render/WindowSystem.h"
#include "core/base/Macro.h"

#include <algorithm>
#include "Runtime/Function/Input/KeyCodes.h"

namespace
{
    // Identity mapping — our internal key codes are already
    // GLFW-compatible, so no translation is needed.
    int ToKeyCode(int key)
    {
        return key;
    }

    int MouseButtonToKeyCode(int button)
    {
        return static_cast<int>(KeyCodes::MouseButton1) + button;
    }
} // namespace

std::vector<std::type_index> EnhancedInputSystem::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(WindowSystem)};
}

bool EnhancedInputSystem::Initialize()
{
    // Register raw input callbacks with WindowSystem
    GET_SYSTEM(WindowSystem)
        ->RegisterOnKeyFunc([this](int key, int scancode, int action, int mods) {
            OnKey(key, scancode, action, mods);
        });
    GET_SYSTEM(WindowSystem)
        ->RegisterOnCursorPosFunc([this](double x, double y) {
            OnCursorPos(x, y);
        });
    GET_SYSTEM(WindowSystem)
        ->RegisterOnMouseButtonFunc([this](int button, int action, int mods) {
            OnMouseButton(button, action, mods);
        });
    GET_SYSTEM(WindowSystem)
        ->RegisterOnScrollFunc([this](double xoff, double yoff) {
            OnScroll(xoff, yoff);
        });
    // 触摸：移动端宿主推入（WindowSystem::PushTouchEvent）。桌面永远不会有
    // 这些事件，注册了也不占什么。
    GET_SYSTEM(WindowSystem)
        ->RegisterOnTouchFunc([this](int pointer_id, int action, float x, float y) {
            OnTouch(pointer_id, action, x, y);
        });

    // Initialize shared game actions
    GameActions::Init();
    for (InputAction* action : GameActions::GetAll())
    {
        RegisterAction(action);
    }

    LOG_INFO(ZEngine, "EnhancedInputSystem initialized");
    return true;
}

void EnhancedInputSystem::Shutdown()
{
    ClearMappingContexts();
    m_DefaultTriggers.clear();
    m_AccumulatedValues.clear();
    m_RegisteredActions.clear();
}

void EnhancedInputSystem::OnKey(int key, int scancode, int action, int mods)
{
    int key_code = ToKeyCode(key);
    if (action == KeyCodes::PRESS || action == KeyCodes::REPEAT)
    {
        m_PressedKeys.insert(key_code);
        // 只有真正的 PRESS 才算"本帧刚按下"。REPEAT 是长按自动重复，算进去
        // 的话 GetKeyDown 会每帧为真 —— 那就退化成 GetKey 了。
        if (action == KeyCodes::PRESS)
        {
            m_PressedThisFrame.insert(key_code);
        }
    }
    else if (action == KeyCodes::RELEASE)
    {
        m_PressedKeys.erase(key_code);
        m_ReleasedThisFrame.insert(key_code);
    }
}

void EnhancedInputSystem::OnCursorPos(double current_cursor_x, double current_cursor_y)
{
    if (m_FocusMode && m_CursorInitialized)
    {
        m_CursorDeltaX = m_LastCursorX - current_cursor_x;
        m_CursorDeltaY = m_LastCursorY - current_cursor_y;
    }
    m_LastCursorX = current_cursor_x;
    m_LastCursorY = current_cursor_y;
    m_CursorX = current_cursor_x;
    m_CursorY = current_cursor_y;
    m_CursorInitialized = true;
}

void EnhancedInputSystem::OnMouseButton(int button, int action, int mods)
{
    int key_code = MouseButtonToKeyCode(button);
    if (action == KeyCodes::PRESS)
    {
        m_PressedKeys.insert(key_code);
        m_PressedThisFrame.insert(key_code);
    }
    else if (action == KeyCodes::RELEASE)
    {
        m_PressedKeys.erase(key_code);
        m_ReleasedThisFrame.insert(key_code);
    }
}

void EnhancedInputSystem::OnScroll(double xoffset, double yoffset)
{
    m_ScrollAccumX += static_cast<float>(xoffset);
    m_ScrollAccumY += static_cast<float>(yoffset);
}

void EnhancedInputSystem::OnTouch(int pointer_id, int action, float x, float y)
{
    using TouchAction = WindowSystem::TouchAction;
    const TouchAction touch_action = static_cast<TouchAction>(action);
    const auto       it = m_Touches.find(pointer_id);

    switch (touch_action)
    {
        case TouchAction::Down:
            if (it == m_Touches.end())
            {
                m_Touches.emplace(pointer_id, TouchPoint{x, y, x, y});
                // 第一根按下的手指成为 primary：之后它的移动被翻译成光标移动
                // 与左键拖拽，UI 与相机就都走通了。
                if (m_PrimaryTouchId < 0)
                {
                    m_PrimaryTouchId = pointer_id;
                }
            }
            break;

        case TouchAction::Move:
            if (it != m_Touches.end())
            {
                // 帧内增量（转视角用）与累计位移（摇杆用）都要，所以两个都更新。
                it->second.frame_dx += x - it->second.x;
                it->second.frame_dy += y - it->second.y;
                it->second.x = x;
                it->second.y = y;
            }
            break;

        case TouchAction::Up:
        case TouchAction::Cancel:
            if (it != m_Touches.end())
            {
                m_Touches.erase(it);
            }
            if (m_PrimaryTouchId == pointer_id)
            {
                // primary 抬起后把身份交给还按着的第一根手指，否则后续拖动
                // （比如另一只手还在转相机）会被完全忽略。
                m_PrimaryTouchId = m_Touches.empty() ? -1 : m_Touches.begin()->first;
            }
            break;

        default:
            break;
    }

    if (m_PrimaryTouchId != pointer_id)
    {
        return;  // 非 primary：只留状态供查询，不动光标
    }

    if (touch_action == TouchAction::Down)
    {
        // 先定锚点再按下，保证 UI 的按下命中测试落在手指真正按下的那一点。
        FeedCursorFromPrimaryTouch(x, y, true);
        OnMouseButton(0, KeyCodes::PRESS, 0);
    }
    else if (touch_action == TouchAction::Move)
    {
        FeedCursorFromPrimaryTouch(x, y, false);
    }
    else
    {
        // Up / Cancel：先更新到最终位置再抬起，抬起事件的命中测试才准。
        FeedCursorFromPrimaryTouch(x, y, false);
        OnMouseButton(0, KeyCodes::RELEASE, 0);
    }
}

void EnhancedInputSystem::FeedCursorFromPrimaryTouch(float x, float y, bool reset_anchor)
{
    if (reset_anchor || !m_CursorInitialized)
    {
        // 刚按下（或本进程第一次收到触摸）：只定锚点，不产生 delta。
        // 否则从屏幕另一边点过来会一次性产生一个巨大的 delta，相机瞬间转飞。
        m_LastCursorX = x;
        m_LastCursorY = y;
        m_CursorInitialized = true;
    }
    else
    {
        m_CursorDeltaX += x - m_LastCursorX;
        m_CursorDeltaY += y - m_LastCursorY;
        m_LastCursorX = x;
        m_LastCursorY = y;
    }
    m_CursorX = x;
    m_CursorY = y;
}

bool EnhancedInputSystem::GetTouchPosition(int pointer_id, float& out_x, float& out_y) const
{
    const auto it = m_Touches.find(pointer_id);
    if (it == m_Touches.end())
    {
        return false;
    }
    out_x = it->second.x;
    out_y = it->second.y;
    return true;
}

bool EnhancedInputSystem::GetTouchDelta(int pointer_id, float& out_dx, float& out_dy) const
{
    const auto it = m_Touches.find(pointer_id);
    if (it == m_Touches.end())
    {
        return false;
    }
    // 相对按下点：虚拟摇杆要的就是这个（摇杆圆心 = 按下点）。
    out_dx = it->second.x - it->second.start_x;
    out_dy = it->second.y - it->second.start_y;
    return true;
}

bool EnhancedInputSystem::GetTouchFrameDelta(int pointer_id, float& out_dx, float& out_dy) const
{
    const auto it = m_Touches.find(pointer_id);
    if (it == m_Touches.end())
    {
        return false;
    }
    out_dx = it->second.frame_dx;
    out_dy = it->second.frame_dy;
    return true;
}

float EnhancedInputSystem::ResolveTouchStickRadius() const
{
    if (m_TouchStickRadiusPixels > 0.0f)
    {
        return m_TouchStickRadiusPixels;
    }
    // 没配就用屏幕短边的 1/4：手机横屏时大约是一个拇指的自然活动范围。
    int width = 0;
    int height = 0;
    if (auto* window_system = GET_SYSTEM(WindowSystem); window_system != nullptr)
    {
        const auto size = window_system->GetWindowSize();
        width  = size[0];
        height = size[1];
    }
    if (width <= 0 || height <= 0)
    {
        return 100.0f;
    }
    return static_cast<float>(width < height ? width : height) * 0.25f;
}

void EnhancedInputSystem::ApplyTouchAxes()
{
    // 摇杆：相对按下点的位移归一化 → "Move" 的 Axis2D。
    // 与键盘 WASD 写的是同一个 action，所以 InputSystem::SyncGameCommands()
    // 不用改就能把它翻译成 forward/backward/left/right。
    if (m_TouchStickEnabled && m_TouchStickPointerId >= 0)
    {
        float dx = 0.0f;
        float dy = 0.0f;
        if (GetTouchDelta(m_TouchStickPointerId, dx, dy))
        {
            const float radius = ResolveTouchStickRadius();
            float nx = dx / radius;
            // 屏幕 y 向下为正，"Move" 的 y 向前为正，所以要翻符号。
            float ny = -dy / radius;

            const float length = std::sqrt(nx * nx + ny * ny);
            if (length > 1.0f)
            {
                nx /= length;
                ny /= length;
            }
            m_AccumulatedValues["Move"] = InputActionValue(Vector2(nx, ny));
        }
    }

    // 视角：本帧位移 → "Look"。与鼠标 delta 同一语义（都是帧内增量）。
    if (m_TouchLookPointerId >= 0)
    {
        float dx = 0.0f;
        float dy = 0.0f;
        if (GetTouchFrameDelta(m_TouchLookPointerId, dx, dy))
        {
            m_AccumulatedValues["Look"] = InputActionValue(Vector2(dx, dy));
        }
    }

    // 帧内增量不再在这里清 —— 移到 EndFrame()，理由同上（脚本要读得到）。
}

void EnhancedInputSystem::SetFocusMode(bool mode)
{
    m_FocusMode = mode;
    if (auto* ws = GET_SYSTEM(WindowSystem))
    {
        ws->SetFocusMode(mode);
    }
}

// ============================================================
// Mapping context management
// ============================================================

void EnhancedInputSystem::AddMappingContext(std::shared_ptr<InputMappingContext> context, int priority)
{
    // Remove existing context with the same name
    auto it = std::find_if(m_ContextStack.begin(), m_ContextStack.end(),
                           [&](const FMappingContextStackEntry& e) {
                               return e.context->GetName() == context->GetName();
                           });
    if (it != m_ContextStack.end())
    {
        m_ContextStack.erase(it);
    }

    m_ContextStack.push_back({context, priority});

    // Sort by priority descending (highest first)
    std::sort(m_ContextStack.begin(), m_ContextStack.end(),
              [](const FMappingContextStackEntry& a, const FMappingContextStackEntry& b) {
                  return a.priority > b.priority;
              });

    m_ContextDirty = true;
}

void EnhancedInputSystem::RemoveMappingContext(const std::string& name)
{
    auto it = std::find_if(m_ContextStack.begin(), m_ContextStack.end(),
                           [&](const FMappingContextStackEntry& e) {
                               return e.context->GetName() == name;
                           });
    if (it != m_ContextStack.end())
    {
        m_ContextStack.erase(it);
        m_ContextDirty = true;
    }
}

void EnhancedInputSystem::ClearMappingContexts()
{
    m_ContextStack.clear();
    m_ContextDirty = true;
}

void EnhancedInputSystem::RegisterAction(InputAction* action)
{
    m_RegisteredActions.push_back(action);
}

InputActionValue EnhancedInputSystem::GetActionValue(const std::string& action_name) const
{
    auto it = m_AccumulatedValues.find(action_name);
    if (it != m_AccumulatedValues.end())
        return it->second;
    return InputActionValue();
}

bool EnhancedInputSystem::IsActionActive(const std::string& action_name) const
{
    auto val = GetActionValue(action_name);
    return val.GetBoolean() || val.GetAxis1D() != 0.0f || val.GetAxis2D() != Vector2::ZERO;
}

// ============================================================
// Internal: rebuild lookup tables
// ============================================================

void EnhancedInputSystem::RebuildMappingTable()
{
    m_KeyToMappings.clear();
    m_MouseMoveMappings.clear();
    m_MouseScrollMappings.clear();

    // Iterate contexts in priority order (already sorted)
    for (const auto& entry : m_ContextStack)
    {
        for (const FKeyMapping& mapping : entry.context->GetMappings())
        {
            // 伪键先分流。它们不是按键，没有"按下"状态，进 m_KeyToMappings 会被
            // m_PressedKeys 查询永远判 false —— 必须走各自的连续量表。
            // 顺序上必须排在 KeyCodes::Unknown 判断之前：旧版伪键取 -1，正好等于
            // Unknown，于是下面那句 continue 把它们全吞了，两张表恒为空。
            if (mapping.key.code == kKeyMouseMove)
            {
                m_MouseMoveMappings.push_back(const_cast<FKeyMapping*>(&mapping));
                continue;
            }
            if (mapping.key.code == kKeyMouseScroll)
            {
                m_MouseScrollMappings.push_back(const_cast<FKeyMapping*>(&mapping));
                continue;
            }

            // Unbound / placeholder binding: nothing to look up.
            if (mapping.key.code == KeyCodes::Unknown)
                continue;

            // Real keys and mouse buttons (1100+ range) go into the key table.
            m_KeyToMappings[mapping.key.code].push_back(const_cast<FKeyMapping*>(&mapping));
        }
    }

    m_ContextDirty = false;
}

// ============================================================
// Internal: apply modifiers
// ============================================================

InputActionValue EnhancedInputSystem::ApplyModifiers(const FKeyMapping& mapping,
                                                      const InputActionValue& value) const
{
    InputActionValue result = value;
    for (const auto& mod : mapping.modifiers)
    {
        result = mod->Modify(result);
    }
    return result;
}

// ============================================================
// Internal: 连续型指针输入 (mouse move / scroll) -> action
// ============================================================

void EnhancedInputSystem::ApplyPointerMapping(const FKeyMapping& mapping, const Vector2& raw_delta)
{
    if (!mapping.action)
        return;

    const std::string&          action_name = mapping.action->GetName();
    const EInputActionValueType value_type  = mapping.action->GetValueType();

    // 按绑定声明的轴取分量：X_Y 给鼠标移动这种天然二维的输入；X / Y 只取一个分量
    // （滚轮 -> Zoom 通常写 Y）。scale 在这里是方向与灵敏度。
    float x = 0.0f;
    float y = 0.0f;
    switch (mapping.axis)
    {
        case EInputAxis::X:
            x = raw_delta.x;
            break;
        case EInputAxis::Y:
            y = raw_delta.y;
            break;
        case EInputAxis::X_Y:
            x = raw_delta.x;
            y = raw_delta.y;
            break;
        case EInputAxis::None:
        default:
            // 没声明轴：这类输入本身就是二维的，两个分量都给。
            x = raw_delta.x;
            y = raw_delta.y;
            break;
    }
    x *= mapping.scale;
    y *= mapping.scale;

    InputActionValue contribution;
    switch (value_type)
    {
        case EInputActionValueType::Axis2D:
            contribution = InputActionValue(Vector2(x, y));
            break;
        case EInputActionValueType::Axis1D:
            // 一维动作拿不了两个分量：取绑定声明的那个（X_Y 时取 y，符合滚轮语义）。
            contribution = InputActionValue(mapping.axis == EInputAxis::X ? x : y);
            break;
        case EInputActionValueType::Boolean:
        default:
            contribution = InputActionValue(x != 0.0f || y != 0.0f);
            break;
    }

    contribution = ApplyModifiers(mapping, contribution);

    auto it = m_AccumulatedValues.find(action_name);
    if (it == m_AccumulatedValues.end())
    {
        m_AccumulatedValues[action_name] = contribution;
        return;
    }

    // 同一个动作可能被多个绑定喂（多 context、或多条伪键绑定）—— 与 Step 1 的
    // 合并规则保持一致：Axis2D / Axis1D 相加，Boolean 取或。
    switch (value_type)
    {
        case EInputActionValueType::Axis2D:
            m_AccumulatedValues[action_name] =
                InputActionValue(it->second.GetAxis2D() + contribution.GetAxis2D());
            break;
        case EInputActionValueType::Axis1D:
            m_AccumulatedValues[action_name] =
                InputActionValue(it->second.GetAxis1D() + contribution.GetAxis1D());
            break;
        case EInputActionValueType::Boolean:
        default:
            m_AccumulatedValues[action_name] =
                InputActionValue(it->second.GetBoolean() || contribution.GetBoolean());
            break;
    }
}

// ============================================================
// Internal: get default trigger for an action
// ============================================================

std::shared_ptr<InputTrigger> EnhancedInputSystem::GetDefaultTrigger(InputAction* action)
{
    auto it = m_DefaultTriggers.find(action->GetName());
    if (it != m_DefaultTriggers.end())
        return it->second;

    // Choose default trigger based on action value type:
    //   Axis2D -> TriggerAlways (continuous: Move, Look)
    //   Boolean -> TriggerDown (held: Sprint, Crouch)
    std::shared_ptr<InputTrigger> trigger;
    switch (action->GetValueType())
    {
        case EInputActionValueType::Axis2D:
            trigger = std::make_shared<TriggerAlways>();
            break;
        case EInputActionValueType::Axis1D:
            trigger = std::make_shared<TriggerAlways>();
            break;
        case EInputActionValueType::Boolean:
        default:
            trigger = std::make_shared<TriggerDown>();
            break;
    }

    m_DefaultTriggers[action->GetName()] = trigger;
    return trigger;
}

// ============================================================
// Tick: accumulate raw values → apply modifiers → evaluate triggers → fire delegates
// ============================================================

void EnhancedInputSystem::Tick()
{
    // Real wall-clock delta since the previous Tick, from a monotonic clock.
    // Triggers (notably TriggerHold: m_HoldElapsed += delta_time) need the
    // actual elapsed time, not a fixed 1/60 guess — at 30fps a held key would
    // otherwise register half as fast as it really is. Clamp so an occasional
    // long stall (debugger break, hitch) can't fast-forward a Hold trigger.
    const double now_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const float delta_time = ComputeFrameDelta(m_LastTickTimeS, m_HasTicked, now_s);
    m_LastTickTimeS = now_s;
    m_HasTicked = true;

    if (m_ContextDirty)
    {
        RebuildMappingTable();
    }

    // ---- Step 1: Accumulate raw values per action from pressed keys ----
    // Clear previous frame's accumulated values
    m_AccumulatedValues.clear();

    // Track which actions have been consumed by higher-priority contexts
    std::unordered_set<std::string> consumed_actions;

    // Process contexts in priority order
    for (const auto& entry : m_ContextStack)
    {
        for (const FKeyMapping& mapping : entry.context->GetMappings())
        {
            if (!mapping.action)
                continue;

            // 伪键（鼠标移动 / 滚轮）不参与按键累积 —— 它们没有按下状态，由
            // Step 2 经 ApplyPointerMapping() 处理。
            if (mapping.key.code == kKeyMouseMove || mapping.key.code == kKeyMouseScroll)
                continue;

            const std::string& action_name = mapping.action->GetName();

            // Skip if this action was consumed by a higher-priority context
            if (consumed_actions.count(action_name))
                continue;

            InputActionValue contribution;

            if (mapping.axis != EInputAxis::None && mapping.action->GetValueType() == EInputActionValueType::Axis2D)
            {
                // Axis contribution: only contribute if the key is pressed
                bool pressed = m_PressedKeys.count(mapping.key.code) > 0;
                if (!pressed)
                    continue;

                float value = mapping.scale;

                // Build a 2D contribution
                Vector2 existing = Vector2::ZERO;
                auto it = m_AccumulatedValues.find(action_name);
                if (it != m_AccumulatedValues.end())
                {
                    existing = it->second.GetAxis2D();
                }

                if (mapping.axis == EInputAxis::X)
                    existing.x += value;
                else if (mapping.axis == EInputAxis::Y)
                    existing.y += value;

                contribution = InputActionValue(existing);
            }
            else if (mapping.action->GetValueType() == EInputActionValueType::Boolean)
            {
                bool pressed = m_PressedKeys.count(mapping.key.code) > 0;
                if (!pressed)
                    continue;
                contribution = InputActionValue(true);
            }
            else if (mapping.action->GetValueType() == EInputActionValueType::Axis1D)
            {
                bool pressed = m_PressedKeys.count(mapping.key.code) > 0;
                if (!pressed)
                    continue;
                contribution = InputActionValue(mapping.scale);
            }

            // Apply modifiers
            contribution = ApplyModifiers(mapping, contribution);

            // Merge into accumulated value
            auto it = m_AccumulatedValues.find(action_name);
            if (it != m_AccumulatedValues.end())
            {
                // Merge: for Axis2D take the latest with additive logic,
                // for Boolean take OR, for Axis1D take max
                switch (mapping.action->GetValueType())
                {
                    case EInputActionValueType::Axis2D:
                    {
                        // Already accumulated above
                        m_AccumulatedValues[action_name] = contribution;
                        break;
                    }
                    case EInputActionValueType::Boolean:
                    {
                        bool existing = it->second.GetBoolean();
                        m_AccumulatedValues[action_name] = InputActionValue(existing || contribution.GetBoolean());
                        break;
                    }
                    case EInputActionValueType::Axis1D:
                    {
                        float existing = it->second.GetAxis1D();
                        m_AccumulatedValues[action_name] = InputActionValue(existing + contribution.GetAxis1D());
                        break;
                    }
                }
            }
            else
            {
                m_AccumulatedValues[action_name] = contribution;
            }
        }
    }

    // ---- Step 2: 连续型指针输入（鼠标移动 / 滚轮）----
    //
    // 鼠标 delta 在 OnCursorPos() 里累加，滚轮在 OnScroll() 里累加。它们不是按键，
    // 走的是 RebuildMappingTable() 填出来的伪键表 —— 绑定写法：
    //     ctx->MapKey(FKey::MouseMove(),   GameActions::Look, EInputAxis::X_Y);
    //     ctx->MapKey(FKey::MouseScroll(), GameActions::Zoom, EInputAxis::Y);
    if (m_FocusMode)
    {
        const Vector2 move_delta(static_cast<float>(m_CursorDeltaX),
                                 static_cast<float>(m_CursorDeltaY));
        if (move_delta != Vector2::ZERO)
        {
            if (m_MouseMoveMappings.empty())
            {
                // 没有任何 context 绑定 FKey::MouseMove()：退回老行为直接喂
                // "Look"，免得修完伪键后反而把鼠标视角改没了。
                auto it = m_AccumulatedValues.find("Look");
                if (it != m_AccumulatedValues.end())
                    m_AccumulatedValues["Look"] = InputActionValue(it->second.GetAxis2D() + move_delta);
                else
                    m_AccumulatedValues["Look"] = InputActionValue(move_delta);
            }
            else
            {
                for (const FKeyMapping* mapping : m_MouseMoveMappings)
                    ApplyPointerMapping(*mapping, move_delta);
            }
        }
    }

    // 滚轮：同样走映射表。没有绑定就什么都不做（值在 Step 4 清掉）。
    if (m_ScrollAccumX != 0.0f || m_ScrollAccumY != 0.0f)
    {
        const Vector2 scroll_delta(m_ScrollAccumX, m_ScrollAccumY);
        for (const FKeyMapping* mapping : m_MouseScrollMappings)
            ApplyPointerMapping(*mapping, scroll_delta);
    }

    // ---- Step 2b: 触摸摇杆 / 触摸视角（移动端）----
    // 必须在 Tick 内部写：m_AccumulatedValues 在 Step 1 就清空了，而
    // InputSystem::Tick() 紧接着要读，外部注入赶不上。
    ApplyTouchAxes();

    // ---- Step 3: Evaluate triggers and fire delegates ----
    for (InputAction* action : m_RegisteredActions)
    {
        auto it = m_AccumulatedValues.find(action->GetName());
        InputActionValue value = (it != m_AccumulatedValues.end()) ? it->second : action->GetCurrentValue().ZeroValue();

        // Find the trigger for this action (from the mapping context or default)
        std::shared_ptr<InputTrigger> trigger;
        // Check if any active mapping provides a custom trigger
        for (const auto& entry : m_ContextStack)
        {
            for (const FKeyMapping& mapping : entry.context->GetMappings())
            {
                if (mapping.action == action && mapping.trigger)
                {
                    trigger = mapping.trigger;
                    break;
                }
            }
            if (trigger) break;
        }
        if (!trigger)
        {
            trigger = GetDefaultTrigger(action);
        }

        uint8_t events = trigger->Evaluate(value, delta_time);

        // Fire delegates for each triggered event
        if (events != 0)
        {
            if (events & static_cast<uint8_t>(ETriggerEvent::Started))
                action->OnTriggerEvent(ETriggerEvent::Started, value);
            if (events & static_cast<uint8_t>(ETriggerEvent::Ongoing))
                action->OnTriggerEvent(ETriggerEvent::Ongoing, value);
            if (events & static_cast<uint8_t>(ETriggerEvent::Triggered))
                action->OnTriggerEvent(ETriggerEvent::Triggered, value);
            if (events & static_cast<uint8_t>(ETriggerEvent::Canceled))
                action->OnTriggerEvent(ETriggerEvent::Canceled, value);
            if (events & static_cast<uint8_t>(ETriggerEvent::Completed))
                action->OnTriggerEvent(ETriggerEvent::Completed, value);
        }

        // Store current value for queries
        action->SetCurrentValue(value);
    }

    // 帧内量（光标位移 / 滚轮 / 按键边沿 / 触摸帧位移）**不在这里清** ——
    // 由 EndFrame() 在脚本 tick 之后滚动。原因见 EndFrame() 的注释。
}

void EnhancedInputSystem::EndFrame()
{
    // 收集器 -> 读数，然后清空收集器。
    m_PressedPrevFrame.swap(m_PressedThisFrame);
    m_PressedThisFrame.clear();
    m_ReleasedPrevFrame.swap(m_ReleasedThisFrame);
    m_ReleasedThisFrame.clear();

    m_CursorDeltaPrevX = m_CursorDeltaX;
    m_CursorDeltaPrevY = m_CursorDeltaY;
    m_CursorDeltaX = 0.0;
    m_CursorDeltaY = 0.0;

    m_ScrollPrevX = m_ScrollAccumX;
    m_ScrollPrevY = m_ScrollAccumY;
    m_ScrollAccumX = 0.0f;
    m_ScrollAccumY = 0.0f;

    // 触摸的"本帧位移"与鼠标 delta 同语义：用掉即清，清之前已经被 ApplyTouchAxes
    // 写进 m_AccumulatedValues（喂给 "Look"）了。放在这里（而不是 Tick 内）是
    // 为了让脚本在帧末之前也读得到。
    for (auto& entry : m_Touches)
    {
        entry.second.frame_dx = 0.0f;
        entry.second.frame_dy = 0.0f;
    }
}

// ============================================================
// 按键状态查询（脚本 / 玩法层）
// ============================================================

bool EnhancedInputSystem::IsKeyDown(int key_code) const
{
    return m_PressedKeys.find(key_code) != m_PressedKeys.end();
}

bool EnhancedInputSystem::WasKeyPressedThisFrame(int key_code) const
{
    return m_PressedPrevFrame.find(key_code) != m_PressedPrevFrame.end();
}

bool EnhancedInputSystem::WasKeyReleasedThisFrame(int key_code) const
{
    return m_ReleasedPrevFrame.find(key_code) != m_ReleasedPrevFrame.end();
}

bool EnhancedInputSystem::IsMouseButtonDown(int button) const
{
    return IsKeyDown(MouseButtonToKeyCode(button));
}

bool EnhancedInputSystem::WasMouseButtonPressedThisFrame(int button) const
{
    return WasKeyPressedThisFrame(MouseButtonToKeyCode(button));
}

bool EnhancedInputSystem::WasMouseButtonReleasedThisFrame(int button) const
{
    return WasKeyReleasedThisFrame(MouseButtonToKeyCode(button));
}

int EnhancedInputSystem::GetTouchIdAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Touches.size()))
    {
        return -1;
    }

    // unordered_map 的迭代顺序不确定，必须排序才能得到稳定的"第 N 根手指"。
    // 手指数量是个位数，每帧排一次无所谓。
    std::vector<int> ids;
    ids.reserve(m_Touches.size());
    for (const auto& entry : m_Touches)
    {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids[static_cast<size_t>(index)];
}
