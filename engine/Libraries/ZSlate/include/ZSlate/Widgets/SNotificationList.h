#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"

#include <chrono>
#include <deque>
#include <functional>
#include <string>

namespace ZSlate
{

// =============================================================================
// SNotificationList — toast notification panel (UE SNotificationList analogue)
// =============================================================================
//
// A vertical list of auto-dismissing notification items. Each item has
// a message, an optional action button, and a lifetime.
//
class SNotificationList : public SWidget
{
public:
    enum class ECompletionState { Pending, Success, Fail };

    struct FNotificationInfo
    {
        std::string Message;
        std::string ActionLabel;  // e.g. "Undo"
        std::function<void()> OnAction;
        float ExpireSeconds {5.0f};
        ECompletionState State {ECompletionState::Pending};
    };

    // Add a notification. Returns index for removal.
    int32_t AddNotification(const FNotificationInfo& info)
    {
        m_Items.push_back({info, std::chrono::steady_clock::now()});
        return static_cast<int32_t>(m_Items.size()) - 1;
    }

    void RemoveNotification(int32_t index)
    {
        if (index >= 0 && index < static_cast<int32_t>(m_Items.size()))
            m_Items.erase(m_Items.begin() + index);
    }

    void Clear() { m_Items.clear(); }

    float ItemHeight {32.0f};
    float FontSize {13.0f};

    Vector2 ComputeDesiredSize() const override
    {
        return Vector2(280.0f, ItemHeight * m_Items.size());
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer || m_Items.empty()) return;
        const UIRect r = geom.ToRect();

        auto now = std::chrono::steady_clock::now();
        float curY = r.y;

        for (size_t i = 0; i < m_Items.size(); ++i)
        {
            UIRect ir(r.x, curY, r.w, ItemHeight);

            // Background based on state
            UIColor bg;
            switch (m_Items[i].Info.State)
            {
            case ECompletionState::Success: bg = UIColor(0.12f, 0.30f, 0.15f, 0.95f); break;
            case ECompletionState::Fail:    bg = UIColor(0.30f, 0.12f, 0.12f, 0.95f); break;
            default:                        bg = UIColor(0.12f, 0.16f, 0.24f, 0.95f); break;
            }
            ctx.Renderer->DrawQuad(ir, bg);
            ctx.Renderer->DrawRect(ir, UIColor(0.30f, 0.33f, 0.40f, 1.0f), 1.0f);

            // Status icon
            const char* icon;
            switch (m_Items[i].Info.State)
            {
            case ECompletionState::Success: icon = "\xE2\x9C\x93"; break;  // ✓
            case ECompletionState::Fail:    icon = "\xE2\x9C\x97"; break;  // ✗
            default:                        icon = "\xE2\x80\xA2"; break;  // •
            }
            ctx.Renderer->DrawTextLabel(
                UIRect(ir.x + 6.0f, ir.y, 22.0f, ir.h),
                icon, FontSize, UIColor(1.0f, 1.0f, 1.0f, 1.0f),
                TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

            // Message
            ctx.Renderer->DrawTextLabel(
                UIRect(ir.x + 30.0f, ir.y, ir.w - 90.0f, ir.h),
                m_Items[i].Info.Message, FontSize,
                UIColor(0.88f, 0.89f, 0.92f, 1.0f),
                TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

            // Action button
            if (!m_Items[i].Info.ActionLabel.empty())
            {
                ctx.Renderer->DrawTextLabel(
                    UIRect(ir.Right() - 70.0f, ir.y, 65.0f, ir.h),
                    m_Items[i].Info.ActionLabel, FontSize,
                    UIColor(0.4f, 0.6f, 1.0f, 1.0f),
                    TextAnchor::MiddleRight, TextWrapMode::NoWrap);
            }

            // Auto-dismiss
            float elapsed = std::chrono::duration<float>(now - m_Items[i].StartTime).count();
            if (elapsed > m_Items[i].Info.ExpireSeconds)
            {
                // Mark for removal — deferred via const_cast (Tick handles cleanup)
                const_cast<SNotificationList*>(this)->m_PendingRemoval.insert(
                    static_cast<int32_t>(i));
            }

            curY += ItemHeight;
        }
    }

    void Tick(const FGeometry&, float) override
    {
        if (m_PendingRemoval.empty()) return;
        for (auto it = m_PendingRemoval.rbegin(); it != m_PendingRemoval.rend(); ++it)
        {
            if (*it >= 0 && *it < static_cast<int32_t>(m_Items.size()))
                m_Items.erase(m_Items.begin() + *it);
        }
        m_PendingRemoval.clear();
    }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        const UIRect r = GetCachedGeometry().ToRect();

        int32_t idx = static_cast<int32_t>((pos.y - r.y) / ItemHeight);
        if (idx < 0 || idx >= static_cast<int32_t>(m_Items.size()))
            return FReply::Unhandled();

        // Hit action button area?
        float actionX = r.Right() - 70.0f;
        if (pos.x >= actionX && m_Items[idx].Info.OnAction)
        {
            m_Items[idx].Info.OnAction();
            return FReply::Handled();
        }

        return FReply::Unhandled();
    }

private:
    struct FItem
    {
        FNotificationInfo Info;
        std::chrono::steady_clock::time_point StartTime;
    };
    mutable std::vector<FItem> m_Items;
    mutable std::set<int32_t> m_PendingRemoval;
};

}  // namespace ZSlate
