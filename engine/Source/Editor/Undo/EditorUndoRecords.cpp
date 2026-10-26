#include "Editor/Undo/EditorUndoRecords.h"

#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/IPPtrResolver.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/YamlSerialize/YAMLRead.h"
#include "Runtime/Core/YamlSerialize/YAMLWrite.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/RenderObject.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"

#include <algorithm>
#include <unordered_set>
#include "Runtime/BaseClasses/Cast.h"

namespace EditorUndoRecords
{
    namespace
    {
        // Coalesce-key namespaces: the low 32 bits are the InstanceID.
        constexpr uint64_t kComponentKeyTag = 1ull << 56;
        constexpr uint64_t kTransformKeyTag = 2ull << 56;

        using Document = ComponentSnapshotRecord::Document;
        using Value = rapidjson::GenericValue<rapidjson::UTF8<>, JSONAllocator>;

        std::unique_ptr<Document> SerializeComponent(Component* component)
        {
            // Null marker on the resolver stack: PPtr / ImmediatePtr fields encode
            // their raw InstanceID, which resolves back to the same live object on
            // read. Without it a resolver left active by an enclosing scene write
            // would remap them into file-local ids that mean nothing here.
            ScopedPPtrResolver raw_instance_ids(nullptr);

            YAMLWrite writer(kNoTransferInstructionFlags);
            component->VirtualRedirectTransfer(writer);

            auto document = std::make_unique<Document>();
            document->CopyFrom(writer.GetDocument(), document->GetAllocator());
            return document;
        }

        void DeserializeComponent(Component* component, const Document& document)
        {
            ScopedPPtrResolver raw_instance_ids(nullptr);
            YAMLRead reader(document, kNoTransferInstructionFlags);
            component->VirtualRedirectTransfer(reader);
        }

        size_t EstimateDomBytes(const Value& value)
        {
            size_t bytes = sizeof(Value);
            if (value.IsString())
            {
                bytes += value.GetStringLength() + 1;
            }
            else if (value.IsObject())
            {
                for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
                {
                    bytes += EstimateDomBytes(it->name) + EstimateDomBytes(it->value);
                }
            }
            else if (value.IsArray())
            {
                for (const Value& element : value.GetArray())
                {
                    bytes += EstimateDomBytes(element);
                }
            }
            return bytes;
        }

        void NotifyAllComponentsUpdated(GameObject* object)
        {
            if (object == nullptr)
            {
                return;
            }
            for (const ImmediatePtr<Component>& component_ptr : object->getComponents())
            {
                if (Component* component = component_ptr)
                {
                    component->OnSerializedFieldsUpdated();
                }
            }
        }

        bool IsLinkedSceneRoot(Level* level, Transform* transform)
        {
            if (level == nullptr || transform == nullptr)
            {
                return false;
            }
            return transform == level->GetTransformSceneRoots().GetFirstRoot() ||
                   transform->GetPrevSceneRoot() != nullptr || transform->GetNextSceneRoot() != nullptr;
        }

        GObjectID GetParentObjectId(Transform* transform)
        {
            Transform* parent = transform != nullptr ? transform->GetParent() : nullptr;
            GameObject* parent_object = parent != nullptr ? parent->GetParentObject() : nullptr;
            return parent_object != nullptr ? parent_object->GetID() : k_invalid_gobject_id;
        }

        // Root first, then depth-first through the Transform children that are live
        // objects of `level`. A child that is not in the level (already deleted by an
        // older path) is skipped, not followed.
        void CollectSubtree(Level* level, const std::shared_ptr<GameObject>& root,
                            std::vector<std::shared_ptr<GameObject>>& out)
        {
            std::unordered_set<GObjectID> visited;
            std::vector<std::shared_ptr<GameObject>> stack {root};
            while (!stack.empty())
            {
                std::shared_ptr<GameObject> object = std::move(stack.back());
                stack.pop_back();
                if (object == nullptr || !visited.insert(object->GetID()).second)
                {
                    continue;
                }
                out.push_back(object);

                Transform* transform = object->tryGetComponent(Transform);
                if (transform == nullptr)
                {
                    continue;
                }
                // Push in reverse so children come out in sibling order.
                for (int child_index = transform->GetChildrenCount() - 1; child_index >= 0; --child_index)
                {
                    Transform* child = transform->GetChild(child_index);
                    GameObject* child_object = child != nullptr ? child->GetParentObject() : nullptr;
                    if (child_object == nullptr)
                    {
                        continue;
                    }
                    if (std::shared_ptr<GameObject> live_child = level->GetGObjectByID(child_object->GetID()).lock())
                    {
                        stack.push_back(std::move(live_child));
                    }
                }
            }
        }

        RenderSwapData* GetLogicSwapData()
        {
            RenderSystem* render_system = GET_SYSTEM(RenderSystem);
            return render_system != nullptr ? &render_system->GetSwapContext().GetLogicSwapData() : nullptr;
        }
    }  // namespace

    int32_t EnsureInstanceID(Object* object)
    {
        if (object == nullptr)
        {
            return 0;
        }
        if (object->GetInstanceID() == 0)
        {
            if (ObjectManager* object_manager = GET_SYSTEM(ObjectManager))
            {
                object_manager->AllocateAndAssignInstanceID(object);
            }
        }
        return object->GetInstanceID();
    }

    Level* GetActiveLevel()
    {
        WorldManager* world_manager = GET_SYSTEM(WorldManager);
        return world_manager != nullptr ? world_manager->getCurrentActiveLevel() : nullptr;
    }

    // =================================================================================
    // ComponentSnapshotRecord
    // =================================================================================

    uint64_t ComponentSnapshotRecord::MakeKey(int32_t instance_id)
    {
        return kComponentKeyTag | static_cast<uint32_t>(instance_id);
    }

    std::unique_ptr<ComponentSnapshotRecord> ComponentSnapshotRecord::Create(Component* component)
    {
        const int32_t instance_id = EnsureInstanceID(component);
        if (instance_id == 0)
        {
            return nullptr;
        }

        auto record = std::unique_ptr<ComponentSnapshotRecord>(new ComponentSnapshotRecord());
        record->m_InstanceID = instance_id;
        record->m_Stored = SerializeComponent(component);
        record->m_MemoryBytes = sizeof(ComponentSnapshotRecord) + EstimateDomBytes(*record->m_Stored);
        return record;
    }

    Component* ComponentSnapshotRecord::Resolve() const
    {
        ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
        Object* object = object_manager != nullptr ? object_manager->IDToPointer(m_InstanceID) : nullptr;
        return Cast<Component>(object);
    }

    void ComponentSnapshotRecord::Apply()
    {
        Component* component = Resolve();
        if (component == nullptr || m_Stored == nullptr)
        {
            LOG_WARNING(ZUndo, "component (instance {}) no longer exists; step skipped", m_InstanceID);
            return;
        }

        std::unique_ptr<Document> live = SerializeComponent(component);
        DeserializeComponent(component, *m_Stored);
        m_Stored = std::move(live);
        component->PostEditUndo();
    }

    bool ComponentSnapshotRecord::Finalize()
    {
        Component* component = Resolve();
        if (component == nullptr || m_Stored == nullptr)
        {
            return false;
        }
        // rapidjson's deep equality: member order independent, doubles exact.
        const std::unique_ptr<Document> live = SerializeComponent(component);
        return !(static_cast<const Value&>(*live) == static_cast<const Value&>(*m_Stored));
    }

    // =================================================================================
    // TransformStateRecord
    // =================================================================================

    bool TransformState::operator==(const TransformState& other) const
    {
        return parent_instance_id == other.parent_instance_id && sibling_index == other.sibling_index &&
               local_position == other.local_position && local_rotation == other.local_rotation &&
               local_scale == other.local_scale && enabled == other.enabled;
    }

    uint64_t TransformStateRecord::MakeKey(int32_t instance_id)
    {
        return kTransformKeyTag | static_cast<uint32_t>(instance_id);
    }

    TransformState TransformStateRecord::Capture(Transform* transform)
    {
        TransformState state;
        Transform* parent = transform->GetParent();
        state.parent_instance_id = parent != nullptr ? EnsureInstanceID(parent) : 0;
        state.sibling_index = parent != nullptr ? transform->GetSiblingIndex() : 0;
        state.local_position = transform->GetLocalPositionD();
        state.local_rotation = transform->GetLocalRotation();
        state.local_scale = transform->GetLocalScale();
        state.enabled = transform->isEnabled();
        return state;
    }

    void TransformStateRecord::Restore(Transform* transform, const TransformState& state)
    {
        Transform* parent = nullptr;
        if (state.parent_instance_id != 0)
        {
            ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
            parent = Cast<Transform>(object_manager != nullptr ? object_manager->IDToPointer(state.parent_instance_id)
                                                                        : nullptr);
            if (parent == nullptr)
            {
                LOG_WARNING(ZUndo, "parent transform (instance {}) no longer exists; restoring as a root",
                            state.parent_instance_id);
            }
        }

        // Local stays: the stored local TRS is applied right below, so the world pose
        // ends up exactly where it was (no decompose round-off from world-stays).
        if (transform->GetParent() != parent)
        {
            transform->SetParent(parent, Transform::kLocalPositionStays);
        }
        if (parent != nullptr && transform->GetSiblingIndex() != state.sibling_index)
        {
            transform->SetSiblingIndex(state.sibling_index);
        }

        transform->SetLocalPosition(state.local_position);
        transform->SetLocalRotation(state.local_rotation);
        transform->SetLocalScale(state.local_scale);

        if (transform->isEnabled() != state.enabled)
        {
            // Same path as the Inspector's active toggle: every component re-derives
            // its runtime state (renderers drop / resubmit their entities).
            transform->SetEnabled(state.enabled);
            NotifyAllComponentsUpdated(transform->GetParentObject());
        }
    }

    std::unique_ptr<TransformStateRecord> TransformStateRecord::Create(Transform* transform)
    {
        const int32_t instance_id = EnsureInstanceID(transform);
        if (instance_id == 0)
        {
            return nullptr;
        }
        auto record = std::unique_ptr<TransformStateRecord>(new TransformStateRecord());
        record->m_InstanceID = instance_id;
        record->m_Stored = Capture(transform);
        return record;
    }

    Transform* TransformStateRecord::Resolve() const
    {
        ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
        Object* object = object_manager != nullptr ? object_manager->IDToPointer(m_InstanceID) : nullptr;
        return Cast<Transform>(object);
    }

    void TransformStateRecord::Apply()
    {
        Transform* transform = Resolve();
        if (transform == nullptr)
        {
            LOG_WARNING(ZUndo, "transform (instance {}) no longer exists; step skipped", m_InstanceID);
            return;
        }
        const TransformState live = Capture(transform);
        Restore(transform, m_Stored);
        m_Stored = live;
    }

    bool TransformStateRecord::Finalize()
    {
        Transform* transform = Resolve();
        return transform != nullptr && !(Capture(transform) == m_Stored);
    }

    // =================================================================================
    // GameObjectLifetimeRecord
    // =================================================================================

    GameObjectLifetimeRecord::GameObjectLifetimeRecord(GObjectID root_id)
        : m_RootId(root_id)
    {
    }

    size_t GameObjectLifetimeRecord::GetMemoryBytes() const
    {
        // The objects themselves are what a removed record keeps alive; count them
        // coarsely so a history full of big deletes still hits the memory budget.
        constexpr size_t kApproxBytesPerObject = 2048;
        return sizeof(*this) + m_Subtree.size() * kApproxBytesPerObject;
    }

    size_t GameObjectLifetimeRecord::Remove()
    {
        Level* level = GetActiveLevel();
        if (level == nullptr || !m_InLevel)
        {
            return 0;
        }
        std::shared_ptr<GameObject> root = level->GetGObjectByID(m_RootId).lock();
        if (root == nullptr)
        {
            return 0;
        }

        m_Subtree.clear();
        CollectSubtree(level, root, m_Subtree);

        Transform* root_transform = root->tryGetComponent(Transform);
        m_ParentId = GetParentObjectId(root_transform);
        m_SiblingIndex = 0;
        if (root_transform != nullptr)
        {
            if (root_transform->GetParent() != nullptr)
            {
                // Detach so the parent's child list / hierarchy buffer no longer
                // contains the deleted subtree (previously it kept pointing at it).
                m_SiblingIndex = root_transform->GetSiblingIndex();
                root_transform->SetParent(nullptr, Transform::kLocalPositionStays);
            }
            if (IsLinkedSceneRoot(level, root_transform))
            {
                level->GetTransformSceneRoots().OnTransformLeftRoot(root_transform);
            }
        }

        RenderSwapData* swap_data = GetLogicSwapData();
        for (const std::shared_ptr<GameObject>& object : m_Subtree)
        {
            // Out of the change dispatch: SetParent above queued changes for this
            // hierarchy, and the renderer's dispatch callback would otherwise
            // resubmit (resurrect) the entities of objects that are being deleted.
            if (Transform* transform = object->tryGetComponent(Transform))
            {
                transform->SetDispatchInterestedAll(false);
            }
            level->DeleteGObjectByID(object->GetID());
            if (swap_data != nullptr)
            {
                swap_data->AddDeleteGameObject(GameObjectDesc {object->GetID(), {}});
            }
        }

        m_InLevel = false;
        return m_Subtree.size();
    }

    bool GameObjectLifetimeRecord::Restore()
    {
        Level* level = GetActiveLevel();
        if (level == nullptr || m_InLevel || m_Subtree.empty())
        {
            return false;
        }

        for (const std::shared_ptr<GameObject>& object : m_Subtree)
        {
            if (!level->RestoreGObject(object))
            {
                LOG_ERROR(ZUndo, "could not restore '{}' (id {})", object->GetName().c_str(), object->GetID());
            }
        }

        const std::shared_ptr<GameObject>& root = m_Subtree.front();
        if (Transform* root_transform = root->tryGetComponent(Transform))
        {
            // Registered as a root first: SetParent below unregisters it again, and
            // a transform that leaves the root list must be on it.
            level->GetTransformSceneRoots().OnTransformBecameRoot(root_transform);

            if (m_ParentId != k_invalid_gobject_id)
            {
                std::shared_ptr<GameObject> parent = level->GetGObjectByID(m_ParentId).lock();
                Transform* parent_transform = parent != nullptr ? parent->tryGetComponent(Transform) : nullptr;
                if (parent_transform != nullptr)
                {
                    root_transform->SetParent(parent_transform, Transform::kLocalPositionStays);
                    root_transform->SetSiblingIndex(m_SiblingIndex);
                }
                else
                {
                    LOG_WARNING(ZUndo, "parent {} of '{}' is gone; restored as a root", m_ParentId,
                                root->GetName().c_str());
                }
            }
        }

        RenderSwapData* swap_data = GetLogicSwapData();
        for (const std::shared_ptr<GameObject>& object : m_Subtree)
        {
            if (swap_data != nullptr)
            {
                swap_data->CancelDeleteGameObject(object->GetID());
            }
            if (Transform* transform = object->tryGetComponent(Transform))
            {
                transform->SetDispatchInterestedAll(true);  // engine default
            }
            // Re-derive runtime state from the (unchanged) serialized fields: renderers
            // resubmit their entities, cameras / volumes re-register.
            for (const ImmediatePtr<Component>& component_ptr : object->getComponents())
            {
                Component* component = component_ptr;
                if (component != nullptr && Cast<Transform>(component) == nullptr)
                {
                    component->OnSerializedFieldsUpdated();
                }
            }
        }

        m_Subtree.clear();
        m_InLevel = true;
        return true;
    }

    void GameObjectLifetimeRecord::Apply()
    {
        if (m_InLevel)
        {
            Remove();
        }
        else
        {
            Restore();
        }
    }

    // =================================================================================
    // ComponentLifetimeRecord
    // =================================================================================

    ComponentLifetimeRecord::ComponentLifetimeRecord(Component* component, GObjectID host_id)
        : m_Component(component)
        , m_HostId(host_id)
        , m_Attached(component != nullptr)
    {
    }

    ComponentLifetimeRecord::~ComponentLifetimeRecord()
    {
        // Only a DETACHED component is ours: while it is attached its lifetime belongs
        // to the object graph / serialization driver, exactly like every other
        // component in the scene (GameObject::~GameObject does not delete its
        // components either). Deleting on the attached side would leave the host with
        // a dangling ImmediatePtr.
        if (m_Component != nullptr && !m_Attached)
        {
            if (ObjectManager* object_manager = GET_SYSTEM(ObjectManager))
            {
                object_manager->Destroy(m_Component);
            }
        }
        m_Component = nullptr;
    }

    GameObject* ComponentLifetimeRecord::ResolveHost() const
    {
        Level* level = GetActiveLevel();
        if (level == nullptr || m_HostId == k_invalid_gobject_id)
        {
            return nullptr;
        }
        return level->GetGObjectByID(m_HostId).lock().get();
    }

    void ComponentLifetimeRecord::Detach()
    {
        GameObject* host = ResolveHost();
        if (host == nullptr || m_Component == nullptr)
        {
            // Nothing we can address anymore: drop the component so Finalize() reports
            // the record as a no-op and it is discarded without touching anything.
            m_Component = nullptr;
            m_Attached = false;
            return;
        }

        auto& components = host->EditorGetComponentsRef();
        for (size_t i = 0; i < components.size(); ++i)
        {
            if (static_cast<Component*>(components[i]) == m_Component)
            {
                m_Index = i;
                components.erase(components.begin() + static_cast<std::ptrdiff_t>(i));
                m_Attached = false;
                return;
            }
        }

        // Not on the host (already removed by someone else): not ours to own.
        m_Component = nullptr;
        m_Attached = false;
    }

    void ComponentLifetimeRecord::Attach()
    {
        GameObject* host = ResolveHost();
        if (host == nullptr || m_Component == nullptr)
        {
            return;
        }

        auto& components = host->EditorGetComponentsRef();
        for (const auto& slot : components)
        {
            if (static_cast<Component*>(slot) == m_Component)
            {
                m_Attached = true;  // already there; do not duplicate the slot
                return;
            }
        }

        const size_t index = std::min(m_Index, components.size());
        components.insert(components.begin() + static_cast<std::ptrdiff_t>(index), m_Component);
        m_Attached = true;

        // m_ParentObject is deliberately NOT re-published through PostLoadResource:
        // detaching never cleared it, and re-running it would re-register the
        // component with its subsystem (audio voice, physics body, script binding)
        // a second time.
    }

    size_t ComponentLifetimeRecord::Remove()
    {
        if (m_Component == nullptr || !m_Attached)
        {
            return 0;
        }
        Detach();
        return m_Attached ? 0 : 1;
    }

    void ComponentLifetimeRecord::Apply()
    {
        if (m_Component == nullptr)
        {
            return;
        }
        if (m_Attached)
        {
            Detach();
        }
        else
        {
            Attach();
        }
    }

    // =================================================================================
    // SelectionContext
    // =================================================================================

    SelectionContext::Snapshot SelectionContext::Capture()
    {
        Snapshot snapshot;
        if (EditorSceneManager* scene_manager = GET_SYSTEM(EditorSceneManager))
        {
            snapshot.ids = scene_manager->GetSelectedObjectIDs();
            snapshot.active = scene_manager->getSelectedObjectID();
        }
        return snapshot;
    }

    void SelectionContext::Restore(const Snapshot& snapshot)
    {
        if (EditorSceneManager* scene_manager = GET_SYSTEM(EditorSceneManager))
        {
            scene_manager->SetGObjectSelection(snapshot.ids, snapshot.active);
        }
    }

    void SelectionContext::CaptureBefore() { m_Before = Capture(); }
    void SelectionContext::CaptureAfter() { m_After = Capture(); }
    void SelectionContext::RestoreBefore() { Restore(m_Before); }
    void SelectionContext::RestoreAfter() { Restore(m_After); }
}  // namespace EditorUndoRecords
