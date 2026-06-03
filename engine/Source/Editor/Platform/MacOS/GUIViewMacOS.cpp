#include "Editor/Platform/Interface/GUIView.h"

GUIView::GUIView()
    : m_AutoRepaint(false), m_NeedsRepaint(false) {}
GUIView::~GUIView() = default;

void GUIView::RepaintAll(bool)
{
}
