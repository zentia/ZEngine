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
        AssetManager* asset_manager = GET_SYSTEM(AssetManager).get();
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
        AssetManager* asset_manager = GET_SYSTEM(AssetManager).get();
        if (asset_manager == nullptr)
        {
            return {};
        }

        std::filesystem::path level_path = asset_manager->ResolveProjectContentPath(level_url);
        const std::string extension = LowerExtension(level_path);
        if (extension == ".json")
        {
            // Legacy engine paths like asset/level/1-1.level.json -> Assets/.../1-1.scene
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

void Level::clear()

{
    m_TransformSceneRoots.Clear();
    m_CurrentActiveCharacter.reset();
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

    const std::filesystem::path level_full_path = ResolveLevelLoadPath(levelPath);
    if (LowerExtension(level_full_path) == ".scene")
    {
        std::vector<std::pair<int64_t, Object*>> entries;
        if (!GET_SYSTEM(AssetManager)->ReadObjectsFromYaml(level_full_path, entries))
        {
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
        RebuildAllTransformHierarchies();
        LOG_INFO(ZLevel, "level load succeed (yaml)");
        return true;
    }

    LevelRes* level_res = GET_SYSTEM(AssetManager)->loadAsset<LevelRes>(levelPath);
    if (!level_res)
    {
        return false;
    }

    m_PhysicsScene = GET_SYSTEM(PhysicsManager)->CreatePhysicsScene(level_res->m_Gravity);
    ParticleEmitterIDAllocator::reset();

    for (const GameObject* gameObject : level_res->m_Objects)
    {
        CreateObject(*gameObject);
    }

    // create active character
    for (const auto& object_pair : m_Gobjects)
    {
        std::shared_ptr<GameObject> object = object_pair.second;
        if (object == nullptr)
            continue;

        if (level_res->m_CharacterName == object->GetName())
        {
            m_CurrentActiveCharacter = std::make_shared<Character>(object);
            break;
        }
    }

    m_IsLoaded = true;
    RebuildAllTransformHierarchies();

    LOG_INFO(ZLevel, "level load succeed");

    return true;
}

void Level::Unload()
{
    FlushRenderDeletes();
    clear();
    LOG_INFO(ZLevel, "unload level: {}", m_LevelResUrl.c_str());
}

bool Level::save()
{
    LOG_INFO(ZLevel, "saving level: {}", m_LevelResUrl.c_str());
    LevelRes output_level_res;

    const size_t object_count = m_Gobjects.size();
    std::vector<std::unique_ptr<GameObject>> saved_objects;
    saved_objects.reserve(object_count);

    std::vector<GameObject*>& output_objects = output_level_res.m_Objects;
    output_objects.reserve(object_count);

    for (const auto& id_object_pair : m_Gobjects)
    {
        if (id_object_pair.second == nullptr)
        {
            continue;
        }

        std::unique_ptr<GameObject> saved_object = std::make_unique<GameObject>(id_object_pair.first);
        id_object_pair.second->save(*saved_object);
        output_objects.push_back(saved_object.get());
        saved_objects.emplace_back(std::move(saved_object));
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager).get();
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

    bool is_save_success = false;
    if (LowerExtension(level_path) == ".scene")
    {
        // YAML multi-object graph: a LevelRes header (fileID 1) carrying scene
        // settings, then every live GameObject and its Components as their own
        // objects. The GameObject->Component links serialize as local fileID
        // references (see YamlObjectGraph). Imported assets referenced by
        // components stay binary and are linked by GUID through the externals
        // table.
        std::vector<Object*> graph_objects;
        std::vector<int64_t> graph_ids;

        LevelRes* header = static_cast<LevelRes*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<LevelRes>(), 0));
        if (header == nullptr)
        {
            LOG_ERROR(ZLevel, "failed to allocate LevelRes header for {}", m_LevelResUrl.c_str());
            return false;
        }
        header->m_Gravity = output_level_res.m_Gravity;
        header->m_CharacterName = output_level_res.m_CharacterName;
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

        is_save_success =
            asset_manager->WriteObjectsToYaml(level_path, graph_objects.data(), graph_ids.data(), graph_objects.size());

        MemoryManager::DestroyObject(header);
    }
    else
    {
        is_save_success = asset_manager->WriteObjectToDiskThreadSafe(level_path, output_level_res);
    }

    if (!is_save_success)
    {
        LOG_ERROR(ZLevel, "failed to save {}", level_path.generic_string());
    }
    else
    {
        LOG_INFO(ZLevel, "level save succeed -> {}", level_path.generic_string());
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
    if (m_CurrentActiveCharacter && g_isEditorMode == false)
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
            }
        }
    }

    m_Gobjects.erase(go_id);
}
