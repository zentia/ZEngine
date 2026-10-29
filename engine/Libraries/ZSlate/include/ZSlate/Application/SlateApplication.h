#pragma once

#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Core/SlateClipboard.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "ZSlate/Widgets/SWidget.h"

#include <memory>

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

    // Install the text-measurement backend (forwards to ISlateRenderer::MeasureText
    // or the editor font atlas). Stored in the process-wide GSlateTextMeasurer.
    void SetTextMeasurer(ISlateTextMeasurer* measurer);

    // Install clipboard callbacks (so SEditableTextBox can copy/paste without
    // depending on WindowSystem/GLFW). userdata is typically GLFWwindow*.
    void SetClipboardCallbacks(SlateGetClipboardFunc getFunc,
                              SlateSetClipboardFunc setFunc,
                              void* userdata);

    void SetRootContent(std::shared_ptr<SWidget> root) { m_Root = std::move(root); }
    std::shared_ptr<SWidget> GetRootContent() const { return m_Root; }

    // ---- Modal layer (UE FSlateApplication::AddModalWindow, simplified) -----
    // While a modal widget is installed, PaintInto dims the root content and
    // paints the modal centred at its desired size on top. Hosts route input
    // to GetInputRoot() so the content behind the modal cannot be clicked.
    void ShowModal(std::shared_ptr<SWidget> modal) { m_Modal = std::move(modal); }
    // The closed modal is kept alive until the next PaintInto: CloseModal is
    // typically called from a click handler *inside* the modal, which must
    // not be destroyed while it is still on the call stack.
    void CloseModal() { if (m_Modal) m_ClosedModal = std::move(m_Modal); }
    bool HasModal() const { return m_Modal != nullptr; }
    std::shared_ptr<SWidget> GetModal() const { return m_Modal; }

    // The widget tree that should receive input this frame.
    std::shared_ptr<SWidget> GetInputRoot() const { return m_Modal ? m_Modal : m_Root; }

    // Colour drawn over the root content while a modal is open.
    UIColor ModalScrimColor {0.0f, 0.0f, 0.0f, 0.45f};

    // Advance any animations / timers. No-op until widgets need it.
    void Tick(float delta_seconds);

    // Lay out and paint the root tree into `region` (screen-space pixels) using
    // the platform's renderer. Safe to call with a null root or renderer (does nothing).
    void PaintInto(ISlateRenderer* renderer, const UIRect& region);

private:
    SlateApplication() = default;

    std::shared_ptr<SWidget> m_Root;
    std::shared_ptr<SWidget> m_Modal;
    std::shared_ptr<SWidget> m_ClosedModal;
};
}  // namespace ZSlate
