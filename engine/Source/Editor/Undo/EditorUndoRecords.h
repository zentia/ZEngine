#pragma once

// Concrete undo records for the editor (see EditorUndo.h for the public API and
// UndoSystem.h for the swap-based record contract). Kept in their own TU so the
// serialization / transform / level includes stay out of EditorUndo.h.

#include "Editor/Undo/UndoSystem.h"
#include "Runtime/Core/JsonSerialize/JSONAllocator.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include "rapidjson/document.h"

#include <cstdint>
#include <memory>
#include <vector>

class Component;
class GameObject;
class Level;
class Object;
class Transform;

namespace EditorUndoRecords
{
    /// Records refer to components by InstanceID (resolved through ObjectManager at
    /// apply time), never by raw pointer: a pointer kept across an unrelated delete /
    /// level reload would dangle, an id simply fails to resolve. Assigns an id to
    /// objects created through MemoryManager::CreateObject, which start at 0.
    int32_t EnsureInstanceID(Object* object);

    Level* GetActiveLevel();

    // ---------------------------------------------------------------------------------
    // ComponentSnapshotRecord -- Unity Undo.RecordObject for any non-Transform component.
    // ---------------------------------------------------------------------------------
    // The snapshot is the component's YAML DOM: exactly the representation the .scene
    // file uses, so anything that survives a scene save/load round-trip survives an
    // undo. The DOM (not emitted YAML text) is kept so floats are restored bit-exact.
    // PPtr / ImmediatePtr fields round-trip by raw InstanceID (no resolver active).
    class ComponentSnapshotRecord : public IUndoRecord
    {
    public:
        using Document = rapidjson::GenericDocument<rapidjson::UTF8<>, JSONAllocator, JSONAllocator>;

        static std::unique_ptr<ComponentSnapshotRecord> Create(Component* component);
        static uint64_t MakeKey(int32_t instance_id);

        void Apply() override;
        bool Finalize() override;
        uint64_t GetCoalesceKey() const override { return MakeKey(m_InstanceID); }
        size_t GetMemoryBytes() const override { return m_MemoryBytes; }

    private:
        Component* Resolve() const;

        int32_t m_InstanceID {0};
        std::unique_ptr<Document> m_Stored;
        size_t m_MemoryBytes {0};
    };

    // ---------------------------------------------------------------------------------
    // TransformStateRecord -- Transform gets its own record instead of a raw snapshot:
    // its serialized m_Father / m_Children links mirror a TransformHierarchy buffer and
    // the scene-root list, which only SetParent / SetSiblingIndex keep consistent.
    // ---------------------------------------------------------------------------------
    struct TransformState
    {
        int32_t parent_instance_id {0};  // 0 = scene root
        int sibling_index {0};
        Vector3d local_position {Vector3d::ZERO};
        Quaternion local_rotation {Quaternion::IDENTITY};
        Vector3 local_scale {Vector3::UNIT_SCALE};
        bool enabled {true};  // Transform::m_Enabled is the GameObject active flag

        bool operator==(const TransformState& other) const;
    };

    class TransformStateRecord : public IUndoRecord
    {
    public:
        static std::unique_ptr<TransformStateRecord> Create(Transform* transform);
        static uint64_t MakeKey(int32_t instance_id);

        static TransformState Capture(Transform* transform);
        static void Restore(Transform* transform, const TransformState& state);

        void Apply() override;
        bool Finalize() override;
        uint64_t GetCoalesceKey() const override { return MakeKey(m_InstanceID); }
        size_t GetMemoryBytes() const override { return sizeof(*this); }

    private:
        Transform* Resolve() const;

        int32_t m_InstanceID {0};
        TransformState m_Stored;
    };

    // ---------------------------------------------------------------------------------
    // GameObjectLifetimeRecord -- create / delete of one object and its child subtree.
    // ---------------------------------------------------------------------------------
    // While the subtree is out of the level the record OWNS it (shared_ptr), so undoing
    // a delete brings back the very same GameObject / Component instances: ids, PPtrs
    // into them from elsewhere and runtime state all stay valid. This is UE's model (a
    // deleted actor lives on in the transaction buffer). When the record is discarded
    // (history trimmed / cleared) the objects are released -- exactly what a delete
    // did before undo existed.
    class GameObjectLifetimeRecord : public IUndoRecord
    {
    public:
        /// The record starts in the "object is in the level" state -- right for a
        /// create that was just made. A delete constructs it the same way and then
        /// calls Remove().
        explicit GameObjectLifetimeRecord(GObjectID root_id);

        /// Take the subtree out of the level now. Returns the number of objects
        /// removed (0 if the root id is unknown).
        size_t Remove();

        void Apply() override;
        bool Finalize() override { return true; }
        size_t GetMemoryBytes() const override;

    private:
        bool Restore();

        GObjectID m_RootId {k_invalid_gobject_id};
        bool m_InLevel {true};

        // Valid while removed (m_InLevel == false).
        std::vector<std::shared_ptr<GameObject>> m_Subtree;  // root first, then DFS order
        GObjectID m_ParentId {k_invalid_gobject_id};
        int m_SiblingIndex {0};
    };

    // ---------------------------------------------------------------------------------
    // SelectionContext -- the GameObject selection before / after each transaction.
    // ---------------------------------------------------------------------------------
    class SelectionContext : public IUndoContext
    {
    public:
        void CaptureBefore() override;
        void CaptureAfter() override;
        void RestoreBefore() override;
        void RestoreAfter() override;

    private:
        struct Snapshot
        {
            std::vector<GObjectID> ids;
            GObjectID active {k_invalid_gobject_id};
        };
        static Snapshot Capture();
        static void Restore(const Snapshot& snapshot);

        Snapshot m_Before;
        Snapshot m_After;
    };
}  // namespace EditorUndoRecords
