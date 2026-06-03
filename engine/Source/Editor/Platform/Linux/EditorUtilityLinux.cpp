#include "Editor/Platform/Interface/EditorUtility.h"

#include <cstdlib>
#include <filesystem>

namespace
{
    std::filesystem::path normalizeExistingPath(const eastl::string& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::filesystem::path fs_path(path.c_str());
        fs_path = fs_path.lexically_normal();
        if (fs_path.is_relative())
        {
            fs_path = std::filesystem::absolute(fs_path).lexically_normal();
        }
        return fs_path;
    }
}  // namespace

void EditorUtility::RevealInFinder(const eastl::string path)
{
    const std::filesystem::path fs_path = normalizeExistingPath(path);
    if (fs_path.empty())
    {
        return;
    }

    std::error_code ec;
    std::filesystem::path target = fs_path;
    if (std::filesystem::is_regular_file(target, ec))
    {
        target = target.parent_path();
    }
    if (target.empty())
    {
        return;
    }

    const std::string quoted = "\"" + target.string() + "\"";
    std::string command = "xdg-open " + quoted;
    std::system(command.c_str());
}

void EditorUtility::OpenInExternalEditor(const eastl::string path)
{
    const std::filesystem::path fs_path = normalizeExistingPath(path);
    if (fs_path.empty())
    {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(fs_path, ec))
    {
        return;
    }

    const std::string quoted = "\"" + fs_path.string() + "\"";

    if (const char* override_path = std::getenv("ZENGINE_EXTERNAL_EDITOR"))
    {
        if (override_path && *override_path)
        {
            std::string cmd = std::string("\"") + override_path + "\" " + quoted;
            if (std::system(cmd.c_str()) == 0)
            {
                return;
            }
        }
    }

    {
        std::string cmd = "code " + quoted;
        if (std::system(cmd.c_str()) == 0)
        {
            return;
        }
    }

    std::string fallback = std::string("xdg-open ") + quoted;
    std::system(fallback.c_str());
}

bool EditorUtility::OpenFileDialog(const std::string&, const std::string&, const std::string&, std::string& out_path)
{
    out_path.clear();
    return false;
}

bool EditorUtility::SaveFileDialog(const std::string&,
                                   const std::string&,
                                   const std::string&,
                                   std::string& out_path,
                                   const char*,
                                   const char*)
{
    out_path.clear();
    return false;
}

SceneSavePromptResult EditorUtility::PromptUnsavedScene(const std::string&)
{
    return SceneSavePromptResult::Cancel;
}
