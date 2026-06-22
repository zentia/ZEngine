#pragma once

#include "Editor/EditorWindow/PlayModeView/PlayModeView.h"
#include "ZSlate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <memory>
#include <string>

namespace ZSlate
{
class SWidget;
}

// Native ZSlate replacement for the legacy ImGui GameWindow.
//
// The Game viewport is a "transparent hole" panel: the 3D game render target is
// composited UNDER the (background-less) ImGui host window by RenderSystem after
// the editor reports the panel rect through PlayModeView::UpdateViewport. So the
// only thing this window actually paints is a small informational text overlay
// (camera-speed readout in edit mode, the Alt-to-toggle-cursor hint in play
// mode). That overlay is now drawn through the native ZSlate batched renderer
// (BatchedUIRenderer) instead of ImGui::TextColored.
//
// Deliberately NO InvisibleButton / input router: capturing the panel area would
// steal mouse input from the game composited beneath it. The overlay is purely
// presentational.
class ZSlateGameWindow : public PlayModeView
{
public:
    explicit ZSlateGameWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    void BuildOverlay(bool editor_mode, float camera_speed, float scale);

    std::shared_ptr<ZSlate::SWidget> m_Root;

    // Rebuild-gating: the overlay tree is only rebuilt when the displayed state
    // actually changes (mode flip / camera-speed delta / UI scale change), not
    // every frame.
    bool m_BuiltEditorMode {false};
    float m_BuiltCameraSpeed {-1.0f};
    float m_BuiltScale {-1.0f};
};
