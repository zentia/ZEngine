#pragma once

#include "Editor/Platform/Interface/EditorView.h"

#include <string>

namespace ZSlate
{
class SMenu;
}

class Menu : public EditorView
{
public:
    explicit Menu(const char* name, EditorUI* editor_ui);

    // Menus are never hosted as standalone editor views; they only contribute
    // dropdowns to the native ZSlate menu bar through BuildZSlateMenu. OnGUI is
    // a required EditorView override, satisfied here as a no-op.
    void OnGUI() override {}

    // Populate a ZSlate dropdown for this menu (native menu-bar path). Called
    // once when the dropdown opens. Default is empty; menus with content override.
    virtual void BuildZSlateMenu(ZSlate::SMenu& /*menu*/, float /*scale*/) {}
};