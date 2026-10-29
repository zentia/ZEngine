#pragma once

#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Core/SlateKeys.h"
#include "ZSlate/Core/SlateGeometry.h"

#include <memory>
#include <vector>

namespace ZSlate
{

// =============================================================================
// FNavigationRouter — Tab/Arrow focus navigation (UE FNavigationConfig analogue)
// =============================================================================
//
// Implements keyboard focus movement: Tab (next/prev), Arrow keys (directional).
// Call ProcessKey on each keyboard event; returns true if focus moved.
//
class FNavigationRouter
{
public:
    // Call from the host's keyboard handler. Returns true if navigation consumed the key.
    bool ProcessKey(EKey key, std::shared_ptr<SWidget> root)
    {
        if (!root) return false;

        switch (key)
        {
        case EKey::Tab:
            return NavigateNext(root, true);  // forward
        case EKey::Left:
            return NavigateDirection(root, EDirection::Left);
        case EKey::Right:
            return NavigateDirection(root, EDirection::Right);
        case EKey::Up:
            return NavigateDirection(root, EDirection::Up);
        case EKey::Down:
            return NavigateDirection(root, EDirection::Down);
        default:
            return false;
        }
    }

    void SetFocusedWidget(std::shared_ptr<SWidget> widget)
    {
        m_FocusedWidget = std::move(widget);
    }

    std::shared_ptr<SWidget> GetFocusedWidget() const { return m_FocusedWidget.lock(); }

private:
    std::weak_ptr<SWidget> m_FocusedWidget;

    enum class EDirection { Left, Right, Up, Down };

    // Tab navigation: collect all focusable widgets, focus next/prev
    bool NavigateNext(std::shared_ptr<SWidget> root, bool forward)
    {
        std::vector<std::shared_ptr<SWidget>> focusable;
        CollectFocusable(root, focusable);

        if (focusable.empty()) return false;

        auto cur = m_FocusedWidget.lock();
        int32_t curIdx = -1;

        if (cur)
        {
            for (int32_t i = 0; i < static_cast<int32_t>(focusable.size()); ++i)
            {
                if (focusable[i] == cur) { curIdx = i; break; }
            }
        }

        int32_t nextIdx;
        if (forward)
            nextIdx = (curIdx + 1) % focusable.size();
        else
            nextIdx = (curIdx <= 0) ? static_cast<int32_t>(focusable.size()) - 1 : curIdx - 1;

        // Lose old focus
        if (auto old = m_FocusedWidget.lock())
            old->OnFocusLost();

        m_FocusedWidget = focusable[nextIdx];
        focusable[nextIdx]->OnFocusReceived();
        return true;
    }

    // Directional navigation: find nearest widget in the given direction
    bool NavigateDirection(std::shared_ptr<SWidget> root, EDirection dir)
    {
        auto cur = m_FocusedWidget.lock();
        if (!cur) return NavigateNext(root, true);

        UIRect curRect = cur->GetCachedGeometry().ToRect();
        Vector2 curCenter(curRect.x + curRect.w * 0.5f, curRect.y + curRect.h * 0.5f);

        std::vector<std::shared_ptr<SWidget>> focusable;
        CollectFocusable(root, focusable);

        float bestDist = 1e9f;
        std::shared_ptr<SWidget> best;

        for (auto& w : focusable)
        {
            if (w == cur) continue;
            UIRect wr = w->GetCachedGeometry().ToRect();
            Vector2 wc(wr.x + wr.w * 0.5f, wr.y + wr.h * 0.5f);

            float dx = wc.x - curCenter.x;
            float dy = wc.y - curCenter.y;

            // Must be roughly in the right direction
            bool inDir = false;
            switch (dir)
            {
            case EDirection::Left:  inDir = dx < -1.0f && std::abs(dy) < wr.h; break;
            case EDirection::Right: inDir = dx > 1.0f  && std::abs(dy) < wr.h; break;
            case EDirection::Up:    inDir = dy < -1.0f && std::abs(dx) < wr.w; break;
            case EDirection::Down:  inDir = dy > 1.0f  && std::abs(dx) < wr.w; break;
            }
            if (!inDir) continue;

            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < bestDist) { bestDist = dist; best = w; }
        }

        if (best)
        {
            cur->OnFocusLost();
            m_FocusedWidget = best;
            best->OnFocusReceived();
            return true;
        }
        return false;
    }

    void CollectFocusable(std::shared_ptr<SWidget> widget,
                          std::vector<std::shared_ptr<SWidget>>& out) const
    {
        if (!widget || !widget->IsVisible()) return;

        if (widget->SupportsKeyboardFocus() && widget->IsHitTestVisible())
            out.push_back(widget);

        int32_t n = widget->GetChildCount();
        for (int32_t i = 0; i < n; ++i)
        {
            auto child = widget->GetChildAt(i);
            if (child)
                CollectFocusable(child, out);
        }
    }
};

}  // namespace ZSlate
