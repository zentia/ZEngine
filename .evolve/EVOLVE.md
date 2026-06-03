# ZEngine 演进记录

## 2026-05-06: ShaderLab 实现

### 完成的工作

实现了 ZEngine 的 ShaderLab 系统，参考 Unity 的 ShaderLab 架构。

### 实现内容

#### 1. ShaderLab 核心模块
- **目录**: `engine/Source/Runtime/Function/ShaderLab/`
- **文件**:
  - `shader_lab_asset.h/cpp` - ShaderLab 资源数据结构
  - `shader_lab_lexer.h/cpp` - 词法分析器
  - `shader_lab_parser.h/cpp` - 语法分析器
  - `shader_lab_compiler.h/cpp` - 编译器入口
  - `CMakeLists.txt` - 构建配置
  - `shader_lab_test.cpp` - 测试程序

#### 2. 支持的 ShaderLab 语法
- `Shader` - 根定义
- `Properties` - 属性定义（Float, Int, Range, Color, Vector, Texture2D, Cube, 3D）
- `SubShader` - 子着色器，支持 LOD 和 Tags
- `Pass` - 渲染通道
- `Tags` - 标签（RenderPipeline, Queue, LightMode 等）
- 渲染状态（Cull, ZWrite, ZTest, Blend, ColorMask, Offset）
- `HLSLPROGRAM/ENDHLSL` - HLSL 代码块
- `GLSLPROGRAM/ENDGLSL` - GLSL 代码块
- `CGPROGRAM/ENDCG` - CG 代码块
- `FallBack` - 后备着色器

#### 3. 设计文档
- `doc/SHADER_LAB_DESIGN.md` - 详细的 ShaderLab 设计文档

#### 4. 示例文件
- `engine/shader/example_standard.zshader` - 标准 PBR Shader 示例

### 技术架构

```
ShaderLab 源码 (.zshader)
        │
        ▼
┌─────────────────────────┐
│   ShaderLabLexer        │  词法分析
│   - Token 识别          │
│   - 代码块提取          │
└─────────────────────────┘
        │
        ▼
┌─────────────────────────┐
│   ShaderLabParser      │  语法分析
│   - AST 构建           │
│   - 属性解析            │
│   - Pass 解析           │
└─────────────────────────┘
        │
        ▼
┌─────────────────────────┐
│   ShaderLabAsset       │  中间表示
│   - Shader             │
│   - SubShader          │
│   - Pass               │
│   - Properties         │
└─────────────────────────┘
        │
        ▼
┌─────────────────────────┐
│   ShaderLabCompiler     │  编译集成
│   - #pragma 提取       │
│   - #include 处理       │
│   - 变体生成            │
└─────────────────────────┘
        │
        ▼
┌─────────────────────────┐
│   ShaderCompiler        │  SPIR-V 编译
│   (使用 glslang)        │
└─────────────────────────┘
```

### 与现有系统集成

1. **渲染系统集成** - 使用现有的 `ShaderCompiler` (glslang)
2. **资产系统集成** - `ShaderLabAsset` 支持序列化
3. **材质系统** - 可扩展支持 ShaderLab 风格的材质

### 下一步计划

1. 将 ShaderLab 集成到资产导入管道
2. 实现 Shader GUI/Inspector
3. 完善变体系统
4. 添加更多 Shader 语法支持

### 参考

- Unity ShaderLab 源码: `../unity2023.1/Runtime/Shaders/`
- Unity ShaderParser: `../unity2023.1/Editor/Src/AssetPipeline/ShaderImporting/ShaderParser/`
