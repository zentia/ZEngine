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
AppleRender: frame=241 entities=31 visible=3 scene_color_view=1 blit_ready=1 rp2_tonemapped=1 swapchain=2560x1440
```

六个字段的含义与判定：

| 字段 | 含义 | 不通过说明 |
|---|---|---|
| `entities` | `RenderScene` 里的实体数 | 0 = 关卡没加载进来 |
| `visible` | 主相机视锥内的网格节点数 | 0 = 可见性/相机有问题，画面必然空 |
| `scene_color_view` | 全屏 blit 要读的那张离屏图在不在 | **0 = blit 没有源图，屏幕只有清屏色**（见 §3.3） |
| `blit_ready` | `AppleSceneBlit` 的两条管线（tonemap + 直通）是否都建成 | 0 = MSL 编译失败或 shader 没拿到 |
| `rp2_tonemapped` | 源图是不是 **RP2 已经变换过的输出** | 1 = `backup_even`（blit 走直通）；0 = `backup_odd`（blit 自己 tonemap）。两个都是"能出画面"的正常值，但**必须和 `scene_color_view` 同源**，见 §3.9 |
| `swapchain` | 当前 swapchain 尺寸 | `0x0` = 窗口还没就绪 |

启动时还应该看到这两行（RP2 起来了）：

```
UploadIblResources: BRDF LUT loaded: TRUE
Metal CreateCubeMap: uploaded mip0 + GPU-generated 8 mips (256x256, fmt=109)
RenderResource: IBL HDR upload complete (irradiance 256x256, specular 256x256, BRDF 256x256, api=2)
RenderResource: color grading LUT 'asset/texture/lut/color_grading_lut_01.png' uploaded (256x16, format=37)
AppleRp2Chain: offscreen post chain (backup_odd -> backup_even) ready (lut=1)
DesktopRenderPipelineModule(Metal): shadow passes + MainCameraRp1Pass initialized (rp2=TRUE)
MainCameraRp1Pass::SetupDescriptorSets: IBL bound (irradiance=real, specular=real, brdf=real)
```

`lut=1` 表示 RP2 拿到了真 LUT；`lut=0` 表示它用的是 1×1 中性兜底（画面没有分级，但不
会黑）。`format=37` 是 `RHI_FORMAT_R8G8B8A8_UNORM` —— 若这里出现压缩格式，LUT 会被块
压缩破坏（§4 最后一条）。

`IBL bound (...=real)` 表示 RP1 绑的是上传上去的 HDR cubemap；出现 `fallback` 说明用的是
4×4 中性灰（环境光恒定，天光和反射都没有），去查 `UploadIblResources` 那几行（§3.11）。

### 2.3 画面

场景视口里应该能看到关卡里的 31 个实体（其中 3 个在相机视锥内），而不是纯色。
编辑器 UI（ZSlate 面板 / 菜单）叠在场景之上 —— 顺序错了会是"UI 被场景盖掉"。

颜色上现在能看的是两件事：明暗层次（§3.6 的 tonemap）和**真实贴图**。工程里的 9 个
`.tga` 会在编辑器启动时被导入成 `Texture2D` 资产（见 §3.7），产物落在
`asset/_Generated/Textures/` 下并镜像源图路径，日志里首次启动应该是：

```
TextureImporter: ImportProjectTextures seeded 19 editor-platform texture(s)
```

第二次启动**不该**再有这行（产物已存在，导入是幂等的）。并且全程**不该**出现
`Material: legacy texture path ... could not be resolved`（修复前是 1.5 万条）。
判断 tonemap 有没有生效，看光源附近是不是有渐变而不是一片纯白 —— 线性 HDR 直接写进
UNORM 的表现就是高光硬切成 255 且整体发暗（§3.6）。

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
| `AppleSceneBlitSmokeTest` | 332 | 离屏场景 → swapchain 的那段**手写** MSL：能编译、铺满全屏、上下没颠倒、tonemap+gamma 的曲线和 `glsl/tone_mapping.frag` 一致；RP2 的 LUT 条带采样；IBL 的 RGBA32F cubemap 真跑一遍（`CreateCubeMap` 的 row pitch）；`m_LevelResourceDesc` 的生产者 + swap 门闩（§3.12）；视口/裁剪矩形链路（§3.13）；门闩必须清在早退之前（§3.14）；关卡清理不是空桩（§3.15）；相机必须来自 `m_CameraConfig`（§3.16）；环境光 + 方向光必须真的设进 `RenderScene`（§3.17）；Apple Tick 必须同步点光源 + 探针关卡本身（§3.18）；Nanite 的每帧推进必须在**四条 Tick 路径上都存在**（§3.21）；GPU 路径的每一步都要能被证伪，心跳带 `ngpu=`（§3.22）；编辑器 gizmo（axis）的**上传 + 绘制两半**都在，且 RHI 真的把 dynamic offset 用上（§3.23）；粒子的解耦 + Apple 名单 + 运行时接线 + **每帧推进**（§3.24）；prefab 定义树不许跑 post-load，且 flag 要真的传下去（§3.25） |
| `MetalSpirvShaderSmokeTest` | 47 | 9 个**内置** SPIR-V blob 能不能过 SPIR-V→MSL：sampler 紧凑编号 ≤ 15、`Z_HAS_SPIRV_SHADERS` 是开的、非 SPIR-V 要被拒 |
| `NaniteGPUCullingSmokeTest` | 95 | 全平台，但在 Apple 上是**真 Metal 真 dispatch**（`MTLDevice` 不需要窗口）：GLSL 与 C++ 的字节契约、打包结果、剔除计数器读回、间接绘制真的落进 pass；外加"后端能力不许靠猜是不是 Vulkan"（§3.20） |
| `MemoryPairingSmokeTest` | 23 | 分配器配对（§3.26）：Apple 上 `Produce` 出来的块**不在 mimalloc 堆区**（所以喂给 mi_free 必坏）；错配会被守卫拦下而不是崩；正确配对不触发守卫；全库扫描 0 处错配 + **扫描器自检**（含已知错配/已知正确的双向对照） |
| `ZEditorLauncherWiringSmokeTest` | 73 | **编辑器运行中双击 .zproject（§3.30）**：平台层暴露接入点、有 delegate、真的挂到 NSApp 上、openFile 真的存路径、**消费点在 `PollEvents()` 里**；Editor 侧注册了 handler、双击当前项目时不重启（钉的是判定式不是函数）。**双击 .zproject（§3.29）**：bundle 声明 `CFBundleDocumentTypes` + 导出 UTI、两处 UTI 字符串必须一致、挂 `zproject` 扩展名、构建后刷 `lsregister`；Launcher 是 `NSApplicationDelegate` 且实现了 `-application:openFile:`、delegate 挂在 `finishLaunching` **之前**、openFile 里认扩展名**且真的提交**；重启走 bundle 入口、exec 前刷日志。Launcher 接线（§3.27）：两个平台的 `OpenLauncher` 分支都接到 Launcher、Unix 那条不许退回 `reportMissingProject()`（双击闪退的那一行）；重启用 `POSIX_SPAWN_SETEXEC` 且全文件不许有 `fork()`；`runEditorUnix` 要回写最近项目；三份实现（Win32 / Apple / POSIX 桩）不能同时编；Apple 侧确实是窗口不是空桩。**应用图标（§3.28）**：Apple 分支必须解链 exe 路径再拼 ini、且解链排在拼 ini 之前；ini 打不开要刷日志；bundle 脚本要生成 icns / 写 `CFBundleIconFile` / 用**相对** symlink；Launcher 的 bundle 让位分支排在 PNG 兜底之前**且真的 return**。**扫描器双向自检**（含"位置版扫描器也跳过整行注释"） |

MSL 只在运行时编译，构建期永远看不见错误，这两个测试是唯一的自动防线。
`MetalSpirvShaderSmokeTest` 一条就能把 §3.1 / §3.2 / §3.3 三个错全抓出来。

另外 `-DZENGINE_BUILD_PROJECT_INFO_PATHS_SMOKE_TEST=ON` 是跨平台的，盯的是 §3.7(a) 那
四个目录字段有没有被真的反序列化（23 检查）。

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

### 3.6 blit 里没有 tonemap / gamma（颜色不对，但不会报任何错）

跟前面几条不同 —— 这条**不会在日志里留下任何痕迹**，只能肉眼或靠测试发现。

链路是这样：RP1 的 `backup_odd` 是 `R16G16B16A16_SFLOAT`（**线性 HDR**，可以 >1），
而 Metal 的 swapchain 是 `BGRA8Unorm`（**非 sRGB**）。Vulkan/DX12 上 HDR→LDR 这一步
由 RP2 的 `tone_mapping → color_grading → combine_ui` 完成，而这三个 pass 在 Apple 上全
被 `ZENGINE_APPLE_EXCLUDED_PASSES` 排除了。于是 Apple 上唯一剩下的出口 ——
`AppleSceneBlit` 那段手写 MSL —— 原来是**直通采样**：线性值被当成显示值直接写进
UNORM。结果是 >1 的地方硬切成纯白，其余整体偏暗。

修法：把 `engine/shader/glsl/tone_mapping.frag` 的 Uncharted2（曝光 4.5、白点 11.2、
gamma 1/2.2）**逐系数**抄进 blit 的 fragment MSL。

对拍方式：`AppleSceneBlitSmokeTest` 里放了一份同一条曲线的 C++ 镜像，要求 GPU 出来的
像素等于镜像算出来的值（实测 156.0/192.0/210.0 vs 156.4/191.6/210.4）。三份实现
（`tone_mapping.frag` / blit 的 MSL / 测试镜像）互相看不见，这是唯一把它们对齐的手段。

> 这段 tonemap 现在**已经**从 blit 里挪走了 —— 见 §3.9：Apple 上有了真正的 RP2
> （`AppleRp2Chain`），blit 退化成直通。

### 3.7 两个"完全无声"的资源管线 bug（贴图永远是占位白图）

症状是 1.5 万条 `Material: legacy texture path ... could not be resolved`，材质全部采样
占位白图，但**没有任何一条日志说"我找错目录了"**。根因有两个，都在"静默失败"这一层：

**（a）`ProjectInfo` 的目录字段从来没有真的被反序列化过。**

`content_dir` / `config_dir` / `saved_dir` / `source_dir` 是裸 `std::string` 且没有
`ZPROPERTY()` ⇒ 生成的 `Transfer()` 里根本没有它们 ⇒ `.zproject` 里的
`content_dir: asset` 被当成陌生 key 丢掉 ⇒ `GetProjectContent()` 永远返回
`<project>/Assets`。真实资源在 `<project>/asset`（小写），macOS 文件系统区分大小写，
所以这条链暴露得最彻底：

```
content_dir 恒为 "Assets" → 扫描/导入/cook 全走空目录
→ 9 个 .tga 一个都没被导入成 Texture2D .zasset
→ Material 的 legacy 贴图路径解析不到 → 采样占位白图
```

修法：四个字段加 `ZPROPERTY()` 并把类型换成 `eastl::string`（`SerializeTraits` 只对
`eastl::string` 有特化，见 `SerializeTraits.h`）。顺带修 `TextureImporter::CanImport()`
里的 `.tag`（`.tga` 的笔误），以及 `LoadFromFile` 里对裸文件名的 `weakly_canonical` ——
`-p ZGameSmoke.zproject` 的 `parent_path()` 是空串，`project_path.empty()` 会让所有
导入器静默 bail。

回归门禁：`ProjectInfoPathsSmokeTest`（23 检查，
`-DZENGINE_BUILD_PROJECT_INFO_PATHS_SMOKE_TEST=ON`）。摘掉 `ZPROPERTY()` 它会立刻红 4 条。

**（b）`ObjectManager::Produce()` 出来的对象被 `MemoryManager::DestroyObject()` 释放。**

修好 (a) 之后编辑器改成 2 秒内 SIGBUS。崩溃栈：

```
MemoryManager::Free -> mi_usable_size -> mi_checked_ptr_segment -> EXC_BAD_ACCESS
MemoryManager::DestroyObject<Object>(Object*)
TextureImporter::Import -> ImportProjectTextures -> EditorAssetManager::Initialize
```

`Produce()` 走 `ProduceHelper::Produce()` = 普通 `new`；而 `MemoryManager::DestroyObject`
走 mimalloc。**Apple 上全局 `operator new/delete` 刻意没有接管成 mimalloc**（见
`MemoryManager.cpp` 顶部注释），于是系统 malloc 的指针被交给 `mi_free`。Windows/Linux 上
`new` 就是 mimalloc，所以这个错在那两边一直是隐形的 —— 贴图导入这条路径以前压根没被
走过，一走就炸。

修法：新增 `ObjectManager::Destroy(Object*)`（`delete`，instance-ID 由
`Object::~Object()` 摘掉），把资产管线里 16 处
`MemoryManager::DestroyObject(produced)` 全换掉：TextureImporter ×3、MeshImporter ×3、
FontImporter ×2、DataTableImporter ×2、XlsxImporter ×2、AssetBundleBuilder、
FbxAnimation.h、UMGAssetIO、ContentBrowserAssetActions。
`ShaderImporter` 用的是 `MemoryManager::CreateObject`，配对本来就是对的，没动。

> 判据很简单：**`CreateObject` 配 `DestroyObject`，`Produce` 配 `ObjectManager::Destroy`**。
> 混用只在 Apple 上致命，但错的那一半在任何平台都是错的。

### 3.8 导入产物从"源图旁边"挪进 `_Generated/Textures/`

(a) 修好之后贴图能显示了，但产物 `.zasset` 是**写在源图旁边**的（`gold.tga` →
`gold.zasset` 同一目录）。这是设计债：导入产物是派生数据，源图是签进 VCS 的，两者混在
一起分不清"该留的"和"能重算的"；而且 content 被整体重建时它们和源图一起消失。

改成落进仓库自己约定的派生数据桶 —— `_Generated/Data/`（CSV）、`_Generated/Shaders/`
（.shader）已经这么做了，贴图补上第三个 `_Generated/Textures/`，镜像源图在 content 下的
相对路径：

```
asset/objects/_textures/gold.tga
  -> asset/_Generated/Textures/objects/_textures/gold.zasset
```

配套改动：
- 新增 `ProjectInfo::GetGeneratedTexturesRoot()` / `GetGeneratedTexturePath(source)`（和
  `GetGeneratedDataRoot()` 同一族）。源图不在 content 下时退化成只镜像文件名。
- `Material::AssignTextureFromAssetPath()` 先查规范化位置，**再**回退老的两个候选
  （源图旁边 / content 下同名）—— 老工程已导入的产物靠回退继续工作。
- `ImportProjectTextures` 的"注册表补种"分支判据从"注册表记的那个路径在不在"改成
  "规范化产物在不在"：注册表里还留着指向旧位置的条目，用旧路径判断会让它们每次启动
  都重导一遍。
- `CookProjectTextures` 的 editor 产物推导同步改用 `GetGeneratedTexturePath()`。
- `EnsureScriptsScaffold` 写进 `.gitignore` 的规则原来是写死的 `/Assets/_Generated/`，
  对 `content_dir: asset` 的工程根本不匹配 ⇒ 改成跟着 `content_dir` 走。

验证：19 个产物全部落在 `_Generated/Textures/` 下、源图旁边一个 `.zasset` 都不剩；
legacy 告警仍然 0（说明材质走新路径解析到了）；**第二次启动不再出现 `seeded` 行**（导入
幂等，这是挪位之前做不到的）。`ProjectInfoPathsSmokeTest` 补 4 条路径映射断言（27 检查）。

### 3.9 Apple 上启用 RP2：显示变换回到它该在的位置

§3.6 的修法把"把离屏结果搬到屏幕"和"把 HDR 压成 LDR"两件事焊进了一次采样。现在拆开了：

```
RP1 → backup_odd (线性 HDR)
      ↓  AppleRp2Chain step 0（离屏，自己的 render pass）
      backup_even (LDR)
      ↓  AppleSceneBlit（直通，在 swapchain pass 里）
      swapchain
      ↓  post-UI 回调
      编辑器 UI
```

**为什么不是复用 `Passes/MainCameraRp2Pass.cpp`**（两个硬障碍）：

1. 它只认 HLSL（`CreateShaderModuleFromFile` 传 `.hlsl`），而 MetalRHI 的
   `CreateShaderModuleFromFile` **只收 `.metal`**，HLSL 直接返回 nullptr。换 SPIR-V blob
   也不行：`glsl/color_grading.frag` 和 `glsl/combine_ui.frag` 用的是 `subpassInput`
   （Vulkan input attachment），Metal 没这个概念，那份 SPIR-V 在 Metal 上编译不了。
2. 它的 step 3（combine_ui）写 swapchain framebuffer，而 Metal **没有持久 swapchain
   image**（`GetSwapchainInfo().imageViews` 是空的），那一步在 Apple 上没有可绑的目标。
   Apple 的 UI 本来就直接画进 swapchain，也不需要它。

所以新增 `AppleRp2Chain`（MSL，只跑离屏那一半），顺带把 `MainCameraRp2Pass.cpp` 的
`DX12RHI.h` include 收进 `_WIN32` 守卫 —— 那个 TU 一个 d3d12 符号都没用，收完之后它在
Apple 上**编译通过**，从 `ZENGINE_APPLE_EXCLUDED_PASSES` 里放了出来（只是编译校验，运行时
不用它）。

附件和 render pass 全部是现成的：`MainCameraFramebufferResources` 早就建好了
`m_Rp2HdrRenderPass` / `m_Rp2ColorGradingFramebuffer`，之前只是没人用。

**最容易悄悄坏掉的一环（也是这次唯一的新风险）**：blit 现在有**两条**管线（tonemap /
直通），共用同一个 pipeline layout 和 descriptor，只有 fragment 函数不同。"源图"和"要不
要 tonemap"必须来自**同一个**判断，否则画面只是"有点发灰"，没人会当 bug 报。钉法：

- `AppleRp2Chain::GetOutputView()` 和 `CanRender()` 用**同一套**判据（不写第二份近似）；
  `DesktopRenderPipelineModule::GetSceneColorView()` 与 `IsSceneColorTonemapped()` 都直接
  复用它；`RenderSystem` 用 `!IsSceneColorTonemapped()` 决定 blit 的那一位。
- 判据每帧重算：swapchain 头几帧是 0x0，RP2 那一帧跑不了，源图要能退回 `backup_odd`。
- 两条 MSL 是**两个独立的字符串**，不是一个 library 里的两个函数：MetalRHI 找入口时
  硬编码 `"main"`，落空会退回 `library.functionNames.firstObject`，放两个函数必然取错。
- `AppleSceneBlitSmokeTest` 里跑真 GPU 对拍：直通那次要求**逐行等于源值**
  （实测 66.0/129.0/189.0 = 源），同时必须**不等于** tonemap 那次的 156/192/210 ——
  差 40+ 灰阶，误用管线一眼就能看出来。加上 RP2 的文本契约，测试从 51 检查涨到 62。

### 3.10 接上颜色分级 LUT（RP2 的第二个槽位）

`rendering.global.json` 里的 `color_grading_map`（`asset/texture/lut/color_grading_lut_01.png`，
256×16）以前只是被上传成一张贴图，**从来没有被采样过**：Vulkan 的 `glsl/color_grading.frag`
算出 `_COLORS` 然后把 `texture(...)` 那行注释掉了，DX12 的 `hlsl/rp2/color_grading.frag.hlsl`
写明 "LUT is bound but unused"。所以这是全库第一次真的查这张表。

**条带布局**（用真实像素对拍确认，不是猜的）：16 个 16×16 切片横向排开，也就是一个
16×16×16 立方体摊平成横带 —— `x = 切片(b) * 16 + r`，`y = g`。判据：固定 r=g=0.5 把切片从
0 推到 15，输出蓝通道 0.22 → 0.59 单调上升而红通道几乎不动 ⇒ 切片确实是 b。

**采样序：tonemap → LUT → gamma**。这条序没有既有实现可抄，是自己定的，依据是 LUT 内容
本身：沿灰阶对角线，输出大致是输入再叠一条 S 形对比曲线（t=0.2→0.13，t=0.5→0.35，
t=0.9→0.95），离"恒等"比离 `t^(1/2.2)` 近一个数量级（最大偏差 0.10~0.19 对 0.36~0.43）
⇒ 它在线性空间工作，必须放在 gamma 之前。**套反了画面会整体压暗一档，看着像"美术风
格"，没人会当 bug 报** —— 所以顺序写进了注释和文本契约。

三个容易踩的点，都有断言盯着：半 texel 内缩（没有它 r=1 会渗进下一个切片）、跨切片手工
mix 并钳在最后一片（硬件的线性过滤不会跨切片插值，只采一片蓝通道会有 16 级色带）、
`level(0)`（低一级 mip 会把相邻切片糊到一起）。

LUT 视图**每帧重取**（`ResolveLutView`），不是初始化时缓存一次：关卡热重载会重新
`UploadGlobalRenderResource`、换掉这张 view。缓存下来就是"换了 LUT 但画面不变"。取不到时
用 1×1 中性 texel 兜底 —— 少了它，`[[texture(1)]]` 绑的是空，Metal 采样返回 0 ⇒ 整屏黑，
而且日志里什么都没有。

**（a）Apple 上 LUT 从来没上过 GPU。** `RenderSystem::Initialize` 有一份 Apple 精简版
（文件开头 `#if defined(__APPLE__) || defined(__EMSCRIPTEN__)`），它刻意不加载
`GlobalRenderingRes`，因此**从不调用 `UploadGlobalRenderResource`**。IBL 那边有
`EnsureFallbackIblTextures` 兜底（日志里那条
`[E] MainCameraRp1Pass::SetupDescriptorSets: IBL textures unavailable (device removed?)` 就是
这么来的 —— 它是"没上传"，不是"设备掉了"），但 LUT 没有任何兜底之外的出路。真机第一次跑
出来是 `AppleRp2Chain ... ready (lut=FALSE)`。

修法：把 LUT 段从 `UploadGlobalRenderResource` 里拆成 `RenderResource::UploadColorGradingLut(
rhi, path)`，Apple 那条精简版 `Initialize` 单独调它（只补 LUT，不顺带读 6 张 HDR cubemap ——
那是另一件事，而且 RP1 现在用兜底 IBL 跑得好好的）。修完真机日志：
`RenderResource: color grading LUT 'asset/texture/lut/color_grading_lut_01.png' uploaded
(256x16, format=37)` + `AppleRp2Chain: ... ready (lut=TRUE)`。

**（b）LUT 会被烤成 BC7。** `TextureImporter` 把 `asset/texture/lut/*.png` 一起导入成
`_Generated/Textures/texture/lut/*.zasset`（`cook=BC7`），而 `LoadTexture` 默认**优先返回烤
好的那份**。4×4 块压缩的查表 = 色带 + 错色，而且从日志到画面都不报错。修法：给
`RenderResourceBase::LoadTexture` 加 `allow_cooked` 参数，LUT 传 `false` 强制走源图解码。
真机确认 `format=37`（`RHI_FORMAT_R8G8B8A8_UNORM`，未压缩）。

顺带把 LUT 段的守卫补上：以前既没有 `m_ColorGradingMap` 空检查（DX12 那份 twin 有）也没有
失败日志，`LoadTexture` 返回 nullptr 时下一行直接解引用。现在三种出口都有日志，成功那行
打出尺寸 —— 真机上靠它确认 LUT 真的到了 GPU、以及它是不是 256×16（切片数是从高度推的）。

**验证**：`AppleSceneBlitSmokeTest` 从 62 涨到 **103 检查**。除了文本契约（binding 1 的三
处手工对齐、内缩、钳位、mip 0、采样序），还加了三层：

- 条带公式的 C++ 镜像在**恒等 LUT** 上必须是逐通道恒等 —— 轴向搞反给出排列，少了内缩给
  出整体缩放，都远超 0.01 容差。
- 从 `AppleRp2Chain.cpp` **抠出真身 MSL 字符串**建管线跑一遍（不是复制品，复制品和真身会
  各自漂移）。恒等 LUT + **彩色**场景（R/G/B 三条通道各走各的斜坡）：实测
  src=82/128/173 → expect=167.9/191.2/206.2 → got=168/191/206。
- 再跑一遍**常量 LUT**（整张表都是 0.25/0.5/0.75）：输出必须等于这三个值的 gamma
  （136/186/224，实测吻合），且必须明显不同於恒等那遍 —— 恒等 = 什么都不做，"绕过 LUT"
  也能长得一样，只有第二遍能证伪。
- 变异检验：把切片轴从 `c.b` 改成 `c.g`，蓝通道退化成绿通道的值（206 → 191），测试立刻红。

### 3.11 接上真 IBL（6 张 irradiance + 6 张 specular HDR + BRDF）

§3.10 (a) 只修了 LUT，IBL 还是 `EnsureFallbackIblTextures` 造的那张 **4×4 中性灰 cubemap +
1×1 BRDF** —— 环境光恒等于一个常数，天光和环境反射全没了，而日志里只有一条把人往"设备掉
了"上带的 `[E]`。

**坑 A：Metal 的上传路径硬编码 4 字节/texel。** `MetalRHI::CreateGlobalImage` 和
`CreateCubeMap` 都用 `bytesPerRow = width * 4`。对 R8G8B8A8 的兜底图和 UI 白图是对的，但对
HDR 是错的 —— `stbi_loadf` 出来的是 `R32G32B32A32_SFLOAT`，**16 字节/texel**。按错的 row
pitch 去 `replaceRegion`，数据是既错位又越界的。这是修 IBL 的真正前置条件：不修这个，"真
IBL"上传上去也只是噪声（实测：把这个值改回 4，cubemap 采样出来是 0/0/0）。

修法：加 `BytesPerTexel(RHIFormat)`，两处上传都按格式算 row pitch；未知格式返回 0，宁可不
上传也不按 4 猜（并打 `[E]`）。

**坑 B：mip 链。** shader 里是 `lod = roughness * MAX_REFLECTION_LOD`（=8），没有 mip 链的话
粗糙表面采到的是 mip 0 ⇒ 粗糙和镜面长得一样。Vulkan 用 `GenerateTextureMipMaps`（blit），
DX12 因为"mipgen 全屏 pass 会在首帧之前把设备搞掉"退回 mip0-only。Metal 走 Vulkan 那条路：
`generateMipmapsForTexture:` 是 blit encoder 的一步，不需要 render target，也就绕开了 DX12
那个坑。它要求纹理带 `ShaderWrite` 用途（不只是 `ShaderRead`），所以 mip 数 > 1 时才加这个
bit。真机日志：`Metal CreateCubeMap: uploaded mip0 + GPU-generated 8 mips (256x256, fmt=109)`。

**接线**：把 IBL 段从 `UploadGlobalRenderResource` 拆成
`RenderResource::UploadIblResources(rhi, const LevelIBLResourceDesc&)`，Apple 那条精简版
`Initialize` 在 `m_RenderPipeline->Initialize` **之前**调它 —— RP1 的 `SetupDescriptorSets`
就在那儿把 descriptor 写死了，晚一步得额外走
`MainCameraRp1Pass::RefreshMeshGlobalIblDescriptors()` 重绑。

顺带两处可观测性：

- `SetupDescriptorSets` 现在每次都打
  `IBL bound (irradiance=real, specular=real, brdf=real) -- 'real' = the uploaded HDR
  cubemaps, 'fallback' = the 4x4 neutral stand-in`。以前"环境光其实是 4×4 中性灰"这件事在日
  志里完全看不出来，Apple 上挂了很久没人发现。
- 那条 `[E] ... IBL textures unavailable (device removed?)` 改成 `[W] ... no IBL textures
  bound yet ...`。

**验证**：`AppleSceneBlitSmokeTest` 新增 25 条检查，总数 **132**。新增部分里有一条是真跑
GPU 的，其余是文本契约 —— 前者是这份测试里唯一能证伪"每 texel 字节数"的地方：编译期看不
见 `replaceRegion` 的 row pitch，日志也不报，只有真采一次才看得出来：

- 用 `R32G32B32A32_SFLOAT`（真 IBL 的格式，不是兜底图的 R8G8B8A8）建一张 4×4 cubemap，面
  内 4 个 texel **不相等**（r 沿 x 爬、g 沿 y 爬），否则 pitch 错了也看不出来。
- 从 +X 方向（Metal 的第 0 个 slice）正中心采一次：4×4 面内双线性落在 x=y=1.5 ⇒
  r=g=0.45（115）、b=0.60（153）。实测 `got=115.0/115.0/153.0`。
- 变异检验：把 `R32G32B32A32_SFLOAT` 的返回值改回 4，采出来变成 `0/0/0`，测试立刻红。

真机（2560×1440，跑到 481 帧无 `[E]`）：

```
UploadIblResources: irradiance loaded: +X=TRUE, -X=TRUE, +Y=TRUE, -Y=TRUE, +Z=TRUE, -Z=TRUE
UploadIblResources: specular loaded:   +X=TRUE, -X=TRUE, +Y=TRUE, -Y=TRUE, +Z=TRUE, -Z=TRUE
UploadIblResources: BRDF LUT loaded: TRUE
Metal CreateCubeMap: uploaded mip0 + GPU-generated 8 mips (256x256, fmt=109)
RenderResource: IBL HDR upload complete (irradiance 256x256, specular 256x256, BRDF 256x256, api=2)
RenderResource: color grading LUT 'asset/texture/lut/color_grading_lut_01.png' uploaded (256x16, format=37)
MainCameraRp1Pass::SetupDescriptorSets: IBL bound (irradiance=real, specular=real, brdf=real)
AppleRp2Chain: offscreen post chain (backup_odd -> backup_even) ready (lut=TRUE)
```

### 3.12 全局资源热重载：补生产者，顺带挖出整条 swap 通道的死锁

**坑 A —— `m_LevelResourceDesc` 全库没有生产者。** `RenderSwapData::m_LevelResourceDesc` 被
两条 `ProcessSwapData()` 读、`IsReadyToSwap()` 拿它当门闩，但**没有任何地方赋值** ⇒ "关卡
热重载刷新全局资源"在任何平台都是死路，而且死得无声无息：编译期一切正常，运行期只是"换
关卡后 IBL 还是旧的那份"。

补的东西：

- `RenderSwapData::SetLevelResourceDesc()`（**覆盖**语义：全局资源是一整份替换，同一帧排
  两次只有最后一次有意义）＋ `LevelResourceDesc` 的 `operator==`/`!=`。
- `RenderSystem::RequestGlobalResourceUpload()`：读 `GlobalRenderingRes` → 组装
  `LevelResourceDesc` → 写 **logic** swap data。挂在 `ResubmitActiveLevelRenderers()` 上
  （`EditorSceneManager::AfterSceneOpened` / `FileMenu` / `ZSlateSceneWindow` 三个关卡切换点
  都调它）。Apple 上这个函数以前是空桩，现在和 PC 共用一份平台无关实现。
- 去重**只**针对"上一次请求还在 render buffer 里没被消费"。**故意不跟"上次已上传的那一份"
  比**：重载关卡要的不只是"内容变了要重传"，更是"传完要重绑 descriptor"；按内容去重的话
  开场景时 desc 永远和启动时那份相同 ⇒ 一次都不会重绑。
- 两个 `ProcessSwapData()` 上传完都调 `MainCameraRp1Pass::RefreshMeshGlobalIblDescriptors()`
  （RP1 的 `mesh_global` descriptor 在 `SetupDescriptorSets` 里就写死了）。Apple 那条只补
  `UploadIblResources` + `UploadColorGradingLut`，不走整套 `UploadGlobalRenderResource`
  （那要顺带建 texture streaming / MegaLights，在 Metal 上没验过）。

**坑 B —— Apple 的 `ProcessSwapData` 漏消费 ⇒ swap 永久死锁。** A 补完以后真机一跑：日志里
`RequestGlobalResourceUpload: queued ...` 出现了，但渲染线程那行永远不出现。查下去是

```cpp
bool IsReadyToSwap() const   // 只要有一个字段挂着，swap() 就永远不会被调用
```

而 Apple 那份 `ProcessSwapData` 只处理三件事（全局资源 / GameObject 增删改 / 坐标轴），
粒子、相机、场景视口一概不碰。`1-1.scene` 里有一个粒子发射器，`ParticleManager` 提交一次
`m_ParticleSubmitRequest` 之后就再没人清它 ⇒ **第 N 帧之后整条 swap 通道永久死锁**。

临时打点（`SwapLogicRenderData` 里打印 render buffer 的每个门闩字段）确认：

```
[SWAP-BLOCKED] level=0 go=0 del=0 cam=0 particle=1 tick=0 xform=0 viewport=0
```

影响远不止热重载：游戏线程之后写的**任何** swap 数据都送不到渲染线程。修完以后
`entities` 从 31 变成 30（1-2.scene 的 30 个 renderer 真正进来了）—— 以前那 31 个只是死锁
之前抢进去的第一批。

修法：Apple 的 `ProcessSwapData` 末尾把粒子 / 相机 / 场景视口三个门闩字段清掉（Apple 上没
有消费者，但**必须**清，否则下一帧继续死锁）。粒子**故意不**交给
`RenderPipeline::ConsumeParticleSwapData()`：那条路径会在 Metal 上建 GPU 资源，没验过。

真机（切到另一张关卡，18:05:15）：

```
RequestGlobalResourceUpload: queued global resource upload (brdf=asset/texture/global/brdf_schilk.hdr, lut=asset/texture/lut/color_grading_lut_01.png)
ResubmitActiveLevelRenderers: 30 renderer(s)
Restored last scene: level/1-2.scene
ProcessSwapData: level resource swap data arrived -- re-uploading IBL + LUT and rebinding
RenderResource: IBL HDR upload complete (irradiance 256x256, specular 256x256, BRDF 256x256, api=2)
RenderResource: color grading LUT 'asset/texture/lut/color_grading_lut_01.png' uploaded (256x16, format=37)
AppleRender: frame=121 entities=30 visible=2 scene_color_view=1 blit_ready=1 rp2_tonemapped=1 swapchain=2560x1440
```

想自己复现：把 `asset/level/1-1.scene` 复制成 `1-2.scene`，再写
`bin/Debug/saved/config/editor_project_prefs.json` = `{"LastOpenedScene": "level/1-2.scene"}`
（level url **不带** `asset/` 前缀，`LevelUrlToAbsolutePath` 自己会加），启动时
`TryRestoreLastOpenedScene` 就会真走一次 `OpenSceneInternal` → `AfterSceneOpened`。
注意 `scripts/run_editor_macos.sh` 会 `rm -rf bin/Debug/asset` 重拷，跑之前先确认 1-2 还在。

### 3.13 视口链路：Metal 的 `UpdateViewport` 从不更新 scissor（DX12 会）

**发现的曲折**：本来以为 Apple 上场景视口整条链路是断的
（`TryGetRenderSceneViewport` 是 `return false;` 的桩）。修到一半发现两个假设都不对 ——
`RenderResourcePerFrame` 那两处消费者紧跟一段一模一样的 RHI 退化解，等于没差；而真正的
主相机 pass 在 Apple 上走的是 **`game`（type 0）** 视口，编辑器场景面板的矩形由它承载，
而且真的被发布和应用了。在 `SetViewportScissor` 里塞了个临时探针（真机 50 秒、6027 次）：

```text
[PROBE] SetViewportScissor type=0 vp=(389,104) 1525x915 sc=(389,104) 1524x915   ← 每帧的主路径
[PROBE] SetViewportScissor type=1 vp=(0,0) 2560x1440 sc=(0,0) 2560x1440        ← 只在启动出现 3 次
```

**真正的 bug**：`DX12RHI::UpdateViewport` 同时写 `m_Viewports[id]` **和** `m_Scissors[id]`，
`MetalRHI::UpdateViewport`（`MetalRHI.mm`）只写 `m_Viewports` —— `m_Scissors` 只有
`SyncLayerMetrics`（swapchain 创建 / resize）会重算，一进来就重置成整窗。真机证实
（去掉那一行 = 修复前行为）：

```text
修复前  game_sc=(0,0) 2560x1440   ← scissor 永远整窗，编辑器面板挪动不跟
修复后  game_sc=(389,104) 1524x915  ← 跟着面板走
```

viewport 变换会把几何钳在面板矩形里，所以修复前画面"看起来也对"——但任何**只靠
scissor 钳制**的路径（pipeline 静态 scissor、只设 scissor 的全屏 pass、`DebugDrawManager`
）都允许把像素画到编辑器其它面板的区域上，而且 `scene`（type 1）那份在 macOS 上没有
发布者（只有 `PlayModeView` 关窗时清成 0x0），整个字段是无人维护状态。

**修了三处**：

1. `MetalRHI::UpdateViewport` —— 补上 scissor 同步（和 DX12 一字对齐）；
2. `RenderSystem::TryGetRenderSceneViewport`（Apple 分支）—— 删掉 `return false;` 的桩，
   直接从 RHI 读 viewport + scissor。Apple 没有渲染线程，RHI 状态就是最新的，
   不需要非 Apple 那条跨线程快照往返；
3. `RenderSystem::UpdateViewport`（Apple 分支）—— 视口矩形**变化时**打一条
   `AppleViewport[i]: rect = ...`（game / scene 都记），顺带把 `AppleRender` 心跳扩成
   打印两个视口的 vp + sc —— 0x0 的视口一格像素都画不出来且**不报任何错**，
   必须出现在心跳里才能验收。

**验收**：启动后心跳里应该看到 `game_vp` 和 `game_sc` **完全一致**且跟随编辑器面板
（拖动/分屏后值会变），`scene_vp=0x0` 是正常的（没有发布者）：

```text
AppleViewport[0]: rect = (389, 104) 1524x915
AppleRender: frame=1921 ... game_vp=(389,104) 1524x915 game_sc=(389,104) 1524x915 scene_vp=(0,0) 0x0 scene_sc=(0,0) 0x0
```

### 3.14 粒子：门闩清在哪，比清没清更重要（顺带查清"为什么不冒粒子"）

§3.12 修死锁时把粒子三个门闩字段的 `Reset` 加在了 `ProcessSwapData` 的**末尾** —— 而那个
函数开头有一个 `if (!m_RenderScene || !m_RenderResource || m_Rhi == nullptr) return;`。只要那个
早退被触发一次（关卡切换的瞬间就够），门闩就再也没人清 ⇒ 又是同一个死锁。

**这次的改动**：

1. 粒子 / 相机 / 场景视口的门闩清理**全部上移到早退之前**；
2. 粒子不再手写三个 `Reset`，改成走 `RenderPipeline::ConsumeParticleSwapData()` —— 和非 Apple
   那条是同一段代码，将来 Metal 上真有了 `ParticlePass` 就自动接上。

**顺带查清了"Apple 上粒子不冒"的根因，它比"Apple 没接"更普遍**：

- `ParticlePass.cpp` 经传递 include 拉到 `vulkan/vulkan.h`，所以 `ZENGINE_APPLE_EXCLUDED_PASSES`
  （`Runtime/CMakeLists.txt`）把它整个排除在 Apple 构建之外；
- 于是 `DesktopRenderPipelineModule::ConsumeParticleSwapData` 里那段消费代码被
  `#if defined(Z_HAS_VULKAN)` 门住，**Metal 和 DX12 都落到"只清"分支**；
- 也就是说 **DX12 同样不冒粒子**，这不是 Apple 独有的洞。

试过把 `ParticlePass` 从排除名单里放出来，编译直接
`fatal error: 'vulkan/vulkan.h' file not found`（经 `VulkanRHI.h` →
`VulkanBindlessTextureManager.h` 传进来）⇒ 排除名单是对的。要让粒子真跑起来，第一步是把
那个 TU 从后端头里解耦出来，之后才能谈在 Metal 上创建 pass（它还挂在 `MainCameraPass`
上，而 Apple 用的是 `MainCameraRp1Pass`，没有粒子挂载点）。

> **后续：这条已在 §3.24 补完** —— 解耦、放出名单、挂到 RP1、每帧推进、编辑态 tick
> 五步一起做完，真机心跳 `particle=(has=1 emit=2 draw=2 parts=300000 vp=1)`。

**验证**：冒烟测试新增 5 条契约盯"门闩必须在早退之前"（176 → 181 checks）。变异：把那批
清理挪回早退之后 ⇒ 3 条红。（契约自己先踩了一个坑：搜早退时只匹配条件会先命中**注释里**
引用的同一句，必须带 `if (`。）

### 3.15 关卡清理在 Apple 上是空桩：历史空桩在功能打通之后变成洞

`RenderSystem::ClearForLevelReloading()` 在 Apple 分支里是 `{}`，非 Apple 分支里才是真实现
（清 `RenderScene` + 丢掉两个 swap buffer 里还压着的 `GameObjectDesc`）。

这是最难发现的一类洞：**它当初是对的**。Apple 上以前根本没有渲染实体（`ProcessSwapData`
曾经也是空体），清不清都没差别；等场景管线打通之后，它就成了：

- `m_RenderEntities` / `m_MeshObjectIdMap` 从不清 ⇒ 旧关卡实体残留，重载后和新实体并存；
- `m_InstanceIdAllocator` 从不清 ⇒ 切一次关卡涨一截，永不回收；
- 交换数据里还压着的 `GameObjectDesc` 不清 ⇒ 它们持有 Play World 对象的引用，而调用方
  （`WorldManager::EndPlayWorld`）清完**立刻**就 `UnloadLevelByUrl()` —— 之后 `ProcessSwapData`
  拿到的就是已销毁对象。`WorldManager.cpp` 里那段注释写的正是这个（原先记的是 Windows 上
  的 `0xc0000005`）。

**改动**：删掉 Apple 空桩，实现搬到文件末尾的平台无关段（和 §3.12 的
`ResubmitActiveLevelRenderers` 同一处），并补一条 `LOG_INFO` —— 这一条本来完全无声，清掉的
实体数是唯一能证伪它的量。

**真机验证**（编辑器启动会读 `saved/config/last_scene.txt` 恢复上次场景，这条路径必然经过它）：

```
19:52:20.291 [ZLevel]  loading level: asset/level/1-1.scene
19:52:21.546 [ZRender] ClearForLevelReloading: dropped 31 render entities and reset the GameObject swap data
19:52:21.546 [ZLevel]  unload level: asset/level/1-1.scene
19:52:21.546 [ZLevel]  loading level: level/1-2.scene
19:52:21.571 [ZEditor] Restored last scene: level/1-2.scene
19:53:03.275 [ZRender] AppleRender: frame=1921 entities=30 visible=2 ... blit_ready=1 rp2_tonemapped=1
```

清掉 31 个（和心跳里的 `entities=31` 对得上），重载后重建回 30 个并继续出画面 —— 不是"清完
就没了"。重载那一下会让物理的形状重建错误再打一遍（`[ZPhysics] Unsupported Shape`，360 条
里一半来自启动、一半来自重载），那是既有问题，与本次改动无关。

**验证**：冒烟测试新增 6 条契约（181 → 187 checks）。三次变异各自精确变红：整体回退成空桩
⇒ 5 条红；只漏掉 logic buffer 的清理 ⇒ 1 条红；去掉 `m_RenderScene` 空指针保护 ⇒ 1 条红。
（契约自己也踩了坑：函数体不能按固定长度截，加注释就会把窗口撑爆 —— 改成截到 `\n}`。）

### 3.16 相机：Apple 版 Initialize 用硬编码相机，完全不看 `m_CameraConfig`

探针的初衷是查另一个假设（`SyncGameCameraFromMainCamera` 在 Apple 上是空桩），**结果否掉了它**：
心跳的 `main_cam_valid=0` 说明这个工程的关卡里根本没有主相机组件
（`level/1-1.scene` 是 YAML，全文没有 `camera` 字样），所以那份同步在两个平台上都是空转。

但探针顺带挖出一个真问题：Apple 版 `RenderSystem::Initialize` 里两台相机是硬编码的
`LookAt((0,-10,5), ZERO, UNIT_Z)` + `SetAspect(16/9)` + `0.1/1000`，从不读
`GlobalRenderingRes::m_CameraConfig`。和 §3.15 同一类（"当初只为画 UI，硬编码没有代价"），
场景管线打通之后成了洞：

- pose 对不上：配置里是 `(-5,0,3) → (-4,0,3)`，硬编码是 `(0,-10,5) → 原点`；
- **aspect 对不上**（最要紧）：配置 `1280x768 = 1.667`，硬编码 `16:9 = 1.778`，而编辑器
  的 game 视口面板就是 `1524x915 = 1.666` ⇒ 画面被横向拉伸，而且**不报任何错**。

**改动**：`GlobalRenderingRes` 提到建相机**之前**读一次（同一份指针顺带给 IBL / LUT 用，
所以只 load 一次），pose / znear / zfar / aspect 全从 `m_CameraConfig` 来，硬编码降级成
"读不到配置时的兜底"，并打一条日志说明用的是哪一份。心跳新增
`game_cam=(x,y,z) fov=.. main_cam_valid=.. main_cam=(..) cam_sync=..`。

**真机**（`cam_sync=-1` 表示没有主相机组件可比较）：

```
修复前  game_cam=(0.0000000,-10.0000000,5.0000000) fov=89.0   （aspect 16:9 = 1.778）
修复后  RenderSystem(Apple): cameras from GlobalRenderingRes::m_CameraConfig
        -- pos=(-5.0000000,0.0000000,3.0000000) znear=0.1 zfar=1000.0 aspect=1.6666666
        AppleRender: ... entities=31 visible=3 blit_ready=1 rp2_tonemapped=1 game_vp=(389,104) 1524x915
```

**验证**：冒烟测试新增 18 条契约（187 → 205 checks）。三次变异各自精确变红：pose 改回硬编码
⇒ 1 条；aspect 改回硬编码 ⇒ 1 条；把 `loadAsset<GlobalRenderingRes>` 挪到 `push_back` 之后
⇒ 1 条。（第一次的顺序变异没打红 —— 我只挪到 `m_Cameras.clear()` 之后，而"首次 push_back 之前"
这条约束本来就满足，是变异本身不是缺陷。）

**踩坑**：心跳的格式串和实参顺序必须逐位对齐，我把相机字段插进格式串中间、实参却排在后面，
第一次打出来是 `game_cam=(389,104,1524) fov=915`（scissor 的值跑到了相机字段上）。

### 3.17 光照：Apple 上环境光和方向光从来没被设置过

`m_AmbientLight` / `m_DirectionalLight` **只有非 Apple 那条 `Initialize` 设过**（用
`GlobalRenderingRes` 里的值），Apple 这一条从来没设 ⇒ `RenderScene` 里一直是默认构造的
`(0,0,0)`（`Vector3` 的成员是 `float x {0.f}`，`normalise()` 对零向量原样返回，所以不会
NaN，只是**静默地等于没有光**）。

而 `RenderResourcePerFrame` 正是拿这两个填每帧 UBO 的（`scene_directional_light.color` /
`ambient_light`）⇒ Apple 上主光方向=颜色=(0,0,0)、环境光=0，画面只剩 IBL 在照。
和 §3.15 / §3.16 完全同一类：精简版 Initialize 当初只为把 UI 画出来。

**改动**：Apple 版 Initialize 在 `RenderScene` 建好之后、建 `RenderPipeline` 之前把两样
都设上（用的就是 §3.16 已经提前 load 的那份 `GlobalRenderingRes`）；心跳新增
`dir_light` / `dir_color` / `ambient`。

**真机**：

```
修复前  dir_light=(0,0,0)             dir_color=(0,0,0)  ambient=(0,0,0)
修复后  dir_light=(0.577,0.577,0.577) dir_color=(1,1,1)  ambient=(0.03,0.03,0.03)
        （rendering.global.json 里是方向 (0.5,-0.5,0.5) / 颜色 (1,1,1) / 环境光 (0.03,0.03,0.03)）
```

观测到的方向 y 是 **+0.577** 而配置里写 `y: -0.5`：那是 JSON 反序列化里的坐标转换
（两个平台同一份代码），与本次改动无关 —— 我这条用的表达式和非 Apple 那条**逐字相同**，
所以 Apple 现在与 Windows 一致。

**验证**：冒烟测试新增 13 条契约（205 → 218 checks）。三次变异：漏环境光 ⇒ 2 条红；漏方向光
颜色 ⇒ 1 条红。
契约自己也踩了坑：只搜 `m_RenderScene->m_AmbientLight` 会被**心跳探针里那句**
`ambient_light = m_RenderScene->m_AmbientLight.m_Irradiance` 蒙过去（第一次变异只红了 1 条），
必须搜带 ` =` 的赋值形式。

### 3.18 点光源：Apple 的 Tick 从来不同步 `m_PointLightList`（造关卡证伪）

上一节的审计已经记下"Apple 的 `Tick` 从不调 `SyncPointLightsFromLevel`"，但一直**无法
证伪**：这个工程的资产里根本没有任何 `LightComponent`，真机上 `plights=0` 既可能是
"没同步"也可能是"没灯"。所以先造一个带灯的关卡：

`asset/level/light_probe.scene` —— `1-1.scene` 的副本，末尾追加两个 GameObject
（`ProbeLight_A` 在 (0,0,2)，flux 100 / radius 8；`ProbeLight_B` 在 (6,4,2)，
flux 250 / radius 12）。生成脚本见 `.workbuddy/memory/2026-10-22.md`。

**踩到的两个坑**（都不是渲染问题，但足以让探针失效）：

1. **序列化键名不是成员名**。`LightComponent` 的 YAML 键是 `flux` / `radius`，不是
   `m_Flux` / `m_Radius` —— ZHeaderTool 剥 `m_` 前缀小写化（见
   `build/Generated/ZHT/ZRuntime/Src/LightComponent.gen.cpp`）。写错键名不报错，
   只会静默落回默认值，探针就证伪不了任何东西。
2. **关卡 URL 有两套解析规则，必须同时满足**。`EditorSceneManager` 的
   `LevelUrlToAbsolutePath` 用 `GetProjectContent()` 拼 + `replace_extension(".scene")`；
   `Level::load` 的 `ResolveLevelLoadPath` 用 `GetProjectContent()` 拼、不做扩展名替换。
   `asset/level/x.scene` ⇒ 前者算出 `asset/asset/level/x.scene`（不存在，直接跳过）；
   `level/x` ⇒ 后者算出 `level/x`（无扩展名，被当成不支持的格式）。只有
   **`level/light_probe.scene`** 两条规则都指向同一个文件。

**真机**（`saved/config/last_scene.txt` 写 `level/light_probe.scene`）：

```
修复前  plights=0  gos=34  lcmp=2   ← 有灯、没同步，洞实锤
修复后  plights=2  gos=34  lcmp=2
```

单看 `plights` 区分不了"没灯"和"没同步"，所以心跳同时打 `gos`（关卡 GameObject 总数）
和 `lcmp`（带 enabled `LightComponent` 的数量）。

**改动**：Apple 版 `Tick` 在 `SetVisibleNodesReference()` 之后、
`UpdatePerFrameBuffer` 之前调 `SyncPointLightsFromLevel(active_level)`，和非 Apple
那两条路（`TickRenderThread` / `RenderUpdateSceneCmd::Execute`）对齐。顺序很关键：
把点光源写进每帧 UBO 的是 `UpdatePerFrameBuffer`。

**验证**：冒烟测试新增 13 条契约（218 → 231 checks）。三次变异各自精确变红：删掉
同步调用 ⇒ 2 条红；把同步挪到 `UpdatePerFrameBuffer` 之后 ⇒ 1 条红（顺序）；把探针
关卡的 `flux:` 改成 `m_Flux:` ⇒ 1 条红（键名）。
契约自己踩的坑跟 §3.17 同型：只搜 `UpdatePerFrameBuffer` 会被**注释里那句**
"必须排在 UpdatePerFrameBuffer 之前"抢先匹配到，必须搜调用形式
`UpdatePerFrameBuffer(m_RenderScene, camera)`。

### 3.19 跟上三槽环：可见节点列表改成写/发布分离后 Apple 侧要换 API

（跟上 `6f33299` "可见节点列表改三槽环"）`RenderScene` 的那组 `m_*PerViewport` 数组
进了 `VisibleNodeSet` 的私有存储，对外只剩 getter；同时 `SetVisibleNodesReference()`
被拆成 `BeginVisibleNodeUpdate()`（每帧轮转一次，在任何 `UpdateVisibleObjects` 之前）
+ `PublishVisibleNodes()`（**所有**相机更新完之后才发布）。

Apple 侧跟着改：
- 心跳的 `m_MainCameraVisibleMeshNodesPerViewport[0]` →
  `GetMainCameraVisibleMeshNodes(ViewportType::game)`（不改就编译不过）。
- Tick 里原来在循环**前**调一次 `SetVisibleNodesReference()`，改成循环前
  `BeginVisibleNodeUpdate()`、循环后 `PublishVisibleNodes()`。Apple 是同线程跑，
  本来没有那个"RHI 线程还在读旧帧"的竞态，但顺序必须跟非 Apple 一致 —— 否则这里
  永远写槽 0，环退化成单槽，将来 Apple 一旦拆线程就会立刻踩到空材质。

顺带的观测：改完之后心跳的 `visible` 从 **2 变成 15**（entities=30，相机在 (-5,0,3)）。
这是因为旧代码"每个相机都调一次 `SetVisibleNodesReference`、只有最后一个相机的结果
能留下"，Apple 侧同样受影响；三槽环 + 正确的发布时机把它一并修好了。

### 3.20 后端能力不许"猜是不是 Vulkan"：Nanite 的 GPU 剔除在 Metal 上一直静默退化

**症状**：`NaniteGPUCullingSmokeTest` 在 Apple 上红 **21 条**。真机日志每次启动一条
`nanite: GPU cluster culling needs SPIR-V shaders; unavailable on this RHI (graphics api 2)`
（`2` 就是 Metal），修之前一次会话里出现 6 次。

**根因**：`NaniteRenderer::EnsureComputePipeline()` 开头判的是
"只要不是 Vulkan 后端就 `return false`"。这条判据是给 **DX12** 写的：DX12 会把 SPIR-V
blob 包成一个"成功"的 shader module，但建 compute pipeline 时才失败 ⇒ 不挡的话会每帧
重试、每帧刷一条日志。问题是它顺手把 **Metal 也挡掉了** —— `MetalRHI` 有 SPIRV-Cross，
`CreateShaderModule` 把 SPIR-V 转成 MSL、`CreateComputePipelines` 也是完整实现
（`newComputePipelineStateWithFunction`），完全能吃这份 kernel。于是 Nanite 的 GPU 剔除
在 Apple 上一直静默退化到 CPU 路径。

这是"历史空桩在功能打通之后变成洞"的第五例（前四例 §3.15 / §3.16 / §3.17 / §3.18），
但形态不同：它不是"没人调"，而是"**用错误的代理量猜能力**"。

**改动**：不猜后端，让创建结果说话。
- 删掉那条判据（换成一段说明它为什么错的注释）。
- shader module 建不出来 ⇒ 锁 `m_GPU.gpu_pipelines_unsupported = true`（原来这条路径
  **没锁**，只有 pipeline 创建失败那条锁了）。能不能从这个后端建出 shader module 是
  确定的（比如 WebGL2 根本没有 compute），每帧重试只会刷屏。
- 成功路径补一条一次性 `LOG_INFO`（`compute_ready` 早退保证只打一次）：GPU 路径以前
  **只有失败日志**，"Metal 上到底建没建出 compute 管线"在真机上只能证伪、不能证实。

**验证**：`NaniteGPUCullingSmokeTest` 21 条红 → **0**；新增 10 条契约（85 → 95 checks）。
五次变异各自精确变红：

| 变异 | 红几条 |
|---|---|
| 把后端判据塞回去 | 22（21 旧 + 1 条"不许猜后端"） |
| 删 shader 失败路径的锁存 | 1 |
| 删 pipeline 失败路径的锁存 | 1 |
| 删成功日志 | 2 |
| 删 `compute_ready` 早退 | 1 |

**契约自己踩的坑（两处假绿，都是变异测试抓出来的）**：
1. 用 `rfind("gpu_pipelines_unsupported = true", 日志位置)` 找锁存 —— `rfind` 是往**前**
   找的，第二条失败路径会一路找到第一条的 latch 上，于是"删掉第二条"照样绿。改成
   "先定位最近的一个 `if (`，再在 `[if, 日志)` 这段里找"。
2. `x < std::string::npos` **恒为真**，所以"日志存在"和"早退在它前面"两条判据各自都得
   先判 `found`，否则日志被删掉时第二条照样绿。

**遗留观察 → 已证伪并在 §3.21 修掉**：真机 Editor 里 Nanite 一行日志都没有，不是"场景里
没有 Nanite mesh"，而是**帧循环从来没有推进过 Nanite** —— §3.21。修完真机心跳是
`nanite=(has=1 inst=31 batches=14 vis=1)`。

### 3.21 Nanite 的每帧推进：四条 Tick 路径里三条都没有（Apple 是死代码）

**症状**：阶段 57 修完 Vulkan 判据之后，真机 Editor 里 Nanite **仍然一行日志都没有**
（只有 6 行 `r.Nanite.*` 的 console 变量注册）。

**根因**：不是"场景里没有 Nanite mesh"，而是**帧循环从来没有推进过 Nanite**。它有四条
Tick 路径，而"推进 Nanite"这句话只在**其中一条**里：

| 线程模型 | 后端 | 路径 | 修之前 |
|---|---|---|---|
| 并行 | DX12 | `BuildRenderSystemFrameCommands` DX12 分支 | ✅ 有 `RenderNaniteCmd` |
| 并行 | 非 DX12（Vulkan） | `BuildRenderSystemFrameCommands` 通用分支 | ❌ 没有 |
| 单线程 | DX12 | `TickRenderThread` DX12 分支 | ❌ 没有 |
| 单线程 | 非 DX12 | `TickRenderThread` 通用分支 ← **Apple/Metal** | ❌ 没有 |

于是集群构建 / 逐簇裁剪 / 批次组装在 Apple 上一次都没跑过，而且**不报任何错**。
这是"历史空桩在功能打通之后变成洞"的第六例，形态是**漏抄**：Nanite 接进来时只往
DX12 那条路上写了一份，后来加别的 Tick 路径时没人想起它。

**改动**：抽出 `RenderSystem::UpdateNaniteForFrame(delta_time)` 作为唯一入口，四条路径
统一调它（可见性更新之后、`PreparePassData` 之前 —— Nanite 读场景实例，所以必须在可见性
更新之后；产出的批次要赶在上传 GPU 资源之前）。`RenderNaniteCmd::Execute` 也改成调它，
不再自己抄一份判空。

> ⚠️ 这个函数的定义必须住在 RenderSystem.cpp **平台无关的公共尾巴**上。第一次我把它放在
> `TickRHIThread` 旁边（那是非 Apple 大分支里），结果 Apple 链接时
> `undefined symbol: RenderSystem::UpdateNaniteForFrame(float)` —— 静态库看不出来，只有链
> 出 `ZEditor` 时才炸。这和 `AllocateFrameDrawListSlot` 当初踩的是同一个坑。

顺带给 `RenderPipelineBase` 加了 `GetNanitePass()`（心跳探针要读实例数），Apple 心跳多打
一组 `nanite=(has={} inst={} batches={} vis={})`：`has` 区分"有没有 Nanite pass"，`inst`
区分"认没认出实例"，`vis` 区分"认出来了但全被裁掉"。单看任何一个都分不开。

**验证**：真机心跳从"Nanite 一行都没有"变成 `nanite=(has=1 inst=31 batches=14 vis=1)`
（entities=31，14 个渲染批次，有可见集群）。`AppleSceneBlitSmokeTest` 231 → **242 checks**
（新增 11 条契约）。四次变异各自精确变红：

| 变异 | 红几条 |
|---|---|
| 删 Apple Tick 里的调用 | 3（存在 + 两条顺序） |
| 删 `BuildRenderSystemFrameCommands` 通用分支的入队 | 1 |
| 把 Apple 的调用挪到 `PublishVisibleNodes()` 之前 | 1（顺序） |
| 删心跳的 `nanite=` 探针 | 1 |

顺带修了一条被自己改坏的旧契约：它硬编码签名 `"void RenderSystem::Tick(float)"`，而我为了
拿 `delta_time` 把形参补成了 `Tick(float delta_time)` ⇒ 整段契约假红。改成只匹配到
`(float` 为止 —— 形参有没有名字不影响语义，钉死右括号会让一次无关改名把整段打成红。

### 3.22 GPU 那一半也已落地（并纠正一次自己的误判）

§3.21 结尾我写过"GPU 那一半仍然没落地，两层都没通"。**那条结论是错的**，已在下面纠正 ——
把错误结论连同它怎么来的都留着，因为"我是怎么误判的"本身就是这一节的价值。

**真实情况**：阶段 58 一修完帧循环，GPU 路径当场就通了。真机日志（2026-10-23 09:42:16）：

```
nanite: cluster-cull compute pipeline ready -- GPU cluster culling is live
nanite: dispatched cluster culling (clusters=871 groups=14 owned_command_buffer=0)
```

`owned_command_buffer=0` 是关键：它用的是**帧命令缓冲**，不是 `BeginSingleTimeCommands`
那条"自己开、自己提交并等待"的兜底路 —— 也就是说真机帧循环里 Nanite 的 GPU 剔除
真的在跑，而不是只在冒烟测试的 headless 环境里跑。心跳则是
`nanite=(has=1 inst=31 batches=14 vis=1 ngpu=1)`。

**误判是怎么来的（两条，都值得记住）**：
1. **bash `grep -i "a\|b"` 在这台机器上是假阴性**（本文件 §构建/验证 已经记过一次，这次又踩）：
   我 grep 昨天的日志找 `cluster-cull`，什么都没匹配到，就当它不存在。
2. **BqLog 按天 rotate**：那次真机跑在 21:38（10-22），而我在第二天 09:37 去查 ——
   新日志已经切到 `engine_20261023_1.log`，我 grep 的仍是 `engine_20261022_2.log`。
   两个错误叠加 ⇒ "明明有，却查出来没有"。

**顺带加的三件东西**（都是为了让下一次退化不再静默）：
- `NaniteRenderer::IsGPUComputeReady()` —— 心跳的 `ngpu=`。Nanite 的 GPU 路径是**静默**
  退化到 CPU 的：`inst` / `batches` 照常产出，画面看不出差别，只有这一个数能分辨这一帧的
  集群裁决发生在 compute kernel 里还是 CPU 上。
- `PrepareGPUData()` 五个出口各一条一次性诊断（`m_GpuDiagLogged`），外加
  `r.Nanite.GPU=0` 那一条 —— 那个出口最安静，因为开关关掉时 `PrepareGPUData` 根本不会被调用。
- `DispatchClusterCulling()` 的一条一次性确认。"管线建成了"不等于"真的发了 dispatch"，
  这是"历史空桩"最爱藏的地方，所以两边都要有日志。

**关于 `CullInstancesGPU()` 那个 TODO**：它不是 bug。`NaniteCullingSystem::CullInstancesGPU()`
的函数体确实只有 `return CullInstancesCPU(...)`，但它的契约是"返回**实例级**结果给 CPU 组装
`m_RenderBatches`"—— 逐集群的裁决本来就在 kernel 里（`DispatchClusterCulling`），而 GPU 的
结果按设计留在 GPU 上不读回。真正该改的是那条没写清的 TODO 注释。

### 3.23 编辑器 gizmo（axis）：两半一起补，顺带把 RHI 的 dynamic offset 补上

**症状**：Apple 编辑器里选中任何物体都**没有坐标轴**，而且不报任何错、不崩、画面照常。
这是本文件 §4 里挂了很久的一条"已知限制"，本次两半一起补掉。

**根因（两半 + 一条地基，缺一不可）**：

| # | 缺的那一半 | 位置 | 后果 |
|---|---|---|---|
| a | **上传**：`RenderSystem::CreateAxis` 在 Apple 分支下是 `{}` | `RenderSystem.cpp` | 不注册 mesh source、不上传 ⇒ 就算画的地方有，也没 mesh 可画 |
| b | **数据通路**：`SetVisibleAxis` 只翻 pipeline 开关、不写 swap data | `RenderSystem.cpp` | `m_VisibleAxisUpdatePending` 恒为 false ⇒ `m_RenderScene->m_RenderAxis` 永远没值 |
| c | **绘制**：`p_axis_node` 只有 `DX12MainCameraPass` 和 `MainCameraPass`（Vulkan，Apple 构建里排除）会画 | `MainCameraRp1Pass.cpp` | Apple 的 RP1 根本没有 axis 绘制路径 |
| d | **每帧状态**：Apple 的 `BuildDrawLists` 不把 `m_IsShowAxis` / `m_SelectedAxis` 交给 pass（DX12 分支每次都交） | `DesktopRenderPipelineModule.cpp` | 就算 c 补了，也因为恒为 false 而什么都不画 |
| e | **地基**：`MetalRHI::CmdBindDescriptorSetsPFN` 把调用方给的 `pDynamicOffsets` 整个丢掉 | `MetalRHI.mm` | 所有 `*_DYNAMIC` 绑定都读第一帧的数据；axis 的 per-frame storage buffer 直接命中 |

(e) 是这一节真正的收获：Metal 没有 dynamic descriptor，`setVertexBuffer:offset:` 的 offset
必须在**绑定时**就算好。RHI 里那两行 `(void)dynamicOffsetCount; (void)pDynamicOffsets;`
让"Vulkan 语义的动态偏移"在 Metal 上静默变成"永远偏移 0"—— 不崩、不报错，只是画错。

**修复**：

- (a)(b) 从 Apple 分支搬进 `RenderSystem.cpp` 的**平台无关尾巴**（和 `UpdateNaniteForFrame`
  同一个理由：静态库看不出缺符号，只有链 `ZEditor` 时才炸）。`CreateAxis` 成功**也**打一条日志
  —— 以前失败才有日志，而"根本没被调用"和"调了但都失败"在日志里长得一模一样（都是一片安静）。
- (c) RP1 新增 axis 的 descriptor layout / pipeline / descriptor set / `DrawAxis()`，挂在
  forward-lighting pass 的末尾（它是最后开的那个 pass，深度测试关掉 ⇒ gizmo 永远盖在场景之上）。
  管线建失败**不抛**：gizmo 只是编辑器的辅助显示，不该把整个渲染管线拖崩。
- (d) Apple 的 `BuildDrawLists` lambda 补上每帧两行状态传递。
- (e) `CmdBindDescriptorSetsPFN` 按 Vulkan 语义取用动态偏移：set 顺序 → set 内 binding 号
  **升序**，只数 dynamic 类型的 binding（`dynamic_cursor`），偏移量加到 `bind_offset` 上。

**验证**：真机（2026-10-23）：

```
MainCameraRp1Pass: axis pipeline ready (editor gizmo can be drawn)
CreateAxis: 3/3 axis meshes uploaded (asset ids 1 / 2 / 3)
DrawAxis: editor gizmo drawn (indices=432 selected=3 viewport=2560x1440)
```

冒烟测试新增 28 条契约（248 → 276 checks）。四次变异各自精确变红：

| 变异 | 红几条 |
|---|---|
| 删 forward pass 里的 `DrawAxis(viewport_type);` | 1 |
| 删 Apple draw list 的两行状态传递 | 1 |
| `bind_offset += 0`（保留 `dynamic_cursor` 和类型判断，只废掉生效那一步） | 1 |
| 改 `CreateAxis` 成功日志的措辞 | 1 |

**三条教训**（都是踩出来的，不是推出来的）：

1. **契约不能只钉"机制存在"**：`dynamic_cursor` 和 `STORAGE_BUFFER_DYNAMIC` 都在的时候，
   只删真正 `bind_offset += pDynamicOffsets[...]` 那一行照样全绿 ⇒ 必须额外盯**生效那一步**。
2. **文本契约会被自己的注释蒙过去（第四次）**：我在 `MetalRHI.mm` 的注释里逐字写了
   `(void)pDynamicOffsets`，于是"不许再出现这句"的判据被注释喂饱、假红。纪律：搜**调用形式**，
   绝不在注释里逐字引用被禁的字面量。
3. **`DrawAxis` 的 `!m_IsShowAxis` 出口刻意不打日志**：没选中物体时它是 99% 的常态，
   打一条 warning 每个会话都会污染日志；而一旦真出问题，其余五个出口的一次性日志
   （管线没建 / 没有 axis 节点 / RenderSystem 没有 / 从来没人 submit / scene 视口 0x0 / mesh 没上传）
   会把原因说清楚。

**顺带查清的一件事（不是 bug）**：`DrawAxis` 里的 `scene` 视口是 0x0 是**设计行为**。
`QueryNativeDockPanel` 的 `out_is_active = (leaf->Tabs[leaf->ActiveTab].PanelId == title)`，
而 Scene 与 Game 是**同一个 tab 组的两个 tab**，只有当前激活的那个才有 rect。会话状态在
`bin/Debug/saved/config/native_dock.json` 的 `active` 字段（0 = Scene 激活）。
为查这件事在 `PlayModeView` 里留了一个**长期保留**的视口发布探针（只在值变化时打）：

```
PlayModeView probe: constructed 'Scene' viewport_id=1
PlayModeView probe: published 'Scene' viewport_id=1 rect=(389,156) 1524x863
```

### 3.24 粒子：`ParticlePass` 解耦之后还有两层洞（每帧推进 + 编辑态 tick）

**症状**：Metal 上场景里一个粒子都不冒，且不崩、不报错、心跳什么都不说。

**根因（三层，一层套一层）**：

| # | 缺的那一层 | 位置 | 后果 |
|---|---|---|---|
| a | **编译期**：`ParticlePass.cpp` 经传递 include 拉进某个后端自己的平台头 | `ParticlePass.cpp` | 被 `ZENGINE_APPLE_EXCLUDED_PASSES` 整 TU 排除 ⇒ Metal 上**根本没有这个 pass** |
| b | **消费期**：`ConsumeParticleSwapData` 的消费代码被一个后端宏门住 | `DesktopRenderPipelineModule.cpp` | Metal / DX12 都落进"只清门闩"分支，粒子数据每帧送到然后原地丢掉 |
| c | **每帧推进**：两步推进只写在 `SubmitDrawLists` 里 | `DesktopRenderPipelineModule.cpp` / `RenderSystem.cpp` | **Apple 那条精简 Tick 根本不走 `SubmitDrawLists`** —— 它直接 `draw_list.ExecuteAll()` ⇒ `Simulate()` 一次都没跑 |
| d | **编辑态 tick**：`ParticleComponent` 不在 `g_editorTickComponentTypes` 白名单里 | `EditorApplication.cpp` | `ParticleComponent::Tick()` 是 tick 请求的**唯一**生产者 ⇒ 编辑器里（非 Play）`Simulate()` 遍历的是空表 |

(a)(b) 是 §3.14 就记下的老账，本次把它们补完。(c) 是这一节真正的新洞，(d) 是顺带挖出来的。

**修复**：

- (a) 删掉那两个后端头，9 处后端 typedef 换成 RHI 的 `RHIDeviceSize` /
  `RHISpecializationMapEntry` / `RHISpecializationInfo` / `RHIPipelineStageFlags` / `RHI_TRUE`；
  顺带删掉注释里那段直接调后端 API 建 pipeline cache 的死代码（本 TU 唯一一处绕开
  RHI 抽象的地方）。`RHISpecializationInfo::pMapEntries` 是 `const**`，得先有一个 const
  指针变量再取它的地址。
- (b) 判据从"是不是某个后端"改成"这一套管线有没有粒子 pass"。
- (c) 抽成 `RenderPipelineBase::PrepareParticlesForDraw()` / `AdvanceParticlesAfterDraw()`
  一对**普通方法**（不是虚函数，基类就认得 `m_ParticlePass`），两条 Tick 路径各调各的。
  Vulkan 那支原本硬写在 `SubmitDrawLists` 里的两行也换成同一对，免得将来再抄漏。
- (d) `EditorApplication::Initialize` 加 `registerEdtorTickComponent("ParticleComponent")`。
  Unity 的粒子在 Scene 视图里本来就不用按 Play 才动，所以这是对齐而不是加戏。

**验证**：真机（2026-10-23），心跳：

```
particle=(has=1 emit=2 draw=2 parts=300000 vp=1)
```

`parts` 是**画出去的 instanceCount**，等于 `Simulate()` 回读的 `alive_count_after_sim`。
修 (c) 之前它是 `parts=0`（建管线、接深度法线、送 swap 数据全都做了，唯独 simulate 没跑）。
`frame=1201` 无 Metal 错误、无崩溃报告。

冒烟测试新增 43 条契约（276 → 319 checks）。五次变异各自精确变红：

| 变异 | 红几条 |
|---|---|
| 删 Apple Tick 里的 `AdvanceParticlesAfterDraw()` | 1 |
| 把 `ParticlePass` 放回 `ZENGINE_APPLE_EXCLUDED_PASSES` | 1 |
| 注释掉 `registerEdtorTickComponent("ParticleComponent")` | 1 |
| 注释掉 RP1 里 `m_ParticlePass->SetupParticlePass()` | 1 |
| 注释掉消费侧的 `SetTickIndices(...)` | 1 |
| 把后端头 include 回 `ParticlePass.cpp` | 1 |

**三条教训**：

1. **"每帧推进"挂在哪个帧钩子上，比它写没写更重要**。我把两步推进照抄进了
   `DesktopRenderPipelineModule::SubmitDrawLists` 的 Apple 分支，而 Apple 的
   `RenderSystem::Tick` 从未调用它 —— 抄对了地方也照样是死代码。判定办法只有一个：
   探针。`ParticlePass::Simulate()` 顶上那行计数探针一次都没打出来，就是铁证。
2. **文本契约会被自己的注释蒙过去（第五次）**：变异把
   `registerEdtorTickComponent("ParticleComponent")` **注释掉**，全文 `find` 依然命中
   那个串 ⇒ 319 条全绿。修法：钉"调用点"的断言一律走 `code_line_find()`（逐行扫、
   跳过整行 `//`），不走全文 `Contains`。另外正文注释里**不许**逐字引用被禁的字面量
   （`ParticlePass.cpp` / `DesktopRenderPipelineModule.cpp` 的注释都为此改写过）。
3. **两个同名函数极易漏一个**：`MainCameraRp1Pass::SetupParticlePass()` 和
   `ParticlePass::SetupParticlePass()` 干的是两件事（接线 vs 映射 uniform buffer）。
   只调前者时 `Draw()` 会往两个 nullptr 里 memcpy —— SIGSEGV
   （`KERN_INVALID_ADDRESS at 0x0`，栈在 `_platform_memmove`）。这个崩溃实打实踩过一次，
   所以现在两个都在同一个函数里、外面只调一次，并且 `Draw()` 自己也守了 nullptr
   + 一次性 ERROR 日志。

**顺带查清的一件事（是真 bug，但平台无关，本次没修）**：同一个 `ParticleComponent`
（`this` 指针相同）会被 `PostLoadResource` 调**两次** —— 先是 prefab 模板对象
`particle.object`（原点），再是场景实例 `Emitter`（32,11,3）。于是场景里 1 个发射器
会开出 2 份 GPU 资源（`emit=2`），模板那一份永远没人 tick、永远 0 个粒子，但每帧照样
被 Draw 遍历。修它要动 prefab 模板的加载路径，影响面比 Apple 大，先记在这里。
**→ 已在 §3.25 修掉**（`emit=2` → `emit=1`）。

### 3.25 prefab 定义树不该跑 post-load：一个发射器开出两份 GPU 资源

**症状**：§3.24 修完之后心跳是 `particle=(has=1 emit=2 draw=2 parts=300000 vp=1)` ——
场景里**只有 1 个** `Emitter`，`emit` 却是 2。粒子数照常 300000，画面看不出任何毛病，
不崩不报错。

**根因**：`GameObject::ExpandDefinition()` 要的只是 prefab 的**组件对象**，好把它们并进
本对象（下面的 merge）。它调 `PrefabUtility::InstantiateFromPath()` 把整棵 definition 树
实例化出来，用完就丢 —— 那棵树不在任何 Level 里、永不 tick、也不会有人析构。但
`PrefabPostLoadDriver` 在 DFS 里对**每个** GameObject 的**每个** Component 都调了一次
`PostLoadResource`，于是模板对象上的 `ParticleComponent` 先按模板自己的变换（原点）
初始化一遍，在 `ParticlePass` 里占掉一个 emitter id、开出一份 GPU 资源；然后
`GameObject::load()` 再按场景实例的正确变换（32,11,3）初始化一遍 —— **同一个组件
（`this` 相同）被 post-load 了两次**。

模板那一份：永远没人 tick（`ParticleComponent::Tick()` 是 tick 请求的生产者，而模板
对象不在任何 Level 里，压根不会被 tick 到）⇒ 永远 0 个粒子；但每帧 `Draw()` 照样遍历
它 ⇒ 白白占一个 emitter slot + 一份 GPU buffer。场景里每多放一个引用该 prefab 的对象，
就多泄漏一份。

**修复**：给 `PrefabUtility::Instantiate` / `InstantiateFromPath` 加
`invoke_post_load`（**默认 `true` = 老行为**，只有 definition 那一支显式 `false`）：

```
PrefabUtility.h     Instantiate(asset, invoke_post_load = true)
                    InstantiateFromPath(path, invoke_post_load = true)
PrefabUtility.cpp   PrefabPostLoadDriver::m_InvokePostLoad 守卫唯一那个调用点
GameObject.cpp      ExpandDefinition() → InstantiateFromPath(prefab_path, false)
```

关掉的**只是** step 2a 那一次 `PostLoadResource`；DFS、嵌套 prefab 展开、Transform 的
owner 解析全都照跑。合并后的组件由 `GameObject::load()` 用**正确的父对象**再 post-load
一遍 —— 那才是真正该跑的那一次。

**验证**：真机（2026-10-23），心跳：

```
particle=(has=1 emit=1 draw=1 parts=300000 vp=1)     ← emit/draw 2 → 1
```

`emit` 和 `draw` 都从 2 降到 1，`parts` 仍是 300000 ⇒ 泄漏的那一份没了，**真正那一份
没受影响**（粒子数一点没少，这就是"没误伤"的判据）。`frame=1321` 无崩溃报告。
`ExpandDefinition` 没被改坏：`Ground` / `Fence` / `NavVolume` 的 `expanded definition`
日志照常出现。

冒烟测试新增 13 条契约（319 → 332 checks）。四次变异各自精确变红 1 条：

| 变异 | 红几条 |
|---|---|
| `ExpandDefinition` 改回默认（不传 `false`） | 1 |
| 守卫改成 `if (true)` | 1 |
| 场景放置路径也关掉 post-load | 1 |
| 二进制路径不把 flag 转发给 `Instantiate` | 1 |

**两条教训**：

1. **契约不能只钉"机制存在"（第二次）**。第一版契约钉了"flag 存在 + 守卫存在 + 调用点
   唯一"，变异"二进制路径不转发 flag"时照样全绿 —— 三个机制一个没少，只是 flag 没传到。
   补了两条"flag 要**真的传下去**"（YAML / 二进制两个转发点）才变红。判据要落在
   **生效的那一步**上。
2. **越界的 `substr` 会带崩整个测试**。变异把调用点整行注释掉后 `find` 返回 npos，
   `substr(npos, 80)` 抛 `out_of_range` ⇒ 测试进程 abort，看起来像"变红了"其实是被崩掉
   的。取子串前必须先判 npos，且取到**行尾**再搜。

### 3.26 分配器配对：`Produce`（new）出来的对象被喂给了 mi_free

**症状**：不崩在出错的地方。要么在 `mi_usable_size → mi_checked_ptr_segment` 里
EXC_BAD_ACCESS / SIGBUS（栈里只有 mimalloc，看不出是谁配错的），要么**根本不崩** ——
`mi_free` 拿到一块系统 malloc 的内存，破坏的是 mimalloc 自己的延迟释放链表，崩在
几秒之后、完全不相干的栈里。Windows / Linux 上一次都没复现过。

**根因**：引擎有两条分配路径，配对错了在非 Apple 上**完全等价**，所以从来没人发现：

| 分配 | 底层 | 必须配的释放 |
|---|---|---|
| `ObjectManager::Produce()` | `ProduceHelper<T,false>::Produce()` = `new T()` | `ObjectManager::Destroy()`（= `delete`） |
| `MemoryManager::CreateObject<T>()` | `mi_malloc` + placement new | `MemoryManager::DestroyObject()` |
| `ObjectManager::NewObject<T>()` | 内部走 `CreateObject` | `MemoryManager::DestroyObject()` |

第三行最容易搞混：名字像 `Produce`，底层却是 mimalloc。

而 `MemoryManager.cpp` 里是 `#if !defined(__APPLE__) #include <mimalloc-new-delete.h>` ——
**Apple 上全局 `operator new/delete` 故意不接管 mimalloc**（否则 dylib 与 host 进程会
交叉释放 `std::string` 的 buffer）。于是只有 Apple 上"new 出来的块"和"mimalloc 的块"
是两种完全不同的内存，喂错了就坏。

**审计结果（6 处真错配，全库扫描 + 人工确认）**：

| # | 位置 | 触发路径 |
|---|---|---|
| 1 | `Level.cpp:358` | `Level::Save` —— 编辑器存关卡 |
| 2 | `AssetManager.cpp:932` | YAML 资产读失败分支 |
| 3 | `AssetManager.cpp:956→1020` | 二进制资产读失败分支 |
| 4 | `PrefabUtility.cpp:599` | `SaveAsPrefabAsset` |
| 5 | `ZSlateInspectorWindow.cpp:2816` | **跨函数**：`AssetManager::ReadObject` 的结果（Inspector 里打开 DataTable） |
| 6 | `AnimatorSmokeTest.cpp:367` | 测试 |

第 5 条是脚本抓不到的：`ReadObject` 底层就是 `Produce`，返回的对象被跨函数
`DestroyObject` 掉了。反过来看，`LuaMemZasset.cpp` 和 `ContentBrowserAssetActions.cpp:484`
之前在记忆里标着"来源未确认"，这次逐条确认下来是**正确**的（都是 `CreateObject` 配
`DestroyObject`）。编辑器各 importer 里的 `object_manager->Destroy(produced)` 也全是对的 ——
那才是既定写法，错的是上面这 6 处。

**修复**：6 处全部改成 `ObjectManager::Destroy`，并各写清"为什么不能用另一个"。

**外加一道运行时护栏**（这才是这次真正值钱的部分）：`MemoryManager::DestroyObject()`
先用 `mi_is_in_heap_region()` 判一次"这块内存是不是 mimalloc 管的"，不是就
`LOG_ERROR` + **跳过 `mi_free`**（宁可泄漏这一块，也不要崩在一个看不出根因的地方）。
`MemoryManager::GetForeignFreeCount()` 记次数，冒烟测试读它。

**验证**：

- **静态**：`MemoryPairingSmokeTest` 23 checks。其中一条是实锤 ——
  `IsMimallocOwned(Produce(...)) == false`（Apple），直接证明两条路径在这台机器上
  确实不是同一种内存。非 Apple 上这条是 `true`，所以那里检测不出来，这也是这类
  bug 只有在 Apple 上才暴露的原因。
- **变异**（各精确变红）：`PrefabUtility` 改回错配 → 3 条；`Level::Save` 改回 → 3 条；
  Inspector 改回 → 3 条；守卫改成 `if (false)` → 1 条。
- **真机**：ZEditor 跑 25 秒，`allocator mismatch` **0 次**、无崩溃报告、粒子心跳
  `particle=(has=1 emit=1 draw=1 parts=300000 vp=1)` 不变 ⇒ 守卫没有误报。
- **没能自动验收**：`Level::Save` / `SaveAsPrefabAsset` / Inspector 打开 DataTable 这
  三条路径要 UI 操作才走得到（macOS 无障碍权限被拒，`osascript` 报 `-10004`），只能
  手动点。这次跑的 25 秒里编辑器没写关卡文件，所以第 1 条路径实际上没被触发到。

**三条教训**：

1. **"0 处错配"这个结果本身必须能证伪**。第一版扫描器因为没跳过 `=` 左边的尾随空格，
   变量名永远取成空串 ⇒ 整张来源表是空的 ⇒ 恒绿；变异"改回错配"只红了钉点那 2 条，
   全库扫描那条纹丝不动 —— 当场暴露。现在扫描器自带**双向自检**（已知错配样本必须
   检出 2 条、已知正确样本必须 0 条）。
2. **`mi_free` 喂错指针不保证当场崩**。变异 4 把守卫关掉后，测试**跑完了**才报 1 条
   失败，没崩。也就是说这类错配的后果是"有时立刻 SIGBUS，有时静默堆破坏、崩在别处"。
   后者更糟，也更能解释为什么它一直没被定位。
3. **跨函数的分配/释放最容易看漏**。`AssetManager::ReadObject` 返回的是 `Produce` 出来
   的对象，调用方毫无察觉就用了 mimalloc 释放。审计必须沿"返回值是谁分配的"追，
   不能只看同一个函数里的字面量。

### 3.27 macOS 上没有 Launcher：双击 ZEditor 就是"闪一下就没了"

**症状**：双击 `ZEditor`，什么都没出现，进程静默退出 1。从终端跑能看到一行
`ZEditor: no project specified.` —— 那行字就是全部线索。

**根因（两处，缺一不可）**：

1. `Main.cpp` 的 Unix 分支里 `case StartupMode::OpenLauncher:` 直接
   `return reportMissingProject();`，而 `reportMissingProject()` 是**往 stderr 打一行字
   再 return 1**。双击（Finder/Launchpad）启动的进程没有终端，那行字谁也看不见 ⇒
   用户看到的就是"崩溃"。
2. `ZEditorLauncher.cpp` **整个文件**包在 `#ifdef _WIN32` 里（1 行 include、594 行
   `#endif`），macOS 上这两个符号根本不存在。就算把 1 接上也是链接错误。

**第二个坑（调查时才发现的）**：Win32 版 Launcher 的 UI 用 `ZSlate::FSlateStandaloneApp`，
而 `engine/Libraries/ZSlate/include/ZSlate/App/SlateStandaloneApp.h` **整个头**是
`#if defined(_WIN32)`（Win32 + D3D11 宿主），ZSlate 的 CMake 在非 Windows 上把
`App/SlateStandaloneApp` 整个从源文件列表里滤掉 —— **macOS 上没有任何可复用的独立
Slate 窗口宿主**。所以不能"照搬 Win32 那套换个平台宏"，得手搭一个 AppKit 窗口
（`ZEditorLauncher.mm`）。

**修复**：

| 文件 | 改动 |
|---|---|
| `Launcher/ZEditorLauncher.mm`（新） | AppKit 版 Launcher：套件下拉 + 最近项目表格（`项目`/`路径` 两列）+ 打开/浏览（`NSOpenPanel`）/新建项目 + 退出。最近列表落 `~/Library/Application Support/ZEngine/ZEditor/recent_projects.txt`，默认项目目录 `~/Documents/ZEngine`。最近列表为空时**补上可执行文件同目录的 `.zproject`**（构建树里 `ZEditor` 和 `ZGameSmoke.zproject` 就是挨着的，第一次启动也能一键进编辑器） |
| `Launcher/ZEditorLauncher.cpp` | 末尾补 `#if !defined(_WIN32) && !defined(__APPLE__)` 兜底桩 —— 这两个符号在 Linux 上必须存在，否则 ZEditor 链接不过 |
| `Main.cpp` | Unix 分支的 `OpenLauncher` 接到 `runLauncherAndDispatchUnix()`；新增 `relaunchWithProject()`；`runEditorUnix()` 补 `RememberProject()`（命令行 `-p` 打开的项目下次也要出现在 Launcher 里） |

**为什么必须重启自己**：`CommandSystem::Initialize()` 是从 `_NSGetArgv()` 里解析 `-p`
的，进程内改 argv 既不干净也不安全（那块内存是 exec 时内核铺好的定长区域）。Win32
用 `ShellExecuteW` 干的是同一件事；macOS 用 **`posix_spawn` + `POSIX_SPAWN_SETEXEC`** ——
**不能 fork**：Launcher 已经把 Cocoa / `NSApplication` 拉起来了，fork 之后的子进程带着
一份半初始化的 AppKit（Mach port、CF run loop 状态全是脏的），是 macOS 上经典的
"fork after Cocoa" 崩溃源。SETEXEC 不走 fork，直接换映像，而且 PID 不变、Dock 里不会
出现两个图标。

**顺带修的**：裸可执行文件默认 `NSApplicationActivationPolicyProhibited` —— 窗口能显示
但拿不到键盘焦点、不进 Dock、没有菜单栏（于是文本框里 Cmd-V 也没用）。Launcher 里显式
设成 `Regular`，并装一个最小主菜单（退出 + 编辑），否则新建项目的名称输入框没法粘贴。

**验证**：

- **构建**：`.mm` 被 CMake 按 `OBJCXX` 编译并链进 ZEditor（`build.ninja` 里可见
  `OBJCXX_COMPILER__ZEditor`）。Editor/CMakeLists.txt 本来就有 `*.mm` 的 glob 和 Cocoa
  framework，不需要改构建配置。
- **真机（关键）**：无障碍权限被拒（`osascript` 报 `-10004`），点不了按钮，所以临时
  插了一行 `performSelector:afterDelay:` 自动触发"打开"按钮，走的是**同一条提交链** ——
  结果：`recent_projects.txt` 被正确写出，`posix_spawn` 重启成功，重启后的编辑器日志里
  `Project path: /Users/.../bin/Debug/ZGameSmoke.zproject`（**绝对路径**，跟手动 `-p
  ZGameSmoke.zproject` 那几次的相对路径不同 —— 这就是重启真的发生了的硬证据），心跳
  一路到 `frame=2761`、`particle=(has=1 emit=1 draw=1 parts=300000 vp=1)`，与直接 `-p`
  启动完全一致。探针已撤回。
- **静态**：新增 `ZEditorLauncherWiringSmokeTest`（29 checks，默认 OFF，
  `-DZENGINE_BUILD_LAUNCHER_WIRING_SMOKE_TEST=ON`）。它只能读源码当文本 —— 启动分支在
  `START_SYSTEM()` **之前**，任何测试进程都跑不到它。
- **变异**（各精确变红）：Unix 分支改回 `reportMissingProject()` → 3 条；
  `POSIX_SPAWN_SETEXEC` 改成 `0` → 1 条；`runEditorUnix` 里删掉 `RememberProject` → 1 条；
  把 `return runLauncherAndDispatchUnix();` **整行注释掉** → 2 条（这条专门验证扫描器
  会跳过整行注释 —— 全文 `find` 是抓不到的）。

**一条教训**：macOS 上"没有终端"是 GUI 进程和 CLI 进程最本质的区别。任何"打一行日志
然后退出"的失败路径，在双击启动的场景下都等价于崩溃。`reportMissingProject()` 作为
兜底（Launcher 也起不来时）可以留，但它绝不能是**唯一**出口。

### 3.28 ZEditor 没有应用图标（以及做成 .app 之后双击崩溃）

**症状**：`zeditor没有应用图标` —— Finder 里是通用可执行图标，Dock / Launchpad 里也没有。
Windows 那边是有的：图标由 `z_embed_app_icon()`（`engine/cmake/AppIcon.cmake`）把
`resource/ZEditor.ico` 嵌进 exe。

**根因 1 —— macOS 侧根本不存在这个机制**：`z_embed_app_icon()` 里只有 Windows 分支
（写 `.rc`），没有对等物。macOS 不给裸 Mach-O 任何图标：`[NSApp setApplicationIconImage:]`
只能管住"进程活着的时候"，访达 / Launchpad 里看到的仍然是通用图标（§3.27 那次已经给
Launcher 补了运行时图标，但那只是半边）。

**根因 2 —— 做成 bundle 之后暴露的洞（真正的阻塞点）**：

`CommandSystem::Initialize()` 用 **argv[0] 的父目录**去拼 `ZEditor.ini`：

```cpp
config_file_path = executable_path.parent_path() / "ZEditor.ini";
```

bundle 里 `Contents/MacOS/ZEditor` 只是指向真身 `bin/<Config>/ZEditor` 的**相对 symlink**，
而 `ZEditor.ini` / `asset` / `resource` 全在真身旁边。于是配置去 `Contents/MacOS` 底下找，
必然落空：

```
cd / && .../ZEditor.app/Contents/MacOS/ZEditor -p <project>
[Bq]base dir type 1: /
libc++abi: terminating due to uncaught exception of type std::runtime_error:
    Failed to initialize system 'ConfigManager', rolling back
EXITCODE=134
```

双击时还多一层：CWD 是 `/`，连"相对路径碰巧能命中"都没有。`cd bin/Debug && ./ZEditor`
一直正常，正是因为它把 CWD 恰好放在了 ini 旁边 —— **这个洞被工作目录掩盖了很多次**。
Linux 分支读 `/proc/self/exe`（内核给的就是解链后的真身），所以只有 macOS 中招。

**修复**：

| 文件 | 改动 |
|---|---|
| `Runtime/Function/Command/CommandSystem.cpp` | Apple 分支取 `executable_path` 时先过 `DereferenceExecutablePath()`（`std::filesystem::canonical` + `error_code`，失败退回原路径 —— 另一份重载会抛异常，启动期抛了就是崩） |
| `Runtime/Resource/Config/ConfigManager.cpp` | ini 打不开时先 `ZEngineForceFlushLogs()` 再 `return false`（见下） |
| `Editor/Launcher/ZEditorLauncher.mm` | `InstallDockIcon()` 开头加"让位分支"：从 bundle 启动时（`mainBundle.bundleIdentifier` + `CFBundleIconFile` 都在）直接 `return`，让 AppKit 用 icns，只留一条日志当证据 |
| `Editor/cmake/MakeMacOSAppBundle.cmake`（新） | `sips` 生成 10 档 iconset → `iconutil -c icns`；`Contents/MacOS/ZEditor -> ../../../ZEditor` **相对** symlink；写 `Info.plist`（`CFBundleIconFile` 只在 icns 真生成了才写 —— 指向不存在的文件系统不报错，只是静默退回通用图标，那正是最想避免的） |
| `Editor/CMakeLists.txt` | `if(APPLE)` 追加 POST_BUILD 调上面的脚本 |

**为什么真身不动（bundle 只是壳）**：① `cd bin/Debug && ./ZEditor` 是本仓库所有真机验证的
入口，不能断；② 资产不进 bundle —— ZEditor 只链系统框架（`otool -L` 里没有
`@executable_path` / `@rpath`），所以经 symlink 启动时 dyld 不会因为 `@executable_path`
指向 bundle 内部而找不到东西；③ 相对 symlink 让整个 `bin/` 被拷走 / 换机器都不失效。

**顺带修的**：`ConfigManager::Initialize()` 返回 false 之后引擎是**抛异常终止**的，而 BqLog
是异步的 —— 不刷那一下，唯一说清楚"到底去哪儿找过 ini"的那行日志会随缓冲区一起消失，
双击时又没有终端可看，于是现场只剩一句 `Failed to initialize system 'ConfigManager'`。

**验证**：

- **真机**：`cd / && .../ZEditor.app/Contents/MacOS/ZEditor -p <project>` → 进程存活，
  心跳 `AppleRender: frame=961 entities=30 visible=15 blit_ready=1 rp2_tonemapped=1`；
  日志 `Configuration file: /Users/.../bin/Debug/ZEditor.ini`（解链后的真身，不是
  `Contents/MacOS`）。
- **真机（图标两条路径）**：从 bundle 起（无 `-p`）→
  `Launcher: dock icon comes from the app bundle 'com.zengine.ZEditor' (Info.plist CFBundleIconFile = ZEditor)`；
  从裸可执行文件起 → 退回
  `Launcher: dock icon installed from '.../bin/Debug/resource/ZEngineEditorBigIcon.png'`。
  两条都实测到过，说明"让位"和"兜底"都真的在跑。
- **静态**：`ZEditorLauncherWiringSmokeTest` 29 → **48** checks。
- **变异**（各精确变红）：去掉解链调用 → 3 条；删 `ZEngineForceFlushLogs()` → 1 条；
  symlink 改绝对路径 → 1 条；让位分支挪到 PNG 覆盖之后 → 1 条；让位分支漏 `return` → 1 条。

**两条教训**：

1. **"历史空桩 / 历史假设"在功能打通之后变成洞 —— 第十二例**：`CommandSystem` 当年写
   "ini 就在可执行文件旁边"时，可执行文件确实只有一个位置。bundle 一出现，同一个
   可执行文件就有了两个路径（symlink 和真身），假设失效。凡是"从可执行文件位置推
   资源位置"的代码，都得先解链。
2. **CWD 是最容易掩盖缺陷的环境变量**：`cd bin/Debug && ./ZEditor` 之所以一直正常，
   只是因为 CWD 碰巧等于 exe 所在目录。验证启动路径时，**故意从 `/` 跑一遍**。

### 3.29 双击 .zproject 直接打开项目（文件关联）

**症状**：`.zproject` 在访达里没有图标也没有默认打开方式，双击没反应。Windows 那边是
有的：`ZEditorLauncher.cpp` 的 `RegisterFileAssociation()` 把 open 命令写成
`"<exe>" -p "%1"`，路径从 argv 进来，什么都不用改就能用。

**根因 —— macOS 根本没有"argv 传路径"这条路**：

LaunchServices 启动 App 时 **argv 里只有 `-psn_0_xxxxx`**（那个参数会被
`CommandParser::FindOrAddOption` 自动注册成 Flag 后忽略，不会让进程退出 —— 但也不会
带来任何信息）。文件路径是**之后**以 `kAEOpenDocuments`（odoc）Apple Event 送过来的，
`NSApplication` 再转给 delegate 的 `-application:openFile:`。

调研确认：全库检索 `NSApplicationDelegate` / `NSAppleEventManager` / `kAEOpenDocuments` /
`openFile:` / `setDelegate` —— **零命中**。也就是说光写 Info.plist 是不够的，代码里必须
有人接，否则那个事件被静默丢掉：不报错、不弹窗，症状就是"双击没反应"。

**修复**：

| 文件 | 改动 |
|---|---|
| `Editor/cmake/MakeMacOSAppBundle.cmake` | `Info.plist` 加 `CFBundleDocumentTypes`（`CFBundleTypeRole: Editor`、`LSHandlerRank: Owner`）+ `UTExportedTypeDeclarations`（UTI `com.zengine.zproject`，`UTTypeConformsTo: public.data`，`public.filename-extension: zproject`）；构建后跑 `lsregister -f -R`（找不到 / 失败只 WARNING） |
| `Editor/Launcher/ZEditorLauncher.mm` | `ZLauncherController` 加 `NSApplicationDelegate` 并实现 `-application:openFile:`；`runLauncher` 里 `setDelegate:self`（**必须排在 finishLaunching 之前**）；新增 `IsZProjectFile()` |
| `Editor/Main/Main.cpp` | 新增 `PickRelaunchExecutable()`：重启优先走 bundle 入口；`posix_spawn` 之前 `LogSystem::ForceFlush()` |

**三个不那么显然的决定**：

1. **`UTTypeConformsTo` 选 `public.data` 而不是 `public.text`**：`.zproject` 是 block YAML
   （旧项目是 JSON），**没有 magic 字节**（`ProjectInfo::LoadFromFile` 只判 `exists()` 就丢给
   解析器），只能靠扩展名 / 声明认。conform 到 text 的话 TextEdit 一类的通用编辑器也会来
   抢默认打开权，我们要的是 ZEditor 独占（`Owner`）。
2. **先 finishLaunching + 手动泵 0.5 秒再决定要不要显示 Launcher 窗口**：LaunchServices 要
   等 App "就绪"才发 odoc，而"就绪"就是 `finishLaunching`。所以先 `finishLaunching` 再泵一小
   段，odoc 到了就直接提交、窗口一次都不显示 —— 否则用户看到的是"双击了 zproject，然后闪
   一个选项目的窗口，再进编辑器"。代价：普通启动（双击 ZEditor.app）时窗口晚 0.5 秒出现。
3. **重启要走 bundle 入口**：`_NSGetExecutablePath()` 给的是 symlink 解链后的**真身**
   （`bin/<Config>/ZEditor`）。直接 exec 真身的话，重启后的进程就不在 bundle 里了 —— Dock
   图标从 icns 退回 PNG 兜底，LaunchServices 也不再把它认成同一个 App（下次双击 .zproject 会
   另起一个实例）。`PickRelaunchExecutable()` 在真身旁边找 `.app`，确认它的
   `Contents/MacOS/<name>` 确实指向自己，就用那个路径 exec。

**顺带修的**：`posix_spawn` + `SETEXEC` 成功后进程映像立刻被换掉，BqLog 缓冲区里没落盘的
东西（包括"是用 bundle 入口重启的"这条）会一起消失 —— 而那正是判断重启走没走 bundle 的唯一
证据。exec 之前必须 `ForceFlush()`。

**验证**：

- **真机（关键）**：`open ./bin/Debug/ZGameSmoke.zproject` → 进程命令行
  `ZEditor.app/Contents/MacOS/ZEditor -p /Users/.../bin/Debug/ZGameSmoke.zproject`，
  心跳 `AppleRender: frame=601`。日志三段证据齐全：
  ```
  16:12:34.805  Launcher: opening document '/Users/.../ZGameSmoke.zproject' (from LaunchServices).
  16:12:34.815  Relaunching with project '...' via '/Users/.../ZEditor.app/Contents/MacOS/ZEditor'.
  16:12:49.611  AppleRender: frame=1 entities=30 ...
  ```
  odoc 在启动后 **96ms** 就到了 —— 0.5 秒那个窗口够用，Launcher 窗口确实没出现过。
- **关联真的注册了**：`lsregister -dump` 里能看到
  `type id: com.zengine.zproject / bundle: ZEditor / conforms to: public.data / tags: .zproject`。
- **回归**：双击 `ZEditor.app` 本身（不带文档）仍然停在 Launcher（进程存活、无崩溃、
  没出现那条 odoc 日志）。
- `open -a <app> <非 zproject 文件>` 会被 LaunchServices 自己拒（-10820），
  `IsZProjectFile()` 是给"右键 → 打开方式"这条路的兜底。
- **静态**：`ZEditorLauncherWiringSmokeTest` 48 → **63** checks。
- **变异**（各精确变红）：不挂 delegate → 3 条；openFile 不提交 → 1 条；去掉扩展名守卫 →
  1 条；UTI 只改一处 → 1 条；重启改用真身 → 1 条；exec 前不刷日志 → 1 条。

**一条教训**：跨平台时最容易漏的不是"这段逻辑怎么写"，而是**操作系统把信息递给进程的通道
都不一样**。Windows 是 argv（`%1`），macOS 是 Apple Event。两边都用"文件关联"这四个字，
实现上没有任何一行是共用的 —— 连"参数没传过来"这件事在 macOS 上都是静默的。

### 3.30 编辑器**已经在跑**的时候双击 .zproject

§3.29 那条路靠 Launcher，而 **Launcher 只在没有 `-p` 时跑一次**。编辑器一旦起来，系统再
发 odoc 过来就没人接了 —— 症状同样是"双击没反应"：不报错、不弹窗。

**难点不在"接住"，在"谁来消费"**：平台层能收下路径，但"要不要重启、重启带哪个项目"是
Editor 的决策（只有它知道当前开的是哪个项目）。而新开一条每帧通道正是本项目踩过的坑 ——
装好了没人调，从外面看不出来（Apple 的 `RenderSystem::Tick` 直接 `ExecuteAll()` 那次）。

**所以消费点放在 `MacOSApplication::PollEvents()` 里**：它是 `Application.cpp:141` 主循环
每帧必调的那一步，不需要新增任何通道。

| 文件 | 改动 |
|---|---|
| `MacOSApplication.h/.mm` | 新增 `SetOpenDocumentHandler()` 接入点 + `ZMacOpenDocumentDelegate`（`-application:openFile:` 把路径存进 `g_pending_open_documents`）；`SetupCocoaOnce()` 里 `[NSApp setDelegate:]`；`PollEvents()` 末尾消费 |
| `Main.cpp` | 新增 `installRuntimeOpenDocumentHandler()`：`runEditorUnix()` 在 `START_SYSTEM()` 之后注册，拿到路径后 `relaunchWithProject()` |

**两个决定**：

1. **平台层不做"这是不是项目文件"的判断** —— 它不知道 `.zproject` 长什么样、也不知道当前
   开的是哪个项目。判断放在 Editor 注册的 handler 里。
2. **双击已经在编辑的那个项目就什么都不做**。重启（SETEXEC 换映像）会把没保存的改动一起
   丢掉，而"我双击了当前项目"这件事没有任何需要执行的副作用 —— 只在日志里留一句。

**验证**：

- `open ZGameSmoke.zproject` → 编辑器起来（PID 22752）；再 `open ZSecond.zproject` →
  **PID 不变**，`-p` 变成
  `ZEditor.app/Contents/MacOS/ZEditor -p /Users/.../bin/Debug/ZSecond.zproject`。
  PID 不变正是 SETEXEC 换映像的硬证据 —— 不是另起了一个实例。
- 再双击一次同一个 `ZSecond.zproject` → PID 不变、心跳继续（`frame=3001`），日志
  `Open document: '.../ZSecond.zproject' is already open.`，**没有重启**。
- **静态**：`ZEditorLauncherWiringSmokeTest` 63 → **73** checks。
- **变异**（各精确变红）：不挂 delegate → 1 条；`PollEvents` 不消费 → 1 条；openFile 不存 →
  1 条；不注册 handler → 1 条；相同项目也重启 → 1 条。

**一条教训**：契约要钉**判定式**，不是"用了某个函数"。第一版钉的是
`std::filesystem::weakly_canonical(document`，而把守卫改成 `if (false)` 之后那条照样命中 ——
真正会丢改动的那次改动反而没人管。改成钉 `!opened.empty() && opened == active` 才对。

## 4. 已知限制（不是 bug，但先说清楚）

- ~~**`NaniteGPUCullingSmokeTest` 在 Apple 上是红的（21 条）**~~ —— **已在 §3.20 修掉**：
  `EnsureComputePipeline()` 里那条"不是 Vulkan 就直接放弃"的后端判据删了，改成
  "试着建、建失败就锁"。修完 21 条红 → 0，并补了 10 条契约（85 → 95 checks）。

- **RP2 只有 step 0（显示变换）+ LUT**：FXAA 没做 —— 它要一个只在 `enable_fxaa=true` 时才
  存在的 framebuffer，而 Apple 上这个 flag 目前是 false，写了也是跑不到的死代码。
  UI 合成在 Apple 上不需要 RP2 做（UI 直接画进 swapchain）。
- **LUT 的采样序（tonemap → LUT → gamma）是本工程第一次实现，没有上游可对齐**：§3.10 里
  那条依据是"LUT 内容离恒等比离 gamma 近一个数量级"，不是权威结论。若美术给的 LUT 是
  在显示空间（sRGB）里调的，把 `SampleColorGradingLut` 那一行挪到 `pow(...)` 之后即可，
  其余不变。
- ~~**LUT 走压缩 `.zasset`**~~ —— 已修：`LoadTexture` 加了 `allow_cooked` 参数，LUT 显式跳过
  烤好的 BC7 产物（§3.10 (b)）。
- ~~**IBL 在 Apple 上仍然是兜底贴图**~~ —— 已修，见 §3.11。现在启动时会看到
  `IBL bound (irradiance=real, specular=real, brdf=real)`，那条
  `IBL textures unavailable` 已经不再出现（它顺带从 `[E]` 降级成 `[W]` 并改成了准确措辞，
  因为它从来不是"设备掉了"，只是"还没上传"）。
- ~~**IBL 的热重载只有 DX12 有**~~ —— 已修，见 §3.12：`m_LevelResourceDesc` 现在有生产者
  （`RenderSystem::RequestGlobalResourceUpload()`），两条 `ProcessSwapData()` 上传完都会重绑
  `MainCameraRp1Pass::RefreshMeshGlobalIblDescriptors()`。顺带挖出并修掉了 Apple 上整条 swap
  通道的死锁（任何关卡切换的数据以前都送不到渲染线程）。
- ~~**Metal 的 `UpdateViewport` 不更新 scissor**~~ —— 已修，见 §3.13。修复前裁剪矩形永远停在
  `SyncLayerMetrics` 给的整窗值，任何只靠 scissor 钳制的路径都能把像素画到编辑器其它面板上。
- ~~**`scene`（type 1）视口在 macOS 上没有发布者**~~ —— **说法已纠正**（§3.23）：发布者是
  有的（`PlayModeView::UpdateViewport`），但 Scene 与 Game 是同一个 tab 组的两个 tab，**只有
  当前激活的那个有 rect**。激活的是 Scene 时心跳是 `game_vp=0x0 scene_vp=(389,156) 1524x863`，
  激活的是 Game 时反过来 —— 两个都是预期值，取决于 `saved/config/native_dock.json`。
- ~~**粒子在 Vulkan 之外的所有后端都不冒（不是 Apple 独有的）**~~ —— **已在 §3.24 修掉**：
  `ParticlePass.cpp` 解耦掉了后端头（放出 Apple 名单）、消费侧改成"有没有粒子 pass"判据、
  每帧推进抽成 `RenderPipelineBase` 那一对方法（Apple 的精简 Tick 原本不走 `SubmitDrawLists`）、
  编辑态 tick 白名单补上 `ParticleComponent`。真机心跳现在是
  `particle=(has=1 emit=2 draw=2 parts=300000 vp=1)`。

- **点光源阴影 pass 在 Metal 上不做**：它靠 geometry shader 做 cubemap 展开，Metal 没有
  geometry shader。现在和 DX12 一样是 clear-only，不抛错。
- ~~**Apple 的 Tick 从不调 `SyncPointLightsFromLevel`**~~ —— **已在 §3.18 修掉**：造了
  `asset/level/light_probe.scene`（两个 `LightComponent`）证伪成
  `plights=0 gos=34 lcmp=2`，修完是 `plights=2`。
- ~~**编辑器 gizmo（axis）在 Apple 上两半都缺**~~ —— **已在 §3.23 修掉**：上传（a）、
  swap data（b）、RP1 的绘制路径（c）、每帧状态传递（d）、以及 RHI 的 dynamic offset（e）
  五处一起补。现在启动会看到 `axis pipeline ready` + `CreateAxis: 3/3 axis meshes uploaded`，
  选中物体后会看到 `DrawAxis: editor gizmo drawn (indices=432 ...)`。
  **唯一没能自动验收的一步是"在 Scene 面板里点选物体"**：macOS 的无障碍权限被拒
  （`osascript` 的 `System Events` / `activate` 报 `-10004` / `-1728`），只能靠手动点一下确认。
- **每次构建仍会冲掉已导入的贴图**：`CopyEngineDataToOutput` 是
  `rm -rf <bin>/<cfg>/asset` + 整目录重拷，而 `_Generated/` 就在 content 底下，所以
  一构建派生数据就没了、下次启动重新导入一遍（重启不会，只有构建会）。
  想连这个一起解决就得让拷贝规则跳过 `_Generated/` —— 代价是源图被删后孤儿产物要人工
  清理（和今天 `_Generated/Data/` 一样的行为），所以暂时没做。
- **物理形状建不出来**：`Create JPH Shapes Failed / geometry reference did not resolve`，
  和渲染无关，是关卡里 mesh 引用没解析。
- **帧率**：心跳日志显示 2 秒 120 帧（≈60 fps）。修复 (a) 之前只有 17–30 fps，差额几乎
  全是 1.5 万条告警日志的落盘开销；没有做过真正的性能分析。
