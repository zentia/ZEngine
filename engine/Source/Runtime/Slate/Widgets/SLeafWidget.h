#pragma once

#include "Runtime/Slate/Widgets/SWidget.h"

namespace ZSlate
{
// A widget with no children (text, image, spacer, ...). Concrete leaves only
// implement ComputeDesiredSize() and OnPaint().
class SLeafWidget : public SWidget
{
public:
    int GetChildCount() const final { return 0; }
    std::shared_ptr<SWidget> GetChildAt(int /*index*/) const final { return nullptr; }
};
}  // namespace ZSlate
