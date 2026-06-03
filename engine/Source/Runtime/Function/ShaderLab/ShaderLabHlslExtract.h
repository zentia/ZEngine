#pragma once

#include "Runtime/Function/Render/RenderType.h"

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

    /// Extract compilable HLSL for `stage` from a `.shader` source file.
    ///
    /// Tries ShaderLabParser first (first SubShader / Pass / Program with code),
    /// then falls back to a text scan for HLSLPROGRAM/CGPROGRAM blocks (same
    /// strategy as ShaderPreviewRenderer). Strips `#pragma` lines from the body
    /// before returning; entry points come from `#pragma vertex` / `#pragma fragment`
    /// when present.
    ///
    /// `fallback_entry` is used only when no stage pragma is found (typically "main").
    ShaderLabHlslExtractResult ExtractHlslFromShaderLabFile(const std::string& file_path,
                                                            ShaderStage stage,
                                                            const std::string& fallback_entry = "main");

    /// Same as ExtractHlslFromShaderLabFile but from an already-loaded source buffer.
    ShaderLabHlslExtractResult ExtractHlslFromShaderLabSource(const std::string& shader_source,
                                                              const std::string& file_path_for_errors,
                                                              ShaderStage stage,
                                                              const std::string& fallback_entry = "main");

}  // namespace ZEngine::ShaderLab
