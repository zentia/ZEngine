#include "Editor/Platform/Interface/GUIView.h"

#include "Runtime/Application/Application.h"
#include "Runtime/Core/Memory/AutoMemoryTyped.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "assert.h"

#include <Windows.h>
static GUIViews g_GUIView;
static int s_InsideRepaintAllGUIViews = 0;

GUIView::GUIView(/* args */)
    : m_AutoRepaint(false), m_NeedsRepaint(false) {}
GUIView::~GUIView() {}
void GUIView::RepaintAll(bool perform_auto_repaint)
{
    RepaintViews(perform_auto_repaint, g_GUIView);
}

int GUIView::s_InsideRepaintLimit = 1;
void GUIView::RepaintViews(bool perform_auto_repaint, const GUIViews& views)
{
    if (s_InsideRepaintAllGUIViews > s_InsideRepaintLimit)
        return;

    assert(!perform_auto_repaint || GET_SYSTEM(Application)->MayUpdate());

    auto need_repaint = AutoMemoryTyped<const GUIView*>(views.size(), CountTag {});
    int repaint_count = 0;
    for (const GUIView* view : views)
    {
        bool should_auto_repaint = view->m_AutoRepaint && perform_auto_repaint;
        if (view->m_NeedsRepaint || should_auto_repaint || GetUpdateRect(view->getWindowHandle(), nullptr, false))
        {
            need_repaint[repaint_count++] = view;
        }
    }
}