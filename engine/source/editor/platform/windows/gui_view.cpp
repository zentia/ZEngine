#include "editor/platform/interface/gui_view.h"

namespace Z
{
    extern GUIView g_GUIView;
    
    GUIView::GUIView(/* args */) {}
    GUIView::~GUIView() {}
    void GUIView::RepaintAll(bool performAutorepaint) {}
} // namespace Z