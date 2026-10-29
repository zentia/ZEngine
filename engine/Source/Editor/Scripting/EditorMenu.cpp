#include "EditorMenu.h"

#include "Editor/Scripting/EditorScriptHooks.h"

#include <string>

namespace
{
    std::string ToStd(const eastl::string& text)
    {
        return std::string(text.c_str(), text.size());
    }
}  // namespace

bool EditorMenu::AddItem(const eastl::string& path,
                         const eastl::string& module,
                         const eastl::string& function_name,
                         int priority)
{
    EditorScriptHooks::Callback callback;
    callback.module = ToStd(module);
    callback.function = ToStd(function_name);
    return EditorScriptHooks::AddMenuItem(ToStd(path), callback, priority);
}

bool EditorMenu::RemoveItem(const eastl::string& path)
{
    return EditorScriptHooks::RemoveMenuItem(ToStd(path));
}

int EditorMenu::GetItemCount()
{
    return static_cast<int>(EditorScriptHooks::GetMenuItems().size());
}

eastl::string EditorMenu::GetItemPathAt(int index)
{
    const auto& items = EditorScriptHooks::GetMenuItems();
    if (index < 0 || static_cast<size_t>(index) >= items.size())
        return {};
    const std::string& path = items[static_cast<size_t>(index)].path;
    return eastl::string(path.c_str(), path.size());
}

bool EditorMenu::InvokeItem(const eastl::string& path)
{
    const std::string wanted = ToStd(path);
    for (const EditorScriptHooks::MenuItem& item : EditorScriptHooks::GetMenuItems())
    {
        if (item.path == wanted)
            return EditorScriptHooks::Invoke(item.callback);
    }
    return false;
}
