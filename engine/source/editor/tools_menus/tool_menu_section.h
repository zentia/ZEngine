#pragma once
#include <functional>
#include <string>
#include <vector>

#include "tool_menu_enty.h"
#include "editor/level_editor/level_editor_create_actor_menu.h"

class UToolMenu;

struct FToolMenuSection
{
    FToolMenuEntry& AddEntry(const FToolMenuEntry& Args);
    FToolMenuEntry& AddSubMenu(const std::string InName/*, std::function<UToolMenu*, EActorCreateMode::Type> InMakeMenu*/);

private:
    int32_t IndexOfBlock(const std::string InName);

public:
    std::vector<FToolMenuEntry> Blocks;
};