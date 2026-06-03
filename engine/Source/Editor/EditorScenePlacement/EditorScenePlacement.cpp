#include "EditorScenePlacement.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorHierarchy/EditorHierarchyReparent.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Runtime/Asset/AssetFile.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshRenderer.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Platform/Path/Path.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"
#include "Runtime/Resource/ResType/Components/Mesh.h"
#include "Runtime/Resource/ResType/Data/MeshData.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace
{
    std::filesystem::path getProjectAssetsPath()
    {
        const std::filesystem::path asset_folder = GET_SYSTEM(ConfigManager)->GetAssetFolder();
        if (asset_folder.empty())
        {
            return {};
        }
        return std::filesystem::absolute(asset_folder).lexically_normal();
    }

    bool finalizeCreatedObjectSelection(GObjectID created_object_id)
    {
        if (created_object_id == k_invalid_gobject_id)
        {
            return false;
        }

        auto scene_manager = GET_SYSTEM(EditorSceneManager);
        scene_manager->OnGObjectSelected(created_object_id);
        scene_manager->FocusSelectedGObject();
        scene_manager->DrawSelectedEntityAxis();
        GET_SYSTEM(WorldManager)->MarkCurrentLevelDirty();
        return true;
    }

    bool isMeshAssetTypeLabel(const std::string& normalized_type)
    {
        return normalized_type == "meshdata" || normalized_type == "mesh";
    }

    bool isPrefabAssetTypeLabel(const std::string& normalized_type)
    {
        return normalized_type == "prefab" || normalized_type == "prefabasset";
    }

    std::string readAssetTypeFromZassHeader(const std::filesystem::path& absolute_zasset_path)
    {
        std::ifstream file(absolute_zasset_path, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }

        AssetFileHeader header {};
        file.read(reinterpret_cast<char*>(&header), sizeof(AssetFileHeader));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(AssetFileHeader)) || header.magic != k_zasset_magic)
        {
            return {};
        }

        return EditorScenePlacement::NormalizeDroppedAssetType(
            std::string(header.asset_type, ::strnlen(header.asset_type, sizeof(header.asset_type))));
    }

    std::string lookupAssetTypeInRegistry(const std::filesystem::path& absolute_zasset_path)
    {
        auto editor_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
        if (editor_mgr == nullptr)
        {
            return {};
        }

        const AssetRegistry& registry = editor_mgr->getAssetRegistry();

        std::vector<std::filesystem::path> key_candidates;
        key_candidates.push_back(absolute_zasset_path);

        if (const auto project_info = GET_SYSTEM(ProjectInfo))
        {
            if (!project_info->m_WorkingDir.empty())
            {
                std::error_code ec;
                const std::filesystem::path rel =
                    std::filesystem::relative(absolute_zasset_path, std::filesystem::absolute(project_info->m_WorkingDir), ec);
                if (!ec && !rel.empty())
                {
                    key_candidates.push_back(rel);
                }
            }
        }

        const std::filesystem::path asset_root = getProjectAssetsPath();
        if (!asset_root.empty())
        {
            std::error_code ec;
            const std::filesystem::path rel =
                std::filesystem::relative(absolute_zasset_path, asset_root, ec);
            if (!ec && !rel.empty())
            {
                key_candidates.push_back(rel);
            }
        }

        for (const std::filesystem::path& candidate : key_candidates)
        {
            const std::string key = candidate.generic_string();
            if (const std::optional<AssetIndexEntry> entry = registry.GetAssetIndex(key))
            {
                if (entry.has_value() && !entry->asset_type.empty())
                {
                    return EditorScenePlacement::NormalizeDroppedAssetType(entry->asset_type);
                }
            }
        }

        return {};
    }

    bool probeMeshDataGeometry(const std::filesystem::path& absolute_zasset_path)
    {
        auto asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return false;
        }

        std::filesystem::path read_path = absolute_zasset_path;
        MeshData* mesh = asset_manager->ReadObject<MeshData>(read_path);
        return mesh != nullptr && !mesh->vertex_buffer.empty() && !mesh->index_buffer.empty();
    }

    std::string resolveDroppedAssetType(const std::filesystem::path& absolute_zasset_path)
    {
        if (absolute_zasset_path.empty())
        {
            return {};
        }

        std::string asset_type = readAssetTypeFromZassHeader(absolute_zasset_path);
        if (!asset_type.empty())
        {
            return asset_type;
        }

        asset_type = lookupAssetTypeInRegistry(absolute_zasset_path);
        if (!asset_type.empty())
        {
            return asset_type;
        }

        auto asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager != nullptr)
        {
            asset_type = EditorScenePlacement::NormalizeDroppedAssetType(asset_manager->GetAssetTypeName(absolute_zasset_path));
        }
        return asset_type;
    }

    bool isPrefabZAsset(const std::filesystem::path& absolute_zasset_path)
    {
        if (absolute_zasset_path.empty() || absolute_zasset_path.extension() != ".zasset")
        {
            return false;
        }

        auto asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return false;
        }

        return isPrefabAssetTypeLabel(resolveDroppedAssetType(absolute_zasset_path));
    }

    // A prefab is either a text .prefab YAML object graph (no header read needed --
    // the extension is authoritative) or a legacy binary .zasset whose header type
    // is the Prefab class.
    bool isPrefabFile(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return false;
        }
        if (path.extension() == ".prefab")
        {
            return true;
        }
        return isPrefabZAsset(path);
    }

    bool parentSpawnedRoot(Level* level, GObjectID spawned_root_id, GObjectID parent_gobject_id)
    {
        if (level == nullptr || spawned_root_id == k_invalid_gobject_id ||
            parent_gobject_id == k_invalid_gobject_id)
        {
            return true;
        }

        if (EditorHierarchyReparent::Reparent(level, spawned_root_id, parent_gobject_id))
        {
            return true;
        }

        LOG_WARNING(ZEditor,
                    "Scene placement: spawned '{}' but failed to parent under id {}",
                    spawned_root_id,
                    parent_gobject_id);
        return false;
    }

    bool instantiatePrefabInCurrentLevel(const std::filesystem::path& prefab_path,
                                         GObjectID parent_gobject_id)
    {
        if (!isPrefabFile(prefab_path))
        {
            LOG_WARNING(ZEditor,
                        "Scene placement: refusing prefab instantiate for non-prefab '{}'",
                        prefab_path.string());
            return false;
        }

        Level* current_active_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (current_active_level == nullptr)
        {
            return false;
        }

        GameObject* prefab_root = PrefabUtility::InstantiateFromPath(prefab_path);
        if (prefab_root == nullptr)
        {
            return false;
        }

        const GObjectID new_id = current_active_level->CreateObject(*prefab_root);
        if (new_id == k_invalid_gobject_id)
        {
            return false;
        }

        (void)parentSpawnedRoot(current_active_level, new_id, parent_gobject_id);
        return finalizeCreatedObjectSelection(new_id);
    }

    std::filesystem::path s_pending_drop_path;
    GObjectID s_pending_drop_parent_id {k_invalid_gobject_id};
    bool s_has_pending_drop = false;
}  // namespace

namespace EditorScenePlacement
{
    std::string NormalizeDroppedAssetType(std::string asset_type)
    {
        std::transform(asset_type.begin(), asset_type.end(), asset_type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (asset_type.size() > 3 && asset_type.compare(asset_type.size() - 3, 3, "res") == 0)
        {
            asset_type.erase(asset_type.size() - 3);
        }
        return asset_type;
    }

    std::string MakeProjectRelativeAssetPath(const std::filesystem::path& path)
    {
        const std::filesystem::path asset_root = getProjectAssetsPath();
        const std::filesystem::path normalized_path = std::filesystem::absolute(path).lexically_normal();
        if (asset_root.empty())
        {
            return normalized_path.generic_string();
        }

        const std::filesystem::path relative_path = Path::GetRelativePath(asset_root, normalized_path);
        const std::string relative_string = relative_path.generic_string();
        if (!relative_string.empty() && !(relative_string.size() >= 2 && relative_string[0] == '.' && relative_string[1] == '.'))
        {
            return relative_string;
        }
        return normalized_path.generic_string();
    }

    bool IsMeshDataZAsset(const std::filesystem::path& absolute_zasset_path)
    {
        if (absolute_zasset_path.empty())
        {
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::exists(absolute_zasset_path, ec) || ec)
        {
            return false;
        }

        if (absolute_zasset_path.extension() != ".zasset")
        {
            return false;
        }

        const std::string asset_type = resolveDroppedAssetType(absolute_zasset_path);
        if (isMeshAssetTypeLabel(asset_type))
        {
            return true;
        }
        if (isPrefabAssetTypeLabel(asset_type))
        {
            return false;
        }

        // Header/registry/reflection all failed (log showed type=''). Preview already
        // loads this file as MeshData -- probe-read before rejecting the drop.
        return probeMeshDataGeometry(absolute_zasset_path);
    }

    bool InstantiateMeshAssetInCurrentLevel(const std::filesystem::path& absolute_zasset_path,
                                          GObjectID parent_gobject_id)
    {
        Level* current_active_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (current_active_level == nullptr || absolute_zasset_path.empty())
        {
            return false;
        }

        if (!IsMeshDataZAsset(absolute_zasset_path))
        {
            LOG_WARNING(ZEditor, "Scene placement: '{}' is not a MeshData .zasset", absolute_zasset_path.string());
            return false;
        }

        const std::string mesh_asset_path = MakeProjectRelativeAssetPath(absolute_zasset_path);
        if (mesh_asset_path.empty())
        {
            return false;
        }

        const std::string object_name = absolute_zasset_path.stem().string();

        GameObject object_template;
        object_template.SetName(object_name.c_str());

        Transform* transform_component = MemoryManager::CreateObject<Transform>();
        object_template.addComponent(transform_component);

        MeshRenderer* mesh_renderer = MemoryManager::CreateObject<MeshRenderer>();
        SubMeshRes sub_mesh_res;
        sub_mesh_res.m_MeshAsset = mesh_asset_path.c_str();

        std::vector<SubMeshRes> sub_meshes;
        sub_meshes.push_back(sub_mesh_res);
        mesh_renderer->SetSubMeshes(sub_meshes);
        // No project material: populateMaterialDesc uses StandardLit + engine default textures.
        mesh_renderer->SetSharedMaterialAssets({});
        mesh_renderer->SetForceDoubleSided(true);
        object_template.addComponent(mesh_renderer);

        const GObjectID new_id = current_active_level->CreateObject(object_template);
        if (new_id == k_invalid_gobject_id)
        {
            return false;
        }

        if (std::shared_ptr<GameObject> created_object = current_active_level->GetGObjectByID(new_id).lock())
        {
            if (MeshRenderer* mesh_renderer = created_object->tryGetComponent(MeshRenderer))
            {
                mesh_renderer->OnSerializedFieldsUpdated();
            }
        }

        (void)parentSpawnedRoot(current_active_level, new_id, parent_gobject_id);

        LOG_INFO(ZEditor,
                 "Scene placement: spawned '{}' with mesh '{}'",
                 object_name,
                 mesh_asset_path);
        return finalizeCreatedObjectSelection(new_id);
    }

    bool InstantiateDroppedProjectAsset(const std::filesystem::path& absolute_zasset_path,
                                        GObjectID parent_gobject_id)
    {
        if (absolute_zasset_path.empty())
        {
            return false;
        }

        // Text .prefab YAML graph: route straight to the prefab instantiate path
        // (it has no binary header, so the MeshData probe below would mis-handle it).
        if (absolute_zasset_path.extension() == ".prefab")
        {
            return instantiatePrefabInCurrentLevel(absolute_zasset_path, parent_gobject_id);
        }

        // Mesh first. Never call PrefabUtility on a MeshData .zasset: ReadObject's
        // (path, fileID=1) cache ignores the requested type and would return a
        // previously loaded MeshData* as PrefabAsset*, corrupting ImmediatePtrs.
        if (IsMeshDataZAsset(absolute_zasset_path))
        {
            return InstantiateMeshAssetInCurrentLevel(absolute_zasset_path, parent_gobject_id);
        }

        if (isPrefabZAsset(absolute_zasset_path))
        {
            return instantiatePrefabInCurrentLevel(absolute_zasset_path, parent_gobject_id);
        }

        const std::string asset_type = resolveDroppedAssetType(absolute_zasset_path);
        LOG_WARNING(ZEditor,
                    "Scene placement: unsupported dropped .zasset '{}' (type='{}')",
                    absolute_zasset_path.string(),
                    asset_type);
        return false;
    }

    void RequestDrop(std::filesystem::path absolute_zasset_path, GObjectID parent_gobject_id)
    {
        if (absolute_zasset_path.empty())
        {
            return;
        }
        s_pending_drop_path = std::move(absolute_zasset_path);
        s_pending_drop_parent_id = parent_gobject_id;
        s_has_pending_drop = true;
    }

    void ExecutePendingDrop()
    {
        if (!s_has_pending_drop)
        {
            return;
        }

        const std::filesystem::path path = s_pending_drop_path;
        const GObjectID parent_id = s_pending_drop_parent_id;
        s_pending_drop_path.clear();
        s_pending_drop_parent_id = k_invalid_gobject_id;
        s_has_pending_drop = false;

        (void)InstantiateDroppedProjectAsset(path, parent_id);
    }
}  // namespace EditorScenePlacement
