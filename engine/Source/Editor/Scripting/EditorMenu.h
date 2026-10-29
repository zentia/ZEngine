#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <eastl/string.h>

#include "EditorMenu.generated.h"

// Editor-VM only: register menu items from TypeScript (Unity's `[MenuItem]`).
//
// P2. Registered items appear under the editor's top-level **Tools** menu;
// `path` may contain "/" to nest, e.g. "Validate/Check Scene" becomes
// Tools > Validate > Check Scene.
//
// The handler is named, not passed
// --------------------------------
// `AddItem("...", "Bootstrap", "OnValidateScene")` names an EXPORTED function
// of a loaded module rather than taking a callback, because ScriptEnv can only
// invoke `module.exports.<fn>()` by name -- it cannot retain a JS function
// value. That mirrors [MenuItem] decorating a static method, and it survives a
// script reload: a name re-resolves against the module that is loaded now,
// where a retained closure would dangle into the old one.
//
// Registering the same `path` twice replaces the first registration, so
// OnEditorLoad running again after a reload does not double every row.
ZCLASS()
class EditorMenu : public Object
{
    ZGENERATED_BODY(EditorMenu)

public:
    /// Adds a Tools-menu item that calls `module`.`function` when clicked.
    /// `priority` orders the rows (lower first; ties sort by path).
    /// Returns false when path / module / function is empty.
    ZFUNCTION()
    static bool AddItem(const eastl::string& path,
                        const eastl::string& module,
                        const eastl::string& function_name,
                        int priority);

    /// Removes the item registered at `path`; returns whether one existed.
    ZFUNCTION()
    static bool RemoveItem(const eastl::string& path);

    /// Number of registered items.
    ZFUNCTION()
    static int GetItemCount();

    /// Path of the item at `index` in menu order, or "" when out of range.
    /// Lets a script verify its own registration without reading the UI.
    ZFUNCTION()
    static eastl::string GetItemPathAt(int index);

    /// Invokes the handler registered at `path` as if the user clicked it.
    /// Returns false when nothing is registered there or the handler threw.
    /// This is what makes menu items testable headlessly -- there is no way to
    /// synthesise a click on a ZSlate row from a script.
    ZFUNCTION()
    static bool InvokeItem(const eastl::string& path);
};
