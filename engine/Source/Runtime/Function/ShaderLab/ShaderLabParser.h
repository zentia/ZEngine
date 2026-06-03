#pragma once

#include "Runtime/Core/Base/Macro.h"
#include "ShaderLabAsset.h"
#include "ShaderLabLexer.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace ZEngine::ShaderLab
{

    // ============= 语法分析器 =============
    class ShaderLabParser
    {
    public:
        ShaderLabParser() = default;
        explicit ShaderLabParser(const std::string& source);

        // 设置源码
        void SetSource(const std::string& source);

        // 解析
        bool Parse();

        // 获取解析结果
        std::shared_ptr<ShaderLabAsset> GetAsset() const { return m_Asset; }

        // 获取错误信息
        const std::string& GetError() const { return m_Error; }
        bool HasError() const { return !m_Error.empty(); }

        // 获取警告信息
        const std::vector<std::string>& GetWarnings() const { return m_Warnings; }

    private:
        // 解析 Shader
        bool ParseShader();

        // 解析 Properties 块
        bool ParseProperties();

        // 解析单个属性
        bool ParseProperty(ShaderProperty& property);

        // 解析属性默认值
        bool ParsePropertyDefault(ShaderProperty& property, PropertyType type);

        // 解析 SubShader
        bool ParseSubShader(ShaderSubShader& subshader);

        // 解析 Tags
        bool ParseTags(std::map<std::string, std::string>& tags);

        // 解析 Pass
        bool ParsePass(ShaderPass& pass);

        // 解析渲染状态
        bool ParseRenderState(ShaderPass& pass);

        // 解析混合模式
        bool ParseBlendState(RenderStates& state);

        // 解析程序块（HLSLPROGRAM/GLSLPROGRAM）
        bool ParseProgram(ShaderProgram& program, TokenType code_token_type);

        // 解析 #pragma 指令
        bool ParsePragma(ShaderProgram& program, const std::string& line);

        // 辅助方法
        bool Expect(TokenType type);
        bool Expect(TokenType type, const std::string& error_msg);
        bool Match(TokenType type);
        bool Peek(TokenType type);

        void Error(const std::string& message);
        void Warning(const std::string& message);

        // 读取标识符（用于属性名、Pass名等）
        std::string ReadIdentifier();

        // 读取字符串
        std::string ReadString();

        // 跳过到指定符号（用于错误恢复）
        void SkipToMatchingBrace();
        void SkipToNextStatement();

    private:
        ShaderLabLexer m_Lexer;
        std::shared_ptr<ShaderLabAsset> m_Asset;
        std::string m_Error;
        std::vector<std::string> m_Warnings;
    };

    // ============= 便捷函数 =============

    // 从文件加载并解析
    inline std::shared_ptr<ShaderLabAsset> ParseShaderLabFromFile(const std::string& file_path)
    {
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            return nullptr;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();
        file.close();

        ShaderLabParser parser(source);
        if (!parser.Parse())
        {
            LOG_ERROR(ZEngine, "ShaderLab parse error: {}", parser.GetError());
            return nullptr;
        }

        return parser.GetAsset();
    }

    // 从字符串加载并解析
    inline std::shared_ptr<ShaderLabAsset> ParseShaderLabFromString(const std::string& source)
    {
        ShaderLabParser parser(source);
        if (!parser.Parse())
        {
            LOG_ERROR(ZEngine, "ShaderLab parse error: {}", parser.GetError());
            return nullptr;
        }

        return parser.GetAsset();
    }

}  // namespace ZEngine::ShaderLab
