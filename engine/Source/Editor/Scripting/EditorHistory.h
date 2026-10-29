#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <eastl/string.h>

#include "EditorHistory.generated.h"

// Editor-VM only: the undo history (Unity's `UnityEditor.Undo`).
//
// P1. A thin facade over the EditorUndo free functions / EditorUndoSystem --
// see doc/UNDO_SYSTEM.md and AGENTS.md 2.32.
//
// Named EditorHistory, not EditorUndo: the class name IS the script-facing name
// (ZHeaderTool has no ScriptName specifier), and `EditorUndo` is already a
// namespace in this module.
//
// What is deliberately NOT here
// -----------------------------
// No `RecordObject` / `RecordComponent`. The undo system records state by
// swapping typed C++ snapshots (IUndoRecord); a script cannot describe "the
// fields of this object before I touch it" without a reflection-driven snapshot
// path, which does not exist yet. Exposing a half-working RecordObject would be
// worse than omitting it -- the script would look undoable and silently lose
// data on undo.
//
// So this is query + navigate + group: scripts can undo, redo, read the
// history, and wrap NATIVE editor operations (which do record properly) into
// one named transaction. RecordObject lands when a scripted-field snapshot
// exists.
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
};
