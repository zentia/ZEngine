#include "Editor/Prefab/PrefabSceneBinding.h"

#include "Editor/Prefab/PrefabInstance.h"
#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Level/Level.h"

#include <algorithm>
#include <memory>

namespace
{
    std::unordered_map<const Level*, std::vector<PrefabInstance*>>& Registry()
    {
        static std::unordered_map<const Level*, std::vector<PrefabInstance*>> s_registry;
        return s_registry;
    }

    /// True if `game_object` is one of `level`'s live objects (not merely alive).
    bool IsInLevel(const Level& level, const GameObject* game_object)
    {
        if (game_object == nullptr)
        {
            return false;
        }
        const std::shared_ptr<GameObject> live = level.GetGObjectByID(game_object->GetID()).lock();
        return live.get() == game_object;
    }

    template<class T>
    void Retarget(PPtr<T>& ptr, const std::unordered_map<int32_t, GameObject*>& from_to)
    {
        const int32_t id = ptr.GetInstanceID();
        if (id == 0)
        {
            return;
        }
        const auto it = from_to.find(id);
        if (it != from_to.end() && it->second != nullptr && it->second->GetInstanceID() != 0)
        {
            ptr = PPtr<T>(it->second->GetInstanceID());
        }
    }

    void CollectExtraObjects(const Level& level, std::vector<Object*>& out)
    {
        for (PrefabInstance* instance : PrefabSceneBinding::GetInstancesToSave(level))
        {
            out.push_back(instance);
        }
    }

    void OnLevelLoaded(Level& level,
                       const std::vector<std::pair<int64_t, Object*>>& entries,
                       const std::unordered_map<int32_t, GameObject*>& file_to_live)
    {
        // A reload of the same Level object starts from a clean slate.
        Registry().erase(&level);
        int count = 0;
        for (const auto& entry : entries)
        {
            PrefabInstance* instance = Cast<PrefabInstance>(entry.second);
            if (instance == nullptr)
            {
                continue;
            }
            PrefabSceneBinding::RetargetGameObjects(*instance, file_to_live);
            PrefabSceneBinding::Register(level, instance);
            ++count;
        }
        if (count > 0)
        {
            LOG_INFO(ZPrefab, "PrefabSceneBinding: {} prefab instance(s) bound to level '{}'", count,
                     level.getLevelResUrl().c_str());
        }
    }

    void OnLevelUnloading(Level& level) { Registry().erase(&level); }
}  // namespace

void PrefabSceneBinding::Install()
{
    LevelSerializationHooks hooks = Level::GetSerializationHooks();
    hooks.collect_extra_objects = &CollectExtraObjects;
    hooks.on_loaded = &OnLevelLoaded;
    hooks.on_unloading = &OnLevelUnloading;
    Level::SetSerializationHooks(hooks);
}

void PrefabSceneBinding::Uninstall()
{
    LevelSerializationHooks hooks = Level::GetSerializationHooks();
    if (hooks.collect_extra_objects == &CollectExtraObjects)
        hooks.collect_extra_objects = nullptr;
    if (hooks.on_loaded == &OnLevelLoaded)
        hooks.on_loaded = nullptr;
    if (hooks.on_unloading == &OnLevelUnloading)
        hooks.on_unloading = nullptr;
    Level::SetSerializationHooks(hooks);
    Registry().clear();
}

bool PrefabSceneBinding::PlaceInLevel(Level& level, PrefabInstance* instance)
{
    if (instance == nullptr)
    {
        return false;
    }
    GameObject* clone_root = instance->GetRootGameObject();
    if (clone_root == nullptr)
    {
        LOG_ERROR(ZPrefab, "PlaceInLevel: instance has no root GameObject");
        return false;
    }

    // Every cloned GameObject of the subtree: the root plus the instance side of the
    // corresponding-source map (which covers the whole subtree, components included).
    std::vector<GameObject*> clones;
    clones.push_back(clone_root);
    for (const PrefabInstance::CorrespondingSourceEntry& entry : instance->GetCorrespondingSourceMap())
    {
        GameObject* clone = Cast<GameObject>(static_cast<Object*>(entry.instance_object));
        if (clone != nullptr && std::find(clones.begin(), clones.end(), clone) == clones.end())
        {
            clones.push_back(clone);
        }
    }

    // Same step as Level::LoadObjectGraph: Level copies each GameObject (the
    // components are shared and get re-bound to the live owner by PostLoadResource).
    // A clone's live component list (m_Components) is already populated by the
    // cloner, so unlike a file read no RebuildRuntimeComponents() is needed here.
    std::unordered_map<int32_t, GameObject*> clone_to_live;
    for (GameObject* clone : clones)
    {
        const GObjectID live_id = level.CreateObject(*clone);
        const std::shared_ptr<GameObject> live = level.GetGObjectByID(live_id).lock();
        if (live == nullptr)
        {
            LOG_ERROR(ZPrefab, "PlaceInLevel: Level::CreateObject failed for '{}'", clone->GetName().c_str());
            continue;
        }
        clone_to_live.emplace(clone->GetInstanceID(), live.get());
    }

    RetargetGameObjects(*instance, clone_to_live);
    level.RebuildAllTransformHierarchies();
    level.MarkDirty();
    Register(level, instance);
    return true;
}

void PrefabSceneBinding::Register(const Level& level, PrefabInstance* instance)
{
    if (instance == nullptr)
    {
        return;
    }
    std::vector<PrefabInstance*>& list = Registry()[&level];
    if (std::find(list.begin(), list.end(), instance) == list.end())
    {
        list.push_back(instance);
    }
}

void PrefabSceneBinding::Unregister(PrefabInstance* instance)
{
    for (auto& level_and_list : Registry())
    {
        std::vector<PrefabInstance*>& list = level_and_list.second;
        list.erase(std::remove(list.begin(), list.end(), instance), list.end());
    }
}

std::vector<PrefabInstance*> PrefabSceneBinding::GetInstances(const Level& level)
{
    const auto it = Registry().find(&level);
    return it != Registry().end() ? it->second : std::vector<PrefabInstance*> {};
}

std::vector<PrefabInstance*> PrefabSceneBinding::GetInstancesToSave(const Level& level)
{
    std::vector<PrefabInstance*> out;
    for (PrefabInstance* instance : GetInstances(level))
    {
        if (instance != nullptr && IsInLevel(level, instance->GetRootGameObject()))
        {
            out.push_back(instance);
        }
    }
    return out;
}

PrefabInstance* PrefabSceneBinding::FindInstanceForGameObject(const Level& level, const GameObject* game_object)
{
    if (game_object == nullptr || game_object->GetInstanceID() == 0)
    {
        return nullptr;
    }
    const int32_t needle = game_object->GetInstanceID();
    for (PrefabInstance* instance : GetInstances(level))
    {
        if (instance == nullptr)
            continue;
        if (instance->GetRootGameObject().GetInstanceID() == needle)
            return instance;
        for (const PrefabInstance::CorrespondingSourceEntry& entry : instance->GetCorrespondingSourceMap())
        {
            if (entry.instance_object.GetInstanceID() == needle)
                return instance;
        }
    }
    return nullptr;
}

void PrefabSceneBinding::RetargetGameObjects(PrefabInstance& instance, const std::unordered_map<int32_t, GameObject*>& from_to)
{
    if (from_to.empty())
    {
        return;
    }
    PPtr<GameObject> root = instance.GetRootGameObject();
    Retarget(root, from_to);
    instance.SetRootGameObject(root);

    for (PrefabInstance::CorrespondingSourceEntry& entry : instance.GetCorrespondingSourceMap())
    {
        Retarget(entry.instance_object, from_to);
    }
    for (PropertyModification& modification : instance.GetModifications())
    {
        Retarget(modification.target, from_to);
        Retarget(modification.object_reference, from_to);
    }
}
