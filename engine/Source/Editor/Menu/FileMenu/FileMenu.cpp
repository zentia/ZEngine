#include "FileMenu.h"

#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/RenderDebugConfig.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Slate/Widgets/SMenu.h"

#include <cstdlib>

FileMenu::FileMenu(EditorUI* editor_ui)
    : Menu("File", editor_ui) {}

void FileMenu::BuildZSlateMenu(ZSlate::SMenu& menu, float scale)
{
    menu.AddItem("Reload Current Level", []() {
        GET_SYSTEM(WorldManager)->ReloadCurrentLevel();
        GET_SYSTEM(RenderSystem)->ClearForLevelReloading();
        GET_SYSTEM(EditorSceneManager)->OnGObjectSelected(k_invalid_gobject_id);
    }, scale);
    menu.AddItem("Save Current Level", []() { GET_SYSTEM(WorldManager)->SaveCurrentLevel(); }, scale);

    {
        std::shared_ptr<ZSlate::SMenu> debug = menu.AddSubMenu("Debug", scale);

        std::shared_ptr<ZSlate::SMenu> anim = debug->AddSubMenu("Animation", scale);
        auto cfg = GET_SYSTEM(RenderDebugConfig);
        anim->AddItem(cfg->animation.show_skeleton ? "off skeleton" : "show skeleton", [cfg]() {
            cfg->animation.show_skeleton = !cfg->animation.show_skeleton;
        }, scale);
        anim->AddItem(cfg->animation.show_bone_name ? "off bone name" : "show bone name", [cfg]() {
            cfg->animation.show_bone_name = !cfg->animation.show_bone_name;
        }, scale);

        std::shared_ptr<ZSlate::SMenu> cam = debug->AddSubMenu("Camera", scale);
        cam->AddItem(cfg->camera.show_runtime_info ? "off runtime info" : "show runtime info", [cfg]() {
            cfg->camera.show_runtime_info = !cfg->camera.show_runtime_info;
        }, scale);

        std::shared_ptr<ZSlate::SMenu> go = debug->AddSubMenu("Game Object", scale);
        go->AddItem(cfg->game_object.show_bounding_box ? "off bounding box" : "show bounding box", [cfg]() {
            cfg->game_object.show_bounding_box = !cfg->game_object.show_bounding_box;
        }, scale);
    }

    menu.AddItem("Exit", []() { std::exit(0); }, scale);
}