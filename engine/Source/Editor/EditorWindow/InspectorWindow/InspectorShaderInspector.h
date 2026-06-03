#pragma once

#include "MaterialInspectorRow.h"

#include <EASTL/string.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Material;
class ShaderRes;

namespace ZSlate
{
class SWidget;
}

// Native ZSlate Shader inspector builder. Returns a widget tree for either a
// `.shader` (ShaderLab) source or a `.zasset` ShaderRes. Source editing is
// delegated to the external editor (Open/Reveal); inline code editing is not
// provided (ZSlate has no multi-line code editor widget). `request_rebuild` is
// invoked when an action (refresh / save / compile / reimport) changes state
// the retained widget tree must re-read. Lives in the shader-inspector TU so it
// can reach the file-local parse / IO helpers.
std::shared_ptr<ZSlate::SWidget> BuildShaderInspectorWidget(const std::filesystem::path& asset_path,
                                                            float scale,
                                                            const std::function<void()>& request_rebuild);

// ----------------------------------------------------------------------------
// UI-agnostic material property enumeration (shared by the legacy ImGui drawer
// and the native ZSlate inspector). The row type lives in MaterialInspectorRow.h.
//
// IMPORTANT: the returned pointers are only valid as long as `material` lives
// and is not structurally mutated (the enumerators pre-reserve the property
// vectors so creating defaults during enumeration does not invalidate earlier
// rows). Re-enumerate after any reload.
// ----------------------------------------------------------------------------

// Built-in (StandardLit / no custom project shader) property rows.
std::vector<MaterialInspectorRow> EnumerateBuiltInMaterialRows(Material& material);

// Rows reflected from a loaded project shader's properties. `out_created` is set
// true when enumerating materialised a new default property on the material
// (caller should persist the asset in that case).
std::vector<MaterialInspectorRow> EnumerateShaderMaterialRows(Material& material,
                                                              const ShaderRes& shader,
                                                              bool& out_created);

// Keyword options (multi_compile / shader_feature) for the shader's variants,
// or empty when the shader has none / is built-in.
std::vector<std::string> CollectMaterialShaderVariantKeywords(const eastl::string& shader_name,
                                                              const std::filesystem::path& shader_source_path);

bool LoadProjectShaderDefinitionForInspector(ShaderRes& out_shader,
                                               const eastl::string& shader_name,
                                               std::filesystem::path* resolved_shader_path = nullptr);
std::vector<eastl::string> FindProjectShaders();
void SanitizeMaterialShaderBindingForInspector(Material& material);
void EnsureShaderPassCompatibility(ShaderRes& shader);
void SyncLegacyShaderFieldsFromPrimaryPass(ShaderRes& shader);
eastl::string GetMaterialAuthoringShaderName(const Material& material);
bool IsShaderSourceAvailableInProject(const eastl::string& shader_name);