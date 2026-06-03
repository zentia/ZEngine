# ZEngine 多 API Shader 适配方案（按当前代码现状修订）

## 一、先回答核心问题

### 1. Vulkan 或 Metal 怎么办？

**结论：开发时可以继续维护源码，打包和运行时优先使用目标后端自己的编译产物，而不是直接依赖源码。**

对应关系应当是：

- **DX12**：`HLSL -> DXIL`
- **Vulkan**：`HLSL/GLSL -> SPIR-V`
- **Metal**：`MSL -> metallib`（或等价的 Metal 可加载产物）

也就是说：

- `HLSL / GLSL / MSL` 是**源码层**
- `DXIL / SPIR-V / metallib` 是**打包产物层 / 运行时加载层**

**Shipping 包中建议不再依赖 HLSL 文本源码作为唯一运行时输入。**
开发模式可以保留源码回退编译；发布模式应优先加载预编译二进制。

---

## 二、当前仓库里的真实现状

### 1. `Shader.zasset` 当前只是元数据容器

当前 `ShaderRes` 只有这几个字段：

- `m_shader_name`
- `m_vertex_shader_file`
- `m_fragment_shader_file`
- `m_render_pipeline`

这意味着当前 `Shader.zasset` 还没有真正描述：

- 多后端目标
- 每个 stage 的 entry point
- 已编译产物位置
- 变体信息
- 反射信息

所以它现在更像是：**源码引用资产**，不是完整的跨平台 shader 包。

### 2. Vulkan 已经有一套较完整的内建链路

当前项目中：

- `VulkanRHI` 已有 shader module 创建逻辑
- `engine/shader/glsl/` + `glslangValidator` 会生成 `SPIR-V`
- 某些 pass 直接包含生成后的 C++ 头文件并创建 shader module

这说明 **Vulkan 当前主力链路是 `GLSL -> SPIR-V`**，而不是 `Shader.zasset -> HLSL` 这条新资产线。

### 3. DX12 已经有 `HLSL -> DXIL`

当前项目中：

- `DX12ShaderCompiler` 已经存在
- Editor 新建 `Shader` 资产时会生成 `.vert.hlsl` 和 `.frag.hlsl`
- Inspector 中已经可以做 DX12 编译校验

这说明 **自定义 Shader 资产链目前最明确打通的是 DX12/HLSL 方向**。

### 4. Metal 目前仍是骨架

当前项目中：

- 有 `MetalRHI` 类型与平台接线
- 但 shader module / graphics pipeline 等核心接口仍大量未完成
- 仓库中没有完整的项目级 `.metal` / `metallib` 生产链

所以当前不能把 Metal 看成“已经具备可用 shader 资产工作流”。

---

## 三、最小可落地原则

### 1. 不把 `Shader.zasset` 直接做成“巨大的二进制包”

**推荐：`Shader.zasset` 只保存作者关心的元数据；编译产物放到派生产物目录或打包产物中。**

原因：

- 源码资产更稳定、可读、可合并
- 二进制产物易失效，适合缓存 / cook 输出
- 不同平台的产物不同，不应该强耦合到同一个手工编辑资产文件里

### 2. 明确分三层

#### 源码层（Source of Truth）

由项目作者维护：

- `Shader.zasset`
- `*.vert.hlsl`
- `*.frag.hlsl`
- `#include` 头文件
- 变体定义

#### 派生产物层（Cook / Cache）

构建系统或编辑器自动生成：

- DX12 用的 `DXIL`
- Vulkan 用的 `SPIR-V`
- Metal 用的 `metallib`（未来）
- 可选反射结果、资源绑定表、变体索引

#### 运行时层（Runtime Load）

运行时只做：

- 根据当前 `GraphicsAPI` 选择目标产物
- 加载对应 stage 的字节码
- 创建 pipeline / shader module
- 在开发模式缺失产物时，才允许回退到源码编译

---

## 四、ZEngine 的最小跨平台方案

### 方案目标

**先把“统一资产入口 + 后端分开产物 + 运行时按 API 选产物”做通。**

不在第一阶段做：

- 自定义 DSL
- 全自动材质图转 shader
- 完整变体剔除系统
- 通用 Metal 反射工具链

### 方案选型

#### 推荐统一作者语言：`HLSL`

对于“项目自定义 shader 资产”这一条线，建议：

- 作者主要写 `HLSL`
- `DX12`：编译成 `DXIL`
- `Vulkan`：通过 `DXC -spirv` 编译成 `SPIR-V`
- `Metal`：后续单独接入 `HLSL -> MSL -> metallib`，或直接维护 `MSL` 分支

### 为什么是最小方案

因为当前仓库里：

- DX12 的 `HLSL` 工具链已存在
- Vulkan 已经接受 `SPIR-V`
- `Shader.zasset` 新链路也已经围绕 `HLSL` 开始建立

因此最经济的推进方式不是再新造一层 DSL，而是：

**把 HLSL 作为“自定义资产 shader”的统一作者语言，把运行时输入统一变成各 API 的二进制产物。**

---

## 五、建议的资产职责划分

### 1. `Shader.zasset` 负责什么

建议它只存：

- shader 名称
- stage 源码路径
- 目标后端开关
- render pipeline 名称
- entry point
- include 根目录
- 变体声明
- debug 选项

**不要直接内嵌大块 DXIL / SPIR-V / metallib。**

### 2. 新增“编译清单”概念

最小实现里，不一定非要新增新的 Object 类型；也可以先生成 sidecar manifest。

例如每个 shader 生成一个清单文件，记录：

- 当前源码哈希
- include 哈希
- 宏组合哈希
- 目标 API
- 各 stage 编译结果路径
- 编译错误信息
- 最后生成时间

### 3. 编译产物存放建议

开发期建议放在非源码目录，例如：

```text
Library/ShaderCache/<shader-hash>/dx12/vertex.dxil
Library/ShaderCache/<shader-hash>/dx12/fragment.dxil
Library/ShaderCache/<shader-hash>/vulkan/vertex.spv
Library/ShaderCache/<shader-hash>/vulkan/fragment.spv
Library/ShaderCache/<shader-hash>/metal/vertex.metallib
Library/ShaderCache/<shader-hash>/manifest.json
```

打包期再把需要的产物收集进平台包，例如：

```text
Packaged/Shaders/<shader-guid>/<api>/...
```

---

## 六、建议的最小数据模型

### 1. 作者资产字段（建议）

可在未来把 `ShaderRes` 扩为类似含义：

- `m_shader_name`
- `m_vertex_shader_file`
- `m_fragment_shader_file`
- `m_render_pipeline`
- `m_source_language`（初期固定为 `HLSL`）
- `m_vertex_entry`（默认 `main`）
- `m_fragment_entry`（默认 `main`）
- `m_enable_dx12`
- `m_enable_vulkan`
- `m_enable_metal`
- `m_include_directories`
- `m_variant_keywords`

### 2. 派生产物字段（建议）

不一定放进 `ShaderRes`，也可以在缓存清单里：

- `source_hash`
- `variant_hash`
- `dx12_vertex_dxil`
- `dx12_fragment_dxil`
- `vulkan_vertex_spv`
- `vulkan_fragment_spv`
- `metal_vertex_metallib`
- `metal_fragment_metallib`
- `reflection_blob`

---

## 七、最小工作流

### 开发态

```text
创建 Shader.zasset
-> 编辑 HLSL 源码
-> Inspector 触发编译
-> 为启用的 API 生成派生产物
-> 运行时优先加载派生产物
-> 若处于编辑器且产物缺失，可回退源码编译
```

### 打包态

```text
扫描所有被引用 Shader.zasset
-> 为目标平台批量 cook
-> 只收集目标平台需要的编译产物
-> 运行包内只带目标后端的 shader 二进制
-> 运行时不依赖 HLSL 文本
```

### 这就直接回答了“打包后还走不走 HLSL”

**推荐答案是：开发时走 HLSL，打包后优先不走 HLSL，而走已编译好的目标后端二进制。**

- Windows DX12 包：带 `DXIL`
- Vulkan 包：带 `SPIR-V`
- macOS / iOS Metal 包：带 `metallib`

---

## 八、与当前代码最兼容的过渡策略

### 阶段 0：保持现有 Vulkan 内建链不动

当前 `engine/shader/glsl/` 和已有内建 pass 不要马上推倒重做。

理由：

- 它已经能工作
- 当前自定义 `Shader.zasset` 链路还没进入完整 runtime pipeline
- 贸然统一会扩大改动面

### 阶段 1：只统一“项目自定义 Shader 资产”

即：

- `Assets/Shaders/*.vert.hlsl`
- `Assets/Shaders/*.frag.hlsl`
- `Shader.zasset`
- 编辑器编译 / 校验 / cook
- 运行时按 API 选产物

这一步做完后，项目资产层就是统一入口了。

### 阶段 2：让 Vulkan 也消费自定义资产产出的 `SPIR-V`

这一步的目标不是删除现有 GLSL，而是让**资产化 shader** 在 Vulkan 上也能跑。

### 阶段 3：再考虑内建 GLSL pass 迁移

等资产工作流稳定后，再决定是否：

- 保留内建 GLSL
- 迁到统一 HLSL 源
- 或者把内建 pass 也 cook 成统一产物

---

## 九、Metal 的最小策略

### 当前不要强行承诺“统一即刻支持 Metal”

因为当前项目里 Metal RHI 本身还没完全可用。

### 正确做法

先在资产模型里把 Metal 作为**目标后端占位能力**预留：

- 有 `m_enable_metal`
- 有 Metal 产物路径槽位
- 有 manifest 结构

但在实现阶段明确分层：

1. **先完成 DX12 / Vulkan** 的统一资产编译产物链
2. **再接 Metal RHI 的 shader module / pipeline**
3. **最后接 Metal shader 编译链**

这样不会阻塞前两端落地。

---

## 十、建议的目录组织

### 作者目录

```text
Assets/Shaders/
  MyLit.zasset
  MyLit.vert.hlsl
  MyLit.frag.hlsl
  Include/
    Common.hlsli
    Lighting.hlsli
```

### 派生产物目录

```text
Library/ShaderCache/
  <hash>/
    manifest.json
    dx12/
      vertex.dxil
      fragment.dxil
    vulkan/
      vertex.spv
      fragment.spv
    metal/
      vertex.metallib
      fragment.metallib
```

### 打包目录

```text
Packaged/Shaders/
  <shader-guid>/
    dx12/...
    vulkan/...
    metal/...
```

---

## 十一、运行时最小选择逻辑

### 伪流程

```text
读取 Material -> Shader.zasset
-> 获取当前 GraphicsAPI
-> 查找该 API 对应的已编译产物
-> 成功则直接创建 shader module / pipeline
-> 失败且处于编辑器开发模式，则尝试源码编译
-> Shipping 模式下直接报错，不再依赖源码救场
```

这样做的优点：

- 开发期体验不差
- 发布期稳定、启动快
- 平台边界清晰
- 便于后续做缓存和热重载

---

## 十二、最小实施顺序（建议按这个来）

### 第一步：把 `ShaderRes` 变成“跨平台作者资产”

先补齐：

- 语言类型
- entry point
- 目标 API 开关
- include 路径
- 变体关键字

### 第二步：增加 shader cook / cache 产物层

先不碰复杂打包系统，先做：

- 手动编译
- 自动写入 `Library/ShaderCache`
- 为每个 API 生成 manifest

### 第三步：运行时从“源码引用”切换为“产物优先”

让 DX12 / Vulkan 都先走：

- 找编译产物
- 找不到再回退源码编译（仅开发态）

### 第四步：再补 Shader 反射和资源绑定

这一步解决：

- 常量缓冲布局
- 纹理 / sampler 绑定槽
- 跨 API descriptor 映射

### 第五步：最后才是 Metal

先把 RHI 能力补齐，再接编译链，不要倒序做。

---

## 十三、最终建议

### 最重要的结论

**对 ZEngine 当前状态来说，最小、最稳、最符合现状的方案是：**

- **作者层统一到 `Shader.zasset + HLSL`**（至少先覆盖自定义资产 shader）
- **构建 / Inspector / Cook 层按后端生成二进制产物**
- **运行时按 `GraphicsAPI` 只加载目标后端产物**
- **Shipping 包不再依赖 HLSL 文本作为唯一运行时输入**

### 一句话版本

**开发时写源码，打包后吃二进制；统一的是资产入口，不是最终 GPU 字节码。**
