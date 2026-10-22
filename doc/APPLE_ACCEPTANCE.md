# Apple / Metal 真机验收手册

本机（macOS + Metal）已经能编出可运行的 `ZEditor` 并把场景画到 swapchain。这篇写清楚
**怎么跑起来**、**看什么算通过**、以及**这一路修掉的坑**（都是"构建期看不见、只有运行时
才炸"的那类）。

## 1. 一条命令跑起来

```bash
./scripts/run_editor_macos.sh          # configure(如需) → 构建 ZEditor → 启动
./scripts/run_editor_macos.sh --no-launch   # 只构建
```

手工版本（想自己分步执行时）：

```bash
export PATH="$HOME/Library/Python/3.9/bin:$PATH"     # cmake / ninja 装在这里
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DZENGINE_USE_VULKAN=OFF
cmake --build build --target ZEditor
cp engine/ZGameSmoke.zproject bin/Debug/
cd bin/Debug && ./ZEditor -p ZGameSmoke.zproject
```

> **必须在 `bin/Debug` 里启动**：`ZEditor` 按可执行文件所在目录找 `resource/` 和 `asset/`，
> 从仓库根目录跑会找不到字体和关卡。

## 2. 验收看什么

### 2.1 启动后不崩

编辑器窗口出现、ZSlate 面板能画出来，控制台没有 `libc++abi: terminating`。

### 2.2 每 2 秒一条的渲染心跳（关键）

日志在 `~/Library/Logs/ZEngine/engine_<日期>_1.log`，搜 `AppleRender`：

```
AppleRender: frame=241 entities=31 visible=3 scene_color_view=1 blit_ready=1 swapchain=2560x1440
```

四个字段的含义与判定：

| 字段 | 含义 | 不通过说明 |
|---|---|---|
| `entities` | `RenderScene` 里的实体数 | 0 = 关卡没加载进来 |
| `visible` | 主相机视锥内的网格节点数 | 0 = 可见性/相机有问题，画面必然空 |
| `scene_color_view` | RP1 离屏结果（backup_odd）的 `RHIImageView` 在不在 | **0 = 全屏 blit 没有源图，屏幕只有清屏色**（见 §3.3） |
| `blit_ready` | `AppleSceneBlit` 的管线是否建成 | 0 = MSL 编译失败或 shader 没拿到 |
| `swapchain` | 当前 swapchain 尺寸 | `0x0` = 窗口还没就绪 |

### 2.3 画面

场景视口里应该能看到关卡里的 31 个实体（其中 3 个在相机视锥内），而不是纯色。
编辑器 UI（ZSlate 面板 / 菜单）叠在场景之上 —— 顺序错了会是"UI 被场景盖掉"。

### 2.4 冒烟测试

```bash
cmake -B build -DZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST=ON \
              -DZENGINE_BUILD_METAL_SPIRV_SHADER_SMOKE_TEST=ON
cmake --build build
cd build && ctest --output-on-failure
```

两个 Apple 专属的冒烟测试（默认关闭，得显式开）：

| 测试 | 检查数 | 盯什么 |
|---|---|---|
| `AppleSceneBlitSmokeTest` | 31 | 离屏场景 → swapchain 的那段**手写** MSL：能编译、铺满全屏、上下没颠倒 |
| `MetalSpirvShaderSmokeTest` | 47 | 9 个**内置** SPIR-V blob 能不能过 SPIR-V→MSL：sampler 紧凑编号 ≤ 15、`Z_HAS_SPIRV_SHADERS` 是开的、非 SPIR-V 要被拒 |

MSL 只在运行时编译，构建期永远看不见错误，这两个测试是唯一的自动防线。
`MetalSpirvShaderSmokeTest` 一条就能把 §3.1 / §3.2 / §3.3 三个错全抓出来。

## 3. 这一路修掉的坑（按发现顺序）

每条的共性都是：**它不在 `.cpp` 里报错，只在运行时的 Metal 编译器 / GPU 上报错**。

### 3.1 `Z_HAS_VULKAN` 被当成"有没有 SPIR-V"在用

`MainCameraRp1Pass` / `DirectionalLightPass` 用 `#if defined(Z_HAS_VULKAN)` 包住
`CreateShaderModule(<SPIR-V>)`。Apple 不定义这个宏，于是走 `#else` 返回 `nullptr`，
pass 直接 `throw`。**但 MetalRHI 是用 SPIRV-Cross 把同一份 SPIR-V 转成 MSL 的，Metal
路径一样需要这些字节。**

修法：新增 `Z_HAS_SPIRV_SHADERS`（`engine/CMakeLists.txt`），语义是"内置 shader 以
SPIR-V 形式下发"，只在 Emscripten 和"Windows 且不用 Vulkan"（那套编 HLSL 源码）关闭。
和 `Z_HAS_LUMEN` 是同一类错误：**`Z_HAS_VULKAN` 回答的是"有没有 Vulkan"，不是"有没有
某个子系统"**。

### 3.2 MSL 的 sampler 索引上限是 15，texture 是 127

```
program_source:162:410: error: 'sampler' attribute parameter is out of bounds:
must be between 0 and 15
```

描述符槽位是 `set * 8 + binding`，材质集（set 2）的 binding 1/2/3/5 已经是 17/18/19/21。
`[[texture(17)]]` 合法，`[[sampler(17)]]` 不合法 —— **两个不能共用一个编号**。

修法：sampler 改成模块内紧凑编号（从 0 开始），并把"槽位 → 紧凑编号"的翻译表挂在
`MetalShader::samplerIndexBySlot` 上；管线创建时**拷贝**到 `MetalPipeline`（不能存指针，
pass 建完管线立刻 `DestroyShaderModule()`，shader 先死）；`CmdBindDescriptorSetsPFN`
绑 sampler 时查表，查不到或越界就不绑。

### 3.3 subpassInput 漏了显式槽位 → texture 撞车

```
program_source:290:312: error: cannot reserve 'texture' resource location at index 3
```

SPIRV-Cross 把 `subpassInput` 单列在 `resources.subpass_inputs` 里，不在
`sampled_images` / `separate_images` 里。原来没给它 `add_msl_resource_binding()`，
SPIRV-Cross 就自己从 0 开始编号：`deferred_lighting.frag` 的 `in_scene_depth`
(set 1 binding 3) 被编成 `[[texture(3)]]`，正好撞上 `brdfLUT_sampler`
(set 0 binding 3)。

修法：`subpass_inputs`（顺带 `storage_images`）也走同一套槽位映射。

### 3.4 `AsGpuMesh` / `AsGpuMaterial` 的 assert 漏了 Metal

```
Assertion failed: (resource == nullptr || resource->backend == Vulkan
                   || resource->backend == DirectX12), function AsGpuMesh
```

Metal 用的是同一套 `GpuMesh` / `GpuPBRMaterial` 布局（只是 `VmaAllocation` 那几个字段
恒为 `nullptr`），assert 里却没放行 `RenderResourceBackend::Metal`。一有网格进场景就崩。

### 3.5 离屏 attachment 建在 0x0 上（这次是最隐蔽的一个）

`DesktopRenderPipelineModule::Setup()` 在 `RenderSystem::Initialize()` 里跑，那时
`GetSwapchainInfo().extent` 还是 `(0,0)` —— 于是 RP1 的 GBuffer / backup_odd 全是
0×0 的图，`GetSceneColorView()` 恒 `nullptr`，全屏 blit 没有源图，**屏幕永远是清屏色，
而且不报错**。

日志里的证据：初始化那行打的是 `ready (0x0, fxaa=off)`，几帧之后才是
`MetalRHI: swapchain ready 2560x1440`。

修法：`MainCameraFramebufferResources` 记住自己按多大建的（`m_Extent`），新增
`SyncExtent()`；`DesktopRenderPipelineModule::BuildDrawLists()` 每帧对一次，尺寸一变
就走 `UpdateAfterRecreate()`（连带刷新 RP1 手上的 Nanite 注入 / 延迟光照输入附件）。
现在日志里会看到：

```
MainCameraFramebufferResources: swapchain 2560x1440 != framebuffers 0x0 -- rebuild needed
MainCameraFramebufferResources: recreated FBs (2560x1440)
```

## 4. 已知限制（不是 bug，但先说清楚）

- **点光源阴影 pass 在 Metal 上不做**：它靠 geometry shader 做 cubemap 展开，Metal 没有
  geometry shader。现在和 DX12 一样是 clear-only，不抛错。
- **材质贴图全是 fallback**：日志里一堆 `Material: legacy texture path ... could not be
  resolved`，是工程里的 `.tga` 没导入成 `Texture2D` 资产，采样到的是占位白图。要看真实
  贴图得先跑一次资源导入。
- **物理形状建不出来**：`Create JPH Shapes Failed / geometry reference did not resolve`，
  和渲染无关，是关卡里 mesh 引用没解析。
- **帧率**：心跳日志显示 120 帧约 4–7 秒（≈17–30 fps），其中一部分是日志落盘开销；
  没有做过性能分析。
