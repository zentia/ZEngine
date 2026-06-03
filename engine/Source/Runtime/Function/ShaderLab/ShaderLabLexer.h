#pragma once

#include "Runtime/Core/Base/Macro.h"
#include "ShaderLabAsset.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{

    // ============= Token 类型 =============
    enum class TokenType : uint8_t
    {
        // 特殊
        EndOfFile,
        Error,

        // 标识符和字面量
        Identifier,
        StringLiteral,
        NumberLiteral,

        // ShaderLab 关键词
        Shader,
        Properties,
        SubShader,
        Pass,
        Tags,
        LOD,
        FallBack,
        CustomEditor,

        // 属性类型
        Float,
        Int,
        Range,
        Color,
        Vector,
        Texture2D,
        Cube,
        Texture3D,
        FloatArray,

        // 渲染状态
        Blend,
        BlendOp,
        ZWrite,
        ZTest,
        Cull,
        ColorMask,
        Stencil,
        Offset,
        GrabPass,
        UsePass,

        // 代码块
        HLSLProgram,
        GLSLProgram,
        CGProgram,
        EndHLSL,
        EndGLSL,
        EndCG,

        // 符号
        LeftBrace,     // {
        RightBrace,    // }
        LeftBracket,   // [
        RightBracket,  // ]
        LeftParen,     // (
        RightParen,    // )
        LessThan,      // <
        GreaterThan,   // >
        Comma,         // ,
        Equal,         // =
        Colon,         // :
        Semicolon,     // ;
        Hash,          // #
        At,            // @
        Slash,         // /
        Pipe,          // |
        Ampersand,     // &

        // 布尔/状态值
        On,
        Off,
        True,
        False,
        Front,
        Back,

        // 渲染队列
        Background,
        Geometry,
        AlphaTest,
        Transparent,
        Overlay,

        // 比较函数
        Less,
        Greater,
        LEqual,
        GEqual,
        Eq,
        NotEqual,
        Always,

        // 混合操作
        Add,
        Sub,
        RevSub,
        Min,
        Max,

        // 混合因子
        Zero,
        One,
        SrcColor,
        OneMinusSrcColor,
        DstColor,
        OneMinusDstColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha,
        ConstantColor,
        OneMinusConstantColor,
        ConstantAlpha,
        OneMinusConstantAlpha,
        SrcAlphaSaturate,
        Src1Color,
        OneMinusSrc1Color,
        Src1Alpha,
        OneMinusSrc1Alpha
    };

    // Token 结构
    struct Token
    {
        TokenType type = TokenType::Error;
        std::string text;
        uint32_t line = 0;
        uint32_t column = 0;

        // 数值（如果是数字字面量）
        float number_value = 0.0f;

        bool IsKeyword() const;
    };

    // ============= 词法分析器 =============
    class ShaderLabLexer
    {
    public:
        ShaderLabLexer() = default;
        explicit ShaderLabLexer(const std::string& source);

        // 设置源码
        void SetSource(const std::string& source);

        // 重置解析位置
        void Reset();

        // 获取下一个 Token
        Token NextToken();

        // 预览下一个 Token（不消费）
        Token PeekToken();

        // 跳过指定类型的 Token
        bool Expect(TokenType type);
        bool Expect(TokenType type, const char* error_msg);

        // 获取当前解析位置
        uint32_t GetLine() const { return m_Line; }
        uint32_t GetColumn() const { return m_Column; }

        // 是否到达末尾
        bool IsEnd() const { return m_Pos >= m_Source.size(); }

        // 获取错误信息
        const std::string& GetError() const { return m_Error; }
        bool HasError() const { return !m_Error.empty(); }

    private:
        // 跳过空白字符
        void SkipWhitespace();

        // 跳过注释
        void SkipComment();

        // 读取标识符或关键词
        Token ReadIdentifier();

        // 读取字符串字面量
        Token ReadString();

        // 读取数字
        Token ReadNumber();

        // 读取 HLSL/GLSL 代码块
        Token ReadShaderCodeBlock(TokenType end_type);

        // 尝试识别关键词
        TokenType IdentifyKeyword(const std::string& text) const;

        // 是否为字母
        static bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        // 是否为数字
        static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        // 是否为字母或数字
        static bool IsAlphaNumeric(char c) { return IsAlpha(c) || IsDigit(c); }

    private:
        std::string m_Source;
        size_t m_Pos = 0;
        uint32_t m_Line = 1;
        uint32_t m_Column = 1;

        Token m_PeekedToken;  // 预览的 Token
        bool m_HasPeeked = false;

        std::string m_Error;
    };

}  // namespace ZEngine::ShaderLab
