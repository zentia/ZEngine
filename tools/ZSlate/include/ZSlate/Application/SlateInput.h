#pragma once

// ============================================================================
// ZSlate/Application/SlateInput.h
//
// Input router. It is driven once per frame by the host with the *current*
// mouse state; the router turns that into enter/leave, press/release, click,
// right-click and wheel events against the widget tree cached by the last
// paint pass.
// ============================================================================

#include "../Core/SlatePaint.h"
#include "../Core/ZSlateTypes.h"
#include "../Widgets/SWidget.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ZSlate
{

class SlateInputRouter
{
public:
    void ProcessMouse(const std::shared_ptr<SWidget>& root,
                      const Vector2& mousePosition,
                      bool isOverWindow,
                      bool leftButtonDown,
                      float wheelDelta = 0.0f,
                      bool rightButtonDown = false);

    // Keyboard (delivered to the focused widget).
    void ProcessKey(uint32_t keyCode, bool isDown);
    void ProcessCharacter(uint32_t codepoint);

    // Call after the widget tree is rebuilt — cached pointers are stale.
    void Reset();

    // Double-click tuning (milliseconds / pixels).
    void SetDoubleClickTime(double milliseconds) { m_DoubleClickTimeMs = milliseconds; }
    double GetDoubleClickTime() const { return m_DoubleClickTimeMs; }
    void SetDoubleClickDistance(float pixels) { m_DoubleClickDistance = pixels; }
    float GetDoubleClickDistance() const { return m_DoubleClickDistance; }

    std::shared_ptr<SWidget> GetHoveredWidget() const { return m_Hovered.lock(); }
    std::shared_ptr<SWidget> GetFocusedWidget() const { return m_Focused.lock(); }
    void SetFocusedWidget(const std::shared_ptr<SWidget>& widget);

private:
    static std::shared_ptr<SWidget> HitTestWidget(const std::shared_ptr<SWidget>& root,
                                                  const Vector2& absolutePos);

    static void CollectAncestors(const std::shared_ptr<SWidget>& widget,
                                 std::vector<std::shared_ptr<SWidget>>& outChain);

    // True when this press is the second half of a double-click on `widget`.
    bool IsDoubleClick(const std::shared_ptr<SWidget>& widget,
                       int button, const Vector2& position) const;

    std::weak_ptr<SWidget> m_Hovered;
    std::weak_ptr<SWidget> m_Pressed;
    std::weak_ptr<SWidget> m_Focused;

    bool    m_PrevLeftDown  = false;
    bool    m_PrevRightDown = false;
    Vector2 m_LastMousePos  { 0.0f, 0.0f };
    bool    m_HasMousePos   = false;

    // ---- double-click tracking ---------------------------------------------
    std::weak_ptr<SWidget> m_LastClicked;
    int      m_LastClickButton = -1;
    double   m_LastClickTimeMs = 0.0;
    Vector2  m_LastClickPos    { 0.0f, 0.0f };
    double   m_DoubleClickTimeMs = 500.0;
    float    m_DoubleClickDistance = 5.0f;
};

} // namespace ZSlate
