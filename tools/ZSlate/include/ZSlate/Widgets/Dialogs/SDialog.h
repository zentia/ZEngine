#pragma once

// ============================================================================
// ZSlate/Widgets/Dialogs/SDialog.h
//
// Title bar + body + button bar. Show it with SDialog::Show(), which installs
// it as the modal widget of SlateApplication (the framework dims the rest of
// the window and routes all input to the dialog until Close() is called).
// ============================================================================

#include "../Layout/SBoxPanel.h"
#include "../Layout/SSpacer.h"
#include "../Text/STextBlock.h"
#include "../Input/SButton.h"
#include "../../Core/ZSlateTypes.h"

#include <memory>
#include <string>

namespace ZSlate
{

class SDialog : public SVerticalBox
{
public:
    std::string Title;
    Vector2     Padding          { 14.0f, 14.0f };
    UIColor     BackgroundColor   { 0.13f, 0.14f, 0.17f, 1.0f };
    UIColor     TitleBarColor     { 0.18f, 0.20f, 0.26f, 1.0f };
    UIColor     BorderColor       { 0.38f, 0.41f, 0.50f, 1.0f };
    UIColor     TitleColor        = Colors::White;
    float       FontSize          = 16.0f;
    float       MinWidth          = 420.0f;

    SDialog();

    void SetTitle(const std::string& title);
    void SetBody(const std::shared_ptr<SWidget>& body);
    void SetButtons(const std::shared_ptr<SWidget>& buttons);

    // Show / hide as the application's modal widget.
    void Show();
    void Close();

    Vector2 ComputeDesiredSize() const override;

    void OnArrangeChildren(const FGeometry& allottedGeometry,
                           std::vector<FArrangedChild>& outChildren) const override;

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;

private:
    std::shared_ptr<STextBlock> m_TitleLabel;
    std::shared_ptr<SWidget>    m_Body;
    std::shared_ptr<SWidget>    m_Buttons;
    bool                        m_Closed = false;
};

} // namespace ZSlate
