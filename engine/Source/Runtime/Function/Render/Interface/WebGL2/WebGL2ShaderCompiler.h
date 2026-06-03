#pragma once

// The HLSL → GLSL ES transpiler bound to this header relies on DXC and
// SPIRV-Cross hooks plumbed through the Emscripten build only.
#if defined(__EMSCRIPTEN__)

// -----------------------------------------------------------------------------
// HLSL → GLSL ES 3.00 transpilation pipeline for the WebGL 2.0 backend.
//
// Pipeline:
//     HLSL  --(DXC)-->  SPIR-V  --(SPIRV-Cross)-->  GLSL ES 3.00 (#version 300 es)
//
// In a mini-game Build (Emscripten target), DXC and SPIRV-Cross are linked
// into ZRuntimeShared so the engine can compile shaders at runtime when the
// asset bundle ships pre-baked .shader source. For a fully precooked build
// you can skip this class entirely and feed pre-translated GLSL strings into
// WebGL2RHI::CreateShaderModule().
// -----------------------------------------------------------------------------

    #include "Runtime/Function/Render/RenderType.h"

    #include <cstdint>
    #include <map>
    #include <memory>
    #include <string>
    #include <vector>

namespace ZEngine
{
    namespace WebGL2
    {

        struct WebGL2ShaderCompileResult
        {
            bool success {false};
            ShaderStage shader_stage {ShaderStage::Vertex};
            std::string glsl_source;  // GLSL ES 3.00 ready to be fed into glShaderSource
            std::string error_message;
        };

        class WebGL2ShaderCompiler
        {
        public:
            WebGL2ShaderCompiler();
            ~WebGL2ShaderCompiler();

            WebGL2ShaderCompileResult CompileFromFile(const std::string& file_path,
                                                      ShaderStage shader_stage,
                                                      const std::vector<std::string>& include_paths = {},
                                                      const std::map<std::string, std::string>& macros = {},
                                                      const std::string& entry_point = "main");

            WebGL2ShaderCompileResult CompileFromSource(const std::string& hlsl_source,
                                                        ShaderStage shader_stage,
                                                        const std::string& shader_name = "",
                                                        const std::vector<std::string>& include_paths = {},
                                                        const std::map<std::string, std::string>& macros = {},
                                                        const std::string& entry_point = "main");

        private:
            // HLSL → SPIR-V via DXC. Output binary words.
            bool HlslToSpirv(const std::string& hlsl_source,
                             ShaderStage shader_stage,
                             const std::string& shader_name,
                             const std::vector<std::string>& include_paths,
                             const std::map<std::string, std::string>& macros,
                             const std::string& entry_point,
                             std::vector<uint32_t>& out_spirv,
                             std::string& out_error);

            // SPIR-V → GLSL ES 3.00 via SPIRV-Cross. Sets correct version, ESSL flag,
            // and remaps cbuffer / texture bindings to UBOs / sampler uniforms.
            bool SpirvToGlsl(const std::vector<uint32_t>& spirv,
                             ShaderStage shader_stage,
                             std::string& out_glsl,
                             std::string& out_error);
        };

    }  // namespace WebGL2
}  // namespace ZEngine

#endif  // __EMSCRIPTEN__
