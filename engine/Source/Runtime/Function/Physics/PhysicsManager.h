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