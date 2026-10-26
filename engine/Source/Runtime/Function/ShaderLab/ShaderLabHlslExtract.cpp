#include "Runtime/Function/ShaderLab/ShaderLabHlslExtract.h"

#include "Runtime/Function/ShaderLab/ShaderLabParser.h"
#include "Runtime/Function/ShaderLab/ShaderLabSemanticRewrite.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ZEngine::ShaderLab
{
    bool IsShaderLabSourcePath(const std::string& file_path)
    {
        std::string ext = std::filesystem::path(file_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".shader";
    }

    namespace
    {

        std::string TrimLine(std::string s)
        {
            auto not_space = [](unsigned char c) {
                return !std::isspace(c);
            };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
            s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
            return s;
        }

        std::string ToLowerAscii(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        /// 决定"哪一类 program 块算数"。
        ///
        /// 同一个 .shader 里可以同时有 HLSLPROGRAM 和 GLSLPROGRAM：前者给 DX12，
        /// 后者给 Vulkan / Metal。两边不能混取 —— 把 HLSL 喂给 glslang、或把
        /// GLSL 喂给 DXC，都是编译错误。所以提取时按语言过滤。
        struct ProgramExtractFilter
        {
            ShaderLanguage language {ShaderLanguage::HLSL};

            /// 只有 HLSL 需要把 Cg 时代的输出语义 `COLOR` 改写成 `SV_Target`；
            /// GLSL 没有语义这个概念，改写只会破坏源码。
            bool rewrite_legacy_semantics {true};

            bool Accepts(ShaderLanguage candidate) const
            {
                if (language == ShaderLanguage::GLSL)
                {
                    return candidate == ShaderLanguage::GLSL;
                }
                // Cg 与 HLSL 走同一条提取 / 编译链路。
                return candidate == ShaderLanguage::HLSL || candidate == ShaderLanguage::CG;
            }

            /// 没有 `#pragma vertex/fragment` 时的入口名默认值。Unity 的 HLSL 例子
            /// 惯用 vert/frag，GLSL 则一律 main。
            const char* VertexEntryDefault() const { return language == ShaderLanguage::GLSL ? "main" : "vert"; }
            const char* FragmentEntryDefault() const { return language == ShaderLanguage::GLSL ? "main" : "frag"; }
        };

        bool StartsWithInsensitive(const std::string& value, const char* prefix)
        {
            const size_t prefix_length = std::strlen(prefix);
            if (value.size() < prefix_length)
            {
                return false;
            }
            for (size_t index = 0; index < prefix_length; ++index)
            {
                if (std::tolower(static_cast<unsigned char>(value[index])) !=
                    std::tolower(static_cast<unsigned char>(prefix[index])))
                {
                    return false;
                }
            }
            return true;
        }

        void ExtractEntryPointsFromProgramText(const std::string& program_source,
                                               std::string& vertex_entry,
                                               std::string& fragment_entry)
        {
            std::istringstream stream(program_source);
            std::string line;
            while (std::getline(stream, line))
            {
                const std::string trimmed = TrimLine(line);
                if (!StartsWithInsensitive(trimmed, "#pragma"))
                {
                    continue;
                }

                std::istringstream line_stream(trimmed);
                std::string pragma_token;
                std::string stage_token;
                std::string entry_token;
                line_stream >> pragma_token >> stage_token >> entry_token;
                stage_token = ToLowerAscii(stage_token);
                if (stage_token == "vertex" && !entry_token.empty())
                {
                    vertex_entry = entry_token;
                }
                else if ((stage_token == "fragment" || stage_token == "pixel") && !entry_token.empty())
                {
                    fragment_entry = entry_token;
                }
            }
        }

        std::string SanitizeProgramBody(const std::string& program_source)
        {
            std::istringstream stream(program_source);
            std::ostringstream sanitized;
            std::string line;
            bool first_line = true;
            while (std::getline(stream, line))
            {
                const std::string trimmed = TrimLine(line);
                if (StartsWithInsensitive(trimmed, "#pragma"))
                {
                    continue;
                }

                if (!first_line)
                {
                    sanitized << '\n';
                }
                sanitized << line;
                first_line = false;
            }

            return TrimLine(sanitized.str());
        }

        std::string PickEntryForStage(ShaderStage stage, const std::string& vertex_entry, const std::string& fragment_entry, const std::string& fallback_entry)
        {
            switch (stage)
            {
                case ShaderStage::Vertex:
                    return !vertex_entry.empty() ? vertex_entry : (!fallback_entry.empty() ? fallback_entry : "main");
                case ShaderStage::Fragment:
                    return !fragment_entry.empty() ? fragment_entry : (!fallback_entry.empty() ? fallback_entry : "main");
                default:
                    return !fallback_entry.empty() ? fallback_entry : "main";
            }
        }

        bool FillFromProgramBlock(const std::string& program_source,
                                  ShaderStage stage,
                                  const std::string& fallback_entry,
                                  const ProgramExtractFilter& filter,
                                  std::string& out_source,
                                  std::string& out_entry)
        {
            std::string vertex_entry = (stage == ShaderStage::Vertex) ? filter.VertexEntryDefault() : std::string {};
            std::string fragment_entry =
                (stage == ShaderStage::Fragment) ? filter.FragmentEntryDefault() : std::string {};
            ExtractEntryPointsFromProgramText(program_source, vertex_entry, fragment_entry);

            const std::string body = SanitizeProgramBody(program_source);
            if (body.empty())
            {
                return false;
            }

            out_entry = PickEntryForStage(stage, vertex_entry, fragment_entry, fallback_entry);
            out_source = filter.rewrite_legacy_semantics ? RewriteLegacyOutputSemantics(body, out_entry, stage).code
                                                         : body;
            return true;
        }

        bool FillFromProgram(const ShaderProgram& program,
                             ShaderStage stage,
                             const std::string& fallback_entry,
                             const ProgramExtractFilter& filter,
                             std::string& out_source,
                             std::string& out_entry)
        {
            if (!filter.Accepts(program.language))
            {
                return false;
            }

            const std::string& raw = !program.hlsl_code.empty() ? program.hlsl_code : program.source_code;
            if (raw.empty())
            {
                return false;
            }

            std::string vertex_entry = program.vertex_entry;
            std::string fragment_entry = program.fragment_entry;
            if (vertex_entry.empty() && fragment_entry.empty())
            {
                ExtractEntryPointsFromProgramText(raw, vertex_entry, fragment_entry);
            }

            const std::string body = SanitizeProgramBody(raw);
            if (body.empty())
            {
                return false;
            }

            out_entry = PickEntryForStage(stage, vertex_entry, fragment_entry, fallback_entry);
            out_source = filter.rewrite_legacy_semantics ? RewriteLegacyOutputSemantics(body, out_entry, stage).code
                                                         : body;
            return true;
        }

        bool TryExtractFromParser(const std::string& shader_source,
                                  ShaderStage stage,
                                  const std::string& fallback_entry,
                                  std::size_t subshader_index,
                                  std::size_t pass_index,
                                  const ProgramExtractFilter& filter,
                                  std::string& out_source,
                                  std::string& out_entry,
                                  std::string& parse_error)
        {
            ShaderLabParser parser(shader_source);
            if (!parser.Parse())
            {
                parse_error = parser.GetError();
                return false;
            }

            const std::shared_ptr<ShaderLabAsset> asset = parser.GetAsset();
            if (asset == nullptr)
            {
                parse_error = "ShaderLab asset is null.";
                return false;
            }

            // 指定了 SubShader / Pass：直接取那一个，不再扫全文件。
            if (subshader_index != kExtractAnySubShader || pass_index != kExtractAnyPass)
            {
                const std::size_t resolved_sub = subshader_index == kExtractAnySubShader ? 0 : subshader_index;
                const std::size_t resolved_pass = pass_index == kExtractAnyPass ? 0 : pass_index;

                if (resolved_sub >= asset->subshaders.size())
                {
                    parse_error = "SubShader index out of range";
                    return false;
                }

                const ShaderSubShader& subshader = asset->subshaders[resolved_sub];
                if (resolved_pass >= subshader.passes.size())
                {
                    parse_error = "Pass index out of range";
                    return false;
                }

                const ShaderPass& pass = subshader.passes[resolved_pass];
                bool has_matching_program = false;
                for (const ShaderProgram& program : pass.programs)
                {
                    if (filter.Accepts(program.language))
                    {
                        has_matching_program = true;
                        if (FillFromProgram(program, stage, fallback_entry, filter, out_source, out_entry))
                        {
                            return true;
                        }
                    }
                }
                if (!has_matching_program)
                {
                    parse_error = filter.language == ShaderLanguage::GLSL ? "No GLSLPROGRAM in pass"
                                                                          : "No HLSLPROGRAM/CGPROGRAM in pass";
                    return false;
                }
                parse_error = "Pass has no compilable program body";
                return false;
            }

            for (const ShaderSubShader& subshader : asset->subshaders)
            {
                for (const ShaderPass& pass : subshader.passes)
                {
                    for (const ShaderProgram& program : pass.programs)
                    {
                        if (FillFromProgram(program, stage, fallback_entry, filter, out_source, out_entry))
                        {
                            return true;
                        }
                    }
                }
            }

            parse_error.clear();
            return false;
        }

        using TextMarkers = std::array<std::pair<const char*, const char*>, 2>;

        // 第二项留空表示"GLSL 只有一对标记"。
        const TextMarkers k_HlslTextMarkers {{std::pair<const char*, const char*> {"HLSLPROGRAM", "ENDHLSL"},
                                              std::pair<const char*, const char*> {"CGPROGRAM", "ENDCG"}}};
        const TextMarkers k_GlslTextMarkers {{std::pair<const char*, const char*> {"GLSLPROGRAM", "ENDGLSL"},
                                              std::pair<const char*, const char*> {nullptr, nullptr}}};

        bool TryExtractFromTextMarkers(const std::string& shader_source,
                                       ShaderStage stage,
                                       const std::string& fallback_entry,
                                       const TextMarkers& markers,
                                       const ProgramExtractFilter& filter,
                                       std::string& out_source,
                                       std::string& out_entry)
        {
            for (const auto& marker : markers)
            {
                if (marker.first == nullptr)
                {
                    continue;
                }

                const size_t begin = shader_source.find(marker.first);
                if (begin == std::string::npos)
                {
                    continue;
                }

                const size_t content_begin = begin + std::strlen(marker.first);
                const size_t end = shader_source.find(marker.second, content_begin);
                if (end == std::string::npos)
                {
                    continue;
                }

                const std::string program_source = shader_source.substr(content_begin, end - content_begin);
                if (FillFromProgramBlock(program_source, stage, fallback_entry, filter, out_source, out_entry))
                {
                    return true;
                }
            }

            return false;
        }

    }  // namespace

    namespace
    {
        /// HLSL / GLSL 两条提取路径共用的主体。差别只在"接受哪类 program 块"、
        /// 要不要做语义改写、以及纯文本兜底时扫哪些标记。
        ///
        /// 成功时 `out_source` / `out_entry` 被填上；失败时 `out_error` 带上原因
        /// （解析错误优先，其次才是"没找到块"）。
        bool ExtractProgramFromShaderLabSource(const std::string& shader_source,
                                               ShaderStage stage,
                                               const std::string& fallback_entry,
                                               std::size_t subshader_index,
                                               std::size_t pass_index,
                                               const ProgramExtractFilter& filter,
                                               const TextMarkers& markers,
                                               std::string& out_source,
                                               std::string& out_entry,
                                               std::string& out_error)
        {
            std::string parse_error;
            if (TryExtractFromParser(shader_source,
                                     stage,
                                     fallback_entry,
                                     subshader_index,
                                     pass_index,
                                     filter,
                                     out_source,
                                     out_entry,
                                     parse_error))
            {
                return true;
            }

            if (TryExtractFromTextMarkers(shader_source, stage, fallback_entry, markers, filter, out_source, out_entry))
            {
                return true;
            }

            out_error = parse_error;
            return false;
        }
    }  // namespace

    ShaderLabHlslExtractResult ExtractHlslFromShaderLabSource(const std::string& shader_source,
                                                              const std::string& file_path_for_errors,
                                                              ShaderStage stage,
                                                              const std::string& fallback_entry,
                                                              std::size_t subshader_index,
                                                              std::size_t pass_index)
    {
        ShaderLabHlslExtractResult out {};
        std::string error;

        if (ExtractProgramFromShaderLabSource(shader_source,
                                              stage,
                                              fallback_entry,
                                              subshader_index,
                                              pass_index,
                                              ProgramExtractFilter {},
                                              k_HlslTextMarkers,
                                              out.hlsl_source,
                                              out.entry_point,
                                              error))
        {
            out.ok = true;
            return out;
        }

        out.ok = false;
        out.hlsl_source.clear();
        out.error_message = !error.empty() ? error : ("No HLSLPROGRAM/CGPROGRAM block found in " + file_path_for_errors);
        return out;
    }

    ShaderLabHlslExtractResult ExtractHlslFromShaderLabFile(const std::string& file_path,
                                                            ShaderStage stage,
                                                            const std::string& fallback_entry,
                                                            std::size_t subshader_index,
                                                            std::size_t pass_index)
    {
        ShaderLabHlslExtractResult out {};
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            out.error_message = "Failed to open shader file: " + file_path;
            return out;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return ExtractHlslFromShaderLabSource(buffer.str(),
                                              file_path,
                                              stage,
                                              fallback_entry,
                                              subshader_index,
                                              pass_index);
    }

    ShaderLabGlslExtractResult ExtractGlslFromShaderLabSource(const std::string& shader_source,
                                                              const std::string& file_path_for_errors,
                                                              ShaderStage stage,
                                                              const std::string& fallback_entry,
                                                              std::size_t subshader_index,
                                                              std::size_t pass_index)
    {
        ShaderLabGlslExtractResult out {};
        std::string error;

        ProgramExtractFilter filter;
        filter.language = ShaderLanguage::GLSL;
        filter.rewrite_legacy_semantics = false;

        if (ExtractProgramFromShaderLabSource(shader_source,
                                              stage,
                                              fallback_entry,
                                              subshader_index,
                                              pass_index,
                                              filter,
                                              k_GlslTextMarkers,
                                              out.glsl_source,
                                              out.entry_point,
                                              error))
        {
            out.ok = true;
            return out;
        }

        out.ok = false;
        out.glsl_source.clear();
        out.error_message = !error.empty() ? error : ("No GLSLPROGRAM block found in " + file_path_for_errors);
        return out;
    }

    ShaderLabGlslExtractResult ExtractGlslFromShaderLabFile(const std::string& file_path,
                                                            ShaderStage stage,
                                                            const std::string& fallback_entry,
                                                            std::size_t subshader_index,
                                                            std::size_t pass_index)
    {
        ShaderLabGlslExtractResult out {};
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            out.error_message = "Failed to open shader file: " + file_path;
            return out;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return ExtractGlslFromShaderLabSource(buffer.str(),
                                              file_path,
                                              stage,
                                              fallback_entry,
                                              subshader_index,
                                              pass_index);
    }

}  // namespace ZEngine::ShaderLab
