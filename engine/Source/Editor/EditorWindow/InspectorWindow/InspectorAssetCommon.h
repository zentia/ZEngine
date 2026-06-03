#pragma once

#include <EASTL/string.h>

#include <filesystem>
#include <string>

class Material;

std::string ResolveInspectorAssetType(const std::filesystem::path& asset_path, const std::string& selected_asset_type);
bool IsGenericInspectorZAssetType(const std::filesystem::path& asset_path, const std::string& resolved_asset_type);

// Live material edits in the native ZSlate inspector (not yet saved to disk).
void SetInspectorLiveMaterialSource(const std::filesystem::path& asset_path, const Material* live_material);
void NotifyInspectorLiveMaterialPreviewChanged();
uint64_t GetInspectorLiveMaterialPreviewRevision();
bool TryGetInspectorLiveMaterialForPreview(const std::filesystem::path& asset_path, const Material*& out_material);

// True if `resolved_asset_type` (lowercased, "...Res" suffix stripped, as produced by
// ResolveInspectorAssetType) is a Texture2D asset. Used by the Inspector and Preview
// windows to route texture `.zasset` selections. (The old inline bindless ImGui preview
// that this predicate used to gate was removed; only the type check remains.)
bool IsTexture2DInspectorAssetType(const std::string& resolved_asset_type);
bool LoadMaterialDefinitionForInspector(Material& out_material, const std::filesystem::path& asset_path);
bool MaterialUsesCustomProjectShader(const Material& material);

std::filesystem::path ResolveProjectAssetPath(const eastl::string& stored_path);
std::filesystem::file_time_type GetInspectorFileWriteTime(const std::filesystem::path& path);
