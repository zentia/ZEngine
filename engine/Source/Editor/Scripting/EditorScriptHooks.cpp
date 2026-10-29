#include "Editor/Scripting/EditorScriptHooks.h"

#include "Editor/Scripting/EditorScriptingManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"

#include <algorithm>

namespace EditorScriptHooks
{
    namespace
    {
        std::vector<MenuItem>& MenuStore()
        {
            static std::vector<MenuItem> items;
            return items;
        }

        std::vector<InspectorButton>& ButtonStore()
        {
            static std::vector<InspectorButton> buttons;
            return buttons;
        }

        void SortMenu()
        {
            std::vector<MenuItem>& items = MenuStore();
            std::stable_sort(items.begin(), items.end(), [](const MenuItem& lhs, const MenuItem& rhs) {
                if (lhs.priority != rhs.priority)
                    return lhs.priority < rhs.priority;
                return lhs.path < rhs.path;
            });
        }
    }  // namespace

    bool AddMenuItem(const std::string& path, const Callback& callback, int priority)
    {
        if (path.empty() || !callback.IsValid())
        {
            LOG_WARNING(ZScripting, "EditorMenu: ignoring item with empty path or callback");
            return false;
        }

        std::vector<MenuItem>& items = MenuStore();
        // Replace rather than duplicate: OnEditorLoad runs again after a script
        // reload, and two identical menu rows would be worse than a stale one.
        auto existing = std::find_if(items.begin(), items.end(),
                                     [&path](const MenuItem& item) { return item.path == path; });
        if (existing != items.end())
        {
            existing->callback = callback;
            existing->priority = priority;
        }
        else
        {
            items.push_back(MenuItem {path, callback, priority});
        }
        SortMenu();
        return true;
    }

    bool RemoveMenuItem(const std::string& path)
    {
        std::vector<MenuItem>& items = MenuStore();
        auto it = std::find_if(items.begin(), items.end(),
                               [&path](const MenuItem& item) { return item.path == path; });
        if (it == items.end())
            return false;
        items.erase(it);
        return true;
    }

    const std::vector<MenuItem>& GetMenuItems()
    {
        return MenuStore();
    }

    bool AddInspectorButton(const std::string& component_type, const std::string& label, const Callback& callback)
    {
        if (component_type.empty() || label.empty() || !callback.IsValid())
        {
            LOG_WARNING(ZScripting, "EditorInspector: ignoring button with empty type / label / callback");
            return false;
        }

        std::vector<InspectorButton>& buttons = ButtonStore();
        // Same reload argument as AddMenuItem: (type, label) is the identity.
        auto existing = std::find_if(buttons.begin(), buttons.end(), [&](const InspectorButton& button) {
            return button.component_type == component_type && button.label == label;
        });
        if (existing != buttons.end())
        {
            existing->callback = callback;
            return true;
        }
        buttons.push_back(InspectorButton {component_type, label, callback});
        return true;
    }

    size_t ClearInspectorButtons(const std::string& component_type)
    {
        std::vector<InspectorButton>& buttons = ButtonStore();
        const size_t before = buttons.size();
        buttons.erase(std::remove_if(buttons.begin(), buttons.end(),
                                     [&component_type](const InspectorButton& button) {
                                         return button.component_type == component_type;
                                     }),
                      buttons.end());
        return before - buttons.size();
    }

    std::vector<const InspectorButton*> GetInspectorButtons(const std::string& component_type)
    {
        std::vector<const InspectorButton*> out;
        if (component_type.empty())
            return out;
        for (const InspectorButton& button : ButtonStore())
        {
            if (button.component_type == component_type)
                out.push_back(&button);
        }
        return out;
    }

    size_t GetInspectorButtonCount()
    {
        return ButtonStore().size();
    }

    bool Invoke(const Callback& callback)
    {
        if (!callback.IsValid())
            return false;

        auto manager = GET_SYSTEM(EditorScriptingManager);
        if (!manager || !manager->IsRunning())
        {
            LOG_WARNING(ZScripting, "EditorScriptHooks: editor VM is not running; '{}.{}' not called",
                        callback.module, callback.function);
            return false;
        }
        ScriptEnv* env = manager->GetEnv();
        if (env == nullptr)
            return false;

        // InvokeExportedFunction already swallows and logs a JS throw, so a
        // script that faults inside a menu handler cannot unwind through the
        // UI callback that invoked it.
        return env->InvokeExportedFunction(callback.module, callback.function.c_str());
    }

    void Clear()
    {
        MenuStore().clear();
        ButtonStore().clear();
    }
}  // namespace EditorScriptHooks
