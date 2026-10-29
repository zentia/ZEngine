#pragma once

// ----------------------------------------------------------------------------
// EditorScriptPanelModel -- what the Script Panel window shows (P5).
//
// A PUSH model: the script calls EditorScriptPanel.SetLines(...) / AddButton(...)
// whenever it wants to change the panel, this holds the result, and
// ZSlateScriptPanelWindow renders it. Native, NOT script-bound; the script side
// is the EditorScriptPanel facade.
//
// Why push and not an OnGUI callback
// ----------------------------------
// Unity's EditorWindow.OnGUI is a PULL model: the editor calls the script every
// repaint and the script draws. Pull needs two things this engine does not have
// yet -- (a) ScriptEnv::InvokeExportedFunction has no return-value plumbing, so
// a handler cannot hand content back, and (b) there is no widget-construction
// API across the boundary, so "draw" has nothing to call. Push needs neither:
// content crosses as plain arrays through the P4 container marshaller, and the
// native side owns every widget.
//
// The visible trade-off is that the panel does not refresh itself. A script that
// wants live data re-pushes from a menu item / Inspector button. That is a real
// limitation and it is why this is a minimal example rather than a general
// window system.
//
// Threading: main thread only (script calls and painting are both on the editor
// frame thread).
// ----------------------------------------------------------------------------

#include "Editor/Scripting/EditorScriptHooks.h"  // EditorScriptHooks::Callback

#include <string>
#include <vector>

namespace EditorScriptPanelModel
{
    /// One action row in the panel.
    struct Button
    {
        std::string label;
        EditorScriptHooks::Callback callback;
    };

    /// Replaces the text content. Empty clears it.
    void SetLines(std::vector<std::string> lines);

    /// Appends an action row; replaces the handler when `label` already exists,
    /// so re-pushing after a script reload does not stack duplicates.
    /// Returns false when label / callback are empty.
    bool AddButton(const std::string& label, const EditorScriptHooks::Callback& callback);

    /// Drops every button. Returns how many were removed.
    size_t ClearButtons();

    /// Clears lines and buttons.
    void Clear();

    const std::vector<std::string>& GetLines();
    const std::vector<Button>& GetButtons();

    /// Bumped on every mutation. The window repaints from scratch only when this
    /// changes, so a panel nobody is pushing to costs one integer compare per
    /// frame instead of a widget rebuild.
    uint64_t GetRevision();
}  // namespace EditorScriptPanelModel
