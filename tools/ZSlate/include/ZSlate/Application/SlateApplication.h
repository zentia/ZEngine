#pragma once

// ============================================================================
// ZSlate/Application/SlateApplication.h
//
// Root of the framework: owns the widget tree and drives one paint pass per
// frame. The host supplies a platform, a renderer and a text measurer.
// ============================================================================

#include "../Core/SlatePlatform.h"
#include "../Core/SlatePaint.h"
#include "../Core/SlateTextMeasurer.h"
#include "../Core/ZSlateTypes.h"
#include "../Widgets/SWidget.h"

#include <memory>

namespace ZSlate
{

class SlateApplication
{
public:
    static SlateApplication& Get();

    // Replacing the root is allowed at any time (tab switching does exactly
    // that); remember to call SlateInputRouter::Reset() afterwards.
    void SetRootContent(const std::shared_ptr<SWidget>& root) { m_Root = root; }
    std::shared_ptr<SWidget> GetRootContent() const { return m_Root; }

    void PaintInto(ISlateRenderer* renderer, const UIRect& region);

    void SetTextMeasurer(ITextMeasurer* measurer) { m_TextMeasurer = measurer; }
    ITextMeasurer* GetTextMeasurer() const { return m_TextMeasurer; }

    // ---- Modal layer -------------------------------------------------------
    // While a modal widget is installed, PaintInto dims the content behind it
    // and SlateInputRouter sends every event to the modal instead of the root.
    void ShowModal(const std::shared_ptr<SWidget>& modal) { m_Modal = modal; }
    void CloseModal() { m_Modal.reset(); }
    bool HasModal() const { return m_Modal != nullptr; }
    std::shared_ptr<SWidget> GetModal() const { return m_Modal; }

private:
    SlateApplication() = default;

    std::shared_ptr<SWidget> m_Root;
    std::shared_ptr<SWidget> m_Modal;
    ITextMeasurer*           m_TextMeasurer = nullptr;
};

} // namespace ZSlate
