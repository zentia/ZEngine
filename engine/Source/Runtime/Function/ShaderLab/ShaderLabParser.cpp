#include "Runtime/Function/ShaderLab/ShaderLabParser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ZEngine::ShaderLab
{

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
                if (!ParsePass(pass))
                {
                    return false;
                }
                subshader.passes.push_back(std::move(pass));
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
            else if (token.type == TokenType::Blend || id == "blend")
            {
                ParseBlendState(pass.render_states);
            }
            else if (token.type == TokenType::ZWrite || id == "zwrite")
            {
                Token value = m_Lexer.NextToken();
                if (value.type == TokenType::On || value.text == "On")
                {
                    pass.render_states.zwrite = true;
                }
                else if (value.type == TokenType::Off || value.text == "Off")
                {
                    pass.render_states.zwrite = false;
                }
            }
            else if (token.type == TokenType::ZTest || id == "ztest")
            {
                Token value = m_Lexer.NextToken();
                pass.render_states.ztest = CompareFunc::LEqual;
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
            else if (token.type == TokenType::Cull || id == "cull")
            {
                Token value = m_Lexer.NextToken();
                if (value.type == TokenType::Off || value.text == "Off")
                {
                    pass.render_states.cull = CullMode::Off;
                }
                else if (value.type == TokenType::Front || value.text == "Front")
                {
                    pass.render_states.cull = CullMode::Front;
                }
                else if (value.type == TokenType::Back || value.text == "Back")
                {
                    pass.render_states.cull = CullMode::Back;
                }
            }
            else if (token.type == TokenType::ColorMask || id == "colormask")
            {
                Token value = m_Lexer.NextToken();
                pass.render_states.color_mask = value.text == "0" ? 0 : 0xF;
            }
            else if (token.type == TokenType::Offset || id == "offset")
            {
                Token factor = m_Lexer.NextToken();
                Token units = m_Lexer.NextToken();
                pass.render_states.offset_factor = factor.number_value;
                pass.render_states.offset_units = units.number_value;
            }
            else if (token.type == TokenType::HLSLProgram || token.type == TokenType::GLSLProgram || token.type == TokenType::CGProgram)
            {
                ShaderProgram program;
                program.language = token.type == TokenType::HLSLProgram ? ShaderLanguage::HLSL : token.type == TokenType::GLSLProgram ? ShaderLanguage::GLSL
                                                                                                                                      : ShaderLanguage::CG;
                program.source_code = token.text;
                pass.programs.push_back(std::move(program));
            }
            else
            {
                Warning("Unexpected token in Pass block: " + token.text);
            }
        }

        return true;
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
        while (!m_Lexer.IsEnd())
        {
            Token token = m_Lexer.PeekToken();
            if (token.type == TokenType::Semicolon ||
                token.type == TokenType::RightBrace)
            {
                m_Lexer.NextToken();
                break;
            }
            m_Lexer.NextToken();
        }
    }

}  // namespace ZEngine::ShaderLab
