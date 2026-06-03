#pragma once
#include "Editor/LevelEditor/LevelEditorCreateActorMenu.h"
#include "ToolMenuEntry.h"

#include <functional>
#include <string>
#include <vector>

class UToolMenu;

struct FToolMenuSection
{
    FToolMenuEntry& AddEntry(const FToolMenuEntry& Args);
    FToolMenuEntry& AddSubMenu(const std::string InName /*, std::function<UToolMenu*, EActorCreateMode::Type> InMakeMenu*/);

private:
    int32_t IndexOfBlock(const std::string InName);

public:
    std::vector<FToolMenuEntry> Blocks;
};