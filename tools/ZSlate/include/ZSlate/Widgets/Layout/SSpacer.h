#pragma once

// ============================================================================
// ZSlate/Widgets/Layout/SSpacer.h
//
// A blank widget with a fixed desired size — the canonical way to express
// "8 pixels of horizontal gap" or "60 pixels of vertical gap" in ZSlate.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/ZSlateTypes.h"

namespace ZSlate
{

class SSpacer : public SWidget
{
public:
    explicit SSpacer(const Vector2& size) : m_Size(size) {}

    void SetSize(const Vector2& size) { m_Size = size; }
    const Vector2& GetSize() const { return m_Size; }

    Vector2 ComputeDesiredSize() const override { return m_Size; }

private:
    Vector2 m_Size { 0.0f, 0.0f };
};

} // namespace ZSlate
