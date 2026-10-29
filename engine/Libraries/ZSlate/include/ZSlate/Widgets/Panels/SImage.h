#pragma once

#include "ZSlate/Core/SlateBrush.h"
#include "ZSlate/Widgets/SLeafWidget.h"

namespace ZSlate
{
/**
 * Texture (or solid color) quad Widget.
 * Reference UE SImage, using FSlateBrush to describe drawing method.
 * 
 * Usage:
 *   SImage::Make(FSlateBrush::Image(tex))  -- simple image
 *   SImage::Make(FSlateBrush::Box(tex, FMargin(0.25f)))  -- nine-slice
 *   SImage::Make(FSlateBrush::RoundedBox(8.0f, fill, border, 1.0f))  -- rounded box
 */
class SImage : public SLeafWidget
{
public:
    // Core: uses FSlateBrush to describe texture, color, margin, etc.
    FSlateBrush Brush;

public:
    // Factory methods
    static std::shared_ptr<SImage> Make(const FSlateBrush& InBrush)
    {
        std::shared_ptr<SImage> Image = std::make_shared<SImage>();
        Image->Brush = InBrush;
        return Image;
    }

    // Simple factory: direct texture
    static std::shared_ptr<SImage> Make(void* InTexture, const UIColor& InTint = UIColor(1,1,1,1))
    {
        return Make(FSlateBrush::Image(InTexture, InTint));
    }

    Vector2 ComputeDesiredSize() const override
    {
        return Brush.GetImageSize();
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override;
};
}  // namespace ZSlate
