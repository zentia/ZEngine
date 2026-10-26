#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"

#include <vector>
#include "Shader.generated.h"

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

    // ---- 常量缓冲落点 ----
    //
    // 属性名 -> UBO 字节偏移。没有它，脚本只能设 MaterialCustomParamNames.h 里
    // 写死的几个槽位；有了它才能 SetFloat("_WhateverYourShaderDeclared", v)。
    //
    // 由 ShaderImporter 在导入 .shader 时算好（见 ShaderCbufferReflection），
    // 不是运行时算：反射要读源码、跑文本解析，放在每帧建材质数据的路径上太贵，
    // 而且结果只随 shader 源码变，不随材质变。
    //
    // <0 表示这个属性没有出现在 UnityPerMaterial 里（纯 CPU 侧开关，或解析没
    // 找到对应的 cbuffer）。这不是错误 —— 按名字写值时应当返回 false，而不是
    // 往 UBO 里瞎写一个猜出来的偏移（猜错会静默写坏别人的属性）。
    int32_t m_CbufferOffset {-1};
    uint32_t m_CbufferSize {0};
    /// 分量数 1..4。矩阵记行数。0 表示未知。
    uint32_t m_CbufferComponents {0};
    /// 属性落在哪个 cbuffer 里（通常是 "UnityPerMaterial"）。空表示没找到。
    eastl::string m_CbufferName;

    bool HasCbufferSlot() const { return m_CbufferOffset >= 0 && m_CbufferSize > 0; }

    // ---- 贴图槽落点 ----
    //
    // 2D 属性绑到材质描述符集的哪个 binding（0..4，对应 binding 1..5）。
    // -1 = 不占槽（不是 2D 属性，或者是第 6 张之后的 2D 属性）。
    //
    // 和 cbuffer 落点一样是**导入期**算好的，按 Properties{} 里 2D 属性的声明
    // 顺序分配（见 ShaderLab/ShaderTextureSlot.h）。运行时不重算：重算意味着
    // 每帧建材质数据时都要遍历属性表，而且同一份 shader 算出来的答案是一样的。
    int32_t m_TextureSlot {-1};

    bool HasTextureSlot() const { return m_TextureSlot >= 0; }

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
        transfer.Transfer(m_CbufferOffset, "cbuffer_offset");
        transfer.Transfer(m_CbufferSize, "cbuffer_size");
        transfer.Transfer(m_CbufferComponents, "cbuffer_components");
        transfer.Transfer(m_CbufferName, "cbuffer_name");
        transfer.Transfer(m_TextureSlot, "texture_slot");
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

ZCLASS()
class ShaderRes : public Object
{
    ZGENERATED_BODY(ShaderRes)

public:
    ZPROPERTY()
    eastl::string m_ShaderName;
    ZPROPERTY()
    std::vector<ShaderPropertyDesc> m_Properties;
    ZPROPERTY()
    std::vector<ShaderPassDesc> m_Passes;

    ZPROPERTY()
    eastl::string m_VertexShaderFile;
    ZPROPERTY()
    eastl::string m_FragmentShaderFile;
    ZPROPERTY()
    eastl::string m_RenderPipeline {"StandardLit"};
    ZPROPERTY()
    eastl::string m_SourceLanguage {"HLSL"};
    ZPROPERTY()
    eastl::string m_VertexEntry {"main"};
    ZPROPERTY()
    eastl::string m_FragmentEntry {"main"};
    ZPROPERTY()
    eastl::string m_IncludeDirectory {"Assets/Shaders"};
    ZPROPERTY()
    bool m_EnableDx12 {true};
    ZPROPERTY()
    bool m_EnableVulkan {true};
    ZPROPERTY()
    bool m_EnableMetal {false};
};
