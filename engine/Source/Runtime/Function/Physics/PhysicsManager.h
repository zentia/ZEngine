#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector3.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
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
class GameObject;

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

    // --- body -> GameObject -----------------------------------------------
    //
    // Jolt 的接触事件只给 body id（一个 uint32），而脚本要的是"撞到哪个
    // GameObject"。没有这张表，`OnCollisionEnter(other)` 就无从填 `other`。
    //
    // 放在这里而不是 PhysicsScene 里：scene 是纯物理层，不该知道 GameObject
    // 的存在（Web / slim 构建连 Jolt 都没有）。由 RigidBodyComponent 在
    // 建/删 body 的那一刻维护。

    /// `owner` 为 null 等于注销。重复注册同一个 id 会覆盖（body id 会被 Jolt
    /// 复用，覆盖比"留着指向已销毁对象的野指针"安全得多）。
    void RegisterBodyOwner(uint32_t body_id, GameObject* owner);
    void UnregisterBodyOwner(uint32_t body_id);

    /// @return nullptr when the body has no owner.
    GameObject* FindBodyOwner(uint32_t body_id) const;

    /// 排空每个场景的触发器 / 碰撞事件并派发给脚本。
    ///
    /// **每帧只能由一个地方调一次** —— 队列是被排空的，调两次第二次什么都
    /// 拿不到，表现为"OnCollisionEnter 偶尔不触发"。调用点在
    /// `Application::LogicalTick` 的固定步循环之后（物理在 FixedTick 里推进，
    /// 事件要等它跑完才有）。
    void DispatchContactEvents();

private:
    /// 给两个对象各发一次（Unity 的碰撞/触发回调在两边都调）。任一为空就跳过。
    void DeliverToBoth(GameObject* object_a, GameObject* object_b, const char* method_name);

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    void RenderPhysicsWorld(float delta_time);
#endif

protected:
    std::vector<std::shared_ptr<PhysicsScene>> m_Scenes;

    /// body id -> 拥有它的 GameObject。不持有所有权（组件自己管生命周期，
    /// 注销在它析构前发生）。
    std::unordered_map<uint32_t, GameObject*> m_BodyOwners;

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    Renderer* m_Renderer {nullptr};
    Font* m_Font {nullptr};

    JPH::DebugRenderer* m_DebugRenderer {nullptr};
#endif
};