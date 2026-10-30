#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector3.h"

#include <memory>
#include <vector>

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
class Renderer;
class Font;

namespace JPH
{
    class DebugRenderer;
}
#endif

class PhysicsScene;

class PhysicsManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "PhysicsManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }

    bool Initialize() override;
    void Shutdown() override;

    std::weak_ptr<PhysicsScene> CreatePhysicsScene(const Vector3& gravity);
    void DeletePhysicsScene(std::weak_ptr<PhysicsScene> physics_scene);

    // --- scene lookup -------------------------------------------------------
    //
    // `Physics` (the script facade) needs a scene to query and there was no way
    // to get one: the vector was private and every caller so far went through
    // WorldManager's active-level weak_ptr. A headless test has no WorldManager,
    // so the facade falls back to scene 0 here.

    size_t GetPhysicsSceneCount() const { return m_Scenes.size(); }

    /// @return nullptr when `index` is out of range.
    std::shared_ptr<PhysicsScene> GetPhysicsScene(size_t index) const;

    /// The scene the static `Physics` facade queries: the first one created,
    /// which at runtime is the active level's.
    std::shared_ptr<PhysicsScene> GetPrimaryPhysicsScene() const;

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    void RenderPhysicsWorld(float delta_time);
#endif

protected:
    std::vector<std::shared_ptr<PhysicsScene>> m_Scenes;

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    Renderer* m_Renderer {nullptr};
    Font* m_Font {nullptr};

    JPH::DebugRenderer* m_DebugRenderer {nullptr};
#endif
};