#pragma once

// =====================================================================================
// EditorUndo -- the editor's undo/redo service (Edit > Undo, Ctrl+Z / Ctrl+Y).
// -------------------------------------------------------------------------------------
// Two layers:
//   * UndoSystem (UndoSystem.h)      engine-agnostic transaction stack, unit tested.
//   * EditorUndoSystem (this file)   owns one UndoSystem, wires it to the editor: the
//                                    selection is restored with every step, the level
//                                    is marked dirty / clean against the save point,
//                                    history is dropped when the level it refers to is
//                                    reloaded, recording is suspended in play mode.
//
// Call sites use the EditorUndo:: free functions, modelled on Unity's Undo class and
// UE's FScopedTransaction:
//
//     EditorUndo::ScopedTransaction transaction("Set Position");   // optional
//     EditorUndo::RecordTransform(transform);                      // BEFORE the change
//     transform->SetLocalPosition(p);
//
//   RecordComponent / RecordTransform   snapshot the object before you modify it
//                                       (Unity Undo.RecordObject)
//   RegisterCreatedGameObject           after creating an object
//                                       (Unity Undo.RegisterCreatedObjectUndo)
//   DestroyGameObjects                  delete through the undo system
//                                       (Unity Undo.DestroyObjectImmediate)
//
// Without a ScopedTransaction, everything recorded during one frame becomes one undo
// step (Unity's per-event undo group). Use an explicit transaction to name the step
// or to span several frames (gizmo drags).
//
// What is NOT covered yet: asset edits (materials, textures, data tables are written
// straight to disk by their inspectors), component add/remove (there is no editor UI
// for it), and anything done while playing.
// =====================================================================================

#include "Editor/Undo/UndoSystem.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <cstdint>
#include <string>
#include <vector>

class Component;
class Level;
class Transform;

class EditorUndoSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return GET_CLASS_NAME(EditorUndoSystem); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    bool Initialize() override;
    void Shutdown() override;

    /// Once per editor frame: closes the implicit (per-frame) transaction, drops the
    /// history if the level it refers to went away, tracks the save point.
    void Tick();

    UndoSystem& GetCore() { return m_Core; }
    const UndoSystem& GetCore() const { return m_Core; }

    bool PerformUndo();
    bool PerformRedo();
    size_t PerformUndoMany(size_t count);
    void ClearHistory(const char* reason);

private:
    void OnUndoChanged(UndoChange change, const std::string& title);
    /// Returns false (and clears the history) when the history belongs to a level
    /// that is no longer the active one, or was reloaded since.
    bool ValidateBoundLevel();
    void BindToLevel(Level* level);
    void SyncDirtyFlagWithSavePoint();

    UndoSystem m_Core;

    Level* m_BoundLevel {nullptr};
    uint64_t m_BoundLevelSerial {0};

    // Save point: the undo state id the level was last saved (or loaded) at. Undoing
    // or redoing back to it clears the dirty flag again (Unity / UE behaviour).
    uint64_t m_SavePointStateId {0};
    bool m_LevelWasDirty {false};
};

namespace EditorUndo
{
    /// Null before the editor systems are up (e.g. during level load at startup);
    /// every function below is a safe no-op then.
    EditorUndoSystem* GetSystem();

    /// True when a record offered now would be kept (system up, not playing, not
    /// in the middle of applying an undo).
    bool IsRecording();

    void BeginTransaction(const std::string& title);
    void EndTransaction();
    /// Revert and discard the open transaction (e.g. Esc during a drag).
    void CancelTransaction();

    /// RAII Begin/End pair (UE FScopedTransaction).
    class ScopedTransaction
    {
    public:
        explicit ScopedTransaction(const std::string& title);
        ~ScopedTransaction();
        ScopedTransaction(const ScopedTransaction&) = delete;
        ScopedTransaction& operator=(const ScopedTransaction&) = delete;

        /// Revert what was recorded inside this scope and record nothing.
        void Cancel();

    private:
        bool m_Active {false};
    };

    /// Snapshot a component's serialized state before modifying it. A Transform is
    /// routed to RecordTransform (its hierarchy links must not be overwritten raw).
    /// Recording the same component twice in one transaction keeps the first state.
    void RecordComponent(Component* component, const std::string& title = "Modify Component");

    /// Snapshot local position / rotation / scale, parent, sibling index and the
    /// active flag of `transform` before changing any of them.
    void RecordTransform(Transform* transform, const std::string& title = "Modify Transform");

    /// Call right after an object was added to the active level.
    void RegisterCreatedGameObject(GObjectID object_id, const std::string& title = "Create GameObject");

    /// Delete objects from the active level through the undo system (each one with
    /// its whole child subtree, Unity semantics; an id whose ancestor is also listed
    /// is skipped). Works -- without an undo step -- even when recording is off.
    /// Returns how many objects (subtree members included) were removed.
    size_t DestroyGameObjects(const std::vector<GObjectID>& object_ids, const std::string& title = "Delete");

    bool CanUndo();
    bool CanRedo();
    bool Undo();
    bool Redo();
    std::string GetUndoTitle();
    std::string GetRedoTitle();
}  // namespace EditorUndo
