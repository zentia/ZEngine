#pragma once

#include "Runtime/BaseClasses/Object.h"
// Full definition, not a forward declaration: the generated binding
// instantiates ZScript::Converter<GameObject*>, whose __is_base_of check needs
// a complete type. A forward declaration compiles this header fine and then
// fails inside EditorSelection.gen.cpp.
#include "Runtime/BaseClasses/GameObject.h"

#include <eastl/string.h>

#include <vector>

#include "EditorSelection.generated.h"

// Editor-VM only: what the user currently has selected (Unity's
// `UnityEditor.Selection`).
//
// P1. Published only into the editor-scripting VM -- ZHeaderTool stamps every
// ZEditor binding as ZScriptDomain::Editor, so game scripts never see it.
//
// Counted access AND array access
// -------------------------------
// `GetObjectCount()` + `GetObjectAt(i)` came first because ZFUNCTION could not
// marshal a container. P4 added Converter<std::vector<T>>, so the array-shaped
// `GetInstanceIds()` / `SetObjects()` exist too. Both shapes are kept on
// purpose:
//   * counted access touches no heap and reads one element at a time -- right
//     for a loop over a large selection, and the only shape that can hand back
//     a live GameObject*;
//   * the array shape is a COPY (see the Converter note in ScriptBinding.h), so
//     it is the ergonomic choice for "give me the whole thing", and the only
//     way to SET a multi-object selection in one call.
//
// Selection state lives on EditorSceneManager; this is a thin facade over it,
// so there is exactly one source of truth and the Hierarchy / Inspector cannot
// disagree with what scripts observe.
ZCLASS()
class EditorSelection : public Object
{
    ZGENERATED_BODY(EditorSelection)

public:
    /// Number of selected GameObjects (0 when nothing is selected).
    ZFUNCTION()
    static int GetObjectCount();

    /// The selected GameObject at `index`, or null when out of range.
    ZFUNCTION()
    static GameObject* GetObjectAt(int index);

    /// The ACTIVE object -- the one the Inspector shows, i.e. the last one
    /// clicked. Null when the selection is empty. Mirrors Unity's
    /// `Selection.activeGameObject`.
    ZFUNCTION()
    static GameObject* GetActiveObject();

    /// InstanceID of the selected object at `index`, or 0 when out of range.
    /// Useful for comparing selections without holding the objects alive.
    ZFUNCTION()
    static int GetInstanceIdAt(int index);

    /// Replaces the selection with a single object, which also becomes active.
    /// A null object clears the selection (same as Clear()).
    ZFUNCTION()
    static void SetActiveObject(GameObject* object);

    /// Selects the object named `name` in the active level, and returns whether
    /// one was found. Convenience for scripts that address objects by name the
    /// way the Hierarchy search field does.
    ZFUNCTION()
    static bool SelectByName(const eastl::string& name);

    /// Empties the selection.
    ZFUNCTION()
    static void Clear();

    // ---- array access (P4) -------------------------------------------------

    /// InstanceIDs of everything selected, in selection order. A snapshot --
    /// mutating the returned array does not change the selection.
    ///
    /// IDs rather than GameObject[]: a borrowed GameObject* is only valid for
    /// the duration of the call that produced it, so an ARRAY of them would
    /// outlive its own contract the moment the script held onto it. IDs stay
    /// meaningful, and GetObjectAt / SelectByName resolve them when needed.
    ZFUNCTION()
    static std::vector<int> GetInstanceIds();

    /// Replaces the selection with the objects named in `names`, in order; the
    /// first one that resolves becomes active. Names that match nothing are
    /// skipped. Returns how many were selected.
    ///
    /// This is the only way to set a MULTI-object selection from a script --
    /// SetActiveObject takes one object and therefore always collapses to a
    /// single-object selection.
    ZFUNCTION()
    static int SelectByNames(const std::vector<eastl::string>& names);
};
