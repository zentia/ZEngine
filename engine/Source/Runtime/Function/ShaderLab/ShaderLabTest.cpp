#include "Runtime/Function/ShaderLab/ShaderLabCompiler.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace ZEngine::ShaderLab;

void PrintShaderLabAsset(const ShaderLabAsset& asset)
{
    std::cout << "=== ShaderLab Asset ===" << std::endl;
    std::cout << "Name: " << asset.shader_name << std::endl;

    if (!asset.fallback.empty())
    {
        std::cout << "Fallback: " << asset.fallback << std::endl;
    }

    if (!asset.custom_editor.empty())
    {
        std::cout << "CustomEditor: " << asset.custom_editor << std::endl;
    }

    std::cout << std::endl;

    // Properties
    std::cout << "=== Properties (" << asset.properties.size() << ") ===" << std::endl;
    for (const auto& prop : asset.properties)
    {
        std::cout << "  " << prop.name << " (\"" << prop.display_name << "\", "
                  << PropertyTypeToString(prop.type) << ")";

        switch (prop.type)
        {
            case PropertyType::Float:
            case PropertyType::Int:
                std::cout << " = " << prop.default_value.float_value;
                break;
            case PropertyType::Range:
                std::cout << " = " << prop.default_value.float_value
                          << " [" << prop.default_value.range_min
                          << ", " << prop.default_value.range_max << "]";
                break;
            case PropertyType::Color:
                std::cout << " = (" << prop.default_value.color[0] << ", "
                          << prop.default_value.color[1] << ", "
                          << prop.default_value.color[2] << ", "
                          << prop.default_value.color[3] << ")";
                break;
            case PropertyType::Vector:
                std::cout << " = (" << prop.default_value.vector[0] << ", "
                          << prop.default_value.vector[1] << ", "
                          << prop.default_value.vector[2] << ", "
                          << prop.default_value.vector[3] << ")";
                break;
            case PropertyType::Texture2D:
            case PropertyType::Cube:
            case PropertyType::Texture3D:
                std::cout << " = \"" << prop.default_value.texture_default << "\"";
                break;
            default:
                break;
        }
        std::cout << std::endl;
    }

    std::cout << std::endl;

    // SubShaders
    std::cout << "=== SubShaders (" << asset.subshaders.size() << ") ===" << std::endl;
    for (size_t i = 0; i < asset.subshaders.size(); ++i)
    {
        const auto& subshader = asset.subshaders[i];
        std::cout << "SubShader[" << i << "] LOD=" << subshader.lod << std::endl;

        if (!subshader.tags.empty())
        {
            std::cout << "  Tags: ";
            for (const auto& [k, v] : subshader.tags)
            {
                std::cout << k << "=\"" << v << "\" ";
            }
            std::cout << std::endl;
        }

        // Passes
        std::cout << "  Passes (" << subshader.passes.size() << "):" << std::endl;
        for (size_t j = 0; j < subshader.passes.size(); ++j)
        {
            const auto& pass = subshader.passes[j];
            std::cout << "    Pass[" << j << "] \"" << pass.name << "\"";
            if (!pass.light_mode.empty())
            {
                std::cout << " [LightMode=" << pass.light_mode << "]";
            }
            std::cout << std::endl;

            // Render States
            std::cout << "      Render States:" << std::endl;
            std::cout << "        Blend: " << (pass.render_states.blend_enable ? "On" : "Off") << std::endl;
            std::cout << "        ZWrite: " << (pass.render_states.zwrite ? "On" : "Off") << std::endl;
            std::cout << "        ZTest: " << static_cast<int>(pass.render_states.ztest) << std::endl;
            std::cout << "        Cull: " << static_cast<int>(pass.render_states.cull) << std::endl;

            // Programs
            std::cout << "      Programs (" << pass.programs.size() << "):" << std::endl;
            for (size_t k = 0; k < pass.programs.size(); ++k)
            {
                const auto& program = pass.programs[k];
                std::cout << "        Program[" << k << "] Language="
                          << (program.language == ShaderLanguage::HLSL ? "HLSL" : program.language == ShaderLanguage::GLSL ? "GLSL"
                                                                                                                           : "CG")
                          << std::endl;
                std::cout << "          Vertex Entry: " << program.vertex_entry << std::endl;
                std::cout << "          Fragment Entry: " << program.fragment_entry << std::endl;
                std::cout << "          Code Length: " << program.source_code.length() << " chars" << std::endl;

                // 显示代码前100个字符
                if (!program.source_code.empty())
                {
                    std::string preview = program.source_code.substr(0, 100);
                    std::cout << "          Preview: " << preview << "..." << std::endl;
                }
            }
        }
        std::cout << std::endl;
    }
}

int main(int argc, char** argv)
{
    std::string file_path = "shader/example_standard.shader";

    if (argc > 1)
    {
        file_path = argv[1];
    }

    std::cout << "Loading ShaderLab file: " << file_path << std::endl;

    std::ifstream file(file_path);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open file: " << file_path << std::endl;
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();
    file.close();

    std::cout << "Source length: " << source.length() << " chars" << std::endl;
    std::cout << std::endl;

    // Test parser
    std::cout << "=== Testing Parser ===" << std::endl;
    ShaderLabParser parser(source);

    if (!parser.Parse())
    {
        std::cerr << "Parse error: " << parser.GetError() << std::endl;
        return 1;
    }

    if (parser.HasError())
    {
        std::cerr << "Parse warnings/errors: " << parser.GetError() << std::endl;
    }

    for (const auto& warning : parser.GetWarnings())
    {
        std::cout << "Warning: " << warning << std::endl;
    }

    auto asset = parser.GetAsset();
    PrintShaderLabAsset(*asset);

    // Test compiler
    std::cout << std::endl;
    std::cout << "=== Testing Compiler ===" << std::endl;
    ShaderLabCompiler compiler;

    if (!compiler.LoadFromString(source))
    {
        std::cerr << "Compiler error: " << compiler.GetParseError() << std::endl;
        return 1;
    }

    // Try to compile
    auto result = compiler.Compile(0, 0, "fragment", {});

    if (result.success)
    {
        std::cout << "Compilation successful! SPIR-V size: " << result.spirv_code.size() << " bytes" << std::endl;
    }
    else
    {
        std::cout << "Compilation result: " << result.error_message << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Test completed!" << std::endl;

    return 0;
}
