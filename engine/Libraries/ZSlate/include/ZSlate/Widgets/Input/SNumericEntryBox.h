#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"
#include "ZSlate/Widgets/Input/SSpinBox.h"

#include <functional>
#include <string>

namespace ZSlate
{

// =============================================================================
// SNumericEntryBox<T> — typed numeric input (UE SNumericEntryBox analogue)
// =============================================================================
//
// A SSpinBox with text-field editing support. Inline edit-on-click.
//
template <typename T = float>
class SNumericEntryBox : public SCompoundWidget
{
public:
    T Value {};
    T MinValue {std::numeric_limits<T>::lowest()};
    T MaxValue {std::numeric_limits<T>::max()};
    T Step {static_cast<T>(1)};

    float FontSize {13.0f};

    std::function<void(T)> OnValueChanged;

    Vector2 ComputeDesiredSize() const override { return Vector2(140.0f, 26.0f); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Background
        UIColor bg = m_Dragging ? UIColor(0.22f, 0.24f, 0.30f, 1.0f)
                   : m_Editing  ? UIColor(0.20f, 0.25f, 0.35f, 1.0f)
                   : m_Hovered  ? UIColor(0.16f, 0.17f, 0.21f, 1.0f)
                   :              UIColor(0.12f, 0.12f, 0.14f, 1.0f);
        ctx.Renderer->DrawQuad(r, bg);
        ctx.Renderer->DrawRect(r, m_Editing
            ? UIColor(0.45f, 0.55f, 0.80f, 1.0f)
            : UIColor(0.35f, 0.38f, 0.45f, 1.0f), 1.0f);

        // Minus/Plus buttons
        float btnW = 24.0f;
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x, r.y, btnW, r.h), "-", FontSize + 2.0f,
            UIColor(0.7f, 0.7f, 0.7f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
        ctx.Renderer->DrawTextLabel(
            UIRect(r.Right() - btnW, r.y, btnW, r.h), "+", FontSize + 2.0f,
            UIColor(0.7f, 0.7f, 0.7f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        // Value
        std::string label = m_Editing ? m_EditBuffer : Format(Value);
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + btnW + 2.0f, r.y, r.w - 2.0f * btnW - 4.0f, r.h),
            (m_Editing && (m_CursorBlink < 30)) ? label + "|" : label,
            FontSize,
            m_Editing ? UIColor(0.5f, 0.8f, 1.0f, 1.0f)
                      : UIColor(0.88f, 0.89f, 0.92f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
    }

    // ---- Input ---------------------------------------------------------------

    bool SupportsKeyboardFocus() const override { return true; }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }
    void OnFocusLost() override { m_Editing = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        const UIRect r = GetCachedGeometry().ToRect();
        float btnW = 24.0f;

        if (pos.x < r.x + btnW)
        {
            SetValue(Value - Step);
            return FReply::Handled();
        }
        if (pos.x > r.Right() - btnW)
        {
            SetValue(Value + Step);
            return FReply::Handled();
        }

        // Center — if already editing, just focus; if not, start drag
        if (m_Editing)
        {
            // Commit current edit and enter editing
            T v = FromString(m_EditBuffer);
            if (v >= MinValue && v <= MaxValue)
                SetValue(v);
        }
        m_Dragging = true;
        m_DragStartValue = Value;
        m_DragStartY = pos.y;
        return FReply::Handled().CaptureMouse(const_cast<SNumericEntryBox*>(this));
    }

    FReply OnMouseButtonUp(const Vector2& pos, int btn) override
    {
        if (btn == 1)  // Right click → begin text editing
        {
            m_Editing = true;
            m_EditBuffer = Format(Value);
            m_CursorBlink = 0;
            m_Dragging = false;
            return FReply::Handled();
        }
        if (btn != 0 || !m_Dragging) return FReply::Unhandled();

        // If didn't drag much, enter text edit mode
        if (std::abs(pos.y - m_DragStartY) < 3.0f)
        {
            m_Editing = true;
            m_EditBuffer = Format(Value);
            m_CursorBlink = 0;
        }

        m_Dragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (!m_Dragging) return;
        float delta = m_DragStartY - pos.y;
        float sensitivity = std::max(static_cast<float>(MaxValue - MinValue), 1.0f) / 200.0f;
        SetValue(static_cast<T>(m_DragStartValue + static_cast<float>(delta) * sensitivity));
    }

    void OnMouseCaptureLost() override { m_Dragging = false; }

    FReply OnMouseWheel(const Vector2&, float delta) override
    {
        SetValue(static_cast<T>(Value + static_cast<T>(delta * static_cast<float>(Step) * 0.5f)));
        return FReply::Handled();
    }

    void OnKeyChar(unsigned int codepoint) override
    {
        if (!m_Editing) return;

        if (codepoint == '\r' || codepoint == '\n')  // Enter — commit
        {
            T v = FromString(m_EditBuffer);
            if (v >= MinValue && v <= MaxValue)
                SetValue(v);
            m_Editing = false;
            return;
        }
        if (codepoint == 27)  // Escape — cancel
        {
            m_Editing = false;
            return;
        }
        if (codepoint == '\b' || codepoint == 127)
        {
            if (!m_EditBuffer.empty()) m_EditBuffer.pop_back();
            return;
        }
        if (codepoint >= 32 && codepoint < 256)
        {
            m_EditBuffer += static_cast<char>(codepoint);
            m_CursorBlink = 0;
        }
    }

    void Tick(const FGeometry&, float deltaSec) override
    {
        if (m_Editing)
            m_CursorBlink = std::fmod(m_CursorBlink + deltaSec * 120.0f, 60.0f);
    }

private:
    mutable bool m_Hovered {false};
    mutable bool m_Dragging {false};
    mutable bool m_Editing {false};
    mutable float m_DragStartValue {0.0f};
    mutable float m_DragStartY {0.0f};
    mutable std::string m_EditBuffer;
    mutable float m_CursorBlink {0.0f};

    void SetValue(T v)
    {
        v = std::clamp(v, MinValue, MaxValue);
        if (v == Value) return;
        Value = v;
        if (OnValueChanged) OnValueChanged(Value);
    }

    static std::string Format(T v)
    {
        char buf[32];
        if constexpr (std::is_floating_point_v<T>)
            std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        else
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }

    static T FromString(const std::string& s)
    {
        try { return static_cast<T>(std::stod(s)); }
        catch (...) { return T{}; }
    }
};

}  // namespace ZSlate
