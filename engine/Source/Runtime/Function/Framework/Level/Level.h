#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class CameraComponent;
class Character;
class Level;
class Object;
class ObjectInstanceRes;
class PhysicsScene;
#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"

using LevelObjectsMap = std::unordered_map<GObjectID, std::shared_ptr<GameObject>>;

/// Extension points on the level object graph. Runtime knows nothing about
/// editor-only types (PrefabInstance lives in the Editor module), so the editor
/// installs these to keep its bookkeeping objects in the same scene file as the
/// GameObjects they refer to. Every hook is optional; a null hook is skipped.
struct LevelSerializationHooks
{
    /// Level::save, after every live GameObject and Component has been collected:
    /// append extra objects to write into the same file. They may refer to the
    /// level's GameObjects / Components by PPtr (live GameObjects carry instance
    /// IDs, see Level::CreateObject).
    void (*collect_extra_objects)(const Level& level, std::vector<Object*>& out) = nullptr;

    /// Level::LoadObjectGraph, after the live objects exist. `entries` is the
    /// whole graph as read from the file. Level builds each live GameObject as a
    /// copy of the one read from the file (components are shared, the GameObject
    /// itself is not), so `file_to_live` maps the instance ID of every GameObject
    /// read from the file to the live GameObject that replaced it -- a PPtr to the
    /// former must be retargeted to the latter.
    void (*on_loaded)(Level& level,
                      const std::vector<std::pair<int64_t, Object*>>& entries,
                      const std::unordered_map<int32_t, GameObject*>& file_to_live) = nullptr;

    /// Level::Unload, before the object table is cleared.
    void (*on_unloading)(Level& level) = nullptr;
};

/// The main class to manage all game objects
class Level
{
public:
    virtual ~Level() {};

    bool load(const eastl::string& level_res_url);

    /// The part of load() after the file has been read: build the live level from
    /// an object graph already in memory (LevelRes header + GameObjects +
    /// Components). load() calls it with create_physics_scene = true; tools and
    /// headless tests that have no physics backend pass false.
    bool LoadObjectGraph(const std::vector<std::pair<int64_t, Object*>>& entries, bool create_physics_scene = true);

    void Unload();

    static void SetSerializationHooks(const LevelSerializationHooks& hooks);
    static const LevelSerializationHooks& GetSerializationHooks();

    bool save();

    bool IsDirty() const { return m_IsDirty; }
    void MarkDirty() { m_IsDirty = true; }
    void ClearDirty() { m_IsDirty = false; }

    void Tick(float delta_time);

    const eastl::string& getLevelResUrl() const { return m_LevelResUrl; }
    void setLevelResUrl(const eastl::string& level_res_url) { m_LevelResUrl = level_res_url; }

    const LevelObjectsMap& getAllGObjects() const { return m_Gobjects; }

    std::weak_ptr<GameObject> GetGObjectByID(GObjectID go_id) const;
    std::weak_ptr<Character> getCurrentActiveCharacter() const { return m_CurrentActiveCharacter; }
    CameraComponent* GetMainCameraComponent() const;
    void SetMainCamera(GObjectID go_id);

    GObjectID CreateObject(const GameObject& object_instance_res);
    void DeleteGObjectByID(GObjectID go_id);

    /// Editor undo: put back a GameObject that DeleteGObjectByID removed, keeping its
    /// original id and its live component instances (UE keeps a deleted actor alive in
    /// the transaction buffer the same way). Fails if the id is already in use. Only
    /// touches the object table -- transform parenting, scene roots and render state
    /// are the caller's job, mirroring DeleteGObjectByID which does not touch them.
    bool RestoreGObject(const std::shared_ptr<GameObject>& gobject);

    /// Changes every time this Level is loaded or unloaded. Editor undo records
    /// refer to objects by id, which is only meaningful within one load; the undo
    /// system drops its history when this value changes under it.
    uint64_t GetLoadSerial() const { return m_LoadSerial; }

    std::weak_ptr<PhysicsScene> getPhysicsScene() const { return m_PhysicsScene; }

    TransformSceneRoots& GetTransformSceneRoots() { return m_TransformSceneRoots; }
    const TransformSceneRoots& GetTransformSceneRoots() const { return m_TransformSceneRoots; }
    void RebuildAllTransformHierarchies();

protected:
    void clear();
    void FlushRenderDeletes();
    void BumpLoadSerial();

    bool m_IsLoaded {false};
    bool m_IsDirty {false};
    uint64_t m_LoadSerial {0};
    eastl::string m_LevelResUrl;

    // all game objects in this level, key: object id, value: object instance
    LevelObjectsMap m_Gobjects;

    std::shared_ptr<Character> m_CurrentActiveCharacter;
    /// Id of the character's object after DeleteGObjectByID unbound it, so
    /// RestoreGObject can rebind it. Invalid when the character is bound.
    GObjectID m_DetachedCharacterObjectId {k_invalid_gobject_id};

    std::weak_ptr<PhysicsScene> m_PhysicsScene;

    TransformSceneRoots m_TransformSceneRoots;
};