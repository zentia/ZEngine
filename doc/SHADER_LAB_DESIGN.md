# ZEngine ShaderLab 设计文档

## 一、概述

### 什么是 ShaderLab？

ShaderLab 是 Unity 引擎的 Shader 描述语言，是一种声明式的领域特定语言（DSL），用于描述 Shader 的结构、属性、渲染状态和 GPU 程序代码。

### ZEngine ShaderLab 的设计目标

1. **语法兼容**：支持 Unity ShaderLab 语法的核心特性
2. **现代渲染**：基于 Vulkan/HLSL，适合延迟渲染管线
3. **可扩展**：支持自定义 Pass 类型和渲染状态
4. **高性能**：运行时编译优化 + 变体缓存

---

## 二、ShaderLab 语法规范

### 2.1 基本结构

```shaderlab
Shader "ShaderName"
{
    Properties
    {
        // 属性定义
        _MainTex ("Texture", 2D) = "white" {}
        _Color ("Color", Color) = (1,1,1,1)
    }
    
    SubShader
    {
        Tags { "RenderPipeline" = "Deferred" }
        
        Pass
        {
            Name "GBuffer"
            Tags { "LightMode" = "GBuffer" }
            
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "common.h"
            
            // HLSL 代码
            ENDHLSL
        }
    }
    
    FallBack "Diffuse"
}
```

### 2.2 关键词（Keywords）

| 关键词 | 说明 | 示例 |
|--------|------|------|
| `Shader` | Shader 根定义 | `Shader "Custom/Standard"` |
| `Properties` | 属性块 | `Properties { ... }` |
| `SubShader` | 子着色器 | `SubShader { ... }` |
| `Pass` | 渲染通道 | `Pass { ... }` |
| `Tags` | 标签 | `Tags { "LightMode" = "GBuffer" }` |
| `LOD` | 细节级别 | `LOD 200` |
| `FallBack` | 后备着色器 | `FallBack "Diffuse"` |
| `CustomEditor` | 自定义编辑器 | `CustomEditor "CustomShaderGUI"` |

### 2.3 属性类型（Properties）

```shaderlab
Properties
{
    // Float
    _Glossiness ("Smoothness", Range(0,1)) = 0.5
    
    // Int
    _Quality ("Quality", Int) = 2
    
    // Color
    _Color ("Color", Color) = (1,1,1,1)
    
    // Vector
    _Direction ("Direction", Vector) = (0,1,0,0)
    
    // 2D Texture
    _MainTex ("Albedo", 2D) = "white" {}
    
    // Cube Texture
    _Cubemap ("Cubemap", Cube) = "" {}
    
    // 3D Texture
    _Volume ("Volume", 3D) = "" {}
    
    // Float Array
    _Weights ("Weights", FloatArray) = (1, 2, 3)
}
```

### 2.4 渲染状态（Render States）

```shaderlab
Pass
{
    // 混合模式
    Blend Off          // 或 Blend SrcAlpha OneMinusSrcAlpha
    BlendOp Add        // Add, Sub, RevSub, Min, Max
    
    // 深度测试
    ZWrite On          // On, Off
    ZTest LEqual       // Less, Greater, LEqual, GEqual, Equal, NotEqual, Always
    
    // 裁剪
    Cull Off           // Off, Front, Back
    ColorMask RGB      // RGB, A, 0, or any combination like RGBA
    
    // 模板
    Stencil
    {
        Ref 2
        Comp Always
        Pass Replace
    }
    
    // Offset
    Offset 0, 0        // For Factor, For Units
}
```

### 2.5 Tags

#### SubShader Tags
```shaderlab
Tags { 
    "RenderPipeline" = "Deferred"    // Deferred, Forward
    "Queue" = "Geometry"              // Background, Geometry, AlphaTest, Transparent, Overlay
    "RenderType" = "Opaque"          // Opaque, Transparent, TransparentCutout, Background, Overlay
}
```

#### Pass Tags
```shaderlab
Tags { 
    "LightMode" = "GBuffer"          // GBuffer, Forward, ForwardOnly, SRPDefaultUnlit, etc.
    "PassFlags" = "OnlyDirectional"  // OnlyDirectional (只传递主方向光)
}
```

### 2.6 CGPROGRAM/HLSLPROGRAM

```shaderlab
Pass
{
    HLSLPROGRAM
    
    #pragma vertex vert
    #pragma fragment frag
    #pragma multi_compile _ LIGHTMAP_ON
    #pragma multi_compile _ DIRLIGHTMAP_COMBINED
    #pragma shader_feature _ EMISSION
    
    #include "Packages/com.unity.render-pipelines.core/ShaderLibrary/Common.hlsl"
    
    struct appdata
    {
        float4 vertex : POSITION;
        float2 uv : TEXCOORD0;
    };
    
    struct v2f
    {
        float2 uv : TEXCOORD0;
        float4 vertex : SV_POSITION;
    };
    
    v2f vert (appdata v)
    {
        v2f o;
        o.vertex = UnityObjectToClipPos(v.vertex);
        o.uv = v.uv;
        return o;
    }
    
    float4 frag (v2f i) : SV_Target
    {
        return float4(1,0,0,1);
    }
    
    ENDHLSL
}
```

---

## 三、数据结构设计

### 3.1 类图

```
ShaderLabAsset (.shader 文件解析结果)
    │
    ├── shader_name: string
    ├── custom_editor: string
    ├── fallback: string
    │
    ├── properties: vector<ShaderProperty>
    │       ├── name: string
    │       ├── display_name: string
    │       ├── type: PropertyType (Float, Int, Color, Vector, Texture2D, Cube, 3D)
    │       ├── default_value: Variant
    │       └── attributes: vector<PropertyAttribute>
    │
    └── subshaders: vector<SubShader>
            │
            ├── lod: int
            ├── tags: map<string, string>
            │
            └── passes: vector<ShaderPass>
                    │
                    ├── name: string
                    ├── light_mode: string
                    ├── tags: map<string, string>
                    ├── render_states: RenderStates
                    │
                    └── programs: vector<ShaderProgram>
                            ├── language: ShaderLanguage (HLSL, GLSL)
                            ├── vertex_shader: string
                            ├── fragment_shader: string
                            ├── compute_shader: string (optional)
                            └── macros: vector<string>
```

### 3.2 核心数据结构

```cpp
// 属性类型
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
    FloatArray,
    VectorArray
};

// 渲染状态
struct RenderStates
{
    bool        blend_enable = false;
    BlendFunc   blend_src = BlendFunc::One;
    BlendFunc   blend_dst = BlendFunc::Zero;
    BlendOp     blend_op = BlendOp::Add;
    
    CompareFunc ztest = CompareFunc::LEqual;
    bool        zwrite = true;
    
    CullMode    cull = CullMode::Back;
    
    uint8_t     color_mask = 0xF; // RGBA
    
    StencilState stencil;
};

// SubShader
struct ShaderSubShader
{
    int                      lod;
    std::map<string, string> tags;
    std::vector<ShaderPass>  passes;
};

// Shader Pass
struct ShaderPass
{
    string                   name;
    string                   light_mode;
    std::map<string, string> tags;
    RenderStates             render_states;
    std::vector<ShaderProgram> programs;
};

// Shader Program (CGPROGRAM/HLSLPROGRAM 块)
struct ShaderProgram
{
    ShaderLanguage language;  // HLSL, GLSL
    string         source_code;
    string         vertex_entry;
    string         fragment_entry;
    std::vector<string> includes;
    std::vector<std::pair<string, string>> macros; // #pragma multi_compile 定义的宏
};

// ShaderLab 根
class ShaderLabAsset
{
public:
    string                           shader_name;
    string                           custom_editor;
    string                           fallback;
    std::vector<ShaderProperty>      properties;
    std::vector<ShaderSubShader>     subshaders;
};
```

---

## 四、解析器设计

### 4.1 解析流程

```
ShaderLab 源码 (.shader)
        │
        ▼
┌─────────────────────────────────────┐
│      ShaderLab Lexer (词法分析)       │
│  - 识别关键词: Shader, Properties...  │
│  - 识别属性定义                       │
│  - 识别代码块: HLSLPROGRAM...ENDHLSL │
└─────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────┐
│    ShaderLab Parser (语法分析)       │
│  - 构建语法树                        │
│  - 验证语法正确性                     │
│  - 提取元数据                         │
└─────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────┐
│      ShaderLab AST (抽象语法树)      │
│  - Shader → SubShaders → Passes     │
│  - Properties                       │
│  - Tags, RenderStates               │
└─────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────┐
│       ShaderCompiler (编译)          │
│  - 提取 HLSL 代码                    │
│  - 处理 #pragma 和 #include         │
│  - 编译为 SPIR-V                    │
└─────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────┐
│        RHIShaderModule               │
│   (Vulkan/DX12 可用的 Shader 模块)   │
└─────────────────────────────────────┘
```

### 4.2 词法 Token 定义

```cpp
enum class ShaderLabToken
{
    // 关键词
    SHADER,
    PROPERTIES,
    SUBshader,
    PASS,
    TAGS,
    LOD,
    FALLBACK,
    CUSTOMEDITOR,
    
    // 属性类型
    FLOAT,
    INT,
    RANGE,
    COLOR,
    VECTOR,
    TEXTURE2D,
    CUBEMAP,
    TEXTURE3D,
    FLOATARRAY,
    
    // 渲染状态
    BLEND,
    BLENDOP,
    ZWRITE,
    ZTEST,
    CULL,
    COLORMASK,
    STENCIL,
    OFFSET,
    
    // 代码块
    HLSLPROGRAM,
    GLSLPROGRAM,
    CGPROGRAM,
    ENDHLSL,
    ENDGLSL,
    ENDCG,
    
    // 符号
    LBRACE,     // {
    RBRACE,     // }
    LBRACKET,   // [
    RBRACKET,   // ]
    LPAREN,     // (
    RPAREN,     // )
    COMMA,      // ,
    EQUAL,      // =
    SEMICOLON,  // ;
    
    // 字面量
    STRING,     // "..."
    NUMBER,     // 1.0, 2, 3.14
    IDENTIFIER, // _MainTex, _Color
    
    // 特殊
    NEWLINE,
    COMMENT,
    HLSLCODE,   // HLSLPROGRAM 和 ENDHLSL 之间的代码
    GLSLCODE,   // GLSLPROGRAM 和 ENDGLSL 之间的代码
    CGCODE,     // CGPROGRAM 和 ENDCG 之间的代码
    
    END,
    ERROR
};
```

---

## 五、编译集成

### 5.1 与现有 ShaderCompiler 的集成

```cpp
class ShaderLabCompiler
{
public:
    // 从 .shader 文件编译
    bool compileFromFile(const string& file_path);
    
    // 从源码编译
    bool compileFromSource(const string& source);
    
    // 获取编译结果
    const ShaderLabAsset* getShaderLabAsset() const { return m_shader_lab_asset.get(); }
    
    // 获取特定 Pass 的编译后 Shader
    RHIShaderModule* getCompiledShader(size_t subshader_index, 
                                        size_t pass_index,
                                        const ShaderMacros& macros);
    
private:
    std::unique_ptr<ShaderLabAsset>  m_shader_lab_asset;
    std::unique_ptr<ShaderLabParser> m_parser;
    std::unique_ptr<ShaderCompiler>  m_shader_compiler;  // 使用现有的 glslang 编译器
};
```

### 5.2 Shader 变体编译

```cpp
// 变体编译流程
ShaderVariantCollection compileVariants(const ShaderLabAsset& asset,
                                        const vector<string>& keywords)
{
    ShaderVariantCollection variants;
    
    for (const auto& subshader : asset.subshaders)
    {
        for (const auto& pass : subshader.passes)
        {
            for (const auto& program : pass.programs)
            {
                // 提取 HLSL 代码
                string hlsl_code = program.source_code;
                
                // 对每个变体组合编译
                for (const auto& variant_keywords : generateKeywordCombinations(keywords))
                {
                    // 构建宏定义
                    ShaderMacros macros;
                    for (const auto& kw : variant_keywords)
                    {
                        macros[kw] = "1";
                    }
                    
                    // 编译
                    auto result = compileHLSLToSPIRV(hlsl_code, macros);
                    
                    if (result.success)
                    {
                        variants.add(program.entry_point, variant_keywords, result.spirv);
                    }
                }
            }
        }
    }
    
    return variants;
}
```

---

## 六、实现计划

### 6.1 目录结构

```
engine/Source/Runtime/Function/ShaderLab/
├── CMakeLists.txt
├── shader_lab_asset.h           # ShaderLab 资源数据结构
├── shader_lab_asset.cpp
├── shader_lab_lexer.h           # 词法分析器
├── shader_lab_lexer.cpp
├── shader_lab_parser.h          # 语法分析器
├── shader_lab_parser.cpp
├── shader_lab_compiler.h        # 编译器入口
├── shader_lab_compiler.cpp
└── shader_lab_preprocessor.h    # #pragma 处理
    shader_lab_preprocessor.cpp
```

### 6.2 优先级

| 阶段 | 功能 | 优先级 |
|------|------|--------|
| 1 | ShaderLab 词法分析器 | 高 |
| 1 | ShaderLab 语法分析器 | 高 |
| 2 | 属性（Properties）解析 | 高 |
| 2 | Pass 和 SubShader 解析 | 高 |
| 3 | HLSLPROGRAM 代码块提取 | 高 |
| 3 | 与现有 ShaderCompiler 集成 | 高 |
| 4 | #pragma multi_compile 支持 | 中 |
| 4 | 渲染状态解析 | 中 |
| 5 | Shader 变体缓存系统 | 中 |
| 6 | Shader GUI/Inspector | 低 |

---

## 七、示例

### 7.1 标准延迟渲染 Shader (.shader)

```shaderlab
Shader "ZEngine/Standard"
{
    Properties
    {
        _BaseColor ("Base Color", Color) = (1, 1, 1, 1)
        _BaseMap ("Base Map", 2D) = "white" {}
        _Metallic ("Metallic", Range(0, 1)) = 0
        _Smoothness ("Smoothness", Range(0, 1)) = 0.5
        _NormalMap ("Normal Map", 2D) = "bump" {}
        _NormalScale ("Normal Scale", Range(0, 1)) = 1
        _OcclusionMap ("Occlusion Map", 2D) = "white" {}
        _EmissionColor ("Emission Color", Color) = (0, 0, 0, 1)
        _EmissionMap ("Emission Map", 2D) = "white" {}
        _Cutoff ("Cutoff", Range(0, 1)) = 0
    }
    
    SubShader
    {
        Tags 
        { 
            "RenderPipeline" = "Deferred"
            "Queue" = "Geometry"
            "RenderType" = "Opaque"
        }
        LOD 100
        
        // GBuffer Pass
        Pass
        {
            Name "GBuffer"
            Tags { "LightMode" = "GBuffer" }
            
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile _ LIGHTMAP_ON
            #pragma multi_compile _ DIRLIGHTMAP_COMBINED
            #pragma multi_compile _ SHADOWS_SHADOWMASK
            #pragma multi_compile_fragment _ SHADOWS_SOFT
            
            #include "shader/include/common.h"
            #include "shader/include/gbuffer.h"
            
            CBUFFER_START(UnityPerMaterial)
                float4 _BaseColor;
                float4 _BaseMap_ST;
                float  _Metallic;
                float  _Smoothness;
                float  _NormalScale;
                float4 _EmissionColor;
                float  _Cutoff;
            CBUFFER_END
            
            struct appdata
            {
                float4 position : POSITION;
                float3 normal : NORMAL;
                float4 tangent : TANGENT;
                float2 uv : TEXCOORD0;
                float2 uv1 : TEXCOORD1;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            
            struct v2f
            {
                float4 position : SV_POSITION;
                float2 uv : TEXCOORD0;
                float3 normal : TEXCOORD1;
                float3 tangent : TEXCOORD2;
                float3 bitangent : TEXCOORD3;
                UNITY_VERTEX_INPUT_INSTANCE_ID
                UNITY_VERTEX_OUTPUT_STEREO
            };
            
            v2f vert(appdata input)
            {
                v2f output;
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_TRANSFER_INSTANCE_ID(input, output);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);
                
                output.position = TransformObjectToHClip(input.position.xyz);
                output.uv = TRANSFORM_TEX(input.uv, _BaseMap);
                output.normal = TransformObjectToWorldNormal(input.normal);
                output.tangent = TransformObjectToWorldDir(input.tangent.xyz);
                output.bitangent = cross(output.normal, output.tangent) * input.tangent.w;
                
                return output;
            }
            
            void frag(v2f input, out GBufferOutput output)
            {
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);
                
                float4 base_color = SAMPLE_TEXTURE2D(_BaseMap, input.uv) * _BaseColor;
                
                #if defined(_ALPHATEST_ON)
                    clip(base_color.a - _Cutoff);
                #endif
                
                output.GBuffer0 = float4(base_color.rgb, _Metallic);
                output.GBuffer1 = float4(_Smoothness, 0, 0, 0);
                output.GBuffer2 = float4(input.normal * 0.5 + 0.5, 1);
                output.GBuffer3 = _EmissionColor.rgb;
            }
            ENDHLSL
        }
        
        // Forward Lighting Pass (for transparent objects)
        Pass
        {
            Name "ForwardLit"
            Tags { "LightMode" = "ForwardLit" }
            
            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite Off
            Cull Off
            
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile _ LIGHTMAP_ON
            
            #include "shader/include/common.h"
            
            // ... similar to GBuffer but for transparent rendering
            ENDHLSL
        }
    }
    
    FallBack "ZEngine/Diffuse"
}
```

---

## 八、与现有系统的集成

### 8.1 文件格式

`.shader` 文件用于编辑器开发和源码形式存在，编译后会生成 `.shader.bin` 缓存文件。

### 8.2 Asset Pipeline 集成

```cpp
class ShaderLabImporter : public AssetImporter
{
public:
    bool canImport(const std::filesystem::path& file_path) const override
    {
        return file_path.extension() == ".shader";
    }
    
    bool import(const std::filesystem::path& source_path,
                const std::filesystem::path& output_path,
                const AssetImporterSettings& import_settings,
                AssetMetadata& out_metadata) override
    {
        // 1. 解析 ShaderLab 源码
        ShaderLabCompiler compiler;
        if (!compiler.compileFromFile(source_path.string()))
        {
            LOG_ERROR("Failed to compile shader: {}", source_path);
            return false;
        }
        
        // 2. 生成 ShaderLabAsset
        auto shader_asset = compiler.getShaderLabAsset();
        
        // 3. 预编译所有变体
        compileAllVariants(shader_asset);
        
        // 4. 保存到输出路径
        return serializeShaderAsset(output_path, shader_asset);
    }
};
```

### 8.3 材质系统集成

```cpp
class Material
{
public:
    void setShader(const string& shader_name)
    {
        // 加载对应的 ShaderLabAsset
        m_shader_asset = ResourceManager::load<ShaderLabAsset>(shader_name);
        
        // 初始化属性
        for (const auto& prop : m_shader_asset->properties)
        {
            setPropertyDefault(prop);
        }
    }
    
    void setProperty(const string& name, const Variant& value)
    {
        m_properties[name] = value;
    }
    
    void bind()
    {
        // 根据当前属性值构建 ShaderMacros
        ShaderMacros macros;
        for (const auto& [name, value] : m_properties)
        {
            if (value.isKeyword())
            {
                macros[value.asKeyword()] = "1";
            }
        }
        
        // 获取对应变体的 Shader
        auto shader = m_shader_asset->getVariant(macros);
        
        // 绑定到 RHI
        shader->bind();
        
        // 设置 Uniform
        for (const auto& [name, value] : m_properties)
        {
            shader->setUniform(name, value);
        }
    }
};
```

---

## 九、总结

ZEngine ShaderLab 的实现将：

1. **提供 Unity 风格的 Shader 编写体验**
   - 声明式的 Shader 结构
   - 直观的属性定义
   - 灵活的 Pass 配置

2. **保持高性能**
   - 利用现有 glslang 编译器
   - 支持变体缓存
   - 延迟编译

3. **与现代渲染管线集成**
   - 延迟渲染优先
   - 支持前向渲染
   - 可扩展的 Pass 类型

---

*文档版本: 1.0*
*最后更新: 2026-05-06*

