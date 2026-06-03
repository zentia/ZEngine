#pragma once

#include "Editor/Platform/Interface/EditorView.h"

// Unity EditorWindow: dockable ImGui panel hosted by EditorUI.
class EditorWindow : public EditorView
{
public:
    explicit EditorWindow(EditorUI* editor_ui, const char* name);
    virtual EditorWindowState BeginGUI() override;
};
