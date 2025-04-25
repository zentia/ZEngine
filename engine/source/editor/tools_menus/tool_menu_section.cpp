#include "tool_menu_section.h"

FToolMenuEntry& FToolMenuSection::AddEntry(const FToolMenuEntry& Args)
{
    int32_t BlockIndex = IndexOfBlock(Args.Name);
    if (BlockIndex != -1)
    {
        Blocks[BlockIndex] = Args;
        return Blocks[BlockIndex];
    }
    FToolMenuEntry& Result = Blocks.emplace_back();
    return Result;
}

FToolMenuEntry& FToolMenuSection::AddSubMenu(const std::string InName)
{
    return AddEntry(FToolMenuEntry::InitSubMenu(InName));
}

int32_t FToolMenuSection::IndexOfBlock(const std::string InName)
{
    for (int32_t i = 0; i < Blocks.size(); ++i)
    {
        if (Blocks[i].Name == InName)
        {
            return i;
        }
    }
    return -1;
}
