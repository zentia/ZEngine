#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <eastl/string.h>

#include <vector>

#include "EditorScriptPanel.generated.h"

// Editor-VM only: a dockable panel whose content a script pushes (P5).
//
// The window is "Script Panel" in the Window menu. It is ONE built-in panel
// scripts fill, not a general "create N windows" API -- the editor's window
// registry is a static descriptor table resolved at startup, so dynamically
// registered windows would need a different registry, which is a much larger
// change than this minimal example.
//
// Push, not OnGUI
// ---------------
// Unity's EditorWindow.OnGUI pulls: the editor calls the script every repaint
// and the script draws. That needs return-value plumbing through
// ScriptEnv::InvokeExportedFunction (there is none) and a widget-construction
// API across the boundary (there is none). So this pushes instead: the script
// hands over the content, the native side owns every widget.
//
//     EditorScriptPanel.SetLines([
//         "Selected: " + EditorSelection.GetObjectCount(),
//         "Undo depth: " + EditorHistory.GetUndoCount(),
//     ]);
//     EditorScriptPanel.AddButton("Refresh", "Bootstrap", "OnPanelRefresh");
//
// `SetLines` takes a string[] -- that is the P4 container marshaller
// (Converter<std::vector<T>>) doing the work; before it, this API would have
// had to be AddLine() called in a loop.
//
// The honest limitation: the panel does NOT refresh itself. Content is whatever
// was last pushed, so a script showing live data re-pushes from its own button
// or a Tools menu item. A self-refreshing panel needs the pull model above.
ZCLASS()
class EditorScriptPanel : public Object
{
    ZGENERATED_BODY(EditorScriptPanel)

public:
    /// Replaces the panel's text content. An empty array clears it.
    ZFUNCTION()
    static void SetLines(const std::vector<eastl::string>& lines);

    /// Appends an action row that calls `module`.`function_name` when clicked.
    /// Re-adding the same label replaces its handler rather than stacking a
    /// second row, so re-pushing after a script reload stays idempotent.
    ZFUNCTION()
    static bool AddButton(const eastl::string& label,
                          const eastl::string& module,
                          const eastl::string& function_name);

    /// Removes every action row; returns how many.
    ZFUNCTION()
    static int ClearButtons();

    /// Clears text and buttons.
    ZFUNCTION()
    static void Clear();

    /// Current content, for a script that wants to append rather than replace
    /// (SetLines overwrites). A snapshot -- see the Converter note in
    /// ScriptBinding.h.
    ZFUNCTION()
    static std::vector<eastl::string> GetLines();

    ZFUNCTION()
    static int GetButtonCount();

    /// Invokes an action row's handler as if it were clicked. Headless
    /// testability, same reason as EditorMenu.InvokeItem -- a ZSlate click
    /// cannot be synthesised from a script.
    ZFUNCTION()
    static bool InvokeButton(const eastl::string& label);
};
