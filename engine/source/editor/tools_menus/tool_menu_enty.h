#pragma once

struct FToolMenuEntry
{
    static FToolMenuEntry InitSubMenu(const std::string InName);

public:
    std::string Name;
};