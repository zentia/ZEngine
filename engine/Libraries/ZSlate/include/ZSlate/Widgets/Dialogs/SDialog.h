#pragma once

// SDialog: title bar + body + button bar, shown through the SlateApplication
// modal layer (UE analogue: SWindow used as a modal via AddModalWindow).
//
//   auto dialog = std::make_shared<SDialog>();
//   dialog->SetTitle("Rename");
//   dialog->SetBody(body);
//   dialog->SetButtons(buttonRow);
//   dialog->Show();          // installs it as SlateApplication's modal
//   ...
//   dialog->Close();         // removes it again (also bound to the title "X")

#include "ZSlate/Widgets/SCompoundWidget.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"

#include <functional>
#include <memory>
#include <string>

namespace ZSlate
{
class STextBlock;

class SDialog : public SCompoundWidget
{
public:
    UIColor BackgroundColor {0.13f, 0.14f, 0.17f, 1.0f};
    UIColor TitleBarColor   {0.18f, 0.20f, 0.26f, 1.0f};
    UIColor BorderColor     {0.38f, 0.41f, 0.50f, 1.0f};
    UIColor TitleColor      {1.0f, 1.0f, 1.0f, 1.0f};
    float   TitleFontSize   {16.0f};
    float   MinWidth        {420.0f};

    // Fired once when the dialog is closed (title-bar X or Close()).
    std::function<void()> OnClosed;

    SDialog();

    void SetTitle(const std::string& title);
    const std::string& GetTitle() const { return m_Title; }

    // Body and button bar may each be set once; later calls replace them.
    void SetBody(std::shared_ptr<SWidget> body);
    void SetButtons(std::shared_ptr<SWidget> buttons);

    void Show();
    void Close();
    bool IsOpen() const { return m_Open; }

    Vector2 ComputeDesiredSize() const override;
    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override;

private:
    void Rebuild();

    std::string                    m_Title;
    std::shared_ptr<STextBlock>    m_TitleLabel;
    std::shared_ptr<SHorizontalBox> m_TitleBar;
    std::shared_ptr<SWidget>       m_Body;
    std::shared_ptr<SWidget>       m_Buttons;
    std::shared_ptr<SVerticalBox>  m_Layout;
    bool                           m_Open {false};
};

}  // namespace ZSlate
