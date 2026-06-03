#pragma once

#include <string>

enum class SceneSavePromptResult
{
    Save,
    DontSave,
    Cancel,
};

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
    // default_extension: extension without dot (e.g. "scene", "json").
    // filter_glob: Windows file-type glob (e.g. "*.scene"); nullptr keeps the
    // legacy layout/json filter set used by layout save.
    static bool SaveFileDialog(const std::string& title,
                               const std::string& default_directory,
                               const std::string& default_file_name,
                               std::string& out_path,
                               const char* default_extension = "json",
                               const char* filter_glob = nullptr);

    // Unity-style unsaved-scene prompt (Save / Don't Save / Cancel).
    static SceneSavePromptResult PromptUnsavedScene(const std::string& scene_display_name);
};
