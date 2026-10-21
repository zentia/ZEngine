#include "Runtime/Function/ShaderLab/ShaderLabParser.h"

#include "Runtime/Function/ShaderLab/ShaderLabSurface.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ZEngine::ShaderLab
{

    namespace
    {
        // Unity 固定管线遗留命令。ZEngine 只走可编程管线，这些命令没有对应实现，
        // 但它们在真实 shader 里到处都是（Spine 的 SubShader 就有 Fog / Lighting）。
        // 识别出来安静跳过，避免刷一堆 "unexpected token" 警告。
        bool IsIgnoredLegacyCommand(const std::string& id)
        {
            static const char* const kLegacy[] = {
                "fog",              // Fog { Mode Off }
                "lighting",         // Lighting On / Off
                "material",         // Material { Diffuse (1,1,1,1) ... }
                "colormaterial",    // ColorMaterial AmbientAndDiffuse
                "separatespecular", // SeparateSpecular On / Off
                "alphatest",        // AlphaTest Greater 0.5
                "settexture",       // SetTexture [_MainTex] { combine texture }
                "bindchannels",     // BindChannels { Bind "Vertex", vertex }
                "color",            // Color (1,1,1,1)
                "zclip",            // ZClip True / False
                "conservative",     // Conservative True / False
                "alphatomask",      // AlphaToMask On / Off
            };

            for (const char* candidate : kLegacy)
            {
                if (id == candidate)
                {
                    return true;
                }
            }
            return false;
        }
        // Surface shader 选项携带的混合因子 -> ShaderLab 的 BlendFunc 枚举。
        // ShaderLabSurface.h 刻意不依赖 ShaderLabAsset.h（它只 include
        // <string>/<vector>，这样才能当纯 C++ 单测），所以这一层转换落在parser。
        BlendFunc SurfaceBlendFactorToBlendFunc(SurfaceBlendFactor factor)
        {
            switch (factor)
            {
                case SurfaceBlendFactor::Zero:             return BlendFunc::Zero;
                case SurfaceBlendFactor::SrcAlpha:         return BlendFunc::SrcAlpha;
                case SurfaceBlendFactor::OneMinusSrcAlpha: return BlendFunc::OneMinusSrcAlpha;
                case SurfaceBlendFactor::One:
                default:                                   return BlendFunc::One;
            }
        }

        // 从 CGPROGRAM/HLSLPROGRAM 文本里抓 `#pragma vertex|fragment|pixel|compute <name>`。
        //
        // ShaderProgram::vertex_entry / fragment_entry 的初值 "vert" / "frag" 只是
        // 默认值：ParsePragma() 在头文件里只有声明、没有实现，也从来没有调用者。
        // 不解析的话 Compile() 永远拿默认名去找入口，凡是入口不叫 vert/frag 的
        // shader 都会报 "missing entry point definition"。
        void ExtractEntryPoints(const std::string& program_source,
                                std::string& vertex_entry,
                                std::string& fragment_entry,
                                std::string& compute_entry)
        {
            std::istringstream stream(program_source);
            std::string line;
            while (std::getline(stream, line))
            {
                const size_t hash = line.find('#');
                if (hash == std::string::npos)
                {
                    continue;
                }

                std::istringstream line_stream(line.substr(hash));
                std::string pragma;
                std::string stage;
                std::string name;
                line_stream >> pragma >> stage >> name;
                if (pragma != "#pragma" || name.empty())
                {
                    continue;
                }

                std::transform(stage.begin(), stage.end(), stage.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });

                if (stage == "vertex")
                {
                    vertex_entry = name;
                }
                else if (stage == "fragment" || stage == "pixel")
                {
                    fragment_entry = name;
                }
                else if (stage == "compute" || stage == "kernel")
                {
                    compute_entry = name;
                }
            }
        }
    }  // namespace

    ShaderLabParser::ShaderLabParser(const std::string& source)
        : m_Lexer(source), m_Asset(std::make_shared<ShaderLabAsset>())
    {
    }

    void ShaderLabParser::SetSource(const std::string& source)
    {
        m_Lexer.SetSource(source);
        m_Asset = std::make_shared<ShaderLabAsset>();
        m_Error.clear();
        m_Warnings.clear();
    }

    bool ShaderLabParser::Parse()
    {
        m_Lexer.Reset();
        return ParseShader();
    }

    bool ShaderLabParser::ParseShader()
    {
        if (!Expect(TokenType::Shader))
        {
            Error("Expected 'Shader' keyword");
            return false;
        }

        Token name_token = m_Lexer.NextToken();
        if (name_token.type != TokenType::StringLiteral)
        {
            Error("Expected shader name string, got: " + name_token.text);
            return false;
        }
        m_Asset->shader_name = name_token.text;

        if (!Expect(TokenType::LeftBrace))
        {
            Error("Expected '{' after shader name");
            return false;
        }

        while (!m_Lexer.IsEnd())
        {
            Token token = m_Lexer.NextToken();

            if (token.type == TokenType::RightBrace)
            {
                break;
            }

            if (token.type == TokenType::EndOfFile)
            {
                Error("Unexpected end of file, expected '}'");
                return false;
            }

            std::string id = token.text;
            std::transform(id.begin(), id.end(), id.begin(), ::tolower);

            if (token.type == TokenType::Properties || id == "properties")
            {
                if (!ParseProperties())
                {
                    return false;
                }
            }
            else if (token.type == TokenType::SubShader || id == "subshader")
            {
                ShaderSubShader subshader;
                if (!ParseSubShader(subshader))
                {
                    return false;
                }
                m_Asset->subshaders.push_back(std::move(subshader));
            }
            else if (token.type == TokenType::FallBack || id == "fallback")
            {
                Token fallback_token = m_Lexer.NextToken();
                if (fallback_token.type != TokenType::StringLiteral)
                {
                    Error("Expected fallback shader name, got: " + fallback_token.text);
                    return false;
                }
                m_Asset->fallback = fallback_token.text;
            }
            else if (token.type == TokenType::CustomEditor || id == "customeditor")
            {
                Token custom_editor_token = m_Lexer.NextToken();
                if (custom_editor_token.type != TokenType::StringLiteral)
                {
                    Error("Expected custom editor name, got: " + custom_editor_token.text);
                    return false;
                }
                m_Asset->custom_editor = custom_editor_token.text;
            }
            else
            {
                Warning("Unexpected token in Shader block: " + token.text);
                SkipToNextStatement();
            }
        }

        return true;
    }

    bool ShaderLabParser::ParseProperties()
    {
        if (!Expect(TokenType::LeftBrace))
        {
            Error("Expected '{' after 'Properties'");
            return false;
        }

        while (true)
        {
            Token token = m_Lexer.NextToken();

            if (token.type == TokenType::RightBrace)
            {
                break;
            }

            if (token.type == TokenType::EndOfFile)
            {
                Error("Unexpected end of file in Properties block");
                return false;
            }

            while (token.type == TokenType::LeftBracket)
            {
                int bracket_depth = 1;
                while (bracket_depth > 0)
                {
                    Token attribute_token = m_Lexer.NextToken();
                    if (attribute_token.type == TokenType::EndOfFile)
                    {
                        Error("Unexpected end of file in property attribute");
                        return false;
                    }
                    if (attribute_token.type == TokenType::LeftBracket)
                    {
                        ++bracket_depth;
                    }
                    else if (attribute_token.type == TokenType::RightBracket)
                    {
                        --bracket_depth;
                    }
                }
                token = m_Lexer.NextToken();
            }

            if (token.type == TokenType::RightBrace)
            {
                break;
            }

            if (token.type != TokenType::Identifier)
            {
                Error("Expected property name, got: " + token.text);
                SkipToNextStatement();
                continue;
            }

            ShaderProperty property;
            property.name = token.text;

            if (!Expect(TokenType::LeftParen))
            {
                SkipToNextStatement();
                continue;
            }

            Token display_name_token = m_Lexer.NextToken();
            if (display_name_token.type != TokenType::StringLiteral)
            {
                Error("Expected property display name, got: " + display_name_token.text);
                SkipToNextStatement();
                continue;
            }
            property.display_name = display_name_token.text;

            if (!Expect(TokenType::Comma))
            {
                SkipToNextStatement();
                continue;
            }

            Token type_token = m_Lexer.NextToken();

            if (type_token.type == TokenType::Range)
            {
                property.type = PropertyType::Range;
                if (!Expect(TokenType::LeftParen))
                {
                    SkipToNextStatement();
                    continue;
                }

                Token min_token = m_Lexer.NextToken();
                if (min_token.type != TokenType::NumberLiteral)
                {
                    Error("Expected range min value, got: " + min_token.text);
                    SkipToNextStatement();
                    continue;
                }
                property.default_value.range_min = min_token.number_value;

                if (!Expect(TokenType::Comma))
                {
                    SkipToNextStatement();
                    continue;
                }

                Token max_token = m_Lexer.NextToken();
                if (max_token.type != TokenType::NumberLiteral)
                {
                    Error("Expected range max value, got: " + max_token.text);
                    SkipToNextStatement();
                    continue;
                }
                property.default_value.range_max = max_token.number_value;

                if (!Expect(TokenType::RightParen))
                {
                    SkipToNextStatement();
                    continue;
                }
            }
            else if (type_token.type == TokenType::Texture2D)
            {
                property.type = PropertyType::Texture2D;
            }
            else if (type_token.type == TokenType::Cube)
            {
                property.type = PropertyType::Cube;
            }
            else if (type_token.type == TokenType::Texture3D)
            {
                property.type = PropertyType::Texture3D;
            }
            else if (type_token.type == TokenType::Float)
            {
                property.type = PropertyType::Float;
            }
            else if (type_token.type == TokenType::Int)
            {
                property.type = PropertyType::Int;
            }
            else if (type_token.type == TokenType::Color)
            {
                property.type = PropertyType::Color;
            }
            else if (type_token.type == TokenType::Vector)
            {
                property.type = PropertyType::Vector;
            }
            else if (type_token.type == TokenType::FloatArray)
            {
                property.type = PropertyType::FloatArray;
            }
            else
            {
                Warning("Unknown property type: " + type_token.text);
                property.type = PropertyType::Float;
            }

            if (!Expect(TokenType::RightParen))
            {
                SkipToNextStatement();
                continue;
            }

            if (!Expect(TokenType::Equal))
            {
                SkipToNextStatement();
                continue;
            }

            if (!ParsePropertyDefault(property, property.type))
            {
                SkipToNextStatement();
                continue;
            }

            // Unity ShaderLab allows an optional trailing ';' after each property.
            if (Peek(TokenType::Semicolon))
            {
                m_Lexer.NextToken();
            }

            m_Asset->properties.push_back(std::move(property));
        }

        return true;
    }

    bool ShaderLabParser::ParsePropertyDefault(ShaderProperty& property, PropertyType type)
    {
        Token token = m_Lexer.NextToken();

        switch (type)
        {
            case PropertyType::Float:
            case PropertyType::Int:
            case PropertyType::Range:
                if (token.type == TokenType::NumberLiteral)
                {
                    property.default_value.float_value = token.number_value;
                    property.default_value.int_value = static_cast<int>(token.number_value);
                }
                else
                {
                    Warning("Expected number for float/int/range default value");
                }
                break;

            case PropertyType::Color:
                if (token.type == TokenType::LeftParen)
                {
                    float values[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    for (int i = 0; i < 4; ++i)
                    {
                        if (i > 0 && !Expect(TokenType::Comma))
                        {
                            break;
                        }

                        Token value_token = m_Lexer.NextToken();
                        if (value_token.type != TokenType::NumberLiteral)
                        {
                            Warning("Expected number in color default value, got: " + value_token.text);
                            break;
                        }
                        values[i] = value_token.number_value;
                    }
                    if (!Expect(TokenType::RightParen))
                    {
                        Warning("Expected ')' after color values");
                    }
                    property.default_value.color[0] = values[0];
                    property.default_value.color[1] = values[1];
                    property.default_value.color[2] = values[2];
                    property.default_value.color[3] = values[3];
                }
                else if (token.type == TokenType::Identifier)
                {
                    property.default_value.texture_default = token.text;
                }
                break;

            case PropertyType::Vector:
                if (token.type == TokenType::LeftParen)
                {
                    float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    for (int i = 0; i < 4; ++i)
                    {
                        if (i > 0 && !Expect(TokenType::Comma))
                        {
                            break;
                        }

                        Token value_token = m_Lexer.NextToken();
                        if (value_token.type != TokenType::NumberLiteral)
                        {
                            Warning("Expected number in vector default value, got: " + value_token.text);
                            break;
                        }
                        values[i] = value_token.number_value;
                    }
                    if (!Expect(TokenType::RightParen))
                    {
                        Warning("Expected ')' after vector values");
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        property.default_value.vector[i] = values[i];
                    }
                }
                break;

            case PropertyType::Texture2D:
            case PropertyType::Cube:
            case PropertyType::Texture3D:
                if (token.type == TokenType::StringLiteral)
                {
                    property.default_value.texture_default = token.text;

                    if (Peek(TokenType::LeftBrace))
                    {
                        m_Lexer.NextToken();
                        int brace_depth = 1;
                        while (brace_depth > 0)
                        {
                            Token brace_token = m_Lexer.NextToken();
                            if (brace_token.type == TokenType::EndOfFile)
                            {
                                Warning("Unexpected end of file in texture default block");
                                break;
                            }
                            if (brace_token.type == TokenType::LeftBrace)
                            {
                                ++brace_depth;
                            }
                            else if (brace_token.type == TokenType::RightBrace)
                            {
                                --brace_depth;
                            }
                        }
                    }
                }
                else if (token.type == TokenType::LeftBrace)
                {
                    if (!Expect(TokenType::RightBrace))
                    {
                        Warning("Unexpected content in texture default");
                    }
                }
                break;

            default:
                break;
        }

        return true;
    }

    bool ShaderLabParser::ParseSubShader(ShaderSubShader& subshader)
    {
        if (!Expect(TokenType::LeftBrace))
        {
            Error("Expected '{' after 'SubShader'");
            return false;
        }

        // SubShader 级渲染状态（Unity 允许把 Cull/ZWrite/Blend/... 写在 SubShader
        // 上，作为内部 Pass 的默认值）。不进序列化：解析时直接灌进每个 Pass。
        RenderStates subshader_states;

        while (true)
        {
            Token token = m_Lexer.NextToken();

            if (token.type == TokenType::RightBrace)
            {
                break;
            }

            if (token.type == TokenType::EndOfFile)
            {
                Error("Unexpected end of file in SubShader block");
                return false;
            }

            std::string id = token.text;
            std::transform(id.begin(), id.end(), id.begin(), ::tolower);

            if (token.type == TokenType::Tags || id == "tags")
            {
                if (!ParseTags(subshader.tags))
                {
                    return false;
                }
            }
            else if (token.type == TokenType::LOD || id == "lod")
            {
                Token lod_token = m_Lexer.NextToken();
                if (lod_token.type != TokenType::NumberLiteral)
                {
                    Error("Expected number after LOD, got: " + lod_token.text);
                    return false;
                }
                subshader.lod = static_cast<int>(lod_token.number_value);
            }
            else if (token.type == TokenType::Pass || id == "pass")
            {
                ShaderPass pass;
                // Unity 语义：SubShader 级渲染状态是该 SubShader 内所有 Pass 的
                // 默认值，Pass 内的同名命令覆盖它。
                pass.render_states = subshader_states;
                if (!ParsePass(pass))
                {
                    return false;
                }
                subshader.passes.push_back(std::move(pass));
            }
            else if (token.type == TokenType::HLSLProgram || token.type == TokenType::GLSLProgram ||
                     token.type == TokenType::CGProgram)
            {
                // 直接挂在 SubShader 下的程序块 = surface shader。放在
                // TryParseRenderStateCommand 之前没用（它只认状态命令），
                // 但必须放在最后那个 "unexpected token" 之前：那条分支会
                // SkipToNextStatement()，把整段 shader 代码吞掉。
                if (!ParseSubShaderProgram(subshader, token, subshader_states))
                {
                    return false;
                }
            }
            else if (TryParseRenderStateCommand(token, subshader_states))
            {
                // SubShader 级渲染状态，已收集到 subshader_states。
            }
            else if (IsIgnoredLegacyCommand(id))
            {
                // Unity 固定管线遗留命令，ZEngine 不实现：安静跳过整条。
                SkipToNextStatement();
            }
            else
            {
                Warning("Unexpected token in SubShader block: " + token.text);
                SkipToNextStatement();
            }
        }

        return true;
    }

    bool ShaderLabParser::ParseTags(std::map<std::string, std::string>& tags)
    {
        if (!Expect(TokenType::LeftBrace))
        {
            Error("Expected '{' after 'Tags'");
            return false;
        }

        while (true)
        {
            Token token = m_Lexer.NextToken();

            if (token.type == TokenType::RightBrace)
            {
                break;
            }

            if (token.type == TokenType::EndOfFile)
            {
                Error("Unexpected end of file in Tags block");
                return false;
            }

            if (token.type == TokenType::StringLiteral)
            {
                std::string key = token.text;

                if (!Expect(TokenType::Equal))
                {
                    SkipToNextStatement();
                    continue;
                }

                Token value_token = m_Lexer.NextToken();
                if (value_token.type != TokenType::StringLiteral)
                {
                    Error("Expected tag value string, got: " + value_token.text);
                    SkipToNextStatement();
                    continue;
                }

                tags[key] = value_token.text;
            }
            else if (token.type == TokenType::Comma)
            {
                continue;
            }
            else
            {
                Warning("Unexpected token in Tags block: " + token.text);
            }
        }

        return true;
    }

    bool ShaderLabParser::ParsePass(ShaderPass& pass)
    {
        if (Peek(TokenType::StringLiteral))
        {
            pass.name = m_Lexer.NextToken().text;
        }

        if (!Expect(TokenType::LeftBrace))
        {
            Error("Expected '{' after 'Pass'");
            return false;
        }

        while (true)
        {
            Token token = m_Lexer.NextToken();

            if (token.type == TokenType::RightBrace)
            {
                break;
            }

            if (token.type == TokenType::EndOfFile)
            {
                Error("Unexpected end of file in Pass block");
                return false;
            }

            std::string id = token.text;
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            if (token.type == TokenType::Tags || id == "tags")
            {
                if (!ParseTags(pass.tags))
                {
                    return false;
                }

                auto light_mode_it = pass.tags.find("LightMode");
                if (light_mode_it != pass.tags.end())
                {
                    pass.light_mode = light_mode_it->second;
                }
            }
            else if (id == "name")
            {
                Token name_token = m_Lexer.NextToken();
                if (name_token.type != TokenType::StringLiteral)
                {
                    Error("Expected pass name string, got: " + name_token.text);
                    return false;
                }
                pass.name = name_token.text;
            }
            else if (TryParseRenderStateCommand(token, pass.render_states))
            {
                // 渲染状态命令，已处理
            }
            else if (token.type == TokenType::HLSLProgram || token.type == TokenType::GLSLProgram || token.type == TokenType::CGProgram)
            {
                ShaderProgram program;
                program.language = token.type == TokenType::HLSLProgram ? ShaderLanguage::HLSL : token.type == TokenType::GLSLProgram ? ShaderLanguage::GLSL
                                                                                                                                      : ShaderLanguage::CG;
                program.source_code = token.text;
                // 入口名来自 #pragma vertex / #pragma fragment，不是 "vert"/"frag" 默认值。
                ExtractEntryPoints(program.source_code,
                                   program.vertex_entry,
                                   program.fragment_entry,
                                   program.compute_entry);
                pass.programs.push_back(std::move(program));
            }
            else if (IsIgnoredLegacyCommand(id))
            {
                // Unity 固定管线遗留命令：安静跳过整条。
                SkipToNextStatement();
            }
            else
            {
                Warning("Unexpected token in Pass block: " + token.text);
                // 必须把这条命令的剩余部分吃掉。否则 "Fog { Mode Off }" 里的
                // '}' 会被上面的循环当成 Pass 的收尾，后面的 CGPROGRAM 全部丢失。
                SkipToNextStatement();
            }
        }

        return true;
    }

    bool ShaderLabParser::ParseSubShaderProgram(ShaderSubShader& subshader,
                                               const Token& code_token,
                                               const RenderStates& inherited_states)
    {
        const ShaderLanguage language =
            code_token.type == TokenType::HLSLProgram  ? ShaderLanguage::HLSL
            : code_token.type == TokenType::GLSLProgram ? ShaderLanguage::GLSL
                                                        : ShaderLanguage::CG;

        const SurfaceShaderDirective directive = ParseSurfaceDirective(code_token.text);

        if (!directive.valid)
        {
            // Unity 只允许 surface shader 把程序块直接挂在 SubShader 下。没有
            // #pragma surface 就没有可展开的东西，也没有 Pass 可供归属 —— 明确
            // 说出来，而不是像以前那样被 "unexpected token" 分支静默吞掉。
            if (!directive.error.empty())
            {
                Error("Bad '#pragma surface': " + directive.error);
                return false;
            }

            Warning("CGPROGRAM block outside a Pass has no '#pragma surface'; nothing to "
                    "generate. Wrap it in a Pass, or declare it as a surface shader.");
            return true;  // 不致命：SubShader 里剩下的内容照常解析
        }

        const SurfaceGenerateResult generated = GenerateSurfacePasses(code_token.text, directive);
        if (!generated.ok)
        {
            Error("surface shader '" + directive.function + "': " + generated.error);
            return false;
        }

        for (const std::string& warning : generated.warnings)
        {
            Warning("surface shader '" + directive.function + "': " + warning);
        }

        for (const SurfacePass& surface_pass : generated.passes)
        {
            ShaderPass pass;
            pass.name = surface_pass.name;
            pass.light_mode = surface_pass.light_mode;
            pass.tags["LightMode"] = surface_pass.light_mode;
            // Surface shader 内部的 Pass 同样继承 SubShader 级渲染状态。
            pass.render_states = inherited_states;

            // `#pragma surface` 的选项本身携带渲染状态：`alpha` 打开透明混合，
            // ForwardAdd 附加混合且不写深度。这些是 pass 级的，覆盖继承来的值。
            const SurfaceRenderStateOverride& override_state = surface_pass.render_state;
            if (override_state.has_blend)
            {
                pass.render_states.blend_enable = true;
                pass.render_states.blend_src =
                    SurfaceBlendFactorToBlendFunc(override_state.blend_src);
                pass.render_states.blend_dst =
                    SurfaceBlendFactorToBlendFunc(override_state.blend_dst);
                pass.render_states.blend_src_alpha = pass.render_states.blend_src;
                pass.render_states.blend_dst_alpha = pass.render_states.blend_dst;
                pass.render_states.blend_op = BlendOp::Add;
            }
            if (override_state.has_zwrite)
            {
                pass.render_states.zwrite = override_state.zwrite;
            }

            ShaderProgram program;
            program.language = language;
            program.source_code = surface_pass.source_code;
            program.vertex_entry = surface_pass.vertex_entry;
            program.fragment_entry = surface_pass.fragment_entry;
            pass.programs.push_back(std::move(program));

            subshader.passes.push_back(std::move(pass));
        }

        return true;
    }

    bool ShaderLabParser::TryParseRenderStateCommand(const Token& token, RenderStates& states)
    {
        // SubShader 和 Pass 都接受同一套渲染状态命令。以前只写在 ParsePass 里，
        // SubShader 级的状态命令走到 "unexpected token" 分支，被 SkipToNextStatement()
        // 连带吞掉后面的整个 Pass。
        std::string id = token.text;
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        if (token.type == TokenType::Blend || id == "blend")
        {
            ParseBlendState(states);
            return true;
        }

        if (token.type == TokenType::ZWrite || id == "zwrite")
        {
            Token value = m_Lexer.NextToken();
            if (value.type == TokenType::On || value.text == "On")
            {
                states.zwrite = true;
            }
            else if (value.type == TokenType::Off || value.text == "Off")
            {
                states.zwrite = false;
            }
            return true;
        }

        if (token.type == TokenType::ZTest || id == "ztest")
        {
            Token value = m_Lexer.NextToken();
            states.ztest = CompareFunc::LEqual;
            if (value.type == TokenType::Less)
                states.ztest = CompareFunc::Less;
            else if (value.type == TokenType::Greater)
                states.ztest = CompareFunc::Greater;
            else if (value.type == TokenType::LEqual)
                states.ztest = CompareFunc::LEqual;
            else if (value.type == TokenType::GEqual)
                states.ztest = CompareFunc::GEqual;
            else if (value.type == TokenType::Eq)
                states.ztest = CompareFunc::Equal;
            else if (value.type == TokenType::NotEqual)
                states.ztest = CompareFunc::NotEqual;
            else if (value.type == TokenType::Always)
                states.ztest = CompareFunc::Always;
            return true;
        }

        if (token.type == TokenType::Cull || id == "cull")
        {
            Token value = m_Lexer.NextToken();
            if (value.type == TokenType::Off || value.text == "Off")
            {
                states.cull = CullMode::Off;
            }
            else if (value.type == TokenType::Front || value.text == "Front")
            {
                states.cull = CullMode::Front;
            }
            else if (value.type == TokenType::Back || value.text == "Back")
            {
                states.cull = CullMode::Back;
            }
            return true;
        }

        if (token.type == TokenType::ColorMask || id == "colormask")
        {
            Token value = m_Lexer.NextToken();
            states.color_mask = value.text == "0" ? 0 : 0xF;
            return true;
        }

        if (token.type == TokenType::Offset || id == "offset")
        {
            Token factor = m_Lexer.NextToken();
            // "Offset 1, 1" 词法上是 数字 / 逗号 / 数字。旧实现直接连读两个
            // token，units 拿到的是逗号（number_value = 0），斜率永远为 0。
            if (factor.type == TokenType::Comma)
            {
                factor = m_Lexer.NextToken();
            }

            Token units = m_Lexer.NextToken();
            if (units.type == TokenType::Comma)
            {
                units = m_Lexer.NextToken();
            }

            states.offset_factor = factor.number_value;
            states.offset_units = units.number_value;
            return true;
        }

        return false;
    }

    bool ShaderLabParser::ParseRenderState(ShaderPass& pass)
    {
        // 这个函数在解析 Pass 的 { } 块时被调用
        // 消费了开始的 {

        while (true)
        {
            Token token = m_Lexer.NextToken();

            if (token.type == TokenType::RightBrace)
            {
                return true;
            }

            if (token.type == TokenType::EndOfFile)
            {
                Error("Unexpected end of file in render state");
                return false;
            }

            if (token.type == TokenType::Identifier)
            {
                std::string id = token.text;
                std::transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });

                if (id == "tags")
                {
                    // Tags 不在渲染状态中，应该在外部处理
                    // 这里简单忽略
                }
                else if (id == "blend")
                {
                    ParseBlendState(pass.render_states);
                }
                else if (id == "zwrite")
                {
                    Token value = m_Lexer.NextToken();
                    if (value.type == TokenType::On || value.text == "On")
                        pass.render_states.zwrite = true;
                    else if (value.type == TokenType::Off || value.text == "Off")
                        pass.render_states.zwrite = false;
                }
                else if (id == "ztest")
                {
                    Token value = m_Lexer.NextToken();
                    pass.render_states.ztest = CompareFunc::LEqual;  // 默认
                    if (value.type == TokenType::Less)
                        pass.render_states.ztest = CompareFunc::Less;
                    else if (value.type == TokenType::Greater)
                        pass.render_states.ztest = CompareFunc::Greater;
                    else if (value.type == TokenType::LEqual)
                        pass.render_states.ztest = CompareFunc::LEqual;
                    else if (value.type == TokenType::GEqual)
                        pass.render_states.ztest = CompareFunc::GEqual;
                    else if (value.type == TokenType::Eq)
                        pass.render_states.ztest = CompareFunc::Equal;
                    else if (value.type == TokenType::NotEqual)
                        pass.render_states.ztest = CompareFunc::NotEqual;
                    else if (value.type == TokenType::Always)
                        pass.render_states.ztest = CompareFunc::Always;
                }
                else if (id == "cull")
                {
                    Token value = m_Lexer.NextToken();
                    if (value.type == TokenType::Off || value.text == "Off")
                        pass.render_states.cull = CullMode::Off;
                    else if (value.type == TokenType::Front || value.text == "Front")
                        pass.render_states.cull = CullMode::Front;
                    else if (value.type == TokenType::Back || value.text == "Back")
                        pass.render_states.cull = CullMode::Back;
                }
                else if (id == "colormask")
                {
                    // ColorMask RGB / RGBA / 0
                    Token value = m_Lexer.NextToken();
                    if (value.text == "0")
                        pass.render_states.color_mask = 0;
                    else
                        pass.render_states.color_mask = 0xF;  // 默认 RGBA
                }
                else if (id == "offset")
                {
                    Token factor = m_Lexer.NextToken();
                    Token units = m_Lexer.NextToken();
                    pass.render_states.offset_factor = factor.number_value;
                    pass.render_states.offset_units = units.number_value;
                }
                else
                {
                    // 可能是代码块的开始或其他
                    // 停止解析渲染状态
                    Warning("Unexpected identifier in render state: " + token.text);
                }
            }
            else if (token.type == TokenType::HLSLProgram || token.type == TokenType::GLSLProgram || token.type == TokenType::CGProgram)
            {
                // 代码块开始了，停止渲染状态解析
                // 代码块在 ParsePass 中处理
                return true;
            }
        }

        return true;
    }

    bool ShaderLabParser::ParseBlendState(RenderStates& state)
    {
        Token token = m_Lexer.NextToken();

        if (token.type == TokenType::Off || token.text == "Off")
        {
            state.blend_enable = false;
            return true;
        }

        state.blend_enable = true;

        auto parseBlendFunc = [](TokenType type) -> BlendFunc {
            switch (type)
            {
                case TokenType::One:
                    return BlendFunc::One;
                case TokenType::Zero:
                    return BlendFunc::Zero;
                case TokenType::SrcColor:
                    return BlendFunc::SrcColor;
                case TokenType::OneMinusSrcColor:
                    return BlendFunc::OneMinusSrcColor;
                case TokenType::DstColor:
                    return BlendFunc::DstColor;
                case TokenType::OneMinusDstColor:
                    return BlendFunc::OneMinusDstColor;
                case TokenType::SrcAlpha:
                    return BlendFunc::SrcAlpha;
                case TokenType::OneMinusSrcAlpha:
                    return BlendFunc::OneMinusSrcAlpha;
                case TokenType::DstAlpha:
                    return BlendFunc::DstAlpha;
                case TokenType::OneMinusDstAlpha:
                    return BlendFunc::OneMinusDstAlpha;
                case TokenType::ConstantColor:
                    return BlendFunc::ConstantColor;
                case TokenType::OneMinusConstantColor:
                    return BlendFunc::OneMinusConstantColor;
                case TokenType::ConstantAlpha:
                    return BlendFunc::ConstantAlpha;
                case TokenType::OneMinusConstantAlpha:
                    return BlendFunc::OneMinusConstantAlpha;
                case TokenType::SrcAlphaSaturate:
                    return BlendFunc::SrcAlphaSaturate;
                default:
                    return BlendFunc::One;
            }
        };

        state.blend_src = parseBlendFunc(token.type);
        state.blend_dst = BlendFunc::OneMinusSrcAlpha;

        Token next = m_Lexer.PeekToken();
        switch (next.type)
        {
            case TokenType::One:
            case TokenType::Zero:
            case TokenType::SrcColor:
            case TokenType::OneMinusSrcColor:
            case TokenType::DstColor:
            case TokenType::OneMinusDstColor:
            case TokenType::SrcAlpha:
            case TokenType::OneMinusSrcAlpha:
            case TokenType::DstAlpha:
            case TokenType::OneMinusDstAlpha:
            case TokenType::ConstantColor:
            case TokenType::OneMinusConstantColor:
            case TokenType::ConstantAlpha:
            case TokenType::OneMinusConstantAlpha:
            case TokenType::SrcAlphaSaturate:
                next = m_Lexer.NextToken();
                state.blend_dst = parseBlendFunc(next.type);
                break;
            default:
                break;
        }

        Token blend_op_token = m_Lexer.PeekToken();
        if (blend_op_token.type == TokenType::Add || blend_op_token.type == TokenType::Sub ||
            blend_op_token.type == TokenType::RevSub || blend_op_token.type == TokenType::Min ||
            blend_op_token.type == TokenType::Max)
        {
            blend_op_token = m_Lexer.NextToken();
            if (blend_op_token.type == TokenType::Add)
                state.blend_op = BlendOp::Add;
            else if (blend_op_token.type == TokenType::Sub)
                state.blend_op = BlendOp::Sub;
            else if (blend_op_token.type == TokenType::RevSub)
                state.blend_op = BlendOp::RevSub;
            else if (blend_op_token.type == TokenType::Min)
                state.blend_op = BlendOp::Min;
            else if (blend_op_token.type == TokenType::Max)
                state.blend_op = BlendOp::Max;
        }

        return true;
    }

    // ============= 辅助方法 =============

    bool ShaderLabParser::Expect(TokenType type)
    {
        Token token = m_Lexer.NextToken();
        if (token.type != type)
        {
            Error("Expected token type " + std::to_string(static_cast<int>(type)) +
                  ", got: " + token.text);
            return false;
        }
        return true;
    }

    bool ShaderLabParser::Expect(TokenType type, const std::string& error_msg)
    {
        Token token = m_Lexer.NextToken();
        if (token.type != type)
        {
            Error(error_msg + ", got: " + token.text);
            return false;
        }
        return true;
    }

    bool ShaderLabParser::Match(TokenType type)
    {
        Token token = m_Lexer.NextToken();
        return token.type == type;
    }

    bool ShaderLabParser::Peek(TokenType type)
    {
        Token token = m_Lexer.PeekToken();
        return token.type == type;
    }

    void ShaderLabParser::Error(const std::string& message)
    {
        m_Error = message + " at line " + std::to_string(m_Lexer.GetLine()) +
                  ", column " + std::to_string(m_Lexer.GetColumn());
    }

    void ShaderLabParser::Warning(const std::string& message)
    {
        m_Warnings.push_back(message + " at line " + std::to_string(m_Lexer.GetLine()) +
                             ", column " + std::to_string(m_Lexer.GetColumn()));
    }

    std::string ShaderLabParser::ReadIdentifier()
    {
        Token token = m_Lexer.NextToken();
        return token.text;
    }

    std::string ShaderLabParser::ReadString()
    {
        Token token = m_Lexer.NextToken();
        return token.text;
    }

    void ShaderLabParser::SkipToMatchingBrace()
    {
        int brace_count = 0;
        while (!m_Lexer.IsEnd())
        {
            Token token = m_Lexer.NextToken();
            if (token.type == TokenType::LeftBrace)
                ++brace_count;
            else if (token.type == TokenType::RightBrace)
            {
                if (brace_count == 0)
                    break;
                --brace_count;
            }
        }
    }

    void ShaderLabParser::SkipToNextStatement()
    {
        // ShaderLab 的命令有两种形状：
        //   行导向：Cull Off / ZWrite On / Lighting Off / Offset 1, 1
        //   块形式：Fog { Mode Off } / Material { ... }
        //
        // 旧实现一路吞到下一个 ';' 或 '}'，并且把那个 '}' 也一起吃掉。行导向命令
        // 没有 ';'，于是从 "Cull" 开始会一直吞到后面 Pass 的收尾 '}' —— 整个
        // Pass（连同里面的 CGPROGRAM）被静默丢弃。Spine 的 Caster pass 就是这样
        // 消失的，SubShader 最终 passes=0。
        //
        // 规则：吃掉本行剩余 token；遇到 ';' 吃掉它；遇到 '{' 走块匹配整体跳过；
        // 遇到 '}' 直接返回且不消费 —— 它是外层块的收尾，必须交回外层循环。

        if (m_Lexer.IsEnd())
        {
            return;
        }

        const uint32_t start_line = m_Lexer.PeekToken().line;

        while (!m_Lexer.IsEnd())
        {
            const Token token = m_Lexer.PeekToken();

            // 显式语句结束符：吃掉分号。
            if (token.type == TokenType::Semicolon)
            {
                m_Lexer.NextToken();
                return;
            }

            // 块形式命令：整体跳过（含收尾 '}'）。放在换行判断之前，这样
            // "Fog" 换行再写 "{" 也能正确识别。
            // 注意：SkipToMatchingBrace() 假定开头的 '{' 已经被消费掉了，
            // 所以这里先吃掉 '{' 再调。
            if (token.type == TokenType::LeftBrace)
            {
                m_Lexer.NextToken();
                SkipToMatchingBrace();
                return;
            }

            // 外层块的收尾：不消费。
            if (token.type == TokenType::RightBrace)
            {
                return;
            }

            // 换行 = 行导向语句结束。
            if (token.line != start_line)
            {
                return;
            }

            m_Lexer.NextToken();
        }
    }

}  // namespace ZEngine::ShaderLab
