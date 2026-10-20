#pragma once

#include "Runtime/Function/Render/RenderType.h"

#include <cstddef>
#include <string>

namespace ZEngine::ShaderLab
{

    /// Result of pulling a single-stage HLSL program out of a ShaderLab `.shader` file.
    struct ShaderLabHlslExtractResult
    {
        bool ok = false;
        std::string hlsl_source;
        std::string entry_point;
        std::string error_message;
    };

    /// Sentinel for "do not care which SubShader / Pass -- take the first one
    /// that yields a program". This is the historical behaviour and stays the
    /// default, so every existing call site is unaffected.
    inline constexpr std::size_t kExtractAnySubShader = static_cast<std::size_t>(-1);
    inline constexpr std::size_t kExtractAnyPass = static_cast<std::size_t>(-1);

    /// Extract compilable HLSL for `stage` from a `.shader` source file.
    ///
    /// Tries ShaderLabParser first (first SubShader / Pass / Program with code),
    /// then falls back to a text scan for HLSLPROGRAM/CGPROGRAM blocks (same
    /// strategy as ShaderPreviewRenderer). Strips `#pragma` lines from the body
    /// before returning; entry points come from `#pragma vertex` / `#pragma fragment`
    /// when present.
    ///
    /// `fallback_entry` is used only when no stage pragma is found (typically "main").
    ///
    /// `subshader_index` / `pass_index` select a specific Pass instead of the
    /// first one found. Pass both as `kExtractAnySubShader` / `kExtractAnyPass`
    /// (the default) to get the old scan. Indices are bounds-checked: an
    /// out-of-range or program-less Pass is an error, NOT a silent fallback to
    /// Pass 0 -- the caller asked for that Pass and should hear about it.
    ShaderLabHlslExtractResult ExtractHlslFromShaderLabFile(const std::string& file_path,
                                                            ShaderStage stage,
                                                            const std::string& fallback_entry = "main",
                                                            std::size_t subshader_index = kExtractAnySubShader,
                                                            std::size_t pass_index = kExtractAnyPass);

    /// Same as ExtractHlslFromShaderLabFile but from an already-loaded source buffer.
    ShaderLabHlslExtractResult ExtractHlslFromShaderLabSource(const std::string& shader_source,
                                                              const std::string& file_path_for_errors,
                                                              ShaderStage stage,
                                                              const std::string& fallback_entry = "main",
                                                              std::size_t subshader_index = kExtractAnySubShader,
                                                              std::size_t pass_index = kExtractAnyPass);

}  // namespace ZEngine::ShaderLab
