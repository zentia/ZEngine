#pragma once

#include "Runtime/Function/Input/InputAction.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// InputTrigger — determines WHEN an action fires based on
// input state transitions. Analogous to UE's UInputTrigger.
//
// The EnhancedInputSystem evaluates each active mapping's
// trigger every frame, feeding it the raw accumulated value.
// The trigger returns which ETriggerEvent(s) to fire.
// ============================================================
class InputTrigger
{
public:
    virtual ~InputTrigger() = default;

    // Called once per frame per action binding that uses this trigger.
    // `value` is the current raw input value for this action.
    // Returns a bitmask of ETriggerEvent values to fire.
    virtual uint8_t Evaluate(const InputActionValue& value, float delta_time) = 0;

    // Reset internal state (e.g. hold timers). Called when the mapping
    // context is popped or the action is canceled.
    virtual void Reset() {}
};

// --- Built-in trigger types ---

// Triggered on the frame the input transitions from zero to non-zero (press).
// Also fires Started on the press frame.
class TriggerPressed : public InputTrigger
{
public:
    uint8_t Evaluate(const InputActionValue& value, float delta_time) override
    {
        const bool is_actuated = value.GetBoolean() || value.GetAxis1D() != 0.0f || value.GetAxis2D() != Vector2::ZERO;
        uint8_t result = 0;
        if (is_actuated && !m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Started);
            result |= static_cast<uint8_t>(ETriggerEvent::Triggered);
        }
        else if (is_actuated && m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Ongoing);
        }
        m_WasActuated = is_actuated;
        return result;
    }
    void Reset() override { m_WasActuated = false; }

private:
    bool m_WasActuated = false;
};

// Fires Started+Triggered on press, then Ongoing every frame while held,
// and Completed on release.
class TriggerDown : public InputTrigger
{
public:
    uint8_t Evaluate(const InputActionValue& value, float delta_time) override
    {
        const bool is_actuated = value.GetBoolean() || value.GetAxis1D() != 0.0f || value.GetAxis2D() != Vector2::ZERO;
        uint8_t result = 0;
        if (is_actuated && !m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Started);
            result |= static_cast<uint8_t>(ETriggerEvent::Triggered);
        }
        else if (is_actuated && m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Ongoing);
        }
        else if (!is_actuated && m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Completed);
        }
        m_WasActuated = is_actuated;
        return result;
    }
    void Reset() override { m_WasActuated = false; }

private:
    bool m_WasActuated = false;
};

// Fires only on release (Started on press, Ongoing while held,
// Triggered+Completed on release). Good for "charge" actions.
class TriggerReleased : public InputTrigger
{
public:
    uint8_t Evaluate(const InputActionValue& value, float delta_time) override
    {
        const bool is_actuated = value.GetBoolean() || value.GetAxis1D() != 0.0f || value.GetAxis2D() != Vector2::ZERO;
        uint8_t result = 0;
        if (is_actuated && !m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Started);
        }
        else if (is_actuated && m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Ongoing);
        }
        else if (!is_actuated && m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Triggered);
            result |= static_cast<uint8_t>(ETriggerEvent::Completed);
        }
        m_WasActuated = is_actuated;
        return result;
    }
    void Reset() override { m_WasActuated = false; }

private:
    bool m_WasActuated = false;
};

// Fires Triggered after the input is held for `hold_time` seconds.
class TriggerHold : public InputTrigger
{
public:
    explicit TriggerHold(float hold_time = 0.5f) : m_HoldTime(hold_time) {}

    uint8_t Evaluate(const InputActionValue& value, float delta_time) override
    {
        const bool is_actuated = value.GetBoolean() || value.GetAxis1D() != 0.0f || value.GetAxis2D() != Vector2::ZERO;
        uint8_t result = 0;
        if (is_actuated)
        {
            if (!m_WasActuated)
            {
                result |= static_cast<uint8_t>(ETriggerEvent::Started);
                m_HoldElapsed = 0.0f;
            }
            m_HoldElapsed += delta_time;
            if (m_HoldElapsed >= m_HoldTime)
            {
                result |= static_cast<uint8_t>(ETriggerEvent::Triggered);
                result |= static_cast<uint8_t>(ETriggerEvent::Ongoing);
            }
            else
            {
                result |= static_cast<uint8_t>(ETriggerEvent::Ongoing);
            }
        }
        else
        {
            if (m_WasActuated)
            {
                if (m_HoldElapsed < m_HoldTime)
                {
                    result |= static_cast<uint8_t>(ETriggerEvent::Canceled);
                }
                result |= static_cast<uint8_t>(ETriggerEvent::Completed);
            }
            m_HoldElapsed = 0.0f;
        }
        m_WasActuated = is_actuated;
        return result;
    }
    void Reset() override { m_WasActuated = false; m_HoldElapsed = 0.0f; }

private:
    float m_HoldTime;
    float m_HoldElapsed = 0.0f;
    bool  m_WasActuated = false;
};

// Always fires Ongoing while the input is non-zero. Good for
// continuous axes (Look, Move) where you want every-frame value.
class TriggerAlways : public InputTrigger
{
public:
    uint8_t Evaluate(const InputActionValue& value, float delta_time) override
    {
        const bool is_actuated = value.GetBoolean() || value.GetAxis1D() != 0.0f || value.GetAxis2D() != Vector2::ZERO;
        uint8_t result = 0;
        if (is_actuated)
        {
            if (!m_WasActuated)
                result |= static_cast<uint8_t>(ETriggerEvent::Started);
            result |= static_cast<uint8_t>(ETriggerEvent::Ongoing);
        }
        else if (m_WasActuated)
        {
            result |= static_cast<uint8_t>(ETriggerEvent::Completed);
        }
        m_WasActuated = is_actuated;
        return result;
    }
    void Reset() override { m_WasActuated = false; }

private:
    bool m_WasActuated = false;
};

// Toggle trigger: fires Triggered on press, then toggles an internal
// state. Good for toggle-actions like free camera.
class TriggerToggle : public InputTrigger
{
public:
    uint8_t Evaluate(const InputActionValue& value, float delta_time) override
    {
        const bool is_actuated = value.GetBoolean() || value.GetAxis1D() != 0.0f || value.GetAxis2D() != Vector2::ZERO;
        uint8_t result = 0;
        if (is_actuated && !m_WasActuated)
        {
            m_Toggled = !m_Toggled;
            result |= static_cast<uint8_t>(ETriggerEvent::Started);
            result |= static_cast<uint8_t>(ETriggerEvent::Triggered);
        }
        m_WasActuated = is_actuated;
        return result;
    }
    void Reset() override { m_WasActuated = false; m_Toggled = false; }
    bool IsToggled() const { return m_Toggled; }

private:
    bool m_WasActuated = false;
    bool m_Toggled = false;
};

// ============================================================
// InputModifier — transforms raw input values before trigger
// evaluation. Analogous to UE's UInputModifier.
// ============================================================
class InputModifier
{
public:
    virtual ~InputModifier() = default;
    virtual InputActionValue Modify(const InputActionValue& value) const = 0;
};

// Dead zone modifier: values below threshold are zeroed.
class ModifierDeadZone : public InputModifier
{
public:
    explicit ModifierDeadZone(float lower = 0.15f, float upper = 0.25f)
        : m_Lower(lower), m_Upper(upper) {}

    InputActionValue Modify(const InputActionValue& value) const override
    {
        switch (value.GetType())
        {
            case EInputActionValueType::Axis1D:
            {
                float v = value.GetAxis1D();
                float abs_v = std::fabs(v);
                if (abs_v <= m_Lower) return InputActionValue(0.0f);
                if (abs_v >= m_Upper) return InputActionValue(v);
                float remapped = (abs_v - m_Lower) / (m_Upper - m_Lower);
                return InputActionValue(v > 0 ? remapped : -remapped);
            }
            case EInputActionValueType::Axis2D:
            {
                Vector2 v = value.GetAxis2D();
                float mag = std::sqrt(v.x * v.x + v.y * v.y);
                if (mag <= m_Lower) return InputActionValue(Vector2::ZERO);
                if (mag >= m_Upper) return InputActionValue(v);
                float remapped = (mag - m_Lower) / (m_Upper - m_Lower);
                return InputActionValue(v * (remapped / mag));
            }
            default:
                return value;
        }
    }

private:
    float m_Lower;
    float m_Upper;
};

// Scalar modifier: multiplies axis values.
class ModifierScalar : public InputModifier
{
public:
    explicit ModifierScalar(float scale) : m_Scale(scale) {}

    InputActionValue Modify(const InputActionValue& value) const override
    {
        switch (value.GetType())
        {
            case EInputActionValueType::Axis1D:
                return InputActionValue(value.GetAxis1D() * m_Scale);
            case EInputActionValueType::Axis2D:
                return InputActionValue(value.GetAxis2D() * m_Scale);
            default:
                return value;
        }
    }

private:
    float m_Scale;
};

// Swizzle input axis (e.g. flip Y for inverted mouse).
class ModifierSwizzleAxis : public InputModifier
{
public:
    ModifierSwizzleAxis(bool flip_x = false, bool flip_y = true)
        : m_FlipX(flip_x), m_FlipY(flip_y) {}

    InputActionValue Modify(const InputActionValue& value) const override
    {
        if (value.GetType() == EInputActionValueType::Axis2D)
        {
            Vector2 v = value.GetAxis2D();
            if (m_FlipX) v.x = -v.x;
            if (m_FlipY) v.y = -v.y;
            return InputActionValue(v);
        }
        return value;
    }

private:
    bool m_FlipX;
    bool m_FlipY;
};
