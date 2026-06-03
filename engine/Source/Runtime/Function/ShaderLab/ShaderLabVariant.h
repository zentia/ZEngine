#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{

    using ShaderVariantKey = std::map<std::string, std::string>;

    /// One `#pragma multi_compile` or `#pragma shader_feature` line.
    struct MultiCompileLine
    {
        std::vector<std::string> options;
        bool is_shader_feature = false;
    };

    /// Canonical `k=v;` string for cache keys (sorted keys).
    std::string StringifyVariantKey(const ShaderVariantKey& variant_key);

    /// Parse pragma lines from HLSL/ShaderLab program text.
    void ExtractMultiCompileLines(const std::string& hlsl_code, std::vector<MultiCompileLine>& out_lines);

    /// Cartesian product across pragma lines. `_` means "macro disabled" (no define).
    /// Stops at `max_variants` and sets `out_truncated` when the full space is larger.
    std::vector<ShaderVariantKey> GenerateVariantCombinations(const std::vector<MultiCompileLine>& lines,
                                                              size_t max_variants,
                                                              bool* out_truncated);

    /// Build material variant key from enabled keyword names (Inspector / MaterialRes).
    ShaderVariantKey ShaderVariantKeyFromEnabledKeywords(const std::vector<std::string>& enabled_keywords);

    /// Unity-style build strip: full Cartesian product for `#pragma multi_compile` only,
    /// plus each variant key used by a project material for this shader. Unused
    /// `#pragma shader_feature` combinations are omitted.
    std::vector<ShaderVariantKey>
    GenerateVariantCombinationsBuildStrip(const std::vector<MultiCompileLine>& lines,
                                          const std::vector<ShaderVariantKey>& material_used_variant_keys,
                                          size_t max_variants,
                                          bool* out_truncated);

    /// Build DXC/glslang macro map: only keywords with value `"1"` become defines.
    std::map<std::string, std::string> VariantKeyToMacros(const ShaderVariantKey& variant_key);

    /// Collect unique keyword tokens from pragma lines (`_` excluded). Sorted output.
    void CollectShaderKeywordOptions(const std::string& program_or_shader_source,
                                     std::vector<std::string>& out_keywords);

}  // namespace ZEngine::ShaderLab
