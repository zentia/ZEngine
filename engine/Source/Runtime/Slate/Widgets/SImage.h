#pragma once

#include "Runtime/Slate/Core/SlateBrush.h"
#include "Runtime/Slate/Widgets/SLeafWidget.h"

namespace ZSlate
{
/**
 * 纹理（或纯色）四边形 Widget。
 * 参考 UE SImage，使用 FSlateBrush 描述绘制方式。
 * 
 * 用法:
 *   SImage::Make(FSlateBrush::Image(tex))  -- 简单图像
 *   SImage::Make(FSlateBrush::Box(tex, FMargin(0.25f)))  -- 九宫格
 *   SImage::Make(FSlateBrush::RoundedBox(8.0f, fill, border, 1.0f))  -- 圆角框
 */
class SImage : public SLeafWidget
{
public:
    // 核心: 使用 FSlateBrush 描述纹理、颜色、边距等
    FSlateBrush Brush;

public:
    // 工厂方法
    static std::shared_ptr<SImage> Make(const FSlateBrush& InBrush)
    {
        std::shared_ptr<SImage> Image = std::make_shared<SImage>();
        Image->Brush = InBrush;
        return Image;
    }

    // 简化工厂: 直接传纹理
    static std::shared_ptr<SImage> Make(void* InTexture, const UIColor& InTint = UIColor(1,1,1,1))
    {
        return Make(FSlateBrush::Image(InTexture, InTint));
    }

    Vector2 ComputeDesiredSize() const override
    {
        return Brush.GetImageSize();
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;

        switch (Brush.DrawAs)
        {
        case ESlateBrushDrawType::Image:
            // 简单图像绘制
            ctx.Renderer->drawTexturedQuad(geom.ToRect(), Brush.Texture, Brush.Tint, Brush.Uv0, Brush.Uv1);
            break;

        case ESlateBrushDrawType::Box:
            // 九宫格绘制 (3x3)
            // TODO: Renderer 需要实现 drawNinePatch
            ctx.Renderer->drawTexturedQuad(geom.ToRect(), Brush.Texture, Brush.Tint, Brush.Uv0, Brush.Uv1);
            break;

        case ESlateBrushDrawType::Border:
            // 边框绘制
            // TODO: Renderer 需要实现 drawBorder
            ctx.Renderer->drawTexturedQuad(geom.ToRect(), Brush.Texture, Brush.Tint, Brush.Uv0, Brush.Uv1);
            break;

        case ESlateBrushDrawType::RoundedBox:
            // 圆角框绘制: 先画填充色，再画边框
            ctx.Renderer->drawQuad(geom.ToRect(), Brush.Tint);
            if (Brush.OutlineSettings.Width > 0.0f && Brush.OutlineSettings.Color.w > 0.0f)
            {
                ctx.Renderer->drawRect(geom.ToRect(), Brush.OutlineSettings.Color, Brush.OutlineSettings.Width);
            }
            break;

        case ESlateBrushDrawType::NoDrawType:
        default:
            // 不绘制
            break;
        }
    }
};
}  // namespace ZSlate
