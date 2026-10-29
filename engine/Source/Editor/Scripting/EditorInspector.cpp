#include "EditorInspector.h"

#include "Editor/Scripting/EditorScriptHooks.h"

#include <string>

namespace
{
    std::string ToStd(const eastl::string& text)
    {
        return std::string(text.c_str(), text.size());
    }
}  // namespace

bool EditorInspector::AddButton(const eastl::string& component_type,
                                const eastl::string& label,
                                const eastl::string& module,
                                const eastl::string& function_name)
{
    EditorScriptHooks::Callback callback;
    callback.module = ToStd(module);
    callback.function = ToStd(function_name);
    return EditorScriptHooks::AddInspectorButton(ToStd(component_type), ToStd(label), callback);
}

int EditorInspector::ClearButtons(const eastl::string& component_type)
{
    return static_cast<int>(EditorScriptHooks::ClearInspectorButtons(ToStd(component_type)));
}

int EditorInspector::GetButtonCount(const eastl::string& component_type)
{
    return static_cast<int>(EditorScriptHooks::GetInspectorButtons(ToStd(component_type)).size());
}

int EditorInspector::GetTotalButtonCount()
{
    return static_cast<int>(EditorScriptHooks::GetInspectorButtonCount());
}

bool EditorInspector::InvokeButton(const eastl::string& component_type, const eastl::string& label)
{
    const std::string wanted_label = ToStd(label);
    for (const EditorScriptHooks::InspectorButton* button :
         EditorScriptHooks::GetInspectorButtons(ToStd(component_type)))
    {
        if (button->label == wanted_label)
            return EditorScriptHooks::Invoke(button->callback);
    }
    return false;
}
