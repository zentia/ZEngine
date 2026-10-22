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
| `AppleSceneBlitSmokeTest` | 132 | 离屏场景 → swapchain 的那段**手写** MSL：能编译、铺满全屏、上下没颠倒、tonemap+gamma 的曲线和 `glsl/tone_mapping.frag` 一致；RP2 的 LUT 条带采样；以及 IBL 的 RGBA32F cubemap 真跑一遍（`CreateCubeMap` 的 row pitch） |
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

## 4. 已知限制（不是 bug，但先说清楚）

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
- **IBL 的热重载只有 DX12 有**：`RenderSwapContext::m_LevelResourceDesc` 全库没有生产者，
  两条 `ProcessSwapData()` 都读它，但没有任何地方给它赋值 ⇒ "关卡热重载刷新全局资源"这条
  路目前在任何平台都不通。DX12 靠 `OnGlobalRenderResourceUploaded()` /
  `MainCameraRp1Pass::RefreshMeshGlobalIblDescriptors()` 走另一条路；Apple 因为把上传排在
  `RenderPipeline::Initialize` 之前，初始化时就绑到了真图，不需要重绑 —— 但**换关卡换 IBL
  依然不会生效**。LUT 的热重载是靠 RP2 每帧重取 view 绕过去的。

- **点光源阴影 pass 在 Metal 上不做**：它靠 geometry shader 做 cubemap 展开，Metal 没有
  geometry shader。现在和 DX12 一样是 clear-only，不抛错。
- **每次构建仍会冲掉已导入的贴图**：`CopyEngineDataToOutput` 是
  `rm -rf <bin>/<cfg>/asset` + 整目录重拷，而 `_Generated/` 就在 content 底下，所以
  一构建派生数据就没了、下次启动重新导入一遍（重启不会，只有构建会）。
  想连这个一起解决就得让拷贝规则跳过 `_Generated/` —— 代价是源图被删后孤儿产物要人工
  清理（和今天 `_Generated/Data/` 一样的行为），所以暂时没做。
- **物理形状建不出来**：`Create JPH Shapes Failed / geometry reference did not resolve`，
  和渲染无关，是关卡里 mesh 引用没解析。
- **帧率**：心跳日志显示 2 秒 120 帧（≈60 fps）。修复 (a) 之前只有 17–30 fps，差额几乎
  全是 1.5 万条告警日志的落盘开销；没有做过真正的性能分析。
