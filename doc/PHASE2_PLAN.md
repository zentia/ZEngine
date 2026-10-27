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
| C3 | **Nanite 收尾** | 流送 → 几何压缩 → HiZ 遮挡 → 页池 | **大部分实现**（真 GPU dispatch 已有；几何压缩✅ + streaming✅ + **页池簿记层✅ 已落地 2026-10-26**（opt-in，`streaming_pool_budget_bytes>0`；显存上传待真机），见 §2 落地记录·2.5 页池；**HiZ CPU 侧✅ 已落地 2026-10-26**，见 §2 落地记录·2.5 HiZ；**HiZ GPU 侧✅ 已落地 2026-10-27**：切法一 compute 生成金字塔 14/14、切法二 GPU 遮挡剔除 16/16，见 `doc/PHASE2_STEP2.5_HIZ_GPU.md` §6/§7；**页池 GPU 路径✅ 逐像素一致 13/13**。剩切法三（device-local+真 mip 纹理，可选打磨）与顶点去重（回归面>收益，主动推迟）） | C1/C2 给其 RG 管理的目标 | ★★★ 高价值/中风险 |
| C4 | **Virtual Texture 接线** | 分页纹理虚拟化，从"孤儿"到可用 | **CPU 侧✅ + GPU 侧✅ 2026-10-27**（页上传进物理页池 + 页表纹理上传，headless A/B 18/18 全绿、逐字节比对 0 失配，见 §2 落地记录·2.6 VirtualTexture GPU 侧）。页表间接寻址 shader（`vt_common.h` + `vt_resolve.comp`）2026-10-27 已落地并 A/B 22/22（见 §2 落地记录·2.6 VT 页表间接寻址 shader）。**仍无真实材质/draw 消费者** —— 把 `vt_common.h` 接进一个真实 fragment shader 是 VT 从"shader 读得对"到"能画"的最后一步 | C1/C2 的 RG 资源注册 | ★★ 高价值/中高风险 |
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
| **2.5** | C3 | Nanite：streaming → 几何压缩 → HiZ 遮挡 | 三个子项风险递增、各自独立可验（streaming/压缩是纯 CPU 数据、可 headless 验；HiZ 是 GPU 侧需真机）；已有真 GPU dispatch 作基线。**几何压缩已于 2026-10-25 落地**（见下方落地记录）：新增 `GeometryCodec::{Raw,Quantized}` 有界有损量化编码，WASM headless 验证 67/67 全绿，vertex_data 10368→5208B、index_data 3456→1728B。**streaming 已于 2026-10-25 落地**（见下方落地记录）：`UpdateStreaming` 从 TODO 兑现为基于距离的叶子驻留判据，WASM headless 验证 77/77 全绿（含 10 项新增 streaming 断言）。**HiZ GPU 侧 2026-10-27 已落地**（切法一 14/14 + 切法二 16/16，Vulkan headless，见 `doc/PHASE2_STEP2.5_HIZ_GPU.md`）。 |
| **2.6** | C4 | VirtualTexture 接线（CVar 门控） | 孤岛子系统，不会回归现有路径；需要 2.4 的 RG 资源注册。**已于 2026-10-25 落地**（见下方落地记录）：9 处 TODO 里 6 处 CPU 侧填实（屏幕投影 mip/优先级重算/LRU 淘汰/统计/时间戳/世界足迹），3 处 GPU 侧（页上传/页表纹理）如实留作真机；加 `r.VirtualTexture.Enable` CVar 门控 + 帧循环 `UpdateVirtualTextureForFrame`（3 条 Tick 路径统一入口，默认关、Vulkan 编译门内、零回归）；headless WASM 验证 15/15 全绿。**页上传/页表纹理（GPU）2026-10-27 已落地**（headless A/B 18/18，见 §2 落地记录·2.6 VirtualTexture GPU 侧）。剩 mip 链（现只传 mip 0）、path 方式注册的页无源像素（如实留白）、2.4 的 RG 资源注册、**以及 shader 侧按页表间接寻址采样（无消费者）**。 |
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

### 落地记录 · 2.5 页池簿记层（2026-10-26，CPU 侧；显存上传待真机）

**做了什么**：兑现 streaming 落地记录里承诺的"页池后续项"——把**页池的簿记**抽成不依赖任何
GPU/引擎类型的纯逻辑 `Function/Render/Nanite/NanitePagePool.h`（`nanite_pages::PagePool`），
与 HiZ/几何压缩同一手法（header-only 核心 + headless 单测）。

- **页模型**：页（page）= 驻留单位，带固定字节预算 `budget_bytes`。每个叶子 cluster 注册为一页
  （`byte_size` = 该 cluster 的几何字节，Raw 12B/顶点+4B/索引、Quantized 6B+2B，见
  `ClusterGeometryBytes`）。结构页（internal）也登记但标 `is_leaf=false`，**永不驱逐**——卸载它们
  没有渲染收益，只会让层次查询拿到空缺（与 UpdateStreaming 的"内部节点永远驻留"不变量一致）。
- **驻留判据**：仍是距离（与 UpdateStreaming 同一语义），但**进池要过预算闸**——想驻留的叶子页按
  "距离升序 + LRU 破平"排序，预算装不下的远页被拒/驱逐。语义从 UpdateStreaming 的"标不标"升级为
  "装不装得进预算池"。
- **LRU**：每页一个 `last_used` 时间戳，只由 `Touch`（渲染层真正用到该页）推进。**刻意不在
  `Update` 里给 want 页刷时间戳**——否则"被 want"会让所有页时间戳塌成同一帧，LRU 破平失效。
  `UpsertPage` 也**保留已有 `last_used`**（元数据刷新 ≠ 生命周期重置）。
- **增量队列**：`Update` 结束快照当前驻留页集合，下一帧与之比对——新进池进 `admitted`、不再驻留进
  `evicted`。调用方每帧拿到**增量**而非全量，正好是渲染层"拷进/释放显存"的真机清单。

**接线（opt-in，默认关闭）**：`NaniteConfig.streaming_pool_budget_bytes`（默认 0）。`> 0` 时
`UpdateStreaming` 走页池路径：喂 (page_id, byte_size, distance) → `PagePool::Update` → 按池子驻留位
同步 `cluster_loaded_flags`；`= 0` 时退回原纯距离裁剪，**字节级行为不变**（这是 77/77 回归全绿的前提）。
`RegisterMeshResource` 分配页 id（全局自增 `m_NextPageId`）+ 播种池子（重复注册同一 mesh_id 先撤旧页）；
`UnregisterMeshResource` / `clear()` 撤页，避免悬空登记累积占预算。

**验证（Windows headless，无需真机）**：
- 新增 `Nanite/Test/NanitePagePoolSmokeTest.cpp`（header-only，只链 include 根），**30 项断言全绿**：
  预算闸、距离优先、结构页永不驱逐、增量换入/换出队列、LRU 驱逐、`Touch` 保护、`RemovePage`/
  `SetBudget`、零预算。开发中修掉三个真 bug：①结构页测试预算给太小（50 < 30+30）导致结构页都装不下；
  ②`Update` 给所有 want 页刷 `last_used=now` 使 LRU 塌平；③`UpsertPage` 把 `last_used` 清零使 `Touch` 失效。
- **回归**：`NaniteClusterBuilderSmokeTest` **77/77 全绿**（默认 budget=0 走距离裁剪路径，行为逐位不变）。

**修掉的既有构建 bug**：页池测试的 `main()` 此前被 `/WHOLEARCHIVE` 拉进 ZRuntime.lib，与消费 exe 的
`main` 冲突（LNK2005）。在 `Runtime/CMakeLists.txt` 补 carve-out。**顺带修掉同病未治的 5 个 ShaderLab
冒烟测试**（TextureSlot/PropertyLayout/PropertyBlob/ProgramExtract/CbufferReflection）——它们各有独立
exe 目标却从未从 `GLOB_RECURSE` 剔除，任何 `/WHOLEARCHIVE` 链接 ZRuntime 的消费 exe 都会 LNK2005；
此前只在恰好没开这些测试时侥幸不炸。carve-out 后各自 exe 仍独立可建（已验 `ShaderLabTextureSlotTest`）。

**剩下**：把 `admitted`/`evicted` 队列真正拷进/释放显存是**渲染绑定**（RHI 上传/回收），需真机。
本层只算簿记，不碰 GPU。

---

### 落地记录 · 2.5 页槽位分配器（2026-10-26，CPU 侧；GPU 上传/回收待真机）

**做了什么**：兑现页池落地记录里"剩下：拷进/释放显存"的第一层——**页槽位分配器**
`Function/Render/Nanite/NanitePageSlotTable.h`（`nanite_pages::PageSlotTable`），header-only 纯簿记。

**为什么需要它（现状诊断）**：`NaniteBuildSceneGPUData`（`NaniteGPUData.h:202`）把**整个场景**的几何
摊成一段稠密 `positions`/`indices`，`NaniteRenderer`（`:1081-1111`）每帧把**全量** `UploadBytes` 到 GPU。
`cluster_loaded_flags` 只闸渲染提交（`PrepareRenderData:431` 跳过未驻留叶子），**不闸上传**——所以 GPU
始终物理持有全部几何，页池的 `admitted`/`evicted` 队列没有东西可拷/可释放。要让"拷进/释放显存"成立，
GPU 数据布局必须从"整场景稠密"改成"持久页槽位 + 间接寻址"：

1. 持久 GPU vertex/index 缓冲，容量 = 页池预算（不是整场景）。
2. **本表**维护 `page_id → (vertex_slot_base, index_slot_base)` 的稳定映射 + 空闲表。`admitted` 页认领
   槽位（渲染层随后把该页几何切片拷进对应 GPU 偏移）；`evicted` 页把槽位还回空闲表。
3. shader 侧间接寻址：cluster geometry 经页表解析，而非稠密 `vertex_base`。

**本层职责**：只管槽位簿记——稳定句柄、空闲表回收、churn 下不泄漏不重叠。设计要点：
- **best-fit 分配**：从空闲段里选能装下的最小段，减少碎片。
- **相邻段合并**：`Evict` 归还时按 start 排序合并相邻空闲段，防碎片化（已验：释放三段后能重新装下整段）。
- **稳定句柄**：已驻留页重复 `Admit` 是 no-op，返回既有槽位、不重复计费。
- **原子 admit**：vertex + index 两段必须同时拿到才 admit；任一不足则整体失败、不半驻留（避免页只进
  一半几何）。
- **稠密槽位数组**：`Evict` 用末尾元素填洞并同步被移动页的索引，`m_Slots` 无空洞。

**验证（Windows headless，无需真机）**：新增 `Nanite/Test/NanitePageSlotTableSmokeTest.cpp`
（header-only，只链 include 根），**32 项断言全绿**：基本认领连续/base 单调、稳定句柄 no-op、evict 归还 +
相邻段合并、**200 轮 churn 无泄漏不重叠**、best-fit 装碎片洞、空间不足整体失败（含 index 段不足不半驻留）、
evict 未驻留页安全 no-op、Reset 全清。`ZRuntime` 仍链接干净（carve-out 生效）。

**剩下（真机项）**：①把本表接进 `NaniteRenderer`——`admitted` 页按 `vertex_slot_base` 增量 `UploadBytes`
该页几何切片、`evicted` 页释放槽位；②持久 GPU 缓冲容量从"整场景"改成"页池预算换算的元素数"；③shader
cluster geometry 改经页表间接寻址。这三步需要真机验证（本地只能编译 + 静态检查）。

---

### 落地记录 · 2.5 增量上传计划器 + 接管 UpdateStreaming（2026-10-27，CPU 侧；UploadBytes/间接寻址待真机）

**做了什么**：兑现页槽位落地记录"剩下①"的 CPU 可验部分——**增量上传计划器**
`Function/Render/Nanite/NanitePageUploadPlanner.h`（`nanite_pages::PlanStreamingUploads`），header-only
纯簿记；并把**从未被调用的 `UpdateStreaming` 接管进帧循环**。

**关键发现（本步起点）**：`NaniteClusterManager::UpdateStreaming` **此前从未被任何帧循环调用**——全仓库
非测试的 `UpdateStreaming` 调用点只有 `WorldPartition` 和 `TextureStreamingManager`。`cluster_loaded_flags`
注册时置 `true` 后永不变，Nanite streaming 实际处于**休眠**。所以"增量上传"之前得先让 streaming 真正跑起来。

**计划器职责**：输入本帧 `PoolUpdate`（admitted/evicted page_id）+ 每页几何描述符（mesh 流里的
offset/count，manager 填）+ 页槽位表，输出**具体到字节段**的上传/释放计划：
- `uploads`：admitted 页 → 认领的 GPU 槽位（dst）+ 从 mesh 流哪段拷（src）。
- `releases`：evicted 页 → 已归还的槽位区间（渲染器可标记 GPU 段可复用）。
- `failed_admits`：想进但槽位不够的页（本帧不拷、保守不渲染）。
- `UploadBytes(elem_sizes)`：估算本计划拷贝字节（渲染器用它估带宽 / 决定分帧）。
- **先释放后认领**：先处理 evicted 腾位，再处理 admitted，减少误拒。

**接管调用**：`NaniteRenderer::Render` 在裁剪之后、`PrepareRenderData` 之前调用
`m_ClusterManager.UpdateStreaming(camera->position(), config.max_streaming_distance)`——必须在闸门之前，
后者用 `IsClusterLoaded` 决定本帧提交哪些叶子。manager 侧 `UpdateStreaming` 页池分支把增量队列存进
`m_LastPoolUpdate`；新增 `IsPagePoolEnabled()` / `GetLastPoolUpdate()` / `CollectResidentPageGeometry()`
（填出每个驻留叶子页的几何描述符）供渲染器驱动计划器。

**验证（Windows headless，无需真机）**：
- 新增 `Nanite/Test/NanitePageUploadPlannerSmokeTest.cpp`（header-only，只链 include 根），**25 项断言全绿**：
  admitted 产出 uploads（src 来自 mesh offset / dst 来自槽位 base）、evicted 产出 releases 且槽位可复用、
  槽位不够进 failed_admits 不半驻留、`UploadBytes` 带宽估算、**300 帧 churn 无泄漏不重叠用量有界**。
- **回归**：`NaniteClusterBuilderSmokeTest` **77/77 全绿**、`NanitePagePoolSmokeTest` 全绿、
  `NanitePageSlotTableSmokeTest` 全绿；`ZRuntime` 链接干净。

**剩下（真机项）**：渲染器拿到计划后把 `uploads` 逐条 `UploadBytes` 进持久页缓冲（dst slot_base ← src
几何切片）、把 `releases` 槽位标记空闲；持久 GPU 缓冲容量从"整场景"改成"页池预算换算元素数"；shader
cluster geometry 改经页表间接寻址（现行稠密 `vertex_base` → `page_table[slot]`）。这三步需真机验证。

---

### 落地记录 · 2.5 渲染器执行增量上传（2026-10-27，编译通过；GPU 行为待真机）

**做了什么**：兑现增量上传计划器落地记录"剩下"的前两项——渲染器**执行**计划：新增持久页缓冲 +
`NaniteRenderer::ExecuteStreamingUploads()`，把 `uploads` 逐条拷进持久页缓冲的槽位子段、`releases` 槽位
归还。

**接线**：
- `NaniteGPUResources` 新增 `page_positions`/`page_indices` 持久页缓冲（容量 = 页池预算换算元素数，与
  稠密整场景 `positions`/`indices` **并存**）；`clear()` 释放 + `m_PageSlots.Reset()`。
- `NaniteRenderer` 拥有 `nanite_pages::PageSlotTable m_PageSlots`。
- `Render` 帧循环：`UpdateStreaming`（产出 `m_LastPoolUpdate`）→ `ExecuteStreamingUploads`。
- `ExecuteStreamingUploads`：页池未启用 no-op；否则 `PlanStreamingUploads` 产出计划 → 保证持久页缓冲容量
  → 逐条 `UploadPageSlice`（Map 到槽位子段 + memcpy）。
- `UploadPageSlice`（匿名命名空间）：vertex Vector3→Vector4 展开（与稠密路径一致）、index 加
  `dst_vertex_slot_base` 变页缓冲全局下标；**量化格式**走 24B 头解码（quant_min/quant_scale）+ uint16
  索引，与 `GeometryCodec::Quantized` 一致。每页各自 Map 子段，避免整缓冲重传。
- `PageGeometryDesc` / `PageUploadCmd` 增加 `mesh_id`（渲染器用它取拷贝源 mesh resource）。

**验证（Windows headless）**：`ZRuntime` 链接干净（强制重编 `NaniteRenderer.cpp` 通过）；全 Nanite 回归
**全绿**——`NaniteClusterBuilderSmokeTest` 77/77、`NanitePagePoolSmokeTest`、`NanitePageSlotTableSmokeTest`、
`NanitePageUploadPlannerSmokeTest` 全过。

**诚实定位（真机项）**：这条拷贝路径**只编译通过，GPU 行为本机无法验证**（页池默认关闭，且需要真实 RHI
device + 帧循环）。而且 **draw 路径目前仍读稠密 `positions`/`indices`**——持久页缓冲现在是"几何已物理
驻留、但还没被 draw 消费"的桥接状态。要让 draw 真正用上页缓冲，还需 shader cluster geometry 改经页表
间接寻址（稠密 `vertex_base` → `page_table[slot]`），那是下一步（改 `NaniteBuildSceneGPUData` 的
`geometry.x/y` 语义 + cull/raster shader 读法）。

---

### 落地记录 · 2.5 shader 页表间接寻址（2026-10-27，编译通过；GPU 行为待真机）

**做了什么**：兑现渲染器执行落地记录"剩下"的最后一项——draw 路径从稠密 `positions`/`indices` 改成消费
持久页缓冲。**关键设计：不改 shader**。

**为什么能不改 shader**：现有契约是 `positions[gl_VertexIndex]`（vert）+ `command.firstIndex = geometry.y`
（cull.comp），索引值本身已含基址、`vertexOffset` 恒 0。所以只要把每个**已驻留**集群的 `geometry.x/y`
从稠密基址改写成它在持久页缓冲里的**槽位基址**，shader 端 `positions[slot_base + local]` /
`firstIndex = index_slot_base` 自然落到页缓冲的正确窗口——语义零改动。

**接线**：
- `NaniteClusterManager::GetClusterPageId(mesh_id, cluster_index)`：取集群的页 id。
- `NaniteRenderer::PatchClustersForPagePool()`：`PrepareGPUData` 里 `NaniteBuildSceneGPUData` 之后、集群
  上传之前调用。遍历 `m_SceneGPUData.clusters`，用 `geometry.w`（instance_index）反查 mesh_id、用
  `scene_index - instance.meta.z` 得 mesh 内 cluster_index，再查页槽位：驻留 → `geometry.x/y = slot_base`；
  未驻留 → `geometry.z = 0`（kernel 产出空命令，本帧不画）。结构页（`flags.x==0`）跳过。
- **集群每帧重传**：页随相机 churn，槽位基址每帧都可能变，所以页池启用时集群缓冲每帧重传
  （`clusters_dirty = buffers_recreated || !compute_ready || IsPagePoolEnabled()`），不再只在缓冲重建时传。
- **draw 绑定切换**：`RecordClusterDraw` 的 `CmdBindIndexBuffer` 与 `UpdateDescriptorWrites` 的 positions
  描述符，页池启用时指向 `page_indices`/`page_positions`，否则指向稠密缓冲。

**验证（Windows headless）**：`ZRuntime` 链接干净（强制重编 `NaniteRenderer.cpp` + `NaniteCluster.cpp`
通过）；全 Nanite 回归**全绿**——`NaniteClusterBuilderSmokeTest` 77/77、`NanitePagePoolSmokeTest`、
`NanitePageSlotTableSmokeTest`、`NanitePageUploadPlannerSmokeTest` 全过。

**诚实定位（真机项）**：整条链（页池 → 槽位 → 计划 → 拷进页缓冲 → geometry 改写 → draw 读页缓冲）现在
**编译通过、簿记全绿**，但 GPU 上的最终画面本机无法验证——页池默认关闭，且需要真实 RHI device + 帧循环 +
SPIRV-Cross 重编 shader。真机上要验证三件事：①页池启用时画面与稠密路径一致（几何没丢/没错位）；②未驻留
集群确实不画（无空洞）；③页 churn 时槽位复用不串页。这三件只能真机抓帧比对。

**真机验证可行性核查（2026-10-27，本机 Windows + RTX 5080）**：应"真机跑一遍对比页池/稠密画面"的要求，
实际排查了本机能否跑这条 GPU 路径，结论是**当前不能，且根因是架构性的、不是缺个测试**：

- 本机确有 DX12 runtime（`d3d12.dll`/`dxgi.dll` 在）+ NVIDIA RTX 5080，`DX12RHI::Initialize` 也只
  deref `WindowSystem->GetFramebufferSize()`（无窗口时兜底 1280×720，null-safe），所以**建 headless
  DX12 device 本身可行**。
- 但 Nanite 的 shader 是 **SPIR-V**（`Z::Shader::NANITE_CLUSTER_CULL_COMP`，宏名就叫
  `ZENGINE_NANITE_HAS_CULL_SPIRV`），而 `DX12RHI::CreateShaderModule` 只把字节包成 `ID3DBlob`、
  `DX12ShaderCompiler` 走 **DXC（HLSL→DXIL）**，**没有 SPIR-V→HLSL 的交叉编译**。所以 Nanite 的
  compute/raster 管线在 DX12 上建不出来 —— 这正是 `EnsureComputePipeline` 里 `gpu_pipelines_unsupported`
  那个 latch 预期的失败。建了 harness 也只会画出一屏清屏色，比对无从谈起。
- 交叉编译库 `spirv-cross` **在树里**（`engine/3rdparty/spirv-cross`，MetalRHI 已用它转 MSL），但
  `engine/3rdparty/CMakeLists.txt:252` 是 `if(APPLE ...)` 才 `add_subdirectory(spirv-cross)` ——
  **Windows 上根本不编译**，DX12 也没链它。

**要真正在本机跑 GPU 对比，前置工作是给 DX12 补 SPIR-V→HLSL 交叉编译**（把 spirv-cross 在 Windows 也
编进来 + DX12 `CreateShaderModule`/管线路径接上，或改用 Vulkan 后端——但 Windows 上 Vulkan 后端是否
注册/可用需另查）。这是一项独立的后端能力建设，不是 Nanite 页池本身的事，工作量显著且风险面在 shader
工具链，超出本次页池任务边界。已如实记录，不假装跑过。

**补充核查（Vulkan 后端路径，2026-10-27）**：本机 `ZENGINE_USE_VULKAN=ON`、`VulkanRHI.obj` 已编进
ZRuntime、Vulkan SDK 已装 —— Vulkan 原生吃 SPIR-V，**不需要**交叉编译，所以 shader 这关能过。但
`VulkanRHI::Initialize` 走 `CreateWindowSurface`（无主窗口只打错误日志、`m_Surface` 留空）→
`FindQueueFamilies` 里 `vkGetPhysicalDeviceSurfaceSupportKHR(..., m_Surface=null, ...)` 恒返回不支持 →
`present_family` 留空 → `IsComplete()` 为 false，而 `InitializeLogicalDevice`（`VulkanRHI.cpp:1311`）
无条件 `m_QueueIndices.present_family.value()` —— 对空 `std::optional` 调 `.value()` **抛
`std::bad_optional_access`，headless 直接崩**。

**结论（两个后端都卡在 headless 初始化，不是 shader）**：
- DX12：卡在 shader（SPIR-V 无交叉编译，DXC 只吃 HLSL）。
- Vulkan：shader 能过，但卡在 device 创建（present_family 依赖 surface，headless 无 surface →
  `.value()` 抛异常）。

**要真正在本机无头跑 GPU 对比，前置工作是给 RHI 补 headless 初始化路径**：Vulkan 侧把 `present_family`
改成可选（无 surface 时跳过 present queue，Nanite 离屏渲染本就不需要 swapchain/present）+ 跳过
`CreateWindowSurface`/`CreateSwapchain`；或 DX12 侧接 spirv-cross。两者都是**独立的 RHI 后端能力建设**，
风险面在 swapchain/queue-family 语义，显著超出本次 Nanite 页池任务边界。真实可行的替代：在**带窗口的
目标**上跑（游戏/编辑器运行时，或 Mac 上走 Metal——Nanite 的 GPU 路径目前只有 Metal 端到端测过）。已如实
记录，不假装跑过。

### 落地记录 · 2.5 页池真机 A/B 对比（2026-10-27，Vulkan headless，本机 RTX 5080）

上一节结论"两个后端都卡在 headless 初始化"已被**本轮兑现**——Vulkan 侧的 headless 初始化路径补齐了，
那条"只能真机抓帧比对"的链路现在**在本机无窗口就能端到端跑**。

**Vulkan headless 初始化路径（commit `9903e004` + 本轮 `cba3f9b5`）**：
- `m_Headless`（`WindowSystem::GetMainWindow() == nullptr` 时置真）：跳过 `CreateWindowSurface` /
  `CreateSwapchain` / `CreateSwapchainImageViews` / present queue；离屏 extent 用 WindowSystem 回退尺寸
  （1280×720）铺底，颜色格式取 `R8G8B8A8_UNORM`。
- `QueueFamilyIndices::IsComplete(require_present=true)`：无头时 `present_family` 恒空，用
  `IsComplete(false)` 只要求 graphics + compute（默认 true 保持既有行为）。
- `IsDeviceSuitable` 无头时跳过 `QuerySwapChainSupport`（null surface 会触发 validation 报错且返回空，
  导致每个设备都被判不适合 → "failed to find suitable physical device" → 拿 null 物理设备直接崩），并把
  `VK_KHR_swapchain` 从必需扩展集合临时拿掉再判扩展支持。
- `GetCurrentCommandBuffer` 无头时返回 `nullptr`：`m_CurrentCommandBuffer` 默认是 `new` 出来的空壳，
  `getResource()` 是未初始化句柄，拿它当当前帧缓冲会让 Nanite 误以为自己在 pass 里、把命令录进垃圾句柄，
  `vkCmdBindPipeline` 直接 access violation。返回 null 让 dispatch 退到 `BeginSingleTimeCommands`。
- `SubmitRendering` 无头时 `waitSemaphoreCount = 0`（`m_ImageAvailableForRenderSemaphores` 从不被 signal，
  非 0 会 `vkQueueSubmit` 死锁），present 块整体 `if (!m_Headless)` 跳过，仍推进 `m_CurrentFrameIndex`。

**真机 A/B 对比测试 `Nanite/Test/NanitePagePoolABSmokeTest.cpp`**：注册无窗口 `WindowSystem` → 建 headless
`VulkanRHI` → 12×12 网格（288 三角形，两层集群，多叶子页）→ 两个实例共享 `mesh_id=1` → 同一相机跑两遍
（pool budget 0 vs 足量）→ 读回离屏 GBufferA 逐像素比对。判据：**驻留方式变了、画面不能变**。

**过程中修掉的真 bug**：`ExecuteStreamingUploads` 里 `m_PageSlots` 是构造时的占位容量 `(1,1)`，不重设的话
`Admit` 全部失败（`failed=8`，页池一个页都驻留不进 → 什么都不画）。新增 `ResetWithCapacity`，按本帧可驻留
页的顶点/索引总数上界铺满槽位表。修后 `uploads=8 failed=0`，页池真正画出像素。

**测试现状（诚实）**：12/13 通过。唯一失败是**核心判据本身**——`covered_dense=2136` vs `covered_pool=1545`，
`differing=591`，`max_channel_delta=127`。清掉全部临时 printf 后，诊断线索收敛到一处：页池路径多条 draw
命令的 `firstIndex` 从 `756` **回绕到 `0`**（两个实例共享 `mesh_id=1`，`PatchClustersForPagePool` 按
`(mesh_id, cluster_index)` 取槽位基址 → 两实例的集群别名到同一批 8 个槽位），而稠密路径索引全局唯一
（`0..1620`）。CPU 侧簿记已逐项核对一致（几何/索引/非空命令数都是 16/槽位分配 patched=16 zeroed=0），
分歧在 GPU 光栅——**这是页池"按网格去重、多实例共享页"设计与"逐实例变换"的交互问题**：共享页里的顶点是
local-space 还是 world-space、shader 是否用 `gl_InstanceIndex`/`firstInstance` 反查实例变换，决定了共享是
对是错。需要单独一轮（大概率 RenderDoc GPU 抓帧看 vertex fetch）定位。测试本身正确地把这个分歧隔离了出来。

**定位**：页池 GPU 路径的**画面正确性**仍未闭环（591 像素分歧待解），但"能否在本机无窗口跑 GPU 对比"这个
前置能力问题已解决，且过程中修掉一个让页池完全画不出的真 bug（槽位表容量）。剩余分歧是页池去重语义问题，
不是 headless 基建问题。

**分歧已解（同日，commit `279463c3`）**：先反汇编/读 GLSL 源确认实例变换取法——`nanite_cluster.vert` 用
`gl_InstanceIndex` 反查 cluster、再取 `cluster.geometry.w` 的实例 transform **逐实例变换 local-space 顶点**
（顶点地址只由 index buffer 决定，`vertexOffset=0`）。所以"两实例共享 mesh_id 时集群按 `(mesh_id,
cluster_index)` 别名到同一批页槽位（draw 命令 `firstIndex` 从 756 回绕到 0）"**不是 bug**——共享页本就是正确
设计，之前误判方向。真正根因在 `UploadPageSlice`：mesh.index_data 里的索引值是 0 基于整条 mesh
cluster-vertex 流的（cluster builder 写 `out_positions.size()`，cluster c 的索引落在
`[c.vertex_offset, +c.vertex_count)`），而页只拷了该 cluster 的顶点切片进 `[dst_vertex_slot_base,
+vertex_count)`，所以索引必须先减 `cluster.vertex_offset` 再加 slot 基址。之前漏了减 —— 每个 cluster 的
索引都指到 `vertex_offset` 个槽位之后（下一个 cluster 的顶点）。cluster 0（vertex_offset=0）不受影响，所以
顶点 dump 和首个 cluster 的索引 dump 都对得上、把 bug 藏住了。稠密路径不受影响（整条 mesh 顶点流逐实例上传、
索引按整段 vertex_base 偏移）。修复后 **`NanitePagePoolABSmokeTest` 13/13 全绿**：`covered_dense=2136
covered_pool=2136 differing=0 max_channel_delta=0`，页池与稠密路径**逐像素一致**。全 Nanite 回归全绿
（ClusterBuilder 77/77、PagePool / PageSlotTable / PageUploadPlanner / HiZ 全过）。页池 GPU 路径画面正确性
**闭环**。

---

### 落地记录 · 2.5 HiZ（2026-10-26 CPU 侧；**2026-10-27 GPU 侧已落地**，见下 §2.5 HiZ GPU）

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

### 落地记录 · 2.5 HiZ GPU（2026-10-27，Vulkan headless，本机 RTX 5080）

完整记录见 **`doc/PHASE2_STEP2.5_HIZ_GPU.md`**（§6 切法一 / §7 切法二）。摘要：

- **切法一**（commit `9c57cbef`）：新增 `nanite_hiz.comp` 两阶段 compute（phase 0 深度→mip0、
  phase 1 2×2 max 下采样）+ `NaniteRenderer` 侧 GPU 金字塔资源。渲染一帧 → 读回 GPU 金字塔与 D32
  深度 → 与 CPU `BuildHiZPyramid` 逐 texel 比对：**14/14，total_mismatch=0 max_abs_delta=0**。
- **切法二**（commit `f67a6781`）：把 CPU 侧被遮挡簇判据逐行镜像进 `nanite_cluster_cull.comp`
  （binding 7 只读 storage buffer + `hiz` vec4 常量），帧首剔除读**上一帧**金字塔（时间性 HiZ）。
  新增 `NaniteHiZOcclusionABSmokeTest`：**16/16**，场景 B 画 10 簇 → 2 簇（正确遮挡 8 个后平面
  簇），GBuffer 覆盖均 4096（**无破洞**）。

**关键发现（与上面 CPU 侧记录的假设相反）**：`NaniteHiZ.h` 头注释假设 **reversed-Z**，但**真实深度
缓冲是 standard-Z**（近=0、远=1）——证据：`MakePerspectiveMatrix` 给 `ret[2][2]=zfar/(znear-zfar)`、
`ret[3][2]=-1`；深度清屏值 1.0（`NaniteRenderer.cpp:2189`）；`depthCompareOp=LESS`（`:1284`）。
所以 GPU 判据是"物体最近点（8 角点最小 ndc.z）> 区域最远 texel + eps"才判遮挡，**与 CPU 侧的
reversed-Z 判据方向相反**。这条已写进代码注释，勿再按 reversed-Z 改回去。

**Nanite 全量回归 9 套全绿**（ClusterBuilder 77、GPUCulling 40、HiZAB 14、HiZOcclusionAB 16、
PagePool / PagePoolAB / PageSlotTable / PageUploadPlanner / HiZ 全过），零 validation error。

**剩**：切法三（device-local + 真 mip 纹理 + `textureLod`，把逐 texel 循环换成单次硬件采样，
可选性能打磨）；顶点去重（`NaniteCluster.cpp:277`）按 §2 复扫结论主动推迟。

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

### 落地记录 · 2.6 VirtualTexture GPU 侧（2026-10-27，Vulkan headless，18/18 全绿）

commit `15f40a25`。把上节留的两处 GPU TODO 兑现：**页像素上传进物理页池** +
**页表纹理上传**。此前状态是"簿记全对、显存里一个字节都没有"——槽位分配了、页表项写好了，
但页池图是空的，`m_PageTableTexture` 连对象都没建出来（恒为 nullptr）。

**做法**：新增 `UploadImageRegion`（Vulkan 直连一次性命令缓冲：barrier→TRANSFER_DST、
`vkCmdCopyBufferToImage`（带 imageOffset）、barrier→SHADER_READ_ONLY）。不用现成的
`VulkanUtil::CopyBufferToImage` 是因为它把 `imageOffset` 硬编码成 (0,0,0)，而页上传要写的是
页池里**该槽位对应的那一小块**。三条容易踩反的：

- `bufferRowLength` / `bufferImageHeight` 必须传 **0**：Vulkan 单位是 texel 不是字节，
  传 `width*bpp` 会按倍数溢出（HiZ 切法一踩过同一条）。
- **整页块上传**（`page_size×page_size`）而非真实宽高：源缓冲行距是 page_size（边缘页补了 0），
  按真实宽高传会让残缺页从第二行起错位。
- **discard_prior_content 闸**：只有"这张图从没传过"才允许 `oldLayout=UNDEFINED`，之后必须走
  `SHADER_READ_ONLY→TRANSFER_DST`，否则会把已驻留的页**静默作废**。
  `PhysicalTexturePool::hasContent()` 就是这条闸（`m_PageTableHasContent` 同理）。

另：`VirtualTextureResource` 改为**保留源 TextureData**（此前只取 width/height/format 就把像素
丢了，"从磁盘取页"根本没东西可读）。页表格式用 `R32G32_SFLOAT` 而非注释里写的 `R32G32_UINT`
——本工程 `RHIFormat` 枚举没有 UINT 版，为一个页表动 4 个后端格式表不值当，槽位下标 f32 可精确表示。

**修掉的两个真 bug**（都因 VT 零消费者而一直潜伏）：

1. **`RHIImage*` 不能直接强转 `VkImage`（致命）**：`PhysicalTexturePool::Initialize` 把
   `vmaCreateImage` 出来的裸句柄直接 `(RHIImage*)` 存了，而 RHI 侧所有接口都把 `RHIImage*` 当
   `VulkanImage*` **对象**、用 `((VulkanImage*)image)->getResource()` 取句柄——拿不透明句柄数值当
   对象指针解引用，`CreateImageView` 当场访问违例。改为包一层 `new VulkanImage()` + `setResource`，
   `Shutdown` 对称 `delete`。页表纹理同样处理。
   **这条对全工程适用**：凡是绕过 RHI 自己 `vmaCreateImage`，都必须包 `VulkanImage`。
2. **`GetStats` 读到零值**：`GetStats` 返回的是只在 `Update()` 里刷新的快照，绕过 `Update` 的路径
   （如直接 `ForceLoadPages`）页全加载了却报 `loaded=0、pool_total=0`。改为按需从资源与页池当前状态重算。
   （**勘误**：初版写"`m_Stats` 从来没有任何写入者"是错的——`Update()` 写它，2026-10-27 复核更正。）

**验证**：新增 `VirtualTextureGpuUploadABSmokeTest`（Vulkan headless，选项
`ZENGINE_BUILD_VT_GPU_UPLOAD_AB_SMOKE_TEST`）。256×256 源纹理（4×4 页）→ 同步 `ForceLoadPages`
全部 16 页 → 页池整图拷回 host 逐页与源区域**逐字节**比对 + 页表纹理逐 texel 比对。
**18/18 全绿，零 validation error**：`loaded=16 pool_used=16/16`、页失配 **0 页 0 字节**、
页表坏 texel **0**。判据覆盖：①每页落在自己槽位的像素块上（offset 算错当场红）；②逐页增量上传
不冲掉先前驻留的页（discard 闸的回归保护）；③页表 texel == CPU `PageTableEntry`。

Nanite + VT 全量回归 **10 套全绿**。（**勘误**：这 10 套是原生 exe，**没覆盖 VT 的 WASM 冒烟测试**——
本轮引入的 `VulkanRHIResource.h` 让 `verify_vt_wasm.sh` 编译失败，下一节修复。）

**剩**：① mip 链（目前只传 mip 0 的页）；② 走 `RegisterVirtualTexture(path,...)` 注册的资源没有
源像素，其页只做簿记、槽位留空（如实留白，不伪造）；③ 2.4 的 RG 资源注册（VT 图资源进 RenderGraph）；
④ shader 侧真正按页表间接寻址采样（页表已可采样，但没有消费者 shader）——**这是 VT 从"能传"到
"能画"的最后一公里**。

---

### 落地记录 · 2.6 VT 页表纹理生命周期（2026-10-27，Vulkan headless，36/36 全绿）

commit `30ddd24b`。上节只做了页表纹理的**加载**方向，另外两半生命周期此前根本没实现，补齐这两个接口：

1. **失效同步**：`UnloadPage` 清 CPU 页表项却从不重传 GPU 页表 → GPU 继续说"驻留在槽位 N"，而 N 已分给
   别的页，shader **静默采到别人的像素**。现在 `SetPageTableEntry` 只标脏，`FlushPageTableTexture` 脏才传，
   `VirtualTextureManager::FlushPageTables` 遍历**所有** VT（驱逐 B 会弄脏 A 的表），由
   `ForceLoadPages`/`UnloadPages`/`Update` 批末各调一次 ⇒ 每个脏表每批恰好一次上传（原先每页一次，且在后台
   加载线程提交）。未驻留哨兵改 `-1.0f`（`float(UINT32_MAX)` 舍入为 2^32，转回 uint32 是 UB）。**懒创建**：
   从未驻留过的 VT 不建页表，`GetPageTableTexture()==null` 即"无驻留"。
2. **释放**：`Resource::Shutdown` 只置空指针、注释称"由 manager 销毁"，manager 从未销毁 ⇒ 每个 VT 泄漏一张
   页表纹理。新增幂等的 `ReleasePageTableTexture`；页池 image view 同样补销毁。

**同族真 bug**：三处自死锁（`Unregister` 持 VT 锁调 `UnloadPages→GetVirtualTexture`；池满时 `AllocateSlot`
持槽锁调 `FindLRUSlot`）——WASM 一直绿是因为无 pthreads 时 mutex 是空操作；**池满偷槽位别名**（`AllocateSlot`
把 LRU 槽改派新页而不通知原主人）——改为满则失败，由 `LoadPage` 经 `UnloadPage` 驱逐后重试；LRU 时钟改单调计数
（毫秒时钟下同批页时间戳全等）；**WASM 回归**——新增 `ZENGINE_VT_GPU` 统一守卫，Apple/Emscripten 编译掉全部
VT Vulkan 路径，WASM 恢复 15/15。

**验证** `VirtualTexturePageTableLifecycleABSmokeTest`（选项 `ZENGINE_BUILD_VT_PAGETABLE_LIFECYCLE_AB_SMOKE_TEST`）：
4 槽位池、VT-A 2×2 + VT-B 2×1（6 页 > 4 槽）。A/B = **模拟 shader 间接寻址**：读回 GPU 页表 → 槽位 → 读回
GPU 页池，必须与直接取源纹理逐字节一致。五阶段（加载 / 卸载 / 池满跨 VT 驱逐 / 注销 B / 注销 A+Shutdown），
另钉批量（上传计数精确）与无泄漏（VMA 分配数逐阶段精确、最终回到基线）；会死锁的调用跑在看门狗里。
**36/36，零 validation error**。红绿：注入"卸载不标脏"突变 → 12/36 红，还原全绿。回归：原生 11 套 + WASM 15/15。

**剩**：mip 链；path 注册无源像素；RG 资源注册；**shader 侧消费页表**（最后一公里）；页像素上传仍在后台加载线程
提交（真机异步加载前要收敛到渲染线程）；`ReleasePageTableTexture` 依赖"无在途 GPU 工作"（目前全是 single-time
提交 + `vkQueueWaitIdle` 故成立，绑进帧描述符后需改帧围栏延迟销毁）。

---

### 落地记录 · 2.6 VT 页表间接寻址 shader（2026-10-27，Vulkan headless，22/22 全绿）

commit `dbeca4c7`。页表纹理此前"能传、能验、没人读"——前两个 VT 测试的 A 路是**测试里的 C++ 在模拟 shader**。
现在第一个真正的 GLSL 消费者落地：

- `engine/shader/include/vt_common.h`：**采样契约的唯一一份**（`VTResolveTexel`/`VTFetch`/`VTSampleUV`）。
  虚拟 texel → 页表 `texelFetch`（`x<0` = 不驻留）→ 槽位 → `(slot%poolW, slot/poolW)*page_size + 页内偏移` → 页池
  `texelFetch`。任何材质 shader `#include` 它即可读 VT。**只做点采样**：跨页边界的双线性会读到相邻*物理*槽位里毫不
  相干的页（页池无 gutter），要双线性必须先给页加边界（未做）。
- `vt_resolve.comp` + `VirtualTextureResolver`：每线程一个虚拟 texel，结果写 SSBO 读回。从未驻留的 VT 无页表纹理
  （懒创建）→ CPU 兜底"全不驻留"；池图还是 UNDEFINED 时如实失败。
- `VirtualTextureResolveABSmokeTest`：A 路 = GPU 上 shader，B 路 = 源纹理/fallback。100×90 非整页纹理（右/下边缘
  残缺页）、池满跨 VT 驱逐后**被驱逐页在 shader 里读到 fallback 而不是占了它旧槽位的别的页的像素**（页表失效同步在
  shader 层面的最终证据）、整数入口 == UV 入口、卸载/重载/槽位复用。**22/22，零 validation error**。
- 红绿：在 `vt_common.h` 注入契约错误——槽位 x/y 互换 → 5 项红；丢页内偏移 → 5 项红；槽位 0 当不驻留 → 4 项红。

**构建系统真 bug（突变测试揪出）**：`cmake/ShaderCompile.cmake` 的 SPIR-V 命令只 `DEPENDS` 了 `.comp` 本身，不含它
`#include` 的头 ⇒ 改 `nanite_common.h`/`vt_common.h` 这类共享头**不会重编任何 SPIR-V**，二进制里的 shader 悄悄停在旧
契约，C++ 侧却已按新契约走。我的第一轮突变测试因此全程假绿（"改了头、重建、测试照绿"是唯一的线索）。现把 include
目录下全部文件列为依赖。**这对全工程所有 shader 生效**（Nanite 的也是）。

**剩**：材质管线接入（让真实 draw 的 fragment shader `#include vt_common.h`——需要 2.4 的 RG 资源注册与描述符绑定）；
双线性/各向异性（需页 gutter）；mip 链（现只传 mip 0）；path 注册无源像素。VT 至此从"能传"变成"shader 真能读到对的像素"，
**距"能画"只差把 `vt_common.h` 接进一个真实材质**。

---

### 落地记录 · 2.6 VT 异步加载线程收口（2026-10-27，Vulkan headless 真多线程，22/22 全绿）

commit `e25e2afd`。`enable_async_loading` **默认 true**、`Initialize` 会拉起后台加载线程，但那条路径此前一直是坏的，
只是因为从没真正跑起来才没出事（引擎里没人调 `Initialize`/`RequestPages`，`r.VirtualTexture.Enable` 默认关）。三个缺陷叠加：

1. `RequestPages` 入队后**从不 notify**（全文件唯一的 notify 是关停）⇒ worker 只会被虚假唤醒碰上。
2. 一旦跑起来就是错的：worker **直接调 `LoadPage`**，而 `LoadPage` 分配槽位、改页状态、提交 GPU 命令缓冲，
   与渲染线程并发 = 数据竞争 + 并发 queue submit。
3. cv 等的锁与队列的锁不是同一把（等待谓词无同步读队列）；关停置标志也没持锁 ⇒ 丢失唤醒。

**新线程契约**：worker **只做 CPU 活**（`ExtractPagePixels`），产出 `PreparedPage`；槽位 / GPU 上传 / 页表簿记 /
资源释放**只在 owner 线程**（调 `Initialize` 的线程）经 `PumpLoads()`/`Update()` 提交。一把锁同时保护两个队列并作为
cv 的锁；**背压**：预备未提交页 ≤ `2*max_concurrent_loads`（每页是 `page_size²*4` 字节 CPU 缓冲，不设上限会把整张大
纹理堆进内存）；`m_WorkerInFlight` 计数保证任何时刻页恰好在一个地方，`getPendingLoadCount()==0` 才真代表排空。
新增 `PumpLoads()` 与诊断接口（`getGpuThreadViolationCount`/`getAsyncPreparedCount`/`getPreparedBacklogPeak`）。

**验证** `VirtualTextureAsyncLoadABSmokeTest`（选项 `ZENGINE_BUILD_VT_ASYNC_LOAD_AB_SMOKE_TEST`）：4 线程并发
`RequestPages` + owner 同时 Pump，36 页纹理压 16 槽位池；真 shader 读回逐 texel（驻留页读自己的像素、被驱逐页读 fallback
而非别人像素）；断言 worker 真的跑了、GPU 活零次发生在 owner 线程外、背压有界、带队列关停不死锁、可重新初始化。
**22/22，连跑 10/10，零 validation error**。红绿（三种旧缺陷各注入一次）：worker 直调 `LoadPage` ⇒ **段错误 + 10 条
validation 错误**；不 notify ⇒ 2 项红；去背压 ⇒ 1 项红。**注意**：第一版测试**漏了"不 notify"**——owner 的 Pump 在腾出
预备队列后也会 notify，掩盖了缺失；后补"不 Pump、worker 仅凭 `RequestPages` 醒来并备到背压上限"的隔离断言才抓到。

**至此打开 `r.VirtualTexture.Enable` 的线程安全前置条件已满足。** 仍未满足的：引擎里没有 VT 的真实**生产者**（没人
`Initialize` 单例、没人 `RequestPages`），所以开关打开也什么都不会发生。

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
