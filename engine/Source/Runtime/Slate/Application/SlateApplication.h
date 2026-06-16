#pragma once

#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/Slate/Core/SlateClipboard.h"
#include "Runtime/Slate/Widgets/SWidget.h"
#include "Runtime/UI/Core/UITypes.h"  // UIRect

#include <memory>

class UIRenderer;

namespace ZSlate
{
// Owns the root widget tree and drives the per-frame layout + paint cycle.
// Coexists with the existing ImGui editor during migration: a host (an editor
// panel or a dedicated render hook) calls PaintInto() with the screen region it
// owns, and ZSlate fills it.
//
// This is a singleton for now (mirrors UE's FSlateApplication::Get()), which keeps
// the first integration simple; it can be de-singletonised once multiple
// independent ZSlate surfaces exist.
class SlateApplication
{
public:
    static SlateApplication& Get();

    // Install the text-measurement backend (forwards to UIRenderer::measureText
    // or the editor font atlas). Stored in the process-wide GSlateTextMeasurer.
    void SetTextMeasurer(ISlateTextMeasurer* measurer);

    // Install clipboard callbacks (so SEditableTextBox can copy/paste without
    // depending on WindowSystem/GLFW). userdata is typically GLFWwindow*.
    void SetClipboardCallbacks(SlateGetClipboardFunc getFunc,
                              SlateSetClipboardFunc setFunc,
                              void* userdata);

    void SetRootContent(std::shared_ptr<SWidget> root) { m_Root = std::move(root); }
    std::shared_ptr<SWidget> GetRootContent() const { return m_Root; }

    // Advance any animations / timers. No-op until widgets need it.
    void Tick(float delta_seconds);

    // Lay out and paint the root tree into `region` (screen-space pixels) using
    // `renderer`. Safe to call with a null root or renderer (does nothing).
    void PaintInto(UIRenderer* renderer, const UIRect& region);

private:
    SlateApplication() = default;

    std::shared_ptr<SWidget> m_Root;
};
}  // namespace ZSlate
