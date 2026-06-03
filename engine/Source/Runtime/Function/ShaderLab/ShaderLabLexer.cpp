#include "Runtime/Function/ShaderLab/ShaderLabLexer.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ZEngine::ShaderLab
{

    // ============= Token 方法 =============
    bool Token::IsKeyword() const
    {
        return static_cast<uint8_t>(type) <= static_cast<uint8_t>(TokenType::CustomEditor);
    }

    // ============= 关键词识别表 =============
    struct KeywordEntry
    {
        const char* keyword;
        TokenType type;
    };

    static const KeywordEntry s_KeywordTable[] = {
        // ShaderLab 关键词
        {"Shader", TokenType::Shader},
        {"Properties", TokenType::Properties},
        {"SubShader", TokenType::SubShader},
        {"Pass", TokenType::Pass},
        {"Tags", TokenType::Tags},
        {"LOD", TokenType::LOD},
        {"FallBack", TokenType::FallBack},
        {"CustomEditor", TokenType::CustomEditor},
        {"GrabPass", TokenType::GrabPass},
        {"UsePass", TokenType::UsePass},

        // 属性类型
        {"Float", TokenType::Float},
        {"float", TokenType::Float},
        {"Int", TokenType::Int},
        {"int", TokenType::Int},
        {"Range", TokenType::Range},
        {"range", TokenType::Range},
        {"Color", TokenType::Color},
        {"color", TokenType::Color},
        {"Vector", TokenType::Vector},
        {"vector", TokenType::Vector},
        {"2D", TokenType::Texture2D},
        {"2d", TokenType::Texture2D},
        {"sampler2D", TokenType::Texture2D},
        {"Cube", TokenType::Cube},
        {"cube", TokenType::Cube},
        {"samplerCube", TokenType::Cube},
        {"3D", TokenType::Texture3D},
        {"3d", TokenType::Texture3D},
        {"sampler3D", TokenType::Texture3D},
        {"FloatArray", TokenType::FloatArray},

        // 渲染状态
        {"Blend", TokenType::Blend},
        {"BlendOp", TokenType::BlendOp},
        {"ZWrite", TokenType::ZWrite},
        {"ZTest", TokenType::ZTest},
        {"Cull", TokenType::Cull},
        {"ColorMask", TokenType::ColorMask},
        {"Stencil", TokenType::Stencil},
        {"Offset", TokenType::Offset},

        // 代码块
        {"HLSLPROGRAM", TokenType::HLSLProgram},
        {"GLSLPROGRAM", TokenType::GLSLProgram},
        {"CGPROGRAM", TokenType::CGProgram},
        {"ENDHLSL", TokenType::EndHLSL},
        {"ENDGLSL", TokenType::EndGLSL},
        {"ENDCG", TokenType::EndCG},

        // 布尔/状态值
        {"On", TokenType::On},
        {"Off", TokenType::Off},
        {"True", TokenType::True},
        {"true", TokenType::True},
        {"False", TokenType::False},
        {"false", TokenType::False},
        {"Front", TokenType::Front},
        {"Back", TokenType::Back},

        // 渲染队列
        {"Background", TokenType::Background},
        {"Geometry", TokenType::Geometry},
        {"AlphaTest", TokenType::AlphaTest},
        {"Transparent", TokenType::Transparent},
        {"Overlay", TokenType::Overlay},

        // 比较函数
        {"Less", TokenType::Less},
        {"Greater", TokenType::Greater},
        {"LEqual", TokenType::LEqual},
        {"GEqual", TokenType::GEqual},
        {"Equal", TokenType::Eq},
        {"NotEqual", TokenType::NotEqual},
        {"Always", TokenType::Always},

        // 混合操作
        {"Add", TokenType::Add},
        {"Sub", TokenType::Sub},
        {"RevSub", TokenType::RevSub},
        {"Min", TokenType::Min},
        {"Max", TokenType::Max},

        // 混合因子
        {"Zero", TokenType::Zero},
        {"One", TokenType::One},
        {"SrcColor", TokenType::SrcColor},
        {"OneMinusSrcColor", TokenType::OneMinusSrcColor},
        {"DstColor", TokenType::DstColor},
        {"OneMinusDstColor", TokenType::OneMinusDstColor},
        {"SrcAlpha", TokenType::SrcAlpha},
        {"OneMinusSrcAlpha", TokenType::OneMinusSrcAlpha},
        {"DstAlpha", TokenType::DstAlpha},
        {"OneMinusDstAlpha", TokenType::OneMinusDstAlpha},
        {"ConstantColor", TokenType::ConstantColor},
        {"OneMinusConstantColor", TokenType::OneMinusConstantColor},
        {"ConstantAlpha", TokenType::ConstantAlpha},
        {"OneMinusConstantAlpha", TokenType::OneMinusConstantAlpha},
        {"SrcAlphaSaturate", TokenType::SrcAlphaSaturate},
        {"Src1Color", TokenType::Src1Color},
        {"OneMinusSrc1Color", TokenType::OneMinusSrc1Color},
        {"Src1Alpha", TokenType::Src1Alpha},
        {"OneMinusSrc1Alpha", TokenType::OneMinusSrc1Alpha}};

    // ============= ShaderLabLexer 实现 =============
    ShaderLabLexer::ShaderLabLexer(const std::string& source)
        : m_Source(source)
    {
    }

    void ShaderLabLexer::SetSource(const std::string& source)
    {
        m_Source = source;
        Reset();
    }

    void ShaderLabLexer::Reset()
    {
        m_Pos = 0;
        m_Line = 1;
        m_Column = 1;
        m_HasPeeked = false;
        m_Error.clear();
    }

    Token ShaderLabLexer::NextToken()
    {
        // 如果有预览的 Token，直接返回
        if (m_HasPeeked)
        {
            m_HasPeeked = false;
            return m_PeekedToken;
        }

        // 跳过空白和注释
        SkipWhitespace();
        SkipComment();

        // 检查是否到达末尾
        if (IsEnd())
        {
            return Token {TokenType::EndOfFile, "", m_Line, m_Column};
        }

        char c = m_Source[m_Pos];
        Token token;
        token.line = m_Line;
        token.column = m_Column;

        // 标识符或关键词
        if (IsAlpha(c))
        {
            return ReadIdentifier();
        }

        // 字符串
        if (c == '"')
        {
            return ReadString();
        }

        // 数字
        if (IsDigit(c) || (c == '.' && m_Pos + 1 < m_Source.size() && IsDigit(m_Source[m_Pos + 1])))
        {
            return ReadNumber();
        }

        // 单字符符号
        ++m_Pos;
        ++m_Column;

        switch (c)
        {
            case '{':
                token.type = TokenType::LeftBrace;
                token.text = "{";
                break;
            case '}':
                token.type = TokenType::RightBrace;
                token.text = "}";
                break;
            case '[':
                token.type = TokenType::LeftBracket;
                token.text = "[";
                break;
            case ']':
                token.type = TokenType::RightBracket;
                token.text = "]";
                break;
            case '(':
                token.type = TokenType::LeftParen;
                token.text = "(";
                break;
            case ')':
                token.type = TokenType::RightParen;
                token.text = ")";
                break;
            case '<':
                token.type = TokenType::LessThan;
                token.text = "<";
                break;
            case '>':
                token.type = TokenType::GreaterThan;
                token.text = ">";
                break;
            case ',':
                token.type = TokenType::Comma;
                token.text = ",";
                break;
            case '=':
                token.type = TokenType::Equal;
                token.text = "=";
                break;
            case ':':
                token.type = TokenType::Colon;
                token.text = ":";
                break;
            case ';':
                token.type = TokenType::Semicolon;
                token.text = ";";
                break;
            case '#':
                token.type = TokenType::Hash;
                token.text = "#";
                break;
            case '@':
                token.type = TokenType::At;
                token.text = "@";
                break;
            case '/':
                token.type = TokenType::Slash;
                token.text = "/";
                break;
            case '|':
                token.type = TokenType::Pipe;
                token.text = "|";
                break;
            case '&':
                token.type = TokenType::Ampersand;
                token.text = "&";
                break;
            default:
                token.type = TokenType::Error;
                token.text = std::string(1, c);
                break;
        }

        return token;
    }

    Token ShaderLabLexer::PeekToken()
    {
        if (!m_HasPeeked)
        {
            m_PeekedToken = NextToken();
            m_HasPeeked = true;
        }
        return m_PeekedToken;
    }

    bool ShaderLabLexer::Expect(TokenType type)
    {
        Token token = NextToken();
        if (token.type != type)
        {
            m_Error = "Unexpected token: expected " + std::to_string(static_cast<int>(type)) +
                      ", got " + std::to_string(static_cast<int>(token.type)) +
                      " (" + token.text + ") at line " + std::to_string(token.line);
            return false;
        }
        return true;
    }

    bool ShaderLabLexer::Expect(TokenType type, const char* error_msg)
    {
        Token token = NextToken();
        if (token.type != type)
        {
            m_Error = error_msg;
            return false;
        }
        return true;
    }

    void ShaderLabLexer::SkipWhitespace()
    {
        while (m_Pos < m_Source.size())
        {
            char c = m_Source[m_Pos];
            if (c == ' ' || c == '\t' || c == '\r')
            {
                ++m_Pos;
                ++m_Column;
            }
            else if (c == '\n')
            {
                ++m_Pos;
                ++m_Line;
                m_Column = 1;
            }
            else
            {
                break;
            }
        }
    }

    void ShaderLabLexer::SkipComment()
    {
        if (m_Pos + 1 >= m_Source.size())
            return;

        // 单行注释 //
        if (m_Source[m_Pos] == '/' && m_Source[m_Pos + 1] == '/')
        {
            while (m_Pos < m_Source.size() && m_Source[m_Pos] != '\n')
            {
                ++m_Pos;
                ++m_Column;
            }
            // 递归检查更多注释
            SkipWhitespace();
            SkipComment();
        }
        // 多行注释 /* */
        else if (m_Source[m_Pos] == '/' && m_Source[m_Pos + 1] == '*')
        {
            m_Pos += 2;
            m_Column += 2;
            while (m_Pos + 1 < m_Source.size())
            {
                if (m_Source[m_Pos] == '\n')
                {
                    ++m_Line;
                    m_Column = 1;
                }
                else if (m_Source[m_Pos] == '*' && m_Source[m_Pos + 1] == '/')
                {
                    m_Pos += 2;
                    m_Column += 2;
                    break;
                }
                ++m_Pos;
                ++m_Column;
            }
            // 递归检查更多注释
            SkipWhitespace();
            SkipComment();
        }
    }

    Token ShaderLabLexer::ReadIdentifier()
    {
        Token token;
        token.line = m_Line;
        token.column = m_Column;

        size_t start = m_Pos;
        while (m_Pos < m_Source.size() && IsAlphaNumeric(m_Source[m_Pos]))
        {
            ++m_Pos;
            ++m_Column;
        }

        token.text = m_Source.substr(start, m_Pos - start);

        // 检查是否是关键词
        token.type = IdentifyKeyword(token.text);
        if (token.type != TokenType::Identifier)
        {
            // 如果是代码块开始，特殊处理
            if (token.type == TokenType::HLSLProgram || token.type == TokenType::GLSLProgram || token.type == TokenType::CGProgram)
            {
                return ReadShaderCodeBlock(
                    token.type == TokenType::HLSLProgram ? TokenType::EndHLSL : token.type == TokenType::GLSLProgram ? TokenType::EndGLSL
                                                                                                                     : TokenType::EndCG);
            }
        }

        return token;
    }

    Token ShaderLabLexer::ReadString()
    {
        Token token;
        token.line = m_Line;
        token.column = m_Column;

        ++m_Pos;  // 跳过开始的 "

        size_t start = m_Pos;
        while (m_Pos < m_Source.size() && m_Source[m_Pos] != '"')
        {
            if (m_Source[m_Pos] == '\n')
            {
                ++m_Line;
                m_Column = 0;
            }
            ++m_Pos;
            ++m_Column;
        }

        token.text = m_Source.substr(start, m_Pos - start);

        if (m_Pos < m_Source.size() && m_Source[m_Pos] == '"')
        {
            ++m_Pos;  // 跳过结束的 "
        }

        token.type = TokenType::StringLiteral;
        return token;
    }

    Token ShaderLabLexer::ReadNumber()
    {
        Token token;
        token.line = m_Line;
        token.column = m_Column;

        size_t start = m_Pos;
        bool has_dot = false;

        while (m_Pos < m_Source.size())
        {
            char c = m_Source[m_Pos];
            if (IsDigit(c))
            {
                ++m_Pos;
                ++m_Column;
            }
            else if (c == '.' && !has_dot)
            {
                has_dot = true;
                ++m_Pos;
                ++m_Column;
            }
            else if ((c == 'e' || c == 'E') && m_Pos > start)
            {
                // 科学计数法
                ++m_Pos;
                ++m_Column;
                if (m_Pos < m_Source.size() && (m_Source[m_Pos] == '+' || m_Source[m_Pos] == '-'))
                {
                    ++m_Pos;
                    ++m_Column;
                }
            }
            else
            {
                break;
            }
        }

        token.text = m_Source.substr(start, m_Pos - start);
        token.type = TokenType::NumberLiteral;

        // 解析数值
        try
        {
            token.number_value = std::stof(token.text);
        }
        catch (...)
        {
            token.number_value = 0.0f;
        }

        return token;
    }

    Token ShaderLabLexer::ReadShaderCodeBlock(TokenType end_type)
    {
        Token token;
        token.line = m_Line;
        token.column = m_Column;
        token.type = end_type == TokenType::EndHLSL ? TokenType::HLSLProgram : end_type == TokenType::EndGLSL ? TokenType::GLSLProgram
                                                                                                              : TokenType::CGProgram;

        const char* end_tag = end_type == TokenType::EndHLSL ? "ENDHLSL" : end_type == TokenType::EndGLSL ? "ENDGLSL"
                                                                                                          : "ENDCG";

        size_t start_pos = m_Pos;

        size_t end_pos = m_Source.find(end_tag, m_Pos);
        if (end_pos == std::string::npos)
        {
            token.text = m_Source.substr(start_pos);
            m_Pos = m_Source.size();
            return token;
        }

        token.text = m_Source.substr(start_pos, end_pos - start_pos);
        m_Pos = end_pos + strlen(end_tag);

        for (size_t i = start_pos; i < m_Pos; ++i)
        {
            if (m_Source[i] == '\n')
            {
                ++m_Line;
                m_Column = 1;
            }
            else
            {
                ++m_Column;
            }
        }

        return token;
    }

    TokenType ShaderLabLexer::IdentifyKeyword(const std::string& text) const
    {
        for (const auto& entry : s_KeywordTable)
        {
            if (text == entry.keyword)
            {
                return entry.type;
            }
        }
        return TokenType::Identifier;
    }

}  // namespace ZEngine::ShaderLab
