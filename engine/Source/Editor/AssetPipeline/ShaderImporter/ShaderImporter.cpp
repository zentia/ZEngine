#include "ShaderImporter.h"

#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/ShaderLab/ShaderLabDx12Compiler.h"
#include "Runtime/Function/ShaderLab/ShaderLabParser.h"
#include "Runtime/Function/ShaderLab/ShaderLabVariant.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/Resource/ResType/Data/Shader.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace Runtime
{
    // -------------------------------------------------------------------------
    // PR-SE3b: .shader → ShaderRes .zasset conversion helpers
    // -------------------------------------------------------------------------

    namespace
    {
        std::string NormaliseShaderLookupKey(const eastl::string& shader_name)
        {
            std::string key(shader_name.c_str());
#if defined(_WIN32)
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
            return key;
        }

        ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader CollectMaterialVariantKeysByShader()
        {
            ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader result;

            ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
            AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
            if (project_info == nullptr || asset_mgr == nullptr)
            {
                return result;
            }

            const std::filesystem::path content_root = project_info->GetProjectContent();
            if (content_root.empty())
            {
                return result;
            }

            for (const std::filesystem::path& material_path : asset_mgr->GetAssetsByType("Material", content_root))
            {
                std::filesystem::path read_path = material_path;
                Material* material = asset_mgr->ReadObject<Material>(read_path);
                if (material == nullptr)
                {
                    continue;
                }

                const eastl::string shader_name =
                    material->m_Shader.empty() ? material->GetShaderName() : material->m_Shader;
                if (shader_name.empty() || shader_name == "StandardLit")
                {
                    continue;
                }

                std::vector<std::string> enabled_keywords;
                enabled_keywords.reserve(material->m_EnabledShaderKeywords.size());
                for (const eastl::string& keyword : material->m_EnabledShaderKeywords)
                {
                    if (!keyword.empty())
                    {
                        enabled_keywords.emplace_back(keyword.c_str());
                    }
                }

                const ZEngine::ShaderLab::ShaderVariantKey variant_key =
                    ZEngine::ShaderLab::ShaderVariantKeyFromEnabledKeywords(enabled_keywords);

                const std::string lookup_key = NormaliseShaderLookupKey(shader_name);
                std::vector<ZEngine::ShaderLab::ShaderVariantKey>& bucket = result[lookup_key];
                if (std::find(bucket.begin(), bucket.end(), variant_key) == bucket.end())
                {
                    bucket.push_back(variant_key);
                }
            }

            return result;
        }

        // Convert a parsed ShaderLabAsset into a serialisable ShaderRes.
        // Mirrors the conversion logic that was previously inlined inside
        // inspector_window's LoadProjectShaderAssetByName.
        void ConvertShaderLabAssetToShaderRes(const ZEngine::ShaderLab::ShaderLabAsset& lab_asset,
                                              const std::filesystem::path& source_path,
                                              ShaderRes& out)
        {
            // Name: prefer the parsed shader_name; fall back to filename stem.
            out.m_ShaderName = lab_asset.shader_name.empty()
                                   ? source_path.stem().generic_string().c_str()
                                   : lab_asset.shader_name.c_str();

            // Properties
            out.m_Properties.clear();
            out.m_Properties.reserve(lab_asset.properties.size());
            for (const ZEngine::ShaderLab::ShaderProperty& prop : lab_asset.properties)
            {
                ShaderPropertyDesc desc;
                desc.m_Name = prop.name.c_str();
                desc.m_DisplayName = prop.display_name.c_str();
                desc.m_Type = ZEngine::ShaderLab::PropertyTypeToString(prop.type);
                desc.m_DefaultFloat = prop.default_value.float_value;
                desc.m_RangeMin = prop.default_value.range_min;
                desc.m_RangeMax = prop.default_value.range_max;
                desc.m_DefaultColor = Vector3(prop.default_value.color[0],
                                              prop.default_value.color[1],
                                              prop.default_value.color[2]);
                desc.m_DefaultAlpha = prop.default_value.color[3];
                desc.m_DefaultTexture = prop.default_value.texture_path.c_str();
                desc.m_DefaultToggle = prop.default_value.float_value > 0.5f || prop.default_value.int_value != 0;
                out.m_Properties.push_back(desc);
            }

            // Passes: extract from SubShader → Pass → Programs.
            out.m_Passes.clear();
            for (const auto& subshader : lab_asset.subshaders)
            {
                for (const auto& pass : subshader.passes)
                {
                    ShaderPassDesc pass_desc;
                    pass_desc.m_Name = pass.name.empty() ? "GBuffer" : pass.name.c_str();

                    // Light mode: try Tags["LightMode"], fall back to "GBuffer"
                    auto it = pass.tags.find("LightMode");
                    pass_desc.m_LightMode = (it != pass.tags.end()) ? it->second.c_str() : "GBuffer";

                    // Programs: the ShaderLab parser produces ShaderProgram
                    // entries with source_code / hlsl_code and entry points.
                    // ShaderRes stores FILE paths (not inline code). For
                    // ShaderLab assets we store the source .shader path so the
                    // runtime can locate it for compilation.
                    if (!pass.programs.empty())
                    {
                        pass_desc.m_VertexShaderFile = source_path.generic_string().c_str();
                        pass_desc.m_FragmentShaderFile = source_path.generic_string().c_str();
                    }

                    // Render state from pass.render_states
                    const auto& rs = pass.render_states;
                    pass_desc.m_Zwrite = rs.zwrite;
                    pass_desc.m_Blend = rs.blend_enable ? "On" : "Off";
                    // Cull mode: enum → string
                    switch (rs.cull)
                    {
                        case ZEngine::ShaderLab::CullMode::Off:
                            pass_desc.m_Cull = "Off";
                            break;
                        case ZEngine::ShaderLab::CullMode::Front:
                            pass_desc.m_Cull = "Front";
                            break;
                        default:
                            pass_desc.m_Cull = "Back";
                            break;
                    }
                    // Z-test: enum → string
                    switch (rs.ztest)
                    {
                        case ZEngine::ShaderLab::CompareFunc::Less:
                            pass_desc.m_Ztest = "Less";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::Equal:
                            pass_desc.m_Ztest = "Equal";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::LEqual:
                            pass_desc.m_Ztest = "LEqual";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::Greater:
                            pass_desc.m_Ztest = "Greater";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::NotEqual:
                            pass_desc.m_Ztest = "NotEqual";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::GEqual:
                            pass_desc.m_Ztest = "GEqual";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::Always:
                            pass_desc.m_Ztest = "Always";
                            break;
                        default:
                            pass_desc.m_Ztest = "LEqual";
                            break;
                    }

                    out.m_Passes.push_back(pass_desc);
                }
            }

            // Legacy flat fields (for ShaderRes objects that don't use the
            // multi-pass layout). When there is at least one pass, the
            // runtime resolves shader files per-pass; these flat fields are
            // only used by the legacy single-pass code path.
            if (!out.m_Passes.empty())
            {
                out.m_VertexShaderFile = out.m_Passes[0].m_VertexShaderFile;
                out.m_FragmentShaderFile = out.m_Passes[0].m_FragmentShaderFile;
                out.m_RenderPipeline = out.m_Passes[0].m_RenderPipeline;
            }

            out.m_SourceLanguage = "HLSL";
            out.m_VertexEntry = "main";
            out.m_FragmentEntry = "main";
            out.m_IncludeDirectory = "Assets/Shaders";
            out.m_EnableDx12 = true;
            out.m_EnableVulkan = true;
            out.m_EnableMetal = false;
        }

        bool doImportShaderLab(const std::filesystem::path& source_path,
                               const std::filesystem::path& output_path)
        {
            const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> lab_asset =
                ZEngine::ShaderLab::ParseShaderLabFromFile(source_path.generic_string());
            if (lab_asset == nullptr)
            {
                LOG_WARNING(ZShader, "ShaderImporter: failed to parse {}", source_path.generic_string());
                return false;
            }

            ShaderRes* shader_res = MemoryManager::CreateObject<ShaderRes>();
            if (shader_res == nullptr)
            {
                return false;
            }

            ConvertShaderLabAssetToShaderRes(*lab_asset, source_path, *shader_res);

            AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
            const bool ok = asset_mgr && asset_mgr->WriteObjectToDiskThreadSafe(output_path, *shader_res);
            MemoryManager::DestroyObject(shader_res);
            if (ok)
            {
                ShaderImporter::PrecompileShaderVariants(source_path);
            }
            return ok;
        }
    }  // anonymous namespace

    // -------------------------------------------------------------------------
    // ShaderImporter public interface
    // -------------------------------------------------------------------------

    std::vector<std::string> ShaderImporter::GetSupportedExtensions() const
    {
        return {".shader", ".vert", ".frag"};
    }

    bool ShaderImporter::Import(const std::filesystem::path& source_path,
                                const std::filesystem::path& output_path,
                                const AssetImporterSettings& import_settings,
                                AssetMetadata& out_metadata)
    {
        const std::string ext = [&] {
            std::string e = source_path.extension().string();
            std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return e;
        }();

        if (ext == ".shader")
        {
            return doImportShaderLab(source_path, output_path);
        }

        // .vert / .frag — not yet implemented; the pre-existing stub
        // just returned true. Keep that behaviour for now.
        return true;
    }

    bool ShaderImporter::Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings)
    {
        return true;
    }

    std::unique_ptr<AssetImporterSettings> ShaderImporter::GetDefaultSettings() const
    {
        return std::make_unique<ShaderImporterSettings>();
    }

    int ShaderImporter::ImportProjectShaders()
    {
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr || project_info->project_path.empty())
        {
            return 0;
        }

        const std::filesystem::path shaders_root = project_info->GetShadersRoot();
        const std::filesystem::path generated_root = project_info->GetGeneratedShadersRoot();
        if (shaders_root.empty() || generated_root.empty())
        {
            return 0;
        }

        std::error_code ec;
        if (!std::filesystem::exists(shaders_root, ec) || ec)
        {
            return 0;
        }

        // Ensure output directory exists
        std::filesystem::create_directories(generated_root, ec);
        if (ec)
        {
            LOG_WARNING(ZShader, "ShaderImporter: cannot create output dir {}: {}", generated_root.generic_string(), ec.message());
            return 0;
        }

        int imported = 0;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(shaders_root, ec))
        {
            if (ec)
            {
                break;
            }
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::string ext = [&] {
                std::string e = entry.path().extension().string();
                std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return e;
            }();
            if (ext != ".shader")
            {
                continue;
            }

            // Compute output path: mirror the relative path from Shaders/
            // into Assets/_Generated/Shaders/, changing extension to .shader.zasset
            const std::filesystem::path rel_path = std::filesystem::relative(entry.path(), shaders_root, ec);
            const std::filesystem::path output_path = generated_root / (rel_path.generic_string() + ".zasset");

            // A2: first-time seeding only. Skip if .zasset already exists.
            if (std::filesystem::exists(output_path, ec))
            {
                continue;
            }

            // Parse the .shader source
            const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> lab_asset =
                ZEngine::ShaderLab::ParseShaderLabFromFile(entry.path().generic_string());
            if (lab_asset == nullptr)
            {
                LOG_WARNING(ZShader, "ShaderImporter: failed to parse {}", entry.path().generic_string());
                continue;
            }

            // Convert to ShaderRes
            ShaderRes* shader_res = MemoryManager::CreateObject<ShaderRes>();
            if (shader_res == nullptr)
            {
                continue;
            }

            ConvertShaderLabAssetToShaderRes(*lab_asset, entry.path(), *shader_res);

            // Ensure parent directory exists for nested paths
            const std::filesystem::path output_dir = output_path.parent_path();
            if (!output_dir.empty())
            {
                std::filesystem::create_directories(output_dir, ec);
            }

            // Write .zasset
            AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
            if (asset_mgr == nullptr || !asset_mgr->WriteObjectToDiskThreadSafe(output_path, *shader_res))
            {
                LOG_WARNING(ZShader, "ShaderImporter: failed to write {}", output_path.generic_string());
                MemoryManager::DestroyObject(shader_res);
                continue;
            }

            MemoryManager::DestroyObject(shader_res);
            ++imported;
        }

        if (imported > 0)
        {
            LOG_INFO(ZShader, "ShaderImporter: imported {} shader(s) to {}", imported, generated_root.generic_string());
        }

        return imported;
    }

    void ShaderImporter::PrecompileShaderVariants(const std::filesystem::path& source_shader_path)
    {
#if defined(_WIN32)
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return;
        }

        const std::filesystem::path cache_dir = project_info->GetIntermediateShadersRoot();
        if (cache_dir.empty())
        {
            return;
        }

        ZEngine::ShaderLab::ShaderLabDx12Compiler compiler;
        compiler.SetCacheDirectory(cache_dir);
        if (!compiler.LoadFromFile(source_shader_path.generic_string()))
        {
            LOG_WARNING(ZShader,
                        "ShaderImporter: variant precompile skipped for {} ({})",
                        source_shader_path.generic_string(),
                        compiler.GetParseError());
            return;
        }

        const ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader material_keys =
            CollectMaterialVariantKeysByShader();

        std::vector<ZEngine::ShaderLab::ShaderVariantKey> keys_for_shader;
        if (const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> asset = compiler.GetAsset())
        {
            eastl::string shader_name = asset->shader_name.empty()
                                            ? source_shader_path.stem().generic_string().c_str()
                                            : asset->shader_name.c_str();
            if (const auto keys_it = material_keys.find(NormaliseShaderLookupKey(shader_name));
                keys_it != material_keys.end())
            {
                keys_for_shader = keys_it->second;
            }
        }

        compiler.PrecompileAll({}, keys_for_shader.empty() ? nullptr : &keys_for_shader, true);
#else
        (void)source_shader_path;
#endif
    }

    int ShaderImporter::PrecompileProjectShaderVariants()
    {
#if defined(_WIN32)
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return 0;
        }

        const std::filesystem::path shaders_root = project_info->GetShadersRoot();
        const std::filesystem::path cache_dir = project_info->GetIntermediateShadersRoot();
        const ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader material_keys =
            CollectMaterialVariantKeysByShader();
        return ZEngine::ShaderLab::ShaderLabDx12Compiler::PrecompileProjectShaders(shaders_root,
                                                                                   cache_dir,
                                                                                   64,
                                                                                   &material_keys);
#else
        return 0;
#endif
    }

}  // namespace Runtime
