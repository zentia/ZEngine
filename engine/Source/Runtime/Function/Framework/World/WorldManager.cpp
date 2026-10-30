#include "WorldManager.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Spine/SpineRenderer.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/Level/LevelDebugger.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Resource/Preload/PreloadManager.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/BaseClasses/Cast.h"

bool WorldManager::Initialize()
{
    m_IsWorldLoaded = false;
    m_CurrentWorldUrl = GET_SYSTEM(ConfigManager)->GetDefaultWorldUrl();
    m_LevelDebugger = std::make_shared<LevelDebugger>();
    return true;
}

void WorldManager::Shutdown()
{
    m_WorldPartition.Shutdown();

    for (auto& level_pair : m_LoadedLevels)
    {
        if (level_pair.second != nullptr)
        {
            level_pair.second->Unload();
        }
    }
    m_LoadedLevels.clear();
    m_LevelUrlRefCounts.clear();

    if (m_CurrentActiveLevel != nullptr)
    {
        MemoryManager::DestroyObject(m_CurrentActiveLevel);
        m_CurrentActiveLevel = nullptr;
    }

    m_CurrentWorldResource = nullptr;
    m_CurrentWorldUrl.clear();
    m_IsWorldLoaded = false;
    m_LevelDebugger.reset();
}

void WorldManager::Tick(float delta_time)
{
#if !defined(ZENGINE_SHARED_NO_SPINE)
    // GPU-Spine 世界实例的存活判据：本逻辑帧里没被 SpineRenderer 推过变换的就不画（SpineGpuRenderer.h）
    SpineGpuRenderer::Get().AdvanceWorldTick();
#endif

    if (!m_IsWorldLoaded)
    {
        LoadWorld(m_CurrentWorldUrl);
    }
#if !defined(ZENGINE_SHARED_NO_SPINE)
    // 控制台 spine.spawn 排队的对象（启动参数里执行时关卡还没加载）
    SpineRenderer::FlushPendingSpawns(m_CurrentActiveLevel);
#endif

    if (m_WorldPartition.IsEnabled())
    {
        UpdateWorldPartitionStreaming();
        TickLoadedLevels(delta_time);
    }
    else if (m_CurrentActiveLevel != nullptr)
    {
        m_CurrentActiveLevel->Tick(delta_time);
        m_LevelDebugger->Tick(m_CurrentActiveLevel);
    }

    if (LargeWorldCoordinates::IsEnabled())
    {
        const Vector3 streaming_source = GetWorldPartitionStreamingSource();
        LargeWorldCoordinates::SetRenderTileFromWorldPosition(Vector3d(streaming_source));
    }
}

void WorldManager::FixedTick(float fixed_delta_time)
{
    if (!m_IsWorldLoaded)
    {
        // 世界还没加载完时不补步：加载期攒下的墙钟时间不该变成一批物理步
        // 在加载完成那一帧突然倾泻下去。
        return;
    }

    if (m_WorldPartition.IsEnabled())
    {
        for (auto& level_pair : m_LoadedLevels)
        {
            Level* level = level_pair.second;
            if (level == nullptr)
            {
                continue;
            }
            level->FixedTick(fixed_delta_time);
        }
    }
    else if (m_CurrentActiveLevel != nullptr)
    {
        m_CurrentActiveLevel->FixedTick(fixed_delta_time);
    }
}

eastl::vector<eastl::string> WorldManager::GetLoadedLevelUrls() const
{
    eastl::vector<eastl::string> urls;
    urls.reserve(m_LoadedLevels.size());
    for (const auto& level_pair : m_LoadedLevels)
    {
        urls.push_back(level_pair.first);
    }
    return urls;
}

void WorldManager::TickLoadedLevels(float delta_time)
{
    for (auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }
        level->Tick(delta_time);
    }

    if (m_CurrentActiveLevel != nullptr)
    {
        m_LevelDebugger->Tick(m_CurrentActiveLevel);
    }
}

void WorldManager::UpdateWorldPartitionStreaming()
{
    m_WorldPartition.UpdateStreaming(*this, GetWorldPartitionStreamingSource());
}

Vector3 WorldManager::GetWorldPartitionStreamingSource() const
{
    for (const auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }

        if (std::shared_ptr<Character> character = level->getCurrentActiveCharacter().lock())
        {
            return character->GetPosition();
        }
    }

    for (const auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }

        if (CameraComponent* camera = level->GetMainCameraComponent())
        {
            if (GameObject* camera_object = camera->GetParentObject())
            {
                if (Transform* transform = camera_object->tryGetComponent(Transform))
                {
                    return transform->GetWorldPositionD().ToVector3();
                }
            }
        }
    }

    return m_WorldPartition.GetSettings().m_GridOrigin;
}

std::weak_ptr<PhysicsScene> WorldManager::GetCurrentActivePhysicsScene() const
{
    Level* active_level = m_CurrentActiveLevel;
    if (!active_level)
    {
        return {};
    }
    return active_level->getPhysicsScene();
}

bool WorldManager::LoadWorld(const eastl::string& world_url)
{
    LOG_INFO(ZWorld, "loading world: {}", world_url.c_str());
    WorldRes* world_res = GET_SYSTEM(AssetManager)->loadAsset<WorldRes>(world_url);
    if (!world_res)
    {
        LOG_ERROR(ZWorld, "load world asset failed: {}", world_url.c_str());
        m_CurrentActiveLevel = nullptr;
        return false;
    }

    m_WorldPartition.InitializeFromWorldRes(*world_res);

    if (m_WorldPartition.IsEnabled())
    {
        m_IsWorldLoaded = true;
        MemoryManager::DestroyObject(world_res);
        UpdateWorldPartitionStreaming();
        LOG_INFO(ZWorld, "world load succeed (World Partition streaming)");
        return true;
    }

    const bool is_level_load_success = LoadLevel(world_res->m_DefaultLevelUrl);
    if (!is_level_load_success)
    {
        LOG_ERROR(ZWorld, "load level failed: {}", world_res->m_DefaultLevelUrl.c_str());
        m_CurrentActiveLevel = nullptr;
        MemoryManager::DestroyObject(world_res);
        return false;
    }

    auto iter = m_LoadedLevels.find(world_res->m_DefaultLevelUrl);
    ASSERT(iter != m_LoadedLevels.end());
    m_CurrentActiveLevel = iter->second;

    m_IsWorldLoaded = true;
    MemoryManager::DestroyObject(world_res);
    LOG_INFO(ZWorld, "world load succeed!");
    return true;
}

bool WorldManager::LoadLevel(const eastl::string& level_url)
{
    if (auto existing = m_LoadedLevels.find(level_url); existing != m_LoadedLevels.end())
    {
        m_CurrentActiveLevel = existing->second;
        return m_CurrentActiveLevel != nullptr;
    }

    return LoadLevelAdditive(level_url);
}

bool WorldManager::LoadLevelAdditive(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return false;
    }

    if (auto existing = m_LoadedLevels.find(level_url); existing != m_LoadedLevels.end())
    {
        return existing->second != nullptr;
    }

    Level* new_level = MemoryManager::CreateObject<Level>();
    const bool is_level_load_success = new_level->load(level_url);
    if (!is_level_load_success)
    {
        MemoryManager::DestroyObject(new_level);
        LOG_ERROR(ZWorld, "load level failed: {}", level_url.c_str());
        return false;
    }

    m_LoadedLevels.emplace(level_url, new_level);
    if (m_CurrentActiveLevel == nullptr)
    {
        m_CurrentActiveLevel = new_level;
    }

    // Volumes with build_on_load request their mesh from PostLoadResource(),
    // which runs inside Level::load() -- before this level is published as
    // active, so GetCurrentActivePhysicsScene() was null and the build had to
    // be deferred. The level is active now, so run it.
    //
    // Same timing applies to rigid bodies: a prefab-expanded RigidBodyComponent
    // is created inside Level::load() and cannot build its body there. Create
    // those first -- the navmesh raycasts against them, so running the nav
    // build before this would see an empty physics scene.
    FlushPendingRigidBodies(new_level);

    if (auto* navigation_system = GET_SYSTEM(NavigationSystem); navigation_system != nullptr)
    {
        navigation_system->FlushDeferredBuild();
    }

    return true;
}

void WorldManager::FlushPendingRigidBodies(Level* level)
{
    if (level == nullptr)
    {
        return;
    }

    int created = 0;
    int still_pending = 0;
    for (const auto& id_object_pair : level->getAllGObjects())
    {
        const std::shared_ptr<GameObject>& game_object = id_object_pair.second;
        if (game_object == nullptr)
        {
            continue;
        }

        for (ImmediatePtr<Component>& component_ptr : game_object->getComponents())
        {
            RigidBodyComponent* rigidbody = Cast<RigidBodyComponent>(static_cast<Component*>(component_ptr));
            if (rigidbody == nullptr)
            {
                continue;
            }

            if (rigidbody->HasPendingBody())
            {
                rigidbody->FlushPendingBody() ? ++created : ++still_pending;
            }
        }
    }

    if (created > 0 || still_pending > 0)
    {
        LOG_INFO(ZWorld, "created {} deferred rigid body(ies), {} still pending", created, still_pending);
    }
}

void WorldManager::UnloadLevelByUrl(const eastl::string& level_url)
{
    auto iter = m_LoadedLevels.find(level_url);
    if (iter == m_LoadedLevels.end())
    {
        return;
    }

    Level* level = iter->second;
    if (level != nullptr)
    {
        level->Unload();
        MemoryManager::DestroyObject(level);
    }

    if (m_CurrentActiveLevel == level)
    {
        m_CurrentActiveLevel = nullptr;
    }

    m_LoadedLevels.erase(iter);
    m_LevelUrlRefCounts.erase(level_url);
}

bool WorldManager::AcquireLevelUrl(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return false;
    }

    auto ref_it = m_LevelUrlRefCounts.find(level_url);
    if (ref_it != m_LevelUrlRefCounts.end() && ref_it->second > 0)
    {
        ++ref_it->second;
        return true;
    }

    if (!LoadLevelAdditive(level_url))
    {
        return false;
    }

    m_LevelUrlRefCounts[level_url] = 1;
    return true;
}

void WorldManager::ReleaseLevelUrl(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return;
    }

    auto ref_it = m_LevelUrlRefCounts.find(level_url);
    if (ref_it == m_LevelUrlRefCounts.end() || ref_it->second <= 0)
    {
        return;
    }

    --ref_it->second;
    if (ref_it->second > 0)
    {
        return;
    }

    m_LevelUrlRefCounts.erase(ref_it);
    UnloadLevelByUrl(level_url);
}

void WorldManager::StartPreloadLevelUrl(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return;
    }

    if (auto preload = GET_SYSTEM(PreloadManager))
    {
        preload->PreloadAsset(level_url, PreloadPriority::High);
    }
}

bool WorldManager::IsLevelUrlReadyForActivation(const eastl::string& level_url) const
{
    if (level_url.empty())
    {
        return false;
    }

    if (m_LoadedLevels.find(level_url) != m_LoadedLevels.end())
    {
        return true;
    }

    if (auto preload = GET_SYSTEM(PreloadManager))
    {
        return preload->IsAssetLoaded(level_url);
    }

    return false;
}

void WorldManager::RefreshActiveLevelAfterPartitionUpdate()
{
    Level* preferred = nullptr;

    for (auto& level_pair : m_LoadedLevels)
    {
        Level* level = level_pair.second;
        if (level == nullptr)
        {
            continue;
        }

        if (level->getCurrentActiveCharacter().lock())
        {
            preferred = level;
            break;
        }
    }

    if (preferred == nullptr)
    {
        for (auto& level_pair : m_LoadedLevels)
        {
            Level* level = level_pair.second;
            if (level != nullptr && level->GetMainCameraComponent() != nullptr)
            {
                preferred = level;
                break;
            }
        }
    }

    if (preferred == nullptr && !m_LoadedLevels.empty())
    {
        preferred = m_LoadedLevels.begin()->second;
    }

    m_CurrentActiveLevel = preferred;
}

void WorldManager::ReloadCurrentLevel()
{
    if (m_WorldPartition.IsEnabled())
    {
        for (auto& level_pair : m_LoadedLevels)
        {
            if (level_pair.second != nullptr)
            {
                level_pair.second->Unload();
                MemoryManager::DestroyObject(level_pair.second);
            }
        }
        m_LoadedLevels.clear();
        m_LevelUrlRefCounts.clear();
        m_CurrentActiveLevel = nullptr;

        WorldRes* world_res = GET_SYSTEM(AssetManager)->loadAsset<WorldRes>(m_CurrentWorldUrl);
        if (!world_res)
        {
            LOG_ERROR(ZWorld, "reload failed: cannot load world {}", m_CurrentWorldUrl.c_str());
            return;
        }
        m_WorldPartition.Shutdown();
        m_WorldPartition.InitializeFromWorldRes(*world_res);
        MemoryManager::DestroyObject(world_res);
        UpdateWorldPartitionStreaming();
        LOG_INFO(ZWorld, "reload world partition cells requested");
        return;
    }

    auto active_level = m_CurrentActiveLevel;
    if (active_level == nullptr)
    {
        LOG_WARNING(ZWorld, "current level is nil");
        return;
    }

    const eastl::string level_url = active_level->getLevelResUrl();
    UnloadLevelByUrl(level_url);

    const bool is_load_success = LoadLevel(level_url);
    if (!is_load_success)
    {
        LOG_ERROR(ZWorld, "load level failed {}", level_url.c_str());
        return;
    }

    auto iter = m_LoadedLevels.find(level_url);
    ASSERT(iter != m_LoadedLevels.end());
    m_CurrentActiveLevel = iter->second;

    LOG_INFO(ZWorld, "reload current level succeed");
}

eastl::string WorldManager::MakePlayWorldUrl(const eastl::string& editor_level_url) const
{
    const ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || !project_info->IsValid())
    {
        LOG_ERROR(ZWorld, "MakePlayWorldUrl: no valid project; cannot place the PIE copy");
        return {};
    }

    // <project>/<saved_dir>/PIE/. saved_dir 默认 "saved"，与日志目录同级。
    // 放在项目内而不是临时目录，是为了让相对路径的资产引用（关卡里大量
    // PPtr 指向 asset/...）在加载时仍能按项目根解析；放到系统临时目录会
    // 让这些引用全部落空。
    const std::filesystem::path pie_dir =
        std::filesystem::absolute(project_info->GetProjectRoot() / project_info->saved_dir.c_str() / "PIE");

    std::error_code ec;
    std::filesystem::create_directories(pie_dir, ec);
    if (ec)
    {
        LOG_ERROR(ZWorld, "MakePlayWorldUrl: cannot create {}: {}", pie_dir.generic_string(), ec.message());
        return {};
    }

    const std::filesystem::path stem = std::filesystem::path(editor_level_url.c_str()).stem();
    const std::filesystem::path pie_path = (pie_dir / (stem.string() + "_pie.scene")).lexically_normal();
    return eastl::string(pie_path.generic_string().c_str());
}

bool WorldManager::BeginPlayWorld(eastl::string& out_play_world_url)
{
    out_play_world_url.clear();

    if (m_IsInPlayWorld)
    {
        LOG_WARNING(ZWorld, "BeginPlayWorld: already in a play world");
        return false;
    }

    if (m_WorldPartition.IsEnabled())
    {
        LOG_ERROR(ZWorld, "BeginPlayWorld: world partition is enabled; not supported");
        return false;
    }

    Level* editor_level = m_CurrentActiveLevel;
    if (editor_level == nullptr)
    {
        LOG_ERROR(ZWorld, "BeginPlayWorld: no active level to duplicate");
        return false;
    }

    const eastl::string editor_url = editor_level->getLevelResUrl();
    const eastl::string pie_url = MakePlayWorldUrl(editor_url);
    if (pie_url.empty())
    {
        return false;
    }

    // 先把编辑器这份"当前状态"（含未保存改动）另存成临时副本。用 setLevelResUrl
    // 临时改指向再存，存完立刻改回 —— 否则会把用户的场景文件覆盖掉。
    // 这里刻意不走 SaveCurrentLevelAs()：那个是给"另存为"用的，会改动
    // m_LoadedLevels 的键，PIE 不需要也不应该动编辑器的场景表。
    editor_level->setLevelResUrl(pie_url);
    const bool is_saved = editor_level->save();
    editor_level->setLevelResUrl(editor_url);
    if (!is_saved)
    {
        LOG_ERROR(ZWorld, "BeginPlayWorld: failed to write the PIE copy {}", pie_url.c_str());
        return false;
    }

    // 关键：先把活动关卡清空再加载。LoadLevelAdditive() 只在
    // m_CurrentActiveLevel 为空时才把新关卡设为活动，而此时它还指向编辑器
    // 那份；不清空的话 Play World 会加载成功但活动关卡仍是编辑器的，
    // 游戏逻辑就会直接改到编辑器世界上——正是要避免的事。
    m_EditorLevel = editor_level;
    m_EditorLevelUrl = editor_url;
    m_CurrentActiveLevel = nullptr;

    if (!LoadLevelAdditive(pie_url))
    {
        LOG_ERROR(ZWorld, "BeginPlayWorld: failed to load the PIE world {}", pie_url.c_str());
        m_CurrentActiveLevel = m_EditorLevel;
        m_EditorLevel = nullptr;
        m_EditorLevelUrl.clear();
        return false;
    }

    auto iter = m_LoadedLevels.find(pie_url);
    if (iter == m_LoadedLevels.end() || iter->second == nullptr)
    {
        LOG_ERROR(ZWorld, "BeginPlayWorld: PIE world missing after load {}", pie_url.c_str());
        m_CurrentActiveLevel = m_EditorLevel;
        m_EditorLevel = nullptr;
        m_EditorLevelUrl.clear();
        return false;
    }

    m_CurrentActiveLevel = iter->second;
    m_IsInPlayWorld = true;
    out_play_world_url = pie_url;

    LOG_INFO(ZWorld, "BeginPlayWorld: play world ready {} (editor world {} kept untouched)",
             pie_url.c_str(), editor_url.c_str());
    return true;
}

void WorldManager::EndPlayWorld()
{
    if (!m_IsInPlayWorld)
    {
        return;
    }

    const eastl::string pie_url = (m_CurrentActiveLevel != nullptr) ? m_CurrentActiveLevel->getLevelResUrl() : eastl::string();

    // 必须先排空渲染线程，再动场景。渲染线程每帧都会通过
    // getCurrentActiveLevel() 拿当前关卡并直接解引用里面的 GameObject
    // （RenderUpdateSceneCmd::Execute -> SyncPointLightsFromLevel），而下面
    // 的 UnloadLevelByUrl() 会把 Play World 连同它的 GameObject 一起销毁。
    // 两步之间没有同步的话，渲染线程读到的就是正在被删的对象 —— PIE 停止
    // 时的访问违规（SyncPointLightsFromLevel / GuidAllocator::getAllocatedGuids
    // 里的 0xc0000005）就是这个竞态。这里在游戏线程上等待渲染 + RHI 队列
    // 排空，并丢掉尚未执行的命令，保证卸载时没有人在读这份关卡。
    if (auto* render_system = GET_SYSTEM(RenderSystem); render_system != nullptr)
    {
        render_system->FlushRenderingCommands();
    }

    // 渲染层持有的是按对象 ID 索引的渲染对象；Play World 与编辑器世界的
    // GObjectID 是两套独立分配（ObjectIDAllocator 单调递增），直接卸载会
    // 在交换数据里留下对已销毁对象的删除项，所以先让渲染层整体清空。
    if (auto* render_system = GET_SYSTEM(RenderSystem); render_system != nullptr)
    {
        render_system->ClearForLevelReloading();
    }

    if (!pie_url.empty())
    {
        UnloadLevelByUrl(pie_url);
    }

    // 编辑器那份一直没有动过，直接把指针复位回去即可 —— 不需要重载，
    // 也不会产生"未保存改动"（这正是独立 Play World 相对同世界方案
    // 的价值所在）。保险起见按 URL 再确认一次，防止编辑器在此期间换过场景。
    auto editor_iter = m_LoadedLevels.find(m_EditorLevelUrl);
    if (editor_iter != m_LoadedLevels.end() && editor_iter->second != nullptr)
    {
        m_CurrentActiveLevel = editor_iter->second;
    }
    else if (m_EditorLevel != nullptr)
    {
        m_CurrentActiveLevel = m_EditorLevel;
    }

    m_IsInPlayWorld = false;
    m_EditorLevel = nullptr;
    m_EditorLevelUrl.clear();

    LOG_INFO(ZWorld, "EndPlayWorld: discarded {}, editor world {} restored",
             pie_url.c_str(), m_CurrentActiveLevel != nullptr ? m_CurrentActiveLevel->getLevelResUrl().c_str() : "(none)");
}

bool WorldManager::ReloadActiveLevelInPlace()
{
    if (m_WorldPartition.IsEnabled())
    {
        // Streaming worlds have no single "the scene" to reload; PIE on top of a
        // partitioned world is not supported yet.
        LOG_ERROR(ZWorld, "ReloadActiveLevelInPlace: world partition is enabled; not supported");
        return false;
    }

    Level* active_level = m_CurrentActiveLevel;
    if (active_level == nullptr)
    {
        LOG_WARNING(ZWorld, "ReloadActiveLevelInPlace: no active level");
        return false;
    }

    const eastl::string level_url = active_level->getLevelResUrl();
    if (level_url.empty())
    {
        LOG_ERROR(ZWorld, "ReloadActiveLevelInPlace: active level has no url");
        return false;
    }

    // ReloadCurrentLevel() is the editor-facing variant and is fine here: it
    // unloads by url then re-loads, which is exactly the discard-and-restore
    // PIE needs. It is not called through EditorSceneManager because PIE does
    // not want the unsaved-changes prompt -- gameplay mutations are always
    // meant to be thrown away, never saved.
    const bool was_dirty = active_level->IsDirty();
    ReloadCurrentLevel();

    Level* reloaded = m_CurrentActiveLevel;
    if (reloaded == nullptr)
    {
        LOG_ERROR(ZWorld, "ReloadActiveLevelInPlace: reload failed for {}", level_url.c_str());
        return false;
    }

    // A fresh load must come back clean. A level can legitimately be dirty right
    // after load (e.g. a migration normalised something), so only clear the flag
    // that gameplay itself raised.
    if (was_dirty && reloaded->IsDirty())
    {
        reloaded->ClearDirty();
    }

    LOG_INFO(ZWorld, "ReloadActiveLevelInPlace: restored {}", level_url.c_str());
    return true;
}

void WorldManager::SaveCurrentLevel()
{
    if (m_WorldPartition.IsEnabled())
    {
        for (auto& level_pair : m_LoadedLevels)
        {
            if (level_pair.second != nullptr)
            {
                level_pair.second->save();
            }
        }
        LOG_INFO(ZWorld, "saved {} streamed level(s)", m_LoadedLevels.size());
        return;
    }

    Level* active_level = m_CurrentActiveLevel;
    if (active_level == nullptr)
    {
        LOG_ERROR(ZWorld, "save level failed, no active level");
        return;
    }

    active_level->save();
}

bool WorldManager::IsCurrentLevelDirty() const
{
    if (m_WorldPartition.IsEnabled())
    {
        for (const auto& level_pair : m_LoadedLevels)
        {
            if (level_pair.second != nullptr && level_pair.second->IsDirty())
            {
                return true;
            }
        }
        return false;
    }

    return m_CurrentActiveLevel != nullptr && m_CurrentActiveLevel->IsDirty();
}

void WorldManager::MarkCurrentLevelDirty()
{
    if (m_CurrentActiveLevel != nullptr)
    {
        m_CurrentActiveLevel->MarkDirty();
    }
}

bool WorldManager::SaveCurrentLevelAs(const eastl::string& new_level_url)
{
    if (new_level_url.empty())
    {
        LOG_ERROR(ZWorld, "SaveCurrentLevelAs: empty level url");
        return false;
    }

    if (m_WorldPartition.IsEnabled())
    {
        LOG_WARNING(ZWorld, "SaveCurrentLevelAs: world partition is enabled; saving all loaded levels in place");
        SaveCurrentLevel();
        return true;
    }

    Level* active_level = m_CurrentActiveLevel;
    if (active_level == nullptr)
    {
        LOG_ERROR(ZWorld, "SaveCurrentLevelAs: no active level");
        return false;
    }

    const eastl::string old_url = active_level->getLevelResUrl();
    active_level->setLevelResUrl(new_level_url);
    if (!active_level->save())
    {
        active_level->setLevelResUrl(old_url);
        LOG_ERROR(ZWorld, "SaveCurrentLevelAs: save failed for {}", new_level_url.c_str());
        return false;
    }

    if (old_url != new_level_url)
    {
        m_LoadedLevels.erase(old_url);
        m_LoadedLevels.emplace(new_level_url, active_level);

        if (auto ref_it = m_LevelUrlRefCounts.find(old_url); ref_it != m_LevelUrlRefCounts.end())
        {
            m_LevelUrlRefCounts[new_level_url] = ref_it->second;
            m_LevelUrlRefCounts.erase(ref_it);
        }
    }

    LOG_INFO(ZWorld, "SaveCurrentLevelAs: saved -> {}", new_level_url.c_str());
    return true;
}

bool WorldManager::OpenScene(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        LOG_ERROR(ZWorld, "OpenScene: empty level url");
        return false;
    }

    if (m_WorldPartition.IsEnabled())
    {
        LOG_WARNING(ZWorld, "OpenScene: world partition is enabled; single-scene open is not supported");
        return false;
    }

    eastl::vector<eastl::string> urls_to_unload;
    urls_to_unload.reserve(m_LoadedLevels.size());
    for (const auto& level_pair : m_LoadedLevels)
    {
        urls_to_unload.push_back(level_pair.first);
    }
    for (const eastl::string& url : urls_to_unload)
    {
        UnloadLevelByUrl(url);
    }

    if (!LoadLevel(level_url))
    {
        LOG_ERROR(ZWorld, "OpenScene: failed to load {}", level_url.c_str());
        return false;
    }

    auto iter = m_LoadedLevels.find(level_url);
    if (iter == m_LoadedLevels.end() || iter->second == nullptr)
    {
        LOG_ERROR(ZWorld, "OpenScene: level missing after load {}", level_url.c_str());
        return false;
    }

    m_CurrentActiveLevel = iter->second;

    // 单场景直开也是一个"世界已就绪"的终态：不置位的话 Tick() 里的
    // `if (!m_IsWorldLoaded) LoadWorld(m_CurrentWorldUrl)` 会在下一帧把配置里的
    // 默认 world 再拉起来，把它自己默认关卡设为活动关卡 —— 调用方刚打开的
    // 场景（独立进程 --level / startup_level）活不过一帧就被顶掉，角色接管
    // 于是永远拿到一个没有角色的关卡。置位后 Tick 不再补加载，活动关卡保持
    // 本次 OpenScene 的结果，直到下一次显式切换。
    m_IsWorldLoaded = true;

    LOG_INFO(ZWorld, "OpenScene: loaded {}", level_url.c_str());
    return true;
}