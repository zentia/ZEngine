#pragma once

class ULevelEditorContextMenuContext;
struct FToolMenuSection;
class UToolMenu;

namespace EActorCreateMode
{
    enum Type
    {
        Add,
        Replace
    };
}  // namespace EActorCreateMode

namespace LevelEditorCreateActorMenu
{
    void FillAddReplaceContextMenuSections(FToolMenuSection& Section,
                                           ULevelEditorContextMenuContext* LevelEditorMenuContext);
    void FillAndReplaceActorMenu(UToolMenu* Menu, EActorCreateMode::Type CreateMode);
}  // namespace LevelEditorCreateActorMenu