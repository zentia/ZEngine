#include "ZSlate/Widgets/Dialogs/SDialog.h"

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <algorithm>

namespace ZSlate
{

SDialog::SDialog()
{
    Padding = FMargin(14.0f, 12.0f);

    m_TitleLabel = std::make_shared<STextBlock>();
    m_TitleLabel->FontSize = TitleFontSize;
    m_TitleLabel->Color    = TitleColor;

    auto closeLabel = std::make_shared<STextBlock>();
    closeLabel->Text      = "X";
    closeLabel->FontSize  = TitleFontSize;
    closeLabel->Color     = UIColor(0.75f, 0.78f, 0.85f, 1.0f);
    closeLabel->Alignment = TextAnchor::MiddleCenter;

    auto closeButton = std::make_shared<SButton>();
    closeButton->Padding     = FMargin(10.0f, 3.0f);
    closeButton->NormalColor = UIColor(0.0f, 0.0f, 0.0f, 0.0f);
    closeButton->HoverColor  = UIColor(0.70f, 0.20f, 0.20f, 1.0f);
    closeButton->SetContent(closeLabel);
    closeButton->OnClicked = [this]() { Close(); };

    m_TitleBar = std::make_shared<SHorizontalBox>();
    m_TitleBar->AddSlot(m_TitleLabel).Fill().SetVAlign(EVerticalAlignment::Center);
    m_TitleBar->AddSlot(std::make_shared<SSpacer>(Vector2(24.0f, 1.0f)));
    m_TitleBar->AddSlot(closeButton);

    Rebuild();
}

void SDialog::SetTitle(const std::string& title)
{
    m_Title = title;
    m_TitleLabel->Text = title;
}

void SDialog::SetBody(std::shared_ptr<SWidget> body)
{
    m_Body = std::move(body);
    Rebuild();
}

void SDialog::SetButtons(std::shared_ptr<SWidget> buttons)
{
    m_Buttons = std::move(buttons);
    Rebuild();
}

void SDialog::Rebuild()
{
    m_Layout = std::make_shared<SVerticalBox>();
    m_Layout->AddSlot(m_TitleBar);
    m_Layout->AddSlot(std::make_shared<SSpacer>(Vector2(0.0f, 10.0f)));
    if (m_Body)
        m_Layout->AddSlot(m_Body).Fill();
    if (m_Buttons)
    {
        m_Layout->AddSlot(std::make_shared<SSpacer>(Vector2(0.0f, 10.0f)));
        m_Layout->AddSlot(m_Buttons);
    }
    SetContent(m_Layout);
}

void SDialog::Show()
{
    m_Open = true;
    SlateApplication::Get().ShowModal(shared_from_this());
}

void SDialog::Close()
{
    if (!m_Open)
        return;
    m_Open = false;

    SlateApplication& app = SlateApplication::Get();
    if (app.GetModal().get() == this)
        app.CloseModal();

    if (OnClosed)
        OnClosed();
}

Vector2 SDialog::ComputeDesiredSize() const
{
    const Vector2 content = SCompoundWidget::ComputeDesiredSize();
    return Vector2(std::max(MinWidth, content.x), content.y);
}

void SDialog::OnPaint(const FPaintContext& ctx, const FGeometry& geom) const
{
    if (!ctx.Renderer)
        return;

    const UIRect r = geom.ToRect();
    ctx.Renderer->DrawQuad(r, BackgroundColor);

    // Title strip spans the top padding plus the title row.
    const float titleHeight = Padding.Top + (m_TitleBar ? m_TitleBar->GetDesiredSize().y : 0.0f) + 4.0f;
    ctx.Renderer->DrawQuad(UIRect(r.x + 1.0f, r.y + 1.0f, std::max(0.0f, r.w - 2.0f), titleHeight), TitleBarColor);

    ctx.Renderer->DrawRect(r, BorderColor, 1.0f);
}

}  // namespace ZSlate
