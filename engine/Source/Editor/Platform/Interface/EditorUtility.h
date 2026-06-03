#pragma once

#include <string>

class EditorUtility
{
public:
    static void RevealInFinder(const eastl::string path);

    /// Phase-6 helper: open a text source file (.ts / .tsx / .js / .json /
    /// .glsl / ...) in an external code editor. Tries VSCode first (looks
    /// for `code` on PATH); falls back to the OS file association. Silent
    /// no-op when path is empty.
    ///
    /// On Windows the search order is:
    ///   1. ZENGINE_EXTERNAL_EDITOR env var (full exe path)
    ///   2. `code.cmd` on PATH (VSCode's installer adds itself to PATH)
    ///   3. ShellExecuteW(L"open", path) - whatever is associated with .ts
    /// On macOS:
    ///   1. ZENGINE_EXTERNAL_EDITOR env var
    ///   2. `open -a "Visual Studio Code" <path>`
    ///   3. `open <path>` - default associated app
    static void OpenInExternalEditor(const eastl::string path);

    static bool OpenFileDialog(const std::string& title,
                               const std::string& default_directory,
                               const std::string& default_file_name,
                               std::string& out_path);
    static bool SaveFileDialog(const std::string& title,
                               const std::string& default_directory,
                               const std::string& default_file_name,
                               std::string& out_path);
};
