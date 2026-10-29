#pragma once

#include "Runtime/BaseClasses/Object.h"
// Full definitions, not forward declarations: the generated bindings
// instantiate ZScript::Converter<T*>, whose __is_base_of check needs complete
// types (see EditorSelection.h for the same note).
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"

#include <eastl/string.h>

#include "EditorHistory.generated.h"

// Editor-VM only: the undo history (Unity's `UnityEditor.Undo`).
//
// P1 added query + navigate + group. P3 added the Record* family, which is what
// makes a scripted edit undoable.
//
// Named EditorHistory, not EditorUndo: the class name IS the script-facing name
// (ZHeaderTool has no ScriptName specifier), and `EditorUndo` is already a
// namespace in this module.
//
// Why Record* had to exist (correcting the P1 note)
// -------------------------------------------------
// P1 claimed a script could not record state because "the record types are C++
// and a script cannot describe the fields of this object before I touch it".
// That was WRONG: EditorUndo::RecordComponent snapshots a component's whole
// serialized state through the TypeTree and even routes a Transform to the
// dedicated transform record. The script describes nothing -- it passes the
// pointer it already has from EditorSelection / GetComponent.
//
// The omission mattered, because the editor VM also installs the Runtime-domain
// bindings: an editor script could ALREADY call Transform.SetPosition,
// Transform.SetParent, Component.SetEnabled, GameObject.SetName / SetActive /
// AddComponent since P0 -- and none of it landed in the undo history. Scripted
// edits were silently destroying the user's ability to Ctrl+Z. That is the hole
// P3 closes.
//
// Usage
// -----
//     EditorHistory.BeginTransaction("Flatten Selection");
//     for (let i = 0; i < EditorSelection.GetObjectCount(); i++) {
//         const go = EditorSelection.GetObjectAt(i);
//         EditorHistory.RecordGameObject(go, "Flatten");   // BEFORE the change
//         go.GetTransform().SetLocalPosition(/* ... */);
//     }
//     EditorHistory.EndTransaction();
//
// Record* must be called BEFORE mutating -- it snapshots the current state, so
// recording afterwards stores the already-changed values and Ctrl+Z becomes a
// no-op. Recording the same target twice in one transaction keeps the first
// snapshot, so a loop that touches an object repeatedly stays correct.
ZCLASS()
class EditorHistory : public Object
{
    ZGENERATED_BODY(EditorHistory)

public:
    /// Whether there is anything to undo. False while a transaction is open or
    /// an undo is already being applied.
    ZFUNCTION()
    static bool CanUndo();

    ZFUNCTION()
    static bool CanRedo();

    /// Undoes one transaction; returns whether anything happened.
    ZFUNCTION()
    static bool Undo();

    ZFUNCTION()
    static bool Redo();

    /// Title of the transaction the next undo would revert ("" when none) --
    /// the string Unity shows after "Undo " in the Edit menu.
    ZFUNCTION()
    static eastl::string GetUndoTitle();

    ZFUNCTION()
    static eastl::string GetRedoTitle();

    /// Depth of each stack, for tests and history UI.
    ZFUNCTION()
    static int GetUndoCount();

    ZFUNCTION()
    static int GetRedoCount();

    /// Opens a named transaction: everything recorded until EndTransaction()
    /// collapses into ONE undo step.
    ZFUNCTION()
    static void BeginTransaction(const eastl::string& title);

    /// Closes the open transaction. An empty one is discarded rather than
    /// pushed, so a script that opens a transaction and changes nothing does
    /// not leave a no-op entry in the user's history.
    ZFUNCTION()
    static void EndTransaction();

    /// Abandons the open transaction, reverting what it recorded.
    ZFUNCTION()
    static void CancelTransaction();

    /// Whether a record offered right now would be kept.
    ZFUNCTION()
    static bool IsRecording();

    /// Drops the whole history. `reason` is logged: histories get cleared when
    /// the world changes underneath them (level load, etc.) and an unexplained
    /// clear is very hard to diagnose after the fact.
    ZFUNCTION()
    static void ClearHistory(const eastl::string& reason);

    // ---- recording (P3) ----------------------------------------------------
    //
    // Every one is a no-op when recording is off (no editor systems, Play mode,
    // or an undo is being applied), so a tool script does not have to branch on
    // IsRecording() -- it records unconditionally and the system decides.

    /// Snapshots a component's serialized state. A Transform is routed to the
    /// dedicated transform record automatically, so this is the one call a
    /// script needs for any component.
    ZFUNCTION()
    static void RecordComponent(Component* component, const eastl::string& title);

    /// Snapshots local TRS, parent, sibling index and the active flag.
    /// `RecordComponent` on a Transform does the same thing; this exists so a
    /// script that already holds a Transform does not have to up-cast.
    ZFUNCTION()
    static void RecordTransform(Transform* transform, const eastl::string& title);

    /// Snapshots the object's Transform -- the common "I am about to move /
    /// reparent / (de)activate this" case.
    ///
    /// NOTE: this does NOT cover `GameObject.SetName`. No undo record stores a
    /// GameObject's name today (the native editor has the same gap -- nothing
    /// in the Hierarchy records a rename), so a scripted rename is not
    /// undoable. Flagged here rather than silently implied.
    ZFUNCTION()
    static void RecordGameObject(GameObject* object, const eastl::string& title);

    /// Call right AFTER creating an object (e.g. GameObject.Instantiate) so
    /// undo removes it again. Unlike the Record* calls above, this one comes
    /// after the change -- there is no prior state to snapshot.
    ZFUNCTION()
    static void RegisterCreatedGameObject(GameObject* object, const eastl::string& title);

    /// Deletes `object` and its child subtree THROUGH the undo system, so the
    /// delete can be reverted. Returns how many objects were removed.
    ///
    /// Use this instead of `GameObject.Destroy` in editor scripts:
    /// GameObject.Destroy is the runtime path and leaves no undo step, which
    /// makes a scripted delete unrecoverable.
    ZFUNCTION()
    static int DestroyGameObject(GameObject* object, const eastl::string& title);
};
