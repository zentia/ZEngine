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
// It does not open the project. It returns the chosen .zproject path and the
// caller relaunches ZEditor with -p, because the project path is currently read
// from argv by CommandSystem, and injecting one before START_SYSTEM() would
// mean a second, parallel way to set it.
// =============================================================================

#include <filesystem>

namespace ZEditorLauncher
{
    /// Shows the picker. Returns false when the user closed the window without
    /// choosing (or when the window could not be created at all, in which case
    /// the reason is reported on screen -- stderr is invisible to this process).
    bool Run(std::filesystem::path& out_project_file);

    /// Records a project as most-recently-used.
    ///
    /// Called by the editor once a project has actually loaded, so that projects
    /// opened without going through the picker -- double clicking a .zproject,
    /// or -p on the command line -- still show up in the list next time.
    void RememberProject(const std::filesystem::path& project_file);
}
