#pragma once

// =============================================================================
// ZEditorLauncher — the project picker shown when ZEditor starts without -p
// -----------------------------------------------------------------------------
// ZEditor needs a project to open, and the only way to name one used to be the
// command line. On Windows it is built as a WIN32 (GUI-subsystem) executable,
// so the old fallback -- printing usage to std::cerr -- went nowhere: double
// clicking the exe silently exited 1 and looked like a crash.
//
// The standalone ZLauncher that used to cover this was an ImGui window and was
// deleted with ImGui. This is the replacement: a ZSlate window hosted in the
// ZEditor process itself, before any engine system is started.
//
// It does not open the project. It returns the chosen suite + .zproject path
// and the caller relaunches ZEditor with -suite=/-p, because the project path
// is currently read from argv by CommandSystem, and injecting one before
// START_SYSTEM() would mean a second, parallel way to set it.
// =============================================================================

#include <filesystem>
#include <string>

namespace ZEditorLauncher
{
    /// 一次启动的决定：跑哪个工具套件，以及（若该套件需要）打开哪个项目。
    ///
    /// 为什么要套件这一维：engine/tools/ 下每个 GUI 工具原本都是一个独立 exe，
    /// 各自配置 / 构建 / 分发。合成一个 ZEditor 之后，启动时得先决定这次是
    /// 当编辑器用还是当某个工具用 —— 编辑器那套 START_SYSTEM() 会拉起 RHI
    /// 和世界，跑一个"读文件 → 算 → 写文件"的烘焙器根本不需要它们。
    struct LaunchChoice
    {
        /// 套件 id。`ZEditor::LevelEditorSuiteDesc().id` 表示关卡编辑器本体；
        /// 其它值对应 `ZEditor::FindToolSuite()` 能找到的套件。
        std::string suite_id;

        /// 仅当该套件 `needs_project` 时有效。
        std::filesystem::path project_file;
    };

    /// Shows the picker. Returns false when the user closed the window without
    /// choosing (or when the window could not be created at all, in which case
    /// the reason is reported on screen -- stderr is invisible to this process).
    bool Run(LaunchChoice& out_choice);

    /// Records a project as most-recently-used.
    ///
    /// Called by the editor once a project has actually loaded, so that projects
    /// opened without going through the picker -- double clicking a .zproject,
    /// or -p on the command line -- still show up in the list next time.
    void RememberProject(const std::filesystem::path& project_file);
}
