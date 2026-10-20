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
| **Pass 图硬编码在函数签名里** | `MainCameraPass::Draw(ColorGradingPass&, FXAAPass&, BindlessTonemapPass&, RenderPass&, CombineUIPass&, ParticlePass&, ...)` —— 换顺序要改 C++ 签名 |
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
| `Test/RenderGraphSmokeTest.cpp` | 仅前 3 个 cpp | 78 项无头断言 |

---

## 3. 用法

```cpp
RenderGraph graph("PostChain");
RenderGraphResourcePool pool(rhi);
RenderGraphExecutor executor(rhi, &pool);

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

**现在是并存的，还没有替换。** 现有装配（`DesktopRenderPipelineModule` + `MainCameraPass::Draw(7 参数)`）完全没动，因为改它必须逐帧目视验证，而这一点我在无头环境里做不到。

准备好的接入缝有三处，按风险从低到高：

1. **`RenderPassBase::AppendToDrawList`**（`RenderPassBase.h:36`，现在是默认 no-op 钩子）——让每个 Pass 在 Setup 里声明读写，然后 `DesktopRenderPipelineModule::BuildDrawLists` 改成"收 node → Compile → 把 Execute lambda 塞进 `RHIDrawList`"。`RHIDrawList` 是 `vector<{name, function<void()>}>`，**完全不用改**。
2. **`MainCameraFramebufferResources`**（`Passes/MainCameraFramebufferResources.h:18`，注释已经写了 "UE-style separate simple render passes"）——先把 `ColorGradingPass` / `FXAAPass` / `CombineUIPass` 拆成独立 RG node（Vulkan 下它们今天是 MainCameraPass 的 subpass，DX12 下已经是独立小 RP）。
3. **最先吃到收益的**：用 RG 干掉 `UpdateAfterRecreate` 里那 6 处手写指针重连。

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

## 6. 验证

```bash
cmake -S . -B build -DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON
cmake --build build --config Debug --target RenderGraphSmokeTest
./build/engine/Source/Runtime/Function/Render/Graph/Test/Debug/RenderGraphSmokeTest.exe
```

78 项断言 / 0 失败，覆盖：链式排序、剔除与反向传播、NeverCull/Extract 保活、生命周期、别名复用与重叠拒绝、屏障推导（含"已处于目标态不产生"）、重复声明拒绝、跨帧失效句柄、四类校验错误、WAR/WAW 顺序、缓冲资源、确定性、描述符尺寸。

`ZRuntime` Debug 全量编译通过（含 RHI 执行层）。
