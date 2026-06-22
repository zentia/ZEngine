#include "WindowMenu.h"

#include "Editor/EditorLayout/DefaultLayout/DefaultLayout.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorWindow/EditorWindow.h"
#include "Editor/EditorWindow/ZSlatePackageManagerWindow/ZSlatePackageManagerWindow.h"
#include "Editor/FloatingPanel/FloatingPanelManager.h"
#include "Runtime/Function/Render/Pipeline/RenderPipelineSettings.h"
#include "ZSlate/Widgets/SMenu.h"

WindowMenu::WindowMenu(EditorUI* editor_ui)
    : Menu("Window", editor_ui) {}

void WindowMenu::BuildZSlateMenu(ZSlate::SMenu& menu, float scale)
{
    DefaultLayout* layout_manager = m_EditorUi->getLayoutManager();
    if (layout_manager != nullptr)
    {
        std::shared_ptr<ZSlate::SMenu> layouts = menu.AddSubMenu("Layouts", scale);

        for (const std::string& layout_name : layout_manager->GetBuiltinLayoutNames())
        {
            const bool selected = layout_manager->IsCurrentLayout(layout_name);
            layouts->AddCheckItem(layout_name, selected,
                                  [layout_manager, layout_name]() { layout_manager->QueueBuiltinLayout(layout_name); },
                                  scale);
        }

        const auto user_layouts = layout_manager->GetUserLayoutNames();
        if (!user_layouts.empty())
        {
            layouts->AddSeparator(scale);
            for (const std::string& layout_name : user_layouts)
            {
                const bool selected = layout_manager->IsCurrentLayout(layout_name);
                layouts->AddCheckItem(layout_name, selected,
                                      [layout_manager, layout_name]() { layout_manager->QueueUserLayout(layout_name); },
                                      scale);
            }
        }

        layouts->AddSeparator(scale);
        layouts->AddItem("Save Layout...", [layout_manager]() { layout_manager->OpenSaveLayoutDialog(); }, scale);
        layouts->AddItem("Save Layout to File...",
                         [layout_manager]() { layout_manager->SaveCurrentLayoutToFileDialog(); }, scale);
        layouts->AddItem("Load Layout from File...",
                         [layout_manager]() { layout_manager->LoadLayoutFromFileDialog(); }, scale);

        std::shared_ptr<ZSlate::SMenu> del = layouts->AddSubMenu("Delete Layout", scale);
        if (user_layouts.empty())
        {
            del->AddItem("No Saved Layouts", nullptr, scale, /*disabled=*/true);
        }
        else
        {
            for (const std::string& layout_name : user_layouts)
            {
                del->AddItem(layout_name, [layout_manager, layout_name]() { layout_manager->DeleteLayout(layout_name); },
                             scale);
            }
        }

        layouts->AddItem("Reset All Layouts", [layout_manager]() { layout_manager->ResetAllLayouts(); }, scale);

        menu.AddSeparator(scale);

        // Editor tear-off: detach a docked panel into its own OS window, or re-dock
        // one that is already floating. (Drag-tab tear-off lands in a later phase;
        // this menu is the explicit entry point.)
        FloatingPanelManager& floating = FloatingPanelManager::Get();
        std::shared_ptr<ZSlate::SMenu> float_menu = menu.AddSubMenu("Float Panel", scale);
        bool any_dockable = false;
        for (EditorWindow* window : m_EditorUi->GetEditorWindows())
        {
            if (window == nullptr || window->m_Title == nullptr || !window->m_Open)
                continue;
            if (floating.IsFloating(window->m_Title))
                continue;
            const std::string title = window->m_Title;
            float_menu->AddItem(title, [title]() { FloatingPanelManager::Get().RequestFloat(title); }, scale);
            any_dockable = true;
        }
        if (!any_dockable)
            float_menu->AddItem("No Dockable Panels", nullptr, scale, /*disabled=*/true);

        if (floating.HasFloatingPanels())
        {
            std::shared_ptr<ZSlate::SMenu> dock_menu = menu.AddSubMenu("Dock Floating Panel", scale);
            for (EditorWindow* window : m_EditorUi->GetEditorWindows())
            {
                if (window == nullptr || window->m_Title == nullptr || !floating.IsFloating(window->m_Title))
                    continue;
                const std::string title = window->m_Title;
                dock_menu->AddItem(title, [title]() { FloatingPanelManager::Get().RequestDock(title); }, scale);
            }
        }

        menu.AddSeparator(scale);
    }

    // Render path selector (Desktop deferred vs Mobile forward vs platform Auto).
    // Picking an entry sets the configured path; the RenderSystem applies the
    // teardown/rebuild at the next safe frame boundary.
    {
        using RenderPipelineSettings::RenderPath;
        const RenderPath configured = RenderPipelineSettings::GetConfiguredPath();
        std::shared_ptr<ZSlate::SMenu> render_path = menu.AddSubMenu("Render Path", scale);

        const std::string auto_label =
            std::string("Auto (") + RenderPipelineSettings::ToString(RenderPipelineSettings::ResolveAuto()) + ")";
        render_path->AddCheckItem(
            auto_label, configured == RenderPath::Auto,
            []() { RenderPipelineSettings::SetConfiguredPath(RenderPath::Auto); }, scale);
        render_path->AddCheckItem(
            "Desktop (deferred+forward)", configured == RenderPath::Desktop,
            []() { RenderPipelineSettings::SetConfiguredPath(RenderPath::Desktop); }, scale);
        render_path->AddCheckItem(
            "Mobile (forward)", configured == RenderPath::Mobile,
            []() { RenderPipelineSettings::SetConfiguredPath(RenderPath::Mobile); }, scale);

        menu.AddSeparator(scale);
    }

    if (ZSlatePackageManagerWindow* pkg = m_EditorUi->GetWindow<ZSlatePackageManagerWindow>())
    {
        menu.AddCheckItem(pkg->m_Title, pkg->m_Open, [pkg]() { pkg->m_Open = !pkg->m_Open; }, scale);
        menu.AddSeparator(scale);
    }

    m_EditorUi->BuildEditorWindowZSlateMenu(menu, /*exclude_package_manager=*/true, scale);
}
