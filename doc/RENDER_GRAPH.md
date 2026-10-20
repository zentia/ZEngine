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
| `Test/RenderGraphSmokeTest.cpp` | 仅前 3 个 cpp | 78 项无头断言（Setup/Compile） |
| `Test/RenderGraphExecuteSmokeTest.cpp` | 前 3 个 cpp + `RenderGraphResources.cpp` | 92 项无头断言（池 + Execute），见 §6 |

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

**现在是并存的，还没有替换。** 现有装配（`DesktopRenderPipelineModule` + `MainCameraPass::Draw(7 参数)`）完全没动，因为改它必须逐帧目视验证，而这一点我在无头环境里做不到。

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

### 接线前必须解决的三个阻塞项

1. **swapchain 今天无法导入。** `RGExternalTexture` 要 `RHIImage*` + `RHIImageView*`，但 `RHISwapChainDesc`（`RHIStruct.h:1184`）只有 `imageViews` 没有 images；DX12 侧的 swapchain view 更是 `setResource(nullptr, ...)` 建的（`DX12RHI.cpp:2811`），靠 `setIsSwapchainView(true)` + backbuffer index 走 RTV。需要给 `RHISwapChainDesc` 补一份 images 并在两个后端填上。
2. **DX12 有自己的 swapchain 状态机**（`m_SwapchainResourceStates` / `m_SwapchainSurfaceState`，见 `DX12RHI.cpp:2499` 附近的注释）。图的 `current_state` 是独立的一套影子状态，两套会打架：要么让图接管，要么让 `RGRhiDeviceAdapter` 在 DX12 上写入这个 tracker。
3. **`UpdateDescriptorBindings()` 的位置。** 它每帧从 CBV/SRV/UAV 堆分配新句柄，`MainCameraRp2Pass.cpp:623` 的注释写明漏掉会黑屏。改成图驱动时它必须留在 pass 回调里，不能留在 `DrawRP2()` 开头。

### 接线策略

新路径放在开关后面、默认走老路径，`Compile()` 不合法就自动回落并打日志——这样即使图这一侧有 bug，关掉开关即可恢复。**这一步需要逐帧目视验证，无头环境做不了。**

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

**编译器（`RenderGraphSmokeTest`）121 项断言 / 0 失败**：链式排序、剔除与反向传播、NeverCull/Extract 保活、生命周期、别名复用与重叠拒绝、屏障推导（含"已处于目标态不产生"）、重复声明拒绝、跨帧失效句柄、四类校验错误、WAR/WAW 顺序、缓冲资源、确定性、描述符尺寸，以及**RP2 后处理链**（§5）：FXAA 关/开两种拓扑的执行序与屏障条数、`backup_odd` 只搬一次状态、"SRV/RTV 必须异贴图"落成校验错误。

**执行层（`RenderGraphExecuteSmokeTest`）92 项断言 / 0 失败**：池的复用与 `MaxIdlePerKey` 溢出销毁、`Flush()` 只减一次（缺陷 1 回归）、Pass 按编译序执行、句柄解析与跨代失效句柄拒绝、每 Pass 一次 `CmdPipelineBarrier` 且转的是对的 image、transient 在最后一个读者之后立刻归还、buffer 走同一条路径、被剔除的 Pass 既不执行也不分配（缺陷 3 回归）、Extract 资源活过整张图并在下一次 `Execute()` 才回收（缺陷 2 回归）、缺 device / command buffer / 校验失败时的报错路径、无 device 的池是惰性而非崩。

两者都 `add_test` 注册进 CTest（根 `CMakeLists.txt:483` 有 `enable_testing()`）。

`ZRuntime` Debug 全量编译通过（含 RHI 执行层）。
