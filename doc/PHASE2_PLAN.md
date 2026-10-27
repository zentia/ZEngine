# 阶段二 · 建能力（Capability Building）规划草案

> 本文档是 ZEngine "阶段二·建能力" 的**单一源**（对标阶段一 `PHASE1_FOUNDATION_PLAN.md`）。
> 阶段一（"夯地基"，稳定性/正确性审计补齐，#1–#12 全部落地）已完成，阶段二从**现有设计蓝图
> 与代码现状**里识别主线，不凭空造需求。
>
> 结论等级：沿用阶段一的 **Blocker / High / Medium / Low**（此处表示"作为 Phase-2 主线的优先级
> 与风险"，而非审计严重度）。
> 环境事实：阶段一已把四个 RHI 后端（Vulkan/D3D12/Metal/WebGL2）在各自目标上编译链接打通；
> 阶段二**每一步仍以"可验证、风险可控、能力可见"为纲**，但允许包含较大的渲染/内容管线能力。
>
> ⚠️ 本文档为**草案**（2026-10-25 只读盘点产出，尚未动手）。动手前每个子项必须回读对应文件
> 确认现状未变，并在此追加"落地记录"。

---

## 0. 盘点方法与环境事实（重要）

规划前做了一次全量只读盘点（`doc/` 87 篇设计文档 × `engine/Source` 代码 grep 交叉验证），
三条必须先记录在案的事实，否则会基于错误前提排期：

1. **多篇"系统设计文档"是过时的概念稿，代码远比文档先进**。`doc/nanite/NANITE_SYSTEM_DESIGN.md`、
   `doc/lumen/LUMEN_SYSTEM_DESIGN.md` 是纯散文、无状态标记，但代码各约 4000+ LOC，已有 CVar
   （`r.Nanite.*`/`r.Lumen.*`）、管线挂钩（`RenderPipelineBase::UpdateNanite/UpdateLumen`）、
   真 GPU dispatch（`nanite_cluster_cull.comp` + `.spv`）与 smoke test。**规划要看代码，不要看这两篇
   文档**，否则工作量会估错一个数量级。同理 `doc/asset_management/` 两篇 40KB/34KB 大设计稿自标
   "**早期设计草稿（pre-Route-B）**"。

2. **大量子系统"已实现"**：World Partition + LWC、序列化（YAML 对象图）、Prefab、Undo/Redo、
   DDC/LMDB 缓存、MegaLights、Preload/纹理流送、EASTL、Bindless 纹理、Apple/Metal 验收、
   Package Manager、GPU Spine（活跃主线）—— 这些**不需进 Phase 2**（个别有收尾 follow-up，见 §2）。

3. **可行动 TODO 高度集中**：`engine/Source` 约 152 处 TODO/FIXME（剔除 vendored `zstb_truetype.h`
   13 处后约 139 处），**~80% 集中在 Nanite + Lumen + VirtualTexture + RHI 后端**——且都是各系统
   最硬的那一半（GPU 化/流送/HWRT），这正是它们此前没做完的原因。

---

## 1. Phase-2 候选总表（按推荐优先级）

> 评级 = 作为 Phase-2 主线的价值/风险。★★★ 首选，★★ 主力，★ 填缝/收尾，✗ 不建议进。

| # | 候选 | 主题 | 成熟度 | 依赖 | 评级 |
|---|------|------|--------|------|------|
| C0 | **Vulkan `MainCameraPass` 崩溃根因修复** | 跨切面阻塞：解除 Vulkan 侧验证能力 | **✅ 已被 `7e5d2521` 修掉（2026-10-25 复核关闭）** | — | ★★★ 已关闭 |
| C1 | **RenderGraph 接管流水线** | 帧图编译器：自动屏障、瞬态 RT 别名、剔除 | **✅ 已默认开·真机验证 PASS**（165 编译+92 执行断言；RP2 图路径 2026-10-26 翻默认开 `c0d1bcfc`，同帧 A/B 逐位等价 + soak 无异常，见 §2 落地记录·2.6） | 阶段一 RHI 注册表 | ★★★ 高价值/低风险 |
| C2 | **RenderGraph → RP1**（`render_pass_refactor`） | RP1 三子pass 拆成 3 独立 pass，纳入图管理 | **✅ 已默认开·真机验证 PASS**（2.3 三分已在代码：3 独立 render pass/subpass 恒 0；2.4 compiler 200/200 + executor 121/121 已验。`DrawRP1WithGraph` 2026-10-26 翻默认开 `871878e3`，同帧 A/B `rp1.backup_odd` 逐位 IDENTICAL + soak 无异常，见 §2 落地记录·2.6） | **必须在 C1 之后** | ★★ 高价值/中风险 |
| C3 | **Nanite 收尾** | 流送 → 几何压缩 → HiZ 遮挡 | **大部分实现**（真 GPU dispatch 已有；几何压缩✅ + streaming✅ 已落地；**HiZ CPU 侧✅ 已落地 2026-10-26**，见 §2 落地记录·2.5 HiZ；剩 HiZ GPU 侧——compute 生成金字塔 + GPU 遮挡剔除，需真机） | C1/C2 给其 RG 管理的目标 | ★★★ 高价值/中风险 |
| C4 | **Virtual Texture 接线** | 分页纹理虚拟化，从"孤儿"到可用 | **已实现但零消费者**（6 文件无外部引用、无 CVar、3 处构建排除） | C1/C2 的 RG 资源注册 | ★★ 高价值/中高风险 |
| C5 | ~~**Vulkan `MainCameraPass` 崩溃根因修复**~~ | 跨切面阻塞：解除 Vulkan 侧验证能力 | **✅ 已被 `7e5d2521` 修掉（2026-10-25 复核关闭）** | — | ★★★ 已关闭（见 §2 step 2.0） |
| C6 | **Lumen 收尾（SSR → GPU 化）** | 动态 GI + 反射；取 SSR 一半 + 首个 Lumen shader | **✅ CPU 侧已完整**（GI/反射/接线/CVar/发布桥全落地 + 7 smoke test；2026-10-25 复核，见 §2 落地记录·2.7）。剩 SSR/HWRT/首个 shader 全 GPU 侧 | C2 的 GBuffer/depth 声明 | ★★ 中价值/高风险（CPU 侧无活，剩 GPU 放最后） |
| C7 | **编辑器/内容管线填充**（低风险稳态轨） | 平台窗口化收尾 / Timeline 四片段 / Prefab §9.4 | 各为**部分实现·机械收尾**（**Timeline 四片段 2026-10-25 已落地**，见 `doc/timeline/TIMELINE_IMPLEMENTATION.md`“片段语义”；**Prefab §9.4 第 1 条落地、第 2 条对象图层已验**，见 `doc/PrefabSystem_Design.md` §9.4.1，顺带修 7 个真 bug；平台窗口化未动；**平台窗口化 2026-10-25 对代码核对**：Step 1-5 早已兑现（零 GLFW 引用），计划文档勾选已按代码更新；核对时修了 Win32 修饰键恒 0 / 拖放不派发 / Linux 原生 #error / Web 启动空指针等，见 `doc/platform_windowing_plan.md`；剩 Linux X11 窗口层与多显示器枚举） | — | ★★ 低风险填缝 |
| C7b | **Linux X11 窗口层 + 多显示器枚举**（C7 收尾） | 原生 X11 后端 + XRandR 多屏 | **代码已落地·未真机实测**（2026-10-26，见 `doc/platform_windowing_plan.md`「Linux X11 落地记录」）：`Platform/Linux/LinuxWindow`+`LinuxApplication`（xlib 窗口/事件/剪贴板/光标）+ `vkCreateXlibSurfaceKHR`；XRandR 多显示器枚举（顺带覆盖 C7 的多屏子项）。工厂返回 `LinuxApplication`，CMake `find_package(X11/Xrandr)`+链接，窗口层 TU 用 `Z_PLATFORM_LINUX && !__EMSCRIPTEN__` 守卫。**本机 WSL 被安全策略拦、无 X11 SDK，未编译/运行**——按 Windows/MacOS 接口契约镜像，待 Linux 多屏真机按 doc 步骤验 | — | ★★ 中风险（未验证） |
| C8 | **运行时 UI（UMG）补齐** | 数据绑定 / 焦点导航 / 动画 / 锚点 / widget 广度 | **可本地验证部分全部落地**（2026-10-27，见 `doc/RUNTIME_UI_IMPLEMENTATION_PLAN.md` 与 §2 落地记录·穿插 UI）：P0 数据绑定（`UMGEventRegistry` 22 断言）+ 焦点导航 + 逐帧 Tick（`UMGTree` 全绿）；P1 动画纯核心（`UMGAnimation` 43 断言）+ 锚点换算（`UMGAnchor` 17 断言）；P2 widget 广度（`UProgressBar`/`UComboBox`/`UTabControl`/`USafeZone`，ZRuntime 干净编译）。**剩全是真机绑定**：ZSlate paint 链（RenderOpacity 乘 tint / RenderTransform 折进几何）、`FGeometry::MakeChild` 喂锚点结果、UISystem 取帧 delta 驱动 TickTree、手柄导航、UTabControl 活动页读回、USafeZone 真实 per-platform 安全区 | — | ★★ 高价值（CPU 侧已清，剩真机） |
| — | **RHI 胖接口拆分**（阶段一 #9 遗留） | 124 纯虚 `RHI.h` 拆成职责子接口 | **未动手**（阶段一明确后置） | 触碰 4 后端 + 所有调用点 | ✗ **不建议进 Phase 2**（见 §3） |

---

## 2. 建议落地顺序（每步可验证、风险可控、能力可见）

原则继承自阶段一（`PHASE1_FOUNDATION_PLAN.md:49-53`）：先低风险高杠杆、每步能本地跑通；
阶段二额外要求"能力可见"—— 每个里程碑都应带来可演示的渲染/内容能力。

| 步骤 | 对应候选 | 子项 | 为什么排这里 |
|------|----------|------|--------------|
| **2.0**（✅ 已关闭） | C0 | Vulkan `MainCameraPass::SetupModelGlobalDescriptorSet` 崩溃根因修复 | **已被 `7e5d2521`（2026-10-22）修掉**，2026-10-25 复核确认关闭。根因：MegaLights 默认关闭时 binding 8..12 提交空资源 → `UpdateDescriptorSets` 解引用空指针；修复加判空早退 + RHI 跳过空 write。两篇文档（`CROSS_PLATFORM_PIPELINE.md` §5、`TEXTURE_COOK_PIPELINE.md` §8）写于 2026-06-02、早于修复，已加"已修复"标注。**Vulkan 验证通路已恢复，无需再动手**。 |
| **2.1**（✅ 已默认开） | C1 | RenderGraph RP2 目视验证 → 默认打开（**前置调查见 `PHASE2_STEP2.1_RENDERGRAPH.md`；默认开于 `c0d1bcfc`**） | 图与执行层已有 257 项断言；失败模式是"后处理不对"而非黑屏（`RENDER_GRAPH.md:136`）；一个环境变量可回滚。是所有后续渲染候选的地基。**代码层面默认打开安全**（每处失败点录制前 `return false` 干净回落）。**2026-10-26 翻默认开**：同帧 A/B 抓帧 `rp2.backup_even`/`rp2.backbuffer` 逐位 IDENTICAL + soak 无异常，见落地记录·2.6。 |
| **2.2** | C1 | RenderGraph 扩面（ParticlePass / 后处理链） | 复用已验证机制；消除 `RENDER_GRAPH.md:15` 列举的手写屏障。 |
| **2.3** | C2 | `render_pass_refactor`：RP1 三分 | **必须在 2.1 之后**——此时屏障/别名机制已在真机验证过，单独做它是高风险。 |
| **2.4** | C2 | RenderGraph 接管 RP1 | 2.3 的直接收益兑现；解锁 Nanite/Lumen/VT 的图管理资源。**编译器层已于 2026-10-25 兑现**（见下方落地记录·2.4）：step 2.3（RP1 三分）此前已落地在代码（3 个独立 render pass，subpass 恒 0），本步骤把 RP1 真实拓扑声明进图（`BuildRp1Wiring`），`RenderGraphSmokeTest` group 19/20 断言执行序 + 9 条屏障（4+5+0）+ transient 生命周期，headless WASM 200/200 全绿；`RENDER_GRAPH.md:117`"图只能表达整段"的旧论断被推翻并加更新注记。**executor 层二次复核**：撤回上一版的"两处语义缺口"（RP1 只在 DX12/Metal 跑，INPUT_ATTACHMENT 在那里就是 SRV；Forward 的 blend 是固定功能，本就是 RenderTarget），并修掉一个真 bug——导入的深度图拿到 COLOR aspect 屏障、D32 拿到 STENCIL aspect（executor group 13/14 回归，红绿验证）。`DrawRP1WithGraph` 已实现（DX12 only；生产声明 `MainCameraRp1GraphDecl.h` 与测试共用，compiler 239/239 + executor 130/130，DX12 TU clang-cl 类型检查通过）。**2026-10-26 翻默认开**（`871878e3`）：同帧 A/B 抓帧 `rp1.backup_odd` 逐位 IDENTICAL + soak 无异常，见落地记录·2.6。 |
| **2.5** | C3 | Nanite：streaming → 几何压缩 → HiZ 遮挡 | 三个子项风险递增、各自独立可验（streaming/压缩是纯 CPU 数据、可 headless 验；HiZ 是 GPU 侧需真机）；已有真 GPU dispatch 作基线。**几何压缩已于 2026-10-25 落地**（见下方落地记录）：新增 `GeometryCodec::{Raw,Quantized}` 有界有损量化编码，WASM headless 验证 67/67 全绿，vertex_data 10368→5208B、index_data 3456→1728B。**streaming 已于 2026-10-25 落地**（见下方落地记录）：`UpdateStreaming` 从 TODO 兑现为基于距离的叶子驻留判据，WASM headless 验证 77/77 全绿（含 10 项新增 streaming 断言）。剩 HiZ（GPU 侧需真机）。 |
| **2.6** | C4 | VirtualTexture 接线（CVar 门控） | 孤岛子系统，不会回归现有路径；需要 2.4 的 RG 资源注册。**已于 2026-10-25 落地**（见下方落地记录）：9 处 TODO 里 6 处 CPU 侧填实（屏幕投影 mip/优先级重算/LRU 淘汰/统计/时间戳/世界足迹），3 处 GPU 侧（页上传/页表纹理）如实留作真机；加 `r.VirtualTexture.Enable` CVar 门控 + 帧循环 `UpdateVirtualTextureForFrame`（3 条 Tick 路径统一入口，默认关、Vulkan 编译门内、零回归）；headless WASM 验证 15/15 全绿。剩页上传/页表纹理（GPU 需真机）+ 2.4 的 RG 资源注册。 |
| **2.7**（✅ CPU 侧已完整·无需动手） | C6 | Lumen：SSR + 首个 Lumen shader | 依赖 2.4 的 GBuffer/depth 声明；放最后，**只取 SSR 一半**。**HWRT/BLAS-TLAS 明确移出 Phase 2**（需独立专项）。**已于 2026-10-25 复核：Lumen 的整个 CPU 侧（GI + 反射 + 接线 + CVar + 发布桥）此前就已完整落地并有 7 个 smoke test**，见下方落地记录。剩 SSR/HWRT/首个 shader 全是 GPU 侧（需真机 + 2.4 的 GBuffer/depth），本步骤的 CPU 部分无活可干。 |
| **穿插** | C7 | 平台窗口化收尾 / Timeline 四类片段 / Prefab §9.4 follow-ups | 渲染项卡在真机验证时用来保持节奏；每项都可独立编译演示，低风险。 |

---

### 落地记录 · 2.5 几何压缩（2026-10-25）

**做了什么**：把 `NaniteCluster.cpp` 里那个长期空转的 `TODO: 实现几何压缩` 兑现为一个**版本化、
可选的几何编码格式** `GeometryCodec`（version `zengine-nanite-geom/1`，定义在 `NaniteTypes.h`）：

- **`Raw`（默认）**：未压缩的 `Vector3`(12B) + `uint32`(4B) 布局，**既有字节契约完全不变**——
  原有 54 项 smoke 断言一行未动、全绿。
- **`Quantized`**：有界有损量化。位置按全局 AABB 线性映射到 `int16`（6B/顶点，另加 24B 解码头
  `quant_min`+`quant_scale`），索引降到 `uint16`（2B/个）。解码 `pos ≈ quant_min + q·quant_scale`，
  **每分量误差 ≤ quant_scale/2**（用四舍五入而非截断，把误差压到半级）。

**关键设计约束（都写进了代码注释）**：
1. **任意 32-bit float 无法无损压进更少比特** —— 这是**有界有损**量化，不是无损压缩。文档与断言
   都如实标注了误差上界，不夸大。
2. **层次/边界/error_metric 都在量化之前用全精度位置算好**（`RecursiveClusterSplit` 里），量化只
   动存盘字节，不影响裁剪/LOD 用的边界与误差。
3. **退化轴（range==0，如平铺在 XZ 平面的网格 Y 分量恒为 0）**：不能除零；这类轴量化成常量 0、
   解码精确还原为 `quant_min`。

**验证（headless WASM，无需浏览器/重启/真机）**：新增 `scripts/verify_nanite_wasm.sh` + 
`scripts/nanite_wasm_prelude.h`，把 cluster builder + smoke test 作为独立 TU 用 emcc 编译、node 运行
（绕开 #12 之后 `ZRuntime` 拖入的 WebGL2RHI/emmalloc 浏览器符号）。**67/67 全绿**：54 项 Raw 断言 +
13 项新增 Quantized 断言（round-trip 误差界、字节数确实变小、层次与 Raw 一致、索引解码正确）。
实测 vertex_data **10368→5208B**、index_data **3456→1728B**，最大位置误差 0.000183 = 契约上界。

**验证过程揪出的 3 个真 bug**（正是运行时验证的价值）：① 退化轴除零 → NaN 灌进 int16；② 测试
oracle 误设顶点按 `grid.indices` 顺序（实为空间深度优先序），改用 Raw build 自身 emitted 顶点作对照；
③ 编码器用截断（误差达 1 级）改四舍五入（误差半级）。

**剩下**：HiZ 遮挡的 **GPU 侧**（compute shader 生成深度金字塔 + GPU 遮挡剔除，需真机）。streaming 已落地，
HiZ 的 **CPU 侧**（金字塔构建 + 遮挡判据）已落地，见下方落地记录·2.5 HiZ。

---

### 落地记录 · 2.5 streaming（2026-10-25）

**做了什么**：把 `NaniteClusterManager::UpdateStreaming` 里那个长期空转的 `TODO: 实现流式加载逻辑`
兑现为**基于距离的叶子驻留判据**：

- **判据**：叶子集群包围盒中心到相机的距离 ≤ `streaming_distance` ⇒ 驻留；否则未驻留。
- **内部节点永远驻留**：它们不提交渲染（`IsClusterVisible` 对非叶子恒返回 false），卸载没有任何
  渲染收益，只会让层次查询拿到空缺。保持"结构节点随时可读"的不变量。
- **`enable_streaming == false`**：一切驻留，等价于"没有流式加载"，注册即整个驻留内存。是一等配置。
- **驻留变化的转移**记入 `m_StreamingQueue`（`UpdateStreaming` 首次有真实写者）。

**关键设计约束（都写进了代码注释）**：
1. **`cluster_loaded_flags` 目前是策略/可见性闸门，不是物理显存回收**。所有集群共享**同一段**
   `vertex_data`/`index_data` 字节流（深度优先连续窗口），卸载中间一个集群会破坏邻居的 offset。
   真正的按页驻留要等**页池**落地（step 2.5 后续）；届时语义从"标不标"升级为"拷不拷得进页"。
   这是架构事实逼出来的边界，不是偷懒。
2. **没有网格简化器**：内部节点的几何窗口 == 子树几何的完整聚合（`CalculateClusterError` 返回
   radius）。所以"用父节点几何回退"不省三角形（画父=重画整棵子树），父回退在无简化器时无意义。
   流送因此只做**距离裁剪**，不做反馈式码率预算（那需要屏幕误差积分 + 真实页池）。

**接线**：`NaniteRenderer::PrepareRenderData` 的逐叶子裁剪循环加了 `IsClusterLoaded` 闸门——未驻留
的叶子本帧不提交渲染。**注册时 flags 全为 true、且 `UpdateStreaming` 此前无帧循环调用者**，所以这条
在既有路径里恒放行，不影响任何现存行为（67 项旧断言 + 裁剪 smoke test 全不受影响），只在流送真正
跑起来后生效。

**验证（headless WASM，无需浏览器/重启/真机）**：复用 `scripts/verify_nanite_wasm.sh`，在
`NaniteClusterBuilderSmokeTest.cpp` 加 10 项 streaming 断言。**77/77 全绿**（67 项旧 + 10 项新）：
注册即全驻留、超大距离=全驻留、相机超出距离=叶子全卸载但内部节点存活、中距离=真·部分驻留
（证明距离梯度在排序叶子、不是整块翻转）、`enable_streaming=false`=任意距离全驻留。

**诚实定位**：这是**距离裁剪**，不是 UE 那套 streaming pool。它带来真能力（远处叶子不再进裁剪/
渲染），但没有带宽预算、没有按需分页、没有物理显存回收。页池是明确的后续项。

---

### 落地记录 · 2.5 HiZ（2026-10-26，CPU 侧；GPU 侧待真机）

**做了什么**：把 `NaniteHierarchicalZBuffer` 那组纯 TODO 桩（`UpdateDepthBuffer` / `BuildHierarchy` /
`BuildMipLevel` / `IsOccluded` 全是空实现）兑现为**可运行的 CPU 侧 HiZ**——从一帧深度图构建层次 Z
金字塔 + 按屏幕区域采样 + 保守遮挡判据。与 streaming/几何压缩同一手法：把纯算术抽成 header-only
核心（`Function/Render/Nanite/NaniteHiZ.h`，`nanite_hiz::{HiZPyramid, BuildHiZPyramid, MipLevelForRect,
SampleMaxDepth, TestOccluded}`），脱离 RHI/窗口单测；`NaniteHierarchicalZBuffer` 只做薄封装（存金字塔 +
把 AABB 投影到屏幕矩形）。

- **深度约定**：**reversed-Z**（1.0=最近、0.0=最远），现代深度缓冲通用。金字塔 **max 归约**：每个粗
  texel 存覆盖 2x2 里的最大深度（= 最近表面）。
- **遮挡判据（保守，拿不准绝不剔除）**：物体最近深度 < 区域内金字塔最大深度 ⇒ 被完全遮挡（区域内
  所有东西都比物体近）⇒ 可安全剔除；只要有一个 texel 更远或相等 ⇒ 可见，不剔除（误剔=画面破洞，
  比不剔更糟）。
- **接线**：`NaniteCullingSystem` 拥有 `NaniteHierarchicalZBuffer m_HiZ`；新增 `UpdateHiZDepth(depth,w,h)`
  喂深度图重建金字塔；`OcclusionCull(bounds,transform,view_projection)` 在 HiZ 就绪时委托它，否则保守
  不剔除（与 `FrustumCull` 对无效 bounds 的处理一致）。`CullInstancesCPU` 的遮挡分支已接上。
- **类重排**：`NaniteHierarchicalZBuffer` 移到 `NaniteCullingSystem` 之前（后者按值持有它，需完整类型）。

**验证（Windows headless，无需真机）**：新增 `Nanite/Test/NaniteHiZSmokeTest.cpp`（header-only，只链
include root，开关 `ZENGINE_BUILD_NANITE_HIZ_SMOKE_TEST`），**28/28 全绿**：金字塔构建（尺寸/mip 数/
max 归约/空输入）、mip 级别选择、区域采样、遮挡判据（近/等/远/部分遮挡/越界/无效金字塔）。
`Runtime/CMakeLists.txt` 加 carve-out + `add_subdirectory`。ZRuntime 同步重建（HiZ 接线类型检查通过）。

**诚实定位 / 边界**：
- 这是 HiZ 的 **CPU 侧**。GPU 侧（compute shader 生成深度金字塔、GPU 逐簇遮挡剔除）仍需真机；而且
  现有 `NaniteGPUCullingSmokeTest` 的真 dispatch 段是 **Apple Metal 专属**（`#if __APPLE__`），Windows
  DX12 上还没接——GPU 遮挡剔除落地时这块也要补。
- `NaniteHierarchicalZBuffer::IsOccluded` 的 AABB→屏幕矩形投影是 CPU 侧近似（8 角点投影取包围盒 +
  物体最近深度）；相机横跨视锥的角点保守判可见。真实 GPU 版用 HiZ 纹理采样，精度更高。
- 尚无驱动它的真实渲染帧（深度图从哪来、何时重建），属"核心已验、端到端待真实内容"。

---

### 落地记录 · 2.6 VirtualTexture 接线（2026-10-25）

**做了什么**：把长期休眠的 VirtualTexture 子系统（6 个源文件能编译但**无任何渲染路径调用**、CMakeLists
在 WebGL/Windows/Apple 三条路径整个排除）接上第一根线——填实 9 处 TODO 里 6 处 CPU 侧、加 CVar 门控、
挂帧循环。

**9 处 TODO 的分类**（按能否 headless 验）：
- **CPU 侧 6 处（已填实）**：① `CalculatePageScreenSize`（针孔投影：页世界半幅 = 对象半幅 / 页网格，
  投到屏幕高比例，带退化 fov/距离守卫）；② `RequestPages`（把真实 distance 传给 mip 选择、记录真实
  时间戳、把对象世界 bounds 戳到页上）；③ `UpdatePagePriorities`（用页上存的世界足迹对每帧相机
  重投影、按 `min_screen_size`/`max_distance` 门控、设优先级 + is_visible）；④ `EvictLRUPages`（超
  预算时按 `last_access_time` 最旧优先卸载）；⑤ `Update` 统计（loaded/loading/unloaded 页数 + 已用
  slot + 内存 + 使用率）；⑥ `LoadPage` 时间戳（给 LRU 一个真实排序键，原来所有 slot 都在 0 打平）。
- **GPU 侧 3 处（如实留作真机）**：`LoadPage` 从磁盘读像素、`UploadPageToPhysicalPool`（Vulkan transfer）、
  `UpdatePageTableTexture`（建页表 image）。`PhysicalTexturePool::Initialize` 是 Vulkan-only
  （`static_cast<VulkanRHI*>` + `vmaCreateImage`），这三处没有真机/RHI 无法跑。

**关键设计缺口修复**：骨架的 `VirtualTexturePage` **不存任何世界位置**——这正是 `UpdatePagePriorities`
当年只能是 TODO 的原因（没有可重投影的东西）。给页加了 `object_position` + `object_bounds`，
`RequestPages` 时戳入，`Update` 每帧重算。

**CVar 门控**：新增 `r.VirtualTexture.Enable`（默认 0），仿 `NaniteSettings` 模式（`VirtualTextureSettings`
只暴露一个 bool、不引用任何 VT 类型，所以全平台可编译）。帧循环新增 `RenderSystem::
UpdateVirtualTextureForFrame`，与 `UpdateNaniteForFrame` 同址（3 条 Tick 路径统一入口），**定义里用
`#if defined(Z_HAS_VULKAN)` 包住对 VT 的引用**（VT 只在 Vulkan 路径编译：Android/OHOS/Linux/
Windows-with-vulkan；Apple/Emscripten 的 CMake 排除了 `VirtualTexture.*.cpp` 且未定义该宏），非 Vulkan
构建下是 no-op。默认关 ⇒ 即便 VT 编译进来，不显式打开也零开销、零回归。

**验证（headless WASM，无需浏览器/重启/真机）**：新增 `scripts/verify_vt_wasm.sh` + 影子头
`scripts/vt_wasm_shadow/`。VT 比 Nanite 难缠得多：3 个 `.cpp` 硬 `#include "VulkanRHI.h"`（拉
`<vulkan/vulkan.h>` + `<vma/vk_mem_alloc.h>`），还经 `RenderResourceBase.h` 拖入整个 Object 系统
（`Component.generated.h` 等 codegen 产物）。影子树（放在 engine/Source 之前）提供：Vk/vma 类型桩 +
一个完整的 `VulkanRHI` 桩子类（覆写 RHI 全部 ~60 纯虚，harness 从不调用）、轻量 `RenderResourceBase.h`
（VT 只把它当不透明指针）、vma shim TU、平台三元组（`-DZ_PLATFORM_LINUX` 等，绕过 `Platform.h` 的
`#error`）、复用 Nanite 的序列化 prelude。`PhysicalTexturePool::InitializeLogical()` 建 slot 网格而不建
Vulkan image，让流送策略在无设备上断言。**15/15 全绿**：注册/mip0 全加载/统计一致/LRU 淘汰守预算
（1024×1024=64 页=4MB 配 1MB 预算）/远距离门控标 not-visible/注销清空。

**诚实定位**：这是**接线第一步**——把休眠子系统变成控制台可开、帧循环可驱动、策略可断言。页上传/
页表纹理（GPU）+ 2.4 的 RG 资源注册是明确的后续项。它不是"VT 完成了"，是"VT 从死代码变成可推进"。

---

### 落地记录 · 2.7 Lumen（2026-10-25，复核：CPU 侧已完整，无需动手）

**结论**：本步骤的 CPU 侧工作在进入前**就已经全部落地并有测试**。计划表里那句"部分实现（CPU 探针网格
已通；无任何 Lumen shader）"是**过时评估**——它低估了代码现状（正是 §0 第 1 条警告的"别信过时设计稿
估工作量"的翻版）。逐文件复核后，Lumen 的整个 CPU 数据路径 + 帧循环接线 + CVar + 发布桥**此前就已
完整兑现**，7 个 smoke test 已在仓库里（reflections / GI / ray tracing / surface cache / scene geometry /
render-pass wiring / per-frame bridge）。

**已实现且已测试的部分**（都不是本次新写，是复核确认的既有事实）：
- **GI（`LumenGlobalIllumination.cpp`）**：世界空间辐照度 probe 网格锚定在距离场世界盒上（不跟相机走，
  否则历史全丢）；预算 + 轮转更新（一帧不追整网格）；时间累积（rate 0 冻结、大 rate 追上，帧率无关的
  `1 - exp(-rate·dt)`）；三线性插值查询（`SampleProbeGrid`，跳过未更新 probe，避免 0 拉黑整片）；
  多跳递归（`ComputeMultiBounceGI`）；"边查边填"回填网格（`TemporalAccumulation`）。
- **反射（`LumenReflections.cpp`）**：镜面方向镜像；**粗糙度 = 锥角**（`SampleConeDirection` 按立体角
  均匀采样、多光线平均），不再把 radiance 乘 `(1-roughness)` 压成纯黑；距离场反射（起点按体素对角线
  抬出表面、显式传距离场）；`valid` 标志区分"命中黑色"与"未命中"；`BlendReflections` 按"哪份真命中"
  选（HWRT > SSR > 距离场），**不再用 `max_trace_distance` 常量分支**（那会把已追到的命中换成恒空 SSR
  的 black）。
- **接线**：`RenderSystemFrameCommands.cpp` 的 `RenderLumenCmd` → `RenderPipelineBase::UpdateLumen` →
  `LumenRenderPass::Render`（Update → ComputeGI → ComputeReflections → 发布）每帧驱动；`Z_HAS_LUMEN`
  门控（Apple 剥离时类不存在，用 `dynamic_cast` 分发）。
- **CVar**：`r.Lumen.Enable/GlobalIllumination/Reflections/ProbeBudget/ReflectionBudget/SampleCount/
  ProbeSpacing/TemporalBlendRate/IndirectIntensity` 共 9 个，`RuntimeConsoleCommands.cpp:553` 注册，
  `LumenRenderPass::ApplyConsoleVariables` 每帧热生效。
- **发布桥**：`PublishIndirectLighting`/`PublishReflection` 写进 `RenderScene::m_IndirectLighting`
  （GI 与反射两套独立权重），经 `SceneIndirectLighting::ToUniform()` 打包进 per-frame uniform（rgb=
  辐照度、a=权重；"黑是数据不是缺失"），三份镜像（C++/GLSL/HLSL）布局由 `LumenPerFrameBridgeSmokeTest`
  的 `static_assert` + 运行时断言钉死。

**复核中逐一排除的"看似是活、实则是坑/已做"的项**（避免后来者重复劳动）：
1. **`LumenGlobalIllumination.cpp:328` `view_direction = Vector3(0,0,1)` 的 `TODO: 从相机获取`——
   是死参数，不是 bug**。它传进 `ComputeMultiBounceGI`，但函数体内**从不引用** `view_direction`（只有
   签名那一行出现）；生产路径走 probe 网格的 `GetAverageIrradiance`，`GetIndirectLighting` 的直接调用者
   全是 test，且都传了真实法线做半球采样。"修"它（把相机方向穿进来）是**零行为变化的纯 cosmetic churn**，
   且没有 test 能钉住它 ⇒ 不动，如实标注为死参数。
2. **`LumenGlobalIllumination.cpp:419` "更高级采样（重要性采样）" `TODO`**——当前是**余弦加权半球采样**
   （`SampleIndirectDirection`），已是合法的物理采样；重要性采样是质量增强项，不是修 bug，且收益要真机
   才能看 ⇒ 不属 CPU 可验。
3. **`LumenReflections.cpp:138` SSR `TODO`**——函数注释已说明：CPU 路径**没有深度缓冲也没有 GBuffer**，
   恒返回 `valid=false` 是**正确**行为（历史上它曾返回全零并靠常量分支拿到 100% 权重，把距离场命中换成
   黑，已被 `BlendReflections` 的"按命中选"修复 + `LumenReflectionsSmokeTest` 第 3 组断言钉死）。真 SSR
   依赖 2.4 的 GBuffer/depth ⇒ GPU 侧。
4. **`LumenRayTracing.cpp:171` HWRT `TODO` + `InitializeHardwareRayTracing` 恒 false**——HWRT/BLAS-TLAS
   已在 §5 明确移出 Phase 2（需 Vulkan RT 扩展 + 加速结构独立专项）。

**诚实定位**：step 2.7 的 CPU 部分是**"复核确认已完成"，不是"本次做了"**。Lumen 剩下的全部是 GPU 侧
（SSR 需 depth/GBuffer、HWRT 已移出、首个 Lumen shader 需真机），与 2.4 的 RG 资源注册绑定，须等真机 +
显示器。这再次印证 §0 第 1 条：**规划要看代码，不要看 `doc/lumen/LUMEN_SYSTEM_DESIGN.md` 那篇无状态标记的
纯散文设计稿**，否则会把一个已 4000+ LOC、已接线、已有 7 个测试的系统误判成"只有探针网格通了"。

---

### 落地记录 · 2.4 RenderGraph 接管 RP1（2026-10-25，编译器层 + executor 层兑现；含勘误）

**前置发现：step 2.3（RP1 三分）此前就已落地在代码里**。`MainCameraRp1Pass` 早已拆成 3 个**独立**
render pass —— `DrawGBufferPass`/`DrawDeferredLightingPass`/`DrawForwardLightingPass`，各自一次
`CmdBeginRenderPassPFN`、subpass 恒 0（`MainCameraRp1Pass.cpp:2135/2202/2285`），`MainCameraFramebufferResources`
也建好了 3 个独立 render pass + framebuffer（`SetupGBufferPass`/`SetupDeferredLightingPass`/
`SetupForwardLightingPass`，GBuffer `storeOp=STORE` 保证跨 pass 可见）。**RP1 唯独没有 RenderGraph 接线**
（只有 RP2 有 `DrawRP2WithGraph` + `m_UseRenderGraph`）。所以 step 2.4 的实质 = 证明拆分后的 RP1 拓扑
能在图里声明并推导出正确的屏障/顺序。

**做了什么（编译器层，headless 可验）**：仿 `BuildRp2Wiring` 给 `RenderGraphSmokeTest.cpp` 加了
`BuildRp1Wiring()` + 两组断言（group 19/20），把 RP1 的真实资源拓扑声明进图：
- `GBufferPass`：写 GBufferA/B/C（transient RT）+ 写 scene depth（import）。
- `DeferredLightingPass`：读 GBufferA/B/C（ShaderResource）+ 读 depth（DepthStencilRead），写 BackupOdd（import，RenderTarget）。
- `ForwardLightingPass`：读 depth + 写 BackupOdd（import，RenderTarget）。

**断言（35 项新检查，200/200 全绿）**：执行序 `GBufferPass → DeferredLightingPass → ForwardLightingPass`；
**9 条屏障 = 4+5+0**——GBuffer 三张 + depth 首写各 1 条（Undefined→RenderTarget/DepthStencilWrite）；
Deferred 把 GBuffer 三张 RenderTarget→ShaderResource、depth DepthStencilWrite→DepthStencilRead、BackupOdd
ShaderResource→RenderTarget 共 5 条；Forward 全同态（depth 已 DepthStencilRead、BackupOdd 已 RenderTarget）
**0 条**（正是 DeriveBarriers "已在目标态不产生屏障" 那条）。另断言 GBuffer 三张 transient 生命周期
（slot 0 产生 / slot 1 最后读 / 可别名回收）、BackupOdd 外部资源跨链不别名。group 20 把"同 pass 读写
BackupOdd"钉成**编译期硬错误**（`RenderGraphBuilder::Declare` 拒绝；勘误见下：这拦的是"采样+渲染同一张图"，不是 Forward 的 blend）。

**验证（headless WASM，无需浏览器/重启/真机）**：新增 `scripts/verify_graph_wasm.sh`。图编译器是**纯
标准库**（`RenderGraph.cpp` 只 include `<algorithm>/<queue>/<set>/<utility>` + 自身头，无 RHI/bqlog/object
系统），所以 harness 比 Nanite/VT 轻得多——**无 shadow 树、无 prelude**，emcc 独立编译 4 个 graph TU +
测试、node 运行。200/200 全绿（既有 group 1-18 的 165 项 + 新增 35 项）。

**⚠️ 勘误（2026-10-25 二次复核，execute 层）**：上一版此处写的"两处真实语义缺口"**两处都不成立**，已撤回：
1. ~~GBuffer 的 INPUT_ATTACHMENT 描述符是缺口~~ —— **错**。`MainCameraRp1Pass` 只在 **DX12**
   （`DX12MainCameraPass` 成员）与 **Apple/Metal**（`DesktopRenderPipelineModule.cpp` 的 `#if defined(__APPLE__)`
   块）构造；**Vulkan 走的是旧的多 subpass `MainCameraPass`**（GLSL `subpassLoad` 只在那条路上）。DX12 把
   `RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT` 直接映射成普通 SRV（`DX12RHI.cpp` 的 descriptor range 映射 +
   `UpdateDescriptorSets`），HLSL 用 `Texture2D.Load`；Metal 经 SPIRV-Cross 得到 `[[texture(n)]]`（且 Metal
   的 deferred 子 pass `inputAttachmentCount=0`）。⇒ 在真正跑这条 pass 的后端上，"图里建模成 ShaderResource"
   **就是实际发生的事**。"跨独立 render pass 用 input attachment 在 Vulkan 非法"这句本身没错，但没有任何
   代码路径在 Vulkan 上跑 `MainCameraRp1Pass`。
2. ~~ForwardLighting 的 blend 图表达不了~~ —— **错**。透明混合是 colour attachment 上的**固定功能 blend**
   （`loadOp=LOAD` + blend state），shader 从不采样 BackupOdd（`mesh_forward.frag.hlsl` 只读材质贴图）。这
   本来就是 `RenderTarget` 状态，`WriteTexture(RenderTarget)` 是**忠实**声明，不是简化。group 20 拦的是
   "同 pass 既**采样**又渲染进同一张图"（真·反馈环），与 Forward 无关——已改名为
   "RP1 guard: sampling BackupOdd while rendering into it is a hard error"。

**execute 层二次复核真正找到的东西**：
- **真 bug（已修 + 回归测试）**：`RenderGraph::ImportTexture` 不记录格式（`texture_desc.format` 恒
  `Unknown`），而 `RenderGraphExecutor::RecordBarriers` 靠格式判深度 aspect ⇒ **导入的深度图会拿到
  COLOR aspect 的屏障**（Vulkan validation error；RP2 只导入颜色图所以从没暴露，RP1 是第一个导入
  scene depth 的接线）。另外 transient `D32_SFLOAT` 拿到 `DEPTH|STENCIL`，而 D32 没有 stencil（同样非法，
  且 D32 是 Vulkan `FindDepthFormat` 的首选）。修法：`RGExternalTexture` 加可选 `format` 字段（默认
  `Unknown`，现有调用点零改动），`ImportTexture` 抄进 `texture_desc.format`；executor 只对
  `D24_UNORM_S8_UINT` 加 STENCIL aspect。
- **新增 executor 断言**（`RenderGraphExecuteSmokeTest` group 13/14，+29 项）：RP1 三 pass 经 executor 跑一遍，
  断言执行回调顺序、5 条屏障分 2 次批量 `CmdPipelineBarrier`（Forward 0 条）、只有 GBufferA 走池分配、
  导入资源解析回调用方自己的 image；depth 两条屏障 aspect 恰为 `DEPTH_BIT`、layout `UNDEFINED →
  DEPTH_STENCIL_ATTACHMENT → DEPTH_STENCIL_READ_ONLY`；D24S8 transient 两条屏障均为 `DEPTH|STENCIL`。
  **红绿验证**：用 HEAD 版 `RenderGraph.cpp`/`RenderGraphResources.cpp` 编译新测试 ⇒ 121 项里 2 项 FAIL
  （恰是两条 depth aspect）；修复后 121/121。
- `verify_graph_wasm.sh` 扩展为同时跑 compiler（200/200）+ executor（121/121），executor 只需 platform
  三元组（`-DZ_PLATFORM_LINUX ...`）+ EASTL include，**仍无 shadow 树**（executor 只经 10 方法的
  `IRGResourceDevice` 说话，测试用 FakeDevice）。

**真正剩下的接线工作（`DrawRP1WithGraph`，DX12 编辑器，需显示器验）**：
1. **导入态 = 上一个 render pass 的 finalLayout，不是"方便的 Undefined"**。RP1 的三个 render pass 自己用
   `initialLayout/finalLayout` 转换附件（DX12 在 `CmdBeginRenderPassPFN` 的 `TransitionSubpassAttachments`
   与 `CmdEndRenderPassPFN` 里按 finalLayout `TransitionImage`），所以图驱动时每个资源的"当前态"由 render
   pass 推进，图只需补 render pass **之间**的那一段——与 `DrawRP2WithGraph` 的做法一致（它以 `ShaderResource`
   导入 backup_*，因为那是上一个 render pass 的 finalLayout）。group 19 用的是理想导入态，接线时要按真实
   finalLayout 重写一份断言（参考 RP2 的 group 18）。
2. **DX12 上手写的那一组 4 条屏障本来就是 no-op**：GBuffer pass 的 finalLayout 已把三张 GBuffer 留在
   SHADER_READ_ONLY、depth 留在 DEPTH_STENCIL_READ_ONLY，`DX12MainCameraPass::Draw` 里那组
   `CmdPipelineBarrier` 的目标态等于当前跟踪态，`TransitionImage` 的 `old_state == new_state` 早退。图驱动后
   这组手写屏障可以直接删（图在同一位置推导出 0 条），这是可以落地的简化，但仍需目视确认。
3. depth 在 DX12 的 Deferred/Forward pass 里同时是 DSV 和 SRV：`ToDX12ResourceState` 把
   `DEPTH_STENCIL_READ_ONLY_OPTIMAL` 映射成 `PIXEL_SHADER_RESOURCE`，而 render pass 开头又会把 depth 附件转成
   `DEPTH_WRITE`（`TransitionSubpassAttachments` 不区分只读深度）。这是**既有**行为、与图无关，但图接线时
   depth 的导入态必须照它实际所处的状态填，否则推导出的屏障会和 RHI 的私有跟踪态打架。**未验证是否触发
   D3D12 debug layer 报错**——需真机开 debug layer 看一眼，列为待验证项。

**`DrawRP1WithGraph` 已实现（2026-10-25 第三轮，默认关闭）**：
- 开关 `ZENGINE_RG_RP1=1`，**仅 DX12**（Metal 的 `CmdPipelineBarrier` 是空实现，图无事可做；非 DX12 打开只
  WARNING、走手写链）。`DX12MainCameraPass::Draw` 里图路径替换"三个 `Draw*Pass()` + 中间 4 条手写屏障"，
  `Draw*Pass()` 原样复用。拒绝点全部在录制前，回落日志只打一次（与 RP2 同契约）。
- 声明单一源 `Passes/MainCameraRp1GraphDecl.h`：生产与测试调用同一个 `DeclareRp1Graph()`。全部 import、零
  transient；导入态按上一帧 render pass 的 finalLayout（GBuffer `ShaderResource` / depth `DepthStencilWrite`
  / BackupOdd `ShaderResource`）——上面"剩下的接线工作"第 1 条据此兑现。
- depth 可选：DX12 `GetDepthImageInfo().depth_image` 今天是 nullptr，图不跟踪 depth（与手写链等价）；上面第 3 条
  的 DSV/SRV 问题因此在图路径里不出现，仍是既有行为、待 debug layer 验证。第 2 条（手写 4 条屏障是 no-op）在
  图路径里自然消失：图在同一位置推导出的 GBuffer 屏障同样退化成 `TransitionImage` 同态早退。
- 验证：compiler group 21（生产声明，有/无 depth：8 / 7 条屏障）→ **239/239**；executor group 15（回调顺序、
  0 次池分配、2 次批量 3+5 条、逐条 layout/aspect）→ **130/130**；变异（BackupOdd 导入态改 `Undefined`）立即
  2 FAIL。`MainCameraRp1Pass.cpp` 在真实 web 构建（emcc + ninja）里编译通过并导出两个新符号；DX12 侧
  `MainCameraRp1Pass.cpp`/`DX12MainCameraPass.cpp`/`MainCameraRp2Pass.cpp`/`RenderGraphResources.cpp` 用新增的
  `scripts/syntax_check_dx12.py`（clang-cl `/Zs` + ZRuntime Debug|x64 真实 include/define + PCH）类型检查通过，
  负对照（植入错别字）能抓住。**未做**：MSVC 全量构建与链接（沙箱拦了 MSBuild）、真机运行。

**诚实定位（第三轮）**：`DrawRP1WithGraph` 代码已落地、默认关、headless 两层 + 编译层全验。翻成默认前唯一的
闸门是：DX12 编辑器带 `ZENGINE_RG_RP1=1` 与手写链逐帧目视对比 + D3D12 debug layer 无新报错（与 2.1a 同一道
闸门，可以一次一起做：`ZENGINE_RG_RP1=1 ZENGINE_RG_RP2=1`）。

**翻默认的闸门已工具化（2026-10-25 第四轮）**：新增同帧 A/B 抓帧（`ZENGINE_RG_AB_CAPTURE=<帧号>`，
`Passes/DX12RenderGraphAbCapture`）+ `scripts/rg_ab_compare.py`。在同一帧里 RP1、RP2 各画两遍（手写 / 图），
回读 backup_odd、backup_even、backbuffer，逐位比对；"图"那一遍若回落到手写链会判 INCONCLUSIVE 而不是 PASS。
步骤见 `RENDER_GRAPH.md` §5「目视验证的工具」。2.1a 与 2.4 的闸门因此合成一次操作：DX12 编辑器抓一帧 →
`python scripts/rg_ab_compare.py <dir>` 退出码 0 → 再开 D3D12 debug layer 跑一遍看无新报错 → 翻默认。
工具链 headless 自测 `scripts/rg_ab_selftest.sh` 全绿；GPU 半边（拷贝/回读时机/状态恢复）仍只有真机能验。

### 落地记录 · 2.6 RenderGraph 接管 RP1/RP2 翻默认开（2026-10-26，真机逐位等价验证 PASS）

第四轮铺好的闸门（同帧 A/B 抓帧 + `rg_ab_compare.py`）在真机 DX12 上跑通后，把两个 pass 的图路径
从 opt-in 翻成**默认开**，手写链保留作回落与 A/B 比对。至此 C1（RP2）与 C2（RP1）的"翻默认"闸门
全部兑现，RenderGraph 接管主线闭环。

- **RP1 默认开**（`871878e3`，"Enable RenderGraph-driven RP1 by default (DX12)"）：
  `MainCameraRp1Pass::SetupRenderGraphPath()` 从 `ZENGINE_RG_RP1` opt-in 翻成 default-on
  （`disabled = (env=='0')`）。RP1 的 `SetRenderGraphPathEnabled` 有 DX12-only gate，非 DX12 返回 false
  并告警回落，故保留 warning 分支。
- **RP2 默认开**（`c0d1bcfc`，"Enable RenderGraph-driven RP2 by default (DX12)"）：
  `MainCameraRp2Pass::SetupRenderGraphPath()` 同样翻 default-on。RP2 的 `SetRenderGraphPathEnabled`
  无后端 gate（恒 true），改动比 RP1 简，无 warning 回落分支。
- **真机验证（DX12，两步）**：
  1. **同帧 A/B 抓帧**（`ZENGINE_RG_AB_CAPTURE=<帧>`，默认开状态下抓）：`rg_ab_compare.py` 退出码 0
     → `RESULT: PASS`。RP1 的 `rp1.backup_odd`、RP2 的 `rp2.backup_even` / `rp2.backbuffer` 三对
     **逐位 IDENTICAL**（容差 0），manifest 两 stage `graph_used=true`（未回落，比对有意义），
     debug layer 全程 0 条 CORRUPTION。RP1、RP2 各验一次。
  2. **默认开 soak**（不设 `ZENGINE_RG_*`，跑数百帧）：进程稳、`RP1/RP2 is driven by the render graph`
     出现、0 条 debug-layer CORRUPTION、无 device-removed/crash/hang。当前会话唯一的 `[E]` 是已知的
     Nanite GPU-cull `CreateComputePipelineState failed 0x80004005`（回落到 CPU culling，与 render pass 无关）。
- **回归固化**：验证步骤 + 判定标准 + 两层回归（无头 `scripts/rg_ab_selftest.sh` 22 断言全绿 + 真机
  A/B/soak 勾选清单）沉淀为 `doc/RG_AB_VERIFY.md`（`0a41bf19`）。改图路径/翻默认时按 §6.3 重跑。
- **诚实边界**：仅 DX12。RP1 有后端 gate、RP2 虽无 gate 但图路径同样只在 DX12 验证过；Vulkan/Metal
  未启用。TAA/Bloom 的图路径仍 opt-in（`TAAPass.h:22`、`BloomPass.h:14`），不在本次范围。

---

### 落地记录 · 穿插（2026-10-26）：GAS 冷却时间（主线卡真机时的本地可验 CPU 活）

主线渲染候选（C3 HiZ / C4 GPU / C6 GPU / C7b Linux 实测）全部卡在真机验证时，挑了一件本机可闭环的
CPU 能力缺口兑现——GAS 的冷却时间（此前 3 处 TODO：`GameplayAbility.cpp` 的冷却检查、
`AbilitySystemComponent.h` 的冷却计时器、`AbilitySystemComponent.cpp` 的周期 tick）。

- **设计**：冷却状态放在 **`GameplayAbilitySpec`**（per-ability 的实例化状态容器，已在
  `m_Abilities` 里按能力键好），而非 ASC 的并行 map——避免两份会不同步的状态。把纯算术
  （递减/钳制/判定）抽成 header-only 的 `Function/Gas/GasCooldown.h`（`gas_cooldown::StepCooldown /
  IsOnCooldown / StartCooldown`），与 EnhancedInput 的 `ComputeFrameDelta` 同一手法：核心纯函数可脱离
  ASC/GameplayAbility/AttributeSet 单测；spec 只做薄封装转发。
- **接线**：`CanActivateAbility` 查 `spec.IsOnCooldown()` 拒绝冷却中的重复激活；
  `TryActivateAbility` 激活后按 `cooldown_duration` 起表；`Tick` 新增 `UpdateCooldowns(dt)` 遍历递减
  （只对冷却中的 spec 写入；冷却对所有已授予能力递减，不论激活态，与 UE GAS 一致）。删掉了 ASC.h
  里那个被注释的 `m_CooldownTimers` map。
- **验证**：新增 `Function/Gas/Test/GasCooldownSmokeTest.cpp`（header-only，只链 include root，开关
  `ZENGINE_BUILD_GAS_COOLDOWN_SMOKE_TEST`），19 项 Check：StartCooldown 钳制、IsOnCooldown 边界、
  StepCooldown 帧率无关性 + 负 dt/大 dt/过冲钳制、完整冷却周期到期、激活闸门语义。**19/19 全绿**。
  ZRuntime 同步重建（ASC/spec 接线类型检查通过）。
- **诚实边界**：纯 CPU 算术语义已验；ASC 层的端到端（真实激活→冷却→再激活的运行时流程）尚无驱动
  它的游戏场景，属"接线已通、待真实内容"。

---

### 落地记录 · 穿插（2026-10-27）：GAS 周期性效果执行（同簇 CPU 活）

紧接 GAS 冷却，兑现同在 `AbilitySystemComponent::UpdateEffects` 里的最后一处 GAS CPU TODO——周期性
效果（DoT/HoT 类）的周期执行（此前 `// TODO: 实现周期性执行逻辑`，`onPeriodicExecute` 从未被调用）。

- **设计**：与冷却同一手法。周期计时状态落在 **`GameplayEffectSpec`**（`m_PeriodElapsed` = 距上次触发
  的秒数），纯算术抽成 header-only 的 `Function/Gas/GasPeriodic.h`（`gas_periodic::PendingExecutions /
  IsValidPeriod`），spec 只做薄封装（`StepPeriodic`）转发。核心语义：**跨周期补触发**——推进后只要
  计时跨过 period 就触发一次并**减去 period（不清零）**，低帧率/卡顿下一次 Update 跨过多个周期时正确
  补触发多次，不丢拍；`period<=0` 视为无效周期永不触发（防除零/死循环），负 dt 视为 0，病态输入封顶
  64 次防拖死帧。
- **接线**：`UpdateEffects` 里对 `is_periodic && IsValidPeriod(period)` 的 spec 推进计时并按返回次数调
  `onPeriodicExecute`；`ApplyEffect` 里对 `execute_on_application` 的周期效果在应用时立即执行一次并
  `ResetPeriodic()`（归零计时，避免首个周期被重复补触发）。
- **验证**：新增 `Function/Gas/Test/GasPeriodicSmokeTest.cpp`（header-only，只链 include root，开关
  `ZENGINE_BUILD_GAS_PERIODIC_SMOKE_TEST`），19 项 Check：IsValidPeriod 边界、单触发/不触发不清零、
  跨多周期补触发、病态输入防御（无效 period/负 dt/封顶）、端到端触发次数、帧率无关性、低帧率不丢拍。
  **19/19 全绿**。ZRuntime 同步重建（ASC/spec 接线类型检查通过）。
- **诚实边界**：纯 CPU 算术语义已验；ASC 层端到端（真实 DoT 效果按周期跳伤害的运行时流程）尚无驱动
  它的游戏内容，属"接线已通、待真实内容"。至此 `Function/Gas/` 下非测试 TODO 已清零。

---

### 落地记录 · 穿插（2026-10-27）：运行时 UI（UMG）补齐——可本地验证部分全部落地

主线渲染候选（C3 HiZ GPU / C4 GPU / C6 GPU / C7b Linux 实测）仍卡真机，转去兑现 UI 子系统的 CPU
缺口。**重要勘误**：早期对比分析称"ZEngine 完全没有运行时 UI"是**错的**——实测存在完整的 UE UMG
风格系统（`Runtime/UI/` 22 文件 + `Runtime/UMG/` + ZSlate submodule 完整 Slate widget 库），走
retained-widgets 路线（非 Unity uGUI）。所以这不是从零建 UI，是补齐相对 Unity uGUI / UE UMG 的缺口。
全部细节见 `doc/RUNTIME_UI_IMPLEMENTATION_PLAN.md`；此处只记总账。

- **P0 数据绑定 + 焦点导航 + 逐帧 Tick**：`UMG/Core/UMGDelegates.h`（`UMGEventRegistry` 命名事件
  id→handler 表，Invoke 前拷贝列表防自解绑/重入崩）+ `UMG/Core/UMGTree.h`（模板遍历：深度优先
  Tick + Collapsed 子树跳过 / 焦点收集 / Tab 循环索引算术，模板参数化节点类型让测试用最小桩 struct
  headless 验）。`UWidget` 加 `NativeTick`/焦点字段/虚子节点访问。`UMGDelegatesSmokeTest` 22/22 +
  `UMGTreeSmokeTest` 全绿。
- **P1 动画纯核心 + 渲染变换字段**：`UMG/Animation/UMGAnimation.h`（`ApplyEasing` 越界夹紧 /
  `AnimTrack::Evaluate` 先缓动再插值 / `RenderTransform` 6 槽属性路径分派，只覆盖命中槽）+
  `UWidgetAnimation.h`（命名轨道 + 自持归一化进度状态机 Play/Reverse/Loop/瞬时；**不依赖
  ZSlate::FCurveSequence**——其成员函数前向声明顺序在 MSVC 单独包含时报 C2027）。`UWidget` 加
  `m_RenderTransform`+`m_Animations`，`Tick(dt)` 先推进动画再调 NativeTick。`UMGAnimationSmokeTest`
  43/43。
- **P1 锚点/自适应**：`UMG/Layout/UMGAnchor.h`（Unity RectTransform 最小子集：`ResolveTopLeft` =
  `parent_pos + anchor*parent_size + offset - pivot*size` 统一四角/中心锚 / 拉伸锚跨距 / Clamp01）。
  `UWidget` 加 `anchor_min/max/pivot/offset`（默认贴左上=ZSlate flow layout 现状，现有 widget 不受影响）
  + 序列化 round-trip。`UMGAnchorSmokeTest` 17/17。
- **P2 widget 广度**：`UProgressBar`→`SProgressBar`、`UComboBox`→`SComboBox<std::string>`、
  `UTabControl`→`SDockingArea`、`USafeZone`→`SBorder(DrawBackground=false)`，全部注册进
  `CreateWidgetByClassName`。顺带给 `UWidget::GetSlateAs<T>()` 加 const 重载。
- **验证**：4 个新 smoke test 全绿 + ZRuntime 每步同步重建（接线类型检查通过）。每个新测试都走既定
  手法：header-only 纯核心（只链 include root）+ Runtime/CMakeLists.txt main carve-out +
  `add_subdirectory`。
- **诚实边界 / 剩全是真机绑定**：ZSlate paint 链（`SWidget` 无 opacity/transform 字段，`ISlateRenderer`
  draw 原语吃 `UIColor`，要把 RenderOpacity 乘 tint / RenderTransform 折进 layout/paint/hit-test 全链）；
  `FGeometry::MakeChild` 喂锚点结果；UISystem 取帧 delta 驱动 `TickTree`（`IEngineSystem` 无 Tick(delta)
  接口）；手柄导航；`UTabControl` 活动页读回（`SDockingArea` 未暴露 active index）；`USafeZone` 真实
  per-platform 安全区。

---

### 落地记录 · 全仓库 TODO 复扫（2026-10-27，CPU 可验证项 vs 待补项盘点）

剔除 vendored `zstb_truetype.h`（13 处）、`/Test/`、`.gen.`、`Generated` 后，`engine/Source` 非测试
TODO/FIXME 共 **95 处**。逐簇核对后结论：**真正能本机 headless 验证的 CPU 项已基本清零**，剩余按
"能否离机验证"分两类。

**A. 纯后端/GPU 绑定（约 88 处，全部需真机/设备，无法本机验证）**：
- **RHI 后端**：DX12 13（command buffer/pool/fence/semaphore/viewport RT/内存 flush——全是 device API）、
  WebGL2 11（深度 rb/program/释放）、Vulkan 4。触碰 `RHI.h` 实现的都属此类。
- **Lumen 20**：距离场/表面缓存/GPU 资源更新/释放（GPU）、HWRT（`LumenRayTracing.cpp:171` 已明确移出
  Phase 2）、SSR（`LumenReflections.cpp:138` 需 depth+GBuffer）。`LumenGlobalIllumination.cpp:328`
  `view_direction` 是**死参数**（函数体从不引用，2.7 落地记录已勘误），非活。
- **Nanite 13**：GPU 裁剪/GPU 资源初始化/释放（GPU 侧）。`NaniteCluster.cpp:277` 顶点去重见 B。
- **VirtualTexture 4**：页上传/页表纹理（Vulkan transfer，2.6 落地记录已如实留真机）。
- **Render passes 6**：`PointLightPass.cpp:1022` / `DirectionalLightPass.cpp:1008` "render from near to
  far"（阴影 early-Z 排序，排序键 CPU 可算但收益只在真机渲染可见，且 batch 结构深绑 render pass）、
  `PointLightPass.cpp:775` "test more"、`PickPass`/`MainCameraPass`/`SplashScreen` 各 1。
- **RenderResource/RenderCommon 8**：sampler maxLod、irradiance mip、unmap 时机、cluster lighting、
  data caching——多为调优常量或生命周期收尾，低价值或需真机。

**B. CPU 可做但需权衡（1 处，唯一剩下的本机可验项）**：
- **`NaniteCluster.cpp:277` 顶点去重**：当前每个三角形独占 3 顶点（`vertex_count == triangle_count*3`，
  索引恒 `0,1,2,...`），可用 hash set 去重共享顶点，省 vertex_data 字节。**权衡**：这会打破刚落地的
  `GeometryCodec` 量化格式与 54 项 Raw 断言依赖的"连续索引"不变量，需同步改 codec + smoke test；
  收益（顶点数下降）对已量化的 `int16` 位置有限。属"可做但回归面 > 收益"，**暂不动**，等页池落地
  （2.5 后续）一起评估。

**C. 编辑器/内容管线（约 6 处，注释性/低优先）**：`ZSlateInspectorWindow` 4、`TextureImporter` 2
（非 RGBA 目标、DDS 解码——route B 明确留白）、`AnimationImporter` 1（PR-SK2 代码共享）、
`SkeletonData`/`LMDBDerivedDataCache`/`ShaderCbufferReflection` 各 1。均为机械收尾或概念注记，非阻塞。

**结论**：与 §0 第 3 条（"~80% 集中在 Nanite+Lumen+VT+RHI 后端，且都是各系统最硬的那一半"）一致——
经过 2.5/2.6/2.7 + GAS + UI 五轮 CPU 侧清剿，**本机可闭环的 CPU 项已基本耗尽**。继续推进必须开真机
闸门（C3 HiZ GPU / C4 VT GPU / C6 Lumen GPU / C7b Linux 实测 / UI paint 链），或开新专项（页池、HWRT、
mobile 前向管线——均已在 §5 移出 Phase 2）。

---

## 3. 关于阶段一 #9（RHI 胖接口拆分）—— 建议**不**进 Phase 2

阶段一已正确论证并后置（`PHASE1_FOUNDATION_PLAN.md:688-692, :822-824`）：`RHI.h` = **124 个纯虚、
689 行**。三条理由维持后置：

1. **痛点已被局部化解**：胖接口伤测试的根因，已被 `IRGResourceDevice`（10 方法，`RENDER_GRAPH.md:192-212`）
   局部解决——"RHI 签名漂移只会撞到 adapter 一个文件"。窄接口模式可按子系统复制，不必动 `RHI.h`。
2. **成本/收益此刻倒挂**：它触碰全部 4 后端 + 每个调用点，而阶段一刚加完第 4 个后端（WebGL2）——
   刚加后端就拆分 = 最大化返工。
3. **不产生用户可见能力**：Phase 2 应买渲染/内容能力；此拆只买可维护性，且 churn 最大。

**更优做法：让它"涌现"**。每个需要窄后端切片的 Phase-2 候选（Nanite/Lumen/VT）各自定义自己的
`IRGNaniteDevice` 式接口；等积累 2–3 个后，自然接缝集是"证据驱动"而非"拍脑袋"，届时拆分机械化。
**Phase 2 末再评估**。

---

## 4. 注意事项（排期前必须知道）

1. **别信过时设计稿估工作量**：`doc/nanite/NANITE_SYSTEM_DESIGN.md`、`doc/lumen/LUMEN_SYSTEM_DESIGN.md`
   是纯概念稿、无状态标记，代码却各约 4k LOC 且已接线。**从代码规划**。
2. **Schema 演进可能已完成**：`doc/asset_management/SCHEMA_EVOLUTION_AND_TYPETREE.md:360,391` 标
   Stage 3/4 待做，但 `Core/Serialize/Test/SchemaEvolutionSmokeTest.cpp` 已存在、`TEXTURE_COOK_PIPELINE.md:83-84`
   引用其 T1/T2 覆盖 Texture2D 迁移。**排期前先核对**，别重复劳动。
3. **Vulkan `MainCameraPass` 崩溃——已确认修复，无需再动手**：两篇 doc 断言其"pre-existing"，
   复核（2026-10-25）确认已被 commit `7e5d2521`（2026-10-22，"fix(vulkan): 空描述符资源与动态
   视口不再访问违例"）修掉——正是 §5 描述的同一故障点（`SetupModelGlobalDescriptorSet` 提交
   MegaLights 空资源）。commit 实测调试器下连续跑 240s 无 AV。**step 2.0 直接关闭**，Vulkan 验证
   通路随之恢复。两篇文档已加"已修复"标注。
4. **vendored stb 污染 TODO 指标**：`Runtime/UI/Render/zstb_truetype.h` 贡献 152 处 TODO 里的 13 处，
   任何"TODO 消化率"统计应排除它。
5. **WebGL2 残留仅 2 处真标记**：`WebGL2RHI.cpp:136`（挂深度/模板 rb）、`:885`（释放全部）——
   可在 C1/C2 之后顺手清，不必单列。

---

## 5. 明确移出 Phase 2 的项

- **RHI 胖接口拆分**（见 §3）—— 让它涌现，Phase 2 末再评估。
- **Lumen HWRT / BLAS-TLAS / BVH** —— 需独立专项（Vulkan RT 扩展 + 加速结构）。
- **Mobile 真前向管线**（`CROSS_PLATFORM_PIPELINE.md` §6 首项）—— 依赖尚未存在的 lightmap baker 与
  `LightComponent` 类型枚举。
- **Material editor → 真编译** —— 需与 ShaderLab codegen 绑定，是另一个专项。
- **LLM 内存追踪窗口** —— 仅概念提案（`LlmTracker.{h,cpp}` 存在但无窗口）。
- **GAS 编辑器 UI** —— 除非 gameplay 是 Phase-2 目标，否则价值低（★）。

---

## 6. 与阶段一的衔接状态

阶段一（`PHASE1_FOUNDATION_PLAN.md`）#1–#12 全部落地，其中两项"待真机"验证已铺路（2026-10-25）：
- **#2 桌面 POSIX fork 路径 + #1/#4 Linux RHI 运行期**：本机已装 WSL（组件 3.0.3 + Ubuntu 24.04，
  待重启注册 distro）。重启后跑 `scripts/wsl_verify_linux.sh` 一次性闭环，见 §3.10 补记。
- **#12 WebGL2 浏览器运行期**：待真实浏览器跑通 `.wasm`（沙箱拦截 headless Edge）。

这两项是阶段一的收尾尾巴，与阶段二主线（C1–C7）**相互独立**——阶段二从 C5（Vulkan 崩溃复核）
与 C1（RenderGraph）起步，不依赖它们完成。
