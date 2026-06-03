#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"

#include <vector>

struct ShaderPropertyDesc
{
    eastl::string m_Name;
    eastl::string m_DisplayName;
    eastl::string m_Type {"Float"};
    float m_DefaultFloat {0.0f};
    float m_RangeMin {0.0f};
    float m_RangeMax {1.0f};
    Vector3 m_DefaultColor {1.0f, 1.0f, 1.0f};
    float m_DefaultAlpha {1.0f};
    eastl::string m_DefaultTexture;
    bool m_DefaultToggle {false};

    static const char* GetTypeString() { return "ShaderPropertyDesc"; }
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer)
    {
        transfer.Transfer(m_Name, "name");
        transfer.Transfer(m_DisplayName, "display_name");
        transfer.Transfer(m_Type, "type");
        transfer.Transfer(m_DefaultFloat, "default_float");
        transfer.Transfer(m_RangeMin, "range_min");
        transfer.Transfer(m_RangeMax, "range_max");
        transfer.Transfer(m_DefaultColor, "default_color");
        transfer.Transfer(m_DefaultAlpha, "default_alpha");
        transfer.Transfer(m_DefaultTexture, "default_texture");
        transfer.Transfer(m_DefaultToggle, "default_toggle");
    }
};

struct ShaderPassDesc
{
    eastl::string m_Name {"GBuffer"};
    eastl::string m_LightMode {"GBuffer"};
    eastl::string m_VertexShaderFile;
    eastl::string m_FragmentShaderFile;
    eastl::string m_RenderPipeline {"StandardLit"};
    eastl::string m_VertexEntry {"main"};
    eastl::string m_FragmentEntry {"main"};
    eastl::string m_Cull {"Back"};
    eastl::string m_Ztest {"LEqual"};
    eastl::string m_Blend {"Off"};
    bool m_Zwrite {true};

    static const char* GetTypeString() { return "ShaderPassDesc"; }
    static bool AllowTransferOptimization() { return false; }

    template<typename TransferFunction>
    void Transfer(TransferFunction& transfer)
    {
        transfer.Transfer(m_Name, "name");
        transfer.Transfer(m_LightMode, "light_mode");
        transfer.Transfer(m_VertexShaderFile, "vertex_shader_file");
        transfer.Transfer(m_FragmentShaderFile, "fragment_shader_file");
        transfer.Transfer(m_RenderPipeline, "render_pipeline");
        transfer.Transfer(m_VertexEntry, "vertex_entry");
        transfer.Transfer(m_FragmentEntry, "fragment_entry");
        transfer.Transfer(m_Cull, "cull");
        transfer.Transfer(m_Ztest, "ztest");
        transfer.Transfer(m_Blend, "blend");
        transfer.Transfer(m_Zwrite, "zwrite");
    }
};

class ShaderRes : public Object
{
    REGISTER_CLASS(ShaderRes)
    DECLARE_OBJECT_SERIALIZE(ShaderRes)

public:
    eastl::string m_ShaderName;
    std::vector<ShaderPropertyDesc> m_Properties;
    std::vector<ShaderPassDesc> m_Passes;

    eastl::string m_VertexShaderFile;
    eastl::string m_FragmentShaderFile;
    eastl::string m_RenderPipeline {"StandardLit"};
    eastl::string m_SourceLanguage {"HLSL"};
    eastl::string m_VertexEntry {"main"};
    eastl::string m_FragmentEntry {"main"};
    eastl::string m_IncludeDirectory {"Assets/Shaders"};
    bool m_EnableDx12 {true};
    bool m_EnableVulkan {true};
    bool m_EnableMetal {false};
};
