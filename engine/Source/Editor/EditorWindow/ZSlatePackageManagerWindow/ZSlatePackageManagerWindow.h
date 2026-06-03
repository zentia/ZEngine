#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Slate/Application/SlateInput.h"

#include <memory>
#include <string>

namespace ZSlate
{
class SWidget;
class SScrollBox;
class SEditableTextBox;
class STextBlock;
}  // namespace ZSlate

class PackageManager;

// A ZSlate-rendered, dockable Package Manager (Unity Package Manager-style panel
// at dock title "Package Manager"). Lists resolved ZPM packages for the open
// project with a filter box, a sort toggle, and toolbar actions (re-resolve /
// open manifest / open lock file).
//
// The chrome (toolbar + manifest paths) is built ONCE and kept alive so the
// filter text box keeps focus; only the package rows inside the SScrollBox are
// rebuilt, and only when the filter / sort / resolved-package set changes.
class ZSlatePackageManagerWindow : public EditorWindow
{
public:
    explicit ZSlatePackageManagerWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    void BuildLayout(float scale);
    void RebuildTable(PackageManager& pm);
    size_t ComputeResolveSignature(PackageManager& pm) const;

    // Filter / sort state.
    std::string m_Filter;
    bool m_SortByName {true};

    // Rebuild bookkeeping.
    bool m_TableDirty {true};
    size_t m_LastSignature {0};
    float m_BuiltScale {-1.0f};

    // Persistent ZSlate tree (only the package rows are rebuilt).
    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::shared_ptr<ZSlate::SScrollBox> m_PackageList;
    std::shared_ptr<ZSlate::SEditableTextBox> m_FilterBox;
    std::shared_ptr<ZSlate::STextBlock> m_ManifestText;
    std::shared_ptr<ZSlate::STextBlock> m_LockText;
    std::shared_ptr<ZSlate::STextBlock> m_ErrorText;

    ZSlate::SlateInputRouter m_Input;
};
