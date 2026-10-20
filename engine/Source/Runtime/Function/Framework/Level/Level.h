#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <memory>
#include <unordered_map>

class CameraComponent;
class Character;
class ObjectInstanceRes;
class PhysicsScene;
#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"

using LevelObjectsMap = std::unordered_map<GObjectID, std::shared_ptr<GameObject>>;

/// The main class to manage all game objects
class Level
{
public:
    virtual ~Level() {};

    bool load(const eastl::string& level_res_url);
    void Unload();

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