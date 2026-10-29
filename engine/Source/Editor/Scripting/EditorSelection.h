#pragma once

#include "Runtime/BaseClasses/Object.h"
// Full definition, not a forward declaration: the generated binding
// instantiates ZScript::Converter<GameObject*>, whose __is_base_of check needs
// a complete type. A forward declaration compiles this header fine and then
// fails inside EditorSelection.gen.cpp.
#include "Runtime/BaseClasses/GameObject.h"

#include <eastl/string.h>

#include "EditorSelection.generated.h"

// Editor-VM only: what the user currently has selected (Unity's
// `UnityEditor.Selection`).
//
// P1. Published only into the editor-scripting VM -- ZHeaderTool stamps every
// ZEditor binding as ZScriptDomain::Editor, so game scripts never see it.
//
// Why counted access instead of an array
// --------------------------------------
// `GetObjectCount()` + `GetObjectAt(i)` rather than a `GetObjects()` returning
// a vector: ZFUNCTION marshals scalars, strings and bound-class pointers, and
// `std::vector` is a hard error in the generator (ts_type_for). An array-shaped
// API would have to wait for a container marshaller; counted access needs
// nothing new and reads fine from TS:
//
//     for (let i = 0; i < EditorSelection.GetObjectCount(); i++) { ... }
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
};
