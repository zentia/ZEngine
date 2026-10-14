#include "ZSlate/Widgets/Dialogs/SDialog.h"
#include "ZSlate/Application/SlateApplication.h"

#include <algorithm>

namespace ZSlate
{

SDialog::SDialog()
{
    m_TitleLabel = std::make_shared<STextBlock>();
    m_TitleLabel->FontSize = FontSize;
    m_TitleLabel->Color    = TitleColor;

    auto titleBar = std::make_shared<SHorizontalBox>();
    titleBar->AddSlot(m_TitleLabel);
    titleBar->AddSlot(std::make_shared<SSpacer>(Vector2(24.0f, 1.0f)));

    auto closeLabel = std::make_shared<STextBlock>();
    closeLabel->Text     = "X";
    closeLabel->FontSize = FontSize;
    closeLabel->Color    = UIColor(0.75f, 0.78f, 0.85f, 1.0f);
    auto closeButton = std::make_shared<SButton>();
    closeButton->Padding = Vector2(10.0f, 3.0f);
    closeButton->SetContent(closeLabel);
    closeButton->OnClicked = [this]() { Close(); };
    titleBar->AddSlot(closeButton);

    AddSlot(titleBar);
    AddSlot(std::make_shared<SSpacer>(Vector2(0.0f, 10.0f)));
}

void SDialog::SetTitle(const std::string& title)
{
    Title = title;
    m_TitleLabel->Text = title;
}

void SDialog::SetBody(const std::shared_ptr<SWidget>& body)
{
    if (!body || m_Body) return;
    m_Body = body;
    AddSlot(body);
}

void SDialog::SetButtons(const std::shared_ptr<SWidget>& buttons)
{
    if (!buttons || m_Buttons) return;
    m_Buttons = buttons;
    AddSlot(std::make_shared<SSpacer>(Vector2(0.0f, 10.0f)));
    AddSlot(buttons);
}

void SDialog::Show()
{
    m_Closed = false;
    if (auto self = std::static_pointer_cast<SWidget>(shared_from_this()))
    {
        SlateApplication::Get().ShowModal(self);
    }
}

void SDialog::Close()
{
    if (m_Closed) return;
    m_Closed = true;
    SlateApplication::Get().CloseModal();
}

Vector2 SDialog::ComputeDesiredSize() const
{
    const Vector2 content = SBoxPanel::ComputeDesiredSize();
    return Vector2(std::max(MinWidth, content.x + Padding.x * 2.0f),
                   content.y + Padding.y * 2.0f);
}

void SDialog::OnArrangeChildren(const FGeometry& allottedGeometry,
                                std::vector<FArrangedChild>& outChildren) const
{
    const Vector2 innerSize(std::max(0.0f, allottedGeometry.LocalSize.x - Padding.x * 2.0f),
                            std::max(0.0f, allottedGeometry.LocalSize.y - Padding.y * 2.0f));
    const FGeometry inner = allottedGeometry.MakeChild(Padding, innerSize);
    SBoxPanel::OnArrangeChildren(inner, outChildren);
}

void SDialog::OnPaint(const FPaintContext& context, const FGeometry& geometry) const
{
    if (!context.Renderer) return;

    const UIRect r = geometry.ToRect();

    // Panel background + outline.
    context.Renderer->DrawQuad(r, BackgroundColor);
    context.Renderer->DrawRect(r, BorderColor, 1.0f);

    // Title bar strip.
    const float titleBarHeight = FontSize * 1.4f + Padding.y;
    context.Renderer->DrawQuad(UIRect(r.x + 1.0f, r.y + 1.0f,
                                      std::max(0.0f, r.w - 2.0f), titleBarHeight),
                               TitleBarColor);
}

} // namespace ZSlate
