#include "EditorFileService.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Runtime/File/FileSystem.h"
#include "Runtime/Platform/Path/Path.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>

bool isLikelyJsonTextFile(const std::filesystem::path& file_path)
{
    std::ifstream file_stream(file_path, std::ios::binary);
    if (!file_stream.is_open())
    {
        return false;
    }

    char ch = 0;
    while (file_stream.get(ch))
    {
        if (!std::isspace(static_cast<unsigned char>(ch)))
        {
            return ch == '{' || ch == '[';
        }
    }
    return false;
}

/// helper function: split the input string with separator, and filter the substring

std::vector<std::string>
splitString(std::string input_string, const std::string& separator, const std::string& filter_string = "")
{
    std::vector<std::string> output_string;
    int pos = input_string.find(separator);
    std::string add_string;

    while (pos != std::string::npos)
    {
        add_string = input_string.substr(0, pos);
        if (!add_string.empty())
        {
            if (!filter_string.empty() && add_string == filter_string)
            {
                // filter substring
            }
            else
            {
                output_string.push_back(add_string);
            }
        }
        input_string.erase(0, pos + 1);
        pos = input_string.find(separator);
    }
    add_string = input_string;
    if (!add_string.empty())
    {
        output_string.push_back(add_string);
    }
    return output_string;
}

std::filesystem::path getEditorSourceAssetFolder()
{
    const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
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

// Per-root display whitelist (UE Content Browser model).
//
// Each top-level root in the Project window has its OWN allowed-extension
// set, so a misplaced file (e.g. a stray `.ts` dropped into `Assets/`) is
// silently hidden instead of polluting the wrong tree. This mirrors UE's
// rule that .uasset lives only under Content/ while .cpp/.h live only
// under Source/.
//
// AGENTS.md 2.1/2.2 rationale:
//   * `Assets/`  -> binary `.zasset` products + legacy `.json` text assets.
//                   Source files (.png/.fbx/.wav/.tga/.gltf/.obj/...) are
//                   NEVER surfaced here; they only enter through the
//                   Import dialog (AssetsMenu::ConvertAsset) which emits a
//                   `.zasset` under Assets/. Text source files
//                   (.ts/.hlsl/.csv/...) belong in their own root, not in
//                   Assets/, even if a user accidentally drops them here.
//   * `Scripts/` -> TypeScript / JavaScript source.
//   * `Shaders/` -> HLSL / shader-graph / compute / raytrace source.
//   * `Data/`    -> CSV (V1) / XLSX (V2) data-table source. Compiled
//                   `.zasset` products land under
//                   `<Assets>/_Generated/Data/` and surface inside the
//                   Assets tree via the Assets-root whitelist above.
//   * `Textures/`-> image source (.png/.jpg/.jpeg/.tga/.bmp). Imported
//                   Texture2D `.zasset` products land under `Assets/`.
//   * `Models/`  -> mesh source (.fbx/.obj/.gltf/.glb). Imported mesh
//                   `.zasset` products land under `Assets/`.
//
// The `root_label` is the lower-case identifier passed to BuildRoot()
// (`"asset"`, `"scripts"`, `"shaders"`, `"data"`, `"textures"`, `"models"`).
// Unknown labels fall through to the most permissive set (Assets-root rules)
// for forward compatibility, but every existing root is enumerated explicitly
// so a new root must opt in deliberately.
bool shouldDisplayInProjectWindow(const std::filesystem::path& path,
                                  const eastl::string& root_label)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Scripts/ root: TypeScript / JavaScript source only.
    if (root_label == "scripts")
    {
        return extension == ".ts" || extension == ".tsx" || extension == ".js";
    }

    // Shaders/ root: shader source only.
    if (root_label == "shaders")
    {
        return extension == ".hlsl" || extension == ".shader" ||
               extension == ".compute" || extension == ".raytrace";
    }

    // Data/ root: CSV (V1) and XLSX (V2) data-table source.
    if (root_label == "data")
    {
        return extension == ".csv" || extension == ".xlsx";
    }

    // Textures/ root: image source for Texture2D import.
    if (root_label == "textures")
    {
        return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
               extension == ".tga" || extension == ".bmp";
    }

    // Models/ root: mesh source for mesh import.
    if (root_label == "models")
    {
        return extension == ".fbx" || extension == ".obj" || extension == ".gltf" ||
               extension == ".glb";
    }

    // Assets/ root (and any unknown root, conservatively): binary asset
    // products plus legacy .json text assets only. Source files for the
    // other roots are deliberately NOT surfaced here -- they belong in
    // Scripts/, Shaders/, or Data/, even when misplaced.
    return extension == ".json" || extension == ".zasset" ||
           extension == ".scene" || extension == ".prefab" || extension == ".mat";
}

// Compute the lower-case file-extension label (no leading dot) used to
// route a file by its on-disk extension only -- never by reading the file
// header. This is the analogue of UE's FPaths::GetExtension() / Unity's
// Path.GetExtension(). Returns empty for files we don't surface.
//
// Multi-extension shader sources collapse to "hlsl" here (the .vert/.frag
// secondary extension lives in m_AssetType instead), and JSON variants
// stay "json" with their structural sub-type also moved to m_AssetType.
std::string computeFileExtensionLabel(const std::filesystem::path& file_path)
{
    const auto& extensions = Path::GetFileExtensions(file_path);
    std::string ext = std::get<0>(extensions);
    if (ext.empty())
    {
        return {};
    }
    // Drop leading dot, lower-case.
    if (!ext.empty() && ext.front() == '.')
    {
        ext.erase(0, 1);
    }
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// P9 fast path: ask the AssetRegistry for the cached asset_type of a
// .zasset before falling back to the per-call header read inside
// computeAssetTypeLabel. Returns the label in the SAME shape that
// computeAssetTypeLabel would return ("prefab" / "material" / ... or
// "asset" for legacy JSON .zassets), or empty when the registry doesn't
// have an entry yet (cold scan still warming up, or non-.zasset asked).
//
// The registry's m_AssetMap is keyed by paths relative to the scan_root
// (the project's working_dir). buildRoot calls us with absolute paths,
// so we hand the absolute path to getAssetIndex which now normalises
// internally -- but the existing API takes a string key directly, so we
// reconstruct the relative form here using the scan_root the registry
// already remembers.
std::string computeAssetTypeLabelFromRegistry(const std::filesystem::path& file_path,
                                              const std::string& file_extension)
{
    if (file_extension != "zasset" && file_extension != "mat" && file_extension != "prefab" &&
        file_extension != "scene")
    {
        return {};
    }
    auto am = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager));
    if (am == nullptr)
    {
        return {};
    }
    const AssetRegistry& reg = const_cast<EditorAssetManager*>(am.get())->getAssetRegistry();

    // Walk the relative shape from the same scan_root the registry used.
    // ProjectInfo::m_WorkingDir is what EditorAssetManager::Initialize
    // passed in -- using getProjectRoot here keeps us in sync without
    // exposing m_ScanRoot through the registry's public surface.
    const auto pi = GET_SYSTEM(ProjectInfo);
    if (pi == nullptr)
    {
        return {};
    }
    std::filesystem::path scan_root = pi->m_WorkingDir;
    if (scan_root.empty())
    {
        return {};
    }

    std::error_code ec;
    auto rel = std::filesystem::relative(file_path, scan_root, ec);
    if (ec || rel.empty())
    {
        return {};
    }
    auto idx = reg.GetAssetIndex(rel.generic_string());
    if (!idx.has_value())
    {
        return {};
    }
    std::string asset_type = idx->asset_type;
    if (asset_type.empty())
    {
        return {};
    }
    // Match computeAssetTypeLabel's normalisation: strip trailing "Res"
    // and lower-case. Registry stores the raw reflection class name.
    if (asset_type.size() > 3 && asset_type.compare(asset_type.size() - 3, 3, "Res") == 0)
    {
        asset_type.erase(asset_type.size() - 3);
    }
    std::transform(asset_type.begin(), asset_type.end(), asset_type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return asset_type;
}

// Compute the asset-type label for a binary .zasset by reading its header
// (analogue of UE's FAssetData::AssetClassPath / Unity's
// AssetDatabase.GetMainAssetTypeAtPath()). Returns:
//   - "prefab" / "material" / "mesh" / ...  for genuine binary .zassets,
//     normalised by stripping the trailing "Res" and lower-casing.
//   - "asset"                                for .zasset files that are
//     actually JSON (legacy text assets).
//   - ""                                     for non-.zasset files OR a
//     .zasset whose header didn't yield a runtime type.
//
// For multi-extension files (.vert.hlsl, .transform.component.json) the
// secondary extension is surfaced here so a single field carries the
// "more specific" classification without consumers having to re-parse the
// path. For single-extension non-.zasset files (.ts, .hlsl, .shader) this
// returns empty -- m_FileExtension already carries everything.
std::string computeAssetTypeLabel(const std::filesystem::path& file_path,
                                  const std::string& file_extension)
{
    if (file_extension == "zasset")
    {
        // P9 fast path: AssetRegistry already cached this on first scan.
        // We only reach the header-read fallback when the registry hasn't
        // seen this file yet (e.g. very first frames after project open
        // or a .zasset outside the scan root). Without this short-circuit
        // every Project-window rebuild re-opened every .zasset on disk
        // (~90% of cold-startup cost on the demo project).
        std::string cached = computeAssetTypeLabelFromRegistry(file_path, file_extension);
        if (!cached.empty())
        {
            return cached;
        }

        if (isLikelyJsonTextFile(file_path))
        {
            return "asset";
        }
        std::string asset_type = GET_SYSTEM(AssetManager)->GetAssetTypeName(file_path);
        if (asset_type.empty())
        {
            return "asset";
        }
        if (asset_type.size() > 3 && asset_type.compare(asset_type.size() - 3, 3, "Res") == 0)
        {
            asset_type.erase(asset_type.size() - 3);
        }
        std::transform(asset_type.begin(), asset_type.end(), asset_type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return asset_type;
    }

    const auto& extensions = Path::GetFileExtensions(file_path);
    if (file_extension == "json")
    {
        std::string sub = std::get<1>(extensions);
        if (sub == ".component")
        {
            sub = std::get<2>(extensions) + std::get<1>(extensions);
        }
        if (!sub.empty() && sub.front() == '.')
        {
            sub.erase(0, 1);
        }
        std::transform(sub.begin(), sub.end(), sub.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return sub;
    }
    if (file_extension == "hlsl")
    {
        std::string stage = std::get<1>(extensions);
        if (!stage.empty() && stage.front() == '.')
        {
            stage.erase(0, 1);
        }
        std::transform(stage.begin(), stage.end(), stage.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return stage;
    }
    if (file_extension == "mat" || file_extension == "prefab" || file_extension == "scene")
    {
        std::string cached = computeAssetTypeLabelFromRegistry(file_path, file_extension);
        if (!cached.empty())
        {
            return cached;
        }
        const std::string runtime_asset_type = GET_SYSTEM(AssetManager)->GetAssetTypeName(file_path);
        if (runtime_asset_type.empty())
        {
            if (file_extension == "prefab")
            {
                return "prefab";
            }
            if (file_extension == "scene")
            {
                return "level";
            }
            return file_extension;
        }
        std::string asset_type = runtime_asset_type;
        if (asset_type.size() > 3 && asset_type.compare(asset_type.size() - 3, 3, "Res") == 0)
        {
            asset_type.erase(asset_type.size() - 3);
        }
        std::transform(asset_type.begin(), asset_type.end(), asset_type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return asset_type;
    }
    return {};
}

void EditorFileService::BuildEngineFileTree()
{
    // Reset both the flat node array and the per-root pointer list. We rebuild
    // from scratch on every call (the tree is small and rebuild is fast).
    m_FileNodeArray.clear();
    m_RootNodes.clear();

    // Root 1: Assets/ (always present; legacy single-root callers see this
    // via getEditorRootNode()).
    const std::filesystem::path assets_root = getEditorSourceAssetFolder();
    if (!assets_root.empty())
    {
        EditorFileNode* node = BuildRoot(assets_root, "asset", "Assets");
        if (node != nullptr)
        {
            m_RootNodes.push_back(node);
        }
    }

    // Root 2: Scripts/ (peer of Assets/, UE-style placement; see AGENTS.md
    // 2.2). Only added when ProjectInfo can resolve a real Scripts folder
    // path - on the launcher / "no project loaded" screen this stays empty.
    const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
    if (project_info != nullptr)
    {
        std::filesystem::path scripts_root = project_info->GetScriptsRoot();
        if (!scripts_root.empty())
        {
            std::error_code ec;
            // Don't probe the existence eagerly; EnsureScriptsScaffold()
            // already creates the directory on Initialize(). If for some
            // reason it isn't there yet, BuildRoot() returns null and we
            // simply don't show a Scripts root this frame.
            scripts_root = std::filesystem::absolute(scripts_root, ec);
            EditorFileNode* node = BuildRoot(scripts_root, "scripts", "Scripts");
            if (node != nullptr)
            {
                m_RootNodes.push_back(node);
            }
        }

        // Root 3: Shaders/ (peer of Assets/ and Scripts/; AGENTS.md 2.2).
        // Same lifecycle as the Scripts root: created by
        // EnsureScriptsScaffold() at project open, shown as a top-level
        // tree, hidden when no project is loaded.
        std::filesystem::path shaders_root = project_info->GetShadersRoot();
        if (!shaders_root.empty())
        {
            std::error_code ec;
            shaders_root = std::filesystem::absolute(shaders_root, ec);
            EditorFileNode* node = BuildRoot(shaders_root, "shaders", "Shaders");
            if (node != nullptr)
            {
                m_RootNodes.push_back(node);
            }
        }

        // Root 4: Data/ (peer of Assets/, Scripts/, Shaders/). Holds CSV/XLSX
        // source tables. Same lifecycle as the other source roots: created
        // by EnsureScriptsScaffold() at project open, hidden when no
        // project is loaded. Compiled .zasset products live under
        // <content>/_Generated/Data/ and surface inside the Assets root
        // automatically (gitignored, see ProjectInfo.cpp).
        std::filesystem::path data_root = project_info->GetDataRoot();
        if (!data_root.empty())
        {
            std::error_code ec;
            data_root = std::filesystem::absolute(data_root, ec);
            EditorFileNode* node = BuildRoot(data_root, "data", "Data");
            if (node != nullptr)
            {
                m_RootNodes.push_back(node);
            }
        }

        // Root 5: Textures/ (peer of Assets/, Scripts/, Shaders/, Data/).
        // Holds image sources imported into Texture2D .zasset under Assets/.
        std::filesystem::path textures_root = project_info->GetTexturesRoot();
        if (!textures_root.empty())
        {
            std::error_code ec;
            textures_root = std::filesystem::absolute(textures_root, ec);
            EditorFileNode* node = BuildRoot(textures_root, "textures", "Textures");
            if (node != nullptr)
            {
                m_RootNodes.push_back(node);
            }
        }

        // Root 6: Models/ (peer of the other source roots). Holds mesh
        // sources imported into mesh .zasset under Assets/.
        std::filesystem::path models_root = project_info->GetModelsRoot();
        if (!models_root.empty())
        {
            std::error_code ec;
            models_root = std::filesystem::absolute(models_root, ec);
            EditorFileNode* node = BuildRoot(models_root, "models", "Models");
            if (node != nullptr)
            {
                m_RootNodes.push_back(node);
            }
        }
    }
}

EditorFileNode* EditorFileService::BuildRoot(const std::filesystem::path& root_path,
                                             const eastl::string& root_label,
                                             const eastl::string& display_name)
{
    if (root_path.empty())
    {
        return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::exists(root_path, ec))
    {
        return nullptr;
    }

    const std::string root_path_str = root_path.generic_string();

    // Add the synthetic top-level folder node first. m_NodeDepth = -1 keeps
    // it parent-of-everything, matching the previous Assets-only behaviour.
    auto root_node = std::make_shared<EditorFileNode>();
    root_node->m_FileName = display_name.empty() ? root_label : display_name;
    root_node->m_FileExtension = "folder";
    root_node->m_AssetType.clear();
    root_node->m_FilePath = root_path_str.c_str();
    root_node->m_NodeDepth = -1;
    m_FileNodeArray.push_back(root_node);

    const std::vector<std::filesystem::path> file_paths = GET_SYSTEM(FileSystem)->GetFiles(root_path_str);
    std::vector<std::filesystem::path> project_file_paths;
    std::vector<std::vector<eastl::string>> all_file_segments;
    project_file_paths.reserve(file_paths.size());
    for (const auto& path : file_paths)
    {
        if (!shouldDisplayInProjectWindow(path, root_label))
        {
            continue;
        }

        const std::filesystem::path& relative_path = Path::GetRelativePath(root_path_str, path);
        project_file_paths.push_back(path);
        all_file_segments.emplace_back(Path::GetPathSegments(relative_path));
    }

    std::vector<std::shared_ptr<EditorFileNode>> node_array;

    int all_file_segments_count = static_cast<int>(all_file_segments.size());
    for (int file_index = 0; file_index < all_file_segments_count; file_index++)
    {
        int depth = 0;
        node_array.clear();
        node_array.push_back(root_node);
        int file_segment_count = static_cast<int>(all_file_segments[file_index].size());
        for (int file_segment_index = 0; file_segment_index < file_segment_count; file_segment_index++)
        {
            auto file_node = std::make_shared<EditorFileNode>();
            file_node->m_FileName = all_file_segments[file_index][file_segment_index];
            if (depth < file_segment_count - 1)
            {
                std::filesystem::path folder_path = root_path_str;
                for (int segment_index = 0; segment_index <= file_segment_index; ++segment_index)
                {
                    folder_path /= all_file_segments[file_index][segment_index].c_str();
                }
                file_node->m_FileExtension = "folder";
                file_node->m_AssetType.clear();
                file_node->m_FilePath = folder_path.generic_string().c_str();
            }
            else
            {
                file_node->m_FileExtension = computeFileExtensionLabel(project_file_paths[file_index]);
                if (file_node->m_FileExtension.empty())
                    continue;
                file_node->m_AssetType =
                    computeAssetTypeLabel(project_file_paths[file_index], file_node->m_FileExtension);
                file_node->m_FilePath = project_file_paths[file_index].generic_string().c_str();
            }
            file_node->m_NodeDepth = depth;
            node_array.push_back(file_node);

            bool node_exists = CheckFileArray(file_node.get());
            if (!node_exists)
            {
                m_FileNodeArray.push_back(file_node);
            }
            EditorFileNode* parent_node_ptr = GetParentNodePtr(node_array[depth].get());
            if (parent_node_ptr != nullptr && node_exists == false)
            {
                parent_node_ptr->m_ChildNodes.push_back(file_node);
            }
            depth++;
        }
    }

    return root_node.get();
}

bool EditorFileService::CheckFileArray(EditorFileNode* file_node)
{
    int editor_node_count = m_FileNodeArray.size();
    for (int file_node_index = 0; file_node_index < editor_node_count; file_node_index++)
    {
        if (m_FileNodeArray[file_node_index]->m_FileName == file_node->m_FileName &&
            m_FileNodeArray[file_node_index]->m_NodeDepth == file_node->m_NodeDepth &&
            m_FileNodeArray[file_node_index]->m_FilePath == file_node->m_FilePath)
        {
            return true;
        }
    }
    return false;
}

EditorFileNode* EditorFileService::GetParentNodePtr(EditorFileNode* file_node)
{
    int editor_node_count = m_FileNodeArray.size();
    for (int file_node_index = 0; file_node_index < editor_node_count; file_node_index++)
    {
        if (m_FileNodeArray[file_node_index]->m_FileName == file_node->m_FileName &&
            m_FileNodeArray[file_node_index]->m_NodeDepth == file_node->m_NodeDepth &&
            m_FileNodeArray[file_node_index]->m_FilePath == file_node->m_FilePath)
        {
            return m_FileNodeArray[file_node_index].get();
        }
    }
    return nullptr;
}