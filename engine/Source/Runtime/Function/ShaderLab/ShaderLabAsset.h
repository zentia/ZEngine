#pragma once

#include "Runtime/Core/Base/Macro.h"

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ZEngine::ShaderLab
{

    // ============= 属性类型 =============
    enum class PropertyType
    {
        Float,
        Int,
        Range,
        Color,
        Vector,
        Texture2D,
        Cube,
        Texture3D,
        FloatArray
    };

    // 属性默认值
    struct PropertyDefaultValue
    {
        float float_value = 0.0f;
        int int_value = 0;
        float range_min = 0.0f;
        float range_max = 1.0f;
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float vector[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        std::string texture_path;
        std::string texture_default = "white";
        std::vector<float> float_array;
    };

    using PropertyValue = std::variant<PropertyDefaultValue>;

    // 单个属性定义
    struct ShaderProperty
    {
        std::string name;          // 属性名，如 "_MainTex"
        std::string display_name;  // 显示名，如 "Main Texture"
        PropertyType type = PropertyType::Float;
        PropertyDefaultValue default_value;

        // 序列化
        template<typename TransferFunction>
        void Transfer(TransferFunction& transfer)
        {
            transfer.Transfer(name, "name");
            transfer.Transfer(display_name, "display_name");
            transfer.Transfer(type, "type");
            transfer.Transfer(default_value.float_value, "default_float");
            transfer.Transfer(default_value.range_min, "range_min");
            transfer.Transfer(default_value.range_max, "range_max");
            transfer.Transfer(default_value.color, "default_color");
            transfer.Transfer(default_value.vector, "default_vector");
            transfer.Transfer(default_value.texture_path, "texture_path");
            transfer.Transfer(default_value.texture_default, "texture_default");
        }
    };

    // ============= 混合模式 =============
    enum class BlendFunc
    {
        Zero,
        One,
        Keep,  // 用于 Stencil（当没有定义 StencilFunc 时使用）
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

    enum class BlendOp
    {
        Add,
        Sub,
        RevSub,
        Min,
        Max
    };

    // ============= 深度测试 =============
    enum class CompareFunc
    {
        Less,
        Greater,
        LEqual,
        GEqual,
        Equal,
        NotEqual,
        Always
    };

    // ============= 裁剪模式 =============
    enum class CullMode
    {
        Off,
        Front,
        Back
    };

    // ============= 模板状态 =============
    struct StencilRef
    {
        int ref = 0;
        CompareFunc comp = CompareFunc::Always;
        BlendFunc pass = BlendFunc::Keep;
        BlendFunc fail = BlendFunc::Keep;
        BlendFunc zfail = BlendFunc::Keep;
    };

    // 渲染状态
    struct RenderStates
    {
        bool blend_enable = false;
        BlendFunc blend_src = BlendFunc::One;
        BlendFunc blend_dst = BlendFunc::Zero;
        BlendOp blend_op = BlendOp::Add;
        BlendFunc blend_src_alpha = BlendFunc::One;
        BlendFunc blend_dst_alpha = BlendFunc::Zero;

        bool zwrite = true;
        CompareFunc ztest = CompareFunc::LEqual;

        CullMode cull = CullMode::Back;

        uint8_t color_mask = 0xF;  // RGBA = 0xF

        std::optional<StencilRef> stencil;

        float offset_factor = 0.0f;
        float offset_units = 0.0f;

        template<typename TransferFunction>
        void Transfer(TransferFunction& transfer)
        {
            transfer.Transfer(blend_enable, "blend_enable");
            transfer.Transfer(zwrite, "zwrite");
            transfer.Transfer(cull, "cull");
            transfer.Transfer(color_mask, "color_mask");
        }
    };

    // ============= Shader 程序语言 =============
    enum class ShaderLanguage
    {
        HLSL,
        GLSL,
        CG
    };

    // ============= Shader 程序（CGPROGRAM/HLSLPROGRAM块）=============
    struct ShaderProgram
    {
        ShaderLanguage language = ShaderLanguage::HLSL;

        std::string source_code;  // 原始代码（包含 #pragma 等）
        std::string hlsl_code;    // 预处理后的纯 HLSL 代码

        std::string vertex_entry = "vert";
        std::string fragment_entry = "frag";
        std::string compute_entry;

        std::vector<std::string> includes;
        std::vector<std::string> pragma_keywords;          // #pragma multi_compile 定义的
        std::vector<std::string> shader_feature_keywords;  // #pragma shader_feature 定义的
    };

    // ============= Shader Pass =============
    struct ShaderPass
    {
        std::string name = "Default";
        std::string light_mode;
        std::map<std::string, std::string> tags;
        RenderStates render_states;
        std::vector<ShaderProgram> programs;

        template<typename TransferFunction>
        void Transfer(TransferFunction& transfer)
        {
            transfer.Transfer(name, "name");
            transfer.Transfer(light_mode, "light_mode");
            transfer.Transfer(tags, "tags");
            transfer.Transfer(render_states, "render_states");
        }
    };

    // ============= Shader SubShader =============
    struct ShaderSubShader
    {
        int lod = 100;
        std::map<std::string, std::string> tags;
        std::vector<ShaderPass> passes;

        template<typename TransferFunction>
        void Transfer(TransferFunction& transfer)
        {
            transfer.Transfer(lod, "lod");
            transfer.Transfer(tags, "tags");
            transfer.Transfer(passes, "passes");
        }
    };

    // ============= ShaderLab 根结构 =============
    class ShaderLabAsset
    {
    public:
        std::string shader_name = "NewShader";
        std::string custom_editor;
        std::string fallback;
        std::vector<ShaderProperty> properties;
        std::vector<ShaderSubShader> subshaders;

        // 获取给定关键字组合对应的变体 Shader
        // (后续实现)

        template<typename TransferFunction>
        void Transfer(TransferFunction& transfer)
        {
            transfer.Transfer(shader_name, "shader_name");
            transfer.Transfer(custom_editor, "custom_editor");
            transfer.Transfer(fallback, "fallback");
            transfer.Transfer(properties, "properties");
            transfer.Transfer(subshaders, "subshaders");
        }
    };

    // ============= 工具函数 =============
    inline const char* PropertyTypeToString(PropertyType type)
    {
        switch (type)
        {
            case PropertyType::Float:
                return "Float";
            case PropertyType::Int:
                return "Int";
            case PropertyType::Range:
                return "Range";
            case PropertyType::Color:
                return "Color";
            case PropertyType::Vector:
                return "Vector";
            case PropertyType::Texture2D:
                return "2D";
            case PropertyType::Cube:
                return "Cube";
            case PropertyType::Texture3D:
                return "3D";
            case PropertyType::FloatArray:
                return "FloatArray";
            default:
                return "Unknown";
        }
    }

    inline PropertyType StringToPropertyType(const std::string& str)
    {
        if (str == "Float" || str == "float")
            return PropertyType::Float;
        if (str == "Int" || str == "int")
            return PropertyType::Int;
        if (str == "Range" || str == "range")
            return PropertyType::Range;
        if (str == "Color" || str == "color")
            return PropertyType::Color;
        if (str == "Vector" || str == "vector")
            return PropertyType::Vector;
        if (str == "2D" || str == "2d" || str == "sampler2D")
            return PropertyType::Texture2D;
        if (str == "Cube" || str == "cube" || str == "samplerCube")
            return PropertyType::Cube;
        if (str == "3D" || str == "3d" || str == "sampler3D")
            return PropertyType::Texture3D;
        return PropertyType::Float;
    }

}  // namespace ZEngine::ShaderLab
