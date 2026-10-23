#include "Runtime/Function/Framework/Component/Spine/SpineRenderer.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "core/Log/LogSystem.h"

#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

static_assert(static_cast<int>(SpineBillboardMode::none) == static_cast<int>(SpineGpuBillboardMode::kNone) &&
                  static_cast<int>(SpineBillboardMode::full) == static_cast<int>(SpineGpuBillboardMode::kFull) &&
                  static_cast<int>(SpineBillboardMode::vertical) == static_cast<int>(SpineGpuBillboardMode::kVertical),
              "SpineBillboardMode must mirror SpineGpuBillboardMode");

namespace
{
    std::string ResolveSkeletonPath(const eastl::string& asset_path)
    {
        if (asset_path.empty())
        {
            return {};
        }
        AssetManager* asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return asset_path.c_str();
        }
        // 与 MeshRenderer 同口径：相对路径先找引擎根、再找项目 content 根（AssetManager::GetFullPath）
        const std::filesystem::path full = asset_manager->GetFullPath(asset_path);
        const std::u8string u8 = full.generic_u8string();
        return std::string(u8.begin(), u8.end());
    }

    std::string AnimationArg(const eastl::string& animation)
    {
        return animation.empty() ? std::string("0") : std::string(animation.c_str());
    }

    // 空皮肤 = 只用 default 皮肤 = Skeleton::setSkin(NULL)
    std::string SkinArg(const eastl::string& skin)
    {
        return skin.empty() ? std::string("none") : std::string(skin.c_str());
    }

    struct PendingSpawn
    {
        std::string asset;
        std::string animation;
        Vector3 position;
        float scale {0.01f};
        bool in_front_of_camera {false};
        float yaw {0.0f};   // 绕世界 Z 的转角（弧度）；面向相机时算
    };
    std::mutex g_spawn_mutex;
    std::vector<PendingSpawn> g_pending_spawns;

    // "dir/K_Hero_Spine.skel.bytes" → "K_Hero_Spine"
    std::string ObjectNameFromAsset(const std::string& asset)
    {
        const std::u8string file = std::filesystem::path(std::u8string(asset.begin(), asset.end())).filename().u8string();
        std::string name(file.begin(), file.end());
        for (const char* suffix : {".zspg", ".skel.bytes", ".skel", ".json"})
        {
            const size_t n = std::char_traits<char>::length(suffix);
            if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0)
            {
                name.erase(name.size() - n);
                break;
            }
        }
        return name.empty() ? std::string("Spine") : name;
    }
}  // namespace

GObjectID SpineRenderer::SpawnInLevel(Level* level, const std::string& object_name,
                                      const eastl::string& skeleton_asset, const eastl::string& animation,
                                      const Vector3& position, float scale, float yaw_radians)
{
    ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
    if (level == nullptr || object_manager == nullptr || skeleton_asset.empty())
    {
        return k_invalid_gobject_id;
    }
    GameObject object_template;
    object_template.SetName(object_name.c_str());
    // NewObject（不是 MemoryManager::CreateObject）：组件要有 InstanceID，PPtr 与撤销记录按它找
    Transform* transform = object_manager->NewObject<Transform>();
    object_template.addComponent(transform);
    SpineRenderer* spine = object_manager->NewObject<SpineRenderer>();
    spine->m_SkeletonDataAsset = skeleton_asset;
    spine->m_AnimationName = animation;
    spine->m_Scale = scale;
    object_template.addComponent(spine);

    const GObjectID new_id = level->CreateObject(object_template);
    if (new_id == k_invalid_gobject_id)
    {
        return new_id;
    }
    if (std::shared_ptr<GameObject> created = level->GetGObjectByID(new_id).lock())
    {
        if (Transform* t = created->tryGetComponent(Transform))
        {
            t->SetPosition(position);
            if (yaw_radians != 0.0f)
            {
                t->SetRotation(Quaternion(Radian(yaw_radians), Vector3(0.0f, 0.0f, 1.0f)));
            }
        }
    }
    return new_id;
}

void SpineRenderer::RequestSpawn(const std::string& skeleton_asset, const std::string& animation,
                                 const Vector3& position, float scale)
{
    std::lock_guard<std::mutex> lock(g_spawn_mutex);
    g_pending_spawns.push_back(PendingSpawn {skeleton_asset, animation, position, scale, false});
}

void SpineRenderer::RequestSpawnInFrontOfCamera(const std::string& skeleton_asset, const std::string& animation,
                                                float distance, float scale)
{
    std::lock_guard<std::mutex> lock(g_spawn_mutex);
    g_pending_spawns.push_back(
        PendingSpawn {skeleton_asset, animation, Vector3(distance, 0.0f, 0.0f), scale, true});
}

void SpineRenderer::FlushPendingSpawns(Level* level)
{
    std::vector<PendingSpawn> spawns;
    {
        std::lock_guard<std::mutex> lock(g_spawn_mutex);
        if (g_pending_spawns.empty() || level == nullptr)
        {
            return;
        }
        spawns.swap(g_pending_spawns);
    }
    for (PendingSpawn& sp : spawns)
    {
        if (sp.in_front_of_camera)
        {
            // 场景相机前方 distance 处（position.x 暂存 distance）。相机默认看 +Y，骨架立在 XZ 平面 ⇒ 正对相机
            const float distance = sp.position.x;
            sp.position = Vector3(0.0f, 0.0f, 0.0f);
            RenderSystem* render_system = GET_SYSTEM(RenderSystem);
            const std::shared_ptr<RenderCamera> camera =
                render_system != nullptr ? render_system->GetCamera(ViewportType::scene) : nullptr;
            if (camera != nullptr)
            {
                const Vector3 fwd = camera->forward();
                sp.position = camera->position() + fwd * distance;
                // 骨架立在局部 XZ 平面、法线是局部 ±Y ⇒ 绕 Z 转到"局部 +Y = 相机前向（水平分量）"，正对相机；
                // Rz(θ)·(0,1,0) = (-sinθ, cosθ) ⇒ θ = atan2(-fx, fy)。此时骨架 +x 落在相机的屏幕右方。
                if (std::fabs(fwd.x) + std::fabs(fwd.y) > 1e-4f)
                {
                    sp.yaw = std::atan2(-fwd.x, fwd.y);
                }
            }
        }
        const std::string name = ObjectNameFromAsset(sp.asset);
        const GObjectID id =
            SpawnInLevel(level, name, sp.asset.c_str(), sp.animation.c_str(), sp.position, sp.scale, sp.yaw);
        if (id == k_invalid_gobject_id)
        {
            LOG_ERROR(ZRender, "spine.spawn: could not create a GameObject for '{}'", sp.asset);
            continue;
        }
        LOG_INFO(ZRender, "spine.spawn: GameObject #{} '{}' <- '{}' at ({}, {}, {}) yaw {} deg, scale {}", id, name,
                 sp.asset, sp.position.x, sp.position.y, sp.position.z, sp.yaw * 57.2957795f, sp.scale);
    }
}

SpineRenderer::~SpineRenderer()
{
    ReleaseInstance();
}

void SpineRenderer::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;
    // 实例不在这里建：见头文件类注释（模板 GameObject 上的组件永远不 tick，在这里建会留孤儿）
}

void SpineRenderer::Tick(float delta_time)
{
    SyncInstance(/*allow_create=*/true);
    PushTransform(delta_time);
}

void SpineRenderer::OnSerializedFieldsUpdated()
{
    SyncInstance(/*allow_create=*/false);
    PushTransform(0.0f);
}

void SpineRenderer::SetEnabled(bool enabled)
{
    Component::SetEnabled(enabled);
    PushTransform(0.0f);
}

void SpineRenderer::SetAnimation(int track, const eastl::string& animation, bool loop)
{
    if (track == 0)
    {
        m_AnimationName = animation;
        m_Loop = loop;
        m_AppliedAnimation = animation;
        m_AppliedLoop = loop;
    }
    if (m_InstanceId != 0)
    {
        SpineGpuRenderer::Get().RequestSetAnimationOn(m_InstanceId, track, AnimationArg(animation), loop);
    }
}

void SpineRenderer::SetTimeScale(float time_scale)
{
    m_TimeScale = time_scale;
    SyncInstance(/*allow_create=*/false);
}

void SpineRenderer::SetSkin(const eastl::string& skin)
{
    m_InitialSkinName = skin;
    SyncInstance(/*allow_create=*/false);
}

void SpineRenderer::SetSkeletonDataAsset(const eastl::string& asset_path)
{
    m_SkeletonDataAsset = asset_path;
    SyncInstance(/*allow_create=*/false);
}

void SpineRenderer::ReleaseInstance()
{
    if (m_InstanceId != 0)
    {
        SpineGpuRenderer::Get().RequestRemoveWorldInstance(m_InstanceId);
        m_InstanceId = 0;
    }
    m_AppliedAsset.clear();
}

void SpineRenderer::SyncInstance(bool allow_create)
{
    SpineGpuRenderer& renderer = SpineGpuRenderer::Get();
    if (m_InstanceId != 0 && m_AppliedAsset != m_SkeletonDataAsset)
    {
        ReleaseInstance();   // 换资产 = 重建（下面按 allow_create 决定是否立刻建）
    }
    if (m_InstanceId == 0)
    {
        if (!allow_create || m_SkeletonDataAsset.empty() || m_ParentObject == nullptr)
        {
            return;
        }
        const std::string path = ResolveSkeletonPath(m_SkeletonDataAsset);
        m_InstanceId = renderer.RequestAddWorldInstance(path, AnimationArg(m_AnimationName), m_Loop,
                                                        static_cast<uint64_t>(m_ParentObject->GetID()));
        m_AppliedAsset = m_SkeletonDataAsset;
        m_AppliedAnimation = m_AnimationName;
        m_AppliedLoop = m_Loop;
        // 皮肤与时间缩放在命令队列里排在 add 之后 ⇒ 第一次 apply 之前就生效（spine-unity Initialize 的顺序）
        m_AppliedSkin.clear();
        m_AppliedTimeScale = 1.0f;
        LOG_INFO(ZRender, "SpineRenderer: '{}' -> world instance #{} ('{}')", m_ParentObject->GetName().c_str(),
                 m_InstanceId, path);
    }
    if (m_AppliedAnimation != m_AnimationName || m_AppliedLoop != m_Loop)
    {
        renderer.RequestSetAnimationOn(m_InstanceId, 0, AnimationArg(m_AnimationName), m_Loop);
        m_AppliedAnimation = m_AnimationName;
        m_AppliedLoop = m_Loop;
    }
    if (m_AppliedSkin != m_InitialSkinName)
    {
        renderer.RequestSkinOn(m_InstanceId, SkinArg(m_InitialSkinName), /*setup_slots=*/true);
        m_AppliedSkin = m_InitialSkinName;
    }
    if (m_AppliedTimeScale != m_TimeScale)
    {
        renderer.RequestTimeScaleOn(m_InstanceId, m_TimeScale);
        m_AppliedTimeScale = m_TimeScale;
    }
}

void SpineRenderer::PushTransform(float delta_time)
{
    if (m_InstanceId == 0 || m_ParentObject == nullptr)
    {
        return;
    }
    Matrix4x4 model = Matrix4x4::IDENTITY;
    bool active = true;
    SpineGpuBillboard billboard;
    billboard.mode = static_cast<SpineGpuBillboardMode>(m_Billboard);
    billboard.scale_x = m_Scale;
    billboard.scale_y = m_Scale;
    if (Transform* transform = m_ParentObject->tryGetComponent(Transform))
    {
        model = transform->GetLocalToWorldMatrix();
        active = transform->isEnabled();   // Transform::m_Enabled = GameObject 激活标志
        const Vector3 position = transform->GetPosition();
        const Vector3 lossy = transform->GetLossyScale();
        billboard.position[0] = position.x;
        billboard.position[1] = position.y;
        billboard.position[2] = position.z;
        // 骨架 x ↔ 局部 X、骨架 y ↔ 局部 Z（与非公告板的朝向一致），带符号 ⇒ scale.x = -1 照样水平镜像
        billboard.scale_x = m_Scale * lossy.x;
        billboard.scale_y = m_Scale * lossy.z;
    }
    // 骨架空间 → GameObject 局部空间：L2W · Rx(+90°) · diag(s, s, s)。
    // 本引擎 Z 向上（RenderCamera::m_UpAxis = Z），同 UE Paper2D：骨架 x → 局部 +X、骨架 y → 局部 +Z，
    // 骨架立在局部 XZ 平面上（从 ±Y 看；管线不剔背面，两面都看得见）。不转的话骨架平躺在地面上。
    // 列向量约定 ⇒ 新列 = (s·c0, s·c2, -s·c1, c3)。
    const float s = m_Scale;
    for (int r = 0; r < 4; ++r)
    {
        const float c0 = model.m_Mat[r][0];
        const float c1 = model.m_Mat[r][1];
        const float c2 = model.m_Mat[r][2];
        model.m_Mat[r][0] = s * c0;
        model.m_Mat[r][1] = s * c2;
        model.m_Mat[r][2] = -s * c1;
    }
    const bool visible = active && isEnabled();
    SpineGpuRenderer& renderer = SpineGpuRenderer::Get();
    if (!renderer.RequestSetWorldTransform(m_InstanceId, &model.m_Mat[0][0], delta_time, visible, billboard))
    {
        // 渲染器已不认识这个 id：所属对象被删过（RenderScene::DeleteEntityByGObjectID），现在又在 tick ⇒
        // 撤销复活了。丢掉旧 id 重建（不发 remove：渲染器那边已经删了）。
        m_InstanceId = 0;
        m_AppliedAsset.clear();
        SyncInstance(/*allow_create=*/true);
        if (m_InstanceId != 0)
        {
            renderer.RequestSetWorldTransform(m_InstanceId, &model.m_Mat[0][0], delta_time, visible, billboard);
        }
    }
}
