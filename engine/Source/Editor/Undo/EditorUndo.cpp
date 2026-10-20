#include "Editor/Undo/EditorUndo.h"

#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/Undo/EditorUndoRecords.h"
#include "Runtime/Application/Application.h"  // g_isPlaying
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

#include <algorithm>
#include <unordered_set>

using EditorUndoRecords::ComponentSnapshotRecord;
using EditorUndoRecords::GameObjectLifetimeRecord;
using EditorUndoRecords::GetActiveLevel;
using EditorUndoRecords::SelectionContext;
using EditorUndoRecords::TransformStateRecord;

namespace
{
    // Undo state ids start at 1 and only grow; this one is never reached.
    constexpr uint64_t kUnreachableSavePoint = ~0ull;
}  // namespace

// =====================================================================================
// EditorUndoSystem
// =====================================================================================

bool EditorUndoSystem::Initialize()
{
    m_Core.SetContextFactory([]() -> std::unique_ptr<IUndoContext> { return std::make_unique<SelectionContext>(); });
    m_Core.SetChangeListener(
        [this](UndoChange change, const std::string& title) { OnUndoChanged(change, title); });
    return true;
}

void EditorUndoSystem::Shutdown()
{
    // Records may own removed GameObjects; release them while the systems their
    // destructors talk to still exist.
    m_Core.SetChangeListener({});
    m_Core.Clear();
    m_BoundLevel = nullptr;
}

void EditorUndoSystem::BindToLevel(Level* level)
{
    m_BoundLevel = level;
    m_BoundLevelSerial = level != nullptr ? level->GetLoadSerial() : 0;
    m_SavePointStateId = m_Core.GetCurrentStateId();
    m_LevelWasDirty = level != nullptr && level->IsDirty();
}

void EditorUndoSystem::ClearHistory(const char* reason)
{
    if (m_Core.GetUndoCount() > 0 || m_Core.GetRedoCount() > 0)
    {
        LOG_INFO(ZUndo, "history cleared ({}): {} undo / {} redo steps dropped", reason, m_Core.GetUndoCount(),
                 m_Core.GetRedoCount());
    }
    m_Core.Clear();
    BindToLevel(GetActiveLevel());
}

bool EditorUndoSystem::ValidateBoundLevel()
{
    // In play mode the active level is the Play World copy; the editor level the
    // history belongs to is untouched and becomes active again on stop.
    if (g_isPlaying)
    {
        return true;
    }

    Level* level = GetActiveLevel();
    const uint64_t serial = level != nullptr ? level->GetLoadSerial() : 0;
    if (level == m_BoundLevel && serial == m_BoundLevelSerial)
    {
        return true;
    }

    // Opened another scene, or reloaded this one (records refer to object ids of
    // the previous load): the history is meaningless now.
    ClearHistory(level == m_BoundLevel ? "level reloaded" : "active level changed");
    return false;
}

void EditorUndoSystem::Tick()
{
    m_Core.SetRecordingSuspended(g_isPlaying);
    m_Core.FlushImplicitTransaction();
    ValidateBoundLevel();

    // Save point bookkeeping (see SyncDirtyFlagWithSavePoint).
    if (!g_isPlaying)
    {
        Level* level = GetActiveLevel();
        const bool dirty = level != nullptr && level->IsDirty();
        if (m_LevelWasDirty && !dirty)
        {
            // Went clean without undo touching it: the level was saved. Undoing /
            // redoing back to this state later counts as clean again.
            m_SavePointStateId = m_Core.GetCurrentStateId();
        }
        else if (!m_LevelWasDirty && dirty && !m_Core.IsTransactionOpen())
        {
            // Dirtied by something that is not recorded (no transaction open, no
            // commit marked it): no undo state matches the file on disk any more, so
            // undo must never report the level clean again.
            m_SavePointStateId = kUnreachableSavePoint;
        }
        m_LevelWasDirty = dirty;
    }
}

void EditorUndoSystem::SyncDirtyFlagWithSavePoint()
{
    Level* level = GetActiveLevel();
    if (level == nullptr)
    {
        return;
    }
    if (m_Core.GetCurrentStateId() == m_SavePointStateId)
    {
        level->ClearDirty();
    }
    else
    {
        level->MarkDirty();
    }
    m_LevelWasDirty = level->IsDirty();
}

void EditorUndoSystem::OnUndoChanged(UndoChange change, const std::string& title)
{
    switch (change)
    {
        case UndoChange::Committed:
        {
            // Every undoable edit dirties the scene (Unity semantics). Inspector field
            // edits did not do this on their own.
            if (Level* level = GetActiveLevel())
            {
                level->MarkDirty();
                m_LevelWasDirty = true;
            }
            LOG_DEBUG(ZUndo, "recorded '{}' ({} steps)", title, m_Core.GetUndoCount());
            break;
        }
        case UndoChange::Undone:
        case UndoChange::Redone:
        {
            SyncDirtyFlagWithSavePoint();
            if (EditorSceneManager* scene_manager = GET_SYSTEM(EditorSceneManager))
            {
                scene_manager->DrawSelectedEntityAxis();
            }
            LOG_INFO(ZUndo, "{} '{}'", change == UndoChange::Undone ? "undo" : "redo", title);
            break;
        }
        case UndoChange::Cleared:
            break;
    }
}

bool EditorUndoSystem::PerformUndo()
{
    if (g_isPlaying || !ValidateBoundLevel())
    {
        return false;
    }
    if (m_Core.IsExplicitTransactionOpen())
    {
        LOG_INFO(ZUndo, "undo ignored: an edit is still in progress");
        return false;
    }
    return m_Core.Undo();
}

bool EditorUndoSystem::PerformRedo()
{
    if (g_isPlaying || !ValidateBoundLevel())
    {
        return false;
    }
    if (m_Core.IsExplicitTransactionOpen())
    {
        LOG_INFO(ZUndo, "redo ignored: an edit is still in progress");
        return false;
    }
    return m_Core.Redo();
}

size_t EditorUndoSystem::PerformUndoMany(size_t count)
{
    size_t done = 0;
    while (done < count && PerformUndo())
    {
        ++done;
    }
    return done;
}

// =====================================================================================
// EditorUndo facade
// =====================================================================================

namespace EditorUndo
{
    EditorUndoSystem* GetSystem() { return GET_SYSTEM(EditorUndoSystem); }

    bool IsRecording()
    {
        EditorUndoSystem* system = GetSystem();
        return system != nullptr && !g_isPlaying && system->GetCore().CanRecord();
    }

    void BeginTransaction(const std::string& title)
    {
        if (EditorUndoSystem* system = GetSystem())
        {
            system->GetCore().SetRecordingSuspended(g_isPlaying);
            system->GetCore().BeginTransaction(title);
        }
    }

    void EndTransaction()
    {
        if (EditorUndoSystem* system = GetSystem())
        {
            system->GetCore().EndTransaction();
        }
    }

    void CancelTransaction()
    {
        if (EditorUndoSystem* system = GetSystem())
        {
            system->GetCore().CancelTransaction();
        }
    }

    ScopedTransaction::ScopedTransaction(const std::string& title)
        : m_Active(GetSystem() != nullptr)
    {
        if (m_Active)
        {
            BeginTransaction(title);
        }
    }

    ScopedTransaction::~ScopedTransaction()
    {
        if (m_Active)
        {
            EndTransaction();
        }
    }

    void ScopedTransaction::Cancel()
    {
        if (m_Active)
        {
            CancelTransaction();
            m_Active = false;
        }
    }

    void RecordComponent(Component* component, const std::string& title)
    {
        if (component == nullptr || !IsRecording())
        {
            return;
        }
        if (Transform* transform = dynamic_cast<Transform*>(component))
        {
            RecordTransform(transform, title);
            return;
        }

        UndoSystem& core = GetSystem()->GetCore();
        // Already recorded in this transaction: the first snapshot is the one that
        // matters, skip serializing the component again.
        if (core.HasPendingRecord(ComponentSnapshotRecord::MakeKey(EditorUndoRecords::EnsureInstanceID(component))))
        {
            return;
        }
        core.AddRecord(ComponentSnapshotRecord::Create(component), title);
    }

    void RecordTransform(Transform* transform, const std::string& title)
    {
        if (transform == nullptr || !IsRecording())
        {
            return;
        }
        UndoSystem& core = GetSystem()->GetCore();
        if (core.HasPendingRecord(TransformStateRecord::MakeKey(EditorUndoRecords::EnsureInstanceID(transform))))
        {
            return;
        }
        core.AddRecord(TransformStateRecord::Create(transform), title);
    }

    void RegisterCreatedGameObject(GObjectID object_id, const std::string& title)
    {
        if (object_id == k_invalid_gobject_id || !IsRecording())
        {
            return;
        }
        GetSystem()->GetCore().AddRecord(std::make_unique<GameObjectLifetimeRecord>(object_id), title);
    }

    size_t DestroyGameObjects(const std::vector<GObjectID>& object_ids, const std::string& title)
    {
        Level* level = GetActiveLevel();
        if (level == nullptr || object_ids.empty())
        {
            return 0;
        }

        // Deleting a parent deletes its subtree, so an id whose ancestor is also in
        // the list is already covered (and would otherwise be detached from it).
        const std::unordered_set<GObjectID> requested(object_ids.begin(), object_ids.end());
        auto has_requested_ancestor = [&](GObjectID object_id) {
            std::shared_ptr<GameObject> object = level->GetGObjectByID(object_id).lock();
            Transform* transform = object != nullptr ? object->tryGetComponent(Transform) : nullptr;
            for (Transform* parent = transform != nullptr ? transform->GetParent() : nullptr; parent != nullptr;
                 parent = parent->GetParent())
            {
                GameObject* parent_object = parent->GetParentObject();
                if (parent_object != nullptr && requested.count(parent_object->GetID()) != 0)
                {
                    return true;
                }
            }
            return false;
        };

        const bool recording = IsRecording();
        size_t removed = 0;
        for (const GObjectID object_id : object_ids)
        {
            if (level->GetGObjectByID(object_id).expired() || has_requested_ancestor(object_id))
            {
                continue;
            }

            auto record = std::make_unique<GameObjectLifetimeRecord>(object_id);
            removed += record->Remove();
            if (recording)
            {
                GetSystem()->GetCore().AddRecord(std::move(record), title);
            }
            // else: the record dies here and releases the removed objects -- a plain
            // delete, exactly what happened before undo existed.
        }
        return removed;
    }

    bool CanUndo()
    {
        EditorUndoSystem* system = GetSystem();
        return system != nullptr && !g_isPlaying && system->GetCore().GetUndoCount() > 0 &&
               !system->GetCore().IsExplicitTransactionOpen();
    }

    bool CanRedo()
    {
        EditorUndoSystem* system = GetSystem();
        return system != nullptr && !g_isPlaying && system->GetCore().GetRedoCount() > 0 &&
               !system->GetCore().IsExplicitTransactionOpen();
    }

    bool Undo()
    {
        EditorUndoSystem* system = GetSystem();
        return system != nullptr && system->PerformUndo();
    }

    bool Redo()
    {
        EditorUndoSystem* system = GetSystem();
        return system != nullptr && system->PerformRedo();
    }

    std::string GetUndoTitle()
    {
        EditorUndoSystem* system = GetSystem();
        return system != nullptr ? system->GetCore().GetUndoTitle() : std::string();
    }

    std::string GetRedoTitle()
    {
        EditorUndoSystem* system = GetSystem();
        return system != nullptr ? system->GetCore().GetRedoTitle() : std::string();
    }
}  // namespace EditorUndo
