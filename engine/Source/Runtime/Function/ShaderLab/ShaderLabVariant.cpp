#include "Runtime/Function/ShaderLab/ShaderLabVariant.h"

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>

namespace ZEngine::ShaderLab
{
    namespace
    {

        void AppendTokens(const std::string& token_string, std::vector<std::string>& out_options)
        {
            std::istringstream stream(token_string);
            std::string token;
            while (stream >> token)
            {
                out_options.push_back(token);
            }
        }

        std::string NormalizeHlslForPragmaScan(std::string text)
        {
            text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
            return text;
        }

    }  // namespace

    std::string StringifyVariantKey(const ShaderVariantKey& variant_key)
    {
        std::string out;
        out.reserve(variant_key.size() * 16);
        for (const auto& [k, v] : variant_key)
        {
            out += k;
            out += '=';
            out += v;
            out += ';';
        }
        return out;
    }

    void ExtractMultiCompileLines(const std::string& hlsl_code, std::vector<MultiCompileLine>& out_lines)
    {
        out_lines.clear();

        const std::string normalized = NormalizeHlslForPragmaScan(hlsl_code);

        // Scan line-by-line: without per-line matching, `$` only anchors to the end of
        // the whole HLSLPROGRAM block and pragma lines are missed (0 variants).
        const std::regex multi_compile_regex(R"re(#\s*pragma\s+multi_compile\s+(.+))re", std::regex::icase);
        const std::regex shader_feature_regex(R"re(#\s*pragma\s+shader_feature\s+(.+))re", std::regex::icase);

        std::istringstream stream(normalized);
        std::string line;
        while (std::getline(stream, line))
        {
            std::smatch match;
            if (std::regex_search(line, match, multi_compile_regex))
            {
                MultiCompileLine mc_line;
                mc_line.is_shader_feature = false;
                AppendTokens(match[1].str(), mc_line.options);
                if (!mc_line.options.empty())
                {
                    out_lines.push_back(std::move(mc_line));
                }
                continue;
            }

            if (std::regex_search(line, match, shader_feature_regex))
            {
                MultiCompileLine mc_line;
                mc_line.is_shader_feature = true;
                AppendTokens(match[1].str(), mc_line.options);
                if (!mc_line.options.empty())
                {
                    out_lines.push_back(std::move(mc_line));
                }
            }
        }
    }

    std::vector<ShaderVariantKey> GenerateVariantCombinations(const std::vector<MultiCompileLine>& lines,
                                                              size_t max_variants,
                                                              bool* out_truncated)
    {
        if (out_truncated)
        {
            *out_truncated = false;
        }

        std::vector<ShaderVariantKey> result {ShaderVariantKey {}};
        if (lines.empty())
        {
            return result;
        }

        for (const MultiCompileLine& line : lines)
        {
            if (line.options.empty())
            {
                continue;
            }

            std::vector<ShaderVariantKey> next;
            next.reserve(result.size() * line.options.size());

            for (const ShaderVariantKey& base : result)
            {
                for (const std::string& option : line.options)
                {
                    ShaderVariantKey variant = base;
                    if (option != "_")
                    {
                        variant[option] = "1";
                    }
                    next.push_back(std::move(variant));
                }
            }

            result = std::move(next);
            if (result.size() > max_variants)
            {
                result.resize(max_variants);
                if (out_truncated)
                {
                    *out_truncated = true;
                }
                return result;
            }
        }

        return result;
    }

    ShaderVariantKey ShaderVariantKeyFromEnabledKeywords(const std::vector<std::string>& enabled_keywords)
    {
        ShaderVariantKey key;
        for (const std::string& keyword : enabled_keywords)
        {
            if (!keyword.empty() && keyword != "_")
            {
                key[keyword] = "1";
            }
        }
        return key;
    }

    std::vector<ShaderVariantKey>
    GenerateVariantCombinationsBuildStrip(const std::vector<MultiCompileLine>& lines,
                                          const std::vector<ShaderVariantKey>& material_used_variant_keys,
                                          size_t max_variants,
                                          bool* out_truncated)
    {
        if (out_truncated)
        {
            *out_truncated = false;
        }

        std::vector<MultiCompileLine> multi_compile_lines;
        multi_compile_lines.reserve(lines.size());
        for (const MultiCompileLine& line : lines)
        {
            if (!line.is_shader_feature)
            {
                multi_compile_lines.push_back(line);
            }
        }

        bool mc_truncated = false;
        const std::vector<ShaderVariantKey> multi_compile_variants =
            GenerateVariantCombinations(multi_compile_lines, max_variants, &mc_truncated);

        std::map<ShaderVariantKey, bool> unique_keys;
        for (const ShaderVariantKey& variant : multi_compile_variants)
        {
            unique_keys.emplace(variant, true);
        }
        for (const ShaderVariantKey& material_variant : material_used_variant_keys)
        {
            unique_keys.emplace(material_variant, true);
        }

        if (unique_keys.size() > max_variants)
        {
            if (out_truncated)
            {
                *out_truncated = true;
            }
        }

        std::vector<ShaderVariantKey> result;
        result.reserve(unique_keys.size());
        for (const auto& entry : unique_keys)
        {
            result.push_back(entry.first);
            if (result.size() >= max_variants)
            {
                break;
            }
        }

        if (mc_truncated && out_truncated)
        {
            *out_truncated = true;
        }

        return result;
    }

    std::map<std::string, std::string> VariantKeyToMacros(const ShaderVariantKey& variant_key)
    {
        std::map<std::string, std::string> macros;
        for (const auto& [keyword, value] : variant_key)
        {
            if (!keyword.empty() && value == "1")
            {
                macros[keyword] = "1";
            }
        }
        return macros;
    }

    void CollectShaderKeywordOptions(const std::string& program_or_shader_source,
                                     std::vector<std::string>& out_keywords)
    {
        out_keywords.clear();

        std::vector<MultiCompileLine> lines;
        ExtractMultiCompileLines(program_or_shader_source, lines);

        std::vector<std::string> options;
        for (const MultiCompileLine& line : lines)
        {
            for (const std::string& option : line.options)
            {
                if (option.empty() || option == "_")
                {
                    continue;
                }
                options.push_back(option);
            }
        }

        std::sort(options.begin(), options.end());
        options.erase(std::unique(options.begin(), options.end()), options.end());
        out_keywords = std::move(options);
    }

}  // namespace ZEngine::ShaderLab
