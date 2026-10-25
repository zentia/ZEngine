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
| C1 | **RenderGraph 接管流水线** | 帧图编译器：自动屏障、瞬态 RT 别名、剔除 | **已实现·默认关闭**（165 编译+92 执行断言；`ZENGINE_RG_RP2=1` 门控，默认 off） | 阶段一 RHI 注册表 | ★★★ 高价值/低风险 |
| C2 | **RenderGraph → RP1**（`render_pass_refactor`） | RP1 三子pass 拆成 3 独立 pass，纳入图管理 | **仅提案**（doc 无落地标记） | **必须在 C1 之后** | ★★ 高价值/中风险 |
| C3 | **Nanite 收尾** | 流送 → 几何压缩 → HiZ 遮挡 | **部分实现**（真 GPU dispatch 已有；**几何压缩已落地 2026-10-25**，见 §2 落地记录；剩 streaming + HiZ 两处 TODO） | C1/C2 给其 RG 管理的目标 | ★★★ 高价值/中风险 |
| C4 | **Virtual Texture 接线** | 分页纹理虚拟化，从"孤儿"到可用 | **已实现但零消费者**（6 文件无外部引用、无 CVar、3 处构建排除） | C1/C2 的 RG 资源注册 | ★★ 高价值/中高风险 |
| C5 | ~~**Vulkan `MainCameraPass` 崩溃根因修复**~~ | 跨切面阻塞：解除 Vulkan 侧验证能力 | **✅ 已被 `7e5d2521` 修掉（2026-10-25 复核关闭）** | — | ★★★ 已关闭（见 §2 step 2.0） |
| C6 | **Lumen 收尾（SSR → GPU 化）** | 动态 GI + 反射；取 SSR 一半 + 首个 Lumen shader | **✅ CPU 侧已完整**（GI/反射/接线/CVar/发布桥全落地 + 7 smoke test；2026-10-25 复核，见 §2 落地记录·2.7）。剩 SSR/HWRT/首个 shader 全 GPU 侧 | C2 的 GBuffer/depth 声明 | ★★ 中价值/高风险（CPU 侧无活，剩 GPU 放最后） |
| C7 | **编辑器/内容管线填充**（低风险稳态轨） | 平台窗口化收尾 / Timeline 四片段 / Prefab §9.4 | 各为**部分实现·机械收尾** | — | ★★ 低风险填缝 |
| — | **RHI 胖接口拆分**（阶段一 #9 遗留） | 124 纯虚 `RHI.h` 拆成职责子接口 | **未动手**（阶段一明确后置） | 触碰 4 后端 + 所有调用点 | ✗ **不建议进 Phase 2**（见 §3） |

---

## 2. 建议落地顺序（每步可验证、风险可控、能力可见）

原则继承自阶段一（`PHASE1_FOUNDATION_PLAN.md:49-53`）：先低风险高杠杆、每步能本地跑通；
阶段二额外要求"能力可见"—— 每个里程碑都应带来可演示的渲染/内容能力。

| 步骤 | 对应候选 | 子项 | 为什么排这里 |
|------|----------|------|--------------|
| **2.0**（✅ 已关闭） | C0 | Vulkan `MainCameraPass::SetupModelGlobalDescriptorSet` 崩溃根因修复 | **已被 `7e5d2521`（2026-10-22）修掉**，2026-10-25 复核确认关闭。根因：MegaLights 默认关闭时 binding 8..12 提交空资源 → `UpdateDescriptorSets` 解引用空指针；修复加判空早退 + RHI 跳过空 write。两篇文档（`CROSS_PLATFORM_PIPELINE.md` §5、`TEXTURE_COOK_PIPELINE.md` §8）写于 2026-06-02、早于修复，已加"已修复"标注。**Vulkan 验证通路已恢复，无需再动手**。 |
| **2.1** | C1 | RenderGraph RP2 目视验证 → 默认打开（**前置调查已完成，见 `PHASE2_STEP2.1_RENDERGRAPH.md`**） | 图与执行层已有 257 项断言；失败模式是"后处理不对"而非黑屏（`RENDER_GRAPH.md:136`）；一个环境变量可回滚。是所有后续渲染候选的地基。**代码层面默认打开安全**（每处失败点录制前 `return false` 干净回落），但翻默认前须先目视 diff（需显示器）。 |
| **2.2** | C1 | RenderGraph 扩面（ParticlePass / 后处理链） | 复用已验证机制；消除 `RENDER_GRAPH.md:15` 列举的手写屏障。 |
| **2.3** | C2 | `render_pass_refactor`：RP1 三分 | **必须在 2.1 之后**——此时屏障/别名机制已在真机验证过，单独做它是高风险。 |
| **2.4** | C2 | RenderGraph 接管 RP1 | 2.3 的直接收益兑现；解锁 Nanite/Lumen/VT 的图管理资源。 |
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

**剩下**：HiZ 遮挡（GPU 侧，需真机）。streaming 已落地，见下方落地记录。

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
