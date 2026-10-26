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

    /// True when `file_path` points at a ShaderLab `.shader` source (case-insensitive).
    ///
    /// 每个后端在"把源码交给编译器之前"都要先问一次：`.shader` 不是能直接编译的
    /// GLSL / HLSL，外面还包着 Shader / Properties / SubShader / Pass。以前只有
    /// DX12 侧问了（DX12ShaderCompiler 的局部 IsShaderLabSourcePath），Vulkan 侧
    /// 没问，于是整份 .shader 被直接喂给 glslang —— 必然失败。挪到这里共用，
    /// 免得下次再加后端又漏一个。
    bool IsShaderLabSourcePath(const std::string& file_path);

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

    /// Result of pulling a single-stage GLSL program out of a ShaderLab `.shader` file.
    ///
    /// Vulkan / Metal 侧的对口版本：这两个后端没有 HLSL 前端（树内也没有
    /// HLSL→SPIR-V 的交叉编译器），只有 `GLSLPROGRAM` 块能喂给 glslang。
    struct ShaderLabGlslExtractResult
    {
        bool ok = false;
        std::string glsl_source;
        std::string entry_point;
        std::string error_message;
    };

    /// Extract compilable GLSL for `stage` from a `.shader` source file.
    ///
    /// 与 HLSL 版共用同一套提取逻辑，只改三处：**只认** `GLSLPROGRAM` 块、**不做**
    /// Cg 输出语义改写（GLSL 没有语义这个概念）、纯文本兜底只扫
    /// `GLSLPROGRAM ... ENDGLSL`。参数含义与 HLSL 版一致。
    ShaderLabGlslExtractResult ExtractGlslFromShaderLabFile(const std::string& file_path,
                                                            ShaderStage stage,
                                                            const std::string& fallback_entry = "main",
                                                            std::size_t subshader_index = kExtractAnySubShader,
                                                            std::size_t pass_index = kExtractAnyPass);

    /// Same as ExtractGlslFromShaderLabFile but from an already-loaded source buffer.
    ShaderLabGlslExtractResult ExtractGlslFromShaderLabSource(const std::string& shader_source,
                                                              const std::string& file_path_for_errors,
                                                              ShaderStage stage,
                                                              const std::string& fallback_entry = "main",
                                                              std::size_t subshader_index = kExtractAnySubShader,
                                                              std::size_t pass_index = kExtractAnyPass);

}  // namespace ZEngine::ShaderLab
