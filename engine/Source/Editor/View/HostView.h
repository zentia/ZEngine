#pragma once
#include "Runtime/Core/Math/Rect.h"

class HostView
{
public:
    void InvokeOnGUI(RectT<float>& on_gui_position);

protected:
    std::function<void()> m_OnGui;
};