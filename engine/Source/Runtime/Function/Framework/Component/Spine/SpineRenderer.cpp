#include "Runtime/Function/Framework/Component/Spine/SpineRenderer.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Framework/Component/Script/TypeScriptComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"
#include "Runtime/Function/Render/Spine/SpineGpuAssetFile.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

static_assert(static_cast<int>(SpineBillboardMode::none) == static_cast<int>(SpineGpuBillboardMode::kNone) &&
                  static_cast<int>(SpineBillboardMode::full) == static_cast<int>(SpineGpuBillboardMode::kFull) &&
                  static_cast<int>(SpineBillboardMode::vertical) == static_cast<int>(SpineGpuBillboardMode::kVertical),
              "SpineBillboardMode must mirror SpineGpuBillboardMode");

namespace
{
    std::string FullPathOf(const eastl::string& asset_path)
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
        std::string script;   // 非空 ⇒ 再挂一个 TypeScriptComponent
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
                                      const Vector3& position, float scale, float yaw_radians,
                                      const std::string& script)
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
    spine->SetSkeletonDataAsset(skeleton_asset);   // 同时记下 GUID（还没有父对象 ⇒ 不会建实例）
    spine->m_AnimationName = animation;
    spine->m_Scale = scale;
    object_template.addComponent(spine);
    if (!script.empty())
    {
        ScriptRegistry* registry = GET_SYSTEM(ScriptRegistry);
        ScriptAsset* asset = registry != nullptr ? registry->FindByPath(script.c_str()) : nullptr;
        if (asset == nullptr)
        {
            LOG_ERROR(ZRender, "spine.spawn: script '{}' is not in the ScriptRegistry (project-relative, e.g. Scripts/X.ts)",
                      script);
        }
        else
        {
            TypeScriptComponent* ts = object_manager->NewObject<TypeScriptComponent>();
            ts->SetScriptGuid(asset->m_Guid);
            object_template.addComponent(ts);
        }
    }

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
                                 const Vector3& position, float scale, const std::string& script)
{
    std::lock_guard<std::mutex> lock(g_spawn_mutex);
    g_pending_spawns.push_back(PendingSpawn {skeleton_asset, animation, position, scale, false, 0.0f, script});
}

void SpineRenderer::RequestSpawnInFrontOfCamera(const std::string& skeleton_asset, const std::string& animation,
                                                float distance, float scale, const std::string& script)
{
    std::lock_guard<std::mutex> lock(g_spawn_mutex);
    g_pending_spawns.push_back(
        PendingSpawn {skeleton_asset, animation, Vector3(distance, 0.0f, 0.0f), scale, true, 0.0f, script});
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
            SpawnInLevel(level, name, sp.asset.c_str(), sp.animation.c_str(), sp.position, sp.scale, sp.yaw, sp.script);
        if (id == k_invalid_gobject_id)
        {
            LOG_ERROR(ZRender, "spine.spawn: could not create a GameObject for '{}'", sp.asset);
            continue;
        }
        LOG_INFO(ZRender, "spine.spawn: GameObject #{} '{}' <- '{}' at ({}, {}, {}) yaw {} deg, scale {}", id, name,
                 sp.asset, sp.position.x, sp.position.y, sp.position.z, sp.yaw * 57.2957795f, sp.scale);
    }
}

SpineRenderer::SpineRenderer() = default;

SpineRenderer::~SpineRenderer()
{
    ReleaseInstance();
}

namespace
{
    // NotifyAssetChanged 的记账（只在游戏线程读写）：路径（小写、正斜杠）→ 代数；全局序号让 Tick 在
    // 没有任何变化时只比一个整数
    std::unordered_map<std::string, uint64_t> g_asset_generation;
    uint64_t g_asset_change_serial = 0;

    std::string PathKey(const std::string& absolute_path)
    {
        std::string key = SpineGpuPathToUtf8(std::filesystem::path(SpineGpuPathFromUtf8(absolute_path)).lexically_normal());
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    }

    uint64_t GenerationOf(const std::string& key)
    {
        const auto it = g_asset_generation.find(key);
        return it == g_asset_generation.end() ? 0u : it->second;
    }

    // content 根下的相对路径（与拖放写进字段的形态一致）；不在 content 根下 ⇒ 绝对路径
    eastl::string ContentRelative(const std::filesystem::path& absolute)
    {
        if (ProjectInfo* project = GET_SYSTEM(ProjectInfo))
        {
            const std::filesystem::path root = project->GetProjectContent();
            if (!root.empty())
            {
                std::error_code ec;
                const std::filesystem::path rel = std::filesystem::relative(absolute, root, ec);
                const std::string rel_s = SpineGpuPathToUtf8(rel);
                if (!ec && !rel_s.empty() && rel_s.rfind("..", 0) != 0)
                {
                    return rel_s.c_str();
                }
            }
        }
        return SpineGpuPathToUtf8(absolute).c_str();
    }

    std::string GuidOf(const std::filesystem::path& absolute)
    {
        std::string guid;
        std::string type;
        if (AssetManager* asset_manager = GET_SYSTEM(AssetManager))
        {
            asset_manager->GetAssetGuidAndType(absolute, guid, type);
        }
        return guid;
    }

    // 运行时播放 API 的事件 / 动画参数：空 ⇒ 第 0 个动画
    std::string NameArg(const eastl::string& animation)
    {
        return animation.empty() ? std::string("0") : std::string(animation.c_str());
    }
}  // namespace

SpineRenderer* SpineRenderer::Of(GameObject* game_object)
{
    return game_object != nullptr ? game_object->tryGetComponent(SpineRenderer) : nullptr;
}

void SpineRenderer::NotifyAssetChanged(const std::string& absolute_path)
{
    ++g_asset_generation[PathKey(absolute_path)];
    ++g_asset_change_serial;
}

std::string SpineRenderer::ResolveSkeletonPath()
{
    if (m_SkeletonDataAsset.empty() && m_SkeletonDataGuid.empty())
    {
        return {};
    }
    std::error_code ec;
    if (!m_SkeletonDataAsset.empty())
    {
        const std::filesystem::path full = SpineGpuPathFromUtf8(FullPathOf(m_SkeletonDataAsset));
        if (std::filesystem::is_regular_file(full, ec))
        {
            // 路径有效：顺手记下（或更新）GUID —— 下次文件被挪走时靠它找回来
            if (SpineGpuIsContainerPath(full))
            {
                const std::string guid = GuidOf(full);
                if (!guid.empty() && m_SkeletonDataGuid != guid.c_str())
                {
                    m_SkeletonDataGuid = guid.c_str();
                }
            }
            return SpineGpuPathToUtf8(full);
        }
    }
    // 路径不在了：按 GUID 在 AssetRegistry 里找（编辑器里重命名 / 移动过 .zspg，registry 的条目保留了 GUID）
    if (!m_SkeletonDataGuid.empty())
    {
        std::filesystem::path redirected;
        AssetManager* asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager != nullptr && asset_manager->TryGetPathForGuid(m_SkeletonDataGuid.c_str(), redirected) &&
            std::filesystem::is_regular_file(redirected, ec))
        {
            const eastl::string rel = ContentRelative(redirected);
            LOG_INFO(ZRender, "SpineRenderer: '{}' moved -> '{}' (by GUID {})", m_SkeletonDataAsset.c_str(), rel.c_str(),
                     m_SkeletonDataGuid.c_str());
            m_SkeletonDataAsset = rel;
            return SpineGpuPathToUtf8(redirected);
        }
    }
    // 两条都落空：照原样交给渲染器，它会在日志 / spine.status 里说清楚读不到哪个文件
    return FullPathOf(m_SkeletonDataAsset);
}

std::shared_ptr<const SpineGpuAssetInfo> SpineRenderer::GetAssetInfo() const
{
    // 只读查询：不做 GUID 重定向（那会改字段），重定向在建实例时做
    const std::string path = FullPathOf(m_SkeletonDataAsset);
    if (path.empty())
    {
        auto none = std::make_shared<SpineGpuAssetInfo>();
        none->error = "no skeleton data set";
        return none;
    }
    return SpineGpuGetAssetInfo(SpineGpuPathFromUtf8(path));
}

void SpineRenderer::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;
    m_RuntimeTimeScale = m_TimeScale;
    // 实例不在这里建：见头文件类注释（模板 GameObject 上的组件永远不 tick，在这里建会留孤儿）
}

void SpineRenderer::Tick(float delta_time)
{
    // 自动 cook 重写了我们用的 .zspg ⇒ 重建实例（先比全局序号，没变化时不查表）
    if (m_InstanceId != 0 && m_SeenChangeSerial != g_asset_change_serial)
    {
        m_SeenChangeSerial = g_asset_change_serial;
        if (GenerationOf(m_AppliedPathKey) != m_AppliedGeneration)
        {
            LOG_INFO(ZRender, "SpineRenderer: '{}' was re-cooked, reloading", m_SkeletonDataAsset.c_str());
            ReleaseInstance();
        }
    }
    SyncInstance(/*allow_create=*/true);
    PushTransform(delta_time);
    CollectEvents();
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

void SpineRenderer::Submit(std::function<void(uint32_t)> command)
{
    if (m_InstanceId != 0)
    {
        command(m_InstanceId);
        return;
    }
    constexpr size_t kMaxPending = 64;   // 资产一直没设 / 加载不出来时别无限攒
    if (m_PendingCommands.size() < kMaxPending)
    {
        m_PendingCommands.push_back(std::move(command));
    }
}

void SpineRenderer::SetAnimation(int32_t track, const eastl::string& animation, bool loop)
{
    const std::string anim = NameArg(animation);
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestSetAnimationOn(id, track, anim, loop); });
}

void SpineRenderer::AddAnimation(int32_t track, const eastl::string& animation, bool loop, float delay)
{
    const std::string anim = NameArg(animation);
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestAddAnimationOn(id, track, anim, loop, delay); });
}

void SpineRenderer::SetEmptyAnimation(int32_t track, float mix_duration)
{
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestSetEmptyAnimationOn(id, track, mix_duration); });
}

void SpineRenderer::AddEmptyAnimation(int32_t track, float mix_duration, float delay)
{
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestAddEmptyAnimationOn(id, track, mix_duration, delay); });
}

void SpineRenderer::ClearTrack(int32_t track)
{
    const int t = std::max(0, track);
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestClearTrackOn(id, t); });
}

void SpineRenderer::ClearTracks()
{
    Submit([](uint32_t id) { SpineGpuRenderer::Get().RequestClearTrackOn(id, -1); });
}

void SpineRenderer::SetMix(const eastl::string& from, const eastl::string& to, float duration)
{
    const std::string f = from.c_str();
    const std::string t = to.c_str();
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestMixOn(id, f, t, duration); });
}

void SpineRenderer::SetSkin(const eastl::string& skin)
{
    const std::string s = SkinArg(skin);
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestSkinOn(id, s, /*setup_slots=*/true); });
}

void SpineRenderer::SetTimeScale(float time_scale)
{
    m_RuntimeTimeScale = time_scale;
    Submit([=](uint32_t id) { SpineGpuRenderer::Get().RequestTimeScaleOn(id, time_scale); });
}

eastl::string SpineRenderer::AnimationNameOf(uint32_t index) const
{
    if (index == 0xFFFFFFFFu)
    {
        return "<empty>";   // AnimationState::getEmptyAnimation() 的名字
    }
    const std::shared_ptr<const SpineGpuAssetInfo> info = GetAssetInfo();
    return index < info->animations.size() ? eastl::string(info->animations[index].c_str())
                                            : eastl::string(("#" + std::to_string(index)).c_str());
}

eastl::string SpineRenderer::GetCurrentAnimation(int32_t track) const
{
    if (track < 0 || static_cast<size_t>(track) >= m_Tracks.size() || !m_Tracks[static_cast<size_t>(track)].valid)
    {
        return "";
    }
    return AnimationNameOf(m_Tracks[static_cast<size_t>(track)].animation);
}

float SpineRenderer::GetCurrentTrackTime(int32_t track) const
{
    if (track < 0 || static_cast<size_t>(track) >= m_Tracks.size() || !m_Tracks[static_cast<size_t>(track)].valid)
    {
        return 0.0f;
    }
    return m_Tracks[static_cast<size_t>(track)].track_time;
}

int32_t SpineRenderer::GetAnimationCount() const
{
    return static_cast<int32_t>(GetAssetInfo()->animations.size());
}

eastl::string SpineRenderer::GetAnimationName(int32_t index) const
{
    const std::shared_ptr<const SpineGpuAssetInfo> info = GetAssetInfo();
    return index >= 0 && static_cast<size_t>(index) < info->animations.size()
               ? eastl::string(info->animations[static_cast<size_t>(index)].c_str())
               : eastl::string();
}

float SpineRenderer::GetAnimationDuration(const eastl::string& animation) const
{
    const std::shared_ptr<const SpineGpuAssetInfo> info = GetAssetInfo();
    for (size_t i = 0; i < info->animations.size(); ++i)
    {
        if (info->animations[i] == animation.c_str())
        {
            return info->durations[i];
        }
    }
    return -1.0f;
}

int32_t SpineRenderer::GetSkinCount() const
{
    return static_cast<int32_t>(GetAssetInfo()->skins.size());
}

eastl::string SpineRenderer::GetSkinName(int32_t index) const
{
    const std::shared_ptr<const SpineGpuAssetInfo> info = GetAssetInfo();
    return index >= 0 && static_cast<size_t>(index) < info->skins.size()
               ? eastl::string(info->skins[static_cast<size_t>(index)].c_str())
               : eastl::string();
}

eastl::string SpineRenderer::GetEventType(int32_t index) const
{
    return index >= 0 && static_cast<size_t>(index) < m_FrameEvents.size() ? m_FrameEvents[static_cast<size_t>(index)].type
                                                                           : eastl::string();
}

eastl::string SpineRenderer::GetEventAnimation(int32_t index) const
{
    return index >= 0 && static_cast<size_t>(index) < m_FrameEvents.size()
               ? m_FrameEvents[static_cast<size_t>(index)].animation
               : eastl::string();
}

int32_t SpineRenderer::GetEventTrack(int32_t index) const
{
    return index >= 0 && static_cast<size_t>(index) < m_FrameEvents.size() ? m_FrameEvents[static_cast<size_t>(index)].track
                                                                           : -1;
}

float SpineRenderer::GetEventTrackTime(int32_t index) const
{
    return index >= 0 && static_cast<size_t>(index) < m_FrameEvents.size()
               ? m_FrameEvents[static_cast<size_t>(index)].track_time
               : 0.0f;
}

void SpineRenderer::SetSkeletonDataAsset(const eastl::string& asset_path)
{
    m_SkeletonDataAsset = asset_path;
    // 新路径的 GUID（.zspg 才有；骨架源没有 registry 条目）
    m_SkeletonDataGuid.clear();
    const std::filesystem::path full = SpineGpuPathFromUtf8(FullPathOf(asset_path));
    if (!asset_path.empty() && SpineGpuIsContainerPath(full))
    {
        m_SkeletonDataGuid = GuidOf(full).c_str();
    }
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
    m_Tracks.clear();
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
        if (!allow_create || (m_SkeletonDataAsset.empty() && m_SkeletonDataGuid.empty()) || m_ParentObject == nullptr)
        {
            return;
        }
        const std::string path = ResolveSkeletonPath();   // 可能按 GUID 改写 m_SkeletonDataAsset
        m_InstanceId = renderer.RequestAddWorldInstance(path, NameArg(m_AnimationName), m_Loop,
                                                        static_cast<uint64_t>(m_ParentObject->GetID()));
        m_AppliedAsset = m_SkeletonDataAsset;
        m_AppliedPathKey = PathKey(path);
        m_AppliedGeneration = GenerationOf(m_AppliedPathKey);
        m_SeenChangeSerial = g_asset_change_serial;
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
        renderer.RequestSetAnimationOn(m_InstanceId, 0, NameArg(m_AnimationName), m_Loop);
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
        m_RuntimeTimeScale = m_TimeScale;
    }
    // 实例建好之前排队的运行时命令（TS 在 OnStart 里调 SetAnimation 之类）按顺序补发
    if (!m_PendingCommands.empty())
    {
        std::vector<std::function<void(uint32_t)>> pending;
        pending.swap(m_PendingCommands);
        for (const std::function<void(uint32_t)>& command : pending)
        {
            command(m_InstanceId);
        }
    }
}

void SpineRenderer::CollectEvents()
{
    m_FrameEvents.clear();
    if (m_InstanceId == 0)
    {
        return;
    }
    thread_local std::vector<SpineGpuAnimationEvent> scratch;
    scratch.clear();
    if (!SpineGpuRenderer::Get().DrainWorldEvents(m_InstanceId, scratch, &m_Tracks, nullptr))
    {
        return;
    }
    m_FrameEvents.reserve(scratch.size());
    for (const SpineGpuAnimationEvent& ev : scratch)
    {
        SpineAnimationEvent out;
        out.type = SpineGpuEventTypeName(ev.type);
        out.track = static_cast<int32_t>(ev.track);
        out.animation = AnimationNameOf(ev.animation);
        out.loop = ev.loop;
        out.track_time = ev.track_time;
        m_FrameEvents.push_back(std::move(out));
    }
    if (m_EventListener)
    {
        // 拷一份再派发：回调里调播放 API 不会动到这批，但回调里 SetEventListener 会换掉 m_EventListener
        const EventListener listener = m_EventListener;
        for (const SpineAnimationEvent& ev : m_FrameEvents)
        {
            listener(*this, ev);
        }
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
