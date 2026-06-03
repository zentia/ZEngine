#include "Editor/EditorUI/ProjectWindow/ProjectWindowHelpers.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace ProjectWindowHelpers
{
    bool IsFolderNode(const EditorFileNode* node)
    {
        return node != nullptr && node->isFolder();
    }

    eastl::string GetProjectDisplayName(const EditorFileNode* node)
    {
        if (node == nullptr)
        {
            return "";
        }

        if (IsFolderNode(node))
        {
            return node->m_FileName;
        }

        const std::filesystem::path file_name_path(node->m_FileName.c_str());
        const std::string stem = file_name_path.stem().generic_string();
        return stem.empty() ? node->m_FileName : stem.c_str();
    }

    std::filesystem::path GetEditorSourceAssetFolder()
    {
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info != nullptr)
        {
            const std::filesystem::path project_content = project_info->GetProjectContent();
            if (!project_content.empty())
            {
                std::filesystem::create_directories(project_content);
                return std::filesystem::absolute(project_content);
            }

            const std::filesystem::path project_root = project_info->GetProjectRoot();
            if (!project_root.empty())
            {
                return std::filesystem::absolute(project_root);
            }
        }

        return std::filesystem::absolute(GET_SYSTEM(ConfigManager)->GetAssetFolder());
    }

    bool WriteTextFileIfMissing(const std::filesystem::path& path, const std::string& content)
    {
        std::error_code error_code;
        if (std::filesystem::exists(path, error_code))
        {
            return true;
        }

        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error_code);
        }

        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file.is_open())
        {
            return false;
        }

        file << content;
        return file.good();
    }

    bool IsShaderAssetNode(const EditorFileNode* node)
    {
        if (node == nullptr || IsFolderNode(node) || node->m_FilePath.empty())
        {
            return false;
        }

        std::string extension = std::filesystem::path(node->m_FilePath.c_str()).extension().generic_string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".shader";
    }

    bool IsZassetProductNode(const EditorFileNode* node)
    {
        if (node == nullptr || IsFolderNode(node) || node->m_FilePath.empty())
        {
            return false;
        }
        std::string extension = std::filesystem::path(node->m_FilePath.c_str()).extension().generic_string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".zasset" || extension == ".mat" || extension == ".prefab" || extension == ".scene";
    }

    std::filesystem::path NormalizeProjectPath(const std::filesystem::path& path)
    {
        std::error_code error_code;
        if (std::filesystem::exists(path, error_code))
        {
            return std::filesystem::weakly_canonical(path, error_code).lexically_normal();
        }
        return std::filesystem::absolute(path).lexically_normal();
    }

    EditorFileNode* FindProjectNodeByPath(EditorFileNode* node, const std::filesystem::path& path)
    {
        if (node == nullptr)
        {
            return nullptr;
        }

        if (!node->m_FilePath.empty() && NormalizeProjectPath(node->m_FilePath.c_str()) == NormalizeProjectPath(path))
        {
            return node;
        }

        for (const std::shared_ptr<EditorFileNode>& child_node : node->m_ChildNodes)
        {
            if (EditorFileNode* found_node = FindProjectNodeByPath(child_node.get(), path))
            {
                return found_node;
            }
        }
        return nullptr;
    }

    EditorFileNode* FindProjectNodeAcrossRoots(EditorFileService& service, const std::filesystem::path& path)
    {
        for (EditorFileNode* root : service.getEditorRootNodes())
        {
            if (EditorFileNode* found = FindProjectNodeByPath(root, path))
            {
                return found;
            }
        }
        return nullptr;
    }

    void TrimRenameBufferInPlace(char* buffer, size_t capacity)
    {
        if (buffer == nullptr || capacity == 0)
        {
            return;
        }

        size_t len = std::strlen(buffer);
        while (len > 0 && std::isspace(static_cast<unsigned char>(buffer[len - 1])))
        {
            buffer[--len] = '\0';
        }

        size_t start = 0;
        while (buffer[start] != '\0' && std::isspace(static_cast<unsigned char>(buffer[start])))
        {
            ++start;
        }
        if (start > 0)
        {
            std::memmove(buffer, buffer + start, len - start + 1);
        }
    }

    bool IsValidRenameName(const std::string& name)
    {
        if (name.empty() || name == "." || name == "..")
        {
            return false;
        }

        return name.find_first_of("\\/:*?\"<>|") == std::string::npos;
    }

    bool IsProtectedRenamePath(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return true;
        }

        const std::filesystem::path normalized = NormalizeProjectPath(path);
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return false;
        }

        auto is_same_root = [&](const std::filesystem::path& root) {
            if (root.empty())
            {
                return false;
            }
            std::error_code ec;
            const std::filesystem::path abs_root = std::filesystem::absolute(root, ec);
            return !ec && NormalizeProjectPath(abs_root) == normalized;
        };

        return is_same_root(project_info->GetProjectContent()) || is_same_root(project_info->GetScriptsRoot()) ||
               is_same_root(project_info->GetShadersRoot()) || is_same_root(project_info->GetDataRoot()) ||
               is_same_root(project_info->GetTexturesRoot()) || is_same_root(project_info->GetModelsRoot());
    }

    void UnloadAssetStreamsUnderPath(const std::filesystem::path& target_path)
    {
        AssetManager* asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr || target_path.empty())
        {
            return;
        }

        std::error_code error_code;
        if (std::filesystem::is_directory(target_path, error_code))
        {
            asset_manager->UnloadStream(target_path);
            error_code.clear();
            for (std::filesystem::recursive_directory_iterator iterator(target_path,
                                                                        std::filesystem::directory_options::skip_permission_denied,
                                                                        error_code),
                 end;
                 iterator != end;
                 iterator.increment(error_code))
            {
                if (error_code)
                {
                    error_code.clear();
                    continue;
                }

                std::error_code entry_error;
                if (std::filesystem::is_regular_file(iterator->path(), entry_error))
                {
                    asset_manager->UnloadStream(iterator->path());
                }
            }
            return;
        }

        asset_manager->UnloadStream(target_path);
    }

    std::filesystem::path ResolveDropTargetFolder(const EditorFileNode* node)
    {
        std::filesystem::path folder = GetEditorSourceAssetFolder();
        if (node != nullptr && !node->m_FilePath.empty())
        {
            std::filesystem::path candidate = node->m_FilePath.c_str();
            if (!IsFolderNode(node))
            {
                candidate = candidate.parent_path();
            }
            if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate))
            {
                folder = candidate;
            }
        }
        return folder;
    }

    std::filesystem::path BuildUniquePrefabAssetPath(const std::filesystem::path& folder, const eastl::string& base_name)
    {
        const std::string sanitized_base = base_name.empty() ? std::string("NewPrefab") : base_name.c_str();
        // Prefabs are authoring data -> human-readable YAML object graph
        // (.prefab), peer of scenes (.scene). DDC-imported assets stay binary
        // .zasset. See doc/serialization/TEXT_SERIALIZED_FILE.md.
        const std::string extension = ".prefab";

        std::filesystem::path candidate = folder / (sanitized_base + extension);
        int counter = 1;
        while (std::filesystem::exists(candidate))
        {
            candidate = folder / (sanitized_base + "_" + std::to_string(counter) + extension);
            ++counter;
        }
        return candidate;
    }

    GameObject* ResolveLiveGameObject(GObjectID id)
    {
        if (id == k_invalid_gobject_id)
        {
            return nullptr;
        }
        Level* level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (level == nullptr)
        {
            return nullptr;
        }
        return level->GetGObjectByID(id).lock().get();
    }
}
