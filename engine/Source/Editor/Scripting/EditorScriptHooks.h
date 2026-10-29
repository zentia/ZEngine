#pragma once

// ----------------------------------------------------------------------------
// EditorScriptHooks -- where editor TypeScript hangs its extensions (P2).
//
// Two registries plus the one way to call back into the editor VM. Native,
// NOT script-bound: scripts reach these through the EditorMenu /
// EditorInspector facades, while the editor's own UI (MenuController,
// ZSlateInspectorWindow) reads them directly. Keeping the store out of the
// facades is what lets the UI stay ignorant of scripting.
//
// Why callbacks are (module, function) and not a closure
// ------------------------------------------------------
// ScriptEnv can only invoke `module.exports.<fn>()` by name
// (InvokeExportedFunction); it cannot store a JS function value and call it
// later. So a hook names an exported function, exactly the way Unity's
// [MenuItem] decorates a static method. That is also the robust choice across
// a hot reload: a stale closure would point into a dead module, whereas a name
// re-resolves against whatever is loaded now.
//
// Threading: main thread only. Registration happens during OnEditorLoad and
// invocation happens from UI callbacks, both on the editor's frame thread.
// ----------------------------------------------------------------------------

#include <string>
#include <vector>

namespace EditorScriptHooks
{
    /// An exported editor-VM function, addressed by name.
    struct Callback
    {
        std::string module;    ///< module id, e.g. "Bootstrap"
        std::string function;  ///< exported function name, e.g. "OnBakeClicked"

        bool IsValid() const { return !module.empty() && !function.empty(); }
    };

    /// One script-registered menu entry.
    struct MenuItem
    {
        /// Slash-separated path under the Tools menu, e.g. "Validate/Scene".
        /// The leading segments become submenus; the last is the item label.
        std::string path;
        Callback callback;
        /// Lower sorts first; ties fall back to path order so the menu is
        /// stable regardless of registration order (scripts load in whatever
        /// order the module list happens to be in).
        int priority {0};
    };

    /// One script-registered Inspector button, attached to a component type.
    struct InspectorButton
    {
        /// Component type name as `Component::GetTypeName()` reports it, e.g.
        /// "AnimationComponent". Matched exactly -- no base-class walk, so a
        /// button registered for a base type does not show on derived ones.
        std::string component_type;
        std::string label;
        Callback callback;
    };

    // ---- menu items --------------------------------------------------------

    /// Adds (or replaces, when `path` already exists) a menu item.
    /// Returns false when path / callback are empty.
    bool AddMenuItem(const std::string& path, const Callback& callback, int priority);

    /// Removes the item at `path`; returns whether one was there.
    bool RemoveMenuItem(const std::string& path);

    /// Every item, already sorted by (priority, path).
    const std::vector<MenuItem>& GetMenuItems();

    // ---- inspector buttons -------------------------------------------------

    bool AddInspectorButton(const std::string& component_type, const std::string& label, const Callback& callback);

    /// Drops every button registered for `component_type`; returns how many.
    size_t ClearInspectorButtons(const std::string& component_type);

    /// Buttons for `component_type`, in registration order.
    std::vector<const InspectorButton*> GetInspectorButtons(const std::string& component_type);

    /// Total across all component types (diagnostics / tests).
    size_t GetInspectorButtonCount();

    // ---- invocation --------------------------------------------------------

    /// Calls `callback` in the editor VM. Returns false (and logs) when the VM
    /// is down, the module is not loaded, or the function threw -- a broken
    /// hook must never take the editor with it, because these run from menu
    /// clicks and Inspector buttons.
    bool Invoke(const Callback& callback);

    /// Drops every registration. Called when the editor VM is torn down, so a
    /// reloaded script layer does not inherit hooks pointing at dead modules.
    void Clear();
}  // namespace EditorScriptHooks
