#pragma once

#include "ZSlate/Widgets/Panels/SImage.h"
#include "Runtime/UMG/Core/UWidget.h"

namespace ZUMG
{
// UE UMG UImage analogue -> ZSlate::SImage. Texture is a backend-opaque handle
// (null renders a solid Tint quad).
class UImage : public UWidget
{
public:
    void* Texture {nullptr};
    UIColor Tint {1.0f, 1.0f, 1.0f, 1.0f};
    Vector2 DesiredSize {16.0f, 16.0f};
    Vector2 Uv0 {0.0f, 0.0f};
    Vector2 Uv1 {1.0f, 1.0f};

    void SetTexture(void* texture)
    {
        Texture = texture;
        if (auto* w = GetSlateAs<ZSlate::SImage>())
            w->Brush.Texture = texture;
    }
    void SetTint(const UIColor& tint)
    {
        Tint = tint;
        if (auto* w = GetSlateAs<ZSlate::SImage>())
            w->Brush.Tint = ZSlate::UIColor(tint.x, tint.y, tint.z, tint.w);
    }

    const char* GetWidgetClassName() const override { return "Image"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        // NOTE: Texture is a backend-opaque pointer; it is not serialised in P2.
        // A future texture-by-GUID field lands when UMG references assets.
        UWidget::SerializeProperties(bag);
        bag.SetColor("tint", Tint);
        bag.SetVec2("desired_size", DesiredSize);
        bag.SetVec2("uv0", Uv0);
        bag.SetVec2("uv1", Uv1);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Tint = bag.GetColor("tint", Tint);
        DesiredSize = bag.GetVec2("desired_size", DesiredSize);
        Uv0 = bag.GetVec2("uv0", Uv0);
        Uv1 = bag.GetVec2("uv1", Uv1);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto img = std::make_shared<ZSlate::SImage>();
        img->Brush.Texture = Texture;
        // Convert ::UIColor to ZSlate::UIColor (::Vector4 uses x,y,z,w)
        img->Brush.Tint = ZSlate::UIColor(Tint.x, Tint.y, Tint.z, Tint.w);
        // Convert ::Vector2 to ZSlate::Vector2
        img->Brush.ImageSize = ZSlate::Vector2(DesiredSize.x, DesiredSize.y);
        img->Brush.Uv0 = ZSlate::Vector2(Uv0.x, Uv0.y);
        img->Brush.Uv1 = ZSlate::Vector2(Uv1.x, Uv1.y);
        img->Visibility = m_Visibility;
        return img;
    }
};
}  // namespace ZUMG
