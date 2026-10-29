#pragma once

// SSpacer: an empty widget with a configurable desired size.
// UE analogue: Widgets/Layout/SSpacer.h
// TODO: replace with full implementation from ZSlate submodule.

#include "ZSlate/Widgets/SWidget.h"

namespace ZSlate
{

class SSpacer : public SWidget
{
public:
    SSpacer() = default;
    explicit SSpacer(const Vector2& size) : m_Size(size) {}
    virtual ~SSpacer() = default;

    void SetSize(const Vector2& s) { m_Size = s; }

    Vector2 ComputeDesiredSize() const override { return m_Size; }
    void ArrangeChildren(const FGeometry& geom, std::vector<FArrangedWidget>& out) const override { (void)geom; (void)out; }
    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override { (void)ctx; (void)geom; }

private:
    Vector2 m_Size {0.0f, 0.0f};
};

}  // namespace ZSlate
