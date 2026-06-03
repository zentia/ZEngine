#pragma once

#include "Editor/Menu/Menu.h"

class WindowMenu : public Menu
{
public:
    explicit WindowMenu(EditorUI* editor_ui);
    virtual void BuildZSlateMenu(ZSlate::SMenu& menu, float scale) override;
};