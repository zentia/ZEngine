#pragma once

#include "ZSlate/Core/ZSlateTypes.h"

namespace ZSlate
{
class SWidget;

// Result of an input event handler (UE Slate FReply analogue). Tells the router
// whether the event was consumed and whether mouse capture should change.
class FReply
{
public:
    static FReply Handled() { return FReply(true); }
    static FReply Unhandled() { return FReply(false); }

    bool IsHandled() const { return m_Handled; }

    // Route all subsequent mouse events to `widget` until capture is released.
    FReply& CaptureMouse(SWidget* widget)
    {
        m_Capture = true;
        m_Captor = widget;
        return *this;
    }
    FReply& ReleaseMouseCapture()
    {
        m_Release = true;
        return *this;
    }

    bool ShouldCaptureMouse() const { return m_Capture; }
    bool ShouldReleaseCapture() const { return m_Release; }
    SWidget* GetMouseCaptor() const { return m_Captor; }

private:
    explicit FReply(bool handled)
        : m_Handled(handled) {}

    bool m_Handled;
    bool m_Capture {false};
    bool m_Release {false};
    SWidget* m_Captor {nullptr};
};
}  // namespace ZSlate
