#include "EditorWindow.h"

#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"

EditorWindow::EditorWindow(EditorUI* editor_ui, const char* name)
    : EditorView(editor_ui, name) {}

EditorWindowState EditorWindow::BeginGUI()
{
    // P9: native dock hosting is the only path. The leaf content rect is authoritative and
    // the dock tree's MinNodeExtent is the size floor, so no ImGui min-size constraint is
    // applied (it would force small leaves to overflow their neighbours).
    return EditorView::BeginGUI();
}
