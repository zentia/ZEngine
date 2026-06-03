#pragma once

#include "Editor/EditorFileService/EditorFileService.h"
#include "function/framework/Object/ObjectIdAllocator.h"

#include <EASTL/unordered_map.h>
#include <EASTL/string.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Mutable Project-window state passed to extracted UI modules.
struct ProjectWindowContext
{
    EditorFileService& file_service;
    EditorFileNode*& selected_node;
    eastl::unordered_map<eastl::string, unsigned int>& new_object_index_map;

    std::filesystem::path& pending_delete_path;
    bool& has_pending_delete;

    std::filesystem::path& renaming_target_path;
    bool& is_renaming;
    bool& rename_focus_pending;
    std::array<char, 512>& rename_buffer;

    GObjectID& pending_prefab_source_gid;
    std::filesystem::path& pending_prefab_target_folder;
    bool& has_pending_prefab_create;

    std::vector<std::string>& pending_os_drop_imports;
    std::mutex& os_drop_mutex;

    std::filesystem::path& pending_material_shader_path;
    bool& has_pending_material_from_shader;

    bool& pending_import_dialog;
    bool& asset_tree_dirty;
};
