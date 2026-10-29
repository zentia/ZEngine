#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "ZSlate/Application/SlateInput.h"

#include <cstdint>
#include <memory>

namespace ZSlate
{
class SWidget;
class SVerticalBox;
}  // namespace ZSlate

// The "Script Panel" dock panel: renders whatever an editor TypeScript pushed
// through EditorScriptPanel (P5).
//
// Deliberately dumb -- it owns no state of its own. Content lives in
// EditorScriptPanelModel and this only rebuilds its widget tree when that
// model's revision changes (or the DPI scale does), so an untouched panel
// costs one integer compare per frame.
class ZSlateScriptPanelWindow : public EditorWindow
{
public:
    explicit ZSlateScriptPanelWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    void RebuildContent(float scale);

    std::shared_ptr<ZSlate::SVerticalBox> m_Root;
    float m_BuiltScale {-1.0f};
    /// Model revision the current widget tree was built from. uint64 max forces
    /// a first build (revision 0 is a legitimate "nothing pushed yet" state and
    /// must still produce the placeholder text).
    uint64_t m_BuiltRevision {UINT64_MAX};

    ZSlate::SlateInputRouter m_Input;
};
