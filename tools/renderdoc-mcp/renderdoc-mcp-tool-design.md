# RenderDoc MCP Server 工具设计方案

> 目标：让 Claude Code 通过 MCP 协议接入 RenderDoc，具备渲染 Bug 排查、性能分析、优化建议的完整能力。
> 覆盖移动端 FPS 项目中常见的精度问题、爆闪、颜色偏差、TAA 负值、IBL 异常等典型场景。

---

## 整体架构

```
Claude Code
    │
    └── RenderDoc MCP Server ──── RenderDoc Python API (renderdoc / qrenderdoc module)
            │
            ├── 1. 捕获文件管理（加载、设备信息）
            ├── 2. Event 与 Draw Call 浏览（列表、详情、搜索）
            ├── 3. Pipeline State 深度查询（blend/depth/raster 全状态 + 双事件对比）
            ├── 4. Shader 分析（源码、常量、单像素调试）
            ├── 5. 像素级诊断（Pixel History、区域扫描异常检测）
            ├── 6. 资源检查（纹理统计/读取、Buffer 读取）
            ├── 7. 性能分析（Pass Timing、Overdraw、带宽、状态切换）
            └── 8. 智能诊断（负值扫描、精度检测、反射对比、移动端风险预检）
```

核心设计原则：

- **LLM 可读性优先**：返回值包含人类可读的摘要和诊断提示，不只是裸数据
- **内置异常检测**：工具自动标注 NaN / Inf / 负值 / 极端值，LLM 无需自己判断
- **操作链路通畅**：每个工具的返回值中包含下一步可用的 ID 和建议操作
- **token 预算可控**：大数据量工具提供 brief 模式和 filter 参数，避免撑爆上下文

---

## 模块 1：捕获文件管理

---

### 1.1 `load_capture`

加载 RenderDoc 捕获文件。所有后续操作的前置步骤。

```json
{
  "name": "load_capture",
  "description": "加载 RenderDoc 捕获文件（.rdc）。这是所有分析操作的前置步骤，必须先加载才能查询。加载成功后返回帧的基本概览。如果之前已加载其他文件，会自动关闭旧文件。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "file_path": {
        "type": "string",
        "description": ".rdc 文件的绝对路径"
      }
    },
    "required": ["file_path"]
  }
}
```

**返回值**：
```json
{
  "status": "loaded",
  "api_type": "Vulkan",
  "total_events": 3847,
  "total_draw_calls": 1523,
  "total_dispatches": 47,
  "render_targets_count": 12,
  "frame_duration_ms": 16.7,
  "summary": "Vulkan 帧，3847 个事件（1523 draw / 47 dispatch），帧时长 16.7ms"
}
```

---

### 1.2 `get_capture_info`

获取当前已加载捕获文件的设备和环境信息。

```json
{
  "name": "get_capture_info",
  "description": "获取当前捕获的设备环境信息：GPU 型号、驱动版本、API 版本、分辨率、RT 格式等。在排查手机端精度 / 兼容性 Bug 时，设备信息是关键线索——Adreno 和 Mali 的 half float 行为差异很大，很多爆闪 Bug 跟具体 GPU 强相关。",
  "inputSchema": {
    "type": "object",
    "properties": {},
    "required": []
  }
}
```

**返回值**：
```json
{
  "gpu": "Adreno 640",
  "driver_version": "512.502.0",
  "api": "Vulkan 1.1",
  "resolution": "2400x1080",
  "main_color_format": "R11G11B10_FLOAT",
  "depth_format": "D24_UNORM_S8_UINT",
  "device_type": "mobile",
  "known_gpu_quirks": [
    "Adreno: mediump float 实际精度可能低于 spec 要求",
    "Adreno: R11G11B10_FLOAT 没有符号位，无法存储负值（负值被 clamp 到 0 或产生未定义行为）"
  ]
}
```

> **设计意图**：`known_gpu_quirks` 字段基于 GPU 型号自动附加已知的坑点，让 LLM 在后续分析时直接关联硬件特性。

---

## 模块 2：Event 与 Draw Call 浏览

---

### 2.1 `list_events`

获取帧中的事件列表。

```json
{
  "name": "list_events",
  "description": "获取帧中的事件列表，按 render pass 分组的树状结构。支持按名称过滤（模糊匹配）和按事件类型过滤。返回的 event_id 用于所有后续查询。建议先用 brief=true 快速浏览帧结构，再对感兴趣的 pass 做详细查询。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "filter": {
        "type": "string",
        "description": "按名称模糊过滤，如 'shadow'、'reflection'、'TAA'、'bloom'、'postprocess'"
      },
      "pass_name": {
        "type": "string",
        "description": "只返回指定 render pass 内的事件"
      },
      "event_type": {
        "type": "string",
        "enum": ["all", "draw", "dispatch", "clear", "copy", "resolve"],
        "description": "按事件类型过滤，默认 all"
      },
      "brief": {
        "type": "boolean",
        "description": "true = 只返回 event_id + 名称（快速浏览）；false = 包含 draw 参数和绑定的 shader 名。默认 true"
      }
    },
    "required": []
  }
}
```

---

### 2.2 `get_event_detail`

获取单个事件的完整信息。

```json
{
  "name": "get_event_detail",
  "description": "获取指定事件的完整详情：draw call 参数、绑定的 shader、绑定的所有纹理和 buffer、render target、viewport / scissor、以及 pipeline state 摘要。这是排查具体 draw call 问题的核心入口。返回的 resource_id 可用于 get_texture_info、read_texture_pixels 等资源查询工具。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "event_id": {
        "type": "integer",
        "description": "事件 ID，通过 list_events 获取"
      }
    },
    "required": ["event_id"]
  }
}
```

**返回值**：
```json
{
  "event_id": 1247,
  "name": "DrawIndexed(3456)",
  "parent_pass": "SceneCapture_Reflection",
  "draw_info": {
    "index_count": 3456,
    "instance_count": 1,
    "vertex_offset": 0
  },
  "shaders": {
    "vertex": { "name": "MobileBasePass.usf", "entry": "Main", "resource_id": 42 },
    "fragment": { "name": "MobileBasePass.usf", "entry": "Main", "resource_id": 43 }
  },
  "bound_textures": [
    { "slot": 0, "name": "SceneColorTexture", "format": "R11G11B10_FLOAT", "size": "1200x540", "resource_id": 101 },
    { "slot": 1, "name": "GBufferA", "format": "R8G8B8A8_UNORM", "size": "1200x540", "resource_id": 102 }
  ],
  "render_targets": [
    { "slot": 0, "name": "ReflectionRT", "format": "R11G11B10_FLOAT", "size": "600x270" }
  ],
  "depth_target": { "name": "SceneDepth", "format": "D24_UNORM_S8_UINT" },
  "pipeline_state_summary": {
    "blend": "Src=One, Dst=Zero（不透明覆盖）",
    "depth_test": "enabled, func=LessEqual, write=true",
    "cull_mode": "Back",
    "stencil": "disabled"
  },
  "next_steps_hint": "如需详细 pipeline state 使用 get_pipeline_state；如需查看 shader 源码使用 get_shader_source"
}
```

---

## 模块 3：Pipeline State 深度查询

---

### 3.1 `get_pipeline_state`

获取指定事件的完整渲染管线状态。

```json
{
  "name": "get_pipeline_state",
  "description": "获取指定事件的完整渲染管线状态，包含 blend、depth/stencil、rasterizer、viewport/scissor、multisample 的全部参数。当 get_event_detail 的 pipeline_state_summary 不够详细时使用。常用于排查：倒影颜色偏暗（blend 配置 / color write mask 问题）、物体消失（depth / cull 问题）、半透明错误等。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "event_id": {
        "type": "integer",
        "description": "事件 ID"
      },
      "section": {
        "type": "string",
        "enum": ["all", "blend", "depth_stencil", "rasterizer", "viewport", "multisample"],
        "description": "只查看特定部分，默认 all"
      }
    },
    "required": ["event_id"]
  }
}
```

---

### 3.2 `compare_pipeline_state`

对比两个事件的 pipeline state 差异，高亮不同点。

```json
{
  "name": "compare_pipeline_state",
  "description": "对比两个事件的渲染管线状态差异，只返回不同的字段。每个差异会附带 implication（影响推断）帮助定位问题。典型用法：对比原物体和其倒影的 draw call，找出为什么倒影偏暗（可能 blend 不同、shader 变体不同、color write mask 不同、gamma 处理不同）。也可对比同一个物体在不同 pass 中的渲染行为差异。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "event_id_a": {
        "type": "integer",
        "description": "参考事件（通常是'正确'的那个）"
      },
      "event_id_b": {
        "type": "integer",
        "description": "待比较事件（通常是'有问题'的那个）"
      }
    },
    "required": ["event_id_a", "event_id_b"]
  }
}
```

**返回值**：
```json
{
  "differences": [
    {
      "field": "fragment_shader",
      "event_a": "MobileBasePass.usf (permutation: REFLECTION=0)",
      "event_b": "MobileBasePass.usf (permutation: REFLECTION=1)",
      "implication": "反射 pass 使用了不同的 shader 变体分支，可能包含不同的光照 / tonemapping 逻辑"
    },
    {
      "field": "blend.color_src_factor",
      "event_a": "One",
      "event_b": "SrcAlpha",
      "implication": "反射 pass 使用了 alpha blend，可能导致颜色被 alpha 值衰减而偏暗"
    }
  ],
  "identical_sections": ["depth_stencil", "rasterizer", "viewport"],
  "summary": "发现 2 处差异，集中在 shader 变体和 blend 配置。blend 从 One 变为 SrcAlpha 是倒影颜色偏暗的最可能原因。"
}
```

---

## 模块 4：Shader 分析

---

### 4.1 `get_shader_source`

获取 shader 源代码或反编译结果。

```json
{
  "name": "get_shader_source",
  "description": "获取指定事件绑定的 shader 源代码。Vulkan 返回 SPIR-V 反编译的 GLSL/HLSL，OpenGL ES 返回原始 GLSL。用于分析 shader 逻辑错误、精度问题（half float 导致负值）、IBL 计算错误、tonemapping 溢出等。如果 shader 很长，可用 line_range 只看关键段落。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "event_id": {
        "type": "integer",
        "description": "事件 ID"
      },
      "stage": {
        "type": "string",
        "enum": ["vertex", "fragment", "compute"],
        "description": "着色器阶段，默认 fragment"
      },
      "format": {
        "type": "string",
        "enum": ["hlsl", "glsl", "spirv_disasm"],
        "description": "输出格式偏好，默认根据 API 自动选择"
      },
      "line_range": {
        "type": "array",
        "items": { "type": "integer" },
        "description": "[start_line, end_line]，只返回指定行范围的代码"
      },
      "search": {
        "type": "string",
        "description": "在 shader 源码中搜索关键字，只返回匹配行及上下文（前后各 5 行）。如 'ibl'、'tonemap'、'clamp'、'saturate'"
      }
    },
    "required": ["event_id"]
  }
}
```

---

### 4.2 `get_shader_constants`

获取 shader 常量 buffer 的实际值。

```json
{
  "name": "get_shader_constants",
  "description": "获取指定事件的 shader 常量值（Constant Buffer / Uniform Buffer 的当前数值）。对排查以下问题至关重要：IBL 强度系数异常、反射矩阵不正交、曝光值溢出、HDR tonemapping 参数异常、TAA 的 jitter offset / feedback 权重等。可按变量名过滤只看关注的值。注意：CB 中的值是 float32，但 GPU 实际运算可能使用 half float（精度更低）。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "event_id": {
        "type": "integer",
        "description": "事件 ID"
      },
      "stage": {
        "type": "string",
        "enum": ["vertex", "fragment", "compute"],
        "description": "着色器阶段，默认 fragment"
      },
      "buffer_slot": {
        "type": "integer",
        "description": "只查看指定 slot 的 buffer，不指定则返回所有"
      },
      "filter": {
        "type": "string",
        "description": "按变量名模糊过滤，如 'reflection'、'exposure'、'ibl'、'taa'、'jitter'"
      }
    },
    "required": ["event_id"]
  }
}
```

---

### 4.3 `debug_shader_at_pixel`

在指定像素位置单步执行 shader，返回每步的中间变量值。

```json
{
  "name": "debug_shader_at_pixel",
  "description": "对指定像素位置进行 shader 逐步调试，返回执行过程中每个变量在关键步骤的值。这是诊断精度和计算错误的终极工具。典型用例：(1) IBL 算出负数——追踪 SH 采样 → 乘以 baseColor → tonemapping 的全过程，找到哪步产生负值；(2) TAA 爆闪——追踪 history 采样 → motion vector → color clipping → blend 的过程；(3) 颜色偏差——追踪纹理采样 → 光照计算 → 输出的完整链路。自动标注异常值（NaN / Inf / 负值）。注意：此操作较慢（数秒），请针对性使用。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "event_id": {
        "type": "integer",
        "description": "事件 ID"
      },
      "pixel_x": {
        "type": "integer",
        "description": "像素 X 坐标（render target 空间，左上角为原点）"
      },
      "pixel_y": {
        "type": "integer",
        "description": "像素 Y 坐标"
      },
      "stage": {
        "type": "string",
        "enum": ["vertex", "fragment"],
        "description": "调试阶段，默认 fragment"
      },
      "watch_variables": {
        "type": "array",
        "items": { "type": "string" },
        "description": "只追踪这些变量，如 ['color', 'iblDiffuse', 'exposure']。不指定则返回所有变量（数据量较大）"
      },
      "max_steps": {
        "type": "integer",
        "description": "最大跟踪步数，默认 500"
      }
    },
    "required": ["event_id", "pixel_x", "pixel_y"]
  }
}
```

**返回值**（面向 LLM 可读性优化）：
```json
{
  "pixel": [1024, 512],
  "shader": "MobileBasePass.usf",
  "execution_trace": [
    {
      "line": 142,
      "source": "half3 iblDiffuse = SampleSH(worldNormal);",
      "outputs": { "iblDiffuse": [0.35, 0.28, -0.02] },
      "warning": "⚠️ iblDiffuse.b = -0.02，SH 采样在该法线方向产生负值"
    },
    {
      "line": 145,
      "source": "half3 finalColor = baseColor * iblDiffuse * iblIntensity;",
      "inputs": { "baseColor": [0.8, 0.7, 0.6], "iblDiffuse": [0.35, 0.28, -0.02], "iblIntensity": 1.5 },
      "outputs": { "finalColor": [0.42, 0.294, -0.018] },
      "warning": "⚠️ finalColor.b = -0.018，负值被乘法放大"
    }
  ],
  "final_output": [0.42, 0.294, -0.018, 1.0],
  "anomalies_summary": "检测到 2 处负值。根因：SH 采样在 line 142 产生负的 B 通道 (-0.02)，经 line 145 乘法传播到 finalColor。建议在 SH 采样后添加 max(0, x) clamp。"
}
```

---

## 模块 5：像素级诊断

---

### 5.1 `pixel_history`

获取指定像素在整帧中被修改的完整历史。

```json
{
  "name": "pixel_history",
  "description": "获取指定像素被所有 draw call 修改的完整历史：每次修改前后的颜色值、depth / stencil test 结果、blend 方程。是排查'为什么这个像素是这个颜色'的最直接手段。也会列出被 reject（depth test fail 等）的 draw call 及其本应写入的值。典型用例：倒影偏暗（对比正常渲染和反射 pass 对同一像素的写入）、物体消失（查看是否被 depth/stencil reject）、颜色异常（追踪所有 blend 叠加过程）。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "pixel_x": {
        "type": "integer",
        "description": "像素 X 坐标"
      },
      "pixel_y": {
        "type": "integer",
        "description": "像素 Y 坐标"
      },
      "target_resource_id": {
        "type": "integer",
        "description": "查询哪个 render target 上的历史。不指定 = 最终 backbuffer。可通过 get_event_detail 获取特定 RT 的 resource_id"
      }
    },
    "required": ["pixel_x", "pixel_y"]
  }
}
```

**返回值**：
```json
{
  "pixel": [1024, 512],
  "final_color": [0.65, 0.42, 0.38, 1.0],
  "modifications": [
    {
      "event_id": 245,
      "name": "Clear(SceneColor)",
      "action": "clear",
      "color_after": [0, 0, 0, 1]
    },
    {
      "event_id": 1023,
      "name": "DrawIndexed - SM_Rock_01",
      "action": "draw",
      "shader_output": [0.72, 0.48, 0.41, 1.0],
      "color_after": [0.72, 0.48, 0.41, 1.0],
      "depth_test": "passed (0.847 < 1.0)",
      "blend": "One / Zero（不透明覆盖）",
      "passed": true
    },
    {
      "event_id": 2105,
      "name": "DrawIndexed - TAA",
      "action": "draw",
      "shader_output": [0.65, 0.42, 0.38, 1.0],
      "color_after": [0.65, 0.42, 0.38, 1.0],
      "blend": "One / Zero",
      "passed": true
    }
  ],
  "rejected": [
    {
      "event_id": 1247,
      "name": "DrawIndexed - SM_Rock_01_Reflection",
      "reject_reason": "depth_test_failed (0.923 > 0.847)",
      "would_have_written": [0.52, 0.35, 0.30, 1.0]
    }
  ]
}
```

---

### 5.2 `sample_pixel_region`

批量扫描区域像素值，自动检测异常。

```json
{
  "name": "sample_pixel_region",
  "description": "在指定 render target 上批量采样像素值，自动检测 NaN / Inf / 负值 / 极端亮值。这是排查爆闪 Bug 的首选工具——不需要知道具体哪个像素有问题，直接扫描全屏找异常区域。移动端爆闪的根源通常是某些像素变成 Inf / NaN 或极端大值，然后通过 TAA history feedback 扩散到整个画面。此工具可快速定位'震源'。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "event_id": {
        "type": "integer",
        "description": "在此事件之后的状态上采样。不指定 = 最终输出"
      },
      "resource_id": {
        "type": "integer",
        "description": "指定 render target 的 resource_id。不指定 = 当前 event 的输出 RT"
      },
      "region": {
        "type": "object",
        "description": "采样区域 {x, y, width, height}。不指定 = 全屏均匀采样",
        "properties": {
          "x": { "type": "integer" },
          "y": { "type": "integer" },
          "width": { "type": "integer" },
          "height": { "type": "integer" }
        }
      },
      "sample_count": {
        "type": "integer",
        "description": "采样点数量，默认 256，均匀分布"
      },
      "anomaly_threshold": {
        "type": "number",
        "description": "亮度超过此值标记为异常（用于 HDR RT），默认 10.0"
      }
    },
    "required": []
  }
}
```

**返回值**：
```json
{
  "render_target": "SceneColor (R11G11B10_FLOAT)",
  "total_samples": 256,
  "anomalies": {
    "nan_count": 0,
    "inf_count": 2,
    "negative_count": 14,
    "extreme_bright_count": 8,
    "anomaly_rate": "9.4%"
  },
  "hotspots": [
    { "pixel": [834, 256], "value": ["+Inf", 342.5, 0.8], "type": "inf" },
    { "pixel": [412, 789], "value": [-0.05, 0.3, 0.28], "type": "negative" }
  ],
  "statistics": {
    "min": [-0.05, -0.02, -0.01],
    "max": ["+Inf", 342.5, 1.2],
    "mean": [2.84, 0.67, 0.52],
    "median": [0.45, 0.38, 0.32]
  },
  "diagnosis_hint": "Inf 集中在 (830-840, 250-260) 区域，负值散布全帧。建议对异常像素执行 pixel_history 定位写入源头，并检查 TAA history buffer 是否存在负值累积。"
}
```

---

## 模块 6：资源检查

---

### 6.1 `get_texture_info`

获取纹理元数据和值域统计。

```json
{
  "name": "get_texture_info",
  "description": "获取纹理的格式、尺寸、mip 数量，以及内容的数值统计（min / max / mean / NaN 检测）。不返回完整像素数据。用于快速判断纹理是否有异常：全黑？包含 NaN？值域不正常？格式与预期不符（如该用 SRGB 但实际是 UNORM 导致 gamma 双重校正）？IBL cubemap 某个面是否包含负值？",
  "inputSchema": {
    "type": "object",
    "properties": {
      "resource_id": {
        "type": "integer",
        "description": "纹理 resource_id，通过 get_event_detail 的 bound_textures 获取"
      },
      "mip_level": {
        "type": "integer",
        "description": "查看的 mip 层级，默认 0"
      },
      "all_slices": {
        "type": "boolean",
        "description": "true = 分别统计每个 array slice / cubemap face。对 cubemap 会返回 6 个面各自的统计。默认 false"
      }
    },
    "required": ["resource_id"]
  }
}
```

**返回值**（cubemap 示例）：
```json
{
  "name": "IBL_Cubemap",
  "format": "R16G16B16A16_FLOAT",
  "size": "256x256",
  "mip_count": 9,
  "array_size": 6,
  "type": "cubemap",
  "per_face_statistics": [
    { "face": "+X", "min": [0.0, 0.0, 0.0], "max": [4.2, 3.8, 5.1], "has_nan": false, "has_negative": false },
    { "face": "-X", "min": [-0.003, 0.0, 0.0], "max": [3.1, 2.9, 4.0], "has_nan": false, "has_negative": true },
    { "face": "+Y", "min": [0.0, 0.0, 0.0], "max": [12.5, 11.8, 15.2], "has_nan": false, "has_negative": false },
    { "face": "-Y", "min": [0.0, 0.0, 0.0], "max": [0.8, 0.7, 0.6], "has_nan": false, "has_negative": false },
    { "face": "+Z", "min": [0.0, 0.0, 0.0], "max": [2.5, 2.1, 3.3], "has_nan": false, "has_negative": false },
    { "face": "-Z", "min": [0.0, 0.0, -0.001], "max": [3.8, 3.5, 4.7], "has_nan": false, "has_negative": true }
  ],
  "warnings": [
    "⚠️ -X 面和 -Z 面包含微小负值，可能在 SH projection 或 filtering 过程中引入"
  ]
}
```

---

### 6.2 `read_texture_pixels`

读取纹理指定区域的实际像素值。

```json
{
  "name": "read_texture_pixels",
  "description": "读取纹理中指定区域的实际像素值（浮点精度）。区域大小限制 64x64 以控制返回数据量。用于精确检查：IBL cubemap 某个方向的具体数值、SH 系数贴图内容、LUT 贴图的采样值、history buffer 的累积值等。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "resource_id": { "type": "integer", "description": "纹理 resource_id" },
      "x": { "type": "integer" },
      "y": { "type": "integer" },
      "width": { "type": "integer", "description": "最大 64" },
      "height": { "type": "integer", "description": "最大 64" },
      "mip_level": { "type": "integer", "description": "mip 层级，默认 0" },
      "array_slice": { "type": "integer", "description": "数组 / cubemap slice，cubemap: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z" }
    },
    "required": ["resource_id", "x", "y", "width", "height"]
  }
}
```

---

### 6.3 `read_buffer_data`

读取 buffer 内容。

```json
{
  "name": "read_buffer_data",
  "description": "读取 buffer（Vertex Buffer / Index Buffer / Constant Buffer / SSBO）的内容。可按 float32 / float16 / uint 等格式解释原始数据，或对 Vertex Buffer 按绑定的 layout 自动解析出每个顶点属性。用于排查：顶点位置为 NaN、constant buffer 中的矩阵异常、SSBO 中的计算结果错误等。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "resource_id": { "type": "integer" },
      "offset_bytes": { "type": "integer", "description": "起始偏移，默认 0" },
      "length_bytes": { "type": "integer", "description": "读取长度，默认 1024，最大 4096" },
      "interpret_as": {
        "type": "string",
        "enum": ["raw_hex", "float32", "float16", "uint32", "uint16", "vertex_buffer"],
        "description": "数据解释方式。vertex_buffer 按绑定的 layout 解析"
      },
      "event_id": {
        "type": "integer",
        "description": "关联的事件 ID（vertex_buffer 模式下用于获取 layout）"
      }
    },
    "required": ["resource_id"]
  }
}
```

---

## 模块 7：性能分析

---

### 7.1 `get_pass_timing`

获取各 render pass / draw call 的 GPU 耗时。

```json
{
  "name": "get_pass_timing",
  "description": "获取帧中各 render pass 或各 draw call 的 GPU 耗时统计，默认按耗时从高到低排序。用于快速定位性能瓶颈。注意：移动端 tile-based GPU 上的 timing 可能不完全反映实际开销（tile load/store 不会被单独计时）。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "granularity": {
        "type": "string",
        "enum": ["pass", "draw_call"],
        "description": "统计粒度，默认 pass"
      },
      "sort_by": {
        "type": "string",
        "enum": ["time_desc", "time_asc", "order"],
        "description": "排序方式，默认 time_desc"
      },
      "top_n": {
        "type": "integer",
        "description": "只返回前 N 个，默认 20"
      }
    },
    "required": []
  }
}
```

---

### 7.2 `analyze_overdraw`

分析 overdraw 情况。

```json
{
  "name": "analyze_overdraw",
  "description": "分析帧中 overdraw 情况：平均 overdraw 倍率、overdraw 最严重的区域、导致 overdraw 最多的 draw call。移动端的 fill rate 和带宽是主要瓶颈，overdraw 直接影响帧率和耗电。可只分析指定 pass（如不透明 pass 或半透明 pass）。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "pass_name": { "type": "string", "description": "只分析指定 pass，不指定 = 全帧" },
      "region": {
        "type": "object",
        "description": "只分析指定区域 {x, y, width, height}",
        "properties": {
          "x": { "type": "integer" }, "y": { "type": "integer" },
          "width": { "type": "integer" }, "height": { "type": "integer" }
        }
      }
    },
    "required": []
  }
}
```

---

### 7.3 `analyze_bandwidth`

估算显存带宽消耗。

```json
{
  "name": "analyze_bandwidth",
  "description": "估算帧渲染的显存带宽消耗：纹理读取、RT 读写、buffer 读取的总量和分布。在 tile-based GPU（移动端）上还分析 tile load/store 开销，识别不必要的 full-screen resolve。带宽是移动端 GPU 的最大性能瓶颈和发热来源。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "breakdown_by": {
        "type": "string",
        "enum": ["pass", "resource_type", "operation"],
        "description": "拆分维度，默认 pass"
      }
    },
    "required": []
  }
}
```

---

### 7.4 `analyze_state_changes`

分析 draw call 之间的状态切换和合批机会。

```json
{
  "name": "analyze_state_changes",
  "description": "分析帧中 draw call 之间的渲染状态切换频率，识别合批（batching）机会。统计 shader / 纹理 / RT / blend state 的切换次数和模式，返回哪些连续 draw call 状态一致可以合并，以及如果调整渲染顺序能减少多少切换。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "pass_name": { "type": "string" },
      "change_types": {
        "type": "array",
        "items": {
          "type": "string",
          "enum": ["shader", "texture", "render_target", "blend", "depth_stencil", "vertex_buffer"]
        },
        "description": "关注的切换类型，默认全部"
      }
    },
    "required": []
  }
}
```

---

## 模块 8：智能诊断（高级封装）

这一层将多步底层操作封装为一步完成的诊断流程，面向特定 Bug 类型。

---

### 8.1 `diagnose_negative_values`

全帧扫描负值问题——对应 IBL 负数、TAA 爆闪等场景。

```json
{
  "name": "diagnose_negative_values",
  "description": "全帧扫描所有浮点格式 render target 中的负值像素，定位负值首次出现的 draw call，并追溯负值来源。自动分析 TAA history buffer 是否在放大负值（每帧累积扩散是爆闪的典型模式）。返回根因候选和修复建议。一站式覆盖 'IBL 算出负数' → 'TAA 放大' → '爆闪' 的完整链路。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "check_targets": {
        "type": "array",
        "items": { "type": "string" },
        "description": "指定要检查的 RT 名称，如 ['SceneColor', 'TAAHistory']。不指定 = 检查所有浮点 RT"
      },
      "trace_depth": {
        "type": "integer",
        "description": "发现负值后向前追溯几个 event 定位来源，默认 5"
      }
    },
    "required": []
  }
}
```

**返回值**：
```json
{
  "scan_result": "NEGATIVE_VALUES_DETECTED",
  "affected_targets": [
    {
      "name": "SceneColor",
      "format": "R11G11B10_FLOAT",
      "negative_pixel_count": 47,
      "negative_rate": "0.8%",
      "first_introduced_at": {
        "event_id": 892,
        "name": "DrawIndexed - IBL_Apply",
        "new_negative_pixels": 23,
        "sample": { "pixel": [412, 789], "value": [-0.05, 0.3, 0.28] }
      }
    },
    {
      "name": "TAAHistory",
      "format": "R16G16B16A16_FLOAT",
      "negative_pixel_count": 183,
      "negative_rate": "3.2%",
      "first_introduced_at": {
        "event_id": 2105,
        "name": "DrawIndexed - TAA_Resolve",
        "new_negative_pixels": 183
      },
      "accumulation_warning": "TAAHistory 负值数 (183) 是 SceneColor (47) 的 3.9 倍，TAA feedback 正在放大负值，持续累积将导致爆闪"
    }
  ],
  "root_cause_candidates": [
    {
      "likelihood": "高",
      "cause": "IBL SH 采样在部分法线方向产生负值",
      "evidence": "event 892 首次引入 23 个负值像素",
      "fix": "SH 采样后添加 max(0, result) clamp，或改用非负 SH 基函数"
    },
    {
      "likelihood": "高",
      "cause": "TAA color clipping 未处理负值输入",
      "evidence": "TAAHistory 负值是 SceneColor 的 3.9 倍",
      "fix": "TAA shader 中对 history sample 和 neighborhood AABB 的下界 clamp 到 0"
    }
  ]
}
```

---

### 8.2 `diagnose_precision_issues`

检测 half float 精度问题——对应移动端精度 Bug。

```json
{
  "name": "diagnose_precision_issues",
  "description": "检测帧中可能的浮点精度问题。分析：(1) half float 格式 RT 上的精度丢失（对比 float32 参考值，如果有的话）；(2) shader 中的 mediump 变量在极端值附近的行为；(3) R11G11B10_FLOAT 的特殊限制（无符号位、有限精度）；(4) 深度 buffer 的精度不足导致 z-fighting。返回检测到的精度问题列表和每个问题的影响评估。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "focus": {
        "type": "string",
        "enum": ["all", "color_precision", "depth_precision", "shader_precision"],
        "description": "关注的精度维度，默认 all"
      },
      "threshold": {
        "type": "number",
        "description": "精度偏差超过此阈值才报告（相对误差），默认 0.01 (1%)"
      }
    },
    "required": []
  }
}
```

**返回值**：
```json
{
  "issues_found": [
    {
      "type": "format_limitation",
      "target": "SceneColor (R11G11B10_FLOAT)",
      "description": "R11G11B10_FLOAT 没有符号位，无法存储负值。任何写入该 RT 的负值会被 clamp 到 0 或产生未定义行为。",
      "affected_events": [892, 945, 1023],
      "severity": "high",
      "fix": "如果流程中存在合法负值（如 SH 采样），需要在写入该 RT 前 clamp 到 [0, max]，或改用 R16G16B16A16_FLOAT（但会增加带宽开销）"
    },
    {
      "type": "depth_precision",
      "description": "D24 深度 buffer 在远平面附近精度不足，near=0.1 far=1000 时 z=500 以外的精度约为 0.3 世界单位",
      "z_fighting_risk_zones": [
        { "depth_range": [800, 1000], "precision": "~1.2 world units", "risk": "high" }
      ],
      "severity": "medium",
      "fix": "考虑使用 reversed-Z + D32_FLOAT，或缩小 far plane"
    }
  ]
}
```

---

### 8.3 `diagnose_reflection_mismatch`

专门诊断反射 / 倒影颜色不匹配问题。

```json
{
  "name": "diagnose_reflection_mismatch",
  "description": "诊断反射 / 倒影与原物体之间的颜色差异问题（如倒影偏暗、偏色、缺失高光）。自动执行：(1) 识别帧中的反射相关 pass（SceneCapture / Planar Reflection / SSR 等）；(2) 找到同一个物体在正常渲染和反射 pass 中的 draw call；(3) 对比两者的 shader 变体、pipeline state、绑定的纹理和常量；(4) 在可配对的像素上对比最终颜色值。返回所有差异及其对颜色的影响分析。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "reflection_pass_hint": {
        "type": "string",
        "description": "反射 pass 的名称提示，如 'SceneCapture'、'PlanarReflection'、'SSR'。不指定则自动检测"
      },
      "object_hint": {
        "type": "string",
        "description": "要对比的物体名称提示，如 'SM_Rock'。不指定则选择差异最大的物体"
      }
    },
    "required": []
  }
}
```

**返回值**：
```json
{
  "reflection_type": "SceneCapture (Planar Reflection)",
  "paired_objects": [
    {
      "object": "SM_Building_01",
      "normal_event_id": 1023,
      "reflection_event_id": 1247,
      "color_difference": {
        "sample_pixel_normal": [0.72, 0.48, 0.41],
        "sample_pixel_reflection": [0.52, 0.35, 0.30],
        "brightness_ratio": 0.72,
        "description": "反射比正常渲染暗约 28%"
      },
      "causes": [
        {
          "factor": "shader_variant",
          "detail": "反射 pass 使用 REFLECTION=1 变体，该变体跳过了 IBL specular 计算",
          "estimated_impact": "亮度降低 ~15%",
          "fix": "在 REFLECTION=1 变体中保留 IBL specular 项，或使用简化版本"
        },
        {
          "factor": "blend_state",
          "detail": "反射 pass 的 blend src 从 One 变为 SrcAlpha，而 alpha 值为 0.85",
          "estimated_impact": "亮度降低 ~15%",
          "fix": "检查反射 pass 是否需要 alpha blend，如果不需要应改为 One/Zero"
        },
        {
          "factor": "render_target_format",
          "detail": "反射 RT 分辨率 (600x270) 是主 RT (1200x540) 的 1/4，但格式相同",
          "estimated_impact": "分辨率不影响亮度，但可能影响高光细节",
          "fix": "无需修改"
        }
      ],
      "total_estimated_brightness_loss": "~28%（与实测 28% 吻合）"
    }
  ]
}
```

> **设计意图**：这个工具完整覆盖了"倒影为什么偏暗"的排查。不需要 LLM 自己去找反射 pass、配对 draw call、对比 state——一步完成。

---

### 8.4 `diagnose_mobile_risks`

基于帧数据预检移动端常见风险。

```json
{
  "name": "diagnose_mobile_risks",
  "description": "基于当前帧数据，全面检测移动端常见的渲染风险和潜在 Bug。检查项包括：(1) 精度风险：half float RT 中的极端值、R11G11B10 存储负值、深度精度不足；(2) 性能风险：过高的 overdraw、不必要的全屏 pass、带宽过大的 RT、过多的状态切换；(3) 兼容性风险：依赖桌面端特性（如几何着色器）、超出移动端限制的纹理大小或采样器数量；(4) 已知 GPU 特定问题（基于 get_capture_info 的 GPU 型号）。返回风险列表和优先级排序。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "check_categories": {
        "type": "array",
        "items": {
          "type": "string",
          "enum": ["precision", "performance", "compatibility", "gpu_specific"]
        },
        "description": "要检查的类别，默认全部"
      },
      "severity_filter": {
        "type": "string",
        "enum": ["all", "high", "medium"],
        "description": "只返回指定严重程度以上的风险，默认 all"
      }
    },
    "required": []
  }
}
```

**返回值**：
```json
{
  "device": "Adreno 640 / Vulkan 1.1",
  "risks": [
    {
      "category": "precision",
      "severity": "high",
      "title": "R11G11B10_FLOAT 存储负值风险",
      "detail": "SceneColor 使用 R11G11B10_FLOAT（无符号位），但 IBL pass 写入了负值。在 Adreno 上负值会被 clamp 到 0，在 Mali 上行为未定义。",
      "affected_events": [892, 945],
      "fix": "IBL shader 输出 clamp 到 0，或将 SceneColor 改为 R16G16B16A16_FLOAT"
    },
    {
      "category": "precision",
      "severity": "high",
      "title": "TAA 负值累积导致爆闪风险",
      "detail": "TAAHistory (R16G16B16A16_FLOAT) 中检测到 183 个负值像素，且数量超过上游 SceneColor 的负值数，说明 TAA feedback 在累积放大负值。",
      "fix": "TAA shader 中 clamp history 采样和 AABB 下界到 0"
    },
    {
      "category": "performance",
      "severity": "high",
      "title": "全屏 pass 过多",
      "detail": "检测到 8 个全屏 post-process pass，总耗时 4.2ms（占帧时长 25%）。其中 Bloom 降采样 4 级 + 升采样 4 级共 8 个 pass。",
      "fix": "考虑合并 bloom pass（每级降采样和升采样合并为单 pass），或减少 bloom 层级数"
    },
    {
      "category": "performance",
      "severity": "medium",
      "title": "半透明 overdraw 过高",
      "detail": "半透明 pass 平均 overdraw 3.7x，屏幕中心区域最高达 8.2x（粒子特效叠加区域）。",
      "fix": "减少粒子层数、使用更小的粒子 quad、开启 soft particle 裁剪"
    },
    {
      "category": "performance",
      "severity": "medium",
      "title": "不必要的 RT resolve",
      "detail": "检测到 3 次 MSAA resolve 操作，但后续 pass 没有使用 resolve 后的数据。tile-based GPU 上每次 resolve 都有显著带宽成本。",
      "fix": "移除未使用的 resolve，或使用 subpass 自动 resolve"
    },
    {
      "category": "compatibility",
      "severity": "low",
      "title": "纹理采样器数量接近限制",
      "detail": "event 1023 的 fragment shader 绑定了 14 个纹理采样器，部分低端移动 GPU 限制为 16 个。",
      "fix": "合并纹理 atlas 或使用纹理数组减少采样器绑定数"
    },
    {
      "category": "gpu_specific",
      "severity": "medium",
      "title": "Adreno mediump 精度低于标准",
      "detail": "Adreno 640 的 mediump float 实际精度约为 FP16 的最低要求，在法线重建和 lighting 计算中可能产生可见瑕疵。",
      "fix": "关键计算（法线重建、反射方向计算）强制使用 highp"
    }
  ],
  "summary": "发现 7 个风险项（2 个 high / 3 个 medium / 1 个 low / 1 个 gpu_specific）。最高优先级：修复 IBL 负值和 TAA 负值累积问题以消除爆闪风险。"
}
```

---

## 工具一览表

| # | 工具名 | 模块 | 核心用途 |
|---|--------|------|---------|
| 1 | `load_capture` | 捕获管理 | 加载 .rdc 文件 |
| 2 | `get_capture_info` | 捕获管理 | 获取设备 / 环境信息 |
| 3 | `list_events` | Event 浏览 | 浏览帧中所有事件（支持过滤） |
| 4 | `get_event_detail` | Event 浏览 | 获取单个事件的完整信息 |
| 5 | `get_pipeline_state` | Pipeline State | 获取完整渲染管线状态 |
| 6 | `compare_pipeline_state` | Pipeline State | 对比两个事件的 state 差异 |
| 7 | `get_shader_source` | Shader 分析 | 获取 shader 源码（支持搜索） |
| 8 | `get_shader_constants` | Shader 分析 | 获取 constant buffer 数值 |
| 9 | `debug_shader_at_pixel` | Shader 分析 | 单像素 shader 逐步调试 |
| 10 | `pixel_history` | 像素诊断 | 像素的完整修改历史 |
| 11 | `sample_pixel_region` | 像素诊断 | 区域批量采样 + 异常检测 |
| 12 | `get_texture_info` | 资源检查 | 纹理元数据 + 值域统计 |
| 13 | `read_texture_pixels` | 资源检查 | 读取纹理区域像素值 |
| 14 | `read_buffer_data` | 资源检查 | 读取 buffer 内容 |
| 15 | `get_pass_timing` | 性能分析 | 各 pass 的 GPU 耗时 |
| 16 | `analyze_overdraw` | 性能分析 | overdraw 分析 |
| 17 | `analyze_bandwidth` | 性能分析 | 显存带宽估算 |
| 18 | `analyze_state_changes` | 性能分析 | 状态切换分析 + 合批建议 |
| 19 | `diagnose_negative_values` | 智能诊断 | 负值扫描 + 根因追溯 |
| 20 | `diagnose_precision_issues` | 智能诊断 | 精度问题检测 |
| 21 | `diagnose_reflection_mismatch` | 智能诊断 | 反射颜色差异诊断 |
| 22 | `diagnose_mobile_risks` | 智能诊断 | 移动端风险全面预检 |

---

## 工具调用链路示例

以下展示 Claude Code 在真实场景下如何串联这些工具。

### 场景 A：倒影比原物体颜色偏暗

```
用户: "倒影效果比原物体颜色偏暗，帮我查一下原因"

Claude Code 执行链路:
1. load_capture("frame.rdc")
2. diagnose_reflection_mismatch()                    ← 一步到位
   → 自动识别反射 pass、配对 draw call、对比差异
   → 返回：shader 变体差异 + blend state 差异 + 量化的亮度损失分析

如果需要深入（自动诊断不够）:
3. compare_pipeline_state(event_a=正常DC, event_b=反射DC)
4. get_shader_source(event_id=反射DC, search="ibl")
5. debug_shader_at_pixel(event_id=反射DC, pixel_x=..., pixel_y=...)
```

### 场景 B：IBL 算出负数

```
用户: "IBL 光照算出了负数，帮我定位问题"

Claude Code 执行链路:
1. load_capture("frame.rdc")
2. diagnose_negative_values()                        ← 全帧扫描负值
   → 定位到 event 892 (IBL_Apply) 首次引入负值
3. get_shader_source(event_id=892, search="SampleSH")
   → 查看 SH 采样代码有没有 clamp
4. get_shader_constants(event_id=892, filter="sh")
   → 检查 SH 系数值是否合理
5. debug_shader_at_pixel(event_id=892, pixel_x=412, pixel_y=789, watch=["iblDiffuse"])
   → 追踪负值产生的具体步骤
6. get_texture_info(resource_id=IBL_cubemap_id, all_slices=true)
   → 检查 cubemap 本身是否包含负值
```

### 场景 C：手机端爆闪（TAA 负值）

```
用户: "手机上偶尔出现画面爆闪，怀疑是 TAA 的问题"

Claude Code 执行链路:
1. load_capture("frame_flash.rdc")
2. get_capture_info()                                ← 确认 GPU 型号和已知问题
3. diagnose_negative_values(check_targets=["SceneColor","TAAHistory"])
   → 发现 TAA history 中负值在累积放大
4. sample_pixel_region(event_id=TAA_event, anomaly_threshold=10.0)
   → 定位爆闪像素的具体位置
5. pixel_history(pixel_x=爆闪点X, pixel_y=爆闪点Y)
   → 追踪该像素被哪些 pass 修改
6. debug_shader_at_pixel(event_id=TAA_event, pixel_x=..., pixel_y=..., watch=["historyColor","clampedColor","outputColor"])
   → 追踪 TAA shader 中 history 采样和 color clipping 的过程
7. diagnose_precision_issues(focus="color_precision")
   → 检查 R11G11B10 格式限制是否加剧了问题
```

### 场景 D：全面性能分析与优化建议

```
用户: "帧率不达标，帮我分析一下性能瓶颈并给优化建议"

Claude Code 执行链路:
1. load_capture("frame.rdc")
2. get_capture_info()                                ← 确认设备环境
3. get_pass_timing(granularity="pass", top_n=10)     ← 找最耗时的 pass
4. analyze_overdraw()                                ← 检查 fill rate 压力
5. analyze_bandwidth(breakdown_by="pass")            ← 检查带宽瓶颈
6. analyze_state_changes()                           ← 检查合批机会
7. diagnose_mobile_risks(check_categories=["performance"])
   → 综合以上数据给出优化建议优先级列表
```

### 场景 E：移动端 Bug 风险预检

```
用户: "这帧在 PC 上没问题，帮我预判一下手机上可能出什么 Bug"

Claude Code 执行链路:
1. load_capture("frame.rdc")
2. get_capture_info()                                ← 确认 GPU 和已知问题
3. diagnose_mobile_risks()                           ← 全面风险预检
   → 一次性返回精度 / 性能 / 兼容性 / GPU 特定的所有风险
4. 根据风险项中 severity=high 的内容做深入排查
```

---

## 实现优先级建议

### P0 — 核心功能（先实现，能跑通基本工作流）

| 工具 | 理由 |
|------|------|
| `load_capture` | 一切的起点 |
| `get_capture_info` | 设备信息对移动端诊断必不可少 |
| `list_events` | 需要能浏览帧结构 |
| `get_event_detail` | 需要能看具体 draw call |
| `get_pipeline_state` | 大量 Bug 跟 state 配置有关 |
| `get_shader_source` | 需要能看 shader 代码 |
| `get_shader_constants` | 需要能看 uniform 值 |
| `pixel_history` | 像素级诊断的基石 |
| `get_texture_info` | 资源检查的基础 |

### P1 — 高价值功能（显著提升诊断效率）

| 工具 | 理由 |
|------|------|
| `debug_shader_at_pixel` | 精度 Bug 和计算 Bug 的终极武器，但实现复杂度高 |
| `compare_pipeline_state` | 对比类问题（倒影偏暗等）的效率工具 |
| `sample_pixel_region` | 爆闪定位的首选 |
| `read_texture_pixels` | 精确检查纹理内容 |
| `diagnose_negative_values` | 封装最常见的 Bug 类型 |
| `get_pass_timing` | 性能分析的入口 |

### P2 — 增强功能（锦上添花）

| 工具 | 理由 |
|------|------|
| `read_buffer_data` | 低频使用但偶尔需要 |
| `analyze_overdraw` | 性能优化的辅助数据 |
| `analyze_bandwidth` | 移动端深度性能分析 |
| `analyze_state_changes` | 合批优化的辅助 |
| `diagnose_precision_issues` | 系统性精度排查 |
| `diagnose_reflection_mismatch` | 反射问题的一键诊断 |
| `diagnose_mobile_risks` | 全面预检 |

---

## 实现技术要点

### RenderDoc Python API 映射

```
MCP 工具                      → RenderDoc Python API
──────────────────────────────────────────────────────
load_capture                  → rd.OpenCaptureFile() + ReplayController
get_capture_info              → controller.GetAPIProperties() + GetDriverInfo()
list_events                   → controller.GetDrawcalls() (递归遍历树)
get_event_detail              → controller.SetFrameEvent() + GetD3D11/VulkanPipelineState()
get_pipeline_state            → controller.GetVulkanPipelineState() (或对应 API)
get_shader_source             → controller.GetShader() + DisassembleShader()
get_shader_constants          → controller.GetCBuffers() + GetBufferData()
debug_shader_at_pixel         → controller.DebugPixel()
pixel_history                 → controller.PixelHistory()
sample_pixel_region           → controller.GetTextureData() + numpy 分析
get_texture_info              → controller.GetTextures() + GetTextureData() 统计
read_texture_pixels           → controller.GetTextureData() 切片
read_buffer_data              → controller.GetBufferData()
get_pass_timing               → controller.FetchCounters() (如支持 GPU timing)
```

### 注意事项

1. **RenderDoc Python module 需要编译安装**：renderdoc 的 Python binding 不是 pip 包，需要从源码编译或从 RenderDoc 安装目录导入 `renderdoc.pyd` / `renderdoc.so`
2. **ReplayController 是有状态的**：`SetFrameEvent(eid)` 会改变当前活跃的事件，后续的 state 查询都基于这个事件。MCP Server 需要管理好这个状态
3. **GPU Timing 在移动端可能不可用**：部分移动端 GPU 的 RenderDoc 捕获不支持 timing counter，`get_pass_timing` 需要做好降级处理
4. **大纹理读取的内存管理**：`GetTextureData()` 返回原始字节，大纹理可能占用大量内存，注意及时释放
5. **Shader 调试的平台限制**：`DebugPixel()` 在某些 API / GPU 组合下可能不支持，需要检查 `controller.GetDebugMessages()`
