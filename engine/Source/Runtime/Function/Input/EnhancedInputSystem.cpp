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
    }
    else if (action == KeyCodes::RELEASE)
    {
        m_PressedKeys.erase(key_code);
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
    }
    else if (action == KeyCodes::RELEASE)
    {
        m_PressedKeys.erase(key_code);
    }
}

void EnhancedInputSystem::OnScroll(double xoffset, double yoffset)
{
    m_ScrollAccumX += static_cast<float>(xoffset);
    m_ScrollAccumY += static_cast<float>(yoffset);
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
            // Determine the key category
            if (mapping.key.code == KeyCodes::Unknown)
            {
                // Could be a special mapping (mouse move, scroll)
                continue;
            }

            // Mouse move and scroll are handled separately via pseudo-keys
            // For now, key-based mappings go into the key table.
            // Mouse button mappings (1000+ range) also go here.
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
    const float delta_time = 1.0f / 60.0f; // TODO: use real delta time

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

    // ---- Step 2: Add special continuous values (Look from mouse delta) ----
    // Mouse delta is accumulated in OnCursorPos and converted to the Look action
    if (m_FocusMode)
    {
        Vector2 look_delta(static_cast<float>(m_CursorDeltaX), static_cast<float>(m_CursorDeltaY));
        if (look_delta != Vector2::ZERO)
        {
            auto it = m_AccumulatedValues.find("Look");
            if (it != m_AccumulatedValues.end())
            {
                Vector2 existing = it->second.GetAxis2D();
                m_AccumulatedValues["Look"] = InputActionValue(existing + look_delta);
            }
            else
            {
                m_AccumulatedValues["Look"] = InputActionValue(look_delta);
            }
        }
    }

    // Scroll
    if (m_ScrollAccumY != 0.0f)
    {
        // Could map scroll to a Scroll action, but for now just clear it
        // This is where you'd add scroll-based action accumulation
    }

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

    // ---- Step 4: Clear per-frame deltas ----
    m_CursorDeltaX = 0.0;
    m_CursorDeltaY = 0.0;
    m_ScrollAccumX = 0.0f;
    m_ScrollAccumY = 0.0f;
}
