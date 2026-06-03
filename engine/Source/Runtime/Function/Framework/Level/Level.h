#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <memory>
#include <unordered_map>

class CameraComponent;
class Character;
class ObjectInstanceRes;
class PhysicsScene;

using LevelObjectsMap = std::unordered_map<GObjectID, std::shared_ptr<GameObject>>;

/// The main class to manage all game objects
class Level
{
public:
    virtual ~Level() {};

    bool load(const eastl::string& level_res_url);
    void Unload();

    bool save();

    void Tick(float delta_time);

    const eastl::string& getLevelResUrl() const { return m_LevelResUrl; }

    const LevelObjectsMap& getAllGObjects() const { return m_Gobjects; }

    std::weak_ptr<GameObject> GetGObjectByID(GObjectID go_id) const;
    std::weak_ptr<Character> getCurrentActiveCharacter() const { return m_CurrentActiveCharacter; }
    CameraComponent* GetMainCameraComponent() const;
    void SetMainCamera(GObjectID go_id);

    GObjectID CreateObject(const GameObject& object_instance_res);
    void DeleteGObjectByID(GObjectID go_id);

    std::weak_ptr<PhysicsScene> getPhysicsScene() const { return m_PhysicsScene; }

protected:
    void clear();
    void FlushRenderDeletes();

    bool m_IsLoaded {false};
    eastl::string m_LevelResUrl;

    // all game objects in this level, key: object id, value: object instance
    LevelObjectsMap m_Gobjects;

    std::shared_ptr<Character> m_CurrentActiveCharacter;

    std::weak_ptr<PhysicsScene> m_PhysicsScene;
};