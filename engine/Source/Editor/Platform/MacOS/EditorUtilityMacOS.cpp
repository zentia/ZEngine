#include "Editor/Platform/Interface/EditorUtility.h"

#include <cstdlib>
#include <filesystem>

void EditorUtility::RevealInFinder(const eastl::string path)
{
    if (path.empty())
    {
        return;
    }

    std::filesystem::path fs_path(path.c_str());
    fs_path = fs_path.lexically_normal();
    if (fs_path.is_relative())
    {
        fs_path = std::filesystem::absolute(fs_path).lexically_normal();
    }

    std::string command = "/usr/bin/open -R \"" + fs_path.string() + "\"";
    std::system(command.c_str());
}

void EditorUtility::OpenInExternalEditor(const eastl::string path)
{
    if (path.empty())
        return;

    std::filesystem::path fs_path(path.c_str());
    fs_path = fs_path.lexically_normal();
    if (fs_path.is_relative())
        fs_path = std::filesystem::absolute(fs_path).lexically_normal();

    std::error_code ec;
    if (!std::filesystem::exists(fs_path, ec))
        return;

    const std::string quoted = "\"" + fs_path.string() + "\"";

    // 1. ZENGINE_EXTERNAL_EDITOR env var
    if (const char* override_path = std::getenv("ZENGINE_EXTERNAL_EDITOR"))
    {
        if (override_path && *override_path)
        {
            std::string cmd = std::string("\"") + override_path + "\" " + quoted;
            if (std::system(cmd.c_str()) == 0)
                return;
        }
    }

    // 2. VSCode via `open -a` (works whether VSCode is registered as a
    // protocol handler or not). open returns 0 on success; non-zero
    // means -a couldn't find the app.
    {
        std::string cmd = "/usr/bin/open -a \"Visual Studio Code\" " + quoted;
        if (std::system(cmd.c_str()) == 0)
            return;
    }

    // 3. OS default
    std::string fallback = std::string("/usr/bin/open ") + quoted;
    std::system(fallback.c_str());
}

bool EditorUtility::OpenFileDialog(const std::string&, const std::string&, const std::string&, std::string& out_path)
{
    out_path.clear();
    return false;
}

bool EditorUtility::SaveFileDialog(const std::string&, const std::string&, const std::string&, std::string& out_path)
{
    out_path.clear();
    return false;
}
