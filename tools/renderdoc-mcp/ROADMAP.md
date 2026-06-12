# RenderDoc MCP 优化路线图

> 基于一次完整的 RDC 逆向分析实战（王者荣耀水晶尾巴材质）暴露的痛点，整理的改进计划。
>
> **v0.2.0 更新**: 13 个痛点中已完成 11 个（问题 2-7, 9-13），新增 8 个工具（23→31），详见下方 ✅ 标记。
>
> **v0.3.0 更新**: 基于工具设计文档对比补全——新增 11 个工具（31→42），新增 2 个模块（performance_tools, diagnostic_tools），多个现有工具增强，详见下方 v0.3 标记。

---

## 一、实战复盘：问题总结

### 场景

从一个 OpenGL ES 手游 RDC 帧捕获中，逆向分析水晶尾巴的渲染技术：
- 定位目标 draw call（从 266 个 draw 中找到 EID 2480/2530）
- 分析混合模式、纹理绑定、shader 结构
- 提取纹理资产
- 推断 shader 逻辑
- 生成 UE5 复刻方案

### 结果

MCP 完全没用上，全程手写 Python 脚本，撞了大量 API 错误，效率很低。

---

## 二、问题分类

### P0: 根本性问题（导致 MCP 完全不可用）

#### 问题 1: MCP 只在项目级别配置，换个目录就用不了

**现状**: `.mcp.json` 只配在 `F:/RenderDoc/`，在 `C:\Users\18523` 下工作时完全不可用。

**改进**:
- [ ] 在 README 中推荐 User 级别配置方式
- [ ] 提供一键安装脚本，自动写入 `~/.claude/settings.json`

---

### P1: 高频痛点（每次分析都会遇到）

#### 问题 2: 缺少「一键分析 draw call」的高层工具

**现状**: 分析一个 draw call 需要：
1. `set_event(eid)`
2. `get_pipeline_state()` — 拿混合/深度/剔除
3. `get_shader_bindings("fragment")` — 拿纹理绑定
4. `get_shader_reflection("fragment")` — 拿纹理名
5. 手动关联纹理 ID → 纹理尺寸/格式

AI 需要调用 4-5 个工具才能回答「这个 draw call 用了什么纹理」。

**改进**:
- [x] **新增工具: `get_draw_call_state(event_id)`** — 一次调用返回完整状态：✅ `pipeline_tools.py`

```json
{
  "event_id": 2530,
  "action_name": "glDrawElements()",
  "vertex_count": 8808,
  "instance_count": 1,
  "blend": {
    "enabled": true,
    "color_src": "SrcAlpha",
    "color_dst": "SrcColor",
    "alpha_src": "SrcAlpha",
    "alpha_dst": "SrcColor",
    "equation": "result = src.a * src.rgb + src.rgb * dst.rgb"
  },
  "depth": {"test": true, "write": false, "func": "LessEqual"},
  "cull": "Back",
  "two_sided": false,
  "textures": [
    {"slot": 0, "name": "_BumpMap", "id": "ResourceId::28", "size": "4x4", "format": "R8G8B8A8_UNORM"},
    {"slot": 1, "name": "_DiamondMap", "id": "ResourceId::3139", "size": "512x512", "format": "ASTC_UNORM"}
  ],
  "shader": {
    "vertex_id": "ResourceId::3406",
    "fragment_id": "ResourceId::3407",
    "vertex_inputs": ["POSITION", "NORMAL", "TANGENT", "TEXCOORD0", "TEXCOORD1"]
  }
}
```

- [x] **新增工具: `get_frame_overview()`** — 帧概览：✅ `session_tools.py`

```json
{
  "api": "OpenGL ES",
  "gpu": "...",
  "resolution": "1496x720",
  "total_actions": 285,
  "draw_calls": 266,
  "dispatches": 0,
  "clears": 18,
  "copies": 0,
  "textures": 172,
  "buffers": 141,
  "render_passes": [
    {"name": "Shadow", "draws": 23, "rt": "187x90 D16"},
    {"name": "Main Scene", "draws": 150, "rt": "1496x720 R8G8B8A8 + D24S8"},
    {"name": "Post Process", "draws": 9, "rt": "1496x720"},
    {"name": "UI", "draws": 60, "rt": "1496x720"}
  ]
}
```

---

#### 问题 3: 混合模式显示为裸数字

**现状**: `get_pipeline_state()` 返回的混合因子是枚举整数 (`6`, `7`, `2`)，AI 和人类都无法直接理解。

**改进**:
- [x] `pipeline_tools.py` 中添加 BlendMultiplier 枚举映射表 ✅ `util.py` BLEND_FACTOR_MAP + enum_str()
- [x] 输出改为可读字符串: `"SrcAlpha"`, `"InvSrcAlpha"`, `"SrcColor"` 等 ✅
- [x] 额外生成混合公式文本: `blend_formula()` ✅ 同时应用于 depth func, cull mode, fill mode, topology

映射表:
```python
BLEND_FACTOR_MAP = {
    0: "Zero",
    1: "One",
    2: "SrcColor",
    3: "InvSrcColor",
    4: "DstColor",
    5: "InvDstColor",
    6: "SrcAlpha",
    7: "InvSrcAlpha",
    8: "DstAlpha",
    9: "InvDstAlpha",
    10: "SrcAlphaSat",      # GL_SRC_ALPHA_SATURATE
    11: "BlendFactor",
    12: "InvBlendFactor",
    13: "Src1Color",
    14: "InvSrc1Color",
    15: "Src1Alpha",
    16: "InvSrc1Alpha",
}
```

---

#### 问题 4: 缺少 draw call 对比工具

**现状**: 水晶尾巴用了两 pass（EID 2480 opaque + EID 2530 alpha），需要手动对比两个 draw 的状态差异。对于 10 层 alpha pass 的情况更是噩梦。

**改进**:
- [x] **新增工具: `diff_draw_calls(eid1, eid2)`** — 自动对比两个 draw call 的状态差异：✅ `advanced_tools.py`

```json
{
  "eid1": 2480,
  "eid2": 2530,
  "diff": {
    "blend": {"eid1": "opaque", "eid2": "SrcAlpha + SrcColor"},
    "depth_write": {"eid1": true, "eid2": false},
    "cull": {"eid1": "Back", "eid2": "Back"},
    "fragment_shader": {"eid1": "ResourceId::3404", "eid2": "ResourceId::3407"},
    "textures": {
      "added": [
        {"slot": 1, "name": "_DiamondMap"},
        {"slot": 4, "name": "_FlowMap"}
      ],
      "removed": [],
      "changed": [
        {"slot": 0, "name": "_DissolveMap -> _BumpMap"}
      ]
    },
    "vertex_count": "same (8808)"
  }
}
```

---

#### 问题 5: 批量纹理导出不便

**现状**: 导出一个 draw call 的所有纹理，需要先 `get_shader_bindings` 拿 resource ID，再逐个调用 `save_texture`。对于有 10 张纹理的 draw call，AI 需要调用 11 次工具。

**改进**:
- [x] **新增工具: `export_draw_textures(event_id, output_dir)`** — 一次调用导出所有绑定纹理：✅ `data_tools.py`

```json
{
  "event_id": 2530,
  "output_dir": "C:/output/eid2530_textures/",
  "exported": [
    {"name": "_DiamondMap", "file": "DiamondMap_512x512.png", "size": "512x512"},
    {"name": "_MainTex", "file": "MainTex_1024x1024.png", "size": "1024x1024"}
  ]
}
```

- [x] 文件自动命名: `{uniform_name}_{width}x{height}.png` ✅
- [x] 跳过占位纹理（4x4 或更小，`skip_small` 参数）✅

---

#### 问题 6: 渲染目标快照不便

**现状**: 要看某个 draw call 画了什么，需要手动用 `save_texture` 保存 RT，再用文件查看器打开。

**改进**:
- [x] **新增工具: `save_render_target(event_id, output_path)`** — 保存当前 draw call 执行后的 RT 状态 ✅ `data_tools.py`
- [x] 自动识别当前绑定的 color RT + depth RT ✅
- [x] 可选参数: `save_depth=true` 同时保存深度图 ✅

---

### P2: 中频痛点（特定分析场景需要）

#### 问题 7: Shader 源码/反编译不可靠

**现状**:
- `disassemble_shader` 对 GLES shader 的 SPIR-V 反编译失败
- 没有 fallback 机制去获取原始 GLSL 源码
- 失败时只返回错误信息，没有替代方案

**改进**:
- [x] 自动尝试所有可用 disassembly target，返回第一个成功的结果 ✅ `shader_tools.py`
- [ ] 对 GL/GLES capture，从 structured file 中提取原始 GLSL source（待实现）
- [x] 如果全部反编译失败，自动 fallback 返回 shader reflection（inputs/outputs/uniforms/resources）✅
- [x] 返回结果中标明: `"source_type": "disasm" | "reflection_only"` ✅

---

#### 问题 8: Uniform 值读取失败

**现状**: `get_cbuffer_contents` 在这次分析中完全无法工作（API 参数不匹配）。我们无法读取实际的 uniform 值来对比各 pass 的差异。

**现有代码** (`shader_tools.py`):
```python
# 当前实现可能对新版 RenderDoc API 不兼容
vars = controller.GetCBufferVariableContents(
    pipe.GetGraphicsPipelineObject(),
    pipe.GetShader(stage_enum),
    pipe.GetShaderEntryPoint(stage_enum),
    cbuffer_index,
    cbuf.descriptor.resource,
    cbuf.descriptor.byteOffset,
    cbuf.descriptor.byteSize,
    0
)
```

**改进**:
- [ ] 适配多个 RenderDoc API 版本的参数签名（try/except fallback）
- [ ] 对 GL/GLES，尝试直接从 GL uniform location 读取
- [ ] 新增 `get_uniform_values(event_id, stage)` 高层工具，返回 `{"name": value}` 字典
- [ ] 值格式化: float4 显示为 `[1.0, 0.5, 0.3, 1.0]`，matrix 显示为 `[mat4x4]`

---

#### 问题 9: 缺少按条件搜索 draw call 的能力

**现状**: `search_actions` 只支持按名称和 flag 搜索。但实战中最常见的需求是：
- 「找出所有使用 alpha blend 的 draw call」
- 「找出顶点数 > 5000 的 draw call」
- 「找出绑定了某张纹理的 draw call」

**改进**:
- [x] **新增工具: `find_draws(filter)`** — 按渲染状态搜索：✅ `event_tools.py`

```python
find_draws(
    blend="not_opaque",           # 所有非不透明
    min_vertices=5000,            # 顶点数过滤
    texture_name="_DiamondMap",   # 使用特定纹理
    shader_id="ResourceId::3407"  # 使用特定 shader
)
```

注意: 这个工具需要遍历所有 draw call 并检查每个的状态，可能较慢。应提前告知用户。

---

#### 问题 10: 缺少自动 render pass 分段

**现状**: 需要手动通过 Clear/Invalidate 调用来判断 render pass 边界。

**改进**:
- [x] **新增工具: `analyze_render_passes()`** — 自动检测 render pass 边界并汇总：✅ `advanced_tools.py`

```json
{
  "passes": [
    {
      "index": 0,
      "start_eid": 19,
      "end_eid": 506,
      "draw_count": 23,
      "render_target": {"color": "187x90 R8G8B8A8", "depth": "187x90 D16"},
      "clear_color": [0, 0, 0, 1],
      "description": "Shadow Map"
    }
  ]
}
```

检测逻辑:
- 通过 `glClear` / `glInvalidateFramebuffer` / `SetRenderTarget` 等调用作为边界
- 检测 RT 切换（framebuffer 变化）
- 汇总每段的 draw count 和 RT 信息

---

### P3: 低频但值得做

#### 问题 11: pixel_history 的 `passed` 字段逻辑反转 (BUG)

**现有代码** (`advanced_tools.py` line 63):
```python
"passed": not mod.Passed(),  # BUG: 逻辑反转了
```

**修复**:
- [x] 改为 `"passed": mod.Passed()` ✅ 已修复

---

#### 问题 12: 资源 ID 查找效率低

**现状**: `_parse_resource_id()` 每次都遍历所有 textures + buffers + resources。

**改进**:
- [x] 在 `session.open()` 时构建 resource ID → descriptor 的 dict 缓存 ✅ `_resource_id_cache` + `_texture_desc_cache`
- [x] 后续查找 O(1) ✅ `resolve_resource_id()` + `get_texture_desc()`，所有模块已迁移

---

#### 问题 13: 缺少 mesh 导出功能

**现状**: 无法将 draw call 的 mesh 导出为 OBJ/FBX。

**改进**:
- [x] **新增工具: `export_mesh(event_id, output_path)`** ✅ `data_tools.py`
- [x] 使用 `get_post_vs_data` 获取顶点数据，转为 OBJ 格式 ✅
- [x] 支持: position, normal, uv ✅ 自动检测 output signature 中的语义

---

## 三、优先级排序

### 第一批（解决「能不能用」的问题）

| # | 改进 | 工作量 | 影响 | 状态 |
|---|------|--------|------|------|
| 1 | 添加 User 级别配置说明 + 安装脚本 | 小 | 解决 MCP 不可用的根本问题 | 待做 |
| 3 | 混合模式枚举映射 | 小 | 可读性大幅提升 | ✅ 完成 |
| 11 | 修复 pixel_history passed 字段 bug | 极小 | Bug fix | ✅ 完成 |

### 第二批（解决「好不好用」的问题）

| # | 改进 | 工作量 | 影响 | 状态 |
|---|------|--------|------|------|
| 2 | `get_draw_call_state()` + `get_frame_overview()` | 中 | 核心体验改进 | ✅ 完成 |
| 4 | `diff_draw_calls()` | 中 | 对比分析效率 | ✅ 完成 |
| 5 | `export_draw_textures()` | 小 | 纹理提取效率 | ✅ 完成 |
| 6 | `save_render_target()` | 小 | 快速预览 | ✅ 完成 |

### 第三批（解决「强不强」的问题）

| # | 改进 | 工作量 | 影响 | 状态 |
|---|------|--------|------|------|
| 7 | Shader 反编译 fallback 链 | 中 | Shader 分析 | ✅ 完成（GLSL 提取待做）|
| 8 | Uniform 值读取修复 | 中 | Shader 参数对比 | 待做 |
| 9 | `find_draws()` 状态搜索 | 大 | 高级搜索 | ✅ 完成 |
| 10 | `analyze_render_passes()` | 中 | 自动化分析 | ✅ 完成 |
| 12 | 资源 ID 缓存 | 小 | 性能优化 | ✅ 完成 |
| 13 | Mesh 导出 | 中 | 资产提取 | ✅ 完成 |

---

## 三-B、v0.3.0 新增与增强

### 工具增强（已有工具新增参数）

| 工具 | 新增能力 | 版本 |
|------|----------|------|
| `get_capture_info` | 返回 `api`, `resolution`, `main_color_format`, `known_gpu_quirks`（自动识别 Adreno/Mali/PowerVR/Apple 坑点） | **v0.3** |
| `list_actions` | 新增 `filter`（名称子串过滤）和 `event_type`（draw/dispatch/clear/copy/resolve） | **v0.3** |
| `disassemble_shader` | 新增 `search`（关键词 + ±5 行上下文）和 `line_range`（行范围切片） | **v0.3** |
| `get_cbuffer_contents` | 新增 `filter`（变量名子串过滤，支持嵌套结构体） | **v0.3** |
| `get_texture_stats` | 新增 `all_slices`（遍历所有 mip/cubemap 面），新增异常检测（NaN/Inf/negative warnings） | **v0.3** |
| `diff_draw_calls` | 返回扁平 `differences` 列表，每项附 `implication`（对渲染的影响说明） | **v0.3** |

### 新工具

| 工具 | 模块 | 说明 | 版本 |
|------|------|------|------|
| `read_texture_pixels` | data_tools | 读取矩形区域像素（最大 64×64），含逐像素异常标记 | **v0.3** |
| `sample_pixel_region` | advanced_tools | 均匀网格采样 RT 区域，检测 NaN/Inf/负值/过亮热点 | **v0.3** |
| `debug_shader_at_pixel` | advanced_tools | 逐像素 shader 调试（DebugPixel），不支持时 fallback 返回像素值+反射信息 | **v0.3** |
| `get_pass_timing` | performance_tools | 最耗时 render pass，使用 GPU 计数器或三角形数量启发式估算 | **v0.3** |
| `analyze_overdraw` | performance_tools | 按 RT 分组估算 overdraw | **v0.3** |
| `analyze_bandwidth` | performance_tools | 估算每 RT 的写入/读取带宽 | **v0.3** |
| `analyze_state_changes` | performance_tools | 查找冗余状态切换和 draw call 合批机会 | **v0.3** |
| `diagnose_negative_values` | diagnostic_tools | 扫描所有浮点 RT 中的负值/NaN/Inf，定位首次引入事件，检测 TAA 累积 | **v0.3** |
| `diagnose_precision_issues` | diagnostic_tools | 检查 R11G11B10 符号位缺失、浅深度缓冲、SRGB/线性空间不匹配 | **v0.3** |
| `diagnose_reflection_mismatch` | diagnostic_tools | 对比反射 pass 与主场景 draw，报告 shader/blend/RT 格式差异原因 | **v0.3** |
| `diagnose_mobile_risks` | diagnostic_tools | 全面检查精度/性能/兼容性/GPU 特定风险，返回排优先级的风险列表 | **v0.3** |

---

## 四、完整工具清单 (42 tools)

### Session (4)
| 工具 | 说明 | 版本 |
|------|------|------|
| `open_capture` | 打开 RDC 文件 | v0.1 |
| `close_capture` | 关闭当前捕获 | v0.1 |
| `get_capture_info` | 捕获基本信息 + GPU 坑点自动识别 | v0.1 (**v0.3 增强**) |
| `get_frame_overview` | 帧级别统计：action 分类计数、纹理/缓冲区内存、RT、分辨率 | **v0.2** |

### Events (5)
| 工具 | 说明 | 版本 |
|------|------|------|
| `list_actions` | 列出 action 树（支持 filter/event_type 过滤） | v0.1 (**v0.3 增强**) |
| `get_action` | 获取单个 action 详情 | v0.1 |
| `set_event` | 跳转到指定 event | v0.1 |
| `search_actions` | 按名称/flag 搜索 | v0.1 |
| `find_draws` | 按渲染状态搜索（blend/顶点数/纹理/shader/RT） | **v0.2** |

### Pipeline (4)
| 工具 | 说明 | 版本 |
|------|------|------|
| `get_pipeline_state` | 获取管线状态（枚举值已可读化） | v0.1 (v0.2 增强) |
| `get_shader_bindings` | 获取 shader 资源绑定 | v0.1 |
| `get_vertex_inputs` | 获取顶点输入 | v0.1 |
| `get_draw_call_state` | 一次调用返回完整 draw call 状态 | **v0.2** |

### Resources (4)
| 工具 | 说明 | 版本 |
|------|------|------|
| `list_textures` | 列出纹理 | v0.1 |
| `list_buffers` | 列出缓冲区 | v0.1 |
| `list_resources` | 列出所有命名资源 | v0.1 |
| `get_resource_usage` | 获取资源使用记录 | v0.1 |

### Data (8)
| 工具 | 说明 | 版本 |
|------|------|------|
| `save_texture` | 保存纹理到文件 | v0.1 |
| `get_buffer_data` | 读取缓冲区数据 | v0.1 |
| `pick_pixel` | 拾取像素值 | v0.1 |
| `get_texture_stats` | 纹理统计 + 异常检测（NaN/Inf/negative），支持 all_slices | v0.1 (**v0.3 增强**) |
| `read_texture_pixels` | 读取矩形区域像素（最大 64×64），含逐像素异常标记 | **v0.3** |
| `export_draw_textures` | 批量导出 draw call 所有绑定纹理 | **v0.2** |
| `save_render_target` | 保存当前 RT 快照（color + 可选 depth） | **v0.2** |
| `export_mesh` | 导出 mesh 为 OBJ（position/normal/uv） | **v0.2** |

### Shaders (3)
| 工具 | 说明 | 版本 |
|------|------|------|
| `disassemble_shader` | 反编译 shader（自动 fallback 链 + reflection 兜底），支持 search/line_range | v0.1 (**v0.3 增强**) |
| `get_shader_reflection` | 获取 shader 反射信息 | v0.1 |
| `get_cbuffer_contents` | 读取常量缓冲区（支持 filter 变量过滤） | v0.1 (**v0.3 增强**) |

### Advanced (6)
| 工具 | 说明 | 版本 |
|------|------|------|
| `pixel_history` | 像素历史（已修复 passed 字段 bug） | v0.1 (v0.2 修复) |
| `get_post_vs_data` | 获取 VS 输出数据 | v0.1 |
| `diff_draw_calls` | 对比两个 draw call 状态差异，附 implication 说明 | v0.2 (**v0.3 增强**) |
| `analyze_render_passes` | 自动检测 render pass 边界并汇总 | **v0.2** |
| `sample_pixel_region` | 均匀网格采样 RT 区域，检测 NaN/Inf/负值/过亮热点 | **v0.3** |
| `debug_shader_at_pixel` | 逐像素 shader 调试，不支持时 fallback 返回像素值+反射信息 | **v0.3** |

### Performance (4)
| 工具 | 说明 | 版本 |
|------|------|------|
| `get_pass_timing` | 最耗时 render pass（GPU 计数器 / 三角形数量启发式估算） | **v0.3** |
| `analyze_overdraw` | 按 RT 分组估算 overdraw | **v0.3** |
| `analyze_bandwidth` | 估算每 RT 的写入/读取带宽 | **v0.3** |
| `analyze_state_changes` | 查找冗余状态切换和 draw call 合批机会 | **v0.3** |

### Diagnostics (4)
| 工具 | 说明 | 版本 |
|------|------|------|
| `diagnose_negative_values` | 扫描浮点 RT 中的负值/NaN/Inf，定位首次引入事件，检测 TAA 累积 | **v0.3** |
| `diagnose_precision_issues` | 检查 R11G11B10 符号位缺失、浅深度缓冲、SRGB/线性空间不匹配 | **v0.3** |
| `diagnose_reflection_mismatch` | 对比反射 pass 与主场景，报告 shader/blend/RT 格式差异原因 | **v0.3** |
| `diagnose_mobile_risks` | 全面检查精度/性能/兼容性/GPU 特定风险，返回排优先级风险列表 | **v0.3** |

---

## 五、待完成项

| # | 改进 | 说明 |
|---|------|------|
| 1 | User 级别配置说明 + 安装脚本 | README 中推荐 `~/.claude/settings.json` 配置 |
| 7 (部分) | GL/GLES 原始 GLSL 源码提取 | 从 structured file 中提取原始 GLSL source |
| 8 | Uniform 值读取修复 | 适配多版本 API，新增 `get_uniform_values()` 高层工具 |
| 新 | tests/test_util.py 扩展 | 为 performance_tools / diagnostic_tools 添加 mock 测试 |
