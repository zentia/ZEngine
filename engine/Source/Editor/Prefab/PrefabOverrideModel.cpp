#include "Editor/Prefab/PrefabOverrideModel.h"

#include "Editor/Prefab/PrefabInstance.h"
#include "Editor/Prefab/PrefabSceneBinding.h"
#include "Editor/Prefab/PrefabUtilityEditor.h"
#include "Runtime/Resource/Prefab/PropertyPathResolver.h"
#include "Runtime/Resource/Prefab/PropertyValueLocator.h"
#include "Editor/Undo/UndoSystem.h"
#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_set>

namespace
{
    struct Snapshot
    {
        prefab::SerialisedObject tree;
        std::vector<uint8_t> bytes;
    };

    bool TakeSnapshot(Object& obj, Snapshot& out)
    {
        if (!prefab::SerialiseObject(&obj, out.tree))
            return false;
        out.bytes.clear();
        TransferUtility::WriteObjectToVector(obj, out.bytes);
        return !out.bytes.empty();
    }

    Object* SourceOf(const PrefabInstance* instance, Object* target)
    {
        if (instance == nullptr || target == nullptr)
            return nullptr;
        return static_cast<Object*>(instance->FindSourceForInstance(PPtr<Object>(target)));
    }

    /// Leaf paths under `path` in `snap` (the path itself when it is a fixed-size leaf).
    /// Arrays and variable-size leaves are not overridable through this pipeline.
    void CollectLeaves(const Snapshot& snap, const eastl::string& path, std::vector<eastl::string>& out, int depth = 0)
    {
        prefab::PropertyLocation loc;
        if (depth > 8 || !prefab::LocatePropertyInStream(snap.tree.type_tree, snap.bytes, path, loc) || !loc.valid())
            return;
        if (loc.leaf_node->IsArray())
            return;
        const TypeTreeIterator first = loc.leaf_node.Children();
        if (first.IsNull())
        {
            if (loc.byte_size > 0)
                out.push_back(path);
            return;
        }
        for (TypeTreeIterator child = first; !child.IsNull(); child = child.Next())
        {
            eastl::string child_path = path;
            child_path += ".";
            child_path += child.Name().c_str();
            CollectLeaves(snap, child_path, out, depth + 1);
        }
    }

    bool SameLeafValue(const Snapshot& a, const Snapshot& b, const eastl::string& path)
    {
        prefab::PropertyLocation la;
        prefab::PropertyLocation lb;
        if (!prefab::LocatePropertyInStream(a.tree.type_tree, a.bytes, path, la) || !la.valid())
            return false;
        if (!prefab::LocatePropertyInStream(b.tree.type_tree, b.bytes, path, lb) || !lb.valid())
            return false;
        return la.byte_size == lb.byte_size &&
               std::memcmp(a.bytes.data() + la.byte_offset, b.bytes.data() + lb.byte_offset,
                           static_cast<size_t>(la.byte_size)) == 0;
    }

    void EraseExact(PrefabInstance* instance, const Object* target, const eastl::string& path)
    {
        auto& mods = instance->GetModifications();
        const int32_t id = target->GetInstanceID();
        mods.erase(std::remove_if(mods.begin(), mods.end(),
                                  [&](const PropertyModification& m) {
                                      return m.target.GetInstanceID() == id && m.property_path == path;
                                  }),
                   mods.end());
    }

    int CountUnder(const PrefabInstance* instance, const Object* target, const eastl::string& path)
    {
        int n = 0;
        for (const PropertyModification& m : instance->GetModifications())
        {
            if (m.target.GetInstanceID() == target->GetInstanceID() && PrefabOverrideModel::PathCovers(path, m.property_path))
                ++n;
        }
        return n;
    }

    /// True when the instance's source prefab is a Variant. Apply then has to record
    /// into the Variant asset (and saves it itself) rather than splice into the
    /// base's shared objects.
    bool SourceIsVariant(const PrefabInstance* instance)
    {
        if (instance == nullptr)
            return false;
        const PrefabAsset* source = static_cast<const PrefabAsset*>(instance->GetSourcePrefab());
        return source != nullptr && source->IsVariant();
    }

    /// Push the instance's current modification list into the source. Variant sources
    /// go through ApplyAllToVariant, which also writes the file; plain prefabs splice
    /// into the source objects and FinishApply writes the file afterwards.
    int ApplyToSource(PrefabInstance* instance, bool& out_already_saved)
    {
        out_already_saved = false;
        if (SourceIsVariant(instance))
        {
            out_already_saved = true;
            return PrefabUtilityEditor::ApplyAllToVariant(instance);
        }
        return PrefabUtilityEditor::ApplyAll(instance);
    }

    PrefabOverrideModel::ApplyReport FinishApply(const Level* level, PrefabInstance* instance, int applied,
                                                 bool already_saved = false)
    {
        PrefabOverrideModel::ApplyReport report;
        report.applied = applied;
        if (applied <= 0)
            return report;
        report.saved = already_saved ? true : PrefabOverrideModel::SaveSourcePrefab(instance);
        if (level != nullptr)
        {
            const int32_t source_id = instance->GetSourcePrefab().GetInstanceID();
            for (PrefabInstance* other : PrefabSceneBinding::GetInstances(*level))
            {
                if (other != nullptr && other != instance && other->GetSourcePrefab().GetInstanceID() == source_id)
                {
                    PrefabUtilityEditor::MergeFromSource(other);
                    ++report.merged;
                }
            }
        }
        return report;
    }
}  // namespace

namespace PrefabOverrideModel
{
    PrefabInstance* FindInstance(const Level* level, const GameObject* game_object)
    {
        if (level == nullptr || game_object == nullptr)
            return nullptr;
        return PrefabSceneBinding::FindInstanceForGameObject(*level, game_object);
    }

    bool PathCovers(const eastl::string& outer, const eastl::string& inner)
    {
        if (outer.empty() || inner.size() < outer.size() || inner.compare(0, outer.size(), outer) != 0)
            return false;
        return inner.size() == outer.size() || inner[outer.size()] == '.' || inner[outer.size()] == '[';
    }

    bool IsOverridden(const PrefabInstance* instance, const Object* target, const eastl::string& path)
    {
        if (instance == nullptr || target == nullptr)
            return false;
        for (const PropertyModification& m : instance->GetModifications())
        {
            if (m.target.GetInstanceID() == target->GetInstanceID() &&
                (PathCovers(path, m.property_path) || PathCovers(m.property_path, path)))
                return true;
        }
        return false;
    }

    std::vector<OverrideRow> ListOverrides(const PrefabInstance* instance)
    {
        std::vector<OverrideRow> rows;
        if (instance == nullptr)
            return rows;
        for (const PropertyModification& m : instance->GetModifications())
        {
            OverrideRow row;
            row.target = m.target;
            row.property_path = m.property_path;
            Object* obj = static_cast<Object*>(m.target);
            if (GameObject* go = Cast<GameObject>(obj))
            {
                row.label = go->GetName();
            }
            else if (Component* c = Cast<Component>(obj))
            {
                row.label = c->GetParentObject() != nullptr ? c->GetParentObject()->GetName() : eastl::string("?");
                row.label += " / ";
                row.label += c->GetTypeName();
            }
            else
            {
                row.label = obj != nullptr ? obj->GetTypeName() : "<missing>";
            }
            rows.push_back(row);
        }
        return rows;
    }

    int SyncFieldOverride(PrefabInstance* instance, Object* target, const eastl::string& path)
    {
        Object* source = SourceOf(instance, target);
        if (source == nullptr || path.empty())
            return -1;
        Snapshot inst_snap;
        Snapshot src_snap;
        if (!TakeSnapshot(*target, inst_snap) || !TakeSnapshot(*source, src_snap))
            return -1;

        std::vector<eastl::string> leaves;
        CollectLeaves(inst_snap, path, leaves);
        if (leaves.empty())
            return -1;
        // A compound override recorded earlier is replaced by per-leaf ones.
        if (leaves.size() != 1 || leaves[0] != path)
            EraseExact(instance, target, path);
        int active = 0;
        for (const eastl::string& leaf : leaves)
        {
            if (SameLeafValue(inst_snap, src_snap, leaf))
            {
                EraseExact(instance, target, leaf);
            }
            else if (PrefabUtilityEditor::RecordCurrentValueAsOverride(instance, PPtr<Object>(target), leaf))
            {
                ++active;
            }
        }
        return active;
    }

    int RevertField(PrefabInstance* instance, Object* target, const eastl::string& path)
    {
        if (instance == nullptr || target == nullptr)
            return 0;
        std::vector<eastl::string> to_revert;
        for (const PropertyModification& m : instance->GetModifications())
        {
            if (m.target.GetInstanceID() == target->GetInstanceID() && PathCovers(path, m.property_path))
                to_revert.push_back(m.property_path);
        }
        int reverted = 0;
        for (const eastl::string& p : to_revert)
        {
            if (PrefabUtilityEditor::RevertSingleOverride(instance, PPtr<Object>(target), p))
                ++reverted;
        }
        return reverted;
    }

    ApplyReport ApplyField(const Level* level, PrefabInstance* instance, Object* target, const eastl::string& path)
    {
        if (instance == nullptr || target == nullptr)
            return {};
        std::vector<PropertyModification> selected;
        std::vector<PropertyModification> rest;
        for (const PropertyModification& m : instance->GetModifications())
        {
            const bool match =
                m.target.GetInstanceID() == target->GetInstanceID() && PathCovers(path, m.property_path);
            (match ? selected : rest).push_back(m);
        }
        if (selected.empty())
            return {};

        // PrefabUtilityEditor::ApplyAll applies the whole list: hand it only the
        // selected overrides, then put the others back.
        instance->ClearModifications();
        for (const PropertyModification& m : selected)
            instance->AddModification(m);
        bool already_saved = false;
        const int applied = ApplyToSource(instance, already_saved);
        if (applied <= 0)
        {
            instance->ClearModifications();
            for (const PropertyModification& m : selected)
                instance->AddModification(m);
        }
        for (const PropertyModification& m : rest)
            instance->AddModification(m);
        return FinishApply(level, instance, applied, already_saved);
    }

    ApplyReport ApplyAll(const Level* level, PrefabInstance* instance)
    {
        if (instance == nullptr || instance->GetModifications().empty())
            return {};
        bool already_saved = false;
        const int applied = ApplyToSource(instance, already_saved);
        return FinishApply(level, instance, applied, already_saved);
    }

    namespace
    {
        bool SameModification(const PropertyModification& a, const PropertyModification& b)
        {
            return a.target.GetInstanceID() == b.target.GetInstanceID() && a.property_path == b.property_path &&
                   a.value_bytes == b.value_bytes && a.object_reference.GetInstanceID() == b.object_reference.GetInstanceID();
        }

        class OverrideListRecord final : public IUndoRecord
        {
        public:
            explicit OverrideListRecord(PrefabInstance* instance)
                : m_Instance(instance), m_Stored(instance->GetModifications())
            {
            }

            void Apply() override
            {
                if (PrefabInstance* instance = static_cast<PrefabInstance*>(m_Instance))
                    std::swap(instance->GetModifications(), m_Stored);
            }

            bool Finalize() override
            {
                const PrefabInstance* instance = static_cast<PrefabInstance*>(m_Instance);
                if (instance == nullptr)
                    return false;
                const auto& live = instance->GetModifications();
                return !(live.size() == m_Stored.size() &&
                         std::equal(live.begin(), live.end(), m_Stored.begin(), SameModification));
            }

            uint64_t GetCoalesceKey() const override { return Key(m_Instance.GetInstanceID()); }
            size_t GetMemoryBytes() const override
            {
                return sizeof(*this) + m_Stored.size() * (sizeof(PropertyModification) + 32);
            }

            static uint64_t Key(int32_t id)
            {
                // Distinct from the Component / Transform record keys (their high bits).
                return (uint64_t {0x50524546u} << 32) | static_cast<uint32_t>(id);
            }

        private:
            PPtr<PrefabInstance> m_Instance;
            std::vector<PropertyModification> m_Stored;
        };
    }  // namespace

    std::unique_ptr<IUndoRecord> MakeOverrideListUndoRecord(PrefabInstance* instance)
    {
        if (instance == nullptr)
            return nullptr;
        return std::make_unique<OverrideListRecord>(instance);
    }

    uint64_t OverrideListUndoKey(const PrefabInstance* instance)
    {
        return instance != nullptr ? OverrideListRecord::Key(instance->GetInstanceID()) : 0;
    }

    bool SaveSourcePrefab(PrefabInstance* instance)
    {
        if (instance == nullptr)
            return false;
        PrefabAsset* asset = static_cast<PrefabAsset*>(instance->GetSourcePrefab());
        AssetManager* am = GET_SYSTEM(AssetManager);
        if (asset == nullptr || am == nullptr)
            return false;

        std::filesystem::path path;
        int64_t header_file_id = 0;
        if (!am->TryGetIdentityForInstance(asset->GetInstanceID(), path, header_file_id) || path.empty())
        {
            LOG_WARNING(ZPrefab, "SaveSourcePrefab: the source prefab has no file identity; not saved");
            return false;
        }

        // Every object of the source graph the instance knows about, under the fileID
        // it already has in that file: a scene's reference into the prefab is (file,
        // fileID), so renumbering on save would silently retarget it.
        // The graph is the source subtree (GameObjects, their components, child
        // Transforms' GameObjects) -- what SaveAsPrefabAsset flattens -- plus anything
        // else the corresponding-source map names.
        std::vector<Object*> graph;
        {
            std::vector<GameObject*> pending;
            if (GameObject* root = asset->GetRootGameObject())
                pending.push_back(root);
            while (!pending.empty())
            {
                GameObject* go = pending.back();
                pending.pop_back();
                graph.push_back(go);
                Transform* tc = nullptr;
                for (ImmediatePtr<Component>& c : go->getComponents())
                {
                    if (Component* component = c)
                    {
                        graph.push_back(component);
                        if (tc == nullptr)
                            tc = Cast<Transform>(component);
                    }
                }
                if (tc == nullptr)
                    continue;
                for (uint32_t i = 0; i < tc->GetChildCount(); ++i)
                {
                    Transform* child = tc->GetChild(i);
                    if (child != nullptr && child->GetParentObject() != nullptr)
                        pending.push_back(child->GetParentObject());
                }
            }
            for (const PrefabInstance::CorrespondingSourceEntry& e : instance->GetCorrespondingSourceMap())
            {
                if (Object* src = static_cast<Object*>(e.source_object))
                    graph.push_back(src);
            }
        }

        std::vector<Object*> objects {asset};
        std::vector<int64_t> ids {header_file_id};
        std::unordered_set<int32_t> seen {asset->GetInstanceID()};
        for (Object* src : graph)
        {
            if (!seen.insert(src->GetInstanceID()).second)
                continue;
            std::filesystem::path src_path;
            int64_t file_id = 0;
            // Same file = the ID that file's (path, fileID) mints is this object's.
            if (!am->TryGetIdentityForInstance(src->GetInstanceID(), src_path, file_id) ||
                am->GetInstanceIDFromPathAndFileID(path, file_id) != src->GetInstanceID())
            {
                LOG_WARNING(ZPrefab, "SaveSourcePrefab: a source object is not in {}; not saved", path.generic_string());
                return false;
            }
            objects.push_back(src);
            ids.push_back(file_id);
        }

        const bool yaml = path.extension() == ".prefab";
        const bool ok = yaml ? am->WriteObjectsToYaml(path, objects.data(), ids.data(), objects.size())
                             : am->WriteObjectsToDiskThreadSafe(path, objects.data(), ids.data(), objects.size());
        if (!ok)
            LOG_WARNING(ZPrefab, "SaveSourcePrefab: writing {} failed", path.generic_string());
        return ok;
    }
}  // namespace PrefabOverrideModel
