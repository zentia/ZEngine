#include "Runtime/Function/ShaderLab/ShaderLabDx12Compiler.h"

#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/ShaderLab/ShaderLabHlslExtract.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#if defined(_WIN32)

namespace ZEngine::ShaderLab
{
    namespace
    {
        constexpr size_t kDefaultMaxVariantsPerPass = 64;

        std::vector<std::string> BuildIncludePaths(const std::filesystem::path& source_path,
                                                   const std::vector<std::string>& extra_paths)
        {
            std::vector<std::string> paths = extra_paths;
            if (!source_path.empty())
            {
                const std::string parent = source_path.parent_path().generic_string();
                if (!parent.empty() &&
                    std::find(paths.begin(), paths.end(), parent) == paths.end())
                {
                    paths.push_back(parent);
                }
            }
            return paths;
        }

        ShaderStage ProgramTypeToStage(const std::string& program_type)
        {
            if (program_type == "vertex")
            {
                return ShaderStage::Vertex;
            }
            if (program_type == "compute")
            {
                return ShaderStage::Compute;
            }
            return ShaderStage::Fragment;
        }

    }  // namespace

    ShaderLabDx12Compiler::ShaderLabDx12Compiler() = default;
    ShaderLabDx12Compiler::~ShaderLabDx12Compiler() = default;

    bool ShaderLabDx12Compiler::LoadFromFile(const std::string& file_path)
    {
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            m_ParseError = "Failed to open file: " + file_path;
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::error_code ec;
        m_SourcePath = std::filesystem::absolute(file_path, ec);
        if (ec)
        {
            m_SourcePath = std::filesystem::path(file_path);
        }
        return LoadFromString(buffer.str());
    }

    bool ShaderLabDx12Compiler::LoadFromString(const std::string& source)
    {
        ShaderLabParser parser(source);
        if (!parser.Parse())
        {
            m_ParseError = parser.GetError();
            m_Asset.reset();
            m_SourceText.clear();
            return false;
        }

        m_Asset = parser.GetAsset();
        m_SourceText = source;
        m_ParseError.clear();
        return m_Asset != nullptr;
    }

    void ShaderLabDx12Compiler::SetCacheDirectory(const std::filesystem::path& dir)
    {
        m_CacheDirectory = dir;
    }

    CompiledShaderModuleDx12 ShaderLabDx12Compiler::Compile(size_t subshader_index,
                                                            size_t pass_index,
                                                            const std::string& program_type,
                                                            const ShaderVariantKey& variant_key)
    {
        CompiledShaderModuleDx12 out;

        if (!m_Asset)
        {
            out.error_message = "No shader loaded";
            return out;
        }

        if (m_SourcePath.empty())
        {
            out.error_message = "DX12 ShaderLab compile requires a file path (LoadFromFile)";
            return out;
        }

        if (subshader_index >= m_Asset->subshaders.size())
        {
            out.error_message = "SubShader index out of range";
            return out;
        }

        const ShaderSubShader& subshader = m_Asset->subshaders[subshader_index];
        if (pass_index >= subshader.passes.size())
        {
            out.error_message = "Pass index out of range";
            return out;
        }

        const ShaderPass& pass = subshader.passes[pass_index];
        if (pass.programs.empty())
        {
            out.error_message = "No programs in pass";
            return out;
        }

        const ShaderProgram& program = pass.programs[0];
        const ShaderStage stage = ProgramTypeToStage(program_type);

        std::string entry_point = program.fragment_entry;
        if (program_type == "vertex")
        {
            entry_point = program.vertex_entry;
        }
        else if (program_type == "compute")
        {
            entry_point = program.compute_entry;
        }

        const std::map<std::string, std::string> macros = VariantKeyToMacros(variant_key);

        DX12ShaderCompiler compiler;
        if (!m_CacheDirectory.empty())
        {
            compiler.SetCacheDirectory(m_CacheDirectory);
        }

        const std::vector<std::string> includes = BuildIncludePaths(m_SourcePath, {});
        const DX12ShaderCompileResult dx_result =
            compiler.CompileFromFile(m_SourcePath.generic_string(),
                                     stage,
                                     includes,
                                     macros,
                                     entry_point.empty() ? "main" : entry_point);

        out.entry_point = entry_point.empty() ? "main" : entry_point;
        out.success = dx_result.success;
        out.dxil_code = dx_result.dxil_code;
        out.error_message = dx_result.error_message;
        return out;
    }

    bool ShaderLabDx12Compiler::PrecompileAll(const std::vector<std::string>& include_paths,
                                              const std::vector<ShaderVariantKey>* material_variant_keys,
                                              bool strip_unused_shader_features)
    {
        (void)include_paths;

        if (!m_Asset || m_SourcePath.empty())
        {
            return false;
        }

        std::vector<ShaderVariantKey> material_keys;
        if (material_variant_keys != nullptr)
        {
            material_keys = *material_variant_keys;
        }

        int compiled = 0;
        int failed = 0;
        bool truncated_any = false;
        int stripped_feature_variants_skipped = 0;

        for (size_t sub_idx = 0; sub_idx < m_Asset->subshaders.size(); ++sub_idx)
        {
            const ShaderSubShader& subshader = m_Asset->subshaders[sub_idx];
            for (size_t pass_idx = 0; pass_idx < subshader.passes.size(); ++pass_idx)
            {
                const ShaderPass& pass = subshader.passes[pass_idx];
                if (pass.programs.empty())
                {
                    continue;
                }

                const std::string& program_source = pass.programs[0].source_code;
                std::vector<MultiCompileLine> lines;
                ExtractMultiCompileLines(program_source, lines);
                if (lines.empty() && !m_SourceText.empty())
                {
                    ExtractMultiCompileLines(m_SourceText, lines);
                }

                bool truncated = false;
                std::vector<ShaderVariantKey> variants;
                if (strip_unused_shader_features)
                {
                    variants = GenerateVariantCombinationsBuildStrip(lines,
                                                                     material_keys,
                                                                     kDefaultMaxVariantsPerPass,
                                                                     &truncated);

                    bool has_shader_feature = false;
                    for (const MultiCompileLine& line : lines)
                    {
                        if (line.is_shader_feature)
                        {
                            has_shader_feature = true;
                            break;
                        }
                    }
                    if (has_shader_feature)
                    {
                        bool full_truncated = false;
                        const std::vector<ShaderVariantKey> full_variants =
                            GenerateVariantCombinations(lines, kDefaultMaxVariantsPerPass, &full_truncated);
                        stripped_feature_variants_skipped +=
                            static_cast<int>(full_variants.size()) - static_cast<int>(variants.size());
                    }
                }
                else
                {
                    variants = GenerateVariantCombinations(lines, kDefaultMaxVariantsPerPass, &truncated);
                }

                if (truncated)
                {
                    truncated_any = true;
                }

                if (lines.empty())
                {
                    LOG_WARNING(ZShader,
                                "ShaderLabDx12Compiler: no #pragma multi_compile / shader_feature in pass for {}",
                                m_SourcePath.generic_string());
                }

                for (const ShaderVariantKey& variant : variants)
                {
                    CompiledShaderModuleDx12 vs = Compile(sub_idx, pass_idx, "vertex", variant);
                    if (vs.success)
                    {
                        ++compiled;
                    }
                    else
                    {
                        ++failed;
                        if (!vs.error_message.empty())
                        {
                            LOG_WARNING(ZShader,
                                        "ShaderLabDx12Compiler: vertex variant failed for {}: {}",
                                        m_SourcePath.generic_string(),
                                        vs.error_message);
                        }
                    }

                    CompiledShaderModuleDx12 fs = Compile(sub_idx, pass_idx, "fragment", variant);
                    if (fs.success)
                    {
                        ++compiled;
                    }
                    else
                    {
                        ++failed;
                        if (!fs.error_message.empty())
                        {
                            LOG_WARNING(ZShader,
                                        "ShaderLabDx12Compiler: fragment variant failed for {}: {}",
                                        m_SourcePath.generic_string(),
                                        fs.error_message);
                        }
                    }
                }
            }
        }

        if (truncated_any)
        {
            LOG_WARNING(ZShader,
                        "ShaderLabDx12Compiler: variant precompile truncated at {} per pass for {}",
                        kDefaultMaxVariantsPerPass,
                        m_SourcePath.generic_string());
        }

        if (stripped_feature_variants_skipped > 0)
        {
            LOG_INFO(ZShader,
                     "ShaderLabDx12Compiler: build strip skipped {} unused shader_feature variant(s) for {}",
                     stripped_feature_variants_skipped,
                     m_SourcePath.generic_string());
        }

        LOG_INFO(ZShader,
                 "ShaderLabDx12Compiler: precompiled {} DXIL variant(s) for {} ({} failed)",
                 compiled,
                 m_SourcePath.generic_string(),
                 failed);

        return failed == 0;
    }

    int ShaderLabDx12Compiler::PrecompileProjectShaders(const std::filesystem::path& shaders_root,
                                                        const std::filesystem::path& cache_directory,
                                                        size_t max_variants_per_pass,
                                                        const MaterialVariantKeysByShader* material_variant_keys)
    {
        if (shaders_root.empty() || cache_directory.empty())
        {
            return 0;
        }

        std::error_code ec;
        if (!std::filesystem::exists(shaders_root, ec) || ec)
        {
            return 0;
        }

        int shaders_done = 0;

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

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".shader")
            {
                continue;
            }

            ShaderLabDx12Compiler compiler;
            compiler.SetCacheDirectory(cache_directory);
            if (!compiler.LoadFromFile(entry.path().generic_string()))
            {
                LOG_WARNING(ZShader,
                            "ShaderLabDx12Compiler: skip {} ({})",
                            entry.path().generic_string(),
                            compiler.GetParseError());
                continue;
            }

            std::vector<ShaderVariantKey> keys_for_shader;
            if (material_variant_keys != nullptr && compiler.GetAsset() != nullptr)
            {
                std::string lookup_name = compiler.GetAsset()->shader_name;
                if (lookup_name.empty())
                {
                    lookup_name = entry.path().stem().generic_string();
                }
    #if defined(_WIN32)
                std::transform(lookup_name.begin(),
                               lookup_name.end(),
                               lookup_name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    #endif
                if (const auto keys_it = material_variant_keys->find(lookup_name); keys_it != material_variant_keys->end())
                {
                    keys_for_shader = keys_it->second;
                }
            }

            if (compiler.PrecompileAll({}, keys_for_shader.empty() ? nullptr : &keys_for_shader, true))
            {
                ++shaders_done;
            }
        }

        if (shaders_done > 0)
        {
            LOG_INFO(ZShader,
                     "ShaderLabDx12Compiler: precompile pass finished for {} shader(s) under {}",
                     shaders_done,
                     shaders_root.generic_string());
        }

        (void)max_variants_per_pass;
        return shaders_done;
    }

}  // namespace ZEngine::ShaderLab

#else  // !defined(_WIN32)

namespace ZEngine::ShaderLab
{

    ShaderLabDx12Compiler::ShaderLabDx12Compiler() = default;
    ShaderLabDx12Compiler::~ShaderLabDx12Compiler() = default;

    bool ShaderLabDx12Compiler::LoadFromFile(const std::string&)
    {
        return false;
    }
    bool ShaderLabDx12Compiler::LoadFromString(const std::string&)
    {
        return false;
    }

    CompiledShaderModuleDx12 ShaderLabDx12Compiler::Compile(size_t,
                                                            size_t,
                                                            const std::string&,
                                                            const ShaderVariantKey&)
    {
        return {};
    }

    bool ShaderLabDx12Compiler::PrecompileAll(const std::vector<std::string>&,
                                              const std::vector<ShaderVariantKey>*,
                                              bool)
    {
        return false;
    }

    void ShaderLabDx12Compiler::SetCacheDirectory(const std::filesystem::path&) {}

    int ShaderLabDx12Compiler::PrecompileProjectShaders(const std::filesystem::path&,
                                                        const std::filesystem::path&,
                                                        size_t,
                                                        const MaterialVariantKeysByShader*)
    {
        return 0;
    }

}  // namespace ZEngine::ShaderLab

#endif
