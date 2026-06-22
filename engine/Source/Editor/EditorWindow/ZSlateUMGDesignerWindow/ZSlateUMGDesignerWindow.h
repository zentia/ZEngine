#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <memory>
#include <string>

namespace ZSlate
{
class SWidget;
class SScrollBox;
class SEditableTextBox;
}  // namespace ZSlate

namespace ZUMG
{
class UWidget;
class UPanelWidget;
}  // namespace ZUMG

// Fully native (ImGui-free) UMG designer. Mirrors UMGDesignerWindow but builds
// every region from ZSlate widgets:
//   * a toolbar (New / type-cycle / Add / Delete / asset-URL text box / Save /
//     Load) of SButton + SEditableTextBox,
//   * a hierarchy tree of SButtons (click-to-select, expand toggles, drag-drop
//     reparent via SButton's drag hooks),
//   * a property panel driven generically by each widget's UMGPropertyBag,
//     emitting SDragFloat / SCheckBox / SEditableTextBox / SColorPicker editors,
//   * the live ZSlate preview (UWidget::TakeWidget()) painted into its own region.
// The chrome and the preview are painted in one BatchedUIRenderer pass but routed
// through two independent SlateInputRouters so a property drag never tears down
// the chrome (the chrome only rebuilds on structural / selection / scale change).
// Save / Load round-trip through the .zasset pipeline (UMGAssetIO).
class ZSlateUMGDesignerWindow : public EditorWindow
{
public:
    explicit ZSlateUMGDesignerWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    void BuildChrome(float scale);
    void BuildHierarchyRows(const std::shared_ptr<ZSlate::SScrollBox>& box,
                            const std::shared_ptr<ZUMG::UWidget>& widget,
                            int depth,
                            float scale);
    void BuildPropertyRows(const std::shared_ptr<ZSlate::SScrollBox>& box, float scale);

    void EnsureRoot();
    void AddWidgetToSelection(const std::string& class_name);
    void DeleteSelected();
    void ApplyPendingReparent();
    void RebuildPreviewIfDirty();

    void MarkPreviewDirty() { m_PreviewDirty = true; }
    void MarkUiDirty()
    {
        m_UiDirty = true;
        m_PreviewDirty = true;
    }

    std::shared_ptr<ZUMG::UWidget> FindParent(const std::shared_ptr<ZUMG::UWidget>& node) const;
    std::shared_ptr<ZUMG::UWidget> SelectedShared() const { return m_Selected.lock(); }
    bool IsDescendant(const std::shared_ptr<ZUMG::UWidget>& root, const ZUMG::UWidget* node) const;
    std::shared_ptr<ZUMG::UWidget> ResolveByRaw(const ZUMG::UWidget* raw) const;

    // ---- Model -------------------------------------------------------------
    std::shared_ptr<ZUMG::UWidget> m_Root;
    std::weak_ptr<ZUMG::UWidget> m_Selected;
    std::shared_ptr<ZUMG::UWidget> m_PendingReparentNode;
    std::shared_ptr<ZUMG::UPanelWidget> m_PendingReparentTarget;

    std::string m_AssetUrl {"UMGWidget.zasset"};
    int m_AddTypeIndex {0};
    bool m_PreviewDirty {true};
    bool m_UiDirty {true};

    // ---- Chrome (retained widget tree) -------------------------------------
    std::shared_ptr<ZSlate::SWidget> m_Chrome;
    std::shared_ptr<ZSlate::SEditableTextBox> m_UrlBox;
    float m_BuiltScale {-1.0f};

    // ---- Live preview (separate widget tree) -------------------------------
    std::shared_ptr<ZSlate::SWidget> m_PreviewSlate;

    ZSlate::SlateInputRouter m_ChromeInput;
    ZSlate::SlateInputRouter m_PreviewInput;
};
