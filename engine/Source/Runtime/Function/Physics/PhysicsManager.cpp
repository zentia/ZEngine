#include "Runtime/Function/Physics/PhysicsManager.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/Jolt/Utils.h"
#include "Runtime/Function/Physics/PhysicsScene.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    #include "TestFramework.h"
    #include "TestFramework/Renderer/DebugRendererImp.h"
    #include "TestFramework/Renderer/Font.h"
    #include "TestFramework/Renderer/Renderer.h"
    #include "TestFramework/Utils/Log.h"
#endif

bool PhysicsManager::Initialize()
{
#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    std::shared_ptr<ConfigManager> config_manager = g_runtime_global_context.m_ConfigManager;
    ASSERT(config_manager);

    Trace = TraceImpl;

    m_Renderer = new Renderer;
    m_Renderer->Initialize();

    m_Font = new Font(m_Renderer);
    m_Font->Create("Arial", 24, config_manager->GetJoltPhysicsAssetFolder());

    m_DebugRenderer = new DebugRendererImp(m_Renderer, m_Font, config_manager->GetJoltPhysicsAssetFolder());
#endif
    return true;
}

void PhysicsManager::Shutdown()
{
    m_Scenes.clear();

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    delete m_DebugRenderer;
    m_Font = nullptr;
    delete m_Renderer;
#endif
}

std::weak_ptr<PhysicsScene> PhysicsManager::CreatePhysicsScene(const Vector3& gravity)
{
    std::shared_ptr<PhysicsScene> physics_scene = std::make_shared<PhysicsScene>(gravity);

    m_Scenes.push_back(physics_scene);

    return physics_scene;
}

std::shared_ptr<PhysicsScene> PhysicsManager::GetPhysicsScene(size_t index) const
{
    if (index >= m_Scenes.size())
    {
        return nullptr;
    }
    return m_Scenes[index];
}

std::shared_ptr<PhysicsScene> PhysicsManager::GetPrimaryPhysicsScene() const
{
    return m_Scenes.empty() ? nullptr : m_Scenes.front();
}

void PhysicsManager::RegisterBodyOwner(uint32_t body_id, GameObject* owner)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return;
    }
    if (owner == nullptr)
    {
        m_BodyOwners.erase(body_id);
        return;
    }
    m_BodyOwners[body_id] = owner;
}

void PhysicsManager::UnregisterBodyOwner(uint32_t body_id)
{
    m_BodyOwners.erase(body_id);
}

GameObject* PhysicsManager::FindBodyOwner(uint32_t body_id) const
{
    const auto found = m_BodyOwners.find(body_id);
    return found != m_BodyOwners.end() ? found->second : nullptr;
}

void PhysicsManager::DispatchContactEvents()
{
    // 两个缓冲提到循环外：每帧都要排空好几个场景，反复分配没有任何收益。
    std::vector<PhysicsTriggerEvent> trigger_events;
    std::vector<PhysicsCollisionEvent> collision_events;

    for (const std::shared_ptr<PhysicsScene>& scene : m_Scenes)
    {
        if (!scene)
        {
            continue;
        }

        scene->PollTriggerEvents(trigger_events);
        for (const PhysicsTriggerEvent& event : trigger_events)
        {
            GameObject* trigger = FindBodyOwner(event.m_TriggerBodyId);
            GameObject* other = FindBodyOwner(event.m_OtherBodyId);

            // Unity 在两个对象上都调：触发器自己拿到 OnTriggerEnter，被触发的
            // 那个（只要有脚本）也拿到。只给一边是常见误解。
            const char* method = "OnTriggerEnter";
            if (event.m_Phase == PhysicsContactPhase::stay)
            {
                method = "OnTriggerStay";
            }
            else if (event.m_Phase == PhysicsContactPhase::exit)
            {
                method = "OnTriggerExit";
            }
            DeliverToBoth(trigger, other, method);
        }

        scene->PollCollisionEvents(collision_events);
        for (const PhysicsCollisionEvent& event : collision_events)
        {
            const char* method = "OnCollisionEnter";
            if (event.m_Phase == PhysicsContactPhase::stay)
            {
                method = "OnCollisionStay";
            }
            else if (event.m_Phase == PhysicsContactPhase::exit)
            {
                method = "OnCollisionExit";
            }
            DeliverToBoth(FindBodyOwner(event.m_BodyA), FindBodyOwner(event.m_BodyB), method);
        }
    }
}

void PhysicsManager::DeliverToBoth(GameObject* object_a, GameObject* object_b, const char* method_name)
{
    // 没有脚本组件时 SendMessageWithOther 返回 0，这里**不**因此打警告：
    // 场景里绝大多数碰撞体都没有脚本，"没人接"是常态而不是拼写错误。
    if (object_a != nullptr)
    {
        object_a->SendMessageWithOther(method_name, object_a == object_b ? nullptr : object_b);
    }
    if (object_b != nullptr && object_b != object_a)
    {
        object_b->SendMessageWithOther(method_name, object_a);
    }
}

void PhysicsManager::DeletePhysicsScene(std::weak_ptr<PhysicsScene> physics_scene)
{
    std::shared_ptr<PhysicsScene> deleted_scene = physics_scene.lock();

    auto iter = std::find(m_Scenes.begin(), m_Scenes.end(), deleted_scene);
    if (iter != m_Scenes.end())
    {
        m_Scenes.erase(iter);
    }
}

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
void PhysicsManager::RenderPhysicsWorld(float delta_time)
{
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();

    std::shared_ptr<RenderCamera> RenderCamera = GET_SYSTEM(RenderSystem)->GetRenderCamera(ViewportType::scene);
    const Vector2& fov = RenderCamera->getFOV();

    const EngineContentViewport& engine_viewport = GET_SYSTEM(RenderSystem)->getEngineContentViewport();

    HWND window_handle = m_Renderer->GetWindowHandle();
    RECT rc;
    GetClientRect(window_handle, &rc);

    const float current_width = static_cast<float>(rc.right - rc.left);
    const float current_height = static_cast<float>(rc.bottom - rc.top);

    if (!Math::RealEqual(engine_viewport.width, current_width, 1e-1) ||
        !Math::RealEqual(engine_viewport.height, current_height, 1e-1))
    {
        ::SetWindowPos(window_handle,
                       NULL,
                       rc.left,
                       rc.top,
                       engine_viewport.width,
                       engine_viewport.height,
                       SWP_NOMOVE | SWP_NOACTIVATE | SWP_DEFERERASE | SWP_NOOWNERZORDER);
    }

    CameraState world_camera;
    world_camera.mPos = toVec3(RenderCamera->position());
    world_camera.mForward = toVec3(RenderCamera->forward());
    world_camera.mUp = toVec3(RenderCamera->up());
    world_camera.mFOVY = fov.y;
    world_camera.mFarPlane = RenderCamera->m_Zfar;
    world_camera.mNearPlane = RenderCamera->m_Znear;

    m_Renderer->BeginFrame(world_camera, 1.f);

    physics_scene->DrawPhysicsScene(m_DebugRenderer);

    static_cast<DebugRendererImp*>(m_DebugRenderer)->Draw();
    static_cast<DebugRendererImp*>(m_DebugRenderer)->Clear();

    m_Renderer->EndFrame();
}
#endif