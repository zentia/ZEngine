# Phase 2 · Step 2.1 前置调查：RenderGraph 接管 RP2「默认打开」可行性评估

> 本文档是 Phase 2 step 2.1（`PHASE2_PLAN.md`）的**动手前只读调查**（2026-10-25），
> 结论沉淀于此，动手后再在 `PHASE2_PLAN.md` §3 追加"落地记录"。
>
> 结论等级：**建议「先目视验证、后翻默认」，不盲目翻默认**。
> 环境事实：本次为纯只读调查，未构建、未运行（本机无 cmake/ninja/cl 在 PATH，Windows 走 VS
> 生成器需环境配置，不适合在无头/无人值守下强行构建）。所有判断基于代码与既有断言。

---

## 0. 一句话结论

**代码层面「默认打开」是安全的**（图驱动路径每一处失败点都在开始录制命令前 `return false`，
失败即干净回落到老路径，最坏情况是回退而非花屏/黑屏）。**但真正的闸门不是代码安全，而是逐帧
目视 diff 验证**——这一步在无头环境做不了。因此**不建议直接翻默认**，而应先由有显示器的一方
带 `ZENGINE_RG_RP2=1` 跑一次与老路径逐帧对比，确认等价后再翻。

---

## 1. 现状（动手前已读全相关文件）

**RenderGraph 家底扎实**（`Runtime/Function/Render/Graph/`，5835 LOC）：
- 编译器 `RenderGraphSmokeTest`：**165 项无头断言 / 0 失败**（`RENDER_GRAPH.md:48, :233`）——
  链式排序、剔除与反向传播、NeverCull/Extract 保活、生命周期、别名复用与重叠拒绝、屏障推导、
  重复声明拒绝、跨帧失效句柄、四类校验错误、WAR/WAW 顺序、缓冲资源、确定性、描述符尺寸、
  RP2 后处理链（43 项）、swapchain 往返（15 项）、接线真实导入态（29 项）。
- 执行层 `RenderGraphExecuteSmokeTest`：**92 项无头断言 / 0 失败**（`RENDER_GRAPH.md:49, :235`）——
  池复用与溢出销毁、`Flush()` 只减一次、Pass 按编译序执行、句柄解析与跨代失效拒绝、每 Pass 一次
  `CmdPipelineBarrier` 且转对 image、transient 最后读者后立即归还、被剔除 Pass 不执行不分配、
  Extract 资源活过整图、缺 device/cmd/校验失败的报错路径、无 device 的池惰性而非崩。
- 两个测试均为**可选目标**（`Graph/Test/CMakeLists.txt`，`ZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST` /
  `..._EXECUTE_SMOKE_TEST` 默认 OFF），需 `-D` 打开后 `cmake --build build --target RenderGraphSmokeTest
  RenderGraphExecuteSmokeTest`。**本机未构建**（无 cmake 在 PATH）。

**RP2 已被图接管（但默认关闭）**（`Passes/MainCameraRp2Pass.cpp/.h`）：
- 门控：环境变量 `ZENGINE_RG_RP2`（`SetupRenderGraphPath()`，`MainCameraRp2Pass.cpp:178-200`）——
  非空且非 `'0'` 即开；不开时**一个对象都不构造**，老路径零改动。
- 调用点：`Draw()`（`:670`）`if (m_UseRenderGraph && DrawRP2WithGraph(...)) return;` 否则走手写链。
- 图驱动链：ColorGrading → (可选)FXAA → UI(NeverCull) → CombineUI（`DrawRP2WithGraph`，`:875-1108`）。

---

## 2. 核心判断：为什么代码层面「默认打开」是安全的

**关键不变量：`DrawRP2WithGraph()` 的每一处拒绝点都在开始录制命令前返回 false**（`:891-894`
注释明确点出这个约束——"Once Execute() starts recording there is no way back"）。逐点核实：

| 失败点 | 位置 | 时机 | 回落 |
|--------|------|------|------|
| executor/graph 为空 | `:878-881` | 录制前 | `return false` |
| cmd 为空 | `:883-887` | 录制前 | `return false` |
| swapchain framebuffer 越界/空 | `:895-900` | 录制前 | `return false` |
| swapchain image/imageView 为空（WebGL2/Metal 不发布 image） | `:902-919` | 录制前 | 打**一次** WARNING 后 `return false` |
| backup_odd/even image/view 为空 | `:921-929` | 录制前 | `return false` |
| FXAA 开启但 post_odd 缺失 | `:974-979` | 录制前 | `return false` |
| `Compile()` 结果 invalid | `:1077-1094` | 录制前 | 打**一次** ERROR（含每条 error）后 `return false` |
| `Execute()` 失败 | `:1096-1105` | 仅在未录制任何命令时可能失败 | 打 ERROR 后 `return false` |

⇒ **不存在"图驱动画了一半、回落后又用手写链重画"的中间态**。任一环节不合法，老路径
（`Draw()` 中 `:675` 起的手写链）**原样画这一帧**，行为与今天默认关闭时完全一致。

**两条防刷屏保证**：`m_RgFallbackLogged`（WARNING/ERROR 只打一次）+ 编译失败是"每帧重建同样的图、
失败是永久性的"（`:1080-1081` 注释），不会每帧刷屏。

**结论**：翻默认的最坏情况不是黑屏/花屏，而是"若图路径在某后端/某配置下不合法，就静默回落到
老路径"—— 这恰恰是**理想的稳态**（图路径只在它被证明可用的地方生效）。真正需要规避的是
**"图路径悄悄画错但没回落"**，而这被上述"先校验后录制"的不变量堵死了。

---

## 3. 那为什么仍不建议现在直接翻默认

1. **唯一未闭合的验证是逐帧目视 diff**（`RENDER_GRAPH.md:175`："仍需逐帧目视验证（无头环境
   做不了）"）。257 项无头断言证明的是**编译器/执行器的正确性**（屏障条数、执行序、剔除、别名），
   但**不能替代**"图驱动链与手写链在真实后处理链上像素级一致"的人眼确认——尤其 ColorGrading/FXAA
   这类对浮点顺序敏感的色调映射。
2. **一处已知脆弱点**（`RENDER_GRAPH.md:173`）：`UI` Pass 不声明任何资源，只靠 `NeverCull` +
   拓扑排序"同分取最小声明下标"钉在 FXAA 与 CombineUI 之间（group 18 断言了执行序）。**Pass 声明
   顺序一旦变动就会失效**——翻默认前值得确认这条链的稳定性，否则默认打开后一次无心的 Pass 调整
   会静默改变 UI 合成顺序。
3. **WebGL2/Metal 天然回落**：这两后端不发布 swapchain image，图路径直接 WARNING 回落（`:908-918`）——
   所以翻默认对它们零风险，但也意味着"默认打开"实际只在发布 image 的后端（Vulkan/DX12）生效。

---

## 4. 建议的落地路径（翻默认前必须做的最小闭环）

| 子步 | 动作 | 谁来做 / 前置 |
|------|------|--------------|
| **2.1a** | 本机/真机带 `ZENGINE_RG_RP2=1` 跑编辑器，与老路径**逐帧目视对比**后处理链（ColorGrading/FXAA/UI/CombineUI） | **需有显示器的一方**（无头做不了）；DX12 编辑器即可 |
| **2.1b** | 若目视等价 → 把 `SetupRenderGraphPath()` 的默认从"env 非空非'0'"改为**默认开**，并保留一个 `ZENGINE_RG_RP2=0` 的关闭逃生阀 | 改动极小（~5 行）；可在 Windows 双配置编译验证 |
| **2.1c** | 回归两个 smoke test（165 + 92 断言）确认翻默认未破坏图正确性 | 需 cmake + `-DZENGINE_BUILD_RENDERGRAPH_*_SMOKE_TEST=ON` |
| **2.1d** | 更新 `RENDER_GRAPH.md:167` "默认关闭" 表述 + `PHASE2_PLAN.md` 落地记录 | 文档 |

**风险兜底**：即便 2.1a 发现不等价，也**无需回滚代码**——不翻默认即可，图路径继续靠 env 门控
灰度；老路径零改动。这是"默认打开"方案最大的安全垫。

---

## 5. 与 Phase 2 后续的关系

- **2.2（RG 扩面 ParticlePass/后处理链）**：依赖 2.1b 的默认打开作为基线，复用同一套"先校验后录制"
  回落机制。
- **2.3-2.4（RP1 三分 → RG 接管 RP1）**：RP1 内部靠 `subpassInput`，图当前只能表达"整段"级别
  （`RENDER_GRAPH.md:117`）——所以 2.3 的"RP1 三分"是 2.4"RG 接管 RP1"的**必要前置**，顺序不可颠倒。
- **Nanite/Lumen/VT（2.5-2.7）**：都依赖 2.4 提供的"图管理的瞬态 RT 池 + 自动屏障"，且都复用
  `IRGResourceDevice`（10 方法，`RENDER_GRAPH.md:192-212`）这一"窄接口 adapter"模式——正是 Phase 2
  让 RHI 胖接口"涌现式"拆分（`PHASE2_PLAN.md` §3）的抓手。

---

## 6. 本次调查未覆盖 / 待补

- **未构建 smoke test**：本机无 cmake/ninja/cl 在 PATH。2.1c 的 257 断言回归需在有工具链的环境
  （用户机器，或重启后的 WSL——但 WSL 是 Linux，这两个测试是跨平台 C++、WSL 里也能跑）。
- **未做目视 diff（2.1a）**：这是翻默认的唯一硬闸门，必须有显示器的一方执行。
