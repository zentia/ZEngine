#include "Editor/Scripting/EditorScriptPanelModel.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>

namespace EditorScriptPanelModel
{
    namespace
    {
        std::vector<std::string>& LineStore()
        {
            static std::vector<std::string> lines;
            return lines;
        }

        std::vector<Button>& ButtonStore()
        {
            static std::vector<Button> buttons;
            return buttons;
        }

        uint64_t& Revision()
        {
            static uint64_t revision = 0;
            return revision;
        }
    }  // namespace

    void SetLines(std::vector<std::string> lines)
    {
        LineStore() = std::move(lines);
        ++Revision();
    }

    bool AddButton(const std::string& label, const EditorScriptHooks::Callback& callback)
    {
        if (label.empty() || !callback.IsValid())
        {
            LOG_WARNING(ZScripting, "EditorScriptPanel: ignoring button with empty label or callback");
            return false;
        }
        std::vector<Button>& buttons = ButtonStore();
        auto existing = std::find_if(buttons.begin(), buttons.end(),
                                     [&label](const Button& button) { return button.label == label; });
        if (existing != buttons.end())
        {
            existing->callback = callback;
        }
        else
        {
            buttons.push_back(Button {label, callback});
        }
        ++Revision();
        return true;
    }

    size_t ClearButtons()
    {
        const size_t removed = ButtonStore().size();
        ButtonStore().clear();
        ++Revision();
        return removed;
    }

    void Clear()
    {
        LineStore().clear();
        ButtonStore().clear();
        ++Revision();
    }

    const std::vector<std::string>& GetLines()
    {
        return LineStore();
    }

    const std::vector<Button>& GetButtons()
    {
        return ButtonStore();
    }

    uint64_t GetRevision()
    {
        return Revision();
    }
}  // namespace EditorScriptPanelModel
