# ZEngine RenderGraph

> 状态：已实现（2026-10-20），**尚未替换现有流水线装配**
> 代码：`engine/Source/Runtime/Function/Render/Graph/`
> 设计参考：UE `FRDG`（`E:/Engine/UnrealEngine`，`RenderCore/Private/RenderGraph*.cpp`）+ Unity RenderGraph

---

## 1. 为什么需要它

改造前，ZEngine 的渲染是"直写式"的：

| 问题 | 现状证据 |
| --- | --- |
| **屏障全靠手写** | `CmdPipelineBarrier` 的调用点几乎全在 Pass 层：`ParticlePass.cpp` 14+ 处、`MainCameraRp2Pass.cpp`、`DX12MainCameraPass.cpp`、`MegaLightsSystem.cpp`。`MainCameraRp2Pass.cpp:671` 还能看到 oldLayout == newLayout 的"只同步可见性"手写屏障 |
| **临时 RT 永不复用** | 每个 Pass 自己 `CreateImage` 一张全屏 RT，生命周期 = Pass 对象生命周期：`MainCameraPass.cpp:789`（7 张）、`PickPass.cpp:48`、`ParticlePass.cpp:267`。全仓库**没有**任何帧级 RT 池 |
| ~~**Pass 图硬编码在函数签名里**~~ | ~~`MainCameraPass::Draw(ColorGradingPass&, FXAAPass&, BindlessTonemapPass&, RenderPass&, CombineUIPass&, ParticlePass&, ...)` —— 换顺序要改 C++ 签名~~ **已解决（2026-10-22）**，见 §5.0：不是靠接图，而是靠 `RenderPassBase::AppendToDrawList` |
| **resize 要手工重连指针** | `DesktopRenderPipelineModule.cpp:480` 的 `UpdateAfterRecreate` 里 6 处 `UpdateAfterFramebufferRecreate(a, b, c)` |
| **Vulkan 侧没有 layout 影子状态** | DX12 有 `DX12RHIResource.h:371` 的 `m_States` 自动跟踪，Vulkan 完全靠 `finalLayout` 兜 |

RenderGraph 把这四件事变成编译期自动推导。

---

## 2. 架构：三段式

```
Setup（渲染线程，便宜、不碰 RHI）
   Pass 声明读什么、写什么、要什么状态
        │
        ▼
Compile（纯 CPU，确定性）
   拓扑排序 → 剔除 → 生命周期 → 内存别名 → 屏障推导 → 校验
        │
        ▼
Execute（RHI 线程，录命令）
   池化分配 transient → 批量插屏障 → 跑 Pass 回调 → 归还内存
```

**分层刻意让上层不认识 RHI**：`RenderGraphTypes.h` 只前向声明 `RHIImage/RHIImageView/RHIBuffer/RHICommandBuffer` 作为不透明指针，格式/用途/状态都有自己的 `RGFormat` / `RGTextureUsage` / `RGResourceState` 镜像枚举。所以图编译器能脱离 GPU 做无头单测（就像 Undo 系统那样）。

| 文件 | 依赖 | 职责 |
| --- | --- | --- |
| `RenderGraphTypes.{h,cpp}` | 仅标准库 | 句柄、描述符、状态枚举、回调类型 |
| `RenderGraphBuilder.{h,cpp}` | 仅标准库 | Setup 期 API：`CreateTexture` / `ImportTexture` / `ReadTexture` / `WriteTexture` / `KeepAlive` |
| `RenderGraph.{h,cpp}` | 仅标准库 | 编译器：排序、剔除、生命周期、别名、屏障、校验 |
| `RenderGraphResources.{h,cpp}` | **RHI** | transient 池、Registry、Executor、RG→RHI 枚举映射 |
| `Test/RenderGraphSmokeTest.cpp` | 仅前 3 个 cpp | 239 项无头断言（Setup/Compile；含 step 2.4 的 RP1 三分链 35 项，见 §5 更新） |
| `Test/RenderGraphExecuteSmokeTest.cpp` | 前 3 个 cpp + `RenderGraphResources.cpp` | 130 项无头断言（池 + Execute，含 RP1 + depth aspect），见 §6 |

---

## 3. 用法

```cpp
RenderGraph graph("PostChain");
RGRhiDeviceAdapter device(rhi);              // 见 §6：把 RHI 收敛成 10 个方法
RenderGraphResourcePool pool(&device);
RenderGraphExecutor executor(rhi, &device, &pool);   // rhi 只转发给 Pass 回调，可为 nullptr

// 外部资源（swapchain / 场景深度）：永不分配、永不销毁、永不别名
RGExternalTexture backbuffer {swap_image, swap_view, RGResourceState::Undefined};
auto backbuffer_handle = graph.ImportTexture("Backbuffer", backbuffer);

RGTextureHandle hdr;

graph.AddPass("Scene", RGPassType::Render,
    [&](RenderGraphBuilder& builder) {                 // Setup：只声明
        hdr = builder.CreateTexture("HDR", hdr_desc);
        hdr = builder.WriteTexture(hdr, RGResourceState::RenderTarget);
    },
    [](RenderGraphExecuteContext& ctx) {               // Execute：录命令
        rhi->CmdBeginRenderPassPFN(ctx.GetCommandBuffer(), &begin_info, RHI_SUBPASS_CONTENTS_INLINE);
        // ... ctx.GetImageView(hdr) ...
    });

graph.AddPass("Tonemap", RGPassType::Render,
    [&](RenderGraphBuilder& builder) {
        hdr = builder.ReadTexture(hdr, RGResourceState::ShaderResource);
        builder.WriteTexture(backbuffer_handle, RGResourceState::RenderTarget);
    },
    [](RenderGraphExecuteContext& ctx) { /* ... */ });

executor.Execute(graph, command_buffer);
```

**约定：先 Record，后改。** 记录发生在 Setup（真正修改在 Execute 里），所以顺序天然正确。

---

## 4. 编译器做了什么

| 阶段 | 规则 |
| --- | --- |
| **拓扑排序** | 依赖边来自资源的读写关系：producer→consumer、**WAR**（先读后写）、**WAW**（连续写）。Kahn + 最小索引优先 ⇒ 结果与声明顺序一致且确定 |
| **剔除** | 根 = 写外部资源的 Pass、写 Extract 资源的 Pass、`NeverCull` 的 Pass；沿依赖反向传播。写了一个没人读的临时 RT 的 Pass 直接消失 |
| **生命周期** | 每个资源在执行序上的 first / last Pass |
| **内存别名** | 生命周期不重叠、尺寸够用的 transient 复用同一块内存（贪心区间着色）。外部资源与 Extract 资源不参与 |
| **屏障** | 维护每个资源的当前状态：外部资源从导入状态起步，新建资源从 `Undefined` 起步；状态不同才产生一条屏障，**已处于目标态不产生**。每个 Pass 的屏障合并成一次 `CmdPipelineBarrier` |
| **校验** | 无格式 / 零尺寸、读了从未写过的资源、用读状态去写、同一 Pass 读写同一资源、同一 Pass 用两种状态、跨帧的失效句柄、贴图当缓冲用 —— 全部变成 `errors`，不是 UB |

---

## 5. 与现有流水线的关系（重要）

**RenderGraph 与现有装配并存。**

### 5.0 已解决：`MainCameraPass::Draw(7 参数)`（2026-10-22）

| 项 | 改前 | 改后 |
| --- | --- | --- |
| 签名 | `Draw(ColorGradingPass&, FXAAPass&, BindlessTonemapPass&, RenderPass&, CombineUIPass&, ParticlePass&, uint32_t, ...)` | `SetPostPasses(...)`（Setup 期注册一次）+ `AppendToDrawList(...)`（每帧贡献条目） |
| 顺序活在哪 | C++ 形参表；Desktop 与 Mobile 两个调用点各抄一遍 8 个实参 | `MainCameraPass::AppendToDrawList` 的条目顺序 |
| draw-list 条目 | 整条主相机链挤在一个 `"MainCamera"` 里 | `MainCamera.RP1` / `MainCamera.Tonemap` / `MainCamera.RP2` |
| resize 重连 | 模块里手抄 6 处（backup_odd / backup_even / post_process_odd 三个下标抄了 5 遍） | `MainCameraPass::UpdateAfterFramebufferRecreate()` 一处，唯一真相是本 pass 的附件表 |

**为什么不是"接入 RenderGraph"。** RP1（gbuffer → 延迟光照 → 前向）与 RP2（color grading → fxaa → UI → combine UI）内部全靠 `subpassInput`，而 `Render/Graph/*.h` 全文搜不到 `subpass` —— 图只能表达"整段"级别。所以那 7 个参数**不会**因为接图而消失；真正消除它的是 `RenderPassBase::AppendToDrawList`（`RenderPassBase.h:36`），那句 "so leaf passes stop taking long arg lists" 本来就是为它写的，只是之前没人覆写。

> **更新（2026-10-25，step 2.4）**：上面的"图只能表达整段级别"针对的是**原始的单 RP1 三 subpass
> 设计**。step 2.3 已把 RP1 拆成 3 个**独立** render pass（`DrawGBufferPass`/`DrawDeferredLightingPass`/
> `DrawForwardLightingPass`，各自 `CmdBeginRenderPassPFN`、subpass 恒 0，见 `MainCameraRp1Pass.cpp`），
> GBuffer 靠 `storeOp=STORE` 落到显存、被延迟光照当普通采样纹理读（`deferred_lighting.frag.hlsl` 用
> `register(t0,space1)` 的 input-attachment 语义，但 layout 与 `ShaderResource` 一致）。因此拆分后的
> RP1 拓扑**可以**在图里声明：`RenderGraphSmokeTest` 的 group 19/20（`BuildRp1Wiring`）断言了
> `GBufferPass → DeferredLightingPass → ForwardLightingPass` 的执行序、9 条屏障（4+5+0）、以及 GBuffer
> 三张 transient 的生命周期（slot 0 产生、slot 1 最后读、可别名回收）。`verify_graph_wasm.sh` headless
> 跑通 200/200。
> **勘误（同日二次复核）**：上一版此处列的"两处语义缺口"不成立——`MainCameraRp1Pass` 只在 DX12 与
> Metal 上构造（Vulkan 走旧的多 subpass `MainCameraPass`），DX12 把 `INPUT_ATTACHMENT` 映射成普通 SRV、
> HLSL 用 `Texture2D.Load`，所以 `ShaderResource` 就是实际行为；Forward 的透明混合是固定功能 blend，本就是
> `RenderTarget` 状态。group 20 拦的是"同 pass 既采样又渲染进同一张图"。复核反而在 **executor** 里找到一个
> 真 bug：导入贴图不带格式，导入的深度图拿到 COLOR aspect 屏障，D32 transient 拿到 STENCIL aspect——已修
> （`RGExternalTexture::format`），回归见 §7 executor group 13/14。

**两个不能踩的时序点**（改错都不报错，只在特定帧才看得出来）：

1. `BuildDrawLists` 在 `PrepareBeforePass()`（swapchain acquire）**之前**跑，所以 swapchain 索引必须在条目**执行时**读。把它塞进 `RenderPassContext` 会让 RP2 用到上一帧的 framebuffer —— 这也是 `RenderPassContext` 里刻意没有 swapchain 索引的原因。
2. per-frame 开关（`m_IsShowAxis` / `m_IsShowSkybox` / `m_SelectedAxis`）仍落在 **Build 期**，与旧代码在 `BuildDrawLists` 里赋值的时机一致。

**等价性**：RHI 调用序列逐条不变（RP1 Begin→3 个 subpass→End、tonemap 的 PushEvent/PopEvent、RP2 Begin→4 个 subpass→End），唯一差别是一个 draw-list 条目拆成三个，**仍需跑一次看画面确认**。

### 第一个接入点：DX12 的 RP2 后处理链

`MainCameraRp2Pass::DrawRP2`（`Passes/MainCameraRp2Pass.cpp:611`）在帧的最末端，而且**已经被拆成了三个独立的 Begin/EndRenderPass**：

| Pass | 读 | 写 |
| --- | --- | --- |
| ColorGrading | `backup_odd` | `backup_even`（开 FXAA 时是 `post_odd`） |
| FXAA（可选） | `post_odd` | `backup_even` |
| CombineUI | `backup_odd` + `backup_even` | swapchain |

教科书式的 RG 场景，而且不在主几何路径上——屏障推导出错的症状是后处理不对，不是黑屏。

**`PickPass` 已被排除**（早前曾误列为最低风险）。读完代码后三个理由：`Draw()` 是空的，真正的绘制在 `Pick()` 里，是点击触发的同步操作（自带 command buffer / fence / submit / 全量 `WaitForFences`），根本不在每帧流水线里；它最有价值的一半（拷贝 staging → `MapMemory` → 读一个像素）**不在 `IRGResourceDevice` 的 10 个方法内**，图表达不了；它的 `RHIFramebuffer` 一次性建好并绑死在特定 `RHIImageView*` 上，池子换一张图出来就失效。

### 声明已经过验证（43 项新断言）

接线之前，先把这条链在图里声明出来，并断言编译器推导出的顺序与屏障和 `DrawRP2()` 手写的一致——`RenderGraphSmokeTest` 的 group 14/15/16 就是这件事。断言覆盖：FXAA 关/开两种拓扑的执行序、各自的屏障条数（4 / 6）、`backup_odd` 只搬一次状态（CombineUI 时已在目标态，不产生第二条屏障，这正对应 `MainCameraRp2Pass.cpp:689` 那条 oldLayout == newLayout 的手写屏障），以及源码里那句"SRV 和 RTV 必须是不同贴图"的注释——在图里它是校验错误，不再是注释。

### 接线前的阻塞项（1、2 已清，3 属于接线步骤本身）

1. ~~**swapchain 无法导入。**~~ **已解决。** `RHISwapChainDesc`（`RHIStruct.h:1184`）补了 `std::vector<RHIImage*> images`，与 `imageViews` 平行（`images[i]` 就是 `imageViews[i]` 所视图）。两个后端都在建 view 的同一处建出 image：
   - DX12 `CreateSwapchainImageViews()`（`DX12RHI.cpp:2830` 附近）为每个 backbuffer 建 `DX12Image`，包住 `m_RenderTargets[i]`（`ComPtr` 加引用，不夺取所有权——GPU 内存仍归 swapchain），并在 `GetSwapchainInfo()` 里导出。
   - Vulkan 同名函数（`VulkanRHI.cpp:4144` 附近）建 `VulkanImage` 包住 `m_SwapchainImages[i]`。`VulkanImage` 只存一个 `VkImage`、完全不拥有，所以包装驱动拥有的图不花代价也不改生命周期；wrapper 由 `ClearSwapchain()` / 重建路径删除。
   - WebGL2 / Metal 的 `GetSwapchainInfo()` 仍返回空 `images`——它们不参与这条路径，调用方必须容忍 `images.size() != imageViews.size()`。

2. ~~**DX12 有自己的 swapchain 状态机。**~~ **已解决，做法是"一份真相"而不是"让图接管"。** `DX12Image` 新增 `setExternalStateStorage()`：把贴图的状态跟踪器指向外部存储。swapchain 的三张 wrapper 全部指向 `m_SwapchainResourceStates[i]`，于是：

   | 路径 | 经过 | 读写的状态 |
   | --- | --- | --- |
   | DX12 自己 | `TransitionSwapchainBuffer()` | `m_SwapchainResourceStates[i]` |
   | 渲染图 | `CmdPipelineBarrier` → `TransitionImage()` → `getState()/setState()` | **同一个** `m_SwapchainResourceStates[i]` |

   两套跟踪器由此不可能漂移，而"StateBefore 说谎"这一类 D3D12 debug layer 报错变成不可表达。`m_SwapchainSurfaceState` 保留原职（只是"可能还是 PRESENT"的廉价提示），永远以数组为准。
   附带加固：`SubmitRendering` 原来的 `== RENDER_TARGET` 判断改成 `!= PRESENT`（`DX12RHI.cpp:5945`）。旧写法在图驱动之前是对的（当时只有 `PrepareBeforePass` 碰 backbuffer），但图完全可以合法地把 swapchain 留在 `PIXEL_SHADER_RESOURCE`（例如后处理回读），此时跳过回退转换会让 `Present()` 报 "resource state is invalid for Present"。

3. ~~**`UpdateDescriptorBindings()` 的位置。**~~ **已解决。** 它每帧从 CBV/SRV/UAV 堆分配新句柄，`MainCameraRp2Pass.cpp:623` 的注释写明漏掉会黑屏。图驱动时它留在 pass 回调里（`ColorGrading` 与 `CombineUI` 各自的回调第一行），不在 `DrawRP2()` 开头——这样只在 Pass 真的要执行时才分配，被剔除的 Pass 不会白拿句柄。

### 接线策略

**已实现。** `MainCameraRp2Pass` 里新路径与老路径并存：

- **开关**：环境变量 `ZENGINE_RG_RP2=1`（`SetupRenderGraphPath()`，非空且非 `'0'` 即开）。**默认关闭**，不开时这条路径一个对象都不构造，老路径零改动。
- **回落**：`DrawRP2WithGraph()` 在任何记录动作之前做校验——swapchain 帧号越界、`images[i]`/`imageViews[i]` 为空（WebGL2/Metal 不发布 image，这时打**一次**警告后回落，不是错误）、backup_odd/even 缺失——然后 `Compile()`，不合法就 `LOG_ERROR` 每条错误并回落。`m_RgFallbackLogged` 保证日志只打一次，不会每帧刷屏。
- **回落是安全的**：`Execute()` 只可能在记录之前失败（无 device / 无池 / 图不合法，最后一条已被上面的 `Compile()` 排除），所以手写路径永远不会接在半张图的输出后面。

四个 Pass：`ColorGrading`（读 backup_odd → 写 grading_output）、可选 `FXAA`、`UI`（`RGPassFlags::NeverCull`）、`CombineUI`（读 backup_odd + backup_even → 写 backbuffer）。导入态按**贴图实际所处状态**填：backup_* 是 `ShaderResource`（RP1/RP2 HDR 都是 `finalLayout = SHADER_READ_ONLY_OPTIMAL`），backbuffer 是 `RenderTarget` 而**不是** `Present`——`PrepareBeforePass()` 每帧开头已经转过了。这个选择让 `CombineUI` 对 backbuffer 不产生任何屏障，group 18 断言了这一点。

**`UI` Pass 是一处已知脆弱点**：它不声明任何资源，只有 `NeverCull`（是根，永不剔除）和拓扑排序"同分取最小声明下标"这两件事把它钉在 FXAA 与 CombineUI 之间。group 18 断言了执行序，但 Pass 声明顺序一旦变动就会失效。

**仍需真机验证**（无头环境做不了）：用下面的同帧 A/B 抓帧（`ZENGINE_RG_AB_CAPTURE`）+ `scripts/rg_ab_compare.py` 客观比对。

### 第二个接入点：DX12 的 RP1（`DrawRP1WithGraph`，2026-10-25）

**已实现，默认关闭。** 与 RP2 同一套机制、各自独立的开关：

- **开关**：环境变量 `ZENGINE_RG_RP1=1`（`MainCameraRp1Pass::SetupRenderGraphPath()`，在 `Initialize()` 末尾调用）。
  **只在 DX12 生效**——Metal 也跑 `MainCameraRp1Pass`，但 Metal 的 `CmdPipelineBarrier` 是空实现，图在那里
  什么也不添；非 DX12 后端打开开关只打一条 WARNING、继续走手写链。Vulkan 不跑这个 pass（它走旧的多 subpass
  `MainCameraPass`）。
- **调用点**：`DX12MainCameraPass::Draw` 的 `if (m_Rp1Ready && m_Rp1Pass.DrawRP1WithGraph(...)) {} else if (m_Rp1Ready) { 手写链 }`。
  图路径替换的是"三个 `Draw*Pass()` + 中间那组 4 条手写屏障"，`Draw*Pass()` 本身原样复用、顺序不变。
- **声明单一源**：`Passes/MainCameraRp1GraphDecl.h`（header-only，只依赖 `RenderGraph.h`）。生产代码与两个
  smoke test 调用**同一个** `DeclareRp1Graph()` + `MakeRp1GraphImportStates()`，测试断言的就是生产跑的那张图，
  不是手抄副本。做成头文件而不是 `Passes/*.cpp`，是因为 Windows 构建对 `Passes/*.cpp` 是白名单。
- **全部 import、零 transient**：三个 render pass 的 framebuffer 绑死在 `MainCameraFramebufferResources` 的具体
  view 上，池子分出来的图不会是 framebuffer 渲染的那张。图贡献的是顺序、依赖校验与 pass 之间的屏障。
- **导入态 = 上一帧 render pass 留下的状态**：GBuffer `ShaderResource`、depth `DepthStencilWrite`、BackupOdd
  `ShaderResource`（理由逐条写在头文件里）。
- **depth 可选**：DX12 的 `GetDepthImageInfo().depth_image` 今天是 `nullptr`（`DX12RHI.cpp` 的 TODO），此时
  图不跟踪 depth，depth 的转换全部留给 render pass——与手写链等价（手写那条 depth 屏障也打在 null image 上，
  被 RHI 丢掉）。若将来发布了 depth image 但格式不是 D32/D24S8，拒绝并回落（否则会出 COLOR aspect 屏障）。
- **回落**：与 RP2 同一契约——所有拒绝点都在录制前，`m_RgFallbackLogged` 只打一次日志。

**render pass 内部转换与图的关系（已知、有意的）**：render pass 自己在 Begin/End 里转附件，图看不见，所以
"出 pass 时"图记的状态会比真相落后一步（图以为 GBuffer 还是 RenderTarget，End 已经把它留在 ShaderResource）。
在 DX12 上无害：`DX12RHI::CmdPipelineBarrier` 不看 `oldLayout`，按自己的跟踪态转，落后的那几条退化成 no-op
——正是手写那组屏障本来的样子（`TransitionImage` 同态早退）。这在 Vulkan 上**不**成立（oldLayout 必须精确），
这也是开关只认 DX12 的原因之一。

**验证**：`RenderGraphSmokeTest` group 21（生产声明 + 真实导入态，有/无 depth 两种拓扑：8 / 7 条屏障）、
`RenderGraphExecuteSmokeTest` group 15（经 executor：回调顺序、0 次池分配、2 次批量调用 3+5 条、逐条 layout
与 aspect）。变异测试：把 BackupOdd 导入态改成 `Undefined`，group 21 立刻 2 项 FAIL。DX12 TU 用
`scripts/syntax_check_dx12.py`（clang-cl `/Zs` + ZRuntime Debug|x64 的真实 include/define + PCH）类型检查
通过；植入错别字的负对照能被抓住。**仍需**：带 `ZENGINE_RG_RP1=1` 跑 DX12 编辑器与手写链逐帧目视对比，
并开 D3D12 debug layer 看一眼。

### 目视验证的工具：同帧 A/B 抓帧（2026-10-25）

"逐帧目视对比"已经可以变成一条命令的客观比对。**两次独立运行对比**会被与图无关的噪声淹没
（时间、粒子、Spine dt、MegaLights 时间累积、相机漂移），所以做成**同一帧内 A/B**：
`DX12MainCameraPass::Draw` 在抓帧那一帧把每一站画两遍，输入完全相同：

| 顺序 | 内容 | 抓的图 |
| --- | --- | --- |
| 1 | RP1 手写链 | `rp1.handwritten.backup_odd`（RGBA16F） |
| 2 | RP1 图路径（同一时间状态） | `rp1.graph.backup_odd` |
| 3 | RP2 手写链（读第 2 步的 backup_odd） | `rp2.handwritten.backup_even` + `rp2.handwritten.backbuffer` |
| 4 | RP2 图路径（读同一张 backup_odd） | `rp2.graph.backup_even` + `rp2.graph.backbuffer` |

图路径正确时每一对应当**逐位相同**，所以比对默认容差是 0。

**用法**：

```bash
# DX12 编辑器，等场景稳定后抓第 120 帧（不需要同时设 ZENGINE_RG_RP1/RP2，抓帧会自己强制两条路径再恢复）
set ZENGINE_RG_AB_CAPTURE=120
set ZENGINE_RG_AB_DIR=E:\tmp\rg_ab          # 可选，默认工作目录下 rg_ab_capture
# ...启动编辑器，日志出现 "RenderGraph A/B capture: wrote 6/6 images + manifest" 即可退出
python scripts/rg_ab_compare.py E:\tmp\rg_ab --png
```

退出码：`0` PASS（全部逐位相同 / 在容差内）、`1` FAIL（列出差异像素数、最大通道差、包围盒；`--png`
写两边的预览图 + 放大的差异图）、`2` INCONCLUSIVE——**"图"那一遍实际回落到了手写链**（manifest 里
`graph_used=false`，这时 A==B 是废话，绝不能算通过）、图缺失或写失败。

**实现要点**（`Passes/DX12RenderGraphAbCapture.{h,cpp}`，文件格式 `Passes/RenderGraphAbCaptureFormat.h`）：
- 回读完全异步、不让 GPU 停顿：拷贝录进抓帧那一帧的命令列表，等该 frame slot 再次轮到时（帧循环会先
  `WaitForFences()` 当前 slot）再 map + 写盘；slot 一直不来（swapchain 重建改了帧号）时 8 帧后显式等待兜底。
- 每次拷贝后把图转回下一个使用者期望的状态：backup_* 回 `SHADER_READ_ONLY`，backbuffer 回
  `COLOR_ATTACHMENT`（编辑器 overlay 紧接着画进去）。
- 两条路径用新增的 `MainCameraRp1Pass/Rp2Pass::SetRenderGraphPathEnabled()` 强制切换，抓完恢复原开关；
  RP2 新增 `LastDrawUsedRenderGraph()`，用来如实填 `graph_used`。
- MegaLights 的时间历史在两遍之间用 `MegaLightsSystem::SaveHistoryBookmark/RestoreHistoryBookmark` 复位，
  否则第二遍会在第一遍刚写的历史上再累积一次（ping-pong 也会翻两次）。
- `.zcap` = 52 字节头 + 紧密排列的行（去掉 DX12 的 256 字节行对齐）。比对脚本只用标准库；逐行先比原始字节，
  只解码不同的行，1080p 三对全同约 0.1 s，全帧不同约 1.2 s（加 `--png` 约 13 s）。逐位相同的像素算相等
  （包括两边同一个 NaN）。

**已知的假差异来源**（若比对出差异，先排除这些再怀疑图）：
- 第一遍画 RP1 时 Spine 世界实例消费了累积的 dt，第二遍 dt=0。姿态应当相同（"推进后的姿态"被两遍共用），
  但若 Spine 在绘制里还有别的每帧状态，差异会落在 Spine 实例的包围盒里。
- 两遍都从每帧 upload ring buffer 分配（上限没变）；场景极大时可能触顶，表现为第二遍缺物体。
- 进程在抓帧后 3 帧内就退出时，几块 readback buffer 没释放（`DX12MainCameraPass` 没有 Shutdown 钩子）。

**验证**：`scripts/rg_ab_selftest.sh`——用引擎同一份编码器（emcc + NODERAWFS）写出带真实 256 字节行对齐与
垃圾填充的合成抓帧，跑比对脚本，断言 PASS/FAIL/INCONCLUSIVE 的退出码、单像素差的位置与数值、容差、
`graph_used=false` 与缺图判成 INCONCLUSIVE、逐行去对齐、半精度往返、JSON 转义、PNG 合法、同 NaN 视为相等，
共 7 个场景判定 + 15 项数值检查，全绿；把编码器的行步长改错（变异）立即 FAIL（单像素位置、容差块位置、像素解码都报错）。DX12 侧 5 个 TU 经
`syntax_check_dx12.py` 类型检查通过。**GPU 那一半（拷贝、回读时机、状态恢复）只有真机能验**——抓一次帧就知道。

### 必须自己补的三块（RHI 不管）

1. **Vulkan 侧 layout 影子状态**：DX12 有内部 `m_States`，Vulkan 没有。RG 的 `current_state` 已经是这个影子状态，Executor 每次都给出正确的 oldLayout，两个后端都能用。
2. **transient 内存池**：`RenderGraphResourcePool` 从零实现，键 = 描述符哈希（FNV-1a），`m_MaxIdlePerKey` 上限 8，resize 时调 `Flush()`。
3. **屏障批量化**：现状是 Pass 逐个调 `CmdPipelineBarrier(..., 1, &b)`，RG 改成每个 Pass 一次调用。

### 已知坑

- DX12 的 `CmdPipelineBarrier` **丢弃 `srcStageMask/dstStageMask`**，buffer/UAV 一律退化成 `D3D12_RESOURCE_BARRIER_TYPE_UAV`（`DX12RHI.cpp:5346-5366`）。所以正确性**不能**依赖 stage mask——`RGMapState` 里的 stage 只是给 Vulkan 的提示。
- `RHI.h` 里 `BeginCommandBuffer`（:436）和 `BeginCommandBufferPFN`（:350）是两套重名重载，录命令统一用 `PFN` 版。
- `RHIDrawList` 的 lambda 捕获 `this`（执行时才解引用）。RG 跨线程传编译结果时必须改成值捕获的只读快照。
- `PrepareBeforePass`（swapchain acquire）在 `SubmitDrawLists` 里已调用，所以依赖 swapchain 尺寸的 transient 分配只能在 Execute 阶段定尺寸。

---

## 6. Execute 半层：窄接口 `IRGResourceDevice`

编译器不认识 RHI，但 `RenderGraphResources.cpp`（池 + Executor）原本直接依赖 `RHI*`，而 `RHI` 有 **135 个 virtual / 122 个 pure virtual**——要给它写测试桩就得实现 122 个方法，等于让"图的测试"变成"RHI 的测试"。所以把同样的分层原则再往下用一层：

```
RenderGraphExecutor ──► IRGResourceDevice（10 个方法）
                             ▲
                             └── RGRhiDeviceAdapter ──► RHI（135 个方法）
```

`IRGResourceDevice` 就是 Execute 半层真正用到的全部后端能力：

| 分类 | 方法 |
| --- | --- |
| 分配 | `CreateImage` / `CreateImageView` / `CreateBuffer` |
| 释放 | `DestroyImage` / `DestroyImageView` / `DestroyBuffer` / `FreeMemory` |
| 记录 | `CmdPipelineBarrier` / `PushEvent` / `PopEvent` |

- **`RGRhiDeviceAdapter`** 是生产侧唯一的桥：每个方法先判空 `m_Rhi` 再转发，签名与 `RHI.h` 逐字对齐。
- **Executor 的 `RHI*` 参数仍然保留**，但它只被塞进 `RenderGraphExecuteContext::GetRHI()` 交给 Pass 回调，Executor 自己一行都不碰——所以测试可以传 `nullptr`。
- 好处不只是可测：RHI 签名漂移只会撞到 adapter 这一个文件，不会污染图的测试。

### 顺带修掉的三个缺陷

| # | 位置 | 症状 | 现状 |
| --- | --- | --- | --- |
| 1 | `ResourcePool::Flush()` | 每个资源把 `m_LiveObjectCount` **减了两次**（复制粘贴，缩进能看出来）。计数提前归零，真实泄漏被掩盖。因为每次都 clamp 到 0，**只有当"有资源仍被持有"时才观测得到** | 一次一个；测试里特意留一张 acquire 未还的贴图来暴露它 |
| 2 | `Executor::Execute()` 收尾循环 | **没有跳过 `extracted` 资源**——`KeepAlive` 过的资源被直接还进池子，下一帧就能被别的 Pass 复用，与编译器"Extract 不参与别名"的承诺自相矛盾 | 收进 `m_Extracted*`，在下一次 `Execute()` 开头（或显式 `ReleaseExtracted()`）才归还；`~RenderGraphExecutor` 也会还 |
| 3 | `Executor::Execute()` 分配循环 | 声明过的资源**全部**分配，包括生产者已被剔除的——一个被剔除的 debug Pass 照样占着全屏 RT | `lifetimes[i].first_pass == kRGInvalidIndex` 的直接跳过。剔除是"从根出发的生产者闭包"，所以存活 Pass 读到的资源不可能落进这一类，安全 |

---

## 7. 验证

```bash
cmake -S . -B build -DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON \
                    -DZENGINE_BUILD_RENDERGRAPH_EXECUTE_SMOKE_TEST=ON
cmake --build build --config Debug --target RenderGraphSmokeTest RenderGraphExecuteSmokeTest
ctest -C Debug -R RenderGraph --output-on-failure
```

**编译器（`RenderGraphSmokeTest`）239 项断言 / 0 失败**（含 step 2.4 新增的 RP1 三分链 35 项 group 19/20 + 生产声明 39 项 group 21）：链式排序、剔除与反向传播、NeverCull/Extract 保活、生命周期、别名复用与重叠拒绝、屏障推导（含"已处于目标态不产生"）、重复声明拒绝、跨帧失效句柄、四类校验错误、WAR/WAW 顺序、缓冲资源、确定性、描述符尺寸、**RP2 后处理链**（§5，43 项）：FXAA 关/开两种拓扑的执行序与屏障条数、`backup_odd` 只搬一次状态、"SRV/RTV 必须异贴图"落成校验错误，以及**swapchain 往返**（15 项）：以 `Present` 导入 → 写 → 再写 → 读，断言首条是 `Present → RenderTarget` 而非 `Undefined → RenderTarget`、第二个连续写入者不产生屏障、外部资源不参与别名与预算、写外部资源的 Pass 天然是根所以不被剔除，以及**接线的真实导入态**（§5，29 项）：以 `ShaderResource/ShaderResource/RenderTarget` 导入，断言执行序为 `ColorGrading/UI/CombineUI`（开 FXAA 时中间插一个 `FXAA`）、`backup_odd` 全程不产生屏障、`backup_even` 往返 `ShaderResource → RenderTarget → ShaderResource`、backbuffer 一条屏障都没有（因为导入态已经是 `RenderTarget`）、屏障总数 2 / 4。**RP1 三分链**（§5 更新，step 2.4，group 19/20）：`GBufferPass → DeferredLightingPass → ForwardLightingPass` 执行序、9 条屏障（4+5+0：GBuffer 三张 + depth 首写 4 条、Deferred 把 GBuffer 翻成 ShaderResource + depth 翻成 DepthStencilRead + BackupOdd 写成 RenderTarget 5 条、Forward 全同态 0 条）、GBuffer 三张 transient 生命周期（slot 0 产生 / slot 1 最后读 / 可别名）、BackupOdd 外部资源跨链不别名，以及"同 pass 既采样又渲染进 BackupOdd 是编译期硬错误"这条反馈环守卫。`scripts/verify_graph_wasm.sh` headless 跑通（emcc 独立编译，node 运行，无任何 shadow/prelude）。

**执行层（`RenderGraphExecuteSmokeTest`）130 项断言 / 0 失败**（step 2.4 新增 group 13/14 共 29 项 + 生产声明经 executor 的 group 15 共 9 项：RP1 三 pass 经 executor 的回调顺序、5 条屏障分 2 次批量调用、导入资源不走池、depth 屏障 aspect 恰为 `DEPTH_BIT`（D32）/ `DEPTH|STENCIL`（D24S8）——导入格式 bug 的回归；同样由 `verify_graph_wasm.sh` headless 跑）：池的复用与 `MaxIdlePerKey` 溢出销毁、`Flush()` 只减一次（缺陷 1 回归）、Pass 按编译序执行、句柄解析与跨代失效句柄拒绝、每 Pass 一次 `CmdPipelineBarrier` 且转的是对的 image、transient 在最后一个读者之后立刻归还、buffer 走同一条路径、被剔除的 Pass 既不执行也不分配（缺陷 3 回归）、Extract 资源活过整张图并在下一次 `Execute()` 才回收（缺陷 2 回归）、缺 device / command buffer / 校验失败时的报错路径、无 device 的池是惰性而非崩。

两者都 `add_test` 注册进 CTest（根 `CMakeLists.txt:483` 有 `enable_testing()`）。

`ZRuntime` Debug 全量编译通过（含 RHI 执行层）。
