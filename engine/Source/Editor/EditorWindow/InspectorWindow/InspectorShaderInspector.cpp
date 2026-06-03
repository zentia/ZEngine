#include "InspectorAssetCommon.h"
#include "MaterialInspectorRow.h"  // MaterialInspectorRow type (no fn decls -> safe inside detail ns)

#include "Editor/AssetPipeline/ShaderImporter/ShaderImporter.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/Platform/Interface/EditorUtility.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "core/Log/LogSystem.h"
#include "Runtime/Function/Render/ShaderRegistry.h"
#include "Runtime/Function/ShaderLab/ShaderLabParser.h"
#include "Runtime/Function/ShaderLab/ShaderLabVariant.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/Resource/ResType/Data/Shader.h"
#include "Runtime/Platform/Path/Path.h"

// Native ZSlate Shader inspector widgets (BuildShaderInspectorWidget).
#include "Runtime/Slate/Widgets/SBorder.h"
#include "Runtime/Slate/Widgets/SBoxPanel.h"
#include "Runtime/Slate/Widgets/SButton.h"
#include "Runtime/Slate/Widgets/SCheckBox.h"
#include "Runtime/Slate/Widgets/SEditableTextBox.h"
#include "Runtime/Slate/Widgets/SExpandableArea.h"
#include "Runtime/Slate/Widgets/SScrollBox.h"
#include "Runtime/Slate/Widgets/STextBlock.h"

#ifdef _WIN32
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include <shellapi.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace InspectorShaderDetail
{

void ResetShaderAssetForInspector(ShaderRes& shader, const std::filesystem::path& asset_path);
void CopyShaderAssetForInspector(ShaderRes& destination, const ShaderRes& source);

std::filesystem::path GetProjectAssetsPath()
{
    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info != nullptr)
    {
        const std::filesystem::path project_content = project_info->GetProjectContent();
        if (!project_content.empty())
        {
            return std::filesystem::absolute(project_content).lexically_normal();
        }
    }
    return std::filesystem::absolute(GET_SYSTEM(ConfigManager)->GetAssetFolder()).lexically_normal();
}
std::string SanitizeShaderBaseName(const eastl::string& shader_name)
{
    const std::string source_name = shader_name.empty() ? std::string("NewShader") : std::string(shader_name.c_str());
    std::string sanitized_name;
    sanitized_name.reserve(source_name.size());
    for (char character : source_name)
    {
        if (std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-')
        {
            sanitized_name.push_back(character);
        }
        else if (character == ' ' || character == '.')
        {
            sanitized_name.push_back('_');
        }
    }
    return sanitized_name.empty() ? "NewShader" : sanitized_name;
}
eastl::string MakeProjectRelativeAssetPath(const std::filesystem::path& path)
{
    const std::filesystem::path asset_root = GetProjectAssetsPath();
    const std::filesystem::path normalized_path = std::filesystem::absolute(path).lexically_normal();
    const std::filesystem::path relative_path = Path::GetRelativePath(asset_root, normalized_path);
    const std::string relative_string = relative_path.generic_string();
    if (!relative_string.empty() && !(relative_string.size() >= 2 && relative_string[0] == '.' && relative_string[1] == '.'))
    {
        return relative_string.c_str();
    }
    return normalized_path.generic_string().c_str();
}

std::string MakeInspectorAssetDisplayPath(const eastl::string& stored_path)
{
    if (stored_path.empty())
    {
        return "(not set)";
    }

    const std::filesystem::path resolved_path = ResolveProjectAssetPath(stored_path);
    if (resolved_path.empty())
    {
        return stored_path.c_str();
    }

    return MakeProjectRelativeAssetPath(resolved_path).c_str();
}
std::string MakeShaderDisplayPath(const eastl::string& stored_path)
{
    if (stored_path.empty())
    {
        return "(not set)";
    }
    return MakeProjectRelativeAssetPath(ResolveProjectAssetPath(stored_path)).c_str();
}

std::string BuildDefaultVertexShaderSource(const eastl::string& shader_name)

{
    const std::string display_name = shader_name.empty() ? std::string("NewShader") : std::string(shader_name.c_str());
    return "// " + display_name + " vertex shader\n"
                                  "struct VSInput\n"
                                  "{\n"
                                  "    float3 position : POSITION;\n"
                                  "    float3 normal   : NORMAL;\n"
                                  "    float2 uv       : TEXCOORD0;\n"
                                  "};\n\n"
                                  "struct VSOutput\n"
                                  "{\n"
                                  "    float4 position : SV_POSITION;\n"
                                  "    float3 normal   : TEXCOORD0;\n"
                                  "    float2 uv       : TEXCOORD1;\n"
                                  "};\n\n"
                                  "VSOutput main(VSInput input)\n"
                                  "{\n"
                                  "    VSOutput output;\n"
                                  "    output.position = float4(input.position, 1.0);\n"
                                  "    output.normal   = input.normal;\n"
                                  "    output.uv       = input.uv;\n"
                                  "    return output;\n"
                                  "}\n";
}

std::string BuildDefaultFragmentShaderSource(const eastl::string& shader_name)
{
    const std::string display_name = shader_name.empty() ? std::string("NewShader") : std::string(shader_name.c_str());
    return "// " + display_name + " fragment shader\n"
                                  "struct PSInput\n"
                                  "{\n"
                                  "    float4 position : SV_POSITION;\n"
                                  "    float3 normal   : TEXCOORD0;\n"
                                  "    float2 uv       : TEXCOORD1;\n"
                                  "};\n\n"
                                  "float4 main(PSInput input) : SV_TARGET\n"
                                  "{\n"
                                  "    float3 n = normalize(input.normal * 0.5 + 0.5);\n"
                                  "    return float4(n.xy, input.uv.x, 1.0);\n"
                                  "}\n";
}

std::filesystem::path GetDefaultShaderSourcePath(const eastl::string& shader_name, bool is_vertex_shader)
{
    const std::string extension = is_vertex_shader ? ".vert.hlsl" : ".frag.hlsl";
    return GetProjectAssetsPath() / "Shaders" / (SanitizeShaderBaseName(shader_name) + extension);
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

bool EnsureShaderSourceFile(eastl::string& stored_path, const eastl::string& shader_name, bool is_vertex_shader)
{
    std::filesystem::path absolute_path = ResolveProjectAssetPath(stored_path);
    if (absolute_path.empty())
    {
        absolute_path = GetDefaultShaderSourcePath(shader_name, is_vertex_shader);
        stored_path = MakeProjectRelativeAssetPath(absolute_path);
    }

    return WriteTextFileIfMissing(absolute_path,
                                  is_vertex_shader ? BuildDefaultVertexShaderSource(shader_name)
                                                   : BuildDefaultFragmentShaderSource(shader_name));
}

bool OpenFileWithSystemDefault(const std::filesystem::path& file_path)
{
    if (file_path.empty())
    {
        return false;
    }

#ifdef _WIN32
    HINSTANCE result = ShellExecuteW(nullptr, L"open", file_path.wstring().c_str(), nullptr, nullptr, SW_NORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
#else
    EditorUtility::RevealInFinder(file_path.generic_string().c_str());
    return false;
#endif
}

bool CompileShaderAssetSources(const ShaderRes& shader_asset, std::string& status_message)
{
#ifdef _WIN32
    if (!shader_asset.m_EnableDx12)
    {
        status_message = "DX12 target is disabled for this shader asset.";
        return false;
    }

    if (!shader_asset.m_SourceLanguage.empty() && shader_asset.m_SourceLanguage != "HLSL" && shader_asset.m_SourceLanguage != "hlsl")
    {
        status_message = "DX12 validation currently supports HLSL authoring only.";
        return false;
    }

    if ((!shader_asset.m_VertexEntry.empty() && shader_asset.m_VertexEntry != "main") ||
        (!shader_asset.m_FragmentEntry.empty() && shader_asset.m_FragmentEntry != "main"))
    {
        status_message = "DX12 validation currently assumes the main entry point for both stages.";
        return false;
    }

    const std::filesystem::path vertex_shader_path = ResolveProjectAssetPath(shader_asset.m_VertexShaderFile);
    const std::filesystem::path fragment_shader_path = ResolveProjectAssetPath(shader_asset.m_FragmentShaderFile);
    const std::filesystem::path include_directory = ResolveProjectAssetPath(shader_asset.m_IncludeDirectory);
    if (vertex_shader_path.empty() || fragment_shader_path.empty())
    {
        status_message = "Please set both Vertex and Fragment source paths first.";
        return false;
    }

    std::error_code error_code;
    if (!std::filesystem::exists(vertex_shader_path, error_code) || !std::filesystem::exists(fragment_shader_path, error_code))
    {
        status_message = "Source files do not exist yet. Create the HLSL files first.";
        return false;
    }

    DX12ShaderCompiler compiler;
    std::vector<std::string> include_paths;
    const std::filesystem::path assets_path = GetProjectAssetsPath();
    include_paths.emplace_back(vertex_shader_path.parent_path().string());
    if (!include_directory.empty())
    {
        include_paths.emplace_back(include_directory.string());
    }
    if (!assets_path.empty())
    {
        include_paths.emplace_back(assets_path.string());
    }

    DX12ShaderCompileResult vertex_result = compiler.CompileFromFile(vertex_shader_path.string(), ShaderStage::Vertex, include_paths);
    if (!vertex_result.success)
    {
        status_message = "Vertex compile failed:\n" + vertex_result.error_message;
        return false;
    }

    include_paths.clear();
    include_paths.emplace_back(fragment_shader_path.parent_path().string());
    if (!include_directory.empty())
    {
        include_paths.emplace_back(include_directory.string());
    }
    if (!assets_path.empty())
    {
        include_paths.emplace_back(assets_path.string());
    }

    DX12ShaderCompileResult fragment_result = compiler.CompileFromFile(fragment_shader_path.string(), ShaderStage::Fragment, include_paths);
    if (!fragment_result.success)
    {
        status_message = "Fragment compile failed:\n" + fragment_result.error_message;
        return false;
    }

    status_message = "DX12 compile succeeded:\n- Vertex: " + MakeShaderDisplayPath(shader_asset.m_VertexShaderFile) +
                     "\n- Fragment: " + MakeShaderDisplayPath(shader_asset.m_FragmentShaderFile) +
                     "\n- Include Root: " + MakeShaderDisplayPath(shader_asset.m_IncludeDirectory);
    return true;
#else
    status_message = "Compile validation is currently available on Windows / DX12 only.";
    return false;
#endif
}

const char* ShaderLanguageToString(ZEngine::ShaderLab::ShaderLanguage language)
{
    switch (language)
    {
        case ZEngine::ShaderLab::ShaderLanguage::HLSL:
            return "HLSL";
        case ZEngine::ShaderLab::ShaderLanguage::GLSL:
            return "GLSL";
        case ZEngine::ShaderLab::ShaderLanguage::CG:
            return "CG";
        default:
            return "Unknown";
    }
}

const char* CullModeToString(ZEngine::ShaderLab::CullMode cull_mode)
{
    switch (cull_mode)
    {
        case ZEngine::ShaderLab::CullMode::Off:
            return "Off";
        case ZEngine::ShaderLab::CullMode::Front:
            return "Front";
        case ZEngine::ShaderLab::CullMode::Back:
            return "Back";
        default:
            return "Unknown";
    }
}

const char* CompareFuncToString(ZEngine::ShaderLab::CompareFunc compare_func)
{
    switch (compare_func)
    {
        case ZEngine::ShaderLab::CompareFunc::Less:
            return "Less";
        case ZEngine::ShaderLab::CompareFunc::Greater:
            return "Greater";
        case ZEngine::ShaderLab::CompareFunc::LEqual:
            return "LEqual";
        case ZEngine::ShaderLab::CompareFunc::GEqual:
            return "GEqual";
        case ZEngine::ShaderLab::CompareFunc::Equal:
            return "Equal";
        case ZEngine::ShaderLab::CompareFunc::NotEqual:
            return "NotEqual";
        case ZEngine::ShaderLab::CompareFunc::Always:
            return "Always";
        default:
            return "Unknown";
    }
}

const char* BlendFuncToString(ZEngine::ShaderLab::BlendFunc blend_func)
{
    switch (blend_func)
    {
        case ZEngine::ShaderLab::BlendFunc::Zero:
            return "Zero";
        case ZEngine::ShaderLab::BlendFunc::One:
            return "One";
        case ZEngine::ShaderLab::BlendFunc::Keep:
            return "Keep";
        case ZEngine::ShaderLab::BlendFunc::SrcColor:
            return "SrcColor";
        case ZEngine::ShaderLab::BlendFunc::OneMinusSrcColor:
            return "OneMinusSrcColor";
        case ZEngine::ShaderLab::BlendFunc::DstColor:
            return "DstColor";
        case ZEngine::ShaderLab::BlendFunc::OneMinusDstColor:
            return "OneMinusDstColor";
        case ZEngine::ShaderLab::BlendFunc::SrcAlpha:
            return "SrcAlpha";
        case ZEngine::ShaderLab::BlendFunc::OneMinusSrcAlpha:
            return "OneMinusSrcAlpha";
        case ZEngine::ShaderLab::BlendFunc::DstAlpha:
            return "DstAlpha";
        case ZEngine::ShaderLab::BlendFunc::OneMinusDstAlpha:
            return "OneMinusDstAlpha";
        case ZEngine::ShaderLab::BlendFunc::ConstantColor:
            return "ConstantColor";
        case ZEngine::ShaderLab::BlendFunc::OneMinusConstantColor:
            return "OneMinusConstantColor";
        case ZEngine::ShaderLab::BlendFunc::ConstantAlpha:
            return "ConstantAlpha";
        case ZEngine::ShaderLab::BlendFunc::OneMinusConstantAlpha:
            return "OneMinusConstantAlpha";
        case ZEngine::ShaderLab::BlendFunc::SrcAlphaSaturate:
            return "SrcAlphaSaturate";
        case ZEngine::ShaderLab::BlendFunc::Src1Color:
            return "Src1Color";
        case ZEngine::ShaderLab::BlendFunc::OneMinusSrc1Color:
            return "OneMinusSrc1Color";
        case ZEngine::ShaderLab::BlendFunc::Src1Alpha:
            return "Src1Alpha";
        case ZEngine::ShaderLab::BlendFunc::OneMinusSrc1Alpha:
            return "OneMinusSrc1Alpha";
        default:
            return "Unknown";
    }
}

const char* BlendOpToString(ZEngine::ShaderLab::BlendOp blend_op)
{
    switch (blend_op)
    {
        case ZEngine::ShaderLab::BlendOp::Add:
            return "Add";
        case ZEngine::ShaderLab::BlendOp::Sub:
            return "Sub";
        case ZEngine::ShaderLab::BlendOp::RevSub:
            return "RevSub";
        case ZEngine::ShaderLab::BlendOp::Min:
            return "Min";
        case ZEngine::ShaderLab::BlendOp::Max:
            return "Max";
        default:
            return "Unknown";
    }
}

bool ReadTextFileForInspector(const std::filesystem::path& path, std::string& content)
{
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    return file.good() || file.eof();
}


std::string MakeProjectRelativePathForInspector(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }

    const std::filesystem::path assets_path = GetProjectAssetsPath();
    const std::filesystem::path relative_path = Path::GetRelativePath(assets_path, std::filesystem::absolute(path).lexically_normal());
    const std::string relative_string = relative_path.generic_string();
    if (!relative_string.empty() && !(relative_string.size() >= 2 && relative_string[0] == '.' && relative_string[1] == '.'))
    {
        return relative_string;
    }
    return path.generic_string();
}

bool WriteTextFileAtomicForInspector(const std::filesystem::path& path, const std::string& content, std::string* out_error)
{
    if (path.empty())
    {
        if (out_error != nullptr)
        {
            *out_error = "empty path";
        }
        return false;
    }

    std::error_code ec;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        ec.clear();
    }

    const std::filesystem::path temp_path = path.generic_string() + ".tmp";
    {
        std::ofstream file(temp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!file.is_open())
        {
            if (out_error != nullptr)
            {
                *out_error = "cannot open temp file for writing";
            }
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file.good())
        {
            if (out_error != nullptr)
            {
                *out_error = "failed to write temp file";
            }
            std::filesystem::remove(temp_path, ec);
            return false;
        }
    }

    ec.clear();
    std::filesystem::rename(temp_path, path, ec);
    if (ec)
    {
        std::filesystem::copy_file(temp_path, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(temp_path, ec);
        if (ec)
        {
            if (out_error != nullptr)
            {
                *out_error = "rename/copy failed";
            }
            return false;
        }
    }

    return true;
}

std::filesystem::path ResolveShaderLabImporterOutputPath(const std::filesystem::path& shader_abs_path)
{
    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || shader_abs_path.empty())
    {
        return {};
    }

    std::error_code ec;
    const std::filesystem::path abs_shader = std::filesystem::absolute(shader_abs_path, ec);

    if (ShaderRegistry* registry = GET_SYSTEM(ShaderRegistry))
    {
        const std::filesystem::path project_root = project_info->GetProjectRoot();
        if (!project_root.empty())
        {
            const eastl::string rel_key =
                Path::GetRelativePath(project_root, abs_shader).generic_string().c_str();
            if (const ShaderRegistryEntry* entry = registry->FindByPath(rel_key))
            {
                if (!entry->m_ZassetRelPath.empty())
                {
                    const std::filesystem::path zasset =
                        project_info->GetProjectContent() / entry->m_ZassetRelPath.c_str();
                    if (!zasset.empty())
                    {
                        return zasset;
                    }
                }
            }
        }
    }

    const std::filesystem::path shaders_root = project_info->GetShadersRoot();
    const std::filesystem::path generated_root = project_info->GetGeneratedShadersRoot();
    if (shaders_root.empty() || generated_root.empty())
    {
        return {};
    }

    const std::filesystem::path abs_shaders_root = std::filesystem::absolute(shaders_root, ec);
    ec.clear();
    std::filesystem::path rel_path = std::filesystem::relative(abs_shader, abs_shaders_root, ec);
    if (ec || rel_path.empty())
    {
        rel_path = abs_shader.filename();
    }
    else
    {
        for (const std::filesystem::path& part : rel_path)
        {
            if (part == "..")
            {
                rel_path = abs_shader.filename();
                break;
            }
        }
    }

    std::filesystem::create_directories(generated_root, ec);
    return generated_root / (rel_path.generic_string() + ".zasset");
}

bool ReimportShaderLabAfterSourceSave(const std::filesystem::path& shader_abs_path)
{
    const std::filesystem::path output_zasset = ResolveShaderLabImporterOutputPath(shader_abs_path);
    if (output_zasset.empty())
    {
        LOG_WARNING(ZShader, "ShaderLab Apply: no importer output path for {}", shader_abs_path.generic_string());
        return false;
    }

    if (const std::filesystem::path parent = output_zasset.parent_path(); !parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    Runtime::ShaderImporter importer;
    AssetMetadata metadata;
    AssetImporterSettings default_settings;
    if (!importer.Import(shader_abs_path, output_zasset, default_settings, metadata))
    {
        LOG_WARNING(ZShader,
                    "ShaderLab Apply: import failed for {} -> {}",
                    shader_abs_path.generic_string(),
                    output_zasset.generic_string());
        return false;
    }

    if (auto shader_registry = GET_SYSTEM(ShaderRegistry))
    {
        shader_registry->OnShaderFileEvent(shader_abs_path);
    }

    if (EditorAssetManager* editor_assets =
            dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
    {
        editor_assets->RefreshAsset(output_zasset.generic_string());
    }

    Runtime::ShaderImporter::PrecompileShaderVariants(shader_abs_path);
    return true;
}

const ZEngine::ShaderLab::ShaderSubShader* GetPrimaryShaderLabSubShader(const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset>& asset)
{
    if (asset == nullptr || asset->subshaders.empty())
    {
        return nullptr;
    }
    return &asset->subshaders.front();
}

std::string GetShaderLabTagValue(const std::map<std::string, std::string>& tags, const char* key)
{
    const auto it = tags.find(key);
    return it == tags.end() ? std::string() : it->second;
}

int GetUnityRenderQueueFromShaderLabTag(const std::string& queue_tag)
{
    if (queue_tag.empty())
    {
        return 2000;
    }

    std::string base = queue_tag;
    int offset = 0;
    const size_t plus_pos = queue_tag.find('+');
    const size_t minus_pos = queue_tag.find('-', 1);
    const size_t offset_pos = plus_pos != std::string::npos ? plus_pos : minus_pos;
    if (offset_pos != std::string::npos)
    {
        base = queue_tag.substr(0, offset_pos);
        try
        {
            offset = std::stoi(queue_tag.substr(offset_pos));
        }
        catch (...)
        {
            offset = 0;
        }
    }

    int base_value = 2000;
    if (base == "Background")
    {
        base_value = 1000;
    }
    else if (base == "Geometry")
    {
        base_value = 2000;
    }
    else if (base == "AlphaTest")
    {
        base_value = 2450;
    }
    else if (base == "Transparent")
    {
        base_value = 3000;
    }
    else if (base == "Overlay")
    {
        base_value = 4000;
    }
    else
    {
        try
        {
            return std::stoi(queue_tag);
        }
        catch (...)
        {
            return 2000;
        }
    }

    return base_value + offset;
}

size_t CountShaderLabPasses(const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset>& asset)
{
    if (asset == nullptr)
    {
        return 0;
    }

    size_t pass_count = 0;
    for (const ZEngine::ShaderLab::ShaderSubShader& subshader : asset->subshaders)
    {
        pass_count += subshader.passes.size();
    }
    return pass_count;
}

void CollectShaderLabKeywords(const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset>& asset, std::vector<std::string>& keywords)
{
    keywords.clear();
    if (asset == nullptr)
    {
        return;
    }

    for (const ZEngine::ShaderLab::ShaderSubShader& subshader : asset->subshaders)
    {
        for (const ZEngine::ShaderLab::ShaderPass& pass : subshader.passes)
        {
            for (const ZEngine::ShaderLab::ShaderProgram& program : pass.programs)
            {
                keywords.insert(keywords.end(), program.pragma_keywords.begin(), program.pragma_keywords.end());
                keywords.insert(keywords.end(), program.shader_feature_keywords.begin(), program.shader_feature_keywords.end());
            }
        }
    }

    std::sort(keywords.begin(), keywords.end());
    keywords.erase(std::unique(keywords.begin(), keywords.end()), keywords.end());
}

// Phase B: collect all .zasset paths whose runtime asset_type matches
    // `expected_type` (e.g. "ShaderRes") under `assets_path`. Delegates to
    // AssetManager::GetAssetsByType(): the editor override consults the
    // AssetRegistry's by-type reverse index so we avoid re-reading every
    // .zasset header on every dropdown open. The base implementation
    // performs a directory walk (also used as the fallback when the
    // registry is still warming up after project open).
    std::vector<std::filesystem::path>
    CollectZAssetsOfType(const std::filesystem::path& assets_path, const std::string& expected_type)
    {
        AssetManager* asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return {};
        }
        return asset_manager->GetAssetsByType(expected_type, assets_path);
    }

    void LogLegacyAssetsShaderWarningOnce(const std::string& shader_path)
    {
        static std::unordered_set<std::string> warned_paths;
        if (warned_paths.insert(shader_path).second)
        {
            LOG_WARNING(ZShader,
                        ".shader file under Assets/ is deprecated; move {} to <project>/Shaders/",
                        shader_path);
        }
    }

    bool ShaderNameMatchesFileCandidate(const eastl::string& shader_name,
                                        const eastl::string& candidate_name,
                                        const std::filesystem::path& file_path)
    {
        if (candidate_name == shader_name)
        {
            return true;
        }
        // Unity-style names (Custom/Foo) must not match filename stems like "Foo.shader".
        if (shader_name.find('/') != eastl::string::npos)
        {
            return false;
        }
        return file_path.stem().generic_string() == shader_name.c_str();
    }

    eastl::string GetMaterialAuthoringShaderName(const Material& material)
    {
        if (!material.m_Shader.empty())
        {
            return material.m_Shader;
        }
        return material.GetShaderName();
    }

    bool IsShaderSourceAvailableInProject(const eastl::string& shader_name)
    {
        if (shader_name.empty() || shader_name == "StandardLit")
        {
            return true;
        }

        ShaderRegistry* registry = GET_SYSTEM(ShaderRegistry);
        if (registry == nullptr)
        {
            return false;
        }

        const ShaderRegistryEntry* entry = registry->FindByName(shader_name);
        if (entry == nullptr || entry->m_SourceRelPath.empty())
        {
            return false;
        }

        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr || project_info->GetProjectRoot().empty())
        {
            return false;
        }

        std::error_code source_ec;
        const std::filesystem::path source_abs =
            project_info->GetProjectRoot() / std::filesystem::path(entry->m_SourceRelPath.c_str());
        return std::filesystem::exists(source_abs, source_ec) && !source_ec;
    }

    void SanitizeMaterialShaderBindingForInspector(Material& material)
    {
        const eastl::string authoring_name = GetMaterialAuthoringShaderName(material);

        if (!material.m_ShaderPptr.IsNull())
        {
            if (ShaderRes* shader = material.m_ShaderPptr)
            {
                if (shader != nullptr && !shader->m_ShaderName.empty() && !authoring_name.empty() &&
                    shader->m_ShaderName != authoring_name)
                {
                    material.m_ShaderPptr = PPtr<ShaderRes>();
                    material.m_ShaderGuid.clear();
                }
            }
        }

        if (IsShaderSourceAvailableInProject(authoring_name))
        {
            return;
        }

        material.m_ShaderPptr = PPtr<ShaderRes>();
        material.m_ShaderGuid.clear();
    }

    bool PopulateInspectorShaderFromSourceFile(const std::filesystem::path& source_path,
                                               ShaderRes& out_shader,
                                               std::filesystem::path* resolved_shader_path)
    {
        std::string source;
        if (!ReadTextFileForInspector(source_path, source))
        {
            return false;
        }

        ZEngine::ShaderLab::ShaderLabParser parser(source);
        if (!parser.Parse())
        {
            return false;
        }

        const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> shader_lab_asset = parser.GetAsset();
        if (shader_lab_asset == nullptr)
        {
            return false;
        }

        if (resolved_shader_path != nullptr)
        {
            *resolved_shader_path = source_path;
        }
        ResetShaderAssetForInspector(out_shader, source_path);
        out_shader.m_ShaderName = shader_lab_asset->shader_name.empty() ? source_path.stem().generic_string().c_str()
                                                                        : shader_lab_asset->shader_name.c_str();
        for (const ZEngine::ShaderLab::ShaderProperty& property : shader_lab_asset->properties)
        {
            ShaderPropertyDesc property_desc;
            property_desc.m_Name = property.name.c_str();
            property_desc.m_DisplayName = property.display_name.c_str();
            property_desc.m_Type = ZEngine::ShaderLab::PropertyTypeToString(property.type);
            property_desc.m_DefaultFloat = property.default_value.float_value;
            property_desc.m_RangeMin = property.default_value.range_min;
            property_desc.m_RangeMax = property.default_value.range_max;
            property_desc.m_DefaultColor = Vector3(property.default_value.color[0],
                                                   property.default_value.color[1],
                                                   property.default_value.color[2]);
            property_desc.m_DefaultAlpha = property.default_value.color[3];
            property_desc.m_DefaultTexture = property.default_value.texture_path.c_str();
            property_desc.m_DefaultToggle = property.default_value.float_value > 0.5f || property.default_value.int_value != 0;
            out_shader.m_Properties.push_back(property_desc);
        }
        out_shader.InitializeRuntimeTypeInfo();
        return true;
    }
bool LoadProjectShaderDefinitionForInspector(ShaderRes& out_shader,
                                             const eastl::string& shader_name,
                                             std::filesystem::path* resolved_shader_path = nullptr)
{
    if (resolved_shader_path != nullptr)
    {
        resolved_shader_path->clear();
    }

    if (shader_name.empty() || shader_name == "StandardLit")
    {
        return false;
    }

    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return false;
    }

    const std::filesystem::path assets_path = project_info->GetProjectContent();
    if (!std::filesystem::exists(assets_path))
    {
        return false;
    }

    if (ShaderRegistry* registry = GET_SYSTEM(ShaderRegistry))
    {
        if (const ShaderRegistryEntry* entry = registry->FindByName(shader_name))
        {
            if (!entry->m_SourceRelPath.empty() && !project_info->GetProjectRoot().empty())
            {
                const std::filesystem::path registry_source =
                    project_info->GetProjectRoot() / std::filesystem::path(entry->m_SourceRelPath.c_str());
                if (PopulateInspectorShaderFromSourceFile(registry_source, out_shader, resolved_shader_path))
                {
                    return true;
                }
            }
        }
    }

    // Inspector hot path: ShaderRegistry is the SSOT for Custom/ shader names.
    // Avoid recursive scans of Shaders/ or Assets/ and bulk ShaderRes ReadObject
    // loops here -- those froze the editor when selecting materials (Preview
    // window was already doing a per-frame full-tree parse via FindShaderByName).
    return false;
}

std::vector<eastl::string> FindProjectShaders()
{
    std::vector<eastl::string> shaders;
    shaders.emplace_back("StandardLit");

    std::unordered_set<std::string> seen_names;
    seen_names.insert("StandardLit");

    auto add_unique = [&](const eastl::string& shader_name) {
        if (shader_name.empty())
        {
            return;
        }
        const std::string key = shader_name.c_str();
        if (seen_names.insert(key).second)
        {
            shaders.push_back(shader_name);
        }
    };

    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return shaders;
    }

    const std::filesystem::path project_root = project_info->GetProjectRoot();

    // Canonical list: ShaderRegistry entries under <Project>/Shaders/ only.
    // Do not merge ShaderRes .zasset scans here -- stale _Generated products
    // and legacy Assets/ copies duplicate names and break ImGui IDs.
    if (ShaderRegistry* shader_registry = GET_SYSTEM(ShaderRegistry))
    {
        for (ShaderRegistryEntry* entry : shader_registry->GetAll())
        {
            if (entry == nullptr || entry->m_ShaderName.empty())
            {
                continue;
            }

            if (!project_root.empty() && !entry->m_SourceRelPath.empty())
            {
                std::error_code source_ec;
                const std::filesystem::path source_abs =
                    project_root / std::filesystem::path(entry->m_SourceRelPath.c_str());
                if (!std::filesystem::exists(source_abs, source_ec) || source_ec)
                {
                    continue;
                }
            }

            add_unique(entry->m_ShaderName);
        }
    }

    // Fallback when registry is empty (first open / tests): scan Shaders/ only.
    if (shaders.size() <= 1)
    {
        const std::filesystem::path shaders_root = project_info->GetShadersRoot();
        if (!shaders_root.empty())
        {
            std::error_code root_ec;
            if (std::filesystem::exists(shaders_root, root_ec) && !root_ec)
            {
                std::error_code shader_pass_ec;
                for (const auto& file_entry :
                     std::filesystem::recursive_directory_iterator(shaders_root, shader_pass_ec))
                {
                    if (shader_pass_ec)
                    {
                        break;
                    }
                    if (!file_entry.is_regular_file() || file_entry.path().extension() != ".shader")
                    {
                        continue;
                    }

                    std::string source;
                    if (!ReadTextFileForInspector(file_entry.path(), source))
                    {
                        continue;
                    }

                    ZEngine::ShaderLab::ShaderLabParser parser(source);
                    if (!parser.Parse())
                    {
                        continue;
                    }

                    const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> shader_lab_asset = parser.GetAsset();
                    if (shader_lab_asset != nullptr)
                    {
                        add_unique(shader_lab_asset->shader_name.empty()
                                       ? file_entry.path().stem().generic_string().c_str()
                                       : shader_lab_asset->shader_name.c_str());
                    }
                }
            }
        }
    }

    return shaders;
}
std::string NormalizeShaderPropertyKey(const eastl::string& property_name)
{
    std::string normalized;
    normalized.reserve(property_name.size());
    for (char c : property_name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return normalized;
}

std::string NormalizeShaderPropertyType(const eastl::string& property_type)
{
    return NormalizeShaderPropertyKey(property_type);
}

bool MatchesPropertyKey(const std::string& key, std::initializer_list<const char*> aliases)
{
    for (const char* alias : aliases)
    {
        if (key == alias)
        {
            return true;
        }
    }
    return false;
}

void FillShaderPassFromLegacyFields(ShaderPassDesc& pass, const ShaderRes& shader)
{
    pass.m_Name = pass.m_Name.empty() ? "GBuffer" : pass.m_Name;
    pass.m_LightMode = pass.m_LightMode.empty() ? "GBuffer" : pass.m_LightMode;
    pass.m_VertexShaderFile = shader.m_VertexShaderFile;
    pass.m_FragmentShaderFile = shader.m_FragmentShaderFile;
    pass.m_RenderPipeline = shader.m_RenderPipeline;
    pass.m_VertexEntry = shader.m_VertexEntry;
    pass.m_FragmentEntry = shader.m_FragmentEntry;
    pass.m_Cull = pass.m_Cull.empty() ? "Back" : pass.m_Cull;
    pass.m_Ztest = pass.m_Ztest.empty() ? "LEqual" : pass.m_Ztest;
    pass.m_Blend = pass.m_Blend.empty() ? "Off" : pass.m_Blend;
}

void EnsureShaderPassCompatibility(ShaderRes& shader)
{
    if (shader.m_Passes.empty())
    {
        ShaderPassDesc default_pass;
        FillShaderPassFromLegacyFields(default_pass, shader);
        shader.m_Passes.push_back(default_pass);
        return;
    }

    ShaderPassDesc& first_pass = shader.m_Passes.front();
    if (first_pass.m_VertexShaderFile.empty())
    {
        first_pass.m_VertexShaderFile = shader.m_VertexShaderFile;
    }
    if (first_pass.m_FragmentShaderFile.empty())
    {
        first_pass.m_FragmentShaderFile = shader.m_FragmentShaderFile;
    }
    if (first_pass.m_RenderPipeline.empty())
    {
        first_pass.m_RenderPipeline = shader.m_RenderPipeline;
    }
    if (first_pass.m_VertexEntry.empty())
    {
        first_pass.m_VertexEntry = shader.m_VertexEntry;
    }
    if (first_pass.m_FragmentEntry.empty())
    {
        first_pass.m_FragmentEntry = shader.m_FragmentEntry;
    }
    if (first_pass.m_Name.empty())
    {
        first_pass.m_Name = "GBuffer";
    }
    if (first_pass.m_LightMode.empty())
    {
        first_pass.m_LightMode = "GBuffer";
    }
    if (first_pass.m_Cull.empty())
    {
        first_pass.m_Cull = "Back";
    }
    if (first_pass.m_Ztest.empty())
    {
        first_pass.m_Ztest = "LEqual";
    }
    if (first_pass.m_Blend.empty())
    {
        first_pass.m_Blend = "Off";
    }
}

void SyncLegacyShaderFieldsFromPrimaryPass(ShaderRes& shader)
{
    EnsureShaderPassCompatibility(shader);
    const ShaderPassDesc& first_pass = shader.m_Passes.front();
    shader.m_VertexShaderFile = first_pass.m_VertexShaderFile;
    shader.m_FragmentShaderFile = first_pass.m_FragmentShaderFile;
    shader.m_RenderPipeline = first_pass.m_RenderPipeline;
    shader.m_VertexEntry = first_pass.m_VertexEntry;
    shader.m_FragmentEntry = first_pass.m_FragmentEntry;
}

eastl::string GetShaderPropertyDisplayName(const ShaderPropertyDesc& property)
{
    if (!property.m_DisplayName.empty())
    {
        return property.m_DisplayName;
    }
    if (!property.m_Name.empty())
    {
        return property.m_Name;
    }
    return "Property";
}

void ResetShaderAssetForInspector(ShaderRes& shader, const std::filesystem::path& asset_path)
{
    shader.m_ShaderName = asset_path.stem().generic_string().c_str();
    shader.m_Properties.clear();
    shader.m_Passes.clear();
    shader.m_VertexShaderFile.clear();
    shader.m_FragmentShaderFile.clear();
    shader.m_RenderPipeline = "StandardLit";
    shader.m_SourceLanguage = "HLSL";
    shader.m_VertexEntry = "main";
    shader.m_FragmentEntry = "main";
    shader.m_IncludeDirectory = "Assets/Shaders";
    shader.m_EnableDx12 = true;
    shader.m_EnableVulkan = true;
    shader.m_EnableMetal = false;
}

void CopyShaderAssetForInspector(ShaderRes& destination, const ShaderRes& source)
{
    destination.m_ShaderName = source.m_ShaderName;
    destination.m_Properties = source.m_Properties;
    destination.m_Passes = source.m_Passes;
    destination.m_VertexShaderFile = source.m_VertexShaderFile;
    destination.m_FragmentShaderFile = source.m_FragmentShaderFile;
    destination.m_RenderPipeline = source.m_RenderPipeline;
    destination.m_SourceLanguage = source.m_SourceLanguage;
    destination.m_VertexEntry = source.m_VertexEntry;
    destination.m_FragmentEntry = source.m_FragmentEntry;
    destination.m_IncludeDirectory = source.m_IncludeDirectory;
    destination.m_EnableDx12 = source.m_EnableDx12;
    destination.m_EnableVulkan = source.m_EnableVulkan;
    destination.m_EnableMetal = source.m_EnableMetal;
}

MaterialFloatProperty* FindMaterialFloatProperty(Material& material, const eastl::string& property_name)
{
    for (MaterialFloatProperty& property : material.m_FloatProperties)
    {
        if (property.m_Name == property_name)
        {
            return &property;
        }
    }
    return nullptr;
}

MaterialColorProperty* FindMaterialColorProperty(Material& material, const eastl::string& property_name)
{
    for (MaterialColorProperty& property : material.m_ColorProperties)
    {
        if (property.m_Name == property_name)
        {
            return &property;
        }
    }
    return nullptr;
}

MaterialTextureProperty* FindMaterialTextureProperty(Material& material, const eastl::string& property_name)
{
    for (MaterialTextureProperty& property : material.m_TextureProperties)
    {
        if (property.m_Name == property_name)
        {
            return &property;
        }
    }
    return nullptr;
}

MaterialToggleProperty* FindMaterialToggleProperty(Material& material, const eastl::string& property_name)
{
    for (MaterialToggleProperty& property : material.m_ToggleProperties)
    {
        if (property.m_Name == property_name)
        {
            return &property;
        }
    }
    return nullptr;
}

MaterialFloatProperty& GetOrCreateMaterialFloatProperty(Material& material, const ShaderPropertyDesc& property_desc, bool& created)
{
    if (MaterialFloatProperty* property = FindMaterialFloatProperty(material, property_desc.m_Name))
    {
        return *property;
    }

    created = true;
    MaterialFloatProperty property;
    property.m_Name = property_desc.m_Name;
    property.m_Value = property_desc.m_DefaultFloat;
    material.m_FloatProperties.push_back(property);
    return material.m_FloatProperties.back();
}

MaterialColorProperty& GetOrCreateMaterialColorProperty(Material& material, const ShaderPropertyDesc& property_desc, bool& created)
{
    if (MaterialColorProperty* property = FindMaterialColorProperty(material, property_desc.m_Name))
    {
        return *property;
    }

    created = true;
    MaterialColorProperty property;
    property.m_Name = property_desc.m_Name;
    property.m_Color = property_desc.m_DefaultColor;
    property.m_Alpha = property_desc.m_DefaultAlpha;
    material.m_ColorProperties.push_back(property);
    return material.m_ColorProperties.back();
}

MaterialTextureProperty& GetOrCreateMaterialTextureProperty(Material& material, const ShaderPropertyDesc& property_desc, bool& created)
{
    if (MaterialTextureProperty* property = FindMaterialTextureProperty(material, property_desc.m_Name))
    {
        return *property;
    }

    created = true;
    MaterialTextureProperty property;
    property.m_Name = property_desc.m_Name;
    Material::AssignTextureFromAssetPath(property.m_Texture, property_desc.m_DefaultTexture);
    material.m_TextureProperties.push_back(property);
    return material.m_TextureProperties.back();
}

MaterialToggleProperty& GetOrCreateMaterialToggleProperty(Material& material, const ShaderPropertyDesc& property_desc, bool& created)
{
    if (MaterialToggleProperty* property = FindMaterialToggleProperty(material, property_desc.m_Name))
    {
        return *property;
    }

    created = true;
    MaterialToggleProperty property;
    property.m_Name = property_desc.m_Name;
    property.m_Value = property_desc.m_DefaultToggle;
    material.m_ToggleProperties.push_back(property);
    return material.m_ToggleProperties.back();
}

std::filesystem::path ResolveShaderLabSourcePathForVariants(const eastl::string& shader_name,
                                                            const std::filesystem::path& resolved_shader_path)
{
    if (!resolved_shader_path.empty() && resolved_shader_path.extension() == ".shader")
    {
        return resolved_shader_path;
    }

    if (shader_name.empty() || shader_name == "StandardLit")
    {
        return {};
    }

    ShaderRegistry* registry = GET_SYSTEM(ShaderRegistry);
    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (registry == nullptr || project_info == nullptr)
    {
        return {};
    }

    const ShaderRegistryEntry* entry = registry->FindByName(shader_name);
    if (entry == nullptr || entry->m_SourceRelPath.empty())
    {
        return {};
    }

    const std::filesystem::path abs_path = project_info->GetProjectRoot() / entry->m_SourceRelPath.c_str();
    if (!std::filesystem::exists(abs_path))
    {
        return {};
    }
    return abs_path;
}

// --- UI-agnostic material property enumeration (native ZSlate inspector) -----

std::vector<MaterialInspectorRow> EnumerateBuiltInMaterialRows(Material& material)
{
    std::vector<MaterialInspectorRow> rows;

    auto add_color = [&](const char* label, Vector3* c, float* a) {
        MaterialInspectorRow r;
        r.kind = MaterialInspectorRowKind::Color;
        r.label = label;
        r.color = c;
        r.alpha = a;
        rows.push_back(r);
    };
    auto add_float = [&](const char* label, float* v, float mn, float mx) {
        MaterialInspectorRow r;
        r.kind = MaterialInspectorRowKind::Float;
        r.label = label;
        r.value = v;
        r.range_min = mn;
        r.range_max = mx;
        rows.push_back(r);
    };
    auto add_bool = [&](const char* label, bool* b) {
        MaterialInspectorRow r;
        r.kind = MaterialInspectorRowKind::Bool;
        r.label = label;
        r.boolean = b;
        rows.push_back(r);
    };
    auto add_str = [&](const char* label, eastl::string* s) {
        MaterialInspectorRow r;
        r.kind = MaterialInspectorRowKind::String;
        r.label = label;
        r.str = s;
        rows.push_back(r);
    };

    auto add_tex = [&](const char* label, PPtr<Texture2D>* t) {
        MaterialInspectorRow r;
        r.kind = MaterialInspectorRowKind::Texture;
        r.label = label;
        r.texture = t;
        rows.push_back(r);
    };

    add_color("Base Color", &material.m_BaseColorFactor, &material.m_AlphaFactor);
    add_float("Metallic", &material.m_MetallicFactor, 0.0f, 1.0f);
    add_float("Roughness", &material.m_RoughnessFactor, 0.0f, 1.0f);
    add_float("Normal Scale", &material.m_NormalScale, 0.0f, 4.0f);
    add_float("Occlusion", &material.m_OcclusionStrength, 0.0f, 1.0f);
    add_color("Emission", &material.m_EmissiveFactor, nullptr);
    add_bool("Transparent", &material.m_IsBlend);
    add_bool("Double Sided", &material.m_IsDoubleSided);
    add_tex("Base Map", &material.m_BaseColourTexturePptr);
    add_tex("Metallic/Roughness", &material.m_MetallicRoughnessTexturePptr);
    add_tex("Normal Map", &material.m_NormalTexturePptr);
    add_tex("Occlusion Map", &material.m_OcclusionTexturePptr);
    add_tex("Emission Map", &material.m_EmissiveTexturePptr);
    return rows;
}

std::vector<MaterialInspectorRow> EnumerateShaderMaterialRows(Material& material,
                                                              const ShaderRes& shader,
                                                              bool& out_created)
{
    out_created = false;
    std::vector<MaterialInspectorRow> rows;
    const size_t n = shader.m_Properties.size();
    // Pre-reserve so GetOrCreate*'s push_back never reallocates during the loop;
    // otherwise field pointers handed out for earlier rows would dangle.
    material.m_ColorProperties.reserve(material.m_ColorProperties.size() + n);
    material.m_TextureProperties.reserve(material.m_TextureProperties.size() + n);
    material.m_ToggleProperties.reserve(material.m_ToggleProperties.size() + n);
    material.m_FloatProperties.reserve(material.m_FloatProperties.size() + n);

    for (const ShaderPropertyDesc& pd : shader.m_Properties)
    {
        if (pd.m_Name.empty())
            continue;

        MaterialInspectorRow row;
        row.label = GetShaderPropertyDisplayName(pd).c_str();
        const std::string key = NormalizeShaderPropertyKey(pd.m_Name);
        const std::string type = NormalizeShaderPropertyType(pd.m_Type);

        auto as_color = [&](Vector3* c, float* a) {
            row.kind = MaterialInspectorRowKind::Color;
            row.color = c;
            row.alpha = a;
        };
        auto as_float = [&](float* v, float dmn, float dmx) {
            row.kind = MaterialInspectorRowKind::Float;
            row.value = v;
            row.range_min = (type == "range") ? pd.m_RangeMin : dmn;
            row.range_max = (type == "range") ? pd.m_RangeMax : dmx;
        };
        auto as_bool = [&](bool* b) {
            row.kind = MaterialInspectorRowKind::Bool;
            row.boolean = b;
        };
        auto as_tex = [&](PPtr<Texture2D>* t) {
            row.kind = MaterialInspectorRowKind::Texture;
            row.texture = t;
        };

        if (MatchesPropertyKey(key, {"basecolor", "color", "albedo"}) && type == "color")
            as_color(&material.m_BaseColorFactor, &material.m_AlphaFactor);
        else if (MatchesPropertyKey(key, {"metallic", "metalness"}))
            as_float(&material.m_MetallicFactor, 0.0f, 1.0f);
        else if (MatchesPropertyKey(key, {"roughness", "smoothness"}))
            as_float(&material.m_RoughnessFactor, 0.0f, 1.0f);
        else if (MatchesPropertyKey(key, {"normalscale"}))
            as_float(&material.m_NormalScale, 0.0f, 4.0f);
        else if (MatchesPropertyKey(key, {"occlusion", "occlusionstrength"}))
            as_float(&material.m_OcclusionStrength, 0.0f, 1.0f);
        else if (MatchesPropertyKey(key, {"emissioncolor", "emissivecolor"}) && type == "color")
            as_color(&material.m_EmissiveFactor, nullptr);
        else if (MatchesPropertyKey(key, {"transparent", "isblend", "surfaceblend"}))
            as_bool(&material.m_IsBlend);
        else if (MatchesPropertyKey(key, {"doublesided", "isdoublesided"}))
            as_bool(&material.m_IsDoubleSided);
        else if (MatchesPropertyKey(key, {"basemap", "maintex", "albedomap"}))
            as_tex(&material.m_BaseColourTexturePptr);
        else if (MatchesPropertyKey(key, {"metallicroughnessmap", "metallicmap"}))
            as_tex(&material.m_MetallicRoughnessTexturePptr);
        else if (MatchesPropertyKey(key, {"normalmap"}))
            as_tex(&material.m_NormalTexturePptr);
        else if (MatchesPropertyKey(key, {"occlusionmap", "aomap"}))
            as_tex(&material.m_OcclusionTexturePptr);
        else if (MatchesPropertyKey(key, {"emissionmap", "emissivemap"}))
            as_tex(&material.m_EmissiveTexturePptr);
        else if (type == "color")
        {
            bool created = false;
            MaterialColorProperty& p = GetOrCreateMaterialColorProperty(material, pd, created);
            out_created = out_created || created;
            as_color(&p.m_Color, &p.m_Alpha);
        }
        else if (type == "texture2d" || type == "texture")
        {
            bool created = false;
            MaterialTextureProperty& p = GetOrCreateMaterialTextureProperty(material, pd, created);
            out_created = out_created || created;
            as_tex(&p.m_Texture);
        }
        else if (type == "toggle" || type == "bool")
        {
            bool created = false;
            MaterialToggleProperty& p = GetOrCreateMaterialToggleProperty(material, pd, created);
            out_created = out_created || created;
            as_bool(&p.m_Value);
        }
        else
        {
            bool created = false;
            MaterialFloatProperty& p = GetOrCreateMaterialFloatProperty(material, pd, created);
            out_created = out_created || created;
            if (type == "range")
            {
                p.m_Value = std::clamp(p.m_Value, pd.m_RangeMin, pd.m_RangeMax);
                as_float(&p.m_Value, pd.m_RangeMin, pd.m_RangeMax);
            }
            else
            {
                row.kind = MaterialInspectorRowKind::Float;
                row.value = &p.m_Value;
                row.range_min = -1000.0f;
                row.range_max = 1000.0f;
            }
        }

        rows.push_back(row);
    }
    return rows;
}

std::vector<std::string> CollectMaterialShaderVariantKeywords(const eastl::string& shader_name,
                                                              const std::filesystem::path& shader_source_path)
{
    std::vector<std::string> options;
    if (shader_name.empty() || shader_name == "StandardLit")
        return options;

    const std::filesystem::path source_path = ResolveShaderLabSourcePathForVariants(shader_name, shader_source_path);
    if (source_path.empty())
        return options;

    std::string source_text;
    if (ReadTextFileForInspector(source_path, source_text))
        ZEngine::ShaderLab::CollectShaderKeywordOptions(source_text, options);
    return options;
}

// ============================================================================
// Native ZSlate Shader inspector (replaces the ImGui DrawShaderAssetInspector).
//
// Inline source code editing is intentionally NOT provided -- ZSlate has no
// multi-line code editor widget, and the project's authoring workflow already
// routes source editing to the external editor (P6 double-click -> VS Code).
// We render a read-only, per-line source view plus Open/Reveal buttons. All the
// metadata / settings / property / pass info is rendered natively; the ShaderRes
// `.zasset` path stays editable for its scalar fields (name / language / include
// / enable flags) with an explicit Save button.
// ============================================================================
std::shared_ptr<ZSlate::SWidget> BuildShaderInspectorWidgetImpl(const std::filesystem::path& asset_path,
                                                                float scale,
                                                                const std::function<void()>& request_rebuild)
{
    using namespace ZSlate;

    const UIColor kHeader(0.92f, 0.93f, 0.97f, 1.0f);
    const UIColor kLabel(0.74f, 0.78f, 0.84f, 1.0f);
    const UIColor kValue(0.85f, 0.87f, 0.92f, 1.0f);
    const UIColor kDim(0.50f, 0.52f, 0.58f, 1.0f);
    const UIColor kErr(1.0f, 0.45f, 0.35f, 1.0f);
    const UIColor kOk(0.40f, 0.85f, 0.50f, 1.0f);

    auto text = [&](const std::string& s, float fs, const UIColor& c) {
        auto t = std::make_shared<STextBlock>();
        t->Text = s;
        t->FontSize = fs * scale;
        t->Color = c;
        t->Alignment = TextAnchor::MiddleLeft;
        return t;
    };

    auto column = std::make_shared<SVerticalBox>();
    auto add_widget = [&](const std::shared_ptr<SWidget>& w, float top = 0.0f) {
        column->AddSlot(w).AutoSize().SetPadding(FMargin(0.0f, top * scale, 0.0f, 4.0f * scale));
    };
    auto add_row = [&](const std::string& label, const std::shared_ptr<SWidget>& content) {
        auto r = std::make_shared<SHorizontalBox>();
        r->AddSlot(text(label, 13.0f, kLabel))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 0.0f, 10.0f * scale, 0.0f))
            .SetVAlign(EVerticalAlignment::Center);
        r->AddSlot(content).AutoSize().SetVAlign(EVerticalAlignment::Center);
        column->AddSlot(r).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));
    };
    auto add_text_row = [&](const std::string& label, const std::string& value) {
        add_row(label, text(value.empty() ? "(none)" : value, 13.0f, kValue));
    };
    auto make_button = [&](const std::string& label, std::function<void()> on_click) {
        auto b = std::make_shared<SButton>();
        b->Padding = FMargin(10.0f * scale, 4.0f * scale);
        b->SetContent(text(label, 13.0f, kValue));
        b->OnClicked = std::move(on_click);
        return b;
    };

    // Read-only per-line source view inside a collapsed expandable area. We cap
    // the line count so a huge generated shader doesn't spawn thousands of leaf
    // widgets; the cap is generous for hand-authored sources.
    auto add_source_view = [&](const std::string& title, const std::string& source) {
        auto body = std::make_shared<SVerticalBox>();
        if (source.empty())
        {
            body->AddSlot(text("(empty)", 12.0f, kDim)).AutoSize();
        }
        else
        {
            const size_t kMaxLines = 400;
            size_t line_start = 0;
            size_t line_no = 0;
            while (line_start <= source.size() && line_no < kMaxLines)
            {
                size_t nl = source.find('\n', line_start);
                std::string line = (nl == std::string::npos) ? source.substr(line_start)
                                                             : source.substr(line_start, nl - line_start);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                body->AddSlot(text(line.empty() ? " " : line, 12.0f, kValue)).AutoSize();
                ++line_no;
                if (nl == std::string::npos)
                    break;
                line_start = nl + 1;
            }
            const size_t total_lines = static_cast<size_t>(std::count(source.begin(), source.end(), '\n')) + 1;
            if (total_lines > kMaxLines)
                body->AddSlot(text("... (" + std::to_string(total_lines - kMaxLines) + " more lines; open in editor)",
                                   12.0f, kDim))
                    .AutoSize();
        }
        auto area = std::make_shared<SExpandableArea>();
        area->Title = title;
        area->Expanded = false;
        area->FontSize = 14.0f * scale;
        area->HeaderHeight = 24.0f * scale;
        area->SetContent(body);
        column->AddSlot(area).AutoSize().SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
    };

    auto wrap = [&](const std::shared_ptr<SVerticalBox>& col) -> std::shared_ptr<SWidget> {
        auto scroll = std::make_shared<SScrollBox>();
        scroll->AddChild(col);
        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = UIColor(0.10f, 0.10f, 0.13f, 1.0f);
        border->Padding = FMargin(16.0f * scale);
        border->HAlign = EHorizontalAlignment::Fill;
        border->VAlign = EVerticalAlignment::Fill;
        border->SetContent(scroll);
        return border;
    };

    std::string extension = asset_path.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // ---------------------------------------------------------------------
    // .shader (ShaderLab source) path.
    // ---------------------------------------------------------------------
    if (extension == ".shader")
    {
        static std::filesystem::path s_sl_path;
        static std::string s_sl_source;
        static bool s_sl_loaded = false;
        static std::string s_sl_error;
        static std::vector<std::string> s_sl_warnings;
        static std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> s_sl_asset;
        static std::string s_sl_status;

        auto reparse = []() {
            s_sl_asset.reset();
            s_sl_error.clear();
            s_sl_warnings.clear();
            s_sl_loaded = false;
            ZEngine::ShaderLab::ShaderLabParser parser(s_sl_source);
            if (!parser.Parse())
            {
                s_sl_error = parser.GetError().empty() ? "Failed to parse ShaderLab source." : parser.GetError();
                s_sl_warnings = parser.GetWarnings();
                return;
            }
            s_sl_asset = parser.GetAsset();
            s_sl_warnings = parser.GetWarnings();
            s_sl_loaded = (s_sl_asset != nullptr);
            if (!s_sl_loaded)
                s_sl_error = "ShaderLab parser returned an empty asset.";
        };
        auto reload = [&]() {
            s_sl_source.clear();
            s_sl_asset.reset();
            s_sl_error.clear();
            s_sl_warnings.clear();
            s_sl_loaded = false;
            s_sl_status.clear();
            if (!ReadTextFileForInspector(asset_path, s_sl_source))
            {
                s_sl_error = "Unable to read shader source file.";
                return;
            }
            reparse();
        };

        if (s_sl_path != asset_path)
        {
            s_sl_path = asset_path;
            reload();
        }

        const std::string asset_name = asset_path.stem().generic_string();
        const std::string relative_path = MakeProjectRelativePathForInspector(asset_path);
        const std::string display_path = relative_path.empty() ? asset_path.generic_string() : relative_path;

        add_widget(text("Shader (ShaderLab)", 20.0f, kHeader), 0.0f);

        // Open / Reveal / Refresh / Reimport buttons.
        {
            auto bar = std::make_shared<SHorizontalBox>();
            const std::filesystem::path path_copy = asset_path;
            bar->AddSlot(make_button("Open", [path_copy]() { OpenFileWithSystemDefault(path_copy); })).AutoSize();
            bar->AddSlot(make_button("Reveal", [path_copy]() {
                   EditorUtility::RevealInFinder(path_copy.generic_string().c_str());
               }))
                .AutoSize()
                .SetPadding(FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
            bar->AddSlot(make_button("Refresh", [request_rebuild]() {
                   s_sl_path.clear();  // force the top-of-build reload (asset_path valid there)
                   if (request_rebuild)
                       request_rebuild();
               }))
                .AutoSize()
                .SetPadding(FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
            bar->AddSlot(make_button("Reimport", [path_copy, request_rebuild]() {
                   if (ReimportShaderLabAfterSourceSave(path_copy))
                       s_sl_status = "Shader reimported.";
                   else
                       s_sl_status = "Reimport failed (see log).";
                   if (request_rebuild)
                       request_rebuild();
               }))
                .AutoSize()
                .SetPadding(FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
            column->AddSlot(bar).AutoSize().SetPadding(FMargin(0.0f, 8.0f * scale, 0.0f, 6.0f * scale));
        }

        if (!s_sl_status.empty())
            add_widget(text(s_sl_status, 13.0f, kValue));

        if (!s_sl_loaded || s_sl_asset == nullptr)
        {
            add_text_row("Name", asset_name);
            add_text_row("Path", display_path);
            add_widget(text("Parse Error", 14.0f, kErr), 6.0f);
            add_widget(text(s_sl_error.empty() ? "Unable to load shader asset." : s_sl_error, 13.0f, kValue));
            add_source_view("Source", s_sl_source);
            return wrap(column);
        }

        const ZEngine::ShaderLab::ShaderSubShader* primary = GetPrimaryShaderLabSubShader(s_sl_asset);
        const std::string shader_name = s_sl_asset->shader_name.empty() ? asset_name : s_sl_asset->shader_name;
        const std::string render_pipeline =
            primary == nullptr ? std::string() : GetShaderLabTagValue(primary->tags, "RenderPipeline");
        const std::string render_type =
            primary == nullptr ? std::string() : GetShaderLabTagValue(primary->tags, "RenderType");
        const std::string queue_tag = primary == nullptr ? std::string() : GetShaderLabTagValue(primary->tags, "Queue");
        const std::string queue_label = queue_tag.empty() ? std::string("Geometry") : queue_tag;
        const int render_queue = GetUnityRenderQueueFromShaderLabTag(queue_label);
        const int lod_value = primary == nullptr ? 0 : primary->lod;
        const size_t pass_count = CountShaderLabPasses(s_sl_asset);

        const char* primary_language = "HLSL";
        if (primary != nullptr && !primary->passes.empty() && !primary->passes.front().programs.empty())
            primary_language = ShaderLanguageToString(primary->passes.front().programs.front().language);

        add_text_row("Name", shader_name);
        add_text_row("Path", display_path);
        add_text_row("Source language", primary_language);
        add_text_row("Render pipeline", render_pipeline);
        add_text_row("Render type", render_type);
        add_text_row("Render queue", std::to_string(render_queue));
        add_text_row("Queue tag", queue_label);
        add_text_row("LOD", std::to_string(lod_value));
        add_text_row("Fallback", std::string(s_sl_asset->fallback.c_str()));
        add_text_row("Custom editor", std::string(s_sl_asset->custom_editor.c_str()));
        add_text_row("Pass count", std::to_string(pass_count));
        add_text_row("SubShader count", std::to_string(s_sl_asset->subshaders.size()));

        if (!s_sl_warnings.empty())
        {
            auto warn_box = std::make_shared<SVerticalBox>();
            for (const std::string& w : s_sl_warnings)
                warn_box->AddSlot(text("- " + w, 12.0f, UIColor(1.0f, 0.8f, 0.3f, 1.0f))).AutoSize();
            auto area = std::make_shared<SExpandableArea>();
            area->Title = "Warnings";
            area->Expanded = true;
            area->FontSize = 14.0f * scale;
            area->HeaderHeight = 24.0f * scale;
            area->SetContent(warn_box);
            column->AddSlot(area).AutoSize().SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
        }

        std::vector<std::string> keywords;
        CollectShaderLabKeywords(s_sl_asset, keywords);
        if (!keywords.empty())
        {
            auto kw_box = std::make_shared<SVerticalBox>();
            for (const std::string& k : keywords)
                kw_box->AddSlot(text("- " + k, 12.0f, kValue)).AutoSize();
            auto area = std::make_shared<SExpandableArea>();
            area->Title = "Keywords";
            area->Expanded = false;
            area->FontSize = 14.0f * scale;
            area->HeaderHeight = 24.0f * scale;
            area->SetContent(kw_box);
            column->AddSlot(area).AutoSize().SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
        }

        // Properties (read-only).
        if (!s_sl_asset->properties.empty())
        {
            auto prop_box = std::make_shared<SVerticalBox>();
            for (const auto& p : s_sl_asset->properties)
            {
                std::string line = p.name;
                if (!p.display_name.empty())
                    line += " (" + p.display_name + ")";
                line += " : ";
                line += ZEngine::ShaderLab::PropertyTypeToString(p.type);
                prop_box->AddSlot(text(line, 12.0f, kValue)).AutoSize();
            }
            auto area = std::make_shared<SExpandableArea>();
            area->Title = "Properties";
            area->Expanded = false;
            area->FontSize = 14.0f * scale;
            area->HeaderHeight = 24.0f * scale;
            area->SetContent(prop_box);
            column->AddSlot(area).AutoSize().SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
        }

        add_source_view("Source (read-only -- edit via Open)", s_sl_source);
        return wrap(column);
    }

    // ---------------------------------------------------------------------
    // .zasset (ShaderRes) path.
    // ---------------------------------------------------------------------
    static std::filesystem::path s_path;
    static ShaderRes s_shader;
    static bool s_loaded = false;
    static std::string s_status;
    static bool s_status_ok = false;

    if (s_path != asset_path)
    {
        s_path = asset_path;
        s_loaded = false;
        s_status.clear();
        s_status_ok = false;
        ResetShaderAssetForInspector(s_shader, asset_path);
        if (std::filesystem::exists(asset_path))
        {
            std::filesystem::path read_path = asset_path;
            const ShaderRes* loaded = GET_SYSTEM(AssetManager)->ReadObject<ShaderRes>(read_path);
            if (loaded != nullptr)
            {
                CopyShaderAssetForInspector(s_shader, *loaded);
                s_loaded = true;
                EnsureShaderPassCompatibility(s_shader);
                SyncLegacyShaderFieldsFromPrimaryPass(s_shader);
            }
        }
    }

    add_widget(text("Shader", 20.0f, kHeader), 0.0f);
    add_text_row("Name", asset_path.stem().generic_string());
    add_text_row("Path", asset_path.generic_string());

    if (!s_loaded)
    {
        add_widget(text("Unable to load shader asset.", 14.0f, kDim), 6.0f);
        return wrap(column);
    }

    // Editable scalar fields (committed on Enter; persisted by Save).
    auto edit_string = [&](const std::string& label, eastl::string* field) {
        auto tb = std::make_shared<SEditableTextBox>();
        tb->FontSize = 13.0f * scale;
        tb->MinWidth = 200.0f * scale;
        tb->Padding = FMargin(6.0f * scale, 3.0f * scale);
        tb->Text = field->c_str();
        tb->OnTextCommitted = [field](const std::string& s) { field->assign(s.c_str()); };
        add_row(label, tb);
    };
    auto edit_bool = [&](const std::string& label, bool* field) {
        auto cb = std::make_shared<SCheckBox>();
        cb->Checked = *field;
        cb->BoxSize = 16.0f * scale;
        cb->OnCheckStateChanged = [field](bool v) { *field = v; };
        add_row(label, cb);
    };

    edit_string("Shader Name", &s_shader.m_ShaderName);
    edit_string("Source Language", &s_shader.m_SourceLanguage);
    edit_string("Include Root", &s_shader.m_IncludeDirectory);
    add_text_row("Include Path", MakeShaderDisplayPath(s_shader.m_IncludeDirectory));
    edit_bool("Enable DX12", &s_shader.m_EnableDx12);
    edit_bool("Enable Vulkan", &s_shader.m_EnableVulkan);
    edit_bool("Enable Metal", &s_shader.m_EnableMetal);

    SyncLegacyShaderFieldsFromPrimaryPass(s_shader);
    add_text_row("Primary Vertex", MakeShaderDisplayPath(s_shader.m_VertexShaderFile));
    add_text_row("Primary Fragment", MakeShaderDisplayPath(s_shader.m_FragmentShaderFile));

    // Properties (read-only summary).
    if (!s_shader.m_Properties.empty())
    {
        auto box = std::make_shared<SVerticalBox>();
        for (const auto& p : s_shader.m_Properties)
        {
            std::string line = std::string(p.m_Name.c_str()) + " : " + std::string(p.m_Type.c_str());
            box->AddSlot(text(line, 12.0f, kValue)).AutoSize();
        }
        auto area = std::make_shared<SExpandableArea>();
        area->Title = "Properties";
        area->Expanded = false;
        area->FontSize = 14.0f * scale;
        area->HeaderHeight = 24.0f * scale;
        area->SetContent(box);
        column->AddSlot(area).AutoSize().SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
    }
    // Passes (read-only summary).
    if (!s_shader.m_Passes.empty())
    {
        auto box = std::make_shared<SVerticalBox>();
        for (const auto& p : s_shader.m_Passes)
        {
            std::string line = std::string(p.m_Name.c_str()) + " [" + std::string(p.m_LightMode.c_str()) + "]";
            box->AddSlot(text(line, 12.0f, kValue)).AutoSize();
        }
        auto area = std::make_shared<SExpandableArea>();
        area->Title = "Passes";
        area->Expanded = false;
        area->FontSize = 14.0f * scale;
        area->HeaderHeight = 24.0f * scale;
        area->SetContent(box);
        column->AddSlot(area).AutoSize().SetPadding(FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
    }

    // Action buttons.
    {
        auto bar1 = std::make_shared<SHorizontalBox>();
        bar1->AddSlot(make_button("Create/Open Vertex", [request_rebuild]() {
               if (EnsureShaderSourceFile(s_shader.m_VertexShaderFile, s_shader.m_ShaderName, true))
               {
                   s_status = "Vertex source is ready.";
                   s_status_ok = true;
                   OpenFileWithSystemDefault(ResolveProjectAssetPath(s_shader.m_VertexShaderFile));
               }
               else
               {
                   s_status = "Failed to create or open the Vertex source file.";
                   s_status_ok = false;
               }
               if (request_rebuild)
                   request_rebuild();
           }))
            .AutoSize();
        bar1->AddSlot(make_button("Reveal Vertex", []() {
               const std::filesystem::path p = ResolveProjectAssetPath(s_shader.m_VertexShaderFile);
               if (!p.empty())
                   EditorUtility::RevealInFinder(p.generic_string().c_str());
           }))
            .AutoSize()
            .SetPadding(FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
        column->AddSlot(bar1).AutoSize().SetPadding(FMargin(0.0f, 10.0f * scale, 0.0f, 4.0f * scale));

        auto bar2 = std::make_shared<SHorizontalBox>();
        bar2->AddSlot(make_button("Create/Open Fragment", [request_rebuild]() {
               if (EnsureShaderSourceFile(s_shader.m_FragmentShaderFile, s_shader.m_ShaderName, false))
               {
                   s_status = "Fragment source is ready.";
                   s_status_ok = true;
                   OpenFileWithSystemDefault(ResolveProjectAssetPath(s_shader.m_FragmentShaderFile));
               }
               else
               {
                   s_status = "Failed to create or open the Fragment source file.";
                   s_status_ok = false;
               }
               if (request_rebuild)
                   request_rebuild();
           }))
            .AutoSize();
        bar2->AddSlot(make_button("Reveal Fragment", []() {
               const std::filesystem::path p = ResolveProjectAssetPath(s_shader.m_FragmentShaderFile);
               if (!p.empty())
                   EditorUtility::RevealInFinder(p.generic_string().c_str());
           }))
            .AutoSize()
            .SetPadding(FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
        column->AddSlot(bar2).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

        auto bar3 = std::make_shared<SHorizontalBox>();
        bar3->AddSlot(make_button("Compile DX12", [request_rebuild]() {
               SyncLegacyShaderFieldsFromPrimaryPass(s_shader);
               s_status_ok = CompileShaderAssetSources(s_shader, s_status);
               if (request_rebuild)
                   request_rebuild();
           }))
            .AutoSize();
        const std::filesystem::path save_path = asset_path;
        bar3->AddSlot(make_button("Save", [save_path, request_rebuild]() {
               SyncLegacyShaderFieldsFromPrimaryPass(s_shader);
               GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(save_path, s_shader);
               s_status = "Shader asset saved.";
               s_status_ok = true;
               if (request_rebuild)
                   request_rebuild();
           }))
            .AutoSize()
            .SetPadding(FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
        column->AddSlot(bar3).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));
    }

    if (!s_status.empty())
        add_widget(text(s_status, 13.0f, s_status_ok ? kOk : kErr), 6.0f);

    return wrap(column);
}

}  // namespace InspectorShaderDetail

#include "InspectorShaderInspector.h"

std::shared_ptr<ZSlate::SWidget> BuildShaderInspectorWidget(const std::filesystem::path& asset_path,
                                                            float scale,
                                                            const std::function<void()>& request_rebuild)
{
    return InspectorShaderDetail::BuildShaderInspectorWidgetImpl(asset_path, scale, request_rebuild);
}

std::vector<MaterialInspectorRow> EnumerateBuiltInMaterialRows(Material& material)
{
    return InspectorShaderDetail::EnumerateBuiltInMaterialRows(material);
}

std::vector<MaterialInspectorRow> EnumerateShaderMaterialRows(Material& material,
                                                              const ShaderRes& shader,
                                                              bool& out_created)
{
    return InspectorShaderDetail::EnumerateShaderMaterialRows(material, shader, out_created);
}

std::vector<std::string> CollectMaterialShaderVariantKeywords(const eastl::string& shader_name,
                                                              const std::filesystem::path& shader_source_path)
{
    return InspectorShaderDetail::CollectMaterialShaderVariantKeywords(shader_name, shader_source_path);
}

bool LoadProjectShaderDefinitionForInspector(ShaderRes& out_shader,
                                               const eastl::string& shader_name,
                                               std::filesystem::path* resolved_shader_path)
{
    return InspectorShaderDetail::LoadProjectShaderDefinitionForInspector(out_shader, shader_name, resolved_shader_path);
}

std::vector<eastl::string> FindProjectShaders()
{
    return InspectorShaderDetail::FindProjectShaders();
}

void SanitizeMaterialShaderBindingForInspector(Material& material)
{
    InspectorShaderDetail::SanitizeMaterialShaderBindingForInspector(material);
}

void EnsureShaderPassCompatibility(ShaderRes& shader)
{
    InspectorShaderDetail::EnsureShaderPassCompatibility(shader);
}

void SyncLegacyShaderFieldsFromPrimaryPass(ShaderRes& shader)
{
    InspectorShaderDetail::SyncLegacyShaderFieldsFromPrimaryPass(shader);
}

eastl::string GetMaterialAuthoringShaderName(const Material& material)
{
    return InspectorShaderDetail::GetMaterialAuthoringShaderName(material);
}

bool IsShaderSourceAvailableInProject(const eastl::string& shader_name)
{
    return InspectorShaderDetail::IsShaderSourceAvailableInProject(shader_name);
}