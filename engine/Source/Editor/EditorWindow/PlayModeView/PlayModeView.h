#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Function/Render/Texture/RenderTexture.h"

class PlayModeView : public EditorWindow
{
public:
    explicit PlayModeView(EditorUI* editor_ui, const char* name, const ViewportType viewport_id);
    virtual EditorWindowState BeginGUI() override;
    void UpdateViewport();

protected:
    void ClearViewport();
    virtual void OnViewportHidden() {}

    ViewportType m_ViewportId;
};
