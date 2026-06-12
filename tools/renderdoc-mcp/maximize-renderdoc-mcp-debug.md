# 最大化 RenderDoc MCP 调试能力：搭配方案与集成策略

---

## 核心问题：RenderDoc MCP 单独能做什么、不能做什么

RenderDoc 本质是一个**帧级别的快照分析器**。它能告诉你"这一帧发生了什么"，但有三个根本性的盲区：

```
RenderDoc 能做的                          RenderDoc 做不到的
─────────────────────                     ─────────────────────
✅ 这一帧的 draw call 列表                ❌ 为什么引擎会发出这个 draw call
✅ 这个像素当前的颜色值                    ❌ 这个颜色值是怎么从材质参数算出来的（引擎层面）
✅ shader 编译后的最终代码                 ❌ shader 编译前的原始 .usf/.ush 源码
✅ constant buffer 里的数值                ❌ 这些数值是哪个引擎模块在哪里设置的
✅ 这一帧的 GPU 耗时                      ❌ 跨帧的性能趋势和 CPU 端瓶颈
✅ RT 格式和像素数据                       ❌ 为什么选了这个格式（引擎配置层面）
✅ 帧内的异常检测                          ❌ 异常是否稳定复现、在哪些设备上出现
✅ 发现问题                               ❌ 修改代码修复问题
```

这些盲区决定了：**RenderDoc MCP 单独使用可以完成约 60-70% 的诊断工作（发现问题 + 定位到 shader / pipeline state 级别的原因），但剩下的 30-40%（理解引擎层面的 why、修复代码、验证修复）需要其他工具配合。**

---

## 搭配方案总览

按优先级排序，每个搭配方案解决 RenderDoc 的一个特定盲区：

```
优先级    搭配的 MCP              解决的盲区                           价值
────────────────────────────────────────────────────────────────────────────
P0       Filesystem MCP          读写项目源码、修改 shader           闭环：从诊断到修复
P0       Git MCP                 版本对比、提交修复                   追溯问题引入时间点
P1       UE Editor MCP           引擎层面的材质/渲染设置查询          理解 "why"
P1       ADB / Device MCP        设备管理、截帧触发、日志收集         多设备复现和验证
P2       Performance Log MCP     跨帧性能数据、CPU profiling         补充 GPU-only 的视角
P2       Knowledge Base MCP      GPU 硬件特性、已知 Bug 数据库        辅助根因推理
```

下面逐一详细展开。

---

## P0：Filesystem MCP — 从"发现问题"到"修复问题"的闭环

### 为什么是 P0

RenderDoc MCP 最大的限制是**只读**。它能告诉你"IBL shader 第 142 行的 SH 采样产生了负值，需要加 max(0, x)"，但它不能帮你改代码。如果 Claude Code 在诊断出问题后还需要你手动去找文件、改代码，那整个流程的自动化程度就大打折扣。

### 实现方式

不需要单独开发——Claude Code 本身就有文件系统访问能力。关键是让 RenderDoc MCP 的返回值中包含足够的信息，让 Claude Code 能定位到源码文件：

```
RenderDoc 返回                              Claude Code 后续动作
─────────────────                           ─────────────────────
shader: "MobileBasePass.usf", line 142      → 在项目中搜索 MobileBasePass.usf
                                            → 定位到第 142 行
                                            → 添加 max(0, iblDiffuse) clamp
                                            → 提交修改
```

### 需要 RenderDoc MCP 做的适配

在 `get_shader_source` 和 `debug_shader_at_pixel` 的返回值中，尽量保留原始文件名和行号映射：

```json
{
  "source_file_hint": "Engine/Shaders/Private/MobileBasePassPixelShader.usf",
  "line_mapping": {
    "compiled_line_142": "source_line_387 in MobileBasePassPixelShader.usf"
  }
}
```

> 注意：SPIR-V 反编译后的行号和原始 HLSL/GLSL 的行号不一定对得上。如果 RenderDoc 捕获时包含了 debug info（shader 编译时带 -g 或 -Od），就能拿到精确映射；否则只能靠函数名和代码模式匹配。建议在开发阶段的捕获流程中始终开启 shader debug info。

### 典型闭环流程

```
1. [RenderDoc MCP] diagnose_negative_values()
   → "IBL_Apply shader (MobileBasePass.usf) 第 142 行 SH 采样产生负值"

2. [Filesystem] 搜索项目中的 MobileBasePassPixelShader.usf
   → 找到 Engine/Shaders/Private/MobileBasePassPixelShader.usf

3. [Filesystem] 读取文件，定位到 SampleSH 调用处

4. [Claude Code 推理] 生成修复方案：
   - 方案 A: SH 采样后加 max(0, result)
   - 方案 B: 在 SH 系数预处理阶段 clamp

5. [Filesystem] 写入修改

6. [Git MCP] 提交到 fix/ibl-negative-clamp 分支
```

---

## P0：Git MCP — 追溯问题引入时间 + 管理修复

### 为什么是 P0

渲染 Bug 排查中一个极其常见的需求是：**"这个 Bug 是什么时候引入的？"** 比如"上周这个倒影还是正常的，这周突然偏暗了"。这需要对比不同版本的 shader 代码和渲染配置。

### 关键工具

```json
{
  "name": "git_log_file",
  "description": "查看指定文件的修改历史"
},
{
  "name": "git_diff",
  "description": "对比两个 commit 之间的文件差异"
},
{
  "name": "git_blame",
  "description": "查看文件每一行最后的修改者和时间"
},
{
  "name": "git_commit",
  "description": "提交修改到版本控制"
},
{
  "name": "git_branch",
  "description": "创建/切换分支"
}
```

### 典型流程

```
1. [RenderDoc MCP] diagnose_reflection_mismatch()
   → "反射 pass 使用了 REFLECTION=1 变体，该变体缺少 IBL specular"

2. [Git MCP] git_log_file("MobileBasePassPixelShader.usf")
   → 发现 3 天前有一次修改："优化：反射 pass 移除 IBL specular 以提升性能"

3. [Git MCP] git_diff(commit_before, commit_after, file="MobileBasePassPixelShader.usf")
   → 确认是哪行代码被删了

4. [Claude Code 推理] 
   → "3 天前的性能优化移除了反射 pass 的 IBL specular，导致反射亮度降低约 15%。
      建议：不要完全移除，改用简化版 IBL specular（如只用 mip 最高层 cubemap 采样）"

5. [Filesystem] 写入修改
6. [Git MCP] 提交到 fix/reflection-brightness 分支
```

---

## P1：UE Editor MCP — 理解引擎层面的"为什么"

### 为什么是 P1

RenderDoc 看到的是 GPU 最终执行的指令。但很多问题的根因在引擎配置层面。举几个例子：

| RenderDoc 看到的现象 | 引擎层面的真实原因 |
|--------------------|--------------------|
| 反射 RT 分辨率只有主 RT 的 1/4 | SceneCapture 组件的分辨率缩放参数设成了 0.25 |
| shader 变体 REFLECTION=1 缺少 IBL | 材质的 "Used with Planar Reflection" 开关触发了不同的编译路径 |
| R11G11B10_FLOAT 而非 R16F | 项目的 Mobile HDR 设置选了 "Mosaic" 编码模式 |
| 某个 post-process 意外启用 | PostProcessVolume 的 blend 权重设置问题 |
| TAA 的 feedback 权重异常高 | 项目设置中 TAA 的自定义参数被错误修改 |

没有 UE Editor MCP，Claude Code 只能猜测引擎层面的原因。有了它，就能直接查证。

### 关键工具

```json
{
  "name": "query_material",
  "description": "查询材质实例的参数值、shader 编译配置、使用的 feature flags"
},
{
  "name": "query_render_settings",
  "description": "查询项目的渲染设置：Mobile HDR 模式、TAA 参数、shadow 配置等"
},
{
  "name": "query_scene_component",
  "description": "查询场景中特定组件（SceneCapture、PostProcessVolume、Light）的参数"
},
{
  "name": "query_console_variables",
  "description": "查询引擎的 console variable 值（r.MobileHDR、r.Tonemapper 等）"
},
{
  "name": "list_shader_permutations",
  "description": "列出某个材质编译出的所有 shader 变体及其触发条件"
}
```

### 典型流程（倒影偏暗的完整排查）

```
1. [RenderDoc MCP] diagnose_reflection_mismatch()
   → "反射 pass shader 变体 REFLECTION=1 缺少 IBL specular，blend 用了 SrcAlpha"
   → "反射 RT 分辨率是主 RT 的 1/4"

2. [UE Editor MCP] query_scene_component("PlanarReflection_01")
   → 确认 ScreenPercentage=25（这是 1/4 的来源）

3. [UE Editor MCP] query_material("M_Building_01")
   → 确认材质开启了 "Used with Planar Reflection"
   → 查看是否有自定义的反射亮度补偿参数

4. [UE Editor MCP] list_shader_permutations("M_Building_01")
   → 对比 REFLECTION=0 和 REFLECTION=1 变体的编译选项差异
   → 确认 REFLECTION=1 确实移除了 IBL specular

5. [Claude Code 推理] 综合 RenderDoc 帧数据 + 引擎配置：
   → "问题来自两方面叠加：
      a) shader REFLECTION=1 变体移除了 IBL specular（-15% 亮度）
      b) PlanarReflection blend 使用 SrcAlpha 且 alpha=0.85（再 -15%）
      总计 -28% 亮度，与实测吻合。"

6. [Filesystem] 修改 shader 的 REFLECTION 分支，保留简化版 IBL
7. [UE Editor MCP] 修改 PlanarReflection 的 blend 参数（如果需要）
8. [Git MCP] 提交修复
```

### 暂时没有 UE Editor MCP 时的替代方案

如果 UE Editor MCP 还没实现，Claude Code 可以通过 Filesystem 直接读取配置文件来获取部分信息：

```
- 项目渲染设置  → Config/DefaultEngine.ini 中的 [/Script/Engine.RendererSettings]
- Console 变量  → Config/DefaultEngine.ini 或 DefaultDeviceProfiles.ini
- 材质信息      → 需要解析 .uasset（二进制格式，较难直接读取）
```

这种方式能覆盖约 50% 的引擎配置查询需求，材质和场景组件的查询需要等 UE Editor MCP。

---

## P1：ADB / Device MCP — 多设备复现和验证

### 为什么是 P1

移动端渲染 Bug 的一个核心难点是**设备差异性**。同一个帧在 Adreno 上爆闪、在 Mali 上正常，或者在某个驱动版本上才复现。需要能：

1. 在多台设备上触发截帧
2. 收集不同设备的 GPU 日志和错误信息
3. 修复后在目标设备上验证

### 关键工具

```json
{
  "name": "adb_list_devices",
  "description": "列出所有已连接的 Android 设备及其 GPU 信息"
},
{
  "name": "adb_capture_frame",
  "description": "在指定设备上触发 RenderDoc 截帧，返回 .rdc 文件路径"
},
{
  "name": "adb_pull_file",
  "description": "从设备上拉取文件（截帧、日志等）"
},
{
  "name": "adb_logcat",
  "description": "获取设备 logcat 输出，可按 tag 过滤（如 Vulkan validation layer 日志）"
},
{
  "name": "adb_gpu_info",
  "description": "获取设备的 GPU 型号、驱动版本、支持的扩展列表"
}
```

### 典型流程（跨设备爆闪排查）

```
1. [ADB MCP] adb_list_devices()
   → 设备 A: Adreno 640 (已知爆闪)
   → 设备 B: Mali-G78 (正常)

2. [ADB MCP] adb_capture_frame(device="A") → frame_adreno.rdc
   [ADB MCP] adb_capture_frame(device="B") → frame_mali.rdc

3. [RenderDoc MCP] load_capture("frame_adreno.rdc")
   [RenderDoc MCP] diagnose_negative_values()
   → Adreno 上 SceneColor 有 47 个负值像素

4. [RenderDoc MCP] load_capture("frame_mali.rdc")
   [RenderDoc MCP] diagnose_negative_values()
   → Mali 上 SceneColor 有 0 个负值像素

5. [Claude Code 推理]
   → "负值只在 Adreno 上出现，说明这不是 shader 逻辑错误，
      而是 Adreno 的 mediump 精度导致 SH 采样在边界法线方向产生了负值。
      Mali 的 mediump 精度更高所以没有触发。"
   → "修复方案：在 SH 采样后加 max(0, x) clamp（两个平台都加，defensive coding）"

6. [Filesystem] 修改 shader
7. [ADB MCP] 部署到设备 A 上验证
```

---

## P2：Performance Log MCP — 补充跨帧 + CPU 视角

### 解决什么问题

RenderDoc 只能看一帧。但有些性能问题是跨帧的：

- TAA 的负值累积需要多帧才能观察到扩散
- 帧率波动可能是 CPU 端 Game Thread 卡顿引起的
- GPU 发热降频导致的渐进式性能下降

### 关键工具

```json
{
  "name": "read_stat_log",
  "description": "读取 UE 的 stat 日志文件（stat unit / stat gpu 的输出）"
},
{
  "name": "read_csv_profile",
  "description": "读取 CSV 格式的性能 profiling 数据（如 UE 的 CSV profiler 输出）"
},
{
  "name": "analyze_frame_time_series",
  "description": "分析帧时间序列数据，检测卡顿 spike、渐进式降速、周期性波动"
}
```

---

## P2：Knowledge Base MCP — GPU 硬件知识库

### 解决什么问题

RenderDoc MCP 的 `get_capture_info` 可以返回 GPU 型号，但 Claude Code 需要知道这个 GPU 有什么已知问题。比如：

- Adreno 5xx 系列的 `texelFetch` 在某些 buffer 格式下返回错误值
- Mali T8xx 的 `imageStore` 在 R11G11B10_FLOAT 上有未定义行为
- PowerVR 的 framebuffer fetch 在 MRT 模式下性能严重下降

### 实现方式

不一定需要独立 MCP。可以作为一个**知识文件**集成到 Claude Code 的上下文中：

```
/project/.claude/gpu-known-issues.md

# Adreno 已知问题
## Adreno 640 (Vulkan)
- mediump float 精度：实际为 FP16 最低要求，SH 采样边界情况可能产生负值
- R11G11B10_FLOAT：负值 clamp 到 0（非未定义行为，但不同于桌面端的 saturate）
- ...

# Mali 已知问题
## Mali-G78
- ...
```

或者更好的方式：在 `get_capture_info` 的 `known_gpu_quirks` 字段中直接内嵌，不需要额外工具。

---

## 最大化 RenderDoc MCP 调试能力的策略

除了搭配其他 MCP，还有一些策略可以显著提升 RenderDoc MCP 自身的效果。

### 策略 1：优化截帧质量

截帧的质量直接决定 RenderDoc MCP 能分析出多少信息。

```
截帧配置建议：
───────────────────────────────────────────────────────────
✅ 编译 shader 时保留 debug info（-g 或 -Od）
   → debug_shader_at_pixel 能拿到精确的行号和变量名映射
   → get_shader_source 返回的代码更接近原始源码

✅ 开启 Vulkan Validation Layer 后截帧
   → 部分验证信息会被包含在捕获中

✅ 给 draw call 和 pass 加 debug marker / label
   → list_events 返回的名称更可读（"SM_Rock_01" 比 "DrawIndexed(3456)" 有用得多）
   → UE 默认会加一些 marker，但自定义的更好

✅ 截"有问题的帧"和"正常的帧"各一帧
   → compare_pipeline_state 可以做帧间对比
   → 如果 Bug 是间歇性的，需要等到复现时截帧

❌ 不要在 Release 配置下截帧做 shader 调试
   → Release 的 shader 优化会改变代码结构，debug 信息丢失
   → 用 Development 或 Debug 配置截帧
```

### 策略 2：构建标准化的诊断 Prompt

为常见 Bug 类型预设诊断 Prompt，让 Claude Code 自动执行完整的诊断链路：

```markdown
## 标准诊断流程 Prompt

### 爆闪排查
"加载 {rdc_path}，执行以下诊断：
1. get_capture_info 确认设备
2. diagnose_negative_values 扫描全帧
3. 对每个检测到负值的 RT，用 sample_pixel_region 定位爆闪区域
4. 对爆闪最严重的像素，执行 pixel_history 和 debug_shader_at_pixel
5. diagnose_precision_issues 检查格式限制
6. 综合以上信息给出根因和修复方案"

### 倒影 / 反射问题
"加载 {rdc_path}，执行以下诊断：
1. diagnose_reflection_mismatch 做初步对比
2. 对差异最大的物体，compare_pipeline_state 对比 state
3. get_shader_source 对比两个变体的代码差异
4. 如果差异在常量值，用 get_shader_constants 对比
5. 给出具体的修复方案和亮度影响预估"

### 性能优化
"加载 {rdc_path}，执行以下诊断：
1. get_capture_info 确认设备
2. get_pass_timing 找最耗时的 pass
3. analyze_overdraw 检查 fill rate
4. analyze_bandwidth 检查带宽
5. analyze_state_changes 检查合批机会
6. diagnose_mobile_risks(categories=['performance'])
7. 按 ROI（优化收益/实现难度）排序给出优化建议"
```

### 策略 3：建立对比分析的工作流

很多渲染 Bug 的诊断本质上是**对比**：

```
对比维度              用途                           RenderDoc MCP 支持
──────────────────────────────────────────────────────────────────
帧内对比              正常 DC vs 反射 DC             compare_pipeline_state
                                                    diagnose_reflection_mismatch

帧间对比              正常帧 vs 爆闪帧               load_capture 切换两个 .rdc
                                                    然后用相同工具分析

设备间对比            Adreno 帧 vs Mali 帧           同上（配合 ADB MCP 截帧）

版本间对比            修改前 vs 修改后               配合 Git MCP 定位代码变更

Pass 间对比           BasePass 输出 vs PostProcess 输出  pixel_history 追踪完整链路
```

建议在截帧流程中养成习惯：**每次截帧都同时保存一个"已知正常"的参考帧**。这样 Claude Code 在诊断时可以做帧间对比。

### 策略 4：让智能诊断工具可扩展

模块 8（智能诊断）的 4 个工具覆盖了你提到的场景，但实际工作中一定会遇到新的 Bug 类型。建议设计一个**通用诊断脚本框架**：

```json
{
  "name": "run_custom_diagnostic",
  "description": "执行一段自定义的诊断脚本。脚本可以调用所有底层工具的能力。用于快速添加新的诊断逻辑而不需要修改 MCP Server 代码。",
  "inputSchema": {
    "type": "object",
    "properties": {
      "script_path": {
        "type": "string",
        "description": "Python 诊断脚本的路径，脚本中可以调用 RenderDoc ReplayController 的所有 API"
      },
      "params": {
        "type": "object",
        "description": "传递给脚本的参数"
      }
    },
    "required": ["script_path"]
  }
}
```

这样当你遇到新类型的 Bug（比如某天需要诊断"某个 compute shader 的 SSBO 输出错误"），Claude Code 可以直接写一个诊断脚本并执行，不需要等 MCP Server 更新。

---

## 推荐的实施路径

```
阶段 1（1-2 周）：最小可用
────────────────────────
RenderDoc MCP P0 工具（9 个基础工具）
+ Claude Code 自带的 Filesystem 能力
→ 能完成：基本的帧浏览、shader 查看、像素诊断、简单修复

阶段 2（2-4 周）：核心场景覆盖
────────────────────────
RenderDoc MCP P1 工具（+6 个高价值工具）
+ Git 集成（Claude Code 直接用 git CLI）
→ 能完成：爆闪诊断、负值追溯、反射对比、性能分析、代码修复+提交

阶段 3（1-2 月）：完整体系
────────────────────────
RenderDoc MCP P2 工具（+7 个增强工具）
+ UE Editor MCP（如果需要引擎层面的查询）
+ ADB 集成（用于多设备测试）
→ 能完成：全自动化的端到端诊断、跨设备对比、移动端风险预检

阶段 4（持续迭代）：智能化
────────────────────────
积累诊断案例 → 沉淀为标准化 Prompt 和自定义诊断脚本
不断扩充 GPU 已知问题知识库
根据实际使用中的痛点添加新的智能诊断工具
```

---

## 各场景下的完整工具链

### 场景：倒影偏暗（完整闭环）

```
[RenderDoc MCP]  diagnose_reflection_mismatch    → 定位差异
[RenderDoc MCP]  debug_shader_at_pixel           → 确认 shader 级别的原因
[Filesystem]     读取 MobileBasePassPixelShader.usf  → 找到源码
[Git]            git blame 该文件                  → 确认最近谁改了反射分支
[Filesystem]     修改 shader 代码                  → 修复
[Git]            提交到 fix 分支                   → 版本管理
[ADB] (可选)     在目标设备上截帧验证               → 确认修复有效
```

### 场景：TAA 爆闪（完整闭环）

```
[ADB] (可选)     在爆闪设备上截帧                   → 获取 .rdc
[RenderDoc MCP]  get_capture_info                  → 确认 GPU 型号
[RenderDoc MCP]  diagnose_negative_values           → 定位负值源头和 TAA 放大
[RenderDoc MCP]  debug_shader_at_pixel              → 追踪 TAA shader 执行过程
[RenderDoc MCP]  diagnose_precision_issues           → 确认 R11G11B10 格式限制
[Filesystem]     读取 TAA shader 和 IBL shader       → 找到需要修改的代码
[Filesystem]     修改：IBL clamp + TAA history clamp  → 双重修复
[Git]            提交修复                            → 版本管理
[ADB] (可选)     多设备验证                          → 确认不同 GPU 都正常
```

### 场景：性能优化（完整闭环）

```
[RenderDoc MCP]  get_pass_timing                   → 找瓶颈 pass
[RenderDoc MCP]  analyze_overdraw                   → 找 fill rate 热点
[RenderDoc MCP]  analyze_bandwidth                  → 找带宽瓶颈
[RenderDoc MCP]  analyze_state_changes              → 找合批机会
[RenderDoc MCP]  diagnose_mobile_risks              → 综合风险评估
[Filesystem]     修改渲染代码 / 项目配置             → 实施优化
[Git]            提交优化                            → 版本管理
[RenderDoc MCP]  加载优化后的帧，重新分析             → 验证优化效果
```

---

## 总结

**RenderDoc MCP 单独就是一个强大的诊断引擎**，覆盖了帧级别分析的绝大部分需求。但要实现从"发现问题"到"修复问题"的完整闭环：

- **必须搭配**：Filesystem（读写代码）+ Git（版本管理）— 这两个 Claude Code 已经自带
- **强烈推荐**：UE Editor MCP（理解引擎层面的 why）+ ADB（多设备支持）
- **锦上添花**：Performance Log（跨帧分析）+ 知识库（GPU 硬件特性）

最大化 RenderDoc MCP 效果的关键不在于搭配多少工具，而在于：
1. **截帧质量**：shader debug info + debug marker + 保存参考帧
2. **对比思维**：帧内对比、帧间对比、设备间对比、版本间对比
3. **诊断沉淀**：把每次排查的经验变成标准化 Prompt 和可复用的诊断脚本
