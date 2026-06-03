// -----------------------------------------------------------------------------
// WebGL2ShaderCompiler - skeleton.
//
// Real implementation pipeline:
//   HLSL --(DXC)--> SPIR-V --(SPIRV-Cross GLSL ES 3.00)--> #version 300 es
//
// Hooks for DXC and SPIRV-Cross are intentionally left as TODOs so this file
// compiles standalone in an Emscripten + WebGL2 build that links them in.
// Until those are wired, the stub returns the input HLSL verbatim with a
// boilerplate header so simple shaders that only use intrinsics common to
// both languages will still link in WebGL.
// -----------------------------------------------------------------------------

#if defined(__EMSCRIPTEN__)

    #include "Runtime/Function/Render/Interface/WebGL2/WebGL2ShaderCompiler.h"

    #include <fstream>
    #include <sstream>

namespace ZEngine
{
    namespace WebGL2
    {

        WebGL2ShaderCompiler::WebGL2ShaderCompiler() = default;
        WebGL2ShaderCompiler::~WebGL2ShaderCompiler() = default;

        WebGL2ShaderCompileResult WebGL2ShaderCompiler::CompileFromFile(
            const std::string& file_path,
            ShaderStage shader_stage,
            const std::vector<std::string>& include_paths,
            const std::map<std::string, std::string>& macros,
            const std::string& entry_point)
        {
            std::ifstream ifs(file_path, std::ios::binary);
            if (!ifs.is_open())
            {
                return {false, shader_stage, {}, "Failed to open shader file: " + file_path};
            }
            std::ostringstream oss;
            oss << ifs.rdbuf();
            return CompileFromSource(oss.str(), shader_stage, file_path, include_paths, macros, entry_point);
        }

        WebGL2ShaderCompileResult WebGL2ShaderCompiler::CompileFromSource(
            const std::string& hlsl_source,
            ShaderStage shader_stage,
            const std::string& shader_name,
            const std::vector<std::string>& include_paths,
            const std::map<std::string, std::string>& macros,
            const std::string& entry_point)
        {
            WebGL2ShaderCompileResult result;
            result.shader_stage = shader_stage;

            std::vector<uint32_t> spirv;
            std::string err;
            if (!HlslToSpirv(hlsl_source, shader_stage, shader_name, include_paths, macros, entry_point, spirv, err))
            {
                // Until DXC is linked in, fall back to passing the HLSL through with a
                // GLSL ES 3.00 header so a cross-compatible shader can still run.
                result.glsl_source =
                    "#version 300 es\n"
                    "precision highp float;\n"
                    "// [WebGL2ShaderCompiler] DXC unavailable; passing HLSL through.\n" +
                    hlsl_source;
                result.success = !result.glsl_source.empty();
                if (!result.success)
                {
                    result.error_message = err;
                }
                return result;
            }

            if (!SpirvToGlsl(spirv, shader_stage, result.glsl_source, err))
            {
                result.success = false;
                result.error_message = err;
                return result;
            }

            result.success = true;
            return result;
        }

        bool WebGL2ShaderCompiler::HlslToSpirv(const std::string& /*hlsl_source*/, ShaderStage /*shader_stage*/, const std::string& /*shader_name*/, const std::vector<std::string>& /*include_paths*/, const std::map<std::string, std::string>& /*macros*/, const std::string& /*entry_point*/, std::vector<uint32_t>& /*out_spirv*/, std::string& out_error)
        {
            // TODO: wire DXC (DxcCreateInstance + IDxcCompiler3->Compile) with -spirv flag.
            out_error = "DXC HLSL->SPIRV compilation is not wired up yet.";
            return false;
        }

        bool WebGL2ShaderCompiler::SpirvToGlsl(const std::vector<uint32_t>& /*spirv*/, ShaderStage /*shader_stage*/, std::string& /*out_glsl*/, std::string& out_error)
        {
            // TODO: wire SPIRV-Cross CompilerGLSL { options.version = 300; options.es = true; }.
            out_error = "SPIRV-Cross SPIR-V->GLSL ES translation is not wired up yet.";
            return false;
        }

    }  // namespace WebGL2
}  // namespace ZEngine

#endif  // __EMSCRIPTEN__
