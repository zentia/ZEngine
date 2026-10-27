#include "Runtime/Function/Framework/Level/Level.h"

#include "Application/Application.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Particle/ParticleManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"
#include "Runtime/Function/Render/RenderObject.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Common/Level.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeDispatch.h"
#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    std::string LowerExtension(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return extension;
    }

    std::filesystem::path ResolveLevelLoadPath(const eastl::string& level_url)
    {
        AssetManager* asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return {};
        }

        const std::filesystem::path rel(level_url.c_str());
        if (rel.is_absolute())
        {
            return rel.lexically_normal();
        }

        if (const auto project_info = GET_SYSTEM(ProjectInfo))
        {
            const std::filesystem::path project_content = project_info->GetProjectContent();
            if (!project_content.empty())
            {
                std::error_code ec;
                const std::filesystem::path project_path =
                    std::filesystem::absolute(project_content / rel, ec).lexically_normal();
                if (std::filesystem::exists(project_path, ec))
                {
                    return project_path;
                }
            }
        }

        return asset_manager->GetFullPath(level_url);
    }

    std::filesystem::path ResolveLevelSavePath(const eastl::string& level_url)
    {
        AssetManager* asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return {};
        }

        std::filesystem::path level_path = asset_manager->ResolveProjectContentPath(level_url);
        const std::string extension = LowerExtension(level_path);
        if (extension == ".json")
        {
            // Legacy .json level urls are not a supported level format; levels
            // are .scene (YAML) or .zasset (binary). Remap to .scene so an old
            // reference still resolves instead of failing.
            const std::filesystem::path stem_path = level_path.stem();
            if (LowerExtension(stem_path) == ".level")
            {
                level_path = level_path.parent_path() / (stem_path.stem().string() + ".scene");
            }
            else
            {
                level_path.replace_extension(".scene");
            }
        }

        return level_path;
    }

    LevelSerializationHooks& SerializationHooksStorage()
    {
        static LevelSerializationHooks s_hooks;
        return s_hooks;
    }
}  // namespace

void Level::SetSerializationHooks(const LevelSerializationHooks& hooks)
{
    SerializationHooksStorage() = hooks;
}

const LevelSerializationHooks& Level::GetSerializationHooks()
{
    return SerializationHooksStorage();
}

void Level::FlushRenderDeletes()
{
    auto render_system = GET_SYSTEM(RenderSystem);
    if (!render_system)
    {
        return;
    }

    RenderSwapData& swap_data = render_system->GetSwapContext().GetLogicSwapData();
    for (const auto& id_object_pair : m_Gobjects)
    {
        if (id_object_pair.second == nullptr)
        {
            continue;
        }
        GameObjectDesc delete_desc(id_object_pair.first, {});
        swap_data.AddDeleteGameObject(std::move(delete_desc));
    }
}

void Level::BumpLoadSerial()
{
    // Process-wide counter, so a Level object that is unloaded and reloaded (or a new
    // Level allocated at a recycled address) never repeats a serial.
    static std::atomic<uint64_t> s_next_load_serial {1};
    m_LoadSerial = s_next_load_serial.fetch_add(1, std::memory_order_relaxed);
}

void Level::clear()

{
    m_TransformSceneRoots.Clear();
    m_CurrentActiveCharacter.reset();
    m_DetachedCharacterObjectId = k_invalid_gobject_id;
    m_Gobjects.clear();

    if (PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager))
    {
        physics_manager->DeletePhysicsScene(m_PhysicsScene);
    }
}

GObjectID Level::CreateObject(const GameObject& object_instance_res)
{
    GObjectID object_id = ObjectIDAllocator::Alloc();
    ASSERT(object_id != k_invalid_gobject_id);

    std::shared_ptr<GameObject> gobject;
    try
    {
        gobject = std::make_shared<GameObject>(object_id);
    }
    catch (const std::bad_alloc&)
    {
        LOG_FATAL(ZLevel, "cannot allocate memory for new gobject");
    }

    bool is_loaded = gobject->load(object_instance_res);
    if (is_loaded)
    {
        // A live GameObject needs an instance ID so a PPtr can name it: editor
        // bookkeeping saved next to it (PrefabInstance root / source map) refers
        // to it that way, and the scene writer maps instance ID -> fileID. The
        // ObjectManager table is non-owning and ~Object unregisters the ID, so
        // the shared_ptr stays the sole owner.
        if (ObjectManager* object_manager = GET_SYSTEM(ObjectManager))
        {
            object_manager->AllocateAndAssignInstanceID(gobject.get());
        }
        m_Gobjects.emplace(object_id, gobject);

        CameraComponent* camera_component = gobject->tryGetComponent(CameraComponent);
        if (camera_component != nullptr && camera_component->isMainCamera())
        {
            SetMainCamera(object_id);
        }
    }
    else
    {
        LOG_ERROR(ZLevel, "loading object {} failed", object_instance_res.name.c_str());
        return k_invalid_gobject_id;
    }
    return object_id;
}

bool Level::load(const eastl::string& levelPath)
{
    LOG_INFO(ZLevel, "loading level: {}", levelPath.c_str());

    m_LevelResUrl = levelPath;
    BumpLoadSerial();

    const std::filesystem::path level_full_path = ResolveLevelLoadPath(levelPath);
    // Unity-1:1 -- a level is an object graph, never a nested object array.
    // Both supported backends (YAML .scene for authoring, binary .zasset for
    // cooked builds) hydrate the file's flat object table; LevelRes is the
    // header object at fileID 1 and every GameObject / Component is a sibling.
    // JSON is deliberately NOT supported (see PPTR_DESIGN.md): it cannot carry
    // the (m_FileID, m_PathID) reference pairs that PPtr / ImmediatePtr need.
    const bool is_binary = LowerExtension(level_full_path) == ".zasset";
    const bool is_yaml = LowerExtension(level_full_path) == ".scene";
    if (!is_binary && !is_yaml)
    {
        LOG_ERROR(ZLevel,
                  "unsupported level format '{}': only .scene (YAML) and .zasset (binary) are supported",
                  level_full_path.generic_string());
        return false;
    }

    std::vector<std::pair<int64_t, Object*>> entries;
    const bool read_ok = is_binary ? GET_SYSTEM(AssetManager)->ReadObjectsFromBinary(level_full_path, entries)
                                   : GET_SYSTEM(AssetManager)->ReadObjectsFromYaml(level_full_path, entries);
    if (!read_ok)
    {
        LOG_ERROR(ZLevel, "failed to read level object graph: {}", level_full_path.generic_string());
        return false;
    }

    if (!LoadObjectGraph(entries, true))
    {
        return false;
    }
    LOG_INFO(ZLevel, "level load succeed ({})", is_binary ? "binary" : "yaml");
    return true;
}

bool Level::LoadObjectGraph(const std::vector<std::pair<int64_t, Object*>>& entries, bool create_physics_scene)
{
    LevelRes* header = nullptr;
    std::vector<GameObject*> loaded_objects;
    loaded_objects.reserve(entries.size());
    for (const auto& entry : entries)
    {
        Object* object = entry.second;
        if (object == nullptr)
        {
            continue;
        }
        if (object->GetType() == TypeOf<LevelRes>())
        {
            header = static_cast<LevelRes*>(object);
        }
        else if (object->GetType() == TypeOf<GameObject>())
        {
            loaded_objects.push_back(static_cast<GameObject*>(object));
        }
    }

    const Vector3 gravity = (header != nullptr) ? header->m_Gravity : Vector3 {0.f, 0.f, -9.8f};
    if (create_physics_scene)
    {
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        if (physics_manager == nullptr)
        {
            LOG_ERROR(ZLevel, "cannot load level {}: PhysicsManager unavailable", m_LevelResUrl.c_str());
            return false;
        }
        m_PhysicsScene = physics_manager->CreatePhysicsScene(gravity);
    }
    ParticleEmitterIDAllocator::reset();

    std::unordered_map<int32_t, GameObject*> file_to_live;
    file_to_live.reserve(loaded_objects.size());
    for (GameObject* loaded_object : loaded_objects)
    {
        loaded_object->RebuildRuntimeComponents();
        const GObjectID live_id = CreateObject(*loaded_object);
        const auto live_it = m_Gobjects.find(live_id);
        if (live_it != m_Gobjects.end() && live_it->second != nullptr && loaded_object->GetInstanceID() != 0)
        {
            file_to_live.emplace(loaded_object->GetInstanceID(), live_it->second.get());
        }
    }

    if (header != nullptr && !header->m_CharacterName.empty())
    {
        for (const auto& object_pair : m_Gobjects)
        {
            if (object_pair.second != nullptr && header->m_CharacterName == object_pair.second->GetName())
            {
                m_CurrentActiveCharacter = std::make_shared<Character>(object_pair.second);
                break;
            }
        }
    }

    m_IsLoaded = true;
    m_IsDirty = false;
    RebuildAllTransformHierarchies();

    if (const auto on_loaded = GetSerializationHooks().on_loaded)
    {
        on_loaded(*this, entries, file_to_live);
    }

    LOG_INFO(ZLevel, "level object graph built ({} objects)", loaded_objects.size());
    return true;
}

void Level::Unload()
{
    if (const auto on_unloading = GetSerializationHooks().on_unloading)
    {
        on_unloading(*this);
    }
    FlushRenderDeletes();
    clear();
    BumpLoadSerial();
    LOG_INFO(ZLevel, "unload level: {}", m_LevelResUrl.c_str());
}

bool Level::save()
{
    LOG_INFO(ZLevel, "saving level: {}", m_LevelResUrl.c_str());

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZLevel, "failed to save {}, AssetManager unavailable", m_LevelResUrl.c_str());
        return false;
    }

    const std::filesystem::path level_path = ResolveLevelSavePath(m_LevelResUrl);
    if (level_path.empty())
    {
        LOG_ERROR(ZLevel, "failed to resolve save path for {}", m_LevelResUrl.c_str());
        return false;
    }

    // Unity-1:1 -- write the whole level as one flat object graph. LevelRes is
    // the header at fileID 1; every GameObject and Component is a sibling in
    // the same table, cross-referenced by (m_FileID, m_PathID). The backend is
    // chosen purely by extension: .scene -> YAML (authoring), .zasset ->
    // binary (cooked). JSON is not a supported level format.
    const bool is_binary = LowerExtension(level_path) == ".zasset";
    if (!is_binary && LowerExtension(level_path) != ".scene")
    {
        LOG_ERROR(ZLevel,
                  "unsupported level format '{}': only .scene (YAML) and .zasset (binary) are supported",
                  level_path.generic_string());
        return false;
    }

    std::vector<Object*> graph_objects;
    std::vector<int64_t> graph_ids;

    LevelRes* header = static_cast<LevelRes*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<LevelRes>(), 0));
    if (header == nullptr)
    {
        LOG_ERROR(ZLevel, "failed to allocate LevelRes header for {}", m_LevelResUrl.c_str());
        return false;
    }

    // Round-trip the scene settings that actually exist on the live level.
    // (The previous implementation wrote a freshly default-constructed
    // LevelRes, silently dropping gravity and the active character on save.)
    if (auto physics_scene = m_PhysicsScene.lock())
    {
        header->m_Gravity = physics_scene->getGravity();
    }
    if (m_CurrentActiveCharacter != nullptr)
    {
        const std::shared_ptr<GameObject> character_object = m_CurrentActiveCharacter->getObject().lock();
        if (character_object != nullptr)
        {
            header->m_CharacterName = character_object->GetName();
        }
    }

    graph_objects.push_back(header);
    graph_ids.push_back(1);

    int64_t next_file_id = 2;
    for (const auto& id_object_pair : m_Gobjects)
    {
        std::shared_ptr<GameObject> live_object = id_object_pair.second;
        if (live_object == nullptr)
        {
            continue;
        }
        live_object->SyncSerializedComponents();
        graph_objects.push_back(live_object.get());
        graph_ids.push_back(next_file_id++);

        for (ImmediatePtr<Component>& component_ptr : live_object->getComponents())
        {
            Component* component = component_ptr;
            if (component != nullptr)
            {
                graph_objects.push_back(component);
                graph_ids.push_back(next_file_id++);
            }
        }
    }

    if (const auto collect_extra_objects = GetSerializationHooks().collect_extra_objects)
    {
        std::vector<Object*> extra_objects;
        collect_extra_objects(*this, extra_objects);
        for (Object* extra_object : extra_objects)
        {
            if (extra_object != nullptr)
            {
                graph_objects.push_back(extra_object);
                graph_ids.push_back(next_file_id++);
            }
        }
    }

    const bool is_save_success =
        is_binary ? asset_manager->WriteObjectsToDiskThreadSafe(level_path, graph_objects.data(), graph_ids.data(),
                                                                graph_objects.size())
                  : asset_manager->WriteObjectsToYaml(level_path, graph_objects.data(), graph_ids.data(),
                                                      graph_objects.size());

    // ⚠️ ObjectManager::Destroy（= delete），不是 MemoryManager::DestroyObject：header 是
    // Produce() 出来的（ProduceHelper 内部 `new T()`），而 Apple 上全局 new/delete 不走
    // mimalloc ⇒ 用 mi_free 释放会在 mi_usable_size 里 EXC_BAD_ACCESS。见
    // ObjectManager.h 的 Destroy() 注释。存关卡是编辑器的高频路径，这条必崩。
    GET_SYSTEM(ObjectManager)->Destroy(header);

    if (!is_save_success)
    {
        LOG_ERROR(ZLevel, "failed to save {}", level_path.generic_string());
    }
    else
    {
        LOG_INFO(ZLevel, "level save succeed -> {}", level_path.generic_string());
        m_IsDirty = false;
    }

    return is_save_success;
}

void Level::Tick(float delta_time)
{
    if (!m_IsLoaded)
    {
        return;
    }

    for (const auto& id_object_pair : m_Gobjects)
    {
        assert(id_object_pair.second);
        if (id_object_pair.second)
        {
            id_object_pair.second->Tick(delta_time);
        }
    }

    // 运行时生成的对象（Instantiate 产物）不在主表里，单独 Tick。
    //
    // 这段遍历依赖 Destroy 是**延迟**的（帧末统一执行）：如果脚本在某个对象的
    // Tick 里 Destroy 了这张表里的另一个对象、而销毁是立即的，这里正在持有的
    // 迭代器会当场失效。GameObject::Destroy 走延迟队列正是为了保住这一点。
    for (const auto& id_object_pair : m_RuntimeGObjects)
    {
        if (id_object_pair.second != nullptr)
        {
            id_object_pair.second->Tick(delta_time);
        }
    }

    if (m_CurrentActiveCharacter && g_isPlaying)
    {
        m_CurrentActiveCharacter->Tick(delta_time);
    }

    // 所有 Tick 走完才销毁。脚本在某个对象的 OnUpdate 里 Destroy 另一个对象时，
    // 上面的遍历手里正好拿着那张表的迭代器 —— 立即销毁会让它们当场失效。
    FlushPendingDestroys();

    std::shared_ptr<PhysicsScene> physics_scene = m_PhysicsScene.lock();
    if (physics_scene)
    {
        physics_scene->Tick(delta_time);
    }

    GetTransformChangeDispatch().DispatchChanges();
}

void Level::RebuildAllTransformHierarchies()
{
    m_TransformSceneRoots.Clear();

    std::unordered_set<Transform*> rebuilt_roots;
    for (const auto& id_object_pair : m_Gobjects)
    {
        const std::shared_ptr<GameObject>& gobject = id_object_pair.second;
        if (gobject == nullptr)
        {
            continue;
        }

        Transform* transform = gobject->tryGetComponent(Transform);
        if (transform == nullptr)
        {
            continue;
        }

        if (transform->GetParent() == nullptr)
        {
            m_TransformSceneRoots.OnTransformBecameRoot(transform);
        }

        Transform* root = transform->GetRoot();
        if (root != nullptr && rebuilt_roots.insert(root).second)
        {
            root->RebuildTransformHierarchy();
        }
    }
}

std::weak_ptr<GameObject> Level::GetGObjectByID(GObjectID go_id) const
{
    auto iter = m_Gobjects.find(go_id);
    if (iter != m_Gobjects.end())
    {
        return iter->second;
    }

    return std::weak_ptr<GameObject>();
}

CameraComponent* Level::GetMainCameraComponent() const
{
    for (const auto& object_pair : m_Gobjects)
    {
        const std::shared_ptr<GameObject>& object = object_pair.second;
        if (object == nullptr)
        {
            continue;
        }

        CameraComponent* camera_component = object->tryGetComponent(CameraComponent);
        // 停用（含父级被停用）的对象上的相机不再当主相机 —— 否则
        // SetActive(false) 对相机毫无效果，画面照旧，正是这次要修的那类"看不见
        // 变化"的 bug。Unity 同：disabled camera 不渲染。
        if (camera_component != nullptr && camera_component->isEnabled() && camera_component->isMainCamera() &&
            object->IsActiveInHierarchy())
        {
            return camera_component;
        }
    }

    return nullptr;
}

void Level::SetMainCamera(GObjectID go_id)
{
    for (const auto& object_pair : m_Gobjects)
    {
        const std::shared_ptr<GameObject>& object = object_pair.second;
        if (object == nullptr)
        {
            continue;
        }

        CameraComponent* camera_component = object->tryGetComponent(CameraComponent);
        if (camera_component == nullptr)
        {
            continue;
        }

        camera_component->SetMainCamera(object_pair.first == go_id);
    }
}

bool Level::RegisterRuntimeGObject(GameObject* gobject)
{
    if (gobject == nullptr)
    {
        LOG_ERROR(ZLevel, "RegisterRuntimeGObject: null object");
        return false;
    }
    if (gobject->GetID() == k_invalid_gobject_id)
    {
        LOG_ERROR(ZLevel, "RegisterRuntimeGObject: '{}' has no GObjectID", gobject->GetName().c_str());
        return false;
    }

    const auto inserted = m_RuntimeGObjects.emplace(gobject->GetID(), gobject);
    if (!inserted.second)
    {
        // 走到这里几乎一定是调用方忘了给 clone 重新分配 GObjectID：序列化往返
        // 会把模板的 m_Id 一起拷过来，两次 Instantiate 同一个 prefab 就会撞 id。
        // 撞了不强行覆盖 —— 覆盖会悄悄丢掉前一个对象的唯一引用（本表是非拥有的，
        // 丢了就再也找不到它，也就永远无法 Destroy，等于泄漏 + 悬挂两个错误）。
        LOG_ERROR(ZLevel,
                  "RegisterRuntimeGObject: id {} ('{}') already registered to '{}' — "
                  "the clone did not get a fresh GObjectID",
                  gobject->GetID(),
                  gobject->GetName().c_str(),
                  inserted.first->second->GetName().c_str());
        return false;
    }
    return true;
}

bool Level::UnregisterRuntimeGObject(GObjectID go_id)
{
    return m_RuntimeGObjects.erase(go_id) != 0;
}

GameObject* Level::FindRuntimeGObject(GObjectID go_id) const
{
    const auto iter = m_RuntimeGObjects.find(go_id);
    return iter != m_RuntimeGObjects.end() ? iter->second : nullptr;
}

GameObject* Level::FindGObjectByName(const eastl::string& name) const
{
    if (name.empty())
    {
        return nullptr;
    }

    for (const auto& id_object_pair : m_Gobjects)
    {
        if (id_object_pair.second != nullptr && id_object_pair.second->GetName() == name)
        {
            return id_object_pair.second.get();
        }
    }

    for (const auto& id_object_pair : m_RuntimeGObjects)
    {
        GameObject* gobject = id_object_pair.second;
        if (gobject != nullptr && gobject->GetName() == name)
        {
            return gobject;
        }
    }

    return nullptr;
}

void Level::QueueDestroy(GObjectID go_id)
{
    if (go_id == k_invalid_gobject_id)
    {
        return;
    }
    // 同一帧里重复 Destroy 同一个对象比想象中常见：两个 Behaviour 都请求销毁它，
    // 或者脚本在 OnUpdate 里连着调了两次。不去重的话第二次会去碰一个已经析构的
    // 对象 —— 而销毁是延迟的，第二次请求到达时对象可能还没死，于是看起来"能跑"，
    // 直到某次顺序变了才崩。
    if (std::find(m_PendingDestroys.begin(), m_PendingDestroys.end(), go_id) != m_PendingDestroys.end())
    {
        return;
    }
    m_PendingDestroys.push_back(go_id);
}

void Level::FlushPendingDestroys()
{
    if (m_PendingDestroys.empty())
    {
        return;
    }

    // 先换出来再处理：销毁过程里可能有人再 QueueDestroy（例如某个组件的
    // OnDestroy 又销毁别的对象），那属于下一帧的事，不该在这一轮里被顺手做掉。
    std::vector<GObjectID> batch;
    batch.swap(m_PendingDestroys);

    for (const GObjectID go_id : batch)
    {
        // 两个来源的销毁路径**不同**，混用必错：
        //   主表对象   —— shared_ptr 是唯一持有者，erase 掉 map 条目就析构了。
        //                 再调一次 ObjectManager::Destroy 是双重释放。
        //   运行时对象 —— ObjectManager::Produce（= new）出来的，归 ObjectManager
        //                 管，必须显式 Destroy；只 erase 会泄漏，它的组件还全
        //                 留在 ObjectManager 的表里。
        const bool is_runtime = m_RuntimeGObjects.find(go_id) != m_RuntimeGObjects.end();

        GameObject* gobject = nullptr;
        if (is_runtime)
        {
            gobject = FindRuntimeGObject(go_id);
        }
        else
        {
            const auto iter = m_Gobjects.find(go_id);
            gobject = (iter != m_Gobjects.end()) ? iter->second.get() : nullptr;
        }
        if (gobject == nullptr)
        {
            continue;  // 已经被别的路径销毁了（例如同一帧里关卡卸载）
        }

        if (Transform* transform = gobject->GetTransform())
        {
            // 1) 子对象先脱离。父 Transform 马上就没了，留在它的 m_Children 里
            //    会让每个子对象的 m_Father 指向已释放的内存。这里把子对象提升到
            //    场景根 —— Unity 是连整棵子树一起销毁，我们刻意只销毁指定的那
            //    一个（GameObject::DestroyObject 的注释里有说明）。
            std::vector<Transform*> children;
            children.reserve(transform->GetChildCount());
            for (const PPtr<Transform>& child : transform->GetChildren())
            {
                if (Transform* child_transform = child)
                {
                    children.push_back(child_transform);
                }
            }
            for (Transform* child_transform : children)
            {
                child_transform->SetParent(nullptr, Transform::kWorldPositionStays);
            }

            // 2) 自己也从父的 children 里摘掉。SetParent 内部会走
            //    DetachFromParentList + UpdateSceneRootRegistration，所以
            //    TransformSceneRoots 的注册也一并更新了。
            transform->SetParent(nullptr, Transform::kWorldPositionStays);
        }

        // 3) 渲染侧。Level::FlushRenderDeletes 是卸载时批量发的，单个对象要自己
        //    发一条删除，否则它的 render entity 会留在场景里渲染一个野对象。
        if (auto render_system = GET_SYSTEM(RenderSystem))
        {
            RenderSwapData& swap_data = render_system->GetSwapContext().GetLogicSwapData();
            swap_data.AddDeleteGameObject(GameObjectDesc(go_id, {}));
        }

        // 4) 摘表 / 销毁
        if (is_runtime)
        {
            m_RuntimeGObjects.erase(go_id);
            // Produce() 内部是 new T()，Apple 上全局 operator new 故意不接管
            // mimalloc —— 所以必须走 ObjectManager::Destroy（= delete），
            // 不能用 MemoryManager::DestroyObject。见 PrefabUtility.cpp:703。
            if (ObjectManager* object_manager = GET_SYSTEM(ObjectManager))
            {
                object_manager->Destroy(gobject);
            }
        }
        else
        {
            DeleteGObjectByID(go_id);  // erase -> shared_ptr 析构
        }
    }
}

void Level::DeleteGObjectByID(GObjectID go_id)
{
    auto iter = m_Gobjects.find(go_id);
    if (iter != m_Gobjects.end())
    {
        std::shared_ptr<GameObject> object = iter->second;
        if (object)
        {
            if (m_CurrentActiveCharacter && m_CurrentActiveCharacter->GetObjectID() == object->GetID())
            {
                m_CurrentActiveCharacter->SetObject(nullptr);
                m_DetachedCharacterObjectId = object->GetID();
            }
        }
    }

    m_Gobjects.erase(go_id);
}

bool Level::RestoreGObject(const std::shared_ptr<GameObject>& gobject)
{
    if (gobject == nullptr || gobject->GetID() == k_invalid_gobject_id)
    {
        return false;
    }

    const auto inserted = m_Gobjects.emplace(gobject->GetID(), gobject);
    if (!inserted.second)
    {
        LOG_ERROR(ZLevel, "RestoreGObject: id {} ('{}') is already in use", gobject->GetID(), gobject->GetName().c_str());
        return false;
    }

    // DeleteGObjectByID unbinds the active character when it deletes the character's
    // object; rebind it so undoing that delete gives the player its pawn back.
    if (m_CurrentActiveCharacter != nullptr && m_DetachedCharacterObjectId == gobject->GetID())
    {
        m_CurrentActiveCharacter->SetObject(gobject);
        m_DetachedCharacterObjectId = k_invalid_gobject_id;
    }
    return true;
}
