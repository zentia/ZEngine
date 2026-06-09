#pragma once

#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Vector2.h"

#include <functional>
#include <string>
#include <vector>

// ============================================================
// InputActionValue — the value carried by an action event.
// Analogous to UE's FInputActionValue. Supports boolean (1D),
// axis1D (float), and axis2D (Vector2) value types.
// ============================================================
enum class EInputActionValueType : uint8_t
{
    Boolean,  // digital: pressed or not
    Axis1D,   // single float (e.g. trigger)
    Axis2D    // Vector2   (e.g. stick, mouse delta)
};

class InputActionValue
{
public:
    InputActionValue() : m_Type(EInputActionValueType::Boolean), m_Bool(false) {}
    explicit InputActionValue(bool val) : m_Type(EInputActionValueType::Boolean), m_Bool(val) {}
    explicit InputActionValue(float val) : m_Type(EInputActionValueType::Axis1D), m_Axis1D(val) {}
    explicit InputActionValue(const Vector2& val) : m_Type(EInputActionValueType::Axis2D), m_Axis2D(val) {}

    EInputActionValueType GetType() const { return m_Type; }

    bool GetBoolean() const { return m_Type == EInputActionValueType::Boolean ? m_Bool : false; }
    float GetAxis1D() const
    {
        switch (m_Type)
        {
            case EInputActionValueType::Boolean:  return m_Bool ? 1.0f : 0.0f;
            case EInputActionValueType::Axis1D:   return m_Axis1D;
            case EInputActionValueType::Axis2D:   return m_Axis2D.x; // magnitude approx
            default: return 0.0f;
        }
    }
    Vector2 GetAxis2D() const
    {
        switch (m_Type)
        {
            case EInputActionValueType::Boolean:  return m_Bool ? Vector2(1, 0) : Vector2::ZERO;
            case EInputActionValueType::Axis1D:   return Vector2(m_Axis1D, 0);
            case EInputActionValueType::Axis2D:   return m_Axis2D;
            default: return Vector2::ZERO;
        }
    }

    // Zero / default value for the current type.
    InputActionValue ZeroValue() const
    {
        switch (m_Type)
        {
            case EInputActionValueType::Boolean:  return InputActionValue(false);
            case EInputActionValueType::Axis1D:   return InputActionValue(0.0f);
            case EInputActionValueType::Axis2D:   return InputActionValue(Vector2::ZERO);
            default: return InputActionValue();
        }
    }

private:
    EInputActionValueType m_Type;
    bool   m_Bool = false;
    float  m_Axis1D = 0.0f;
    Vector2 m_Axis2D;
};

// ============================================================
// ETriggerEvent — when a bound delegate fires, analogous to
// UE's ETriggerEvent.
// ============================================================
enum class ETriggerEvent : uint8_t
{
    None        = 0,
    Started     = 1 << 0,  // first frame the trigger condition begins
    Ongoing     = 1 << 1,  // every frame while trigger condition holds
    Triggered   = 1 << 2,  // the action is "committed" (e.g. on press for tap, on release for release)
    Canceled    = 1 << 3,  // action was started but canceled before triggering
    Completed   = 1 << 4   // action finished (for timed triggers)
};

// ============================================================
// InputAction — a named logical action that can be bound to
// physical keys/axes through an InputMappingContext.
// Analogous to UE's UInputAction.
//
// Users create InputAction instances (e.g. "Move", "Jump",
// "Look") and subscribe to trigger events via BindAction().
// The action itself is independent of any key binding — that
// is configured in InputMappingContext.
// ============================================================
class InputAction
{
public:
    explicit InputAction(const std::string& name, EInputActionValueType value_type = EInputActionValueType::Boolean)
        : m_Name(name), m_ValueType(value_type) {}

    const std::string& GetName() const { return m_Name; }
    EInputActionValueType GetValueType() const { return m_ValueType; }

    // --- Delegate types ---
    using FActionDelegate        = std::function<void()>;
    using FActionValueDelegate   = std::function<void(const InputActionValue&)>;
    using FActionInstanceDelegate = std::function<void(const InputActionValue&, ETriggerEvent)>;

    // --- Bind overloads (mirrors UE's BindAction API) ---

    // Bind to a specific trigger event, receive value + event type
    void BindAction(ETriggerEvent trigger, FActionInstanceDelegate delegate)
    {
        m_Bindings.push_back({trigger, std::move(delegate)});
    }

    // Bind to a specific trigger event, receive value only
    void BindActionValue(ETriggerEvent trigger, FActionValueDelegate delegate)
    {
        m_Bindings.push_back({trigger, [d = std::move(delegate)](const InputActionValue& v, ETriggerEvent) { d(v); }});
    }

    // Bind to Triggered event, no params
    void BindAction(ETriggerEvent trigger, FActionDelegate delegate)
    {
        m_Bindings.push_back({trigger, [d = std::move(delegate)](const InputActionValue&, ETriggerEvent) { d(); }});
    }

    // --- Internal: called by EnhancedInputSystem when a trigger fires ---
    void OnTriggerEvent(ETriggerEvent event, const InputActionValue& value)
    {
        for (auto& binding : m_Bindings)
        {
            if ((static_cast<uint8_t>(binding.trigger_event) & static_cast<uint8_t>(event)) != 0 ||
                binding.trigger_event == event)
            {
                binding.delegate(value, event);
            }
        }
    }

    // Current accumulated value this frame (before trigger evaluation).
    const InputActionValue& GetCurrentValue() const { return m_CurrentValue; }
    void SetCurrentValue(const InputActionValue& v) { m_CurrentValue = v; }

    void ResetFrameState()
    {
        m_CurrentValue = m_CurrentValue.ZeroValue();
    }

private:
    struct FBinding
    {
        ETriggerEvent trigger_event;
        FActionInstanceDelegate delegate;
    };

    std::string m_Name;
    EInputActionValueType m_ValueType;
    InputActionValue m_CurrentValue;
    std::vector<FBinding> m_Bindings;
};

// ============================================================
// Shared (global) action instances.
// These are the common gameplay actions that replace the old
// GameCommand bitfield. Systems can also create their own
// InputAction instances if they need custom actions.
// ============================================================
namespace GameActions
{
    // Movement (Axis2D: x=right/left, y=forward/backward)
    extern InputAction Move;
    // Look / aim (Axis2D: x=yaw delta, y=pitch delta)
    extern InputAction Look;
    // Jump (Boolean)
    extern InputAction Jump;
    // Sprint (Boolean, holds while pressed)
    extern InputAction Sprint;
    // Crouch (Boolean)
    extern InputAction Crouch;
    // Fire (Boolean)
    extern InputAction Fire;
    // Toggle free camera (Boolean, triggered on press)
    extern InputAction ToggleFreeCamera;

    // Initialize all shared actions (call once at startup)
    void Init();
    // Get all shared actions for registration
    std::vector<InputAction*> GetAll();
} // namespace GameActions
