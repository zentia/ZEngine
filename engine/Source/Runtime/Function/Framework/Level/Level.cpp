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
}  // namespace

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

    GET_SYSTEM(PhysicsManager)->DeletePhysicsScene(m_PhysicsScene);
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
    m_PhysicsScene = GET_SYSTEM(PhysicsManager)->CreatePhysicsScene(gravity);
    ParticleEmitterIDAllocator::reset();

    for (GameObject* loaded_object : loaded_objects)
    {
        loaded_object->RebuildRuntimeComponents();
        CreateObject(*loaded_object);
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
    LOG_INFO(ZLevel, "level load succeed ({} objects, {})", loaded_objects.size(), is_binary ? "binary" : "yaml");
    return true;
}

void Level::Unload()
{
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

    const bool is_save_success =
        is_binary ? asset_manager->WriteObjectsToDiskThreadSafe(level_path, graph_objects.data(), graph_ids.data(),
                                                                graph_objects.size())
                  : asset_manager->WriteObjectsToYaml(level_path, graph_objects.data(), graph_ids.data(),
                                                      graph_objects.size());

    MemoryManager::DestroyObject(header);

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
    if (m_CurrentActiveCharacter && g_isPlaying)
    {
        m_CurrentActiveCharacter->Tick(delta_time);
    }

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
        if (camera_component != nullptr && camera_component->isEnabled() && camera_component->isMainCamera())
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
