#include "Runtime/Function/ShaderLab/ShaderLabHlslExtract.h"

#include "Runtime/Function/ShaderLab/ShaderLabParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ZEngine::ShaderLab
{
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

        bool FillFromProgramBlock(const std::string& program_source, ShaderStage stage, const std::string& fallback_entry, ShaderLabHlslExtractResult& out)
        {
            std::string vertex_entry = (stage == ShaderStage::Vertex) ? "vert" : std::string {};
            std::string fragment_entry = (stage == ShaderStage::Fragment) ? "frag" : std::string {};
            ExtractEntryPointsFromProgramText(program_source, vertex_entry, fragment_entry);

            const std::string body = SanitizeProgramBody(program_source);
            if (body.empty())
            {
                return false;
            }

            out.hlsl_source = body;
            out.entry_point = PickEntryForStage(stage, vertex_entry, fragment_entry, fallback_entry);
            out.ok = true;
            out.error_message.clear();
            return true;
        }

        bool TryExtractFromParser(const std::string& shader_source, ShaderStage stage, const std::string& fallback_entry, ShaderLabHlslExtractResult& out, std::string& parse_error)
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

            for (const ShaderSubShader& subshader : asset->subshaders)
            {
                for (const ShaderPass& pass : subshader.passes)
                {
                    for (const ShaderProgram& program : pass.programs)
                    {
                        const std::string& raw =
                            !program.hlsl_code.empty() ? program.hlsl_code : program.source_code;
                        if (raw.empty())
                        {
                            continue;
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
                            continue;
                        }

                        out.hlsl_source = body;
                        out.entry_point = PickEntryForStage(stage, vertex_entry, fragment_entry, fallback_entry);
                        out.ok = true;
                        out.error_message.clear();
                        return true;
                    }
                }
            }

            parse_error.clear();
            return false;
        }

        bool TryExtractFromTextMarkers(const std::string& shader_source, ShaderStage stage, const std::string& fallback_entry, ShaderLabHlslExtractResult& out)
        {
            static constexpr std::array<std::pair<const char*, const char*>, 2> k_markers = {
                {{"HLSLPROGRAM", "ENDHLSL"}, {"CGPROGRAM", "ENDCG"}}};

            for (const auto& markers : k_markers)
            {
                const size_t begin = shader_source.find(markers.first);
                if (begin == std::string::npos)
                {
                    continue;
                }

                const size_t content_begin = begin + std::strlen(markers.first);
                const size_t end = shader_source.find(markers.second, content_begin);
                if (end == std::string::npos)
                {
                    continue;
                }

                const std::string program_source = shader_source.substr(content_begin, end - content_begin);
                if (FillFromProgramBlock(program_source, stage, fallback_entry, out))
                {
                    return true;
                }
            }

            return false;
        }

    }  // namespace

    ShaderLabHlslExtractResult ExtractHlslFromShaderLabSource(const std::string& shader_source,
                                                              const std::string& file_path_for_errors,
                                                              ShaderStage stage,
                                                              const std::string& fallback_entry)
    {
        ShaderLabHlslExtractResult out {};
        std::string parse_error;

        if (TryExtractFromParser(shader_source, stage, fallback_entry, out, parse_error))
        {
            return out;
        }

        if (TryExtractFromTextMarkers(shader_source, stage, fallback_entry, out))
        {
            return out;
        }

        out.ok = false;
        if (!parse_error.empty())
        {
            out.error_message = parse_error;
        }
        else
        {
            out.error_message = "No HLSLPROGRAM/CGPROGRAM block found in " + file_path_for_errors;
        }
        return out;
    }

    ShaderLabHlslExtractResult ExtractHlslFromShaderLabFile(const std::string& file_path,
                                                            ShaderStage stage,
                                                            const std::string& fallback_entry)
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
        return ExtractHlslFromShaderLabSource(buffer.str(), file_path, stage, fallback_entry);
    }

}  // namespace ZEngine::ShaderLab
