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
| `AppleSceneBlitSmokeTest` | 231 | 离屏场景 → swapchain 的那段**手写** MSL：能编译、铺满全屏、上下没颠倒、tonemap+gamma 的曲线和 `glsl/tone_mapping.frag` 一致；RP2 的 LUT 条带采样；IBL 的 RGBA32F cubemap 真跑一遍（`CreateCubeMap` 的 row pitch）；`m_LevelResourceDesc` 的生产者 + swap 门闩（§3.12）；视口/裁剪矩形链路（§3.13）；门闩必须清在早退之前（§3.14）；关卡清理不是空桩（§3.15）；相机必须来自 `m_CameraConfig`（§3.16）；环境光 + 方向光必须真的设进 `RenderScene`（§3.17）；Apple Tick 必须同步点光源 + 探针关卡本身（§3.18） |
| `MetalSpirvShaderSmokeTest` | 47 | 9 个**内置** SPIR-V blob 能不能过 SPIR-V→MSL：sampler 紧凑编号 ≤ 15、`Z_HAS_SPIRV_SHADERS` 是开的、非 SPIR-V 要被拒 |

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

试过把 `ParticlePass` 从排除名单里放出来，编译直接 `fatal error: 'vulkan/vulkan.h' file not found`
（经 `VulkanRHI.h` → `VulkanBindlessTextureManager.h` 传进来）⇒ 排除名单是对的。要让粒子真跑
起来，第一步是把那个 TU 从 Vulkan 头里解耦出来，之后才能谈在 Metal 上创建 pass（它还挂在
`MainCameraPass` 上，而 Apple 用的是 `MainCameraRp1Pass`，没有粒子挂载点）。

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

## 4. 已知限制（不是 bug，但先说清楚）

- **`NaniteGPUCullingSmokeTest` 在 Apple 上是红的（21 条）**，由 `6f33299` / `7e5d252`
  带进来，与本次改动无关（已验证：把我的改动撤掉、只留编译修复，它照样红）。
  失败起点是 `NaniteRenderer::EnsureComputePipeline()` 里
  `if (m_Rhi->getGraphicsAPI() != GraphicsAPI::Vulkan) return false;` —— 也就是说
  Nanite 的 GPU 剔除在 Metal 上被这个 Vulkan 判断直接挡掉了，尽管 MetalRHI 本身支持
  SPIR-V→MSL（`Z_HAS_SPIRV_SHADERS`）。CPU 路径不受影响。修它需要改判据，单独处理。

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
- **`scene`（type 1）视口在 macOS 上没有发布者**：只有 `PlayModeView` 关窗时清成 0x0，
  主相机走的是 `game`（type 0）视口。哪天有人真的需要 scene 视口（比如场景相机预览），
  得先给 macOS 补发布者，不然它永远是 0x0 —— 心跳里的 `scene_vp=0x0` 是预期值。
- **粒子在 Vulkan 之外的所有后端都不冒（不是 Apple 独有的）**：`ParticlePass.cpp` 经传递
  include 拉 `vulkan/vulkan.h`，被 `ZENGINE_APPLE_EXCLUDED_PASSES` 排除在 Apple 构建之外；
  `ConsumeParticleSwapData` 的消费代码被 `#if defined(Z_HAS_VULKAN)` 门住 ⇒ **Metal 和 DX12
  都走"只清"分支**。详见 §3.14。`ProcessSwapData` 现在走共享的
  `RenderPipeline::ConsumeParticleSwapData()`（门闩照清），将来 Metal 上真有 pass 就自动接上。

- **点光源阴影 pass 在 Metal 上不做**：它靠 geometry shader 做 cubemap 展开，Metal 没有
  geometry shader。现在和 DX12 一样是 clear-only，不抛错。
- ~~**Apple 的 Tick 从不调 `SyncPointLightsFromLevel`**~~ —— **已在 §3.18 修掉**：造了
  `asset/level/light_probe.scene`（两个 `LightComponent`）证伪成
  `plights=0 gos=34 lcmp=2`，修完是 `plights=2`。
- **编辑器 gizmo（axis）在 Apple 上两半都缺**（审计发现，**未修**）：`CreateAxis` 在 Apple
  上是 `{}`（不注册 mesh source、不上传），而 `p_axis_node` 只有 `DX12MainCameraPass` 和
  `MainCameraPass`（Vulkan，Apple 构建里排除）会画 —— `MainCameraRp1Pass` 没有 axis 绘制
  路径。只补 `CreateAxis` 会上传一堆没人画的 mesh，所以要补就得两半一起补。
- **每次构建仍会冲掉已导入的贴图**：`CopyEngineDataToOutput` 是
  `rm -rf <bin>/<cfg>/asset` + 整目录重拷，而 `_Generated/` 就在 content 底下，所以
  一构建派生数据就没了、下次启动重新导入一遍（重启不会，只有构建会）。
  想连这个一起解决就得让拷贝规则跳过 `_Generated/` —— 代价是源图被删后孤儿产物要人工
  清理（和今天 `_Generated/Data/` 一样的行为），所以暂时没做。
- **物理形状建不出来**：`Create JPH Shapes Failed / geometry reference did not resolve`，
  和渲染无关，是关卡里 mesh 引用没解析。
- **帧率**：心跳日志显示 2 秒 120 帧（≈60 fps）。修复 (a) 之前只有 17–30 fps，差额几乎
  全是 1.5 万条告警日志的落盘开销；没有做过真正的性能分析。
