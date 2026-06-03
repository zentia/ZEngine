#pragma once

#include "Runtime/UMG/Core/UUserWidget.h"

#include <memory>

namespace ZUMG
{
class UTextBlock;

// A self-contained C++ UMG sample (the P1 acceptance artifact): a titled panel
// with a counter button, an editable text field, a check box and a slider. Build
// composes the UWidget tree; AddToViewport() makes it visible + interactive in
// the runtime through the shared UIPass / SlateInputRouter path. Toggled at
// runtime via the "umg.demo" console command.
class UMGDemoWidget : public UUserWidget
{
public:
    void Build() override;

private:
    int m_ClickCount {0};
    std::shared_ptr<UTextBlock> m_CounterLabel;
    std::shared_ptr<UTextBlock> m_SliderLabel;
};
}  // namespace ZUMG
