#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <memory>
#include <string>

namespace ZSlate
{
class SWidget;
class STextBlock;
}

// P2 vertical-slice demo: a dockable editor panel whose content is built and
// rendered entirely by the ZSlate retained-mode framework (no ImGui widgets).
// Proves the full stack: widget tree -> layout -> paint -> UIRenderer -> screen.
class ZSlateDemoWindow : public EditorWindow
{
public:
    explicit ZSlateDemoWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    // Rebuilds the tree with all metrics multiplied by the DPI scale.
    void BuildUI(float scale);

    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::shared_ptr<ZSlate::STextBlock> m_StatusLabel;
    ZSlate::SlateInputRouter m_Input;
    float m_BuiltScale {-1.0f};

    // Live state, mutated by the ZSlate control callbacks.
    int m_ClickCount {0};
    bool m_Enabled {true};
    float m_Opacity {0.5f};
    std::string m_Name {"entity_0"};
};
