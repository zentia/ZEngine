#include "EditorScriptPanel.h"

#include "Editor/Scripting/EditorScriptHooks.h"
#include "Editor/Scripting/EditorScriptPanelModel.h"

#include <string>

namespace
{
    std::string ToStd(const eastl::string& text)
    {
        return std::string(text.c_str(), text.size());
    }

    eastl::string ToEastl(const std::string& text)
    {
        return eastl::string(text.c_str(), text.size());
    }
}  // namespace

void EditorScriptPanel::SetLines(const std::vector<eastl::string>& lines)
{
    std::vector<std::string> converted;
    converted.reserve(lines.size());
    for (const eastl::string& line : lines)
    {
        converted.push_back(ToStd(line));
    }
    EditorScriptPanelModel::SetLines(std::move(converted));
}

bool EditorScriptPanel::AddButton(const eastl::string& label,
                                  const eastl::string& module,
                                  const eastl::string& function_name)
{
    EditorScriptHooks::Callback callback;
    callback.module = ToStd(module);
    callback.function = ToStd(function_name);
    return EditorScriptPanelModel::AddButton(ToStd(label), callback);
}

int EditorScriptPanel::ClearButtons()
{
    return static_cast<int>(EditorScriptPanelModel::ClearButtons());
}

void EditorScriptPanel::Clear()
{
    EditorScriptPanelModel::Clear();
}

std::vector<eastl::string> EditorScriptPanel::GetLines()
{
    const std::vector<std::string>& lines = EditorScriptPanelModel::GetLines();
    std::vector<eastl::string> out;
    out.reserve(lines.size());
    for (const std::string& line : lines)
    {
        out.push_back(ToEastl(line));
    }
    return out;
}

int EditorScriptPanel::GetButtonCount()
{
    return static_cast<int>(EditorScriptPanelModel::GetButtons().size());
}

bool EditorScriptPanel::InvokeButton(const eastl::string& label)
{
    const std::string wanted = ToStd(label);
    for (const EditorScriptPanelModel::Button& button : EditorScriptPanelModel::GetButtons())
    {
        if (button.label == wanted)
            return EditorScriptHooks::Invoke(button.callback);
    }
    return false;
}
