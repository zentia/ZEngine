#pragma once

#include "Editor/Menu/Menu.h"

class FileMenu : public Menu
{
public:
    explicit FileMenu(EditorUI* editor_ui);
    virtual void BuildZSlateMenu(ZSlate::SMenu& menu, float scale) override;
};