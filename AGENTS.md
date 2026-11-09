# ZEngine — AI Agent Working Notes

This file is read at the start of every AI assistant session. It records
**long-lived project conventions** that are easy to forget across turns and
sessions. Keep it short and decision-oriented; design rationale lives in `doc/`.

If you (the agent) discover a new project-wide convention while working with
the user, append it here under the appropriate section so the next session
inherits the knowledge.

---

## 0. Product identity (decided 2026-11-05)

**ZEngine is no longer a standalone game engine.** It is a *capability provider
that ships as plugins into other software*. Five responsibilities: (1) plugin
for game engines (Unity / Godot / Unreal), (2) plugin for DCC tools (3ds Max /
Blender / Maya / Substance Painter), (3) AI-native plugin (MCP-callable,
inference-backed), (4) frontier tech (3DGS, 2D-to-3D), (5) built-in showcase
effects (ocean waves, unlimited lights).

**Hard architectural consequence: capability slicing, NOT engine embedding.**
Do not `dlopen` the whole `ZRuntime` into a host — that produces two RHI
devices, two main loops, two window systems and two asset managers fighting
for control. The unit of delivery is a *capability*, not the engine.

| Layer | Rule |
|---|---|
| L2 Core | std-only C++, no host headers, no main loop, no window, no RHI device ownership |
| L3 Bridge | one thin shell per host; bindings generated from reflection metadata, not hand-written N times |
| L1 Presentation | Tier A (borrow host device, in-process) / B (separate process + shared texture) / C (data-only write-back) |

**Precedents already in the tree — reuse, do not reinvent:**
- `Runtime/ZEngineApi/ZEngineToolApi.h`: the narrow C ABI seam (`const char*` /
  `int`, nothing with an allocator, exception, `std::string` or `Object*`
  crossing the boundary). `ZRuntime_ToolInit()` starts *only*
  Log/Type/FileSystem/Object/Asset and explicitly NOT
  Application/WindowSystem/RHI/Scripting/ProjectInfo — this IS capability
  slicing, already shipped.
- `Player/PlayerShell.h`: host-driven lifecycle, `native_window` owned by the
  host, engine does not own the main loop.
- `engine/Libraries/GPUSpine` and `GPUParticle`: std-only core + adapter, the
  same code shared with the Unity side ("one submodule = one copy of the code",
  §2.44). `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` is the "spec first, code second"
  template.
- `Runtime/ExportRuntime.h` (`EXPORT_RUNTIME`), `ZHeaderTool` (reflection ->
  `zengine.d.ts`; extend it to emit C# / Python / GDScript / TS bindings from
  the same metadata), `RHI::supports*` ("ask before you record", §2.43).

**Specs (规格先行, read before implementing):**
`doc/plugin/ZHOST_ABI_SPEC.md`, `doc/plugin/EFFECT_PACKAGE_SPEC.md`.
`ZHOST_ABI_SPEC.md §14` is the source of truth for what is actually shipped.
**P0 landed (2026-11-05)**: `Runtime/Host/ZHostABI.h` + `ZHostPlugin.cpp` —
a std-only C ABI skeleton (GUID registry, lifecycle, version negotiation,
capability query, render-event dispatch) compiled into `ZRuntimeShared.dll`.
`ZENGINE_BUILD_HOST_ABI_SMOKE_TEST=ON` builds the A1/A2 test (63 checks) and
a Python ctypes host probe (20 checks); both pass.
`EFFECT_PACKAGE_SPEC.md` is mostly still a draft, but its §8 (additive
overlay contract) now has working code + offline verification (see below).

**A3 / Unity host line (2026-11-05):** `integration/unity/` now holds the UPM
package `com.zengine.host`, the L3 native bridge `ZEngineHost.dll` (built from
`ZHostPlugin.cpp` + `ZUnityBridge.cpp`, **not** linked against `ZRuntime`) and a
dotnet C# P/Invoke probe. `CommandBuffer.IssuePluginEventAndData` takes
`ZRender_GetEventAndDataFunc()` directly: Unity's `UnityRenderingEventAndData`
is the same `void(int, void*)`. `data` is the `ZHandle`. See
`doc/plugin/UNITY_HOST_A3.md`.

A3-render also landed: `ZUnityPluginInterface.h` re-declares the minimum
Unity 2023.1 SDK ABI (**no Unity headers copied**, §1) and locks onto
`IUnityGraphicsD3D12v7` only — v2..v6 have different vtable slots, so a
miss is a clean Tier C downgrade, never a guess. `GetDevice` is captured on
the first render event (render-thread constraint), `CommandRecordingState`
supplies Unity's current command list, and the draw command is injected via
`ZUnityBridge_SetDrawCallback`. `ZTexture_*` handle borrowing landed in the
generic ABI. A WARP D3D12 offline harness (fake `IUnityInterfaces` + real
device) verifies the draw by reading back a pixel: 20 checks / 0 failures.
The Unity-render-thread trigger still needs a real Unity 2023.1 editor (not
installed here); **do not claim it passed**.

A3-overlay landed (`EFFECT_PACKAGE_SPEC.md` §8): the generic ABI gained
`ZEffectDispatchDesc` / `ZEffectBackendVTable` / `ZEffect_Dispatch`. The
std-only core validates live handles, forbids **any input aliasing the
output**, and only forwards the host's command list — it never owns a GPU
resource. The Unity L3 bridge installs `ZHost.EffectBackend`, deep-copies
`effect_id` into a one-shot queue and dispatches from the render event. A
WARP harness (`ZUnityOverlaySmokeTest`, 22 checks / 0 failures) reads back
pixels to prove the effect wrote only to an independent output RT while the
borrowed host colour texture stayed byte-for-byte unchanged. **The real
Unity composite pass and effect-package loading are still unimplemented.**

**A4 / Blender host line (2026-11-05):** `integration/blender/zengine_host/`
is a Python add-on (manifest + `bl_info`) implementing Tier C data write-back.
`zpls.py` (ZPLS read/write + community `.splat` Translator + extension
dispatch), `zply.py` (official 3DGS / coloured-point-cloud `.ply` Translator)
and `proxy_mesh.py` (quaternion billboard / point proxy) are stdlib-only: no
`bpy`, no GPU, no native DLL. `operators.py` writes a mesh + vertex-colour
material via `bpy.data`; `zhost_ctypes.py` calls `ZPluginLoad` for capability
negotiation **only when a DLL exists**, otherwise degrading to `python-only`.
Offline verification: **80** unittest checks / 0 failures
(`integration/blender/verify/`), including "forced DLL missing still imports"
and native Tier C / `startup_audit_bits == 0`. Real import inside Blender is
**not verified** (no `bpy` on this machine). See `doc/plugin/BLENDER_HOST_A4.md`.

**3DGS PLY field conventions (do not "simplify"):** verified against
`D:/blender/source/blender/io/ply/importer/ply_import_gsplat.cc` +
`IO_gsplat.hh`. `opacity` is logit, `scale_*` is log-scale (**ZPLS also stores
log-scale, so the translator copies it verbatim** — do not `exp`/`log` twice),
`rot_0..3` is wxyz while `ZSplatRecord.rotation` is xyzw, `f_rest_*` and ZPLS
`sh_rest` are splat-major × channel-major, and `f_dc_*` converts to display
colour via `0.5 + SH_C0 * f_dc` **without clamping** (clamping breaks
`.ply -> ZPLS -> .ply` round-trip). ZPLS caps SH at degree 3 (0/9/24/45
`f_rest`); Blender's degree 4 (72) is a hard error, never a silent truncation.
`.ply` write-back emits only the official 3DGS dialect; the point-cloud
profile is import-only. See `doc/plugin/BLENDER_HOST_A4.md §8` and
`EFFECT_PACKAGE_SPEC.md §4.2`.

**OSGame 棋盘皮肤生成线 (2026-11-05, target=trunk):** `integration/chessboard/`
是「冻结 core spec + 可变 style pack → 一份共享几何 → 双端消费」，**不要**在 three.js 和 Unity 各写
一套棋盘几何公式。`board-spec.json` 是唯一真源；`zboard.py` 生成 `.zbrd`
(ZBRD v1, float32 二进制)；`web/zbrd.mjs` 与
`unity/ZChessboardGenerator.cs` 只解析字节。three.js 是 Y-up 右手系，
Unity 侧必须统一做 `x→-x` / `normal.x→-normal.x` / `uv.y→1-uv.y` /
反转三角形绕序。格子数值来自 `G:/OSGame/Client_proj/Tools/ProtobufDatabin/
Core/Basic/J局内/W.玩法模板参数表.xlsx` 行 4 与 `CrossRectangleUtil.cs:19-45`：
7×8、2.15×1.8 m、奇数行左移 1.075 m、中心偏移 (-5.912, -6.3)；**不要按 8×8
估算**。**交付物形态（2026-11-05 用户纠正，已查证）：**
`Project/Assets/Scenes/pvp/` 配的是 **`.unity` 场景族** `ART_OS_ACxx_{HD,High,Mid,Low}.unity`
（另有 `_HD_Master` 母版、`_PC_HD*` PC 派生），不是 prefab。
`SceneTools.cs:26 ART_NODE="Artist"` 说明场景序列化的输入是 `.unity` 场景里名为
`Artist` 的根节点（不是 prefab）；`SGameTools/场景序列化/输出美术节点` 才产出
运行时加载的 `SceneExport/Artist/*_Artist.asset` + `*_core.bytessv`
（`TinySceneObjCreateComponent.lua:71/94`）——两者是「编辑器源 vs 打包产物」。
High/Mid/Low 由项目自动降配流程（`ArtTools/4.场景工具/--->HD转高中低配`
`AutomaticBakeTool.cs`，或 `FastAutoOptimization`）从 HD 生成后美术手调。
**本线只产一个高配 HD 场景，配置表（`ResSceneCfg` /
`ResPersonaliseChessboardDataBin`）不动。**
`tools/deploy_to_trunk.py` 默认 dry-run，不自动改 G 盘工程。**Windows 上
`python -m http.server` 会把 `.mjs` 返回成 `text/plain`，ES module 被严格
MIME 检查拒绝**，宣发页必须用 `tools/serve.py`。本机无头 Edge 可验证
DOM/WebGL 像素，但 `Page.captureScreenshot` 抓不到合成层（空白图不代表没渲染）。
**两个在 trunk 实测踩到的坑（2026-11-05）：** ① trunk 用
`OSGameForwardRenderer`（自定义 SRP），`Shader.Find("Universal Render Pipeline/Lit")`
返回 null → 回退内置 `Standard`，棋盘在 Scene 视图全黑。必须克隆
`Assets/CustomResources/OSG_Default.mat`（shader GUID `a3ef84c3...`）再只覆盖
`_AlbedoColor` / `_Color` / `_BaseColor`；不要自己 `new Material(shader)`。
② `Project8ArtField.cs:38/41/60` 的 `fieldRoot` / `directionalLight` /
`sceneBackGround` 类型是 **GameObject**，用 `SerializedObject` 赋值时传
`Transform` 或 `Light` 会被静默拒绝，prefab 落成 `{fileID: 0}`，运行时报
`InitScene artField not ready`。几何一致 ≠ 视觉一致：three.js 的天穹渐变
shader / 星空 Points / 灯光是网页渲染层，Unity 侧要用项目自己的材质与天空盒。
**几何分层修正（2026-11-05，用户实测截图纠正）：** 早期版本把 `GridUtil`
的 7×8 **逻辑落点**当成可见几何，生成 56 个独立 cell 方块，与现网 7 套皮肤
全都不像。现网核心区是**一整块连续台面**（纹样/徽记靠贴图或浅浮雕），
只有周围装饰物与风格化几何不确定。现在 `board-spec.json` 只冻结
`core_contract`（`visual_kind=continuous_slab` / `logical_grid_is_visual=false`）、
台面轮廓、包边、`anchors`、`decoration_zones`、`lods.*.max_decorations`；
`style-pack.json`（`zengine.chessboard.style-pack/v1`）由 AI/人工整包替换，
只能在 `decoration_zones` 槽内放 `decoration_*` / `interactable_*`，且不得含
`grid` / `core_contract` / `anchors` / `decoration_zones`。`zboard.py --style`
加载风格包、`--no-style` 只出 12 个冻结核心部件；当前构建 18 parts
(12 core + 6 variable)。`external_mesh`（混元单体装饰）的引用契约
`external_mesh_contract/v1` 已于 2026-11-06 冻结（见文末「第 2 步」），
三端消费在「第 3 步」打通、`star_pillar` 的混元 3D 接管在「第 4 步」完成；
style item 上出现内联 `external_mesh` 字段仍会被 `validate_style_pack` 拒绝。
Unity `VerifyCoreContract`
会拦住任何 `cell_*` 可见格子或缺少 `core_slab` 的旧 ZBRD。离线门禁：
`verify/test_board.py`、`verify/test_zbrd_js.mjs`（含 `scene.quality==HD` 门禁）；
`deploy_to_trunk.py` 部署并逐字节比对 **6** 个文件（含 style-pack.json 与
`ART_OS_AC90_HD.unity`）。早期 trunk 曾残留旧 56-cell 版本的 mesh/material，
`ReportStaleAssets` 只报告不删除；**2026-11-05 20:40 复核已清理干净**：
`Meshes/` 15 个 / `Materials/` 12 个，stale 0、`cell_*` 0，跑场景序列化前
无需再清理。`Meshes/` 里没有 `sky_High/Mid/Low.asset` 是正常的 —— 本线只
交付 HD，低配由项目自动降配流程派生。
**生成器改为产场景（2026-11-05）：** `board-spec.json` 新增 `scene` 段
（`path=Assets/Scenes/pvp/ART_OS_AC90_HD.unity` / `quality=HD` /
`artist_root=Artist` / `field_node=SceneMesh` / `template=null`），manifest 带
`delivered_lod=HD` 与 `scene_parts`；`ZChessboardGenerator.cs` 里曾实现的
Editor 内生成（现为无菜单的 `GenerateInEditorUnsafe()`，仅参考）只消费
`lod_mask & 1` 的部件，把棋盘几何**直接建进场景**的 `SceneMesh` 下，新建空场景搭
`Artist / SceneMesh / Light_Group / RenderSettingGroup` 并用
`EditorSceneManager.SaveScene` 存成 HD 场景。`template` 可填现成 `_HD.unity`
以继承 RenderSettings/Volume，此时 `Artist` 根会被整体替换，残留引用需人工检查；
默认 `null` 时环境/后处理/天空盒由美术在场景里补。`Project8ArtField` 挂在
`SceneMesh` 上、`fieldRoot` 指向自身、`sceneBackGround` 留空（现网 HD 场景同构）。
**Unity trunk「闪退且无 dump」根因（2026-11-05 实测，连崩 2 次）：**
`Editor.log` 显示 `SaveAsPrefabAsset` → `RefreshInternalV2` →
`Prefab file async load enable true` → 导入 `P_ART_OS_AC90.prefab` 后
`Graphics device is null.` + `Fatal Error!`（从非主线程）→
`Exiting without the bug reporter. Application will terminate with return code 0`。
这是 Unity **主动 exit(0)** 而非 native crash，所以不会写 `.dmp`；OSGame 定制引擎
开了 prefab 异步导入，worker 线程初始化渲染资源时 graphics device 不可用。
同一菜单回调里连续 `DeleteAsset`/`CreateAsset` 还触发十几次 refresh 与
`sortedQueueGuids entry size does not match` 告警。**修复：生成器绝不创建
prefab 资产（不 `SaveAsPrefabAsset`、不 `DeleteAsset`、不 `AssetDatabase.Refresh`），
几何直接进场景；mesh/material 用 `StartAssetEditing/StopAssetEditing` 合并成一次
refresh 并复用已有资产 + `EditorUtility.SetDirty`；加 `step N/5` 分步日志便于定位。**
**但第 3 次仍崩（同日）：** `Editor.log` 停在 `step 3/5`（资产已就绪）到
`step 4/5`（场景已保存）之间，`Files imported locally: 12` 已完成。查定制引擎
源码坐实机制：`Runtime/GfxDevice/GfxDevice.cpp:189-195` 在 `GFX_TLS_DEVICE` 下
`GetGfxDevice()` 发现本线程 `g_ThreadedGfxDevice == NULL` 就
`FatalErrorMsg("Graphics device is null.")`；`Editor/Platform/Windows/
EditorUtility.cpp:1136-1140` 因非主线程取消弹窗 → `Exiting without the bug
reporter ... return code 0`。**即任何 worker 线程碰渲染 API 都会让 Editor
主动 exit(0)，与 C# 异常无关；菜单回调里的 AssetDB 导入 / `NewScene` 都是触发源。**
Editor 本身图形设备正常（Direct3D 11.1 / RTX 5080 / BatchMode=0）。
**最终方案：彻底离线生成。** `integration/chessboard/unity_scene.py` 只做纯文本
I/O，读 trunk 已落地的 15 个 `.asset` / 12 个 `.mat` 的 `.meta` GUID，写出
`build/ART_OS_AC90_HD.unity`（83 个 YAML 文档，引用 `4300000`/`2100000` 主对象，
`Project8ArtField` 用 `m_Script guid=603043be3babbf44188404cd589ac0c7,
fileID=-365591601`），`verify()` 校验 fileID 闭合 / GUID 可解析 / 层级 / 15 个 HD
部件 / 无 `!u!1001` prefab instance。`tools/deploy_to_trunk.py` 部署 **6** 个文件，
把 `.unity` 直接放进 `Assets/Scenes/pvp/`；`ZChessboardGenerator.cs` 的菜单降级为
`ZEngine/Chessboard Diagnostics (read-only)`，Editor 内生成逻辑保留为
`GenerateInEditorUnsafe()` 但无菜单。**用户不要在 Unity 里跑生成菜单；先关 Editor
→ 部署 → 重开工程按常规启动流程导入。**
门禁：`verify/test_board.py` 57 tests、`verify/test_zbrd_js.mjs` 14 checks。
**第 4 次崩溃（同日）：双击打开离线生成的 `ART_OS_AC90_HD.unity` 也闪退。**
`Editor.log` 里 `Opening scene 'Assets/Scenes/pvp/ART_OS_AC90_HD.unity'`
的**下一行**就是 `Graphics device is null.` → exit(0)，全程没有生成菜单、
没有 AssetDB 写入 ⇒ 这已不是 ZEngine 脚本能规避的路径。对比现网
`ART_OS_AC10_HD.unity`：`!u!20 Camera` 有 1 个、`!u!1001 PrefabInstance` 31 个、
1494 个文档；离线场景原本 0 个 Camera。已在 `unity_scene.py` 补
`Main Camera`（挂 `RenderSettingGroup` 下，`!u!20`，86 文档）并加门禁断言。
**对照实验结果（用户 2026-11-05 回复）：现成 AC10 等场景能正常打开，只有
AC90 崩，补 Camera 后重试仍崩。** 第 5 次会话日志（19:51，5069 行）坐实不是
YAML 内容错：`2396: Start importing Assets/Scenes/pvp/ART_OS_AC90_HD.unity ...
-> artifact 'ee29198c...' in 0.005505 seconds` 说明场景资产已被 AssetDB 正常导入；
本次会话**没有导入任何 `ZEngineChessboard` 资产**（0 次提及），27 个 mesh/mat
都有 `.meta`，GUID 缓存与磁盘一致 ⇒ 「新资产未导入」假说排除。崩溃时序：
`5063 BallanceWorkerCount - Create new worker!!` → `5064 Starting new worker id: 0`
（`Project/Logs/AssetImportWorker0.log`）→ `5065 Opening scene` →
`5066 Graphics device is null.` → `5069 exit(0)`。
**引擎缺陷坐实（G:/OSGame/unity-engine-source）：**
`Runtime/GfxDevice/GfxDevice.cpp:135-138` 的 `GFX_TLS_DEVICE` 让
`g_ThreadedGfxDevice` 成为 TLS；`:189-195` 的 `GetGfxDevice()` 见 TLS 为 NULL
即 `FatalErrorMsg`，而 `:213-221` 的 `GetUncheckedGfxDevice()` 本来就有
`g_ThreadedGfxDevice ? ... : *g_MainGfxDevice` 的安全回退。
全源码只有 `threaded/GfxDeviceWorker.cpp:356`、`GfxShaderCompileWorker.cpp:83`、
`GfxDeviceClient.cpp:5303`、`d3d12/:6028`、`vulkan/:5171` 会调
`SetThreadedGfxDevice()` ⇒ **任何其它线程（场景加载/导入 worker）碰
`GetGfxDevice()` 必崩，属误用 `GetGfxDevice()` 的引擎侧 bug**。
项目 `ProjectSettings/EditorSettings.asset:28/57/58` 明确开着
`m_AsyncShaderCompilation: 1` / `m_DesiredImportWorkerCountOverride: 1` /
`m_StandbyImportWorkerCountOverride: 1`，日志 `:186-191` 对应
`SetDesiredWorkerCount 1`，`:2395` 还有 `Prefab file async load enable true`。
⇒ AC90 是工程里唯一新增的 `.unity`，打开它要走「首次加载 + 依赖解析 +
worker 分发」，现网场景资源早已预热，所以只有它触发。
**更正一个错误的规避建议（不要再试）：** 让 `EditorSettings.asset` 的
`m_DesiredImportWorkerCountOverride` / `m_StandbyImportWorkerCountOverride`
改成 0 **不是「0 个 worker」**。引擎
`Editor/Mono/Inspector/EditorSettingsInspector.cs:848-850` 写明 0 = 取消项目覆盖、
改用 per-user 自动值。用户实测改 0 后日志变成 `SetDesiredWorkerCount 4`、
`SetStandbyWorkerCount 2`、`desiredWorkers: 4`，反而启动 **2 个** worker
（`AssetImportWorker0/1.log` 都只有常规启动内容，无 fatal），照样
`Graphics device is null.` ⇒ import worker 数量与崩溃无关。`m_AsyncShaderCompilation`
同理救不了。**这条方向已排除。**

**母版模式已实现（2026-11-05）：** `board-spec.json` 的 `scene.template` 设为
`Assets/Scenes/pvp/ART_OS_AC10_HD.unity`，`build/scene_template.unity` 缓存现网
场景全文（1494 docs / 14 根 / 31 PrefabInstance / 445 MonoBehaviour / 真实
`m_LightingDataAsset` 与 `m_LightingSettings`）。`unity_scene.py` 的
`apply_template()` 保留全部文档，只做：① 原 Artist 改名
`Artist_AC10_Reference` 并 `m_IsActive: 0`（**不删文档** —— 现网有 6 个
PrefabInstance 的 modifications 引用子树内 `254680459` / `351891918`，删了会悬空）；
② `SceneRoots.m_Roots` 追加 ZEngine 新根；③ 末尾追加 15 个 HD part 的文档。
新增文档 fileID 用 `[2000000000, 2000100000)` —— **必须加上界**，现网场景有
19 位 fileID（PrefabInstance / SceneRoots），只用 `>= 2e9` 会把母版文档误判成
新增文档。`verify()` 母版模式下只对新增文档做 fileID 闭合/GUID 检查（母版的
prefab modifications 引用的是 prefab 资产内部 fileID，本就不在场景里声明）。
`--no-template` 可退回 86 docs 独立场景。产物 1537 docs / 15 HD parts / sha256 `95afd1512409d709`；但完整母版模式
随后被降级为**仅诊断用**（`--full-template`），原因见下。

**根因确认（用户 2026-11-05 20:38）：闪退是用户自己改的另一个 GPUParticle
定制引擎分支导致的。** 换用项目自带的 `G:/OSGame/Client_proj/UnityEngine`
（稳定 Editor `2022.3.5f1 (497d8f943752)`）后一切正常，AC90 能
`Loaded scene`（Deserialize 55ms / Integration 571ms）。⇒ 之前所有
「离线 YAML / 母版 / Camera / worker 数量 / Library 缓存」的排查方向都不是
根因；现网 `ART_OS_AC10_HD.unity` 也崩只是因为当时正跑在那个坏 Editor 上
（AC10 sha256 `5E065198...` 与模板缓存逐字节一致，从未被 ZEngine 改动）。
先前观察到的旁证 `Class with persistent type id 0x6B1D4F37 has not been
registered` = `Modules/GPUSpine/GPUSpineRenderer.cpp:18` 的
`kClassIDGPUSpineRenderer`，也指向这个 GPUParticle/GPUSpine 集成分支。
`EditorSettings.asset` 已恢复原值（三个 1）。

**默认模式收窄为「只继承场景级 4 文档」（同日）：** 稳定 Editor 打开完整母版版
AC90 虽能加载，但报 `Problem detected while opening the Scene file` +
9 条 `Prefab instance problem. Missing Prefab Asset`。查证 AC10 的 23 个
`m_SourcePrefab` GUID 里有 **11 个在 trunk 的 818363 个 `.meta` 里不存在**；
完整继承还会把 AC10 旧美术带进 `SceneExportChecker`。崩溃根因既已确认与场景
结构无关，就不需要整份继承。现在 `unity_scene.py` 默认用
`header_from_template()` 只取 `!u!29/104/157/196` 四个文档作 HEADER
（补齐 `m_LightmapEditorSettings` / `m_LightingDataAsset` /
`m_LightingSettings`），棋盘对象全部自己写 ⇒ **86 docs / 0 PrefabInstance /
1 Camera / 1 个 `Artist` 根 / 15 HD parts / sha256 `fd43fa98a122dcc8`**。
`verify()` 非完整母版模式下跳过场景级文档再做 GUID 校验（继承来的
LightingDataAsset GUID 不属于棋盘资产）。门禁 61 tests / 14 checks / 0 lint error。

**下一步（需稳定 Editor 里人工完成）：** ① 打开 AC90 确认 Console 无
`Missing Prefab Asset` / `Problem detected`、15 个部件与 `Project8ArtField`
字段正确、材质不是全黑（trunk 自定义 SRP 须用 `OSG_Default.mat` 的 shader）；
② 美术补 Volume / 天空盒 / 后处理与皮肤材质；③ 跑
`SGameTools/场景序列化/输出美术节点` 产 `SceneExport/Artist/*_Artist.asset`
+ `*_core.bytessv`，再过 `SceneExportChecker`；④ High/Mid/Low 走项目自动降配，
配置表不动；⑤ 当时 `style-pack.json` 仍是 PLACEHOLDER、`external_mesh` 未实现
（2026-11-06「第 2 步」已替换为星海棋亭主题包并冻结契约，见文末）。
（trunk 旧 `cell_*` 残留已复核为 0，无需清理。）
**离线 YAML 的实机渲染效果仍待验证（本机无 Unity）。**

**ZPLS spec fix:** `EFFECT_PACKAGE_SPEC.md §4.2` had three contradictory byte
layouts (header `reserved[2]`=40B but labelled 32, record `padding[1]`=60B
but labelled 64, magic `0x53504C5A` LE-spells `ZLPS`). Now `reserved[1]` /
`padding[2]` / `0x534C505A` (literal `b"ZPLS"`), locked by module-level
asserts in `zpls.py`. Do not "restore" the original constants.

**A6 / generated bindings (2026-11-05):** `ZHeaderTool.py bindings` parses
`engine/Source/Runtime/Host/ZHostABI.h` as the **single source of truth** into
a JSON IR, then emits C#, Python(ctypes), TypeScript, GDScript and MCP tool
schema (7 checked-in generated files). `ZHostABI.cs`, `zhost_ctypes.py` and
`zhost_ctypes_probe.py` are now host-specific glue only — **never hand-edit a
general ABI declaration or a `*.generated.*` / `zhost_abi_generated.py` file**.
The IR computes x64 struct layout itself (176 / 72 / 96 ...), so generated
`Expected*Size` constants are derived, not guessed. CTest gates:
`ZHostAbiBindingsCheck` (byte-identical regeneration) and
`ZHostAbiBindingsUnitTests` (19 checks, parameter counts identical across all
five languages). Drift returns exit 1 (verified with a deliberately stale
file). `ZEffect_GetParamSchema` is still unimplemented, so the effect-parameter
half of A6 remains open. See `doc/plugin/BINDINGS_A6.md`.
ZHeaderTool's pre-existing `test_zht.CodegenTests` has 4 unrelated failures;
A6's CTest gate deliberately uses `-p test_abi_bindings.py` to avoid them —
do not claim those 4 pass.

**Blender source:** `D:/blender` is a full Blender 5.0.4 alpha source tree with
**no `blender.exe` and no `bpy` on this machine**. Use it as a read-only
reference (e.g. `source/blender/makesrna/intern/rna_define.cc` for the
"one metadata set -> many generated bindings" precedent). Real in-Blender
import still requires a built Blender; do not claim it passed.

**§2.1 `.meta` scope:** the no-`.meta` rule applies to ZEngine's own asset
system (`Assets/`, `Scripts/`, `Shaders/`). Third-party host packages under
`integration/` follow their host's rules. The Unity package currently ships no
`.meta` and configures `PluginImporter` via an Editor menu instead.

**Root `.gitignore:312` has `*~`**, which silently swallows any path ending in
`~` — including Unity's mandatory `Samples~` folder (same trap as `Tests~` in
§2.44). `integration/unity/.gitignore` re-includes
`com.zengine.host/Samples~/`. New UPM packages must do the same; **do not**
rename the folder, UPM requires the trailing `~`.

**Rollout:** P0 specs -> P1 two validation lines (Unity/D3D12 in-process render,
Blender/Python data + AI) -> P2 UE + Godot -> P3 3ds Max / Maya / Substance.

**Priority re-ranking (not deletion):** `doc/PHASE1_FOUNDATION_PLAN.md` and
`PHASE2_PLAN.md` are historical engine-building plans, **not the current
roadmap** — do not start new work from them without an explicit decision.
Keep investing in the RHI backends, RenderGraph, bindless, shader compile/cache,
reflection + codegen, ZSlate abstractions, `.zasset`/serialization,
`AssetImporter`, MegaLights. Deprioritise *new* investment in Player/packaging,
runtime Addressables, deeper Nanite/Lumen/WorldPartition/LWC layers and the
remaining TS editor-shell phases — the host already owns those. Our
differentiation is frontier effects + cross-host visual consistency + AI
discoverability/verifiability.

---

## 1. Reference codebases (read-only)

- `G:/OSGame/unity-engine-source/` — **user-specified primary Unity
  reference** (2022.3.5f1, `2022.3/release`; `UnitySourceInfo.txt:1-6`),
  the OSGame customized source. Use this for asset pipeline / importer /
  mesh write-back questions. Its `IUnityGraphicsD3D12v7` GUID
  (`Runtime/PluginInterface/Headers/IUnityGraphicsD3D12.h:94`) is identical
  to `integration/unity/native/ZUnityPluginInterface.h:51-52`, so A3's
  v7-only rule holds on this tree too.
- `../unity2023.1/` — 2023.1 Unity engine source; the reference used by the
  A3 design notes for Editor/Runtime API surface and tooling UX.
- `../UnrealEngine/`  — Unreal Engine source. Reference for low-level
  systems (asset registry, reflection, hot-reload, packaging).
- ZEngine's own implementations should look at all of these, but **ZEngine is
  not a fork** — copy ideas, not files.

## 2. Hard project conventions

These override "common practice" elsewhere. Do not re-litigate them in a new
session; if the user wants to change one, update this file in the same turn.

### 2.1 No `.meta` sidecar files

ZEngine does **not** use Unity-style `.meta` sidecar files for any asset type.
Reasons:

- Doubles the file count in version control and the project window.
- Easy for users to delete by accident, breaking GUID stability.
- Forces every external tool (TS compiler, image editor, etc.) to learn the
  convention.

**Replacement strategy (UE-inspired)**:

| Asset kind                                  | Identity carrier                     |
|---------------------------------------------|--------------------------------------|
| Binary assets (`.zasset`)                   | GUID embedded inside the .zasset header. The header is a 176-byte `AssetFileHeader` prefix (magic = "ZASS") stamped at file offset 0 by `SerializedFile::WriteHeaderAndMetadata`; AssetRegistry reads it back via direct `ifstream::read` in `scanSingleAsset`. The GUID is a 32-char lowercase hex string derived deterministically from the absolute output path (lower-cased on Windows) using FNV-1a 64×2, so re-writes on the same path stay GUID-stable across editor sessions. Files written before this convention landed have no prefix and are still loadable -- AssetRegistry assigns them a `"legacy:" + path` synthetic GUID instead. See `engine/Source/Runtime/Core/Serialize/SerializedFile.cpp` (P2 #6 markers) and `engine/Source/Runtime/asset/asset_file.h`. |
| Text source assets (`.ts`, `.js`, `.glsl`)  | GUID stored in a single per-project registry file: `<Project>/AssetRegistry/script_registry.json` (**checked into VCS** — analogous to Unity's `ProjectSettings/`, NOT `Intermediate/`). The registry is the source of truth; missing entries are auto-created on first scan from a deterministic hash of the relative path (`Hash128(rel_path_lower)`), so a clone whose registry was lost rebuilds identical GUIDs. **JSON writes are debounced** (500 ms quiet window via `ScheduleSave` / `TickDeferredSave` in `EditorAssetManager::TickWatcher`; `FlushPendingSave` on `Initialize`/`Shutdown`) so rapid watcher bursts do not rewrite the whole file every event. |
| Project-config assets (`.zproject`, scenes) | path-based, no GUID needed.          |

When a user renames or moves a `.ts` file, the `ScriptRegistry` updates the
`path → guid` mapping in place; existing references (which store the GUID,
not the path) survive automatically. This is structurally equivalent to UE's
`ObjectRedirector`, just centralised in one JSON instead of one redirector
asset per move.

### 2.2 Directory layout for scripting

```
<Project>/
├── Assets/                       # binary .zasset files only
├── Scripts/                      # user-authored .ts (UE-style, peer of Assets/)
├── Shaders/                      # user-authored .shader / .hlsl / .compute
│                                 # / .raytrace (UE-style, peer of Assets/)
├── AssetRegistry/                # checked into VCS
│   └── script_registry.json      # path<->GUID for all .ts (UE Redirector equiv.)
├── Intermediate/                 # gitignored; engine-managed
│   ├── Scripts/                  # tsc output (.js + .js.map)
│   ├── Shaders/                  # compiled SPIR-V/DXIL cache (mtime-keyed)
│   └── Typings/                  # engine-emitted .d.ts for binding
├── tsconfig.json                 # engine-generated if missing
├── package.json                  # engine-generated if missing
└── *.zproject
```

Decided in P1+P2:
- `Scripts/` peer of `Assets/` (UE-style) - chosen because demo project
  `I:\ZEngineDemo\` already had `Scripts/` at project root.
- `AssetRegistry/` peer of `Assets/` - **checked into VCS**, not under
  `Intermediate/`. Rationale: registry IS the GUID source of truth; losing
  it on a fresh clone would silently break cross-scene references. We accept
  occasional 3-way merges on this JSON (entries are line-stable).
- Content Browser shows `Scripts/` as a **top-level root peer of Assets/**,
  not nested under it.

Decided for Shaders (mirrors Scripts 1:1):
- `Shaders/` peer of `Assets/` and `Scripts/`. Created on project open by
  `ProjectInfo::ensureScriptsScaffold()` (yes, same function -- the name
  is now historical; it scaffolds both Scripts and Shaders).
- `Intermediate/Shaders/` holds the on-disk shader cache.
  - For `.shader` files compiled through `ShaderLabCompiler` (currently
    only the standalone unit test `shader_lab_test.cpp` -- ShaderLab is
    NOT on any editor / runtime hot path yet), the cache filename layout
    is `<src-path-hash>_<sub>_<pass>_<variant-hash>_<stage>.spv`. Pointed
    at by `ShaderLabCompiler::SetCacheDirectory()` (per-instance API; no
    project-wide default).
  - For `.hlsl` files compiled through `DX12ShaderCompiler` (the actual
    editor/runtime hot path -- runtime pass `createShaderModuleFromFile`,
    preview renderer's `compileFromSource`, inspector's shader-validate
    button), the cache filename layout is
    `<src-path-hash>_<stage>_<variant-hash>.dxil`. Caching is wired
    engine-wide via `DX12ShaderCompiler::setDefaultCacheDirectory()`,
    which is set ONCE in `DX12RHI::Initialize` from
    `ProjectInfo::getIntermediateShadersRoot()`. All future
    default-constructed `DX12ShaderCompiler` instances inherit this
    default automatically -- preview renderer's
    `static DX12PreviewRenderer renderer;`, the inspector's local
    `DX12ShaderCompiler compiler;`, and the lazy
    `DX12RHI::m_shader_compiler` all benefit zero-config. Per-instance
    override is available via `setCacheDirectory()`.
  - For `.glsl` / `.vert` / `.frag` / etc. compiled through the Vulkan
    backend's `::ShaderCompiler` (note the bare class name -- no
    `Vulkan` prefix; it lives at
    `runtime/function/render/interface/vulkan/shader_compiler.{h,cpp}`
    and is the runtime hot path on Android / OHOS / Linux, plus the
    SPIR-V emit stage of `ShaderLab::ShaderLabCompiler`), the cache
    filename layout is `<src-path-hash>_<stage>_<variant-hash>.spv`.
    Caching is wired engine-wide via
    `ShaderCompiler::setDefaultCacheDirectory()`, set ONCE in
    `VulkanRHI::Initialize` from
    `ProjectInfo::getIntermediateShadersRoot()`. Symmetrical to the
    DX12 wiring; new default-constructed instances pick up the default
    automatically. Variant key for the Vulkan path is
    `stage + sorted(defines)` (entry point is hard-coded to "main" by
    the glslang code path, so it doesn't participate). Both glslang
    in-process and the external `glslangValidator` fallback path go
    through the same cache layer (it sits above the `#if
    GLSLANG_AVAILABLE` split).
  - All three caches use FNV-1a 64 of `lower(absolute_path(src))` for
    the source-key portion (so `Foo.hlsl` vs `foo.hlsl` collapse to
    the same slot on case-insensitive filesystems). Variant-key
    portion hashes (entry_point + stage + sorted defines) for DX12,
    (stage + sorted defines) for Vulkan, or
    `stringifyVariant(ShaderVariantKey)` for ShaderLab. mtime
    invalidation: `last_write_time(cache) < last_write_time(src)` ->
    miss.
  - **Include-header tracking (DX12 + Vulkan)**: on cache hit, both
    backends do a second-stage freshness check that recursively walks
    `#include "..."` and `#include <...>` from the top-level source,
    accumulating max(mtime). If any transitively-included file is
    newer than the cache blob, the cache is treated as stale and the
    next compile overwrites the slot in place. The cache key itself
    does NOT depend on include contents -- it stays stable across
    edits, only the freshness comparator widens. The scanner is a
    blind text scan (no `#if` / `#ifdef` gating respected), which
    over-counts conservatively: false-positive recompiles cost a few
    hundred ms; false-negative stale binaries would cost hours of
    debugging. Recursion is guarded by a visited set + 64-deep cap.
    Implementation: `scanIncludesRecursive` in the anonymous namespace
    of each compiler's `.cpp`, called from `compileInternal` only on
    cache hit (zero overhead on cache miss, where DXC / glslang
    compilation dwarfs the scan cost anyway). NOTE: `WebGL2`
    deliberately has no equivalent because the WebGL2 backend has no
    host-side shader compiler -- emcc forwards GL ES calls straight
    to the browser, which compiles GLSL device-side at link time.
  - `compileFromSource` paths whose `shader_name` is NOT a real on-disk
    file path skip the disk cache entirely (no stable mtime ground
    truth). Preview renderer DOES go through this path (it strips
    `#pragma` lines and re-feeds the shader as a synthetic source) BUT
    sets `shader_name = preview_source.path.string()`, which IS a real
    file -- so the cache still hits there. Verify this remains true if
    the preview pipeline is refactored.
- **`ShaderRegistry` (landed)**: `<Project>/AssetRegistry/shader_registry.json`
  maps each `.shader` under `Shaders/` to a deterministic GUID, ShaderLab
  name, and generated `.zasset` path (`Assets/_Generated/Shaders/...`).
  `ShaderRegistry` is an `IEngineSystem` (see
  `Runtime/Function/Render/ShaderRegistry.{h,cpp}`). `MaterialRes::
  SetShaderByName` / load-by-`m_ShaderGuid` resolve through the registry
  first, then fall back to `AssetManager::getAssetsByType("ShaderRes")`.
  Rename detection uses content-hash like `ScriptRegistry`. JSON persistence
  uses the same 500 ms debounced save as `ScriptRegistry` (see table above).
  Editor shader watcher (PR-AI2) calls `OnShaderFileEvent` for `.shader`
  changes.
  **Still by-name downstream**: `MaterialSourceDesc` / render paths keep
  `eastl::string` shader names via `GetShaderName()` (PR-SE3c decision).
  Note this is bigger than just adding the registry file: today
  `MaterialRes::m_shader` is still `eastl::string` (the shader's name)
  and the same `eastl::string` is propagated through `MaterialSourceDesc`
  / `RenderMaterialData` / `RenderObject` / `VulkanPBRMaterial::
  shader_name`. A real GUID-ification pass needs to migrate
  `MaterialRes` schema (read-old-write-new, since existing `.zasset`
  materials embed the name string), and either GUID-ify the entire
  downstream chain OR introduce a single guid->name resolution
  point at the `MaterialSourceDesc` boundary. Hard-coded built-in
  shader names like `"StandardLit"` (special-cased in `FindShaderByName`
  to return early) stay outside the registry. Until that work is
  scheduled, rename of a project `.shader` breaks every material
  referencing it -- this is the concrete user-visible symptom that
  motivates eventually doing it.

  **PR-SE3 staging plan** (the migration is split across multiple PRs to
  keep the surface area auditable). PR-SE3a itself further splits into
  three sub-PRs (shadow / refine / migrate) because typed PPtr
  resurrection is a binary-format change that must be testable in
  isolation before it touches any res_type:
  * **PR-SE3a-shadow (DONE)**: schema-evolution scaffolding only.
    `MaterialRes` grows a sibling field `m_shader_guid` (`eastl::string`,
    default `""`) next to the existing `m_shader`. Both are written;
    reader prefers `m_shader_guid` once it's non-empty, otherwise falls
    back to `m_shader`. Crucially this is the first PR that puts the
    SafeBinaryRead schema-evolution path on a real production res_type
    -- old `.zasset` files (whose embedded TypeTree has no
    `"shader_guid"` node) load fine because SafeBinaryRead returns
    `kNotFound` for the missing field and leaves `m_shader_guid` at its
    default `""`. End-to-end coverage lives in scenarios M1+M2 of
    `engine/Source/Runtime/Core/Serialize/test/schema_evolution_smoke_test.cpp`.
    No ShaderRegistry yet, no downstream consumer changes -- writer
    leaves `m_shader_guid` empty, so downstream still resolves by name
    and behaviour is byte-identical for users.
  * **PR-SE3a-refine (DONE)**:
    resurrects `PPtr<T>`.
    Today `PPtr<T>::Transfer()` at `engine/Source/Runtime/BaseClasses/
    PPtr.h:97-99` is an empty stub, so the 39 `PPtr<X>` fields scattered
    across res_type / Prefab / Component / animation / Render have never
    serialized -- every cross-asset reference goes through ad-hoc
    string fields (shader name, texture path, prefab GUID string)
    because the reference layer that *should* solve this is dead code.
    This sub-PR (a) fills `PPtr<T>::Transfer()` with Unity's
    `(m_FileID:int32, m_PathID:int64)` pair contract; (b) bumps the
    inner `SerializedFileHeader.version` from 0 -> 1 (the outer ZASS
    prefix is untouched -- `scanSingleAsset` keeps working unmodified);
    (c) appends a real `m_Externals` table (`FileIdentifier{guid,
    asset_type, has_path, path}` array) to the metadata buffer, after
    the existing object table -- mirroring Unity's
    `SerializedFile.cpp:1814-1825` exactly; (d) introduces
    `IPPtrResolver` (`Runtime/BaseClasses/IPPtrResolver.h`) and
    `SerializedFilePPtrResolver` to do the
    `(InstanceID <-> (fileIndex, LFID))` mapping symmetrically with
    Unity's `ILSOIResolver`; the active resolver is **per-thread** via a
    `thread_local` stack + `ScopedPPtrResolver` RAII (NOT a single shared
    pointer like Unity does), so multi-thread AssetRegistry scan and
    AssetManager bulk loads don't contend on a mutex. **Hard constraint**:
    this PR may not modify any file under `Resource/res_type/**` or
    `Function/**`. Provable end-to-end through a new `PptrSmokeTest.cpp`
    (P1-P7: local round-trip, external round-trip, externals dedup, null
    PPtr, v1-file backward compat, dangling-target forward compat,
    concurrent-read TLS contract). Full design including binary layout,
    runtime contract, migration timeline, and risk register lives at
    **`doc/asset_management/PPTR_DESIGN.md`** (read that BEFORE writing
    any code for this sub-PR).
  * **PR-SE3a-migrate (DONE, merged with PR-SE3b)**:
    `MaterialRes` now has `PPtr<ShaderRes> m_shader_pptr` alongside the
    legacy `m_shader` string (shadow phase -- both written, reader prefers
    PPtr when valid, falls back to `m_shader`). `GetShaderName()` /
    `SetShaderByName()` accessors encapsulate the resolution priority.
    `Material.cpp::Transfer()` writes `m_shader_pptr` after `m_shader_guid`.
    Downstream consumers (`base_renderer`, `inspector_window`,
    `shader_preview_renderer`, `project_window`) switched to
    `GetShaderName()` / `SetShaderByName()`. M3a + M3b scenarios in
    `schema_evolution_smoke_test.cpp` cover old-layout→new read and
    PPtr round-trip. All 7 smoke-test scenarios pass.
  * **PR-SE3b (DONE, merged with PR-SE3a-migrate)**:
    chose strategy A2+B2 (no separate ShaderRegistry JSON; first-time
    seeding via `ShaderImporter` + reuse `AssetManager::getAssetsByType
    ("ShaderRes")` for name resolution). `ShaderImporter` scans
    `<Project>/Shaders/*.shader`, generates `ShaderRes .zasset` files
    under `<Project>/Assets/_Generated/Shaders/` only if the `.zasset`
    doesn't already exist (A2: first-time seeding, no continuous watch).
    `ProjectInfo::ensureScriptsScaffold()` creates `_Generated/Shaders/`.
    `EditorAssetManager::Initialize()` calls `ImportProjectShaders()`.
    `MaterialRes::SetShaderByName()` iterates
    `AssetManager::getAssetsByType("ShaderRes")` to find a match and
    sets the PPtr; built-in shaders (`"StandardLit"` etc.) leave PPtr
    null and use the `m_shader` string directly.
  * **PR-SE3c (DONE, decision: no downstream GUID-ification)**:
    Decision: **do NOT GUID-ify downstream** (`MaterialSourceDesc` /
    `RenderMaterialData` / `VulkanPBRMaterial` etc.). Shader reference
    resolution stays at the `MaterialSourceDesc` boundary via
    `MaterialRes::GetShaderName()` (which prefers `m_ShaderPptr` when
    valid, falls back to `m_Shader` string). Downstream structures
    continue to store shader as `eastl::string` and call
    `GetShaderName()` at the boundary. Rationale: no use case for
    cross-project material bindings today; widening the GUID surface
    would add complexity with no user-visible benefit. If a future
    use case emerges (e.g. sharing `.zasset` materials across projects
    with stable shader GUIDs), revisit this decision.
- **Compatibility strategy B (current)**: `.shader` files under
  `<Project>/Assets/` are still resolved (legacy demo projects rely on
  this), but every match logs a `LOG_WARNING(ZShader, ...)` deprecation
  notice. New shaders created via the Content Browser default to
  `<Project>/Shaders/`. A future hard-cutover can remove the legacy walk
  once all in-house demos have migrated.
- Content Browser shows `Shaders/` as a **top-level root peer of Assets/
  and Scripts/**, registered in `EditorFileService::buildEngineFileTree`
  right after the Scripts root.

Decided for Data (mirrors Scripts/Shaders, with one twist):
- `Data/` peer of `Assets/`, `Scripts/`, `Shaders/`. Source location for
  CSV (V1) / XLSX (V2) tables; **checked into VCS**. Created on project
  open by `ProjectInfo::ensureScriptsScaffold()` (the historical name now
  scaffolds Scripts + Shaders + Data). Content Browser shows `Data/` as a
  top-level root, registered in `EditorFileService::buildEngineFileTree`
  right after the Shaders root.
- **Compile output goes UNDER `Assets/`, not `Intermediate/`**: the path
  is `<Project>/Assets/_Generated/Data/<rel>.zasset`, with the source ->
  product path mapping being a 1:1 relative-path mirror, only the file
  extension changes. Rationale: the editor's `AssetRegistry` already
  scans the entire content directory, so emitting products under
  `Assets/_Generated/` makes them discoverable with **zero changes** to
  the registry / Content Browser / Inspector. The whole `_Generated/`
  subtree is gitignored via the scaffolding marker block in `.gitignore`
  (an entry `/Assets/_Generated/` was added next to `/Intermediate/`).
  This is the one place where Data deviates from the Scripts/Shaders
  template, which use `Intermediate/`. The reason Scripts/Shaders can
  use `Intermediate/` is that they are NOT consumed via AssetRegistry --
  scripts are loaded by ScriptingManager directly from
  `Intermediate/Scripts/` and shaders by the RHI's per-stage cache.
- Row class layering: `Object` -> `DataTableBase` (abstract,
  reflection-registered) -> `DataTable<TRow>` (template, type-erased
  helpers) -> user `WeaponDataTable : DataTable<WeaponRow>` final
  class. The user's wrapper class is the one that goes through
  `REGISTER_CLASS` -- `IMPLEMENT_REGISTER_CLASS` cannot swallow `<` `>`
  in a typename, so the template itself is never registered. We provide
  `DECLARE_DATA_TABLE(WeaponDataTable, WeaponRow)` /
  `IMPLEMENT_DATA_TABLE(WeaponDataTable, WeaponRow)` macros so the user
  side stays one line of declaration + one line of implementation.
- Row contract: a row is a plain `struct` (NOT an `Object` subclass)
  with `DECLARE_SERIALIZE(MyRow)` + `template<TF> Transfer()`. The first
  field touched by `Transfer()` must be `eastl::string id;` -- it is the
  primary key, hashed into `DataTable::m_key_index` on `onPostLoad()`,
  and CSV's first column maps to it. String fields use `eastl::string`
  per AGENTS.md 2.3.
- Storage shape: `std::vector<TRow> m_rows;` (NOT `eastl::vector` --
  `SerializeTraits` is only specialised for `std::vector`, see
  `Runtime/Core/Serialize/SerializeTraits.h`).
- No `DataRegistry` yet -- generated `.zasset` files inherit GUIDs from
  `SerializedFile` (same as every other binary asset), and the
  AssetRegistry indexes them automatically because they live under
  `Assets/`. If a future need for cross-project data-table references
  via stable GUID arises, it would be addressed at the AssetRegistry
  layer, not via a per-table-type sidecar registry.
- **CSV importer (V1) + XLSX importer (V2)** -- both landed. CSV lives
  at `engine/Source/Editor/asset_pipeline/data_table_importer/
  data_table_importer.{h,cpp}` (extension `.csv`); XLSX lives at the
  sibling `xlsx_importer.{h,cpp}` (extension `.xlsx`). Both implement
  `AssetImporter`. On editor startup, `EditorAssetManager::Initialize`
  calls `DataTableImporter::compileProject()` AND
  `XlsxImporter::compileProject()` which each walk `<Project>/Data/`
  for their own extension and emit products under
  `<Project>/Assets/_Generated/Data/<rel>.zasset`. Mirrors
  `TypeScriptCompiler`'s startup `tsc` pass on `.ts` sources.
  XLSX parsing is a hand-written SAX-style scanner targeting the
  Office Open XML subset that real spreadsheets emit (sharedStrings.xml
  + sheet1.xml + 5 core entities + numeric `&#NNN;` + `<![CDATA[]]>`);
  ZIP unpacking goes through `minizip-ng` (vendored at
  `engine/3rdparty/minizip-ng/`, linked PRIVATE into ZEditor). Multi-
  sheet workbooks: V1 reads only sheet1 by design (Q&A choice A);
  not a TODO. Stem collisions (`Weapon.csv` + `Weapon.xlsx`): XLSX
  wins, with a `LOG_WARNING(ZDataTable, ...)` so the user notices the
  shadowed CSV. GUID stability: `XlsxImporter::deriveStableGuid`
  hashes `lower(generic_string("Data/<rel>.xlsx"))` with FNV-1a 64,
  symmetric with `DataTableImporter::deriveStableGuid` and unrelated
  to the CSV slot (so a project that contains BOTH a `Weapon.csv`
  and a `Weapon.xlsx` produces two different `.zasset` products with
  two different GUIDs even though only one survives the stem collision
  -- this is intentional so post-hoc `git revert` of the XLSX
  unshadow's the CSV cleanly).
- **Schema registration**: each user `WrapperClass` registers itself with
  the importer via `REGISTER_DATA_TABLE(WrapperClass, RowType, alias,
  applier_lambda)` in an editor-side translation unit. The applier is a
  `void(TRow& row, const eastl::vector<eastl::string>& cells, const
  eastl::vector<eastl::string>& headers)` callback that converts CSV
  cells to row fields by header name. The user must fill `row.id`
  inside the lambda (we keep this convention so the applier stays
  symmetric across all columns). Default CSV->wrapper resolution policy:
  filename stem matches the registered alias (e.g.
  `<Project>/Data/Combat/Weapon.csv` -> alias `"Weapon"`). CSVs with no
  registered schema are skipped with a single batched warning per
  `compileProject` pass.
- **Editor / Runtime split**: the wrapper class
  (`DECLARE_DATA_TABLE`+`IMPLEMENT_DATA_TABLE`) lives in `ZRuntime` so
  prefabs/scenes that PPtr the table can deserialise in player builds.
  The `REGISTER_DATA_TABLE` call (which pulls in the editor-only
  `CsvSchemaRegistry`) lives in a sibling editor TU under
  `engine/Source/Editor/asset_pipeline/data_table_importer/Demo/
  <WrapperClass>Editor.cpp`. The reference demo
  (`WeaponDataTable`+`WeaponRow`) follows this layout exactly; copy it
  when adding a new table type.
- **CSV format (V1)**: RFC 4180 minimal -- comma separator, double-quote
  escape (`""` inside quoted fields), UTF-8 with optional BOM (BOM is
  stripped). First row is the header; first column header MUST be `id`
  (case-sensitive primary key contract). Rows shorter than the header
  are padded with empty cells; longer rows are truncated. Rows whose
  `id` cell is empty are skipped with a warning. Comments / alternate
  delimiters / multi-char quotes are V2.
- **XLSX format (V2)**: drop-in replacement for CSV at the source
  layer. First row is the header; first column header MUST be `id`.
  Cells are read as their displayed string (numeric `<v>` cells go
  through `to_string`-style conversion at parse time, NOT through
  Excel's locale-dependent format strings -- so "0.1" stays "0.1"
  on every machine). Only sheet1 is read (multi-sheet workbooks are
  trimmed silently; this is design choice A from V2 Q&A, not a TODO).
  XLSX support is **read-only from the editor's perspective** -- the
  Inspector edit-cells path is gated behind a `.csv` source check and
  XLSX rows render through the read-only V2 introspection path
  (below). To edit an XLSX-sourced table the user opens it in Excel /
  LibreOffice, saves, and the file watcher fires `compileOne` on the
  next focus pass. Not a TODO: writing `.xlsx` from the editor would
  require a full Office Open XML emitter, which is strictly larger
  than the parser, and the round-trip-through-Excel UX is what real
  data designers prefer anyway.
- **GUID stability for generated tables**: `DataTableImporter::
  deriveStableGuid` hashes `lower(generic_string(rel_csv_path))` with
  FNV-1a 64 -> formats as a canonical 8-4-4-4-12 GUID. Deleting
  `Assets/_Generated/Data/` and re-running `compileProject` produces
  bitwise-identical GUIDs, so any cross-asset references survive a
  clean. Same trick `ScriptRegistry` uses for source-file GUIDs.
- **Logging**: all importer messages go through the `ZDataTable` log
  category (added to `bq_log_category_config.ini` after `ZScripting`).
  Don't introduce a separate per-table category; the importer already
  prefixes the source path in every line.
- **Incremental rebuild (V1+V2)**: `EditorAssetManager` owns a SECOND
  `FileSystemWatcher` (`m_data_watcher`) rooted at `<Project>/Data/`
  with extension filter `{".csv", ".xlsx"}`. Events route by
  extension: `.csv` -> `DataTableImporter::compileOne / deleteGeneratedFor`;
  `.xlsx` -> `XlsxImporter::compileOne / deleteGeneratedFor`. The
  watcher is drained from `EditorAssetManager::TickWatcher` AFTER the
  primary `m_file_watcher` so the same frame's `_Generated/Data/<x>.zasset`
  write gets picked up by the registry one tick later -- this avoids
  fighting any in-flight `ReadObject` the inspector might be holding.
  Don't widen the primary watcher's root to `<Project>/` instead of
  Content/; that would inflate event volume by ~100x for projects with
  large `Scripts/` and force every existing onFileChanged path to
  discriminate by extension. Two narrow watchers stay simpler and have
  near-zero per-frame cost when idle.
- **Inspector view**: `inspector_window.cpp` ships
  `DrawDataTableInspector(asset_path, type*)` which renders any
  `DataTableBase`-derived `.zasset` regardless of which user wrapper
  class. Detection: `ResolveDataTableType` reads the asset's class name
  from the .zasset header, resolves to `Type*` via
  `TypeManager::ClassNameToType`, then walks `Type::base` up to
  `TypeOf<DataTableBase>()`. The branch lives in `InspectorWindow::onGUI`
  AFTER shader/material (those are more specialised) but BEFORE
  `IsGenericInspectorZAssetType` (otherwise DataTables would fall
  through to the unsupported-asset stub). The view shows class / source
  CSV / row count, action buttons (`Reimport` + Win32-only `Open Source
  CSV`), and a filterable per-row table. The "Reimport" button calls
  `DataTableImporter::compileOne` (or `XlsxImporter::compileOne` when
  the source is `.xlsx`) on the recorded source path, useful when the
  watcher missed a kernel notification (sandboxed editors that bypass
  FS events) or when a designer wants a clean rebuild after fixing an
  applier bug. The per-row table runs in EDIT mode by default (PR #7
  write-back) when the source is a reachable `.csv`, falling back to a
  read-only memory view rendered through the V2 introspection path
  (below) when the source is `.xlsx` (PR #6 deliberate fallback -- we
  do not write `.xlsx` from the editor, see V2 caveat below) or when
  the CSV is missing/unreadable.
- **Row introspection (V2 columns)**: `DataTableBase` exposes three
  virtual hooks implemented generically by `DataTable<TRow>`:
  `fillRowTypeTree(out)`, `rowDataAt(i)`, `rowSize()`. The Inspector
  materialises the row's `TypeTree` once per frame from a stack-local
  `TRow{}` probe (so empty tables still produce a header row), walks
  `Root().Children()` in declaration order to build a column list,
  and reads each cell as `*reinterpret_cast<const T*>(rowBase +
  byteOffset)`. Supported cell types (must match
  `SerializeTraits<...>::GetTypeString` exactly): `string`, `int`,
  `uint32_t`, `int64_t`, `uint64_t`, `float`, `bool`, `char`. Anything
  else (vectors, nested structs, PPtrs) renders as `<typename>` so the
  table layout stays valid and the gap is obvious. Bounds check:
  `byte_offset + byte_size > rowSize()` -> render `<oob>` instead of
  reading garbage. Per-frame cost on a 5-column row is ~10us
  (negligible vs ImGui's draw budget); if profiling ever shows it on
  top, the natural cache key is `const Type*` (row schema is invariant
  across instances of the same wrapper). The introspection path adds
  zero per-game cost -- new wrapper classes (`EnemyDataTable`,
  `SkillDataTable`, ...) inherit it for free via `DataTable<TRow>`.
  Strings are a special case: they ARE marked `kFlagIsArray` in the
  TypeTree (because `SerializeTraits<eastl::string>::Transfer` calls
  `TransferStringData` which routes through `TransferArray`), but the
  column collector overrides `is_array=false` for type-string `string`
  and reads them as `eastl::string*` directly -- this is safe because
  every `Transfer()`'d string field IS an `eastl::string` per
  AGENTS.md 2.3.
- **DataTable write-back (PR #7)**: when the source CSV is reachable,
  the Inspector enters EDIT mode and renders cells as
  `ImGui::InputText` / `Checkbox` widgets backed by an in-memory
  `(headers, rows[][])` string matrix. The matrix is the canonical
  authoring state during an editing session; the loaded
  `DataTableBase` is consulted only for the row-struct's per-column
  type info (so the Inspector knows which columns are `bool`, which
  are unsupported-and-therefore-read-only, etc.). Edits flip a `dirty`
  flag; `Save to CSV` calls `DataTableImporter::writeCsv()` which
  writes UTF-8-with-BOM + LF + RFC4180 quoting to a sibling `.tmp`
  and atomically renames onto the target. The PR #5 file watcher
  then sees the CSV change and fires `compileOne()`, refreshing the
  `.zasset`; the Inspector picks up the new mtime on its next frame
  and silently re-parses the matrix (only when `dirty == false` --
  pending edits are never clobbered by the watcher). `Discard`
  re-parses from disk, dropping pending edits. Rationale for
  round-tripping through the CSV (instead of mutating the loaded
  Object and re-serialising the .zasset): the CSV is the single
  source of truth (AGENTS.md "Data pipeline"); writing the .zasset
  directly would let the two drift, breaks `git diff` on data, and
  loses any CSV columns the row struct's applier doesn't read.
  Editable types match `IsSupportedCellType`'s whitelist (string /
  int32 / uint32 / int64 / uint64 / float / bool / char). Unsupported
  columns render as disabled `<typename>` cells but their CSV string
  values pass through `writeCsv` verbatim -- so e.g. a future
  `damage_curve: AnimationCurve` column added to `WeaponRow` won't
  destroy designer-authored data on first save through the
  Inspector. `parseCsv` is now public on `DataTableImporter` (it was
  private before PR #7) precisely so the Inspector edit session can
  load the matrix on selection change. V1 scope deliberately
  excludes: row add/remove (use external CSV editor for now), column
  reorder/rename (CSV header authoritative), undo/redo (`Discard`
  is the escape hatch).







### 2.2b Text serialization for scenes / prefabs (YAML)

Authoring data uses **text YAML**; DDC-imported data stays **binary `.zasset`**:

| Data | Format | File |
|------|--------|------|
| Scenes | YAML object graph | `.scene` |
| Prefabs | YAML object graph | `.prefab` |
| Materials | YAML object graph | `.mat` |
| Textures / meshes / animations / etc. | binary `SerializedFile` | `.zasset` |

Full design: **`doc/serialization/TEXT_SERIALIZED_FILE.md`**. Read it before
touching any of `Core/YamlSerialize/{YamlText,YAMLWrite,YAMLRead,YamlObjectGraph}`,
`Level::{save,load}`, or the `ImmediatePtr`/`PPtr` resolver path.

Key invariants:
- `YamlObjectGraph` is the text analogue of `SerializedFile`: many objects in
  one document (`objects:` array, each with `fileID` + class-name `type` tag +
  `data`), external `.zasset` refs in an `externals:` table by GUID.
- Cross-refs reuse the SAME per-thread `IPPtrResolver` as binary; local target
  = `(m_FileID 0, m_PathID = fileID)`, external = `(m_FileID = idx+1, ...)`.
  `ImmediatePtr<T>::Transfer` (was a stub) now resolves through it, like
  `PPtr<T>`.
- `AssetManager::{WriteObjectsToYaml,ReadObjectsFromYaml}` are the asset-system
  entry points; reuse the same external-ref hooks as the binary writer.
- **`TypeManager::ClassNameToType` is now content-keyed** (added
  `m_ClassNameStringToType`). The old `m_KlassNameToType` keyed on the raw
  `const char*` address, so by-name deserialization (and the asset-type-label /
  DataTable-inspector callers) only worked by accident before.
- Scene/graph code discriminates concrete types with `GetType() == TypeOf<T>()`,
  NOT `Object::Is()` -- `Is()`/`IsBaseOf` need a fully-built DFS type tree and
  return false for leaf types when `descendantCount == 0` in minimal processes.

### 2.3 String types in serialised fields

`SerializeTraits<T>` is specialised only for `eastl::string`, not
`std::string`. **Any field that participates in `Transfer()` must be
`eastl::string`.** Non-serialised path strings can stay `std::string`.

### 2.4 Source-file character set

Sources are compiled under MSVC code page 936 (GBK) on Windows. Avoid
non-ASCII punctuation in code/comments (em-dash `—`, fancy quotes, etc.) —
they trigger C4819 warnings. Use ASCII `-` and `"`. Chinese text in comments
is fine; only fancy ASCII-adjacent characters are problematic.

### 2.5 Log categories

Adding a new `LOG_INFO(ZFoo, ...)` call requires **two** steps:

1. Add `ZFoo` to `engine/3rdparty/bq_log_category_config.ini`.
2. Run (paths must be **absolute** - the cmake script doesn't resolve
   workspace-relative paths):
   ```
   cmake -DCONFIG_FILE=e:/Engine/ZEngine/engine/3rdparty/bq_log_category_config.ini ^
         -DOUTPUT_DIR=e:/Engine/ZEngine/engine/Source/Runtime/Core/log/generated ^
         -DCLASS_NAME=engine_log ^
         -P engine/Source/Precompile/generate_engine_log.cmake
   ```
   to regenerate `engine_log.h`.

Forgetting step 2 produces unresolved-symbol errors that look unrelated.

### 2.6 TypeScript toolchain (P3+)

- ZEngine **does not** bundle node/tsc. It either uses the project-local
  install at `<project>/node_modules/.bin/tsc(.cmd)` (preferred) or a
  globally-installed `tsc`. If neither is available the engine still boots
  but `TypeScriptCompiler` enters degraded mode (no compilation; logs a
  warning).
- For the `I:\ZEngineDemo` smoke project, run `npm install` once after
  fresh clone so tsc is available at the project-local path. The Node binary
  on this dev machine is `E:\emsdk\node\22.16.0_64bit\bin\node.exe`; ensure
  it's on `PATH` before launching ZEditor (Start-Process inherits the parent
  shell's PATH).
- `FileSystemWatcher` ships with a default extension filter of `{".zasset"}`
  for backward compatibility. Any new caller (script reload, shader hot-
  reload, config watching, ...) must call `setExtensionFilter({...})`
  BEFORE `watchDirectory()` or events will be silently dropped.

### 2.7 puerts backend selection (`PAPI_TYPE`)

ZEngine is a **JavaScript-only** scripting host (TypeScript -> tsc -> .js
-> JS VM) with a deliberate **two-VM strategy** that mirrors what
Cocos Creator / UE PuerTS / Unity JS hosts converged on:

- `quickjs` -> Web (emscripten/wasm) and mobile (iOS App Store bans JIT).
  ~400KB pure-interpreter VM. Same VM on every JIT-restricted platform
  so script semantics don't silently diverge.
- `v8`      -> Desktop editor + PC/console runtime. JIT brings script
  logic close to C++ perf, plus V8 Inspector gives Chrome DevTools /
  VSCode debugging out of the box.

Lua, Python and Node.js backends were all removed:
- **Lua / Python**: ZEngine commits to TypeScript-first, single source of
  truth for scripting.
- **Node.js**: game scripts don't need libuv / `fs` / `net` / `cluster`
  (the engine owns assets, networking and scheduling), and using Node-
  only APIs would silently break iOS / Web builds. Pure V8 gives the
  same JS engine without the Node embedding cost (libuv, npm modules,
  multi-MB binary). No mainstream game engine embeds Node for scripting.

`PAPI_TYPE` is selected at configure time via `-DPAPI_TYPE=...`:

| Value | `#define` | Status | Used for |
|-------|-----------|--------|----------|
| `quickjs` (**default** in `gen_windows.bat`, `gen_macos.sh`, `setup_ninja.bat`, `build_web.bat`, iOS scripts) | `PAPI_QUICKJS=1` | Fully wired in `ScriptingManager::Initialize` via `BackendQuickJS`. | Production path on Web / iOS / Android. |
| `v8` | `PAPI_V8=1` | **Wired and fully linking on Win64 (verified 2026-05-16, `bin\RelWithDebInfo\ZEditor.exe` 44.9 MB).** `BackendV8` mirrors `BackendQuickJS` symmetrically (both go through pesapi so `ScriptEnv` / `TypeScriptComponent` / etc. are backend-agnostic). `PapiV8.lib`, `ZRuntime.lib`, `Jolt.lib`, `glfw.lib`, `libcurl.lib` and the `ShaderLab` side-target all build cleanly under `/MT[d]` (Plan C). The earlier `editor_ui_pass.cpp` ifdef compile-phase blocker has been fixed -- the `#else` branch is now `#elif defined(Z_HAS_VULKAN)` with a no-op `#else` fallback, since on Windows the DX12 branch always early-returns and Vulkan, while compiled in as a switchable backend, is not on the editor ImGui hot path. **The `__std_*` STL toolset gap is resolved**: VS Installer was upgraded to 17.14.37301.10 which ships **MSVC 14.44.35207** alongside the legacy 14.38/14.39 SxS components; selecting 14.44 at build time (`/p:VCToolsVersion=14.44.35207` on the msbuild command line, or `$env:VCToolsVersion = "14.44.35207"`) supplies the missing `__std_min_*` / `__std_max_*` / `__std_remove_*` / `__std_find_*` / `__std_replace_*` / `__std_mismatch_*` / `__std_find_end_*` / `__std_find_first_of_trivial_pos_*` vectorized helpers that `wee8.lib` was emitted against. The CMake configure step is left at "default toolset"; only the build command needs the override. **V8 Inspector wired and runtime-verified (2026-05-16)**: `BackendV8::OpenRemoteDebugger / OnTick / DebuggerTick / CloseRemoteDebugger` forward into puerts' `CreateInspector / InspectorTick / DestroyInspector` exports (PapiExport.cpp). The v8::Isolate* is recovered on demand via the `GetV8Isolate(pesapi_env_ref)` export; BackendV8 captures the live ScriptEnv in `OnEnter`. ScriptEnv was reordered so `OnEnter` runs before `OpenRemoteDebugger` and the destructor calls `CloseRemoteDebugger` symmetrically. **Critically, `BackendV8::OnTick` was changed from a no-op stub to forwarding into `DebuggerTick` (= `InspectorTick(isolate)` = `Server.poll()` on the websocketpp server)** -- without this, /json HTTP discovery hangs because the kernel-accepted TCP connections never get drained by user-space. Editor main loop already calls `ScriptingManager::Tick -> ScriptEnv::Tick -> Backend::OnTick` once per frame, so this is enough. Smoke-tested end-to-end on 2026-05-16: launch with `$env:ZENGINE_V8_DEBUG_PORT="9230"`; `GET http://127.0.0.1:9230/json/version` returns `{"Browser":"Puerts/v1.0.0","Protocol-Version":"1.1"}` and `GET /json` returns the inspector target with `webSocketDebuggerUrl=ws://127.0.0.1:9230`. Chrome DevTools / VSCode can attach via that URL. Inspector port selection lives in `ScriptingManager::Initialize` and reads `getenv("ZENGINE_V8_DEBUG_PORT")` (positive int 1..65535); default `-1` keeps the inspector closed, matching the QuickJS path. | Desktop editor + PC/console runtime. |

The `papi-lua/`, `papi-python/` and `papi-nodejs/` subtrees stay vendored
under `engine/3rdparty/puerts/unity/native/` (we don't fork upstream)
but are never `add_subdirectory`'d, and `Runtime/Scripting/backends/{lua,python}/`
have been deleted. Trying `-DPAPI_TYPE=lua|python|nodejs` now FATAL_ERRORs
at configure time.

When debugging scripting issues, **always check
`build/CMakeCache.txt:PAPI_TYPE` first** to confirm the active backend.

Reconfigure with: `cmake -S . -B build -DPAPI_TYPE=quickjs` (or `v8`).

Or via the gen scripts (both already accept `--papi`):
- `gen_windows.bat --papi v8`
- `gen_macos.sh --papi v8`
- `setup_ninja.bat --papi v8`

`gen_windows.bat` additionally accepts `--no-vulkan`, which forwards
`-DZENGINE_USE_VULKAN=OFF`. That produces a **DX12-only Windows build**: no
`VULKAN_SDK` env var / Vulkan SDK install is required (otherwise
`engine/CMakeLists.txt` FATAL_ERRORs), `Z_HAS_VULKAN` is not defined, and
`Runtime/CMakeLists.txt` strips the Vulkan / Metal / non-DX12-pass TUs while
`ShaderLab` drops `ShaderLabCompiler.cpp`. Use it for slim CI or on machines
without a Vulkan SDK. `--vulkan` is the (default) inverse. Run
`gen_windows.bat --help` for the full flag list.

The default stays at `quickjs` so a fresh clone without the V8 backend
binaries still configures cleanly. To switch the project-wide default to
v8 once the V8 binaries are vendored / auto-downloaded, change the four
gen scripts plus the `set(PAPI_TYPE "quickjs" CACHE STRING ...)` line in
`engine/3rdparty/CMakeLists.txt`.

**Build-time prerequisite for `v8`**: puerts' `papi-v8` CMakeLists pulls
V8 headers + static libs from `engine/3rdparty/puerts/unity/native/papi-v8/.backends/papi-v8/`.
puerts upstream expects the directory under `.backends/` to be named after
`JS_ENGINE` (default `v8`), but in this clone the directory is actually
named `papi-v8`, so `engine/3rdparty/CMakeLists.txt` overrides
`set(JS_ENGINE papi-v8)` before `add_subdirectory(papi-v8)`. It also has
to inline the platform-specific `BACKEND_INC_NAMES`/`BACKEND_LIB_NAMES`/
`BACKEND_DEFINITIONS` (puerts' upstream `make_win64.bat` /
`make_linux64.sh` / `make_osx64.sh` set these via the CLI).

The vendored Windows `wee8.lib` (V8 13.6.233) requires C++20 -- the
`BACKEND_DEFINITIONS` therefore include `V8_129_OR_NEWER`, which makes
`papi-v8/CMakeLists.txt` pick `CMAKE_CXX_STANDARD 20`.

Other platforms need the matching `.backends/papi-v8/` slot populated
(Android: `Lib/Android/<abi>/`, iOS: `Lib/iOS/`, macOS: `Lib/macOS_arm64/`
or `Lib/macOS/`, Linux: `Lib/Linux/`). Missing libs surface as link-time
`unresolved external symbol` for V8 internals, NOT for our `BackendV8`
-- if the `BackendV8` symbols themselves are unresolved, check the
`Papiv8`-vs-`PapiV8` gotcha below.

**Win64 v8 build prerequisites (RESOLVED 2026-05-16)**:
The vendored `Lib/Win64/wee8.lib` is built with `MT_StaticRelease` (`/MT`
static release CRT, `_ITERATOR_DEBUG_LEVEL=0`) AND was compiled with
clang-cl on a more recent MSVC STL than 14.39. Three cascading issues
were identified in the original analysis. As of 2026-05-16 **all three
are closed**: #1 by Plan C in this repo, #2 by upgrading to MSVC
14.44.35207 on the dev box (selected per-build via `VCToolsVersion`,
see "Action items" block below), #3 by fixing the `editor_ui_pass.cpp`
Vulkan ifdef leak. The block below is kept as a tombstone so future
maintainers don't repeat the diagnosis from scratch if a similar
toolchain mismatch reappears.

1. **CRT flavor mismatch (resolved by Plan C in this repo)**: ZEngine's
   default `/MD[d]` produces `LNK2038: 'RuntimeLibrary'` when pulling
   `wee8.lib` + `PapiV8.lib` into a Debug binary. Resolved by
   `Plan C` -- the top-level `CMakeLists.txt` now sets
   `CMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded"` (plain `/MT`, no Debug
   variant) when `PAPI_TYPE=v8` on Windows, AND
   `engine/3rdparty/CMakeLists.txt` flips
   `CURL_STATIC_CRT=ON` (cURL) and uses /MT Jolt knobs in the same
   condition. (It used to flip GLFW's `USE_MSVC_RUNTIME_LIBRARY_DLL`
   too; GLFW is gone -- native Win32/Cocoa window layer -- and that
   knob was removed 2026-10-25.) Both branches
   of the gate use FORCE-cache so flipping `PAPI_TYPE` between v8 and
   quickjs in an existing `build/` tree resets cleanly. Stock `Debug`
   config is rejected (`wee8.lib` ships only `MT_StaticRelease`, no
   `MTd_StaticDebug`) -- on multi-config generators (Visual Studio) we
   print a warning at configure time, on single-config (Ninja) we
   `FATAL_ERROR`. Use `--config Release`, `--config RelWithDebInfo`, or
   `--config DebugV8`.

   **`DebugV8` config (added 2026-05-17)**: a custom build configuration
   that gives "Debug-friendly UX on top of Release ABI", so V8 path
   users still get full single-step debugging. It is registered into
   `CMAKE_CONFIGURATION_TYPES` only when `PAPI_TYPE=v8` on Windows
   (multi-config generators). Flag matrix:

   | Knob                            | DebugV8       | Stock Debug | RelWithDebInfo |
   |---------------------------------|---------------|-------------|----------------|
   | `RuntimeLibrary`                | `/MT`         | `/MTd` *    | `/MT`          |
   | `_ITERATOR_DEBUG_LEVEL`         | `0`           | `2` *       | `0`            |
   | `_DEBUG`                        | not defined   | defined *   | not defined    |
   | Optimisation                    | `/Od /Ob0`    | `/Od /Ob0`  | `/O2 /Ob1`     |
   | `/Zi` (PDB)                     | yes           | yes         | yes            |
   | `/RTC1` (runtime checks)        | yes           | yes         | no             |
   | Single-step lands every line    | yes           | yes         | partial        |
   | Variables visible (no opt-out)  | yes           | yes         | partial        |
   | STL bound/iterator checks       | **NO**        | yes         | no             |
   | Links against vendored wee8.lib | **YES**       | LNK2038 *   | yes            |

   `*` = stock Debug under `PAPI_TYPE=v8` actually fails to link, so
   the cells marked `*` only describe what the *flags* would be.

   `CMAKE_MAP_IMPORTED_CONFIG_DEBUGV8` is set to
   `Release;RelWithDebInfo;` so any imported library lookup under
   DebugV8 picks the Release flavor (which is what `wee8.lib` IS).

   The Jolt knobs in `engine/3rdparty/CMakeLists.txt` use
   `$<CONFIG:Debug>` (literal `Debug` only, NOT DebugV8) so DebugV8
   automatically falls through to the `_jolt_compile_release_crt`
   branch, getting `/MT` -- matching the global Plan-C choice.
   `_ITERATOR_DEBUG_LEVEL=0` is supplied for DebugV8 by the top-level
   `CMAKE_CXX_FLAGS_DEBUGV8` instead of by Jolt's per-target options,
   so consistency is centralised.

   **Usage**:
   ```powershell
   # configure step (Visual Studio multi-config generator)
   cmake -S . -B build -DPAPI_TYPE=v8 -G "Visual Studio 17 2022"
   # build step
   cmake --build build --config DebugV8
   # or via the preset:
   cmake --build --preset windows_pc_v8_debug
   # or zbuild:
   python zbuild.py build --config debugv8
   ```

   **What you LOSE compared to stock Debug**: `_IDL=2` STL bound/
   iterator checks (you still have `/RTC1` for stack/uninitialised-var
   checks). `assert()` from `<cassert>` is still active because
   `NDEBUG` is not defined in DebugV8 either.

   **What you GAIN compared to RelWithDebInfo**: `/Od /Ob0` instead of
   `/O2 /Ob1`, so single-step always lands on the next source line and
   no local variables are optimised away.

   **Limitation**: only useful on multi-config generators
   (`Visual Studio 17 2022`, `Ninja Multi-Config`). On Ninja
   single-config you'd reconfigure with `-DCMAKE_BUILD_TYPE=DebugV8`,
   which works but defeats the point of having Debug + Release builds
   side by side.

2. **MSVC STL toolset gap (VERIFIED 2026-05-16, BLOCKING)**: Once
   issue #3 was fixed, the build progressed to the link step and
   produced exactly the predicted symptom: 50 `LNK2019` lines for
   `__std_*` symbols coming from `wee8.lib` (`heap.obj`, `search-util.obj`,
   `regexp-parser.obj`, `regexp-compiler-tonode.obj`,
   `maglev-graph-printer.obj`, `v8-debugger-agent-impl.obj`,
   `json.obj`, ...) culminating in
   `LNK1120: 17 unresolved externals` (full log:
   `e:\Engine\ZEngine\build_v8_link2.log`). The vectorized helpers
   `__std_find_end_2`, `__std_min_4u`, `__std_mismatch_2`,
   `__std_replace_8`, etc. are emitted by clang-cl when `wee8.lib` is
   compiled, but only exist in MSVC STL toolset >= 14.40 (VS 2022
   17.10+). On this dev box (VS 2022 17.9.6 + MSVC 14.38 / 14.39) those
   symbols are absent from `libcpmt.lib`, so the link fails. There is
   no Plan-C-style workaround at the CMake level for this -- the only
   fixes are toolset upgrade or rebuilding `wee8.lib`.

3. **`editor_ui_pass.cpp` Vulkan ifdef leak (FIXED 2026-05-16)**: The
   Windows `#else` branch of
   `engine/Source/Editor/render/pass/editor_ui_pass.cpp` used to
   reference `VulkanRHI` / `VulkanRenderPass` / `imgui_impl_vulkan.h`
   unconditionally, but those types/headers are only pulled in under
   `#if defined(Z_HAS_VULKAN)`. The `#else` is now `#elif defined(Z_HAS_VULKAN)`
   with a no-op `#else` fallback (Windows always early-returns through
   the DX12 branch above; macOS early-returns through the Metal branch;
   the no-op `#else` only fires on platforms that have neither, which
   today is none of our shipping targets). **Project-wide policy**
   (revised 2026-05-16):
   - Windows editor's **default** backend is **DX12** (the
     editor_ui_pass.cpp fast path early-returns through the DX12
     branch on Windows for ImGui rendering).
   - Vulkan is a **first-class supported, switchable backend on
     Windows** — both `Z_HAS_VULKAN` and the `VulkanRHI` runtime path
     remain compiled in for Win64. Bindless via
     `VK_EXT_descriptor_indexing` is auto-enabled on PC desktop GPUs
     (NVIDIA / AMD / Intel) the same way as on Android / HarmonyOS;
     PC just uses a higher default cap (16384 sampled images vs 4096
     mobile), clamped at runtime against
     `maxPerStageDescriptorUpdateAfterBindSampledImages`.
   - Mobile (Android / HarmonyOS) ships with Vulkan as the **only**
     backend.
   - When adding a new render-feature code path, do not write
     `#if defined(_WIN32)` to disable Vulkan; instead key off the
     active RHI selection at runtime (or the existing
     `Z_HAS_VULKAN` / `Z_HAS_DX12` configure-time macros).

**Action items to fully unblock v8 on Win64** (RESOLVED 2026-05-16):
- **MSVC toolset**: VS Installer was upgraded to **17.14.37301.10**,
  which ships **MSVC 14.44.35207** alongside the legacy 14.38/14.39
  components. The 14.44 STL exposes the vectorized `__std_*` helpers
  that `wee8.lib` was emitted against, eliminating the LNK2019 wall.
  Selection is done at **build time** (not configure time) by setting
  `VCToolsVersion`, e.g.:
  ```
  $env:VCToolsVersion = "14.44.35207"
  & MSBuild.exe build_v8\engine\Source\Editor\ZEditor.vcxproj `
      /p:Configuration=RelWithDebInfo /p:Platform=x64 /m `
      /p:VCToolsVersion=14.44.35207
  ```
  CMake configure (`cmake -S . -B build_v8`) is left at default toolset.
  Do NOT pass `-T version=14.44` to CMake -- CMake will FATAL_ERROR
  if the cached toolset doesn't match (`Either remove the CMakeCache.txt
  file ... or choose a different binary directory.`); the per-build
  override is sufficient and avoids a 30+ minute rebuild.
- **Windows SDK**: `10.0.22621.0` is reinstalled and the build no
  longer trips on `MSB8036`.
- **Optional long-term**: Rebuild `wee8.lib` from V8 source with a
  pinned MSVC version (or any version >= 14.39 paired with
  `_DISABLE_VECTOR_ANNOTATION` / disabling the vectorized
  `<algorithm>` helpers) + `/MD` to produce a CRT-flexible static
  library. This is what puerts upstream eventually does in their CI;
  we just haven't run that pipeline ourselves. Not needed now that
  14.44 is on the box.

**`Directory.Build.props` toolset gate (2026-05-29)**: the root
`Directory.Build.props` auto-pins `VCToolsVersion=14.44.35207` so both
`cmake --build` and the VS IDE pick up the v8 toolset without a
per-session env var. The gate now requires the 14.44 toolset to be
**actually installed** (it probes the well-known VS 2022
`Professional/Enterprise/Community/BuildTools\VC\Tools\MSVC\14.44.35207`
paths) AND the vendored `wee8.lib` to be present, before forcing it.
The earlier version keyed only off `wee8.lib` existing, which broke
the CMake `VCTargetsPath` configure probe with `MSB8052` ('14.44.35207'
not compatible with v143) on any box that lacked 14.44 (e.g. a fresh
14.39-only machine, or pre-upgrade MSBuild < 17.10 which maps 14.44 to
v144). `$(VCInstallDir)` is not yet defined when Directory.Build.props
is imported, hence the hardcoded path probe. Net effect: a clone on a
14.39-only box configures (QuickJS) cleanly; a 14.44 box still forces
14.44 for v8. Callers can always override by setting `VCToolsVersion`
in the environment.

**Second dev box brought to v8 parity (2026-05-29)**: the
`ZENTIALI-PC19` box was VS Professional **17.9.6** with only MSVC
**14.39.33519** (MSBuild 17.9.8, which rejects 14.44 for v143). Updating
it to v8 required a full `setup.exe update --channelUri
https://aka.ms/vs/17/release/channel` to **17.14.37314.3** (NOT just a
`modify --add` of the 14.44 component -- the 14.9 catalog has no 14.44,
and MSBuild 17.9 can't use it anyway). The update replaced 14.39 with
**14.44.35207** as the sole "Latest" build-tools toolset. Gotcha:
`Start-Process -ArgumentList @(...)` mangles the `--installPath "C:\Program
Files\..."` quoting (truncates at the first space -> installer errors
`Could not find an installed product matching ... installPath: C:\Program`);
pass the whole arg list as a **single string** with embedded quotes
instead. After the update, wipe `build/CMakeCache.txt` + `build/CMakeFiles`
and reconfigure (the cached 14.39 compiler path is gone); v8
`RelWithDebInfo` then links clean.

**Verified end-to-end Win64 v8 link (2026-05-16, `bin\RelWithDebInfo\
ZEditor.exe`, 44.9 MB)**: Plan C (CRT switch to `/MT[d]` when
`PAPI_TYPE=v8`) + 14.44 toolset + ShaderLab side-target (see below) +
`editor_ui_pass.cpp` Vulkan ifdef fix. Quickjs path is unaffected.

**Build environment regression (post VS 17.14 upgrade, hit + recovered
2026-05-16)**: The IDE upgrade transiently broke the build in two
unrelated ways before VS Installer reseated everything:
1. `cmake -S . -B build_v8` (re-)configure failed with
   `could not find any instance of Visual Studio` even though
   `vswhere -prerelease` found 17.14. Cause: `CMakeCache.txt` pinned
   `CMAKE_GENERATOR_INSTANCE` to a path/instance ID that the new VS
   Installer didn't know about. Fix was to delete that cache line
   (`findstr /V CMAKE_GENERATOR_INSTANCE CMakeCache.txt > .new && move`)
   AND let VS Installer re-register the instance (which it does as
   part of installing the SDK component below).
2. `MSBuild` failed at `_CheckWindowsSDKInstalled` with
   `MSB8036: The Windows SDK version 10.0.22621.0 was not found.`
   The `C:\Program Files (x86)\Windows Kits\10\Include\` directory
   was missing. Fix: install the matching Windows SDK component via
   Visual Studio Installer; that also fixed (1) because VS Installer
   reseats the instance ID at the same time.
Both are noted here for the next time someone bumps VS major versions.

**ShaderLab side-target (FIXED 2026-05-16)**: Independent of v8, an
existing latent bug in the Editor link surface was exposed by the
v8 link logs: `inspector_window.obj`, `project_window.obj` and
`shader_preview_renderer.obj` all reference
`ZEngine::ShaderLab::ShaderLabParser` ctor / `Parse()`, but on
`Win+!Vulkan / Apple / Web` configurations
`engine/source/runtime/CMakeLists.txt` deliberately EXCLUDE's
`Function/ShaderLab/*.cpp` from ZRuntime (because
`shader_lab_compiler.{h,cpp}` includes
`Function/Render/interface/vulkan/shader_compiler.h`). That left those
three Editor TUs with no ShaderLabParser implementation to link
against -> 6 LNK2019/LNK2001 lines. On Linux/Android/OHOS/iOS/Win+Vulkan
this never reproduced because there ShaderLab cpp ships inside
ZRuntime. Fix:
- `engine/source/runtime/Function/ShaderLab/CMakeLists.txt` rewritten
  to compile a Vulkan-free **parser-only** STATIC (`shader_lab_lexer.cpp`
  + `shader_lab_parser.cpp`) when `ZENGINE_USE_VULKAN` is OFF, and
  add `shader_lab_compiler.cpp` only when it is ON. PRIVATE-links
  `ZRuntime` to inherit the bqlog include directory transitively (the
  cpp files include `runtime/core/base/macro.h` which pulls in
  `<bq_log/bq_log.h>` from ZRuntime's INTERFACE). PRIVATE keeps it
  off ShaderLab's INTERFACE so ZEditor doesn't double-propagate.
  No circular dependency: this CMakeLists is only add_subdirectory'd
  on configs where ZRuntime no longer contains the parser cpp itself.
- `engine/source/runtime/CMakeLists.txt` appended an
  `add_subdirectory(Function/ShaderLab)` call gated by the same
  three configurations that EXCLUDE ShaderLab cpp from ZRuntime
  (Web / Apple / `WIN32 AND NOT ZENGINE_USE_VULKAN`), guarded by
  `if(NOT TARGET ShaderLab)` so it's idempotent.
- `engine/Source/Editor/CMakeLists.txt` adds
  `target_link_libraries(${TARGET_NAME} PUBLIC ShaderLab)` guarded
  by `if(TARGET ShaderLab)`. On Vulkan-capable hosts the target
  doesn't exist (cpp is in ZRuntime already), so the link doesn't
  duplicate.

ShaderLab CMakeLists no longer has `target_link_libraries(...PUBLIC Base Render)`
(those targets don't exist as separate libs in this codebase -- the
original CMakeLists was unreachable code).

**Gotcha**: puerts' `papi-v8/CMakeLists.txt` declares the target as
`PapiV8` (capital V), while `papi-quickjs` uses `PapiQuickjs`. CMake target
names are case-sensitive, so `engine/3rdparty/CMakeLists.txt` and
`engine/Source/Runtime/CMakeLists.txt` must reference `PapiV8` exactly --
typo'ing it as `Papiv8` produces silent "TARGET PapiV8 not found" no-ops
and a degraded-mode binary at runtime (`m_ScriptEnv == nullptr`).

### 2.8 QuickJS libbf MSVC patch

`engine/3rdparty/puerts/unity/native/papi-quickjs/quickjs/libbf.c` uses GCC-
style operator overloading on `__m256d` (e.g. `r + m`, `a * b`) inside its
AVX2 FFT/NTT bignum path. MSVC's intrinsic headers don't support those
operators, so the AVX2 code doesn't compile under cl.exe. The parent
`engine/CMakeLists.txt` adds `/arch:AVX2` globally, which makes MSVC
predefine `__AVX2__` and pull the offending code in.

Workaround (in `papi-quickjs/CMakeLists.txt`): give `quickjs/libbf.c`
`COMPILE_OPTIONS "/arch:SSE2"` on MSVC only. MSVC's documented behavior is
that the LAST `/arch:` on the command line wins, so this overrides the
inherited `/arch:AVX2` and stops `__AVX2__` from being predefined for that
TU. Functionally harmless: the AVX2 path is only entered for `>=100`-limb
bignum mul, which scripts won't trigger. clang-cl is fine without the
patch (it understands the GCC operators), so the patch is gated on
`MSVC AND NOT CXX_COMPILER_ID STREQUAL "Clang"`.

### 2.9 Bindless texture path

Full design / per-PR landing notes / open items moved to
**`doc/BINDLESS_TEXTURE_PATH.md`** (the section was >400 lines and
overshadowing the rest of this file). Read that doc first if you are
about to touch any of:

- `RHI::supportsBindlessTextures()` / `getBindlessTextureManager()` /
  `RHIBindlessTextureManager` interface in
  `runtime/function/render/interface/rhi.h`.
- Vulkan backend: `vulkan_bindless_texture_manager.{h,cpp}`,
  `VulkanRHI::Initialize / clear` bindless paths.
- DX12 backend: `dx12_bindless_texture_manager.{h,cpp}`,
  `DX12RHI::createPipelineLayout` bindless detection,
  `DX12RHI::getBindlessRootSignatureFlags()`,
  `DX12RHI::kBindlessStaticSamplerCount` (PR7 SSOT for the
  static-sampler bank size),
  `RHI::cmdSetBindlessIndexPFN`.
- DX12 bindless utility pipeline (PR7):
  `runtime/function/render/interface/dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`
  + `dx12/utility/shaders/bindless_blit_{vs,ps}.hlsl`. First
  RHI-abstracted bindless graphics pipeline. Production consumers
  landed in PR8a/b/c (see `doc/BINDLESS_TEXTURE_PATH.md`):
  PR8a = scoped `SetDescriptorHeaps` swap at the call site;
  PR8b = `bindless_blit_smoke.{h,cpp}` dev-only ImGui canary widget;
  PR8c = `bindless_texture_preview.{h,cpp}` 256x256 inspector preview.
  **REMOVED (2026-05, ImGui cleanup)**: both editor widgets
  (`BindlessBlitSmoke` and `BindlessTexturePreview`) were dead ImGui
  code after the inspector went fully native ZSlate -- `DrawWidget` /
  `DrawPreview` had no remaining call site (the legacy ImGui
  `InspectorWindow` that dispatched them is gone). The
  `bindless_texture_blit_pipeline` itself is UNCHANGED and still the
  reference bindless graphics pipeline; the only live remnant of the
  preview module is the `texture2d` type predicate, now folded into
  `InspectorAssetCommon::IsTexture2DInspectorAssetType`. Texture images
  are viewed through the native ZSlate Preview window, not an inline
  inspector preview.
- HLSL `ResourceDescriptorHeap[NonUniformResourceIndex(...)]` use
  sites or GLSL `nonuniformEXT(...)` use sites.
- The `BindlessIndex::pack / unpackTexture / unpackSampler` helper
  (rhi.h, immediately below `RHIBindlessTextureManager`) -- engine-wide
  source of truth for the 32-bit `tex | (samp << 16)` packing.
- The DX12 bindless smoke-test under
  `runtime/function/render/interface/dx12/test/`
  (`bindless_smoke.hlsl`, `bindless_smoke_vs.hlsl`,
  `dx12_bindless_smoke_test.cpp`, gated by
  `ZENGINE_BUILD_BINDLESS_SMOKE_TEST=ON`).

Quick-reference invariants that must NOT drift without updating both
this file's pointer block AND `doc/BINDLESS_TEXTURE_PATH.md`:

1. `RHIBindlessTextureManager::kInvalidBindlessIndex` is `0xFFFFFFFF`
   and slot 0 is the engine-managed default-white placeholder (cannot
   be freed).
2. The 32-bit root constant pushed via `cmdSetBindlessIndexPFN` is
   always `BindlessIndex::pack(tex, samp)` -- low 16 bits texture,
   high 16 bits sampler. The smoke-test pins this with
   `static_assert`s against the HLSL literals.
3. Sampler-bindless (`SAMPLER_HEAP_DIRECTLY_INDEXED`) is intentionally
   NOT enabled -- static-sampler array is the current model on DX12,
   and the high half of the packed index addresses that array.
4. Bindless and bindful bindings cannot be mixed inside the **same**
   descriptor set on either backend.
5. (PR7) Every bindless-aware DX12 root signature carries
   **exactly 4 static samplers** at `s0..s3 / RegisterSpace 0`
   in the order `LinearWrap, LinearClamp, PointWrap, PointClamp`.
   The count is the `DX12RHI::kBindlessStaticSamplerCount` constexpr;
   the order is mirrored by the `BindlessBlitSampler` enum in
   `dx12/utility/bindless_texture_blit_pipeline.h` and by the
   `register(s0..s3)` slots in
   `dx12/utility/shaders/bindless_blit_ps.hlsl`. All three are
   pinned by `static_assert`s in the smoke-test; widening the
   bank requires updating all four sites (RHI cpp, constexpr,
   enum, HLSL) in lockstep.

### 2.10 Content Browser display rules (UE Content Browser model)

The Content Browser applies a **per-root extension whitelist** -- a file is
shown in a given top-level tree only if its extension belongs to that
tree's set, even when it physically lives there:

| Root        | Allowed extensions                                |
|-------------|---------------------------------------------------|
| `Assets/`   | `.zasset`, `.json`, `.scene`, `.prefab`, `.mat` |
| `Scripts/`  | `.ts`, `.tsx`, `.js`                              |
| `Shaders/`  | `.hlsl`, `.shader`, `.compute`, `.raytrace`       |
| `Data/`     | `.csv`, `.xlsx`                                   |
| `Textures/` | `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`          |
| `Models/`   | `.fbx`, `.obj`, `.gltf`, `.glb`                  |

Image and mesh source files surface under `Textures/` and `Models/` (checked
into VCS). Other source kinds (`.wav`, `.mp3`, ...) remain hidden until they
get their own top-level root. Source files are **never** surfaced under
`Assets/` -- only binary products (`.zasset`, scenes, prefabs) appear there.

**Single source of truth for the rules**:
`engine/Source/Editor/editor_file_service/editor_file_service.cpp`
function `shouldDisplayInContentBrowser(path, root_label)`. The
`root_label` parameter is the lower-case identifier that
`EditorFileService::buildEngineFileTree` passes to `buildRoot()`
(`"asset"`, `"scripts"`, `"shaders"`, `"data"`, `"textures"`, `"models"`).
Adding a new top-level root means adding a new branch here.

**Import product placement (UE rule)**:
`AssetsMenu::convertAsset(source_path, target_dir)` routes the `.zasset`
output into one of two places, in priority order:

1. `target_dir / <source-stem>.zasset` -- when `target_dir` is non-empty
   and lies inside `<Project>/Assets/`. This is what the Content Browser
   passes when the user has a folder selected inside the Assets tree at
   the moment they click Import.
2. `<Project>/Assets/<source-stem>.zasset` -- the project-content
   fallback. Used when the user's selection is in `Scripts/`, `Shaders/`,
   `Data/`, or anywhere else outside the AssetRegistry's scan root.

The product is NEVER written next to the source file (the pre-PR-PW2
behaviour) because that placement leaks `.zasset` files outside the
registry's scan root and silently breaks discovery. `target_dir` membership
in Assets/ is checked lexically against `ProjectInfo::getProjectContent()`
so it works even when the directory is freshly created in the Project
window and not yet on disk.

**Cross-process drag-drop import (PR-AI1, landed)**: dragging a file from
the OS file manager (Windows Explorer / Finder / Linux file manager) onto
the editor window auto-imports it via the same code path as the right-click
"Import…" entry. Wiring is single-direction and minimal:
the platform window turns `WM_DROPFILES` into `GenericWindow::OnDrop`
(`WindowsWindow.cpp`; originally `glfwSetDropCallback`), `WindowSystem`
forwards that into the existing `registerOnDropFunc` listener vector;
`ZSlateContentBrowserWindow`'s ctor subscribes one listener that copies the
path strings (only valid for the duration of the callback) into a
mutex-guarded queue, and `executePendingOsDropImports()` drains the queue
at the tail of `onGUI()` -- right after `executePendingDelete` /
`executePendingPrefabCreate`, so we inherit the same UAF-avoidance
deferral that those use against `buildEngineFileTree()` mid-walk. Drop
target is `resolveDropTargetFolder(m_selected_node)`; PR-PW2's two-stage
fallback in `AssetsMenu::convertAsset` then guarantees products always
land inside `<Project>/Assets/` even when the user had a Scripts/ /
Shaders/ root selected. Files with no registered importer (extension
not handled by any `AssetImporter`) are skipped with a single warning
per file -- equivalent to UE's `IsImportExtensionAllowed` gate.
Directory drops are explicitly out of scope (UE's
`ContentBrowserAssetDataSource` rejects them too). After a successful
batch we reset `m_last_file_tree_update` so the periodic Project-tree
rebuild (1 Hz) fires immediately on the next frame.

**Live HLSL recompile (PR-AI2, landed)**: editing
`<Project>/Shaders/Foo.hlsl` (or `.hlsli` / `.shader` / `.compute` /
`.raytrace`) outside the editor invalidates the matching DX12 DXIL cache
slot(s); the next PSO build the engine performs against that source
recompiles automatically thanks to the cache miss + DXC compile path.
Wiring is single-direction and minimal:
`EditorAssetManager` owns a third watcher `m_shaders_watcher` rooted at
`ProjectInfo::getShadersRoot()` with the source-extension filter listed
above. Events from create / change / delete all funnel through
`queueShaderInvalidation(path)` which stamps a 200 ms debounce deadline
in `m_pending_shader_invalidations` (mutex-guarded). `TickWatcher()`
calls `flushPendingShaderInvalidations()` once per frame after the data
watcher drain; entries past their deadline get popped and routed
through `DX12ShaderCompiler::invalidateCacheForSource(src)`. That static
method recomputes the same FNV-1a / `toLower` / `generic_string()` /
`absolute()` chain that `buildCacheFilePath()` uses, then prefix-matches
every `<src_hash>_*.dxil` under the default cache directory and deletes
them in one pass -- so all (stage, entry, defines, target_profile,
hlsl_version) variants sharing that source disappear together. 200 ms
matches Unity's coalesce window and is half UE's; it covers VS Code's
write-temp-then-rename save sequence on every platform we've measured.
`.hlsli` events currently no-op at the cache layer (the cache key is
the top-level source, not the header) but the recursive include-mtime
scan inside `dx12_shader_compiler.cpp` already rejects stale caches at
the next compile, so editing a header still recompiles dependent shaders
without an explicit reverse index. PSO rebuild itself happens lazily on
the next render-thread reference -- no broadcast event bus, deliberately
simpler than UE's `FShaderCompilingManager`.

**PR-AI3 (AutoReimport for externally-modified source files)** — landed.
The cross-platform `FileSystemWatcher` infrastructure
(`engine/Source/Editor/file_system_watcher/`) is already wired to
`Assets/` (`m_file_watcher`), `Data/` (`m_data_watcher`),
`Shaders/` (`m_shaders_watcher`), and `Intermediate/Scripts/`
(`m_js_watcher`); PR-AI3 added no new watcher modules. The trigger
model is **focus-driven, Unity-style** (not UE-style realtime
`FContentDirectoryMonitor`): on main-window focus-gained (`WM_SETFOCUS` -> `OnWindowFocus`), the editor
walks a `(.zasset → source_abs_path, source_mtime_ns)` registry, stats
each source, and enqueues changed entries into a throttled drain
(`kReimportsPerFrame = 10`) running inside `EditorAssetManager::TickWatcher`.
Rationale: ZEngine's user base is small; a focus-driven model is half
the code of UE's state machine and behaves more predictably (no
`PromptUser` race against typing). After a focus pass enqueues work,
`m_reimport_paused_until_focus = true` suppresses redundant rescans on
subsequent focus pulses (e.g., clicking between ImGui windows on
platforms where focus oscillates) until the queue drains.

The `(.zasset, source)` mapping is persisted to
`<Project>/AssetRegistry/source_registry.json` via the new
`SourceAssetRegistry` (composition member of `EditorAssetManager`,
**not** an `IEngineSystem`; it follows the same VCS-checked-in pattern
as `ScriptRegistry`). JSON shape: `{ "version": 1, "entries":
[ { "zasset", "source", "mtime_ns" }, ... ] }`. The registry is
written atomically (tmp file + rename, with `copy_file` + `remove`
fallback on Windows). Keys are normalised through
`SourceAssetRegistry::normaliseKey` (lowercase on `_WIN32`) so a JSON
shipped from a case-different machine still hits the same slot. Entries
are populated by `EditorAssetManager::recordImportSource(zasset, source)`,
called from `AssetsMenu::convertAsset` immediately after a successful
`importAsset` (the call site uses
`std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager))`
since `IEngineSystem` is polymorphic). Removed `.zasset` files are
unregistered in `EditorAssetManager::onFileDeleted`.

**Important format-stability decision**: the design doc originally
proposed adding `source_path` (64B) + `mtime` (8B) fields to
`AssetFileHeader::reserved[4]` (only 32B available -- would have
required a header-version bump and touched
`SerializedFile::ReadHeader`'s `m_ReadOffset = 176` constant). We
switched to the sidecar JSON approach to keep every existing `.zasset`
**bit-for-bit identical**. This is the same trade-off as
`ScriptRegistry` vs. embedded GUID for `.ts` files (per AGENTS.md §2.1):
the registry is the source of truth, the binary asset stays untouched.

**Importer interface change**: `AssetImporter` gained a
3-arg `reimport(zasset, source, settings)` virtual whose default impl
calls `import(source, zasset, settings, AssetMetadata{})`. This means
existing importers (`TextureImporter`, `ShaderImporter`,
`DataTableImporter`, etc.) work without per-importer overrides; only
importers that need source-discovery shortcuts (none currently) would
override it. The pre-existing single-arg `reimport(asset_path)` stub
on `TextureImporter` is unaffected.

**TickWatcher ordering** (deliberate, do not reshuffle):
`m_file_watcher` → `m_data_watcher` → `m_shaders_watcher` →
`tickReimportQueue()`. Reimports write `.zasset` files **after**
`m_file_watcher` has already polled this frame, so the resulting
register/refresh fires on the *next* frame -- a one-frame delay that
matches the data-watcher rationale and avoids re-entrant registry
mutations during `forEach`. Lock discipline inside `onEditorFocusGained`
and `tickReimportQueue` follows collect-under-lock → act-outside-lock
to avoid deadlocking against `recordImportSource` (which is callable
from importer code paths that may run on the same thread).
- **Manual reimport API**: `EditorAssetManager::reimportAsset(zasset_path)`
  resolves the source via `SourceAssetRegistry` and calls the importer's
  3-arg `Reimport(zasset, source, settings)` (same path as the focus queue).
  `importSourceAsset` wraps `AssetImportManager::ImportAsset` plus
  `RecordImportSource` + registry refresh. `TextureImporter::Reimport`
  (2-arg) delegates to `reimportAsset` when an editor registry entry exists.

### 2.12 Console window (editor)

- **Search**: filter bar with regex (default on), case-sensitive toggle,
  Ctrl+F focus, `visible/total` count. Matching uses `std::regex` in
  `ConsoleWindow.cpp` only (not the header).
- **Category**: `ImGui::BeginCombo` populated from
  `LogSystem::GetInstance().m_Logger->get_categories_name_array()` (same
  names as `engine_log` / `bq_log_category_config.ini`). Default **All**;
  combines with text/regex filter.
- **Deferred IBL logs**: `DX12RHI::LogDeferredIblCubemapDiagnostics()` re-emits
  cubemap GPU-mipgen lines after `EditorUI` init so early `RenderSystem`
  messages appear in the Console (on-disk log always has the originals).
- **Console commands (UE-style)**: `ConsoleManager` (`Runtime/Function/Console/`)
  owns CVars + commands; the Console window input bar calls
  `ExecuteString` (Tab autocomplete, Up/Down history). Case-insensitive
  lookup. CVar syntax: `set Name Value`, `Name Value`, `Name=Value`, or bare
  `Name` to print. Runtime builtins register in
  `RegisterRuntimeConsoleCommands()` (`stat fps`, `quit`/`exit`, `echo`,
  `t.MaxFPS`, `r.ShowFPS`). Editor builtins register in
  `RegisterEditorConsoleCommands()` from `Editor::Initialize` (`obj list`,
  `level reload|save`, `asset.find`, `asset.reimport`, `asset.count`,
  `play`, `pause`). Prefer registering new commands from module
  `Initialize()` rather than static `REGISTER_CONSOLE_*` macros (those run
  before `ConsoleManager` exists). See `doc/CONSOLE_MANAGER_USAGE.md`.

### 2.11 DX12 global texture upload (IBL / LUT)

- HDR cubemap and 2D globals use **`ExecuteDedicatedUploadCommands`** in
  `DX12RHI.cpp`: a one-shot allocator + command list + fence, never the
  frame `m_CommandAllocators[m_CurrentFrameIndex]`. Mirrors
  `DX12BindlessTextureManager::CreateAndUploadPlaceholder`.
- **`RHI_FORMAT_R32G32B32A32_SFLOAT` sources** are stored as
  `RHI_FORMAT_R16G16B16A16_SFLOAT` on DX12; CPU converts float32 to
  float16 in `CreateTextureUploadStaging` (`owned_pixels` keeps storage
  alive through submit). Log line: `DX12: storing HDR as R16G16B16A16`.
- **Init order (DX12 `RenderSystem`)**: `RenderPipeline::Initialize`
  first (root signatures on a clean device), then
  `UploadGlobalRenderResource`, then
  `DX12MainCameraPass::OnGlobalRenderResourceUploaded()` for skybox
  bindless + RP1/RP2 descriptor refresh.
- **Cubemap mips**: `CreateCubeMap` uploads mip 0 via staging, then
  `DX12CubemapMipGenerator` (linear PS downsample, same mip count as
  `RenderResource::CreateIBLTextures` / Vulkan `GenerateTextureMipMaps`).
  Implementation: `Utility/DX12CubemapMipGen.{h,cpp}` on the dedicated
  upload command list.
- Design / path-B landing notes: **`doc/DX12_SUBPASS_RHI.md`**.
- **DX12 editor sky / IBL**: active path is RP1 deferred UNLIT -> specular
  cubemap -> `backup_odd` HDR (not swapchain overlay). Full flow, UE
  comparison, and RenderDoc checklist: **`doc/rendering/DX12_SKYBOX_RENDERING.md`**.

### 2.12 Local variable and assignment style

- **Locals and parameters**: `snake_case` (see `doc/CODING_STYLE.md`). Constructor
  params that shadow a member use `in_` prefix (e.g. `in_system`).
- **Members / static / globals**: unchanged (`m_`, `s_`, `g_` + camelCase).
- **Assignment layout**: single space around `=`, no column alignment. Enforced by
  root `.clang-format` (`AlignConsecutiveAssignments: false`,
  `AlignConsecutiveDeclarations: false`). Re-format with VS LLVM:
  `"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe" -i <file>`.
- **Bulk camelCase->snake_case renames** need a TU-aware tool (regex breaks on lambdas
  and class inline bodies). Rename locals incrementally when touching a file.

### 2.13 MegaLights (Vulkan deferred, V1)

UE-inspired stochastic direct lighting for many local point lights without per-light
shadow maps. Full design: **`doc/rendering/MEGALIGHTS_DESIGN.md`**.

- **Toggle**: `r.MegaLights.Enable` (default `0`). When on, Vulkan skips
  `PointLightShadowPass`, zeros `point_light_num` in the mesh UBO, and
  `MainCameraPass` uses `megalights_deferred` instead of `deferred_lighting`.
- **CPU**: `MegaLightsSystem` in `RenderResource` (8x8 tile cull, SSBO upload).
  `RenderScene::SyncPointLightsFromLevel` rebuilds `m_PointLightList` from
  enabled `LightComponent` each frame. Debug ring: `r.MegaLights.SpawnTestLights [N]`.
  SS shadow ray march uses `subpassLoad(depth, offset)` with viewport size in the
  SSBO header (`ml_viewport_width/height`).
- **GPU**: `engine/shader/glsl/megalights_deferred.{vert,frag}` + `megalights_shading.inl`
  (bindings 8-10 on mesh global set). Rebuild `ZShaderCompile` after shader edits.
- **Vulkan**: `MainCameraPass` + GLSL `megalights_deferred.*`. **DX12**: `MainCameraRp1Pass` +
  HLSL `engine/shader/hlsl/rp1/megalights_deferred.frag.hlsl` (SSBO t8-t10 on mesh global set).
- **Not in V1**: HWRT, temporal denoise, rect/spot lights.

### 2.14 Editor dock layout (recurring clipped / black UI)

Symptom: dock panels squashed, large black regions, Game/Scene hints truncated
(`Press Left Alt...` cut off). Usually stale `<Project>/saved/config/imgui.ini`
or platform window size != ImGui `DisplaySize` after resize / DPI change.

**User recovery**: `Window` menu -> `Layouts` -> **Reset All Layouts** (or pick
`Default`). Restart editor if needed.

**Code SSOT**:
- `engine/Source/Editor/EditorLayout/EditorLayoutConstants.h` — toolbar height
  reserved above dock host (`MenuController` + `DefaultLayout` must match).
- `DefaultLayout::OnGUI` — `DockSpace(..., ImGui::GetContentRegionAvail())`,
  no `PassthruCentralNode`; auto-rebuild when root dock node size drifts
  >= 48px from host; ini auto-repair threshold 120px / 15% (was 400 / 30%).

Do not reintroduce `DockSpace(..., ImVec2(0,0), PassthruCentralNode)` for the
embedded main dock inside `MenuController`.

### 2.15 ZEngine Insights (CPU timing profiler)

Unreal-Insights-inspired CPU timing profiler. Replaces the abandoned Tracy
integration (we don't vendor a 3rd-party analyzer; the timeline renders natively
in ZSlate, both in an in-editor panel AND a standalone `ZInsights.exe` viewer).
Cooperating layers:

- **Capture core** -- `Runtime/Profiler/InsightsTrace.{h,cpp}`
  (`ZEngine::Insights::InsightsTrace` singleton). Per-thread event streams: only
  the owning thread appends (guarded by that thread's mutex; contended only by
  the occasional UI snapshot / frame prune on the main thread). Scope names are
  interned into a global string table (events store a 4-byte id). `BeginScope` /
  `EndScope` keep a strict LIFO open-stack per thread (a `-1` sentinel for scopes
  that began while paused, so begin/end stay balanced across capture toggles).
  Frame boundaries recorded by `EndFrame()`; a sliding window of the last
  `SetRetainedFrames(N)` frames is kept (default 150) and older fully-closed
  events are pruned (open-stack indices are shifted by the erased prefix).
  **Lock order is fixed**: name table -> (release) -> per-thread; the registry /
  frame locks are never held while a per-thread lock is held, and vice versa, so
  there's no deadlock between hot-path appends and `EndFrame` / `BuildSnapshot`.
  `BuildSnapshot()` copies a consistent `InsightsSnapshot` for the UI (the UI
  never touches live buffers).

- **Capture is heartbeat-gated, default OFF.** The Insights window pulses
  `RequestCapture()` every visible, non-paused frame; `EndFrame()` ages the
  heartbeat and flips `m_Capturing` off ~2 frames after the pulses stop (panel
  closed / hidden / paused). So a session that never opens the window pays only
  one relaxed-bool read per scope. When capture stops, pruning stops too, so the
  last view is frozen for inspection.

- **Instrumentation source**: the existing `ProfileScope` RAII helper (driver of
  the `Z_PROFILE_SCOPE` / `Z_PROFILE_FUNCTION` macros, ~30 call sites) now feeds
  BOTH the legacy per-frame `Profiler` aggregator AND `InsightsTrace`. Don't add
  a parallel macro; new instrumentation just uses `Z_PROFILE_SCOPE("Name")` and
  shows up on the timeline automatically. Frame boundary is
  `Application::TickOneFrame` -> `InsightsTrace::EndFrame()`. Thread track labels
  come from `ThreadManager` (`GameThread` in the ctor; worker threads via
  `SetThreadName`).

- **UI widget** -- `Runtime/Profiler/SInsightsTimeline.h` (header-only
  `SLeafWidget`, **lives in Runtime so both the editor panel and the standalone
  viewer share it verbatim**). Paints a frame ruler + one stacked flame-chart
  track per thread; left-drag pans, mouse wheel zooms about the cursor, hover
  reports the scope name + duration. It only depends on the abstract
  `UIRenderer` + `InsightsSnapshot`, so any `UIRenderer` backend can host it.

- **In-editor panel** -- `Editor/EditorWindow/ZSlateInsightsWindow/`.
  `ZSlateInsightsWindow` (toolbar Pause/Resume, Clear, Fit, **Save & View** +
  status line) builds a fresh snapshot each frame and keeps it as a member so
  the widget's snapshot pointer stays valid across paint+input. Renders through
  the editor's RHI-backed `BatchedUIRenderer` (`ZSlateEditorOverlay`).
  Registered as window id `kInsights` (`EditorLayoutWindowIds.h`) in
  `EditorWindowRegistry` (`default_open=false`), so it auto-appears in the
  Window menu via `BuildEditorWindowZSlateMenu`.

- **`.ztrace` file format + standalone viewer** -- `InsightsTrace.{h,cpp}`
  exposes `SaveTrace(path, snapshot)` / `LoadTrace(path, out)`: a flat
  little-endian dump of `InsightsSnapshot` (magic `'ZTRC'` + u32 version, names,
  frame marks, then per-track events). Free of engine-system deps so the
  standalone viewer reads it without `START_SYSTEM`. The editor panel's
  **Save & View** button (and the `insights.dump [view]` console command) writes
  a timestamped `<cwd>/Insights/trace_*.ztrace` and launches `ZInsights.exe` on
  it (`ShellExecute`, resolved next to `ZEditor.exe`).
  **`ZInsights.exe`** (`engine/Source/Insights/`, Win32-only target gated on
  `WIN32`) is a plain `ZSlate::FSlateStandaloneApp` tool: shell owns window,
  DPI, pump, input router **and** the D3D11 render backend, so the exe is only
  the widget tree — a vertical box with an auto-height status bar over the
  shared `SInsightsTimeline` filling the rest (open via argv / drag-drop / `O`
  key, `F` to fit) — plus trace loading. It used to carry a bespoke Direct2D
  host (`D2DUIRenderer`); that bought DirectWrite subpixel text and per-label
  clipping, but the shell's glyph atlas already bakes at the *scaled* integer
  pixel size (`FSlateDpiScaleRenderer` scales `font_size` before the atlas), so
  the sharpness argument did not survive — and the in-editor panel paints the
  same widget through the engine's ZSlate renderer anyway. It links the
  **static `ZRuntime`** (same pattern as the `LuaMemPreview` suite, which also
  absorbed the former `ZProfilerCsvExporter` as
  `ZEditor -batchmode -suite=LuaMemPreview <in.zasset> [out]`) but
  references only the timing-trace + Slate-base objects, so the linker pulls no
  RHI/GLFW/scripting code. No live socket transport yet (the data path is
  offline file by design choice).

- **GPU track (DX12)** -- `Interface/DX12/Utility/DX12GpuProfiler.{h,cpp}`.
  Records matched begin/end `D3D12_QUERY_TYPE_TIMESTAMP` queries on the frame
  command list into a per-frame-slot region of one READBACK buffer
  (`k_max_frames_in_flight` slots x `kQueriesPerFrame`). `DX12RHI::PrepareBeforePass`
  arms the slot (draining the previous use once its frame fence has signalled --
  so readback latency is `k_max_frames_in_flight` frames) and opens a whole-frame
  `"GPU Frame"` span; `SubmitRendering` closes it, `ResolveQueryData`s the range,
  and (after the queue Signal) records the gating fence value via
  `MarkSubmitted`. Per-pass sub-bars come for free: `RHIDrawList::ExecuteAll`
  wraps each entry in `BeginGpuTimingScope/EndGpuTimingScope(entry.debug_name)`
  (the same name as its CPU `Z_PROFILE_SCOPE`), gated on `IsCapturing()`.
  Resolved ticks are converted to the Insights trace clock with
  `ID3D12CommandQueue::GetClockCalibration` (GPU ticks <-> QPC) plus a
  per-frame QPC->steady_clock offset, then pushed via
  `InsightsTrace::PushExternalEvent("GPU", ...)` into a named track that renders
  exactly like a thread track. New RHI seam: `RHI::BeginGpuTimingScope` /
  `EndGpuTimingScope` (default no-op; only DX12 implements them today --
  Vulkan/Metal/WebGL2 get no GPU track yet). All GPU work is gated on capture
  being active (sampled once per frame in `DX12GpuProfiler::BeginFrame`), so a
  closed Insights window adds nothing to the command list.

- **V1 scope** (deliberately): CPU timing + frames + per-thread tracks + DX12 GPU
  track + hover + `.ztrace` save/load + standalone ZSlate/D3D11 viewer. No
  Vulkan/Metal GPU track, no counters/memory, no vertical scroll on tracks, no
  aggregated stats table, and no LIVE (socket) connection to a running editor --
  the standalone viewer is offline/file-based. All are natural follow-ups on the
  same snapshot model.

### 2.16 Cross-platform render pipeline modules

Full design / roadmap: **`doc/rendering/CROSS_PLATFORM_PIPELINE.md`**. Read that
before touching anything under
`engine/Source/Runtime/Function/Render/Pipeline/`.

A render path (Desktop = deferred+forward, Mobile = forward) is a first-class,
runtime-swappable **`RenderPipelineModule`** that lives ABOVE the RHI.
`RenderPipeline` is now a thin dispatcher holding
`std::unique_ptr<RenderPipelineModule> m_ActiveModule`; it forwards
`Initialize / BuildDrawLists / SubmitDrawLists / UpdateAfterRecreate` + the
query methods to the module. The named pass `shared_ptr` slots
(`m_MainCameraPass`, `m_DirectionalLightPass`, ...) still live on
`RenderPipelineBase` (external code reads them); modules are `friend`s and
populate/reset those slots in `Setup`/`Shutdown`.

**Convention (the important part for new work)**: a new render feature attaches
to a **module's** pass list (`DesktopRenderPipelineModule` and/or
`MobileRenderPipelineModule`), NOT to `RenderPipeline` directly, and must
declare its Desktop/Mobile applicability. Each concrete pass keeps its own
internal `getGraphicsAPI()==DirectX12` / `#if Z_HAS_VULKAN` branch -- the module
layer only abstracts *which passes run, in what order, under which path*.

Selection: `RenderPipelineSettings` (`RenderPath { Auto, Desktop, Mobile }`).
`Auto` -> Desktop on PC, Mobile on Android/iOS/OHOS. CVar `r.RenderPath`
(0/1/2) + editor `Window -> Render Path` submenu + `r.renderpath` editor
command, all via `SetConfiguredPath()` which flips a `std::atomic` dirty flag.
The actual switch is deferred to a safe point:
`RenderSystem::ApplyPendingRenderPathChange` (top of `Tick`) does
`ConsumePathDirty -> FlushRenderingCommands -> ExecuteOnRHIThread(SetActiveModule
+ RewireRenderResourceLayoutsAfterPathSwitch) -> GPU idle`. Never call
`SetActiveModule` inline from the game thread. Startup env override
`ZENGINE_RENDER_PATH` (`auto|desktop|mobile` or `0|1|2`) for headless/CI/smoke
(mirrors `ZENGINE_V8_DEBUG_PORT`).

**Mobile is a SKELETON in Milestone 1**: `MobileRenderPipelineModule` composes
the shared main-camera assembly (an internal `DesktopRenderPipelineModule`) so
the scene renders and the Desktop<->Mobile switch is verifiable end-to-end on
DX12; only `BuildDrawLists` diverges (forward-lite strip: no point-light shadow,
no MegaLights). The bespoke G-buffer-free forward chain (via `mesh_forward`
shaders), lightmap ambient, and simplified shadows are documented follow-up
sub-milestones in the design doc -- do NOT assume Mobile is a real forward
renderer yet. **Vulkan path** of either module currently hits a *pre-existing*
crash in `MainCameraPass::SetupModelGlobalDescriptorSet` (proven via A/B revert,
unrelated to the module refactor); DX12 is the verified backend.

### 2.17 Per-platform texture cook pipeline

Full design / phase landing notes / verification at
**`doc/asset_management/TEXTURE_COOK_PIPELINE.md`**. Read it before touching any
of: `Texture2D` mip/format schema, `TextureCompressor`, the DDC, `TextureImporter`
cook paths, `MaterialRes` texture PPtrs, or `RenderResourceBase::LoadTexture`.

Conventions (do not re-litigate):

- **One source image -> per-platform cooked variants.** No `.meta`, no ETC2.
  BC1/BC3/BC7 on desktop + WebGL, ASTC LDR on mobile (Android / iOS / OHOS).
- **`Texture2D`** stores a concatenated mip chain (`m_Pixels` + `m_MipOffsets`)
  and a real `RHIFormat` ordinal in `m_Format` (incl. BC*/ASTC). New `Transfer`
  nodes are append-only; old single-mip RGBA8 `.zasset` still load (SafeBinaryRead
  `kNotFound` -> default). See `Texture2D.{h,cpp}` + T1/T2 in
  `SchemaEvolutionSmokeTest.cpp`.
- **Three output locations**:
  1. `Assets/<stem>.zasset` -- editor-platform variant (Standalone/BC7 on the DX12
     editor). Consumed by editor preview + scene materials. Path-derived GUID.
  2. `Intermediate/DDC/` -- LMDB cache of raw cook artifacts, keyed
     `(cache_type="Texture", asset_guid, hash(settings+platform+encoderVer))` via
     `Runtime::MakeDDCCacheKey`. Opened by `Runtime::GetDerivedDataCache()`.
  3. `Intermediate/Cooked/<Platform>/<rel>.zasset` -- per-platform cook output.
     Reuses the **source** GUID (so player builds resolve references), written via
     `AssetManager::WriteObjectToDiskWithGuid(path, obj, source_guid)` (the only
     explicit-GUID write API; everything else uses `DeterministicGuidFromPath`).
  Both `Intermediate/*` roots are gitignored; settings JSON
  (`AssetRegistry/texture_import_settings.json`) is the only VCS-checked artifact
  (the `.meta` replacement).
- **`MaterialRes`** carries `PPtr<Texture2D>` shadow fields beside each
  `m_*TextureFile` string (write both, read prefer PPtr via `Get*TextureFile()`,
  fall back to the path string -- same staged pattern as `m_shader_pptr`). PPtr
  nodes are appended last in `Transfer()` so old material `.zasset` read back null.
- **Consumption**: `RenderResourceBase::LoadTexture` tries the cooked
  `<source>.zasset` first (`TryLoadCookedTexture`), else falls back to the legacy
  `stb_image` source decode. **Block-compressed cooked variants are gated to DX12**
  -- the Vulkan `CreateGlobalImage` path here does not yet decode BC/ASTC, so on
  Vulkan the fallback path runs (uncompressed, GPU mipgen via `miplevels=0`). ASTC
  is never sampleable on the DX12 editor -> verify ASTC cooks by file inspection,
  not rendering.
- **Entry points**: `Build -> Cook Textures for <Platform>` (MenuController) and
  console `asset.cook <standalone|android|ios|ohos|webgl>` (EditorConsoleCommands).
  Startup `TextureImporter::ImportProjectTextures()` (in `EditorAssetManager::
  Initialize`) seeds the editor-platform `Assets/<stem>.zasset` for any source
  image lacking one (A2 first-time seeding, idempotent).
- **Encoders** (`bc7enc_rdo`, ARM `astc-encoder`) link PRIVATE into ZEditor only
  but compile on the host so the Windows editor can cross-encode mobile ASTC.

### 2.18 ZSlate has exactly one copy

- The only ZSlate source tree is the submodule `engine/Libraries/ZSlate`
  (`github.com/zentia/ZSlate`). Standalone tools (e.g.
  `engine/tools/ClientProjBranches`) consume it with
  `add_subdirectory(<repo>/engine/Libraries/ZSlate ...)` + link `ZSlate`;
  never fork a private copy under `engine/tools/`. Missing widgets go into the
  submodule (commit there, then bump the pointer).
- Layout contract: a widget never asks to fill; the parent slot decides
  (`SBoxPanel::AddSlot(w).Fill()`, UE `SBoxPanel` semantics).
- Modal / popup layers: `SlateApplication::ShowModal` (used by `SDialog`)
  and `SlateInputRouter::OpenPopup` (used by `SComboBox` dropdowns). Hosts
  must route mouse to `SlateApplication::GetInputRoot()` and paint through
  `SlateApplication::PaintInto`, which draws modal + popup on top.
- **Diagnostics go out through a sink, not through `fprintf`**: `ZSlate::Log()`
  (`ZSlate/Core/SlateLog.h`) formats and dispatches to a process-wide
  `FSlateLogSink`; the default sink is the old stderr/stdout pair, so a host
  that installs nothing is unaffected. Hosts that own a real logging stack
  install one (`ZSlate::SetLogSink`) -- ZEnvironmentCollector's GUI routes every
  ZSlate line into 扁鹊, because a WINDOWS-subsystem process has no stderr
  anyone can read. Do not add direct `fprintf`/`printf` to ZSlate sources.

### 2.19 Localization: mechanism in ZSlate, strings in the tool

New UI text goes into the source **in English**, tagged with an id:
`ZSlate::Loc("collection.start", "Start collection and build ZIP")` (the GUI
wraps it in a short `S(id, english)` helper). Translations are tables the host
registers at start-up -- ZSlate ships none of its own. Adding a language is a
new `ELanguage` value plus a table; adding a string is one `Loc()` call plus one
row in the table.

- A missing entry falls back to the English literal and logs the id once through
  ZSlate's sink, so a typo shows up in the log instead of as a blank label.
- Strings resolve when a widget is **built**, so switching language rebuilds the
  tree (`ShellApp::RebuildUi`, which carries the form's contents across).
- The collector's table and the remembered choice live in `UiLocalization.cpp`
  (registry `HKCU\Software\ZEngine\EnvironmentCollector\Language`); that file is
  deliberately ZSlate-free because the CLI-only build links it too.
- **Report file content (TXT/JSON) stays Chinese regardless of the UI language.**
  Those files go to support, not to the player.

### 2.20 CRT mode: /MT tools inside a /MD tree

`MSVC_RUNTIME_LIBRARY` is baked into each object file, so a /MT executable
linking a /MD static library fails with LNK2038 ("RuntimeLibrary" mismatch)
**and** LNK2019 on the library's `__imp_*` symbols (`__imp_strncat`,
`__imp__wstat64`, ...). A tool directory that forces
`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` must therefore either build every
library it links itself, or inherit the tree's CRT when it is configured as part
of the tree (where `minizip-ng` / `BqLog` / `zlib-ng` already exist as /MD).
`engine/tools/ZEnvironmentCollector/CMakeLists.txt` does exactly that via
`ENVIRONMENT_CHECK_STANDALONE`: standalone (its own `cmake -S`, everything
local) -> /MT, which is what ships; inside the tree -> inherit. Do not "fix" a
link error here by forcing /MT unconditionally.

`-DZENGINE_STATIC_CRT=ON` (root `CMakeLists.txt`) flips the **whole tree** to
/MT, `ZRuntimeShared.dll` included. That is the switch that lets a tool which
links the engine DLL still ship to a machine with no VC redist. It has to be
whole-tree: one DLL cannot mix CRTs against the static libraries it links.
Crossing into the DLL is safe regardless, because the tool-facing API is
`extern "C"` only and never moves an allocator-owned object across the boundary.

### 2.21 Audio: three layers, one owner, headless by default

Audio landed as a vertical slice (`Runtime/Function/Audio/`, PR-A1). Keep to
this layering when extending it:

| Layer | Type | Rule |
|---|---|---|
| DSP | `AudioCommon.h`, `AudioDecoder`, `AudioMixer` | **No engine headers**: no LogSystem, no eastl, no engine math (`AudioVec3` exists for this reason). This is what lets `AudioMixerSmokeTest` compile and run standalone in CI. |
| Device | `IAudioDevice` + `AudioDeviceWasapi` / `AudioDeviceNull` | Platform-only. Owns its render thread and pulls frames through a callback; never touches clips or voices. |
| Engine | `AudioSystem` (an `IEngineSystem`, Gameplay phase) | Owns the clip cache, the mixer and the device. Components talk to it, never to the mixer directly. |

Invariants that are easy to break:

1. **Clips are cached for the process lifetime.** A voice stores a raw
   `const AudioClipData*`; evicting a clip while a voice plays is a
   use-after-free. `UnloadAllClips()` must only run after `StopAll()`.
2. **`ZENGINE_AUDIO_DISABLE=1` must keep every API working** with a null
   device, and `AudioSystem::Initialize()` must never return false because of a
   device failure. Headless CI, `-batchmode` and the standalone tools all
   depend on this; a failed `Initialize()` aborts the whole startup sequence.
3. **Mixer API is thread-safe; `Render()` runs on the device thread.** Every
   public mixer entry point takes `m_Mutex`. Keep the critical section free of
   allocation / I/O / logging -- that assumption is why a plain mutex is safe
   instead of a lock-free command queue.
4. **V1 is WAV only.** `DecodeAudioBuffer` reports `.ogg` / `.mp3` as
   unsupported instead of silently playing nothing. Compressed formats need a
   vendored decoder (stb_vorbis / dr_mp3) added to `engine/3rdparty/` first.
5. **Log category is `ZAudio`** (already added to
   `bq_log_category_config.ini` + regenerated `engine_log.h`).
6. Windows links `Mmdevapi` for WASAPI (see the `WIN32` block in
   `Runtime/CMakeLists.txt`). Other platforms get the null device; adding
   CoreAudio / AAudio means a new `AudioDevice*.cpp` plus a factory branch.

Known gaps (intentionally out of the first slice): streaming (clips are fully
decoded in memory), compressed formats, DSP effects / reverb sends, doppler,
OGG/MP3 import, an audio mixer editor window, and an audio importer in
`Editor/AssetPipeline/`.

### 2.22 Animation & skinning: conventions and current state

PR-SK1 landed the skeletal-skinning foundation
(`engine/Source/Runtime/Function/Animation/Skinning.{h,cpp}` + the DX12 GPU
path). Conventions that are easy to get wrong, in the order they bite:

1. **The GPU joint palette has an extra identity slot in front.** Slot 0 is the
   root; bone `i` lives at slot `i+1`; the shaders test `index > 0` and skip a
   zero index. `Skeleton::OutputAnimationResult()` writes `GetID() + 1`, and
   `AnimationSkinning::BuildGpuPaletteWithRootSlot()` is the CPU-side helper.
   Both `mesh.vert` (Vulkan) and `mesh.vert.hlsl` (DX12) rely on this.
2. **Clip channels are DELTAS on the bind pose, not absolute local transforms.**
   `Skeleton::ApplyAnimation()` resets to the bind pose every frame, then
   composes each sampled pose with `AnimationSkinning::ComposeDelta()`
   (position adds, rotation pre-multiplies, scale multiplies). Sampling returns
   absolute poses; never skip the compose step.
3. **Never trust `RawBone::tpose_matrix`.** Nothing in the codebase writes it
   (grep it), so the inverse bind matrices are always derived from
   `binding_pose` through the hierarchy
   (`AnimationSkinning::ComputeInverseBindMatrices()`). Trusting
   `tpose_matrix` silently renders the mesh unskinned.
4. **Skeletons are topologically sorted at build time.**
   `Skeleton::BuildSkeleton()` copies the asset's `bones_map` and runs
   `EnsureTopologicalOrder()`; it never mutates the shared cached `SkeletonData`
   and no longer bails out silently on unordered input (that failure mode used
   to show up as "the character does not animate at all" with no log line).
5. **DX12 storage-buffer SRVs need an explicit stride.**
   `RHIDescriptorBufferInfo::stride` was added for this: DX12 used to guess
   "80 bytes when the range divides by 80", which mis-sizes the 32-byte
   joint-binding buffer whenever the vertex count is a multiple of 5. Set it
   for every new structured buffer (joint palette = 64, RenderMeshInstance =
   80, joint binding = 32).
6. **Log category is `ZAnimation`.**

### 2.23 FBX import: what dropping a file produces (PR-SK2)

`AssetImportManager::FindImporter()` dispatches on **extension only**, so
`.fbx` is handled by `MeshImporter` alone -- a separate animation importer can
never be reached. One dropped `.fbx` therefore produces everything, Unity's
ModelImporter style:

```
Assets/Models/hero.fbx
  -> Assets/Models/hero.zasset                    MeshData (geometry + bind[])
  -> Assets/Models/hero.skeleton.zasset           SkeletonData   (bones, if any)
  -> Assets/Models/hero.anim.zasset               AnimationAsset (one per stack)
  -> Assets/Models/hero.animskelmap.zasset        AnimSkelMap    (channel -> bone)
```

Rules that keep the two importers from drifting apart:

1. **Bone numbering lives in `Editor/AssetPipeline/FbxSkeleton.h` and
   `FbxAnimation.h`** (header-only, guarded by `ZENGINE_HAS_UFBX`). The mesh
   importer uses it for `MeshData::bind`; the animation importer uses it for
   `AnimSkelMap::convert`. Never rebuild the bone list locally -- if the two
   disagree the character animates the wrong bones and nothing warns you.
2. **`MeshData::bind` stores GPU palette slots (bone + 1)**, matching
   `AnimationLoader::addBoneBind()` and the `index > 0` test in both mesh
   shaders. Build them with
   `AnimationSkinning::PackVertexInfluence()` (top-4 + renormalise).
3. **Skinned meshes keep their authored coordinates.** `RecenterMeshAtOrigin()`
   and `ApplyAxisFlip()` are skipped when the file has bones -- moving vertices
   without moving the skeleton slides the mesh out of its own bind pose.
4. **Clip channels are baked DELTAS** (`FbxAnimation::BakeStack()`), sampled at
   the file's own FPS unless `AnimationImporterSettings::sample_rate` overrides
   it; `max_frames` + a 10k hard cap bound asset size.
5. `AnimationAsset::skeleton_file_path` is intentionally left empty at import
   time; it is filled when a clip is wired to a mesh's skeleton in the editor.

### 2.25 Animator: state machine (PR-SK3)

`AnimationComponent` used to mix a fixed clip list at a static weight, always
looping, with no speed and no events -- a character could not react to gameplay.
The state machine adds that, Unity's AnimatorController shape adapted to what
the engine already has:

```
AnimatorController  (asset: states + parameters + transitions + events)
        |
AnimatorInstance    (runtime: parameter values, per-state playhead, active fade)
        |  GetBlendState()  ->  at most 2 clips with weights (1-w, w)
        v
AnimationSystem::GetBlendStateWithClipData()  ->  Skeleton::ApplyAnimation()
```

Rules that are easy to break:

1. **The evaluator does not sample poses.** `AnimatorInstance` emits a
   `BlendState` and hands it to the existing mixer, so there is exactly one
   sampler in the engine. Never add a second sampling path.
2. **A state owns ONE clip, addressed by PATH** (`.zasset`), same as the
   existing `BlendState`. Not a `PPtr` -- PPtr round-tripping is still being
   rolled out (PR-SE3a).
3. **`AnimatorState::clip_length` is in SECONDS and is required.**
   `AnimationClip` only stores `total_frame` and the bake FPS is not persisted,
   so seconds cannot be derived. `<= 0` means "hold at frame 0".
4. **Conditions are evaluated AFTER candidates are collected and sorted by
   priority**, because a satisfied Trigger condition CONSUMES the trigger.
   Evaluating every transition first would let a losing one eat it.
5. **Transitions are not re-evaluated during a fade** (no interruption source,
   matching Unity's default). `Play()` / `CrossFade()` are the escape hatch.
6. **Enums are persisted as `int`** -- the serializer has no enum trait. Cast at
   the boundary; do not "fix" this by adding enum support to the serializer.
7. **Events fire once per pass**; the per-state fired flags reset when the
   playhead wraps or is seeked backwards.
8. `AnimationComponentRes::controller_file_path` is OPTIONAL and empty in every
   pre-existing asset: empty == legacy blend-state playback. This is a
   SafeBinaryRead schema evolution (PR-SE3a pattern) -- do not make it
   mandatory.

**Authoring loop**: `ZSlateAnimatorWindow` (dock title "Animator", Window menu,
category Animation) is the authoring surface -- parameters / states /
transitions as three editable tables, plus New / Open / Save against a
`<Project>/Assets/<name>.controller.zasset` path. A controller asset in the
Content Browser also offers "Open in Animator" on right-click
(`ContentBrowserHelpers::IsAnimatorControllerNode` matches the
`*.controller.zasset` suffix). Attaching it to a character is a plain string
field: `AnimationComponentRes::controller_file_path` shows up in the Inspector
through the generic TypeTree drawer (it is an `eastl::string`, which maps to
`FieldKind::Str`) -- type the path there.

Editing rules for the panel (easy to break):

1. **Transitions address states BY INDEX**; conditions address parameters BY
   NAME. Hence `RemoveState` must drop every transition touching the removed
   state AND renumber the rest, and `RemoveParameter` must drop conditions that
   named it (they would otherwise never fire).
2. **Text commits must not rebuild the chrome tree** -- a rebuild replaces the
   `SEditableTextBox` and drops keyboard focus. Only structural edits / enum
   cycles set `m_UiDirty`.
3. Numeric fields are `SEditableTextBox` + tolerant parse: a malformed value
   keeps the previous one instead of writing NaN into the asset.

**Graph view** (the default tab): one hand-painted node per state, arrows for
transitions.
- Node positions live on the asset (`AnimatorState::pos_x` / `pos_y`) in
  LOGICAL units, multiplied by the UI scale at paint time, so a layout survives
  a DPI change. They were added after the fact, so a controller written before
  the graph existed has every state at (0,0) -- the window treats "all zero" as
  "not laid out yet" and auto-arranges on load.
- Gestures: drag a node body to move it, drag its **bottom port** onto another
  node to create a transition (duplicates are rejected; the new arrow is
  selected immediately). Click a node or an arrow to select it (mutually
  exclusive -- picking one clears the other); clicking empty canvas clears both.
  **Right-click an arrow deletes it** (the host has no `WasRightPressedThisFrame`,
  so the edge is latched from `IsRightDown()` into `m_RightWasDown`).
- The selected arrow opens an editing **strip under the canvas** with that
  transition's own rows: from/to, fade, exit time, priority, conditions. It
  reuses `BuildTransitionBody`, the same function the Transitions tab calls per
  row, so the two editors cannot drift apart.
- The selected **node** opens the same kind of strip (name, clip path, anim skel
  map, wrap cycler, speed, length, events, plus `Set Default` / `Delete` /
  `Deselect`), reusing `BuildStateBody` for the same reason. `AddState` selects
  the new node immediately -- an unselected node has nowhere to type its clip.
- `Delete` in the state strip is a button, NOT a right-click gesture: removing a
  state renumbers every transition that referenced it.
- **`Browse`** next to the clip and anim-skel-map fields opens a picker over the
  right pane. It browses the **Content Browser's own `EditorFileService` tree**
  (via `ZSlateContentBrowserWindow::GetInstance()->GetFileService()`), filtered
  to `m_AssetType == "animationasset"` (clip) / `"animskelmap"` (map). Two rules
  matter:
  1. It must **never call `BuildEngineFileTree()` itself** -- that clears the
     node array the browser holds raw pointers into (`m_SelectedNode`,
     `m_BrowsedFolderNode`) and leaves them dangling. Rebuilding is the
     browser's job (it rebinds its selection afterwards). While the tree is
     empty we scan the content root with `std::filesystem` instead.
  2. Picked paths are stored **content-root-relative** (`ToContentRelative`),
     the form `AssetManager::loadAsset` resolves, so a controller survives the
     project moving.
- The picker's filter box does not rebuild the tree on commit (that would drop
  keyboard focus); rows are filtered every frame by toggling
  `SWidget::Visibility` between `Visible` and `Collapsed`, which `SBoxPanel` and
  `SScrollBox` both honour in layout.
- **Canvas is also a drop target**: drag a `.zasset` out of the Content Browser
  onto a node -> `animationasset` becomes its clip, `animskelmap` its map; onto
  blank canvas an `animatorcontroller` opens. The hovered node is outlined cyan
  and a cursor-following label says what will land where (`-> clip` /
  `(unsupported here)`). While a drag is in flight the canvas' own gestures are
  off, so a drop can never also move the node.
- The type is read with `AssetManager::GetAssetTypeName` and normalized by
  `NormalizeAssetType` (lowercase, trailing "Res" stripped), because the ZAsset
  header sometimes names the Resource wrapper class.

### 2.26 Cross-window drag-drop (Content Browser -> canvas / Inspector)

- The **source side already existed**: Content Browser asset rows/tiles emit
  `FAssetDragDropOp` (`PayloadType = "AssetPath"`, `AssetPath` = ABSOLUTE path)
  through `ZSlate::SetActiveDragOperation`. Do not write a second payload type.
- `SlateInputRouter::ProcessMouse` already dispatches cross-window: it uses
  `m_DragOp ? m_DragOp : GetActiveDragOperation()`, so a window that did not
  start the gesture still delivers `OnDragOver` / `OnDrop` to **its own** widget
  tree. Widget drop targets use `SDropTarget` (`CanAcceptDrop` / `OnDropHandler`).
- **Two consumers exist for a reason — a hand-painted region cannot use
  `SDropTarget`.** The Animator canvas is painted with `ISlateRenderer`, so it
  has no widget to receive `OnDrop`; it caches the op and acts on its own
  release edge (the `ZSlateSceneWindow` pattern). The Inspector's Mesh /
  Material rows are widgets, so they use `SDropTarget`.
- **Race (this is why the router was patched)**: the publishing router clears
  the process-wide channel on its release frame, and window draw order decides
  whether the target ticks before or after that. `SlateInputRouter` therefore
  keeps `m_LastObservedDragOp` and delivers the drop from it; consumers of the
  global channel cache their own copy for the same reason. Detection must key on
  `IsLeftDown() || WasLeftReleasedThisFrame()` -- on the release frame
  `IsLeftDown()` is already false.
- **Path form**: a drag carries an ABSOLUTE path, but serialized fields store
  content-root-relative (what `InspectorAssetCommon::ResolveProjectAssetPath`
  resolves). All drop targets convert through
  `EditorDragDrop::MakeDroppedAssetStoredPath` so they cannot disagree.
- Inspector drops write the path into the text box and then call that box's own
  `OnTextCommitted`: dropping goes through exactly the same code path as typing,
  so validation and the read-only info labels cannot drift.

### 2.27 Animator as a drag SOURCE (clip -> Inspector component field)

- The **clip line inside a node** is the drag-out handle (`ClipChipRect`, shared
  by painting and hit-testing). Press there and move >5px -> the canvas publishes
  an `FAssetDragDropOp`. A press without travel still just selects the node.
- **The source must unpublish the op itself.** Normally the `SlateInputRouter`
  that started the gesture clears the channel; the canvas is hand-painted and has
  no widget (no `OnDragDetected`, no router), so `EndAssetDragOut()` owns it.
  Forget this and the channel stays hot -- every window then thinks a drag is in
  flight forever.
- Payloads carry ABSOLUTE paths; the controller stores content-root-relative, so
  the path is resolved on publish (same root as
  `MakeDroppedAssetStoredPath` uses in reverse).
- States with an empty `clip_file_path` have no handle: there is nothing to drag.
- **AnimationComponent now has a dedicated Inspector section** (Controller /
  Skeleton / Clip / Anim Skel Map / Clip Length). Without it the generic TypeTree
  walk renders `blend_state.blend_clip_file_path` as `<array> (not editable)` --
  exactly the slot a dragged clip has to land in.
- The slot accessors live on `AnimationComponent`, not in the Inspector: a
  `BlendState` is a set of PARALLEL vectors, a clip only counts as usable when
  its bone map is present, and `Tick()` bails out when length / ratio are missing
  (a "clip assigned" character would stand perfectly still). The component owns
  that invariant; callers must not hand-roll the vectors.
- Clip length is authored, not derived: a clip stores frames and the bake FPS is
  not persisted (see 2.25).

### 2.28 Any asset path row is a two-way slot (generalized drag-out)

- **Two kinds of drag SOURCE, and they are not interchangeable:**
  - *Widget rows* answer `SWidget::OnDragDetected`; the router that saw the press
    publishes the op AND clears it on release. Use `SDragSource` (the mirror of
    `SDropTarget`) via `EditorDragDrop::MakeAssetPathDragSource`.
  - *Hand-painted regions* (the Animator canvas) have no widget, so they publish
    with `SetActiveDragOperation` and **must unpublish themselves** -- see 2.27.
- `EditorDragDrop::MakeAssetDragOperation` is the only way to build the op:
  stored (content-root-relative) path -> ABSOLUTE payload path, and an empty path
  returns null so no gesture starts (otherwise the user drags "nothing" and every
  target lights up for a payload it cannot use). Its inverse is
  `MakeDroppedAssetStoredPath`; `MakeDroppedAssetAbsolutePath` is the other half.
- `MakeAssetPathDragSource` polls the value at drag start, so a path typed a
  second ago is the one that travels.
- Animator nodes now show **two** asset lines (clip, anim->skeleton map) and both
  are handles; `kNodeH` grew 56 -> 74 to fit them. `AssetSlotPath` /
  `AssetChipRect` are shared by painting, hit-testing and drag-out, so a handle
  can never carry something other than what it shows.
- The States tab and the in-canvas strip share `BuildStateBody`, so both got
  drag-out for free. Prefer that: one row builder, two call sites, no drift.
- Inspector path rows (`MakeAssetSlot`) are now **two-way**: drop target in, drag
  source out, on the same row.
- Caveat: `OnDragDetected` is polled over the whole press path, so a drag inside a
  focused text box beats selecting its text. Fine for path rows, wrong for prose.

### 2.29 Content Browser as a drop TARGET (move / copy)

- Three drop sites, one policy: **folder rows, asset tiles, and the list
  background** all route through `CanAcceptAssetDrop` / `HandleAssetDrop`. Rows
  and tiles are deeper than the background catch-all, and `OnDragOver` bubbles
  deepest -> root, so the specific row wins and the background only sees blank
  space. Never duplicate the policy per site.
- Ctrl decides move-vs-copy and is read at **drop** time (not at drag start): the
  user makes up their mind mid-drag, and `CanAcceptAssetDrop` is polled every
  frame so the highlight follows the modifier.
- The actual work lives in `ContentBrowserDragDrop::CanDropAssetPath` /
  `DropAssetPath`, and it is registry-aware:
  - a registry-indexed `.zasset` goes through `EditorAssetManager::MoveAsset`,
    which re-keys the registry + dependency graph and rolls the file back if the
    registry update fails. A bare `std::filesystem::rename` would leave the
    registry pointing at the old path.
  - everything else is a plain rename, falling back to copy-then-delete because
    `rename()` cannot cross volumes.
- **Copying a registry `.zasset` re-writes it, it is NOT a byte copy** (see 2.30):
  the GUID is derived from the file's absolute path and stamped into the binary
  header, so a byte copy would carry the SOURCE's GUID to the destination -- two
  files, one GUID, every dependency edge ambiguous. The copy goes through
  ReadObjects* -> WriteObjectsToDiskThreadSafe so it gets its own GUID.
  Non-registry files (shaders, json, ...) copy byte-for-byte, and Ctrl+drag in
  place is the duplicate gesture (`"name 1"`).
- A folder cannot be moved into itself or its own subtree; a move back into the
  folder it already lives in is a no-op and does not light the row up.
- After a successful drop: `RefreshContentBrowserSelection` (rebuild + select the
  new path) and `asset_tree_dirty = true` so the window rebuilds its widget tree.

### 2.30 Duplicating an asset = read the graph, write it out again

- **The identity that must change is the GUID, not the local file id.** A binary
  `.zasset` stamps `DeterministicGuidFromPath(path)` into its header and YAML
  authoring assets derive their GUID the same way, which is exactly why a byte
  copy is broken. Local ids are therefore kept as read: in-file `PPtr`s address
  objects by local id, so renumbering would need a full remap pass and buys
  nothing.
- The copy is safe because the write session calls `RegisterLocalObject` for every
  object it writes *before* any `PPtr` transfers: the graph is embedded in the
  copy, not turned into external references pointing back at the source.
  (The writer hook only fires for instances that are NOT in the write set.)
- Reader/writer pair is chosen by `AssetManager::IsYamlAuthoringAssetPath`
  (`.mat` / `.prefab` / `.scene` are YAML, imported `.zasset` is binary);
  `WriteObjectsToDiskThreadSafe` routes both.
- **Folder copies recurse file-by-file** through the same rule -- a recursive
  `std::filesystem::copy` would byte-copy every `.zasset` inside and produce a
  folder full of duplicate GUIDs.
- Call `EditorAssetManager::RefreshAsset` on the copy: the directory watcher would
  only pick it up on a later tick, and until then it is in no picker.
- `MoveAsset` declines when the destination is outside the registry (e.g. into
  Scripts/) and rolls back before returning false, so falling through to a plain
  rename is safe -- do not report that as a failure.
- Arrows are hit-tested as **fat segments** (point-to-segment distance), not
  rects, since they run at any angle; nodes are tested before arrows so an arrow
  passing under a node cannot steal its clicks. Painting and hit-testing both go
  through `GetTransitionEndpoints`, so they cannot disagree.
- A self-transition (A -> A) has no room for a real arrow: it is drawn as a short
  stub just under the node so it stays visible and clickable.
- Selecting anything rebuilds the chrome (the strip appears/disappears), so only
  a *changed* selection may set `m_UiDirty` -- see rule 2 below.
- The canvas is hand-painted into the rect a **placeholder `SBox`** was
  arranged into (`m_GraphSlot->GetCachedGeometry()`), so the painted region can
  never drift from the widget layout. Do not replace it with analytic maths.
- There is no `DrawLine`: an arrow is a 4-point `DrawConvexPoly` shaft plus a
  3-point head. Note `UIRenderer::DrawConvexPoly` takes the ENGINE `Vector2`,
  which is a different type from `ZSlate::Vector2` -- the graph paints through
  `ZSlate::ISlateRenderer` (which also has `DrawTextLabel`, `UIRenderer` does
  not).
- Node drags deliberately do NOT set `m_UiDirty`: repainting happens every
  frame, and a rebuild would drop a text box's keyboard focus.
- No pan / zoom yet; positions are clamped to the visible canvas.

Known gaps: no layers / blend trees / sub-state machines, no graph pan/zoom, the
Content Browser does not accept drops yet (a path cannot be dragged *into* it),
and no live preview of the driven character's parameters while editing.

### 2.33 RHI: one pipeline per call, and indirect draws

`engine/Source/Runtime/Function/Render/Interface/` — `RHI.h` is the abstract
interface, `RHIStruct.h` the plain data, and `Vulkan/ DX12/ Metal/ WebGL2/` the
four backends.

- **`CreateGraphicsPipelines` / `CreateComputePipelines` create ONE pipeline per
  call.** `pPipelines` is a single `RHIPipeline*`, and `RHIPipeline` is an
  **empty base class** (`class RHIPipeline {};`), so indexing it as an array
  would stride by `sizeof(RHIPipeline)` (== 1) instead of
  `sizeof(VulkanPipeline)` — N pipelines cannot be handed back through this
  out-parameter at all. The Vulkan backend used to forward `createInfoCount` to
  `vkCreate*Pipelines` while filling only one create-info struct and one output
  slot, which **read past the struct and wrote past the output** for any count
  > 1 (a stack overflow nobody hit because every caller passes 1). All backends
  now reject `createInfoCount != 1`. **Create N pipelines with N calls.**
- **`CmdDrawIndexedIndirect` / `CmdDrawIndirect`** (added 2026-10-21) are the
  primitive that makes GPU-driven geometry possible: a compute pass writes the
  arguments into an `RHI_BUFFER_USAGE_INDIRECT_BUFFER_BIT` buffer and the CPU
  never learns how many draws there were. Per-backend caveats:
  - Vulkan: `multiDrawIndirect` is optional in Vulkan 1.0, so it is probed in
    `CreateLogicalDevice` and cached in `m_MultiDrawIndirectSupported`; a batch
    is clamped to 1 with a warning rather than silently truncated. Requesting
    an unsupported feature makes `vkCreateDevice` fail outright — probe, never
    assume.
  - DX12: `ExecuteIndirect` bakes the stride into the command signature at
    creation, so it cannot follow a per-call stride the way Vulkan's can; a
    mismatched stride is refused rather than reinterpreted.
  - Metal: there is **no** multi-draw indirect, so a batch is a loop of single
    draws at `offset + i * stride`.
  - WebGL2: no indirect draw exists; silent no-op (Lumen/Nanite are not
    compiled for Emscripten anyway).
- **`RHIDrawIndexedIndirectCommand` / `RHIDrawIndirectCommand`** are the shared
  argument layout. Their size and field offsets are the ones
  `VkDraw*IndirectCommand`, `D3D12_DRAW_*_ARGUMENTS` and
  `MTLDraw*PrimitivesIndirectArguments` all already agree on — keep them that
  way. `RHIIndirectDrawSmokeTest` pins them with `static_assert` and a stride
  walk, and it matters more than it looks: **on Apple neither
  `Interface/Vulkan` nor `Interface/DX12` is compiled**, so it is the only
  thing that catches a field drifting apart there.

### 2.34 Lumen: where distance-field geometry comes from

`engine/Source/Runtime/Function/Render/Lumen/` — CPU-side distance field
(`LumenDistanceField`). Nothing here runs on the GPU yet.

- **`RenderMeshData` dies right after the GPU upload.** It is passed **by value**
  into `RenderResource::UploadGameObjectRenderResource` and `BufferData::m_Data`
  is `free()`d in the destructor. Anything that needs vertex positions later
  (Lumen, CPU occlusion, ...) must read them from
  **`RenderResourceBase::CpuMeshGeometry`**, keyed by
  `RenderEntity::m_MeshAssetId`. It retains only local-space positions and
  indices widened to `uint32_t` (the asset form is `uint16_t`); do not add
  normals/UVs back — 12 + 4 bytes per vertex/index is the whole point.
  `CacheMeshGeometry` is idempotent and is called from *both*
  `RenderResource::GetOrCreateMesh` implementations (Vulkan/Metal in
  `RenderResource.cpp`, DX12 in `RenderResourceDx12.cpp`) — add a new upload
  path and you must call it there too.
- **`SceneGeometryCache` stores geometry once per mesh asset, not per instance.**
  `positions` / `indices` are the concatenated local-space geometry; `ranges`,
  `transforms` and `bounds` are parallel arrays of length = instance count, and
  several instances legitimately share one `MeshRange`. Do not try to derive a
  range from an instance index — that is exactly why `MeshRange` exists.
- **`RenderEntity::m_BoundingBox` is LOCAL space.** The visibility code
  (`RenderScene::UpdateVisibleObjects*`) transforms it by `m_ModelMatrix` before
  comparing against a frustum; the distance field does the same via
  `BoundingBoxTransform`. `bounds` in the cache is the world-space result.
- **`geometry_hash` is the "did the scene change" gate.**
  `LumenRenderer::Update` calls `UpdateDistanceField` **every frame**, so
  `GenerateFromScene` short-circuits when the hash and the world bounds are
  unchanged. `markForUpdate()` (`needs_update`) is the force-rebuild switch.
- **The voxelizer is CPU brute force, so resolution is capped** at
  `LumenConstants::kMaxCpuDistanceFieldResolution` (64) inside `Initialize`.
  The default config asks for 256 → 16.7M voxels. Lower the cap's relevance by
  moving the voxelization to a compute shader; do not just raise the number.
- **`DistanceToTriangle` needs `edge · (v0 - point)`, not `edge · (point - v0)`.**
  This is Ericson RTCD 5.1.5. Flipping the sign sends every region test down the
  wrong branch and a point *inside* the triangle comes back as the distance to
  `v0`. It shipped wrong and nothing noticed because no caller ever reached it
  with real geometry. `LumenSceneGeometrySmokeTest` pins it.

### 2.35 Lumen: software ray tracing and the resolution limit of the field

`LumenRayTracing` is a CPU sphere-tracer over `LumenDistanceField`. Everything
about it has to be scaled by the voxel size — **never write a distance constant
in this subsystem**.

- **The field has no value below half a voxel diagonal.** `QueryDistance`
  returns the distance stored at the enclosing *voxel centre*, so the smallest
  value any query can produce is ~½ voxel diagonal. A hit test like
  `distance < 0.01f` is therefore unreachable: every ray burns all
  `kMaxSoftwareTraceSteps` and reports a miss. The threshold is
  `max(min_trace_step, GetVoxelDiagonal() * 0.5f)`. Same reasoning applies to
  the normal gradient epsilon in `QueryNearestSurface` — a hard-coded `0.1`
  put all three samples in one voxel, the gradient became zero and
  `normalisedCopy()` produced NaN.
- **`QueryDistance` returns `FLT_MAX` outside `world_bounds`, and that means "no
  data", not "very far".** Marching must clip the ray to the world box first
  (`ClipRayToAxisAlignedBox`), otherwise an origin outside the box drifts at
  maximum step size and never reaches anything. Treat `FLT_MAX` as
  "terminate", not as a large distance.
- **`TraceRaySoftware` is argument-first, member-second.** The
  `LumenDistanceField*` argument wins; `nullptr` falls back to
  `setDistanceField()`. Do not reintroduce an early `return` on `nullptr` —
  that is exactly the bug that made all three call sites (GI x2, reflections x1)
  trace against nothing. An unloaded field is rejected up front.
- **Shading origins must be lifted off their own surface.** GI and reflections
  start on a surface where the field reads ~0, so the first step "hits" the
  surface being shaded. Both offset along the shading normal by one voxel
  diagonal (see `LumenGlobalIllumination::OffsetOriginFromSurface` /
  `LumenReflections::OffsetOriginFromSurface`). If you add another ray origin
  on a surface, do the same.
- `hit_distance` is refined by `kTraceBinaryRefinementSteps` bisections so it
  lands on the surface crossing rather than a voxel away. More iterations than
  ~8 are pointless: the field itself is not that accurate.

### 2.36 Lumen surface cache: pages, lookup, and why the page key is gone

`LumenSurfaceCacheManager` — one page per scene instance (`RenderEntity`),
covering its world AABB, carrying its constant material.

- **Lookup is spatial, never a hash of position floats.** The old key,
  `(uint64_t(x*1000) << 32) | (uint64_t(y*1000) << 16) | uint64_t(z*1000)`, is
  broken three ways: `x` overflows the field it shifts into, `y`'s field overlaps
  `x`'s low bits, `z` is never shifted — and a negative coordinate cast to
  `uint64_t` is UB. Any new "world position -> bucket" key must be a proper
  quantised cell index, not float arithmetic packed into an integer.
- **A page registers into every grid cell its AABB covers**, and the cell size is
  the largest page extent, so registration is bounded at 2×2×2 = 8 cells per
  page. If you change the cell size rule, keep that bound or add a cap.
- **Page boxes are grown by a margin** (2 % of the scene's largest extent)
  because the distance field stops rays about half a voxel *off* the surface and
  a flat quad's AABB has zero thickness. Lookup falls back to "nearest page
  centre" for the same reason — do not drop that fallback.
- **`GetSurfaceInfo` is intentionally non-const**: a hit refreshes the page's LRU
  timestamp, which is the only signal eviction has. Do not make it const for
  convenience; `LumenRayTracing`'s methods can stay const because they reach the
  cache through a pointer.
- **Rebuild is gated on a scene hash** (instance id + mesh/material asset id +
  model matrix + material factors) *and* on `active_page_count == 0`. The second
  condition is what stops an evicted cache from staying empty forever.
- **Per-texel arrays stay empty on the CPU path** (`page.resolution == 0`): there
  is no card rasterizer, so 64×64 × 7 arrays × 1024 pages would be several
  hundred MB holding one repeated constant. Allocate them only when a
  rasterizer actually fills them.
- Material comes from `RenderEntity`'s factors; textures cannot be sampled on the
  CPU, so a page has **one** albedo/roughness/metallic/emissive today.

### 2.37 Lumen GI: the probe grid, and why it must not follow the camera

`LumenGlobalIllumination` produces a world-space irradiance probe grid
(`LumenProbeGrid`, `LumenTypes.h`) — that grid **is** the GI output; everything
downstream reads it.

- **The grid is anchored to the distance field's world box, not the camera.** A
  camera-anchored grid would be rebuilt whenever the camera moves, discarding
  every probe's history — and history is the entire point of temporal
  accumulation. `ResizeProbeGrid` deliberately keeps the existing probes when
  neither the box nor the resolution changed. The camera only filters *which*
  probes get this frame's budget (`gi_probe_max_distance`).
- **Updates are budgeted and rotate** (`gi_probe_update_budget`, `m_ProbeCursor`):
  one probe costs `gi_sample_count` rays, so updating the whole grid in one frame
  is not affordable. Per-axis resolution is clamped to
  `LumenConstants::kMaxGiProbeResolution` for the same reason.
- **`last_updated_frame == 0` means "no data", and that is not the same as "the
  value is zero".** A probe boxed in by closed geometry legitimately holds black.
  `SampleProbeGrid` gates on the timestamp, never on the length of the value —
  otherwise every query inside a dark volume re-traces forever.
- **`GetIndirectLighting` is intentionally non-const**: on a cache miss it traces
  and writes the result back ("query and fill"). The write path only *fills*
  empty probes (`dt <= 0` ⇒ alpha 0 ⇒ filled probes keep their value), so it can
  never fight the per-frame accumulation in `UpdateProbes`.
- **Probes store one directionless `Vector3`** (whole-sphere average of
  `emissive + albedo * 0.1`), so interpolation does not weight by normal. When
  this grows into SH or a six-face irradiance volume, `SpatialReuse` is where the
  normal starts to matter.
- **`gi_temporal_blend_rate = 0` means freeze, not "instant"** — see
  `ComputeTemporalAlpha`, which returns 0 there.

### 2.38 Lumen 的接线：Z_HAS_LUMEN、UpdateLumen 和 RenderScene 上的发布点

- **`Z_HAS_LUMEN` 才是"Lumen 在不在"的开关**，不是 `Z_HAS_VULKAN`。Lumen 的 .cpp
  只在两处被剥掉（Emscripten、`WIN32 AND NOT ZENGINE_USE_VULKAN`，见
  `Source/Runtime/CMakeLists.txt`），而 Apple 编 Metal、没有 Vulkan、却**编 Lumen**
  —— 以前把 `m_LumenPass` 放在 `#if defined(Z_HAS_VULKAN)` 里，Apple 永远拿不到它。
  所有引用 `LumenRenderPass` / `LumenSettings` 的管线/控制台代码都必须包在
  `Z_HAS_LUMEN` 里。
- **GI 在帧里的位置是"场景更新之后、PreparePassData 之前"**（`RenderLumenCmd`）。
  GI 读场景实体，必须等可见性更新完；它算出来的间接光又必须早于本帧的 GPU 资源
  上传，否则进不了 per-frame uniform。
- **Lumen 是纯 CPU 的**，所以 `LumenRenderPass::Initialize` 只需要 `render_resource`
  （CPU 网格几何的唯一来源），**不要**再拿 `m_Rhi` 去 gate 初始化。
- Lumen 的结果通过 `RenderScene::m_IndirectLighting`（POD `SceneIndirectLighting`，
  在 `RenderCommon.h`，**刻意不依赖任何 Lumen 头**）发布。渲染器够不到渲染管线，
  只有场景和相机，所以发布点只能在场景上。
- **发布的是"权重 + 颜色"，不是"有没有值"**：全黑的场景（被封闭几何体包住）算出来
  的辐照度本来就是 0，那是有数据。关掉 Lumen 时权重归零，让延迟光照退回 baked
  irradiance cubemap，而不是留下上一帧的陈旧颜色。
- `r.Lumen.*` 是**热生效**的：`Render()` 每帧调 `LumenRenderer::ApplyConfig()`，把
  配置推给五个子系统而**不**重新 Initialize —— 只有这样才能保住 probe 网格的历史
  和表面缓存的页面。结构性字段（`distance_field_resolution` /
  `surface_cache_resolution`）不在热改范围内，改它们要重建资源。

### 2.39 Lumen 进画面：per-frame uniform 桥，以及"哪份 UpdatePerFrameBuffer 是真身"

- **`UpdatePerFrameBuffer` 有两份互斥的真身**，而且都不是 Apple 的：
  `Function/Render/RenderResource.cpp` 的函数体整个包在 `#if defined(Z_HAS_VULKAN)`
  里（5–1420 行），`Function/Render/RenderResourceDx12.cpp` 包在
  `#if defined(_WIN32)` + `#if !defined(Z_HAS_VULKAN)` 里。**Apple（Metal）和 Web
  根本没有实现** —— 冒烟测试里那个 `RenderResource::UpdatePerFrameBuffer` 桩是会
  `abort()` 的。往这两个文件里加 per-frame 字段必须**两边都加**，否则只有一半平台
  有数据。
- 这是阶段 27 那个 `Z_HAS_VULKAN` / `Z_HAS_LUMEN` 错误的翻版：**"Lumen 在不在"和
  "per-frame buffer 有没有人填"是两个不同的开关**。填 per-frame 的代码按 Vulkan /
  DX12 切分，跟 Lumen 按 Emscripten / Win-non-Vulkan 切分，两者交集为空的平台上
  （Apple）就是"Lumen 编了但结果没人搬"。
- 打包逻辑因此收敛到 **`SceneIndirectLighting::ToUniform()`**（`RenderCommon.h`）：
  两份 `UpdatePerFrameBuffer` 都调它。这样既不会手抄漂移，也让
  `LumenPerFrameBridgeSmokeTest` 有一个**所有平台都能链接**的断言对象 —— 直接调
  `UpdatePerFrameBuffer` 的测试在 Apple 上只能链到 abort 桩。
- **`MainCameraPerFrame` 有三份手工维护的镜像**（C++ `RenderCommon.h`、GLSL
  `shader/include/structures.h`、HLSL `shader/hlsl/common/scene_lighting_structs.hlsli`）。
  只在一边加字段，后面每个 per-frame 常量都会错位 16 字节，而 shader 在本机从不编译，
  没有别的东西能发现它。新字段必须追加在**末尾**，并同步三处 + 两个 bare-name
  accessor（`main_camera_per_frame_access.{inl,hlsli}`）。
- **间接光是"替换"不是"相加"**：`mesh_lighting.inl` 用
  `mix(irradiance, lumen_indirect.rgb, lumen_indirect.a)`、`deferred_lighting.frag.hlsl`
  用 `lerp(ambient_light, ...)`。两者都是辐照度，加起来会把天光算两遍。权重 0 时的
  行为必须和加字段之前**逐字节一致**。

### 2.40 Lumen 反射：从零调用者到进 shader

- **`LumenRenderer::ComputeReflections()` 曾经是个空 TODO**，所以
  `LumenReflections::ComputeReflection()` 零调用者、`LumenReflectionData` 从不离开
  `Function/Render/Lumen/`。这是"代码看着像真的但从没跑过"的典型 —— 一跑就暴露三个 bug：
  ①`BlendReflections` 用 `max_trace_distance`（**常量**）挑分支，`<100` 且 `roughness<0.3`
  时选中 SSR，而 SSR 是恒空 TODO ⇒ 已追到的命中被换成黑色。现在按"哪份真的命中"加权。
  ②`ComputeReflection` 算出的镜像方向被 `BlendReflections` 的返回值整体覆盖掉（它从不
  设这个字段）。③粗糙度 TODO 没实现，粗糙面仍打单条镜面光线，再被 `(1-roughness)` 压暗
  ⇒ `roughness=1` 时恒为纯黑。现在粗糙度是**把圆锥张开、多条光线取平均**，辐射本身不衰减
  （粗糙度是 shader 里 BRDF 波瓣的事，两边都算就重复了）。
- **"有没有命中"必须有独立的 `valid` 字段**，不能靠颜色判断：命中一个纯黑表面本来就是黑的。
  这和 GI 的"用 `last_updated_frame` 而不是用值非零判断有没有数据"是同一条教训。
- **反射的采样点是表面缓存的页面**（自带 `world_position` + `normal`，天然就是着色点），
  所以反射依赖表面缓存而不只是距离场 ⇒ `UpdateSurfaceCache` 的开关条件必须带上
  `enable_reflections`，否则"GI 关、反射开"时反射一个采样点都拿不到。
- 累积按 **`instance_id` 做键**，不按页面下标：页面会因重建/驱逐换槽位，换槽位不该丢历史。
- `lumen_indirect` 与 `lumen_reflection` 是**两个独立权重**（GI 和反射是两个 CVar）。
  发布失败时只清自己那两个字段，不要整份 `SceneIndirectLighting` 清掉。
- **发布的是场景平均**，不是逐像素反射：CPU 既没有屏幕探针也没有 GBuffer，拿不到逐像素的
  着色点和视线方向。它在 shader 里的角色是粗糙的 specular 环境项。延迟通道（HLSL）今天
  没有 specular IBL 项，所以只有前向 GLSL 路径消费它。

### 2.41 Nanite：集群构建器、层次树，以及"没有简化器"意味着什么

`engine/Source/Runtime/Function/Render/Nanite/` —— 阶段 30/31 之前**全仓库零外部调用者**，
整个子目录（构建器 / 裁剪 / 渲染器 / `NaniteSystem`）都没被任何地方调用过，所以下面的 bug
从没被执行过。修的时候记住：**"看着像真的"和"跑过"是两回事**。阶段 32 起 Nanite 有了一条
真实的帧内路径（`RenderNaniteCmd` → `RenderPipelineBase::UpdateNanite` →
`NaniteRenderPass::Render`），但它**仍然一行都不跑在 GPU 上**。

- **构建器以前产出为空，不是"粗一档"而是零字节**：`BuildClustersFromMesh` 在开头
  `clear()` 掉 `vertex_data`/`index_data` 之后**再也没人写过** ⇒ `NaniteMeshResource`
  带着一堆正确的 offset/count 和**零几何字节**出门。现在几何真写进去了（未压缩，
  布局写在 `NaniteTypes.h` 的 `NaniteMeshResource` 上）。
- **递归触顶不能丢三角形**：以前 `depth >= kMaxHierarchyDepth` 直接 `return`，那一整棵
  子树的三角形被静默丢掉。触顶要**降级成叶子**（可以超过 `kMaxTrianglesPerCluster`），
  不能什么都不产出。
- **层次树要按"两次递归各自的返回值"认领直接子节点**，不要按"子树在数组里的区间"回填父
  指针 —— 区间里还包含更深层的节点，它们的父指针已被更深的那一层写好，按区间回填会把
  它们全部覆盖，整棵树退化成"所有节点挂同一个父"。内部节点**后序**创建（子节点建完才
  建自己），几何区间取 `[进入时的 positions.size(), 递归结束后的 size())`，深度优先保证
  它连续。
- **`lod_level` 的 0 是根**（覆盖整个网格、最粗），数值越大越细。内部节点 `is_leaf =
  false`，**只有叶子能被提交渲染**：没有网格简化器，粗一档并不会少画三角形，内部节点
  存在的意义只是让层次结构与逐层误差可查询。
- **`error_metric` 必须被赋值且单调（父 ≥ 子）**。以前它**从来没人赋值**，而
  `NaniteCluster` 又没有默认成员初始化 ⇒ `SelectLOD` 读到的是未初始化内存。值取 `radius`
  （空间二分下随细分单调变小）；原来那个 `radius * (1 + lod_level * 0.1)` 方向是反的，
  会让**越细的簇误差越大**。退化成一个点的簇半径要抬到 `kMinClusterSize`，否则凡是拿它
  当分母/基准的计算都失去意义。
- **输入要在边界就拒**：`indices.size() % 3 != 0` 和越界索引以前都会一路走到
  `positions[indices[i]]` 读越界。
- **Nanite 从不被剥出 ZRuntime**（不像 Lumen 那两处 `list(FILTER ... EXCLUDE)`），所以
  `Nanite/Test/` 的冒烟测试在 `smoke_tests.yml` 里是**开着**的，`ZENGINE_USE_VULKAN=OFF`
  也连得上。
- **裁剪返回值的约定统一为"true = 被剔除"**。以前 `FrustumCull` 返回"可见"、
  同文件的 `OcclusionCull` 返回"被剔除"，两个相反约定只能靠试调用点才能发现。
- **`ExtractFrustumPlanes` 必须用行主序**：本仓库 `Matrix4x4` 是 `m_Mat[row][col]`、
  `clip = M * v`（`operator*(Vector4)`：clip.x = 第 0 行点乘 v，clip.w = 第 3 行点乘 v）。
  所以六个平面是 **row3 ± row_i**（Left/Right=±row0，Bottom/Top=±row1，Near/Far=±row2）。
  抄 Ogre 那套（把 `m[i][j]` 当 `m[col][row]`、向量写左边）会得到六个完全不相干的平面。
- **`NaniteInstance::local_bounds` 是模型空间盒**，由 `NaniteRenderer::RegisterInstance`
  从网格资源补。包围盒**无效时不剔除**（不知道边界就裁掉，比不裁更糟）—— 所以"全都
  没被裁"是注册路径出问题的症状，不是裁剪失效。
- **`SelectLOD` 的方向**：屏幕误差（error_metric/距离）**小 ⇒ 可以粗一档（往 0/根走）**，
  **大 ⇒ 需要更细（往叶子走）**。距离为 0 直接返回自己的层级（除零会得到 inf）。
- **没有网格简化器 ⇒ 没有"粗一档但三角形更少"**。所以：内部节点永不提交（几何区间只是
  子树聚合），唯一站得住的 LOD 是**丢掉投影误差低于预算的簇**（留下的洞是亚像素的）。
  别指望 LOD 减少远处网格的三角形数 —— 那需要简化器。
- `PrepareRenderData` 与 `BuildRenderBatches` 以前往**同一个** vector 里写、后者一进来就
  `clear()` ⇒ 前者的产物每帧被抹掉。批次不产出空批次（`cluster_count == 0` 曾经是唯一产出）。
- **接进帧循环的三件套**（阶段 32，照 Lumen 的先例）：`RenderPipelineBase::UpdateNanite`
  虚函数 + `m_NanitePass` 槽位（槽位静态类型是 `RenderPassBase`，用 `dynamic_cast` 取
  `NaniteRenderPass*`，基类不该知道 Nanite）；`RenderSystemFrameCommands.cpp` 里的
  `RenderNaniteCmd`（紧跟 `RenderLumenCmd`，在 `RenderPreparePassDataCmd` 之前）；
  `RenderSystem.h` 里的 `friend struct RenderNaniteCmd;`。
  **Nanite 不像 Lumen 那样有 `Z_HAS_NANITE` 宏** —— 它从不从 ZRuntime 剥离，所以
  `RenderPipelineBase.cpp` 里那个 include 没有 `#if`。
- **`DesktopRenderPipelineModule::Setup` 在 Apple 上会早退**（`#if defined(__APPLE__) return`）。
  要在早退**之前**建 `m_NanitePass`，否则 Apple 上通道根本不存在（Lumen 当年就踩过这个：
  它原来被塞在 `Z_HAS_VULKAN` 块里）。
- **`NaniteRenderer::UpdateFromScene` 用"场景拓扑哈希"决定是否重建**：哈希只含
  `(instance_id, mesh_asset_id)` 对，**不含变换矩阵**（矩阵每帧都可能变，变了不需要重建
  集群），也**不含没有 CPU 几何的实体**（否则"某资产晚一帧上传完"会被当成拓扑变化）。
  哈希要**用 FNV offset basis 起头**，不能从 0 开始 —— 否则首帧会被误判成"场景没变"。
- **`clear()` 必须把拓扑哈希清回 0**：实例和集群都没了，哈希还匹配的话下次
  `UpdateFromScene` 会走"只更新变换"分支，实例列表永久是空的，Nanite 静默失效。
- **关掉 Nanite 时要清掉上一帧的批次**：`NaniteRenderer::Render` 在 `!config.enabled`
  时调 `ClearFrameResults()`。批次挂在渲染器上，不清的话下游（和未来的编辑器/统计）会一直
  读到最后一次启用时的陈旧结果。
- `NaniteInstance` 的 `instance_id` / `mesh_id` / `transform` / `prev_transform` **以前没有
  默认成员初始化**，默认构造出来的是垃圾（除了那条零调用者的手工注册路径）。
  现在都补上了。
- 下一个缺口（还没做）：`NaniteRenderPass` 的终点仍然只是"产出渲染批次" —— `UpdateGPUBuffers`
  是 TODO，`NaniteHierarchicalZBuffer` 全是 TODO（所以遮挡剔除恒 false，是有意的），
  **没有 Nanite shader、也没有间接绘制**，批次产出来还没有人消费。

### 2.42 GPU-Spine 进场景：`SpineRenderer` 组件 + `.zspg` 资产

- **资产**：`.zspg`（SpineCook 产物，`ZEditor -batchmode -suite=SpineCook <骨架> -o <Assets 下路径>`，
  会把图集页拷到 .zspg 旁边）由 `AssetManager::SpineSkeletonAssetExtension()` 登记进
  `RegistryIndexedAssetExtensions()` ⇒ 注册表扫描 / 文件监视 / Content Browser 自动认。它**没有 ZASS 头**：
  类型名 `SpineSkeletonData` 只看魔数 'ZSPG'，GUID 按路径派生（与 YAML 资产同一分支）。
  **绝不能走 SerializedFile / ReadObject**；不显示 Reimport（不是导入器产物）。
- **组件**：`Runtime/Function/Framework/Component/Spine/SpineRenderer.{h,cpp}`，字段存**路径**（同 MeshRenderer）。
  拖 `.zspg` 进 Scene ⇒ `EditorScenePlacement` 按扩展名分派 ⇒ `SpineRenderer::SpawnInLevel`（控制台
  `spine.spawn <zspg> [x y z | camera <dist>] [scale] [anim]` 共用；不给坐标 = 场景相机前方并正对相机）。
  编辑模式 tick 已注册（`registerEdtorTickComponent`）。
- **朝向**：`m_Billboard`（`SpineBillboardMode`，默认 `full`）。`none` = 骨架立在 GameObject 局部 **XZ 平面**
  （Z 向上，骨架 x → +X、y → +Z，同 UE Paper2D；组件乘 `Rx(+90°)·diag(m_Scale)`）；`full` = 平行相机平面；
  `vertical` = 只绕世界 Z 转（人物直立）。公告板在**渲染线程**按当帧那台相机算（view 矩阵第 0/1 行 = 右/上，
  编辑器相机和游戏相机通吃；`RenderCamera::right()` 对游戏相机不准）。组件只给枢轴 + 带符号缩放（lossy x / z）。
- **渲染（两层）**：控制台实例 = UI 层（`Draw`，无深度、不经 tonemap）；世界实例 = **RP1 forward / 透明 subpass**
  （`DrawWorld`：Vulkan 在 `MainCameraPass::DrawRp1` 粒子之后，DX12 / Apple 在 `MainCameraRp1Pass::DrawForwardLightingPass`
  axis 之后）—— 深度测试 LEQUAL、不写深度、实例按观察空间 z 远→近排；`clip_from_world = proj·view·model` 与
  `mesh.vert` **同一个乘法、同一个视口**（不另翻 y，否则深度对不上）；片元 bit5 把 sRGB 转线性（场景线性 HDR，
  随后 tonemap + gamma）。**两层的每帧缓冲 / 合包 / 管线各一份**（同帧两时刻录制，共用会互相覆盖）；
  延迟销毁倒计时与资产回收只在 UI 层跑（每帧一次、且在世界层之后）。`DrawWorld` 每帧至多一次（只画一个视口）。
  组件只持有世界实例 id，每 tick 推 local-to-world × `m_Scale`、公告板参数与 dt。**三条生命期规则，改的时候别破**：
  1. 实例**只在 Tick 里建**，不在 PostLoadResource：放置流程的模板 GameObject 上的组件永不 tick，在那建会留孤儿；
  2. 存活判据 = **逻辑帧序号**（`WorldManager::Tick` 开头 `AdvanceWorldTick()`）：本帧 / 上一帧没被推过变换的不画
     ⇒ disabled / 非激活 / 已不在关卡里自动隐藏，暂停时逻辑帧不走 ⇒ 定格。**不要靠组件析构**：删 GameObject
     时组件对象不析构（撤销要留着它，`GameObject::~GameObject` 也不删组件）；
  3. `RenderScene::DeleteEntityByGObjectID` 顺带 `RemoveWorldInstancesOfOwner`；组件下一次 tick 若
     `RequestSetWorldTransform` 返回 false（id 不认识）就重建 ⇒ 撤销删除自动复活。
- `spine.load` / `spine.clear` 只动控制台实例，不清世界实例。
- 颜色：世界层经 tonemap（Vulkan Uncharted2 ×4.5 / DX12 ACES），与透明网格一致，**不再等于** spine 编辑器里的颜色；
  要"不受后处理"的话用控制台实例那层或以后加开关。
- **加载只有一条路**：`Render/Spine/SpineGpuAssetFile.{h,cpp}`（`SpineGpuLoadAssetFile` / `SpineGpuCookSkeleton` /
  `SpineGpuGetAssetInfo`）。渲染器、SpineCook 套件、自动 cook、组件的元数据查询都调它；**别再在别处手写"烘焙 → 读"**。
  `SpineGpuCookedPathFor("X.skel.bytes") = X.zspg`（不是 `X.skel.zspg`）。渲染器的资产缓存按 (路径, mtime) 认，
  `SpineGpuGetAssetInfo` 按 (路径, mtime, size) 缓存。
- **自动 cook**（`Editor/AssetPipeline/SpineImporter/SpineAutoCook`）：启动时扫 content 根（`TextureImporter` 之后），
  `.skel.bytes / .skel / .json`（有同名图集）比 mtime 过期就 cook 到同目录 `.zspg`；`EditorAssetManager` 另起一个
  `m_SpineSourceWatcher` 监视源 + 图集（300 ms 去抖），写盘走 `.zspg.tmp` → rename。写完调
  `SpineRenderer::NotifyAssetChanged(abs)`（只能在游戏线程调：`g_asset_generation` 无锁）⇒ 用它的组件下一 tick 重建实例。
- **GUID 引用**：组件同时存 `m_SkeletonDataAsset`（路径）和 `m_SkeletonDataGuid`；`.zspg` 的 GUID 是路径派生的，
  所以 **`AssetRegistry::RefreshAsset` / 启动重扫必须保留已有条目的 GUID**（改名时 registry 把 GUID 挪到新路径，
  重新派生就丢了）。建实例时路径失效 ⇒ `TryGetPathForGuid` 重定向并改写路径字段。设路径一律走
  `SetSkeletonDataAsset()`（它顺带记 GUID），别直接写字段。
- **Inspector**：`ZSlateInspectorWindow::BuildSpineRendererSection` —— `.zspg` 拖放槽 + 状态行（含 `N event(s)`）+
  GUID 行 + Animation / Initial Skin / Face Camera 下拉（选项来自 `GetAssetInfo()`）+ Events 行（列出该动画的
  用户事件名），其余字段仍走通用 `BuildFieldNode`。下拉在无头 e2e 里没法点 ⇒ `g_combo_hooks` 登记 label，
  控制台 `inspector.pick <Label> <option>`（label 里的空格写成 `_`）/ `inspector.combos` 列出全部；
  每次 pick 各自成一步撤销。`obj select <id|name>` 选中对象（配合上面两条做无头 e2e）。
- **运行时 API（TS 可调，ZFUNCTION）**：`SpineRenderer.Of(go)`（TS 里 `GetComponent` 静态类型是 `Component`，用它）、
  `SetAnimation / AddAnimation / SetEmptyAnimation / AddEmptyAnimation / ClearTrack(s) / SetMix / SetSkin / SetTimeScale`、
  `GetCurrentAnimation / GetCurrentTrackTime`、`GetAnimation*/GetSkin*` 元数据、本帧事件 `GetEventCount / Type / Animation /
  Track / TrackTime`、**用户事件字段** `GetEventName / GetEventInt / GetEventFloat / GetEventString /
  GetEventAudioPath / GetEventVolume / GetEventBalance / GetEventTime`（只在 `Type == event` 时有意义）、
  `SetEventThreshold(track, th)`（混合期是否派发旧动画事件）、`GetUserEventCount / GetUserEventName`（资产级事件表）。
  实例没建好之前的调用**进队**（`Submit`，上限 64），建实例后按序补发 ⇒ TS 在 OnStart 里调是安全的。
  C++ 侧用 `SetEventListener`。
- **事件**：`SpineGpuAnimationState` 的 `m_Queue` + `Drain()` 逐条复刻 spine-cpp 3.8 EventQueue（start / interrupt / end /
  complete / dispose / **event**）。世界实例在渲染线程解算 ⇒ 事件在 `WorldEntry::events` 里攒（上限 1024，溢出计数），
  组件 tick 时 `DrainWorldEvents` 取走 ⇒ **比解算晚 1~N 帧**。
  **v16 起用户事件真的进容器**（规格 §16）：chunk 23 = `SpineGpuEventData`(28 B) + 自带字符串池（第 0 条恒空串），
  事件时间轴 type 7 的帧载荷 = `SpineGpuEventKey`(24 B)；**动画 duration 现在把最后一个事件的 time 算进去**
  （v15 及以前不烘事件 ⇒ 少算，complete 时机错）。派发规则照抄参考：同 time 多帧取**第一帧**、
  `queueEvents` 按 complete 切成两段、**mixingFrom 只在 `mix < eventThreshold` 时收集且只在 `to->mixDuration > 0`
  时排队**。`eventThreshold` 按 (instance, track) 走 `SpineGpuRenderer::RequestEventThresholdOn()`（作用在轨道尾 entry）。
  对拍：`SpineGpuRefParityTest` S5 逐帧比事件序列 + 用户事件字段（OSGame 全量 111,266 条一致，
  其中用户事件 4,458 条；v15 基线是 101,369 条 / 用户事件 0 —— 多出来的 5,439 条调度事件
  正是 duration 修正的产物：complete +1,957、start +1,043、end +879、dispose +893、interrupt +667）；
  `SpineGpuImportTest` 另有手算夹具 `SpineGpuEventFixture.h`（45 条检查）；改调度逻辑后必须重跑。
  改格式要连带升 `kFormatVersion`（§11 的"静默错"六连坑）。
- **控制台**：`spine.spawn … script=Scripts/X.ts`（顺手挂 TypeScriptComponent）、`exec.after <frames> <cmd>`（延迟执行，
  配合 `--cvar` 写无人值守 e2e）、`asset.move <old> <new>`（content 根相对，测 GUID 重定向）、
  `obj select <id|name>`、`inspector.pick <Label> <option>` / `inspector.combos`。
- **性能基准**：`SpineGpuBenchmark`（`Render/Spine/Test/`，`EXCLUDE_FROM_ALL`、**不进 ctest**、不链 `ZRuntime`）：
  `--config Release`，`--frames 360 --csv <文件> [--top N] [--limit N] [--verbose] [--split] <Assets 目录>`，
  与参考 spine-cpp 的 CPU 蒙皮路径逐段比（anim / pose / geo / upload / CPU 顶点）。`--split` 把 anim
  再拆成 Update / Apply 并报每帧时间轴条数（两边同一切法）⇒ 定位"慢在哪半边"先加它。最新版本数字与
  逐类型分解见规格 `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` §17 / §17.2。
  ⚠️ anim 至今比参考慢 ~1.4x，**根源就一件事：deform**（两侧同时消融实测占差距 **100.2%**；
  容器内只存增量 ⇒ 每 float 约 13 次访存，参考加载期已摊稠密、约 3 次）。其余类型净贡献 ≈ 0。
  想动它先看 §17.4 那张"贡献"表和四个方案（**P1 融合快路径已做并实测否决：访存 8.01 → 3.18
  次/float 却慢 1.7~3.5%，见 §17.5**；P4 载入期稠密化实测要 96 MB，不划算）。
  §17.2 那张表是**单侧**消融，只能说明"deform 最贵"，说不出它占差距多少。
  ⚠️ **deform 的逐元素循环必须带 `__restrict`**（`ExpandDeformFrame` 的 setup 累加、
  `ApplyDeform` 主路径的插值循环）：这几块 `float*` 互不相交但编译器证明不了，
  不写这句 MSVC 不向量化 ⇒ 实测差 3.0~4.7%（anim 0.70x → 0.82x）。
  **这条适用于全工程**：两块指针参与的逐元素循环，先想 restrict。
  ⚠️ **全模块已扫完一遍（§17.6）**：剩下能加的（末帧分支、`alpha != 1` 分支、`time < FirstTime()`、
  `ClipTriangles`）加起来只值 **~0.3%**，在噪声边缘 —— 别再指望这条线出量。
  **循环体内有 `break` 之类边界检查的不用加**（`PoseSolver` 的 path / mesh 顶点循环）：
  MSVC 本来就不向量化，加了也白加。
  ⚠️ **加 `__restrict` 时的两个野指针坑**：① 指针**必须在最后一次可能重分配的操作之后**取 ——
  `last` / `prev` / `next` 是 `ExpandDeformFrame` 里 `full.assign()` 的目标，容量不够就重分配，
  提前取会悬垂（稳态"看起来没问题"正是最难复现的那类 bug）；② 同一个对象**只能有一份**
  `__restrict` 指针，别在外层声明一份、内层块里又声明一份指向同一块内存。
  ⚠️ **低于 ~2% 的效果只有"同一份二进制 + 环境变量开关 + 交错跑"才量得出来**：绝对值漂 50%，
  两边一起漂；跨会话拿数字对比（甚至改前改后各跑 3 次）都是噪声。消融估的值偏大 ~2 倍
  （rigid setup memcpy 估 0.20 µs/帧、交错 A/B 实测 ~1%）⇒ 消融只用来找"有没有"，定收益要交错跑。
  见规格 §17.3。
  ⚠️ `SpineReference` 按 config 改名（`OUTPUT_NAME SpineReference_$<CONFIG>`）：全工程
  `CMAKE_ARCHIVE_OUTPUT_DIRECTORY` 被统一设成同一目录（根 `CMakeLists.txt:56-60`），不改名的话
  Release 的基准会把 Debug 的 `.lib` 覆盖掉，回来编对拍就 **LNK2038**（`_ITERATOR_DEBUG_LEVEL` 不匹配）。
- **热路径 0 分配**：`SpineGpuHotLoopTest`（`Render/Spine/Test/`，**进 ctest**，同样不链 `ZRuntime`）——
  断言"稳态每帧 0 次堆分配"：动画+骨架、动画+骨架+裁剪（三角形）都必须是 0，四边形裁剪每帧 ≤ 2 次。
  谁在热路径里加回一个局部 `std::vector` / 每帧拼串，这条就红。三种修法固定为：
  ① 烘焙期展开派生缓存（曲线表 `SpineGpuBuildCurveCache`，缓存缺失**报错**不许悄悄退化）；
  ② `thread_local` scratch（deform 展开、裁剪几何）；
  ③ 状态对象的成员 scratch 跨帧复用（`SpineGpuTriangulator`，配合裸指针避免每帧拷贝）。
  ⚠️ **复用 scratch 的入口第一行必须 `clear()`** —— 漏了就是上一帧的残留漏进这一帧（对拍报
  `clipStart 凸块数 我们 2 vs 参考 1`）。
- **容器里的东西能直接读就别拷**：rigid 的 setup 顶点在 `asset.vertex_buffer` 的 rigid 段里本来就是
  连续的 `[x0,y0,x1,y1,…]`（8 B/顶点）⇒ `ApplyDeform` 用 `RigidSetupAt()` **直接把 `setup` 指进去**，
  不 `memcpy` 到 scratch。前提是三条（字节偏移 4 对齐 / 指针真的 4 对齐 / 区间整个在 rigid 段内），
  由 `SpineGpuHotLoopTest` section 3 钉住；坏了会**静默**读错数，只有对拍会红。区间校验留在热路径。
  ⚠️ 该目标整目标以 `_ITERATOR_DEBUG_LEVEL=0` 编译：MSVC 的 Debug 迭代器调试给每个容器构造发一个
  16 字节 `_Container_proxy`（8 次/帧），不是数据缓冲，但会把这条断言打成假阳性。
  基准新加的分配计数同理 —— **只有 Release 的数字能读**。
- **ZFUNCTION 静态函数**：pesapi 的实例方法与静态函数是**两张表**（`set_property_info_size(method_count, function_count, …)`
  各自从 0 编号）；混一起编号 ⇒ 第一个静态函数越界写 `Functions[]`，编辑器启动即崩。见 `ScriptBinding.cpp`。
- **`Component::m_ParentObject` 必须有初值 `{nullptr}`**（曾经没有）：组件 New 出来到 `PostLoadResource` 之间，
  `TypeScriptComponent::SetScriptGuid` 之类的 setter 用 `GetParentObject() != nullptr` 判断"已挂上"，读到堆垃圾就去
  `BindAndAwake → GetTransform` 崩，**是否复现取决于堆布局**（spine.spawn 带 script= 只在启动时 cook 过才崩）。
  崩溃栈在 `bin/<Config>/crash_stack.txt`（`InstallCrashHandler`，已符号化）—— 先看它，别二分。
- 抓帧验证：`renderdoc.capture [delay_frames] [noui]`（Vulkan 现在也能抓：in-app API 传 (NULL, NULL) 通配），
  `renderdoccmd thumb` 出缩略图；要看中间 RT / 逐像素深度测试结果用 `qrenderdoc.exe --python <脚本>`（自带 python36，
  `pyrenderdoc.LoadCapture` + `Replay().BlockInvoke`）。DX12 的 PushEvent 在抓帧里没有名字，按 eid 区间找。

### 2.32 Undo/Redo: two layers, and you record BEFORE you edit

`engine/Source/Editor/Undo/` (`doc/UNDO_SYSTEM.md`).

- **Two layers, one owner.** `UndoSystem.{h,cpp}` is the transaction stack and
  depends on **nothing but the standard library** -- that is deliberate, it is
  what makes `UndoSystemSmokeTest` run headless. Everything that knows about
  GameObjects / selection / the level lives in `EditorUndo.{h,cpp}`
  (`EditorUndoSystem`, an `IEngineSystem` in the PostInit phase, the single
  owner of one `UndoSystem`) and in `EditorUndoRecords.{h,cpp}`. Do not leak
  engine types into `UndoSystem.h`.
- **Records are swap-based.** `IUndoRecord::Apply()` exchanges the state it
  stores with the live state and must be its own inverse, so one implementation
  serves both undo and redo and never needs to know the direction. UE's
  `FTransaction` model. `Finalize()` runs once when the transaction closes and
  returns false for "nothing changed" -- the record is then dropped, so
  committing the same value never pollutes the history.
- **Record BEFORE the mutation, never after.** Call sites use the
  `EditorUndo::` facade: `RecordComponent` / `RecordTransform` /
  `RegisterCreatedGameObject` / `DestroyGameObjects`, optionally wrapped in
  `EditorUndo::ScopedTransaction("Title")` (RAII, has `Cancel()`). Without a
  scope, everything one event handler records lands in the *implicit*
  transaction that `EditorUndoSystem::Tick()` flushes once per frame -- Unity's
  per-event undo group.
- **Resolve targets by InstanceID, never by raw pointer.** Records go through
  `ObjectManager::IDToPointer`; between undo and redo the object is very likely
  at a different address. `GameObjectLifetimeRecord` therefore *owns* the
  removed subtree, so undo restores the same objects with the same IDs (and
  every `PPtr` to them stays valid) instead of clones.
- **Component snapshots go through the normal `Transfer()` path** into a YAML
  **DOM** (not emitted text) with `ScopedPPtrResolver(nullptr)` pushed, which
  makes `PPtr` / `ImmediatePtr` serialise raw instance IDs. That is why undo is
  bit-exact for floats and why anything that survives a scene save/load
  survives undo. A `Transform` is the exception: it is routed to
  `RecordTransform`, because a raw snapshot would overwrite its hierarchy links.
- **Deleting objects must go through `EditorUndo::DestroyGameObjects`,** not
  `Level::DeleteGObjectByID` directly -- otherwise there is no undo step (and no
  `CancelDeleteGameObject` on the render swap).
- **A long drag is one step.** The gizmo opens the transaction lazily on first
  move and `EditorSceneManager::EndGizmoUndo()` closes it when the mouse is
  released; `RecordTransform` coalesces by instance ID inside the open
  transaction, so a 300-frame drag is still one Ctrl+Z.
- **Not recorded:** play mode (recording is suspended, matching Unity/UE),
  asset edits (Inspectors write straight to disk), component add/remove (no
  editor UI for it yet).
- **New editor entry points must add a `Record*` call** before they write.
  Current ones: gizmo `MoveEntity`, `OnDeleteSelectedGObject`,
  `ZSlateInspectorWindow` (every field commit), `CreateEmpty`,
  `EditorHierarchyReparent`, `EditorScenePlacement`.
- **Prefab overrides have their own two records** (both offered by the Inspector
  before it writes, keyed so they coalesce within one transaction):
  `PrefabOverrideModel::MakeOverrideListUndoRecord` for the property override
  list, and `MakeStructureUndoRecord` for the component STRUCTURE -- the
  component list of every GameObject the instance owns, plus the added / removed
  lists and the corresponding-source map. The structure one backs **Revert All**
  and a component row's **Revert** (an added component is detached, a suppressed
  one comes back as a clone). Undo/redo never destroys a component: a detached
  one stays parked and the record re-attaches the very same instance.
  Structural **Apply** is deliberately NOT recorded -- it rewrites the asset on
  disk, and asset edits are outside the undo stack.
- Entry points for the user: Ctrl/Cmd+Z, Ctrl+Y, Ctrl+Shift+Z
  (`EditorInputManager::OnKey`), the Edit menu (`MenuController`), and the
  console (`undo [steps]` / `redo [steps]` / `undo.history [rows]` /
  `undo.clear`). New `ZUndo` log category -- see 2.5.

### 2.31 RenderGraph: three phases, and the compiler must stay RHI-free

`engine/Source/Runtime/Function/Render/Graph/` (`doc/RENDER_GRAPH.md`).

- **Setup / Compile / Execute.** Setup (`RenderGraphBuilder`) only *declares* reads and
  writes -- no RHI call, no allocation; it is cheap enough to run every frame. Compile
  is pure CPU (topological sort, culling, lifetimes, memory aliasing, barriers,
  validation) and deterministic. Execute (`RenderGraphExecutor`) resolves handles to
  real resources and records commands.
- **The compiler depends on nothing but the standard library.** `RenderGraphTypes.h`
  forward-declares `RHIImage` / `RHIImageView` / `RHIBuffer` / `RHICommandBuffer` as
  opaque pointers and mirrors the enums as `RGFormat` / `RGTextureUsage` /
  `RGResourceState`. Do **not** include `Interface/RHI.h` in `RenderGraphTypes.h`,
  `RenderGraphBuilder.*` or `RenderGraph.*` -- that is what lets
  `RenderGraphSmokeTest` run headless. `RenderGraphResources.*` is the one place that
  knows the RHI.
- **Declare before you touch.** A pass's setup callback declares the "before" state;
  the mutation happens in the execute callback. Same rule as the editor undo system
  (2.32).
- **Roots are what survive culling:** a pass is kept if it writes an imported
  (external) resource, writes an extracted resource, or carries
  `RGPassFlags::NeverCull`. Everything else that nobody consumes disappears. A pass
  that only reads and writes nothing *is* culled -- that is correct, not a bug.
- **A resource read and written by the same pass is rejected** (no RW textures), as is
  declaring one with two different states. Both surface as `RGCompiledGraph::errors`,
  never as UB.
- **Never rely on pipeline stage masks for correctness.** DX12's
  `CmdPipelineBarrier` drops `srcStageMask` / `dstStageMask` entirely and degrades
  every buffer/UAV barrier to `D3D12_RESOURCE_BARRIER_TYPE_UAV`
  (`DX12RHI.cpp:5346`). The stage in `RGMapState` is a hint for Vulkan only.
- **RHI destroy/free take `T*&`** (they null the caller's pointer), so values copied
  out of a `const` reference must go through a mutable local first.
- **The Execute half talks to `IRGResourceDevice`, not to `RHI`.** The pool and the
  executor need exactly ten backend operations (create/destroy image, view, buffer,
  memory; `CmdPipelineBarrier`; `PushEvent`/`PopEvent`). `RGRhiDeviceAdapter` is the
  production bridge to `RHI`; a ten-method stub is what makes
  `RenderGraphExecuteSmokeTest` possible. Do **not** widen this port casually -- every
  method added here is one more thing a test has to fake. `RenderGraphExecutor` still
  takes an `RHI*`, but only to hand to pass callbacks via
  `RenderGraphExecuteContext::GetRHI()`; the executor itself never calls it, so tests
  may pass `nullptr`.
- **Extracted resources outlive the graph.** `KeepAlive` means the caller reads the
  resource after `Execute()` returns, so the executor parks it in `m_Extracted*` and
  only returns it to the pool at the start of the *next* `Execute()` (or an explicit
  `ReleaseExtracted()`). Never release one into the idle pool during the frame that
  produced it -- that is exactly what the compiler's "extracted resources do not
  participate in aliasing" rule is protecting.
- **Resources whose producer was culled are never allocated.** Culling is a producer
  closure from the roots, so if a surviving pass read a resource, its producer would
  have been marked. `lifetimes[i].first_pass == kRGInvalidIndex` identifies the rest;
  allocating them anyway is how a culled debug pass would still reserve a full-screen
  render target.
- **First target is the DX12 RP2 post-process chain** (`MainCameraRp2Pass::DrawRP2`,
  `MainCameraRp2Pass.cpp:611`): ColorGrading -> (FXAA) -> CombineUI, already three
  independent Begin/EndRenderPass, at the very end of the frame, off the geometry path.
  **It is wired, behind a switch.** The graph path is opt-in via `ZENGINE_RG_RP2=1`
  (`SetupRenderGraphPath()`); when it is off not one graph object is constructed and the
  hand-written path is byte-for-byte what it was. `DrawRP2WithGraph()` validates
  *before recording anything* (frame index, `images[i]`/`imageViews[i]` -- WebGL2/Metal
  publish no image and fall back with a one-time warning, not an error -- backup
  textures), then compiles; an invalid `Compile()` logs each error once
  (`m_RgFallbackLogged`) and returns false so the hand-written path draws the frame.
  Fallback is safe because `Execute()` can only fail before it records. `DesktopRenderPipelineModule`
  and `MainCameraPass::Draw(...)` are still untouched.
  **Still needs per-frame visual verification** -- run with the switch on and compare
  against the old path; do not claim it correct from the headless tests alone.
- **`PickPass` is not a candidate**, despite looking self-contained: `Draw()` is empty
  and the real work is in `Pick()`, a click-driven synchronous operation with its own
  command buffer, fence, submit and full `WaitForFences`. Its pixel readback is outside
  the ten-method `IRGResourceDevice` port, and its `RHIFramebuffer` is pinned to one
  `RHIImageView*` that a pool would invalidate.
- **Blockers before RP2 can be wired** (details in `doc/RENDER_GRAPH.md` §5). Two of the
  three are cleared: (1) `RHISwapChainDesc` now carries `images` alongside `imageViews`,
  populated by both `CreateSwapchainImageViews()` implementations -- DX12 wraps
  `m_RenderTargets[i]` in a `DX12Image` (ComPtr add-ref, the swapchain still owns the
  memory), Vulkan wraps `m_SwapchainImages[i]` in the non-owning `VulkanImage`;
  (2) DX12's swapchain state machine is no longer a second truth -- the wrappers alias
  `m_SwapchainResourceStates[i]` through `DX12Image::setExternalStateStorage()`, so
  `TransitionSwapchainBuffer()` and the graph's `TransitionImage()` read and write the
  same `D3D12_RESOURCE_STATES`. `SubmitRendering` now transitions back to PRESENT from
  any state, not just RENDER_TARGET, because the graph may legitimately leave the
  back-buffer in PIXEL_SHADER_RESOURCE. (3) `UpdateDescriptorBindings()` now runs inside
  the `ColorGrading` and `CombineUI` pass callbacks, not at the top of `DrawRP2()`
  (omitting it is a documented black screen); a culled pass therefore allocates no
  handles at all.
- **Import the state the texture is actually in, not the state you wish it were.**
  RP2's import states are `ShaderResource` for backup_odd/backup_even (RP2's HDR pass
  ends in `finalLayout = SHADER_READ_ONLY_OPTIMAL`) and `RenderTarget` for the
  back-buffer -- **not** `Present`, because `PrepareBeforePass()` has already
  transitioned and cleared it at the top of the frame. Get this wrong and the graph
  emits a barrier from a state that was never true; get it right and `CombineUI` emits
  no back-buffer barrier at all, which is what the test asserts.
- Tests: `-DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON` -> `RenderGraphSmokeTest`
  (165 checks, compiler: 78 generic + 43 on the RP2 chain + 15 on the swapchain
  round-trip + 29 on the wiring's real import states) and
  `-DZENGINE_BUILD_RENDERGRAPH_EXECUTE_SMOKE_TEST=ON` ->
  `RenderGraphExecuteSmokeTest` (92 checks, pool + executor). Both `main()`s are carved
  out of the ZRuntime glob in `Runtime/CMakeLists.txt`; the compiler files *and*
  `RenderGraphResources.cpp` are part of ZRuntime, so changing them must keep the
  whole-archive build green. Both are registered with CTest and both are listed in
  `.github/workflows/smoke_tests.yml` -- a new smoke test must be added to that file's
  Configure flag list *and* its target list, or it does not run in CI.

### 2.24 Packaging: `.zbundle` + manifest (PR-PAK1)

There was no pak, no manifest and no dependency pruning, so "shipping" meant
copying `Assets/` whole. `AssetBundle.h` was a 16-line stub whose `GetImpl()`
body was `return nullptr`. Now:

```
<project>/Build/<Platform>/
├── <project>.manifest.zasset   AssetBundleManifest (bundles + deps)
└── Bundles/<name>.zbundle      archive of cooked .zasset blobs
```

Rules that are easy to break:

1. **A bundle stores cooked `.zasset` FILE BYTES, not re-serialised objects.**
   There is exactly one binary writer/reader in the engine (`SerializedFile`);
   inventing a second one for bundles would guarantee drift. Assets are
   addressed by REGISTRY-RELATIVE path with forward slashes
   (`"Models/hero.zasset"`) -- the same key `AssetRegistry` uses.
2. **Bundle serving hangs off ONE hook: `SerializedFile::InitializeRead()`.**
   It asks `AssetBundleManager` first and, on a hit, reads the blob into
   `SerializedFile::m_OwnedMemory` and builds a `MemoryCacheReader` over it.
   That is what makes `loadAsset<T>()`, PPtr resolution and `Level::load` work
   unchanged in a player build with no `Assets/` tree. Do not add a second
   lookup path.
3. **Bundle wins over disk.** A mounted bundle shadows an on-disk file of the
   same path. That is deliberate (mounting is an explicit act) but it means
   never mount bundles in a normal editing session.
4. **Cached blobs must outlive readers.** `MemoryCacheReader` holds its buffer
   BY REFERENCE. `AssetBundle::GetAssetBytes()` caches per asset and never
   moves or frees it until `Unload()`, so unmount only after dropping the
   assets (`AssetManager::UnloadStream` per asset).
5. **The manifest is a plain `.zasset`** (`AssetBundleManifest`), not JSON:
   ZRuntime has no general-purpose JSON writer. It carries `platform`,
   `content_root`, and per bundle: `asset_paths` + `dependencies`.
6. **Bundle assignment**: one bundle per root (auto-detected `LevelRes` /
   `WorldRes` / `PrefabAsset` / `SceneRes`, or an explicit list); anything
   reachable from >=2 roots goes to a single `shared` bundle, so loading one
   root needs at most one extra bundle. Bundle dependencies are derived from
   the asset graph, not guessed.
7. **Log category is `ZPak`.**

**The builder is `Editor/Build/AssetBundleBuilder.{h,cpp}`** (added
2026-10-20; before that the two entry points below referenced a class that had
never been committed, which is why ZEditor stopped compiling). `Build()` never
throws or asserts -- it runs on the editor UI thread from a menu item, so every
failure comes back through `AssetBundleBuildResult::error`. Steps:
1. Roots: `options.roots` if set, else every `LevelRes` / `WorldRes` /
   `PrefabAsset` the registry indexes. `PrefabAsset` has no `Res` suffix.
2. If the registry is still scanning, **wait** (`WaitForScanComplete`) --
   packing against a half-built index silently omits assets, which is the one
   failure mode invisible until runtime.
3. Optional `cook_first` -> `TextureImporter::CookProjectTextures(target)`.
4. BFS per root over `AssetRegistry::GetDependencies`; `>=2` roots -> `shared`.
5. Bytes: `<Cooked>/<Platform>/<rel>` when it exists, else the source file.
   Entries are sorted by path so the same content yields the same bytes (a CI
   size check would otherwise flap on unordered-map iteration order).
6. `<output>/Bundles/<name>.zbundle` + `<output>/<project>.manifest.zasset`.
   Because the bundles sit in `Bundles/`, a player mounts
   `MountDirectory(<Build>/<Platform>/Bundles, <Build>/<Platform>/<project>.manifest.zasset)`
   -- `MountDirectory` only globs `*.zbundle` inside the dir it is given, so
   passing the Build root instead finds nothing.

Entry points: Build menu (`Build Asset Bundles (...)`) and the console
command `bundle.build <standalone|android|ios|ohos|webgl>`. Both now actually
run.

- **`.gitignore` line 1 is `build/`, which matches `Build/` case-insensitively
  on macOS and Windows.** `Editor/Build/` was therefore invisible to `git add`
  until `.gitignore` got `!engine/Source/Editor/Build` (same fix as the
  pre-existing `!engine/3rdparty/JoltPhysics/Build`). Do not "fix" a missing
  file in that directory by `git add -f` -- the next file added there would be
  dropped the same way. A `!` rule has to name the *directory*: git does not
  descend into an excluded directory, so a negation for a file inside it can
  never re-include it.

Runtime resolution (added 2026-10-20, PR-PAK1 follow-ups):

- **GUID -> path comes from the mounted bundles at runtime.** The runtime ships
  no `AssetRegistry`, so `AssetBundleManager` indexes every entry GUID on mount
  (`TryResolveGuid`) and `AssetManager::TryGetPathForGuid` asks it instead of
  returning false. `EnsureBundleMountedForAsset()` also mounts a *sibling*
  bundle on demand by looking the asset up in the manifest's `asset_paths` --
  `FileIdentifier` carries a GUID and a path but never a bundle name, so
  without it a player would have to mount everything up front.
- **"Not on disk" is not "missing" in a player.** Three places used to draw
  that conclusion (`ResolveExternalAssetPath`, `ReadObject(instanceID)`,
  `RenderResourceBase::TryLoadCookedTexture`) and each one silently dropped
  content that lived only inside a bundle. They now ask `AssetBundleManager`
  first. Keep that shape: in a packaged build there is no `Assets/` tree, so an
  `exists()` gate is a bug, not a guard.
- **`AssetBundleSmokeTest` runs on macOS now.** It could not link there (same
  DX12-only symbols as ZEditor); `AssetBundle/Test/CMakeLists.txt` now adds
  `SmokeTestAppleLinkStubs.cpp` on APPLE, exactly like `Core/Serialize/Test`.
  It also used to crash at the first `MemoryManager::DestroyObject`:
  `ObjectManager::Produce` allocates with plain `new`, while
  `MemoryManager::DestroyObject` frees through mimalloc, and on Apple the
  global `operator new/delete` is deliberately NOT routed to mimalloc (see
  `MemoryManager.cpp`) -- the free faulted inside `mi_usable_size`. Objects
  from `Produce` must be released with `delete` (the test's
  `DestroyProduced()`); objects from `MemoryManager::CreateObject` with
  `MemoryManager::DestroyObject`. Never mix the two.

**The player bootstrap is `ZGame`** (added 2026-10-20), and it is
unconditional there -- this IS the player:
`MountPackagedContent()` -> `AssetBundleManager::MountBuild(project_info->GetBuildRoot())`,
placed between `START_SYSTEM()` and `OpenStartupLevel()` because a level is
just another asset. `MountBuild` = `FindPackagedBuild` + `MountDirectory`.
It is deliberately NOT called anywhere else: in the editor a mounted bundle
shadows real files (rule 3).
`FindPackagedBuild` **locates** rather than constructs the platform directory:
`<Build>/<Platform>/Bundles/` plus the `*.manifest.zasset` next to it. The
platform name comes from `TextureImporterSettings::BuildTarget`, an
editor-only enum a player cannot compute -- do not "simplify" this into
`Build/Standalone`. 0 mounted means "launched from the source tree", which is
a normal outcome, not an error.

**Assembly is `install()` + CPack** (added 2026-10-20), at the bottom of
`engine/CMakeLists.txt`:

```
cmake --install <build> --prefix <dir>     # <dir> is the shippable tree
cpack -C Release -G ZIP                    # -G TGZ on Linux/macOS
```

```
<dir>/
├── ZGame(.exe)      player
├── ZEditor.ini      DEPLOYMENT profile
├── asset/           default textures + global render/particle resources
├── resource/        icons + font
└── Project/<name>/  optional (ZENGINE_PACKAGE_PROJECT)
```

- **`CMAKE_INSTALL_PREFIX` is NOT a usable packaging prefix.** The root
  `CMakeLists.txt` already points it at `<repo>/bin` so every target lands in
  one output directory. `cmake --install` must be given `--prefix` explicitly.
  CPack is unaffected -- it installs into its own staging directory.
- **The ini has to sit next to the executable.** `CommandSystem` derives the
  config path from the executable's own directory, and a packaged player is
  started from an arbitrary CWD. Shipping the ini somewhere else is the kind of
  bug that only shows up after packaging.
- **`ZENGINE_PACKAGE_PROJECT`** copies project content into `Project/<name>/`,
  excluding `Intermediate/Cooked`, `Intermediate/DDC`, `Intermediate/Shaders`
  and `Logs` -- build caches, not runtime data. `Intermediate/Scripts` is
  deliberately kept as a FALLBACK: compiled JS is packed into the `scripts`
  bundle (below), but a player pointed at a build produced before that existed
  still finds loose `.js` there.
- Shader sources are not shipped: they come from the project's own
  `GetShadersRoot()`, and the engine's generated SPIR-V/DXIL headers are
  compiled into the binary.
- The whole block is wrapped in `if(TARGET ZGame)`: **`ZGame` is not built on
  macOS** (the desktop branch of `engine/CMakeLists.txt` adds only Editor
  there), and `install(TARGETS)` naming a missing target is a fatal configure
  error, not a warning.

Related: `CommandSystem::Initialize()` reads the real command line on
Linux/POSIX (`/proc/self/cmdline` + `/proc/self/exe`) instead of synthesising
`argc=1` and deriving the executable path from the CWD. The fake argv
discarded every argument and left a launched player unable to find its own
`ZEditor.ini`. `argv_ptrs` is filled **once, after** the platform `#endif` --
it holds `data()` pointers into `argv_strings`, so any later `push_back`
invalidates the earlier entries.

**Compiled scripts are packed too** (added 2026-10-20). A `.js` is not a
registry asset: `ScriptAsset` names a MODULE (`"player/Mover"`, no extension)
and the file lives in `<project>/Intermediate/Scripts/`, outside the content
directory `AssetRegistry` indexes -- so the dependency walk can never reach it.

- Entry key: `AssetBundles::MakeScriptEntryPath(module_id)` ->
  `"Intermediate/Scripts/player/Mover.js"`, a reserved prefix. `Intermediate/`
  cannot collide with a registry asset because the registry indexes the content
  directory only and `Intermediate/` is its peer at the project root. Both
  directions live in `AssetBundleFormat.{h,cpp}` -- do not re-derive the prefix
  at a call site.
- They go into their own `scripts` bundle, and **every other bundle declares
  `scripts` as a dependency**: mounting one level must be enough to run the
  behaviours hanging off it. An empty scripts bundle is erased rather than
  written -- `MountBundle` treats a missing dependency as a hard failure.
- Runtime: `AssetBundleManager::TryReadScriptModule()` =
  `EnsureBundleMountedForAsset` + `TryReadAssetBytes`, and
  `ScriptEnv::LoadModuleInternal` asks it FIRST and falls back to `fopen`.
  False is the normal editing case (the editor mounts nothing), not an error.
- `AssetBundleBuildResult::script_count` counts modules separately from
  `asset_count`, which stays "what the dependency walk found".

**Packaging is incremental** (added 2026-10-21): both the cook and the
`.zbundle` write are skipped when nothing that goes into them changed. The
shared machinery is `Editor/Build/BuildCache.{h,cpp}`.

- **The verdict is a content hash (FNV-1a 64), never a timestamp.** (size,
  mtime) is only a FAST PATH: unchanged stamp => reuse the hash recorded last
  time without reading the file; changed stamp => read and re-hash, and if the
  bytes turn out to be identical after all (`git checkout` moved mtimes) the
  hash still matches and the artifact is still skipped. The one hole is a file
  whose bytes changed while size AND mtime stayed identical -- that needs a
  deliberately rewritten timestamp, and `force_rebuild` covers it.
- **A bundle's fingerprint** = builder version + platform + compression switch
  + the bundle's dependency list + every entry's (path, guid, type, content
  hash). The dependency list is in there on purpose: a bundle that *gains* a
  dependency (shared content appeared, script modules arrived) has to be
  rewritten even though none of its own bytes changed.
- **A cook's fingerprint** = cook version + platform + `PlatformSettings::
  StableKey()` + the source image hash + the editor `.zasset` hash. Settings
  are part of it because changing the compression format is a different cook
  with identical source bytes. `StableKey()` is field-by-field and uses the
  format NAME, never the struct bytes (padding is indeterminate) and never the
  enum integer (inserting a format would silently invalidate every key).
- **Cache files sit next to what they describe**, so they can never outlive it:
  `<output_dir>/AssetBundleBuildCache.json`, and
  `<Cooked>/<Platform>/cook_cache.json` for the cook. Delete the output
  directory and the next pass is a full one.
- **Everything fails safe**: a missing / corrupt / wrong-version cache yields
  an empty cache and logs why; `IsUpToDate` also checks the artifact exists and
  still has the recorded size, so a deleted bundle always loses; and keys the
  cache knows but this pass did not produce have their `.zbundle` **deleted** --
  a level removed from the project must not keep shipping.
- `force_rebuild` (console: `bundle.build --force`) is for "I don't trust the
  cache", not for "I changed something".
- **A RELEASE editor defaults `force_rebuild` to true.** `-DZENGINE_PACKAGING_
  FORCE_REBUILD=ON` is passed by `build_windows.bat` / `build_linux.sh` /
  `build_macos.sh` for the `release` configuration, and
  `AssetBundleBuildOptions` picks it up as `ZENGINE_PACKAGING_FORCE_REBUILD`.
  Rationale: an incremental package is only reproducible by whoever owns the
  cache, and a release artifact should not depend on that. It is a DEFAULT, not
  a lock -- `bundle.build --incremental` opts back in at run time. Note that
  `zbuild.py --extra-args` is an argparse REMAINDER, so a second `-D` has to
  join the existing `--extra-args` (which is why the Windows script branches on
  whether CI already added one for `ZENGINE_USE_VULKAN`).
  Pass it explicitly in BOTH directions (`=OFF` for a non-release config): CMake
  options are sticky, and the Windows presets are multi-config, so a debug build
  reusing one build directory would otherwise inherit the previous release's
  full-rebuild default.
- `BuildCache` is deliberately engine-free (no Object / LOG / Transfer), which
  is what lets `Editor/Build/Test/BuildCacheSmokeTest.cpp` run standalone.
  Its Test/ dir is excluded from the ZEditor glob (see
  `engine/Source/Editor/CMakeLists.txt`) -- a stray `main()` in the glob is
  LNK2005.

**Compression is decided PER ENTRY, not per bundle** (added 2026-10-20).

- A bundle is dominated by cooked textures, and BC7 / ASTC / BC1 payloads are
  ALREADY block-compressed: deflating them burns CPU at build and at load time
  and can even make them bigger. Script modules and mesh data, on the other
  hand, shrink a lot. So `WriteBundleFile` deflates each entry and keeps the
  compressed copy **only when it came out smaller**; everything else stays
  Stored. Incompressible content therefore pays nothing -- there is no downside
  to leaving compression on.
- The method lives in `BundleEntry::flags` (low 4 bits, `EntryCompression`), so
  no header layout changed and old v1 bundles still read (`flags == 0` is
  Stored). `BundleFileHeader::compression` reports what actually happened, not
  what was asked for.
- **CRC-32 guards the STORED bytes**, so a corrupt deflate stream is rejected
  before it ever reaches the decompressor.
- The writer REFUSES to emit a method this build cannot inflate
  (`AssetBundles::CanDecompress`), and the reader rejects one with a clear
  error. Never let it fall through to "return the raw bytes" -- they would be
  parsed as a `.zasset` and fail somewhere unrecognisable.
- zlib is a **vendored submodule** at `engine/3rdparty/zlib` (pinned v1.3.2),
  built by `engine/3rdparty/CMakeLists.txt` as the real target `zlibstatic`
  (static only, `ZLIB_INSTALL OFF` so it never joins our install/CPack rules).
  Do NOT go back to `find_package(ZLIB)`: the host copy was missing on several
  machines ("zlib.h not found") and minizip-ng's fallback was a configure-time
  `git clone` of zlib-ng. `Source/Runtime` links `zlibstatic` directly (real
  targets are global, so it is visible there); it only falls back to
  `find_package(ZLIB)` when the submodule is absent. No zlib at all
  (Emscripten) => `ZENGINE_BUNDLE_HAS_ZLIB` undefined, bundles stay Stored.
  `AssetBundleBuildOptions::compress` is the per-target override.
- minizip-ng consumes the SAME `zlibstatic`: its CMake does
  `find_package(ZLIB QUIET)`, and `FindZLIB` skips its own search when
  `ZLIB_LIBRARY` is already set and leaves an existing `ZLIB::ZLIB` alone --
  so `engine/3rdparty/CMakeLists.txt` pre-seeds `ZLIB_LIBRARY` /
  `ZLIB_INCLUDE_DIR` and aliases `ZLIB::ZLIB -> zlibstatic` *before*
  `add_subdirectory(minizip-ng)`, which is what lets `MZ_FETCH_LIBS` stay OFF.
  Keep that ordering: dropping it re-enables the network clone and/or links a
  second zlib. If the submodule is missing, `MZ_FETCH_LIBS` is left ON as a
  last resort.
- `packed_storage.reserve(entries.size())` in the writer is load-bearing:
  `payloads` holds pointers INTO that vector, and a reallocation would dangle.

**PlayerSettings** (`Runtime/Function/PlayerSettings/`) is the per-project
identity of a packaged player: `<project>/<config_dir>/player_settings.json`,
checked in and shipped by `ZENGINE_PACKAGE_PROJECT`.

- It used to be a 7-line stub that copied `ProjectInfo::name` into
  `m_ProjectName` and left the rest empty. The real consequence:
  `UserPreferences` derives its per-user directory from `m_ProductName`, so
  EVERY project collapsed onto the same `ZEngine` folder and shared one
  `preferences.json`. `m_ProductName` now defaults to the project name.
- Split of responsibility: `ProjectInfo` is the project (directories, startup
  level); `UserPreferences` is per-user and per-machine (where `Window.Width`
  ends up once someone resizes the window); `PlayerSettings` is the project's
  default. `WindowSystem` therefore reads, highest precedence first:
  UserPreferences > PlayerSettings > built-in fallback.
- `m_ProjectName` is deliberately NOT persisted -- it IS the project name, and
  an override would just let the two drift.
- A missing or malformed file is "use the defaults", never an error. Keys that
  are absent or ill-typed keep their current value, so an older file still
  loads. A resolution <= 0 is rejected at the file boundary.
- JSON via rapidjson, not ZCLASS/ZPROPERTY: PlayerSettings is not an Object and
  a Transfer-based format would have to be regenerated by ZHeaderTool.
  `ApplyProjectDefaults()` takes the two strings rather than a `ProjectInfo`
  precisely so it stays testable without a registered project.
- `Save()` is atomic (temp + rename, copy fallback on Windows) and must stay
  tolerant of failure: a packaged player can sit on a read-only install.

Known gaps, in the order they should be closed (PR-PAK2+):
- **ZGame is not built on macOS.** `engine/CMakeLists.txt`'s `elseif(APPLE)`
  branch adds Editor only, so the install rules above cannot be exercised
  end-to-end on a dev machine, and there is no local player to run.
- No incremental build, no content-hash change detection, no per-asset
  reference counting (unloading a bundle drops everything it cached).
- Only textures have a real cook today; every other asset type is packed as-is.
- **PlayerSettings has no editor UI** (there is no Project Settings panel at
  all), and no fullscreen / vsync / player-icon support: `GenericApplication::
  Initialize(title, width, height)` has no fullscreen parameter and the RHI
  picks its present mode unconditionally. Those need platform work first.

Known gaps, in the order they should be closed (PR-SK3+):
- **No importer test coverage**: the baking path is compile-verified only.
  There is no FBX fixture in the repo, so exercise it manually once with a real
  character asset before trusting it.
- glTF/GLB produce no skinning data at all (`.gltf` is not even parsed -- the
  mesh importer returns "not implemented yet").
- No IK, no root motion, no retargeting, no animation compression.
- The state machine has **no layers** (no upper-body / additive overrides), no
  blend trees inside a state (a state owns exactly one clip), and no sub-state
  machines.
- No layers / blend trees / sub-state machines in the state machine (see 2.25).
- `AnimationImporter` exists but is **not registered**: registration would
  shadow `MeshImporter` on `.fbx` and break mesh import. It runs the same
  `FbxAnimation::BakeAllStacks()` and is there for explicit / future use.
- DX12 **shadow** pass does not skin: skinned meshes cast their bind-pose
  shadow (the shadow VS has no joint palette bound yet). Vulkan's
  `mesh_directional_light_shadow.vert` does.
- Bone-count / instance limits are engine constants: 1024 joints per instance
  and 64 instances per drawcall (`s_MeshVertexBlendingMaxJointCount`,
  `s_MeshPerDrawcallMaxInstanceCount`).

### 2.43 RHI compute-capability queries: ask before you record

`RHI` now carries four capability predicates, declared in
`runtime/function/render/interface/rhi.h` immediately after the bindless block:

- `supportsComputeShaders()`
- `supportsIndirectDispatch()`
- `supportsStorageBuffers()`
- `supportsFloatRenderTargets()`

Defaults are **true** (compute / SSBO / indirect dispatch are core in Vulkan
1.0, D3D12 and Metal); a backend opts **out** by overriding. `WebGL2RHI`
overrides all four to false — WebGL2 / GLSL ES 3.00 has no compute stage, no
indirect dispatch, no SSBO (that is ES 3.1+), and float colour attachments only
via the optional `EXT_color_buffer_float`. `VulkanRHI` probes the one item that
is genuinely optional everywhere — renderable float formats — with
`vkGetPhysicalDeviceFormatProperties` on `VK_FORMAT_R16G16B16A16_SFLOAT` and
caches it in `m_FloatRenderTargetSupported`.

Rules:

- **Branch on the predicate BEFORE recording the work**, never after. The
  `GraphicsAPI` enum tells you which backend *compiled*, not what the device can
  do; WebGL2's `CreateComputePipelines` / `CmdDispatch` / `CmdDraw*Indirect`
  were silent no-ops, so a compute-driven pass that forgot to ask produced zero
  output and zero errors. They now log **once** through
  `LOG_UNSUPPORTED_ONCE` (WebGL2RHI.cpp) instead.
- Capability is latched per backend and read during pass setup; do not call
  `vkGetPhysicalDevice*` per frame. Probe once, latch, expose an
  `Is*Ready()`-style accessor, and surface it in the heartbeat so "which tier
  is running" is visible in telemetry.
- **`NaniteRenderer` is the first consumer** and defines the two-layer shape to
  copy: `HasGPUComputeSupport()` (= `supportsComputeShaders()` +
  `supportsStorageBuffers()`) gates `EnsureComputePipeline()` /
  `EnsureHiZResources()` up front and latches `gpu_pipelines_unsupported` with
  one log; the "try to build, latch on failure" layer stays underneath as the
  final arbiter, because a capability of `true` does not guarantee that this
  SPIR-V builds on that backend (DX12 wraps the blob into a "successful"
  shader module and only fails at pipeline creation). Note that Nanite's
  dispatch is plain `CmdDispatch` with CPU-computed group counts, so
  `supportsIndirectDispatch()` is deliberately NOT part of its gate.
- When adding a new backend or a new GPU-driven pass, override all four
  explicitly (even to `return true`) so the decision is stated in that backend
  rather than silently inherited from the base class.
- Startup already logs all four (`RenderSystem::Initialize`, right after
  `m_Rhi` is acquired); check that line first when a pass mysteriously draws
  nothing on one platform.

### 2.44 GPUParticle: 独立库（std-only core + 引擎适配器）

`engine/Libraries/GPUParticle/` 是与 GPUSpine 同款定位的库：核心**只吃标准库**，
ZEngine 的 `ParticlePass` 与 Unity 侧共用同一份发射 / 曲线 / 拼包逻辑。设计
（三档能力分级、Tier-1 无状态粒子、core/adapter 边界）见
`engine/Libraries/GPUParticle/doc/GPU_PARTICLE_DESIGN.md`。

- **Tier 1 是主力档**：粒子没有状态，出生时烘焙成一条 `ParticleGpuSpawn`
  （4 × vec4），之后 VS 用闭式解 `p0 + v0*t + 0.5*a*t²` 求位置 ⇒ 不需要 compute
  pass、alive/dead 链表、counter buffer、indirect dispatch。所以能在 GLES3.0 /
  WebGL2（无 compute、无 SSBO）上跑。
- **发射调度是 O(1)**：恒定速率的第 k 个粒子出生在 `k/rate` 秒，存活窗口
  `k ∈ ((t-life)*rate, t*rate]` 取整即得。不要改成 `carry += rate*dt` —— 那会让
  发射节奏随帧率漂移（测试 `ParticleGpuCoreTest` 有断言守着）。
- **出生记录布局要能同时当实例属性和 SSBO 一行**：GLES3.0 没有 SSBO，只能走
  `vertexAttribDivisor`；Vulkan/DX12/Metal 走 SSBO 或 VTF。改布局必须两边一起改。
- **随机器必须可跨语言复现**：`ParticleGpuRandom01` / `ParticleGpuSpawnSeed`
  用 lowbias32（C# 侧要 `unchecked`）；种子掩码到 24 位，因为载体是 float
  （实例属性通道没有 uint）。
- 构建：`cmake -S engine/Libraries/GPUParticle -B build/gpuparticle
  -DGPUPARTICLE_BUILD_TESTS=ON` + `ctest --test-dir build/gpuparticle -C Debug`。
  引擎侧在 `Runtime/CMakeLists.txt` 里 `add_subdirectory`（照 GPUSpine 那块），
  **目前还没 `target_link_libraries`** —— 等第 5 步 Tier-1 渲染路径落地再接。
- **它已经是真正的子模块**（`https://github.com/zentia/GPUParticle.git`），不是
  in-tree 目录：改核心要**先**在 `engine/Libraries/GPUParticle` 里提交推送，再回
  ZEngine 更新 submodule 指针，否则 ZEngine 侧永远拉不到你的改动。
- ~~C# 镜像在 `Runtime/CSharp/`~~ **2026-10-30 已删**（GPUParticle 与 GPUSpine 都删了）。
  原因：Unity 侧的导入器/运行时改成引擎内 C++，C# 镜像不在任何 ship 路径上。
  跨端一致性现在靠**同一个子模块 = 同一份代码**结构性保证，不靠对拍。
  公式层（`shader/particle_sim.h` / `shader/spine_skinning.h`）也在子模块里，
  `engine/shader/include/` 下那两个只是**转发头**，别往里加公式。
  改核心公式后要跑的是 core 自己的测试（`ctest -R ParticleGpu`）。
  （老教训仍然有效：出生位置不能逐位比，单精度 sqrt 允许 1 ulp；uint 运算必须
  unchecked，随机器依赖溢出 wrap。）
- 本仓库根 `.gitignore:312` 的 `*~` 会静默吞掉任何 `Tests~` 目录 —— GPUParticle
  的测试就这么丢过一次（`git add -f` 才进来）。新库的 `Tests~` 要留意。
- **RateAndBurst（`mode=3`）= 双窗口，不是新单窗口排班**（2026-11-05 落地）。起因：恒速流 +
  1 个 time=0 单次 Burst 同时存活时，存活粒子是**两段不连续的环形区间**，单 `ParticleGpuWindow
  {first,count}` 装不下 ⇒ 拆两个窗口、适配器发两次 draw。关键不变式（下次别推翻）：
  Burst 批固定占环形缓冲前 `burstCount` 个槽位 `[0,n)`，rate 流从 `n` 起接续、`spawnTime =
  rateOrdinal/rate`（不能复用 `序号/rate`，用 `ParticleGpuMakeSpawnAt`）；容量 `capacity >=
  ceil(rate·life) + n + 2` 保证 rate 流在 Burst 存活期不绕回覆盖 `[0,n)`；**shader 零改动**——
  两次 draw 同 `capacity`、只改 `_ParticleParams.x`；**重新激活走完整重启**（`ParticleGpuEmitterReset`）
  而非软重触发，因为 Burst 位置固定、软重触发会让 `emitted` 回退破坏 `Advance()` 的增量上传。
  只支持「1 个、单次、time=0」的 Burst 叠加（实测 `Client_proj` 里 rate+Burst 的 96.9% time=0、
  98.4% 单次，覆盖绝大多数）；循环/延迟/多 Burst 与 rate 叠加仍只保留 rate。
  ⚠️ **跨仓库一致性**：核心改动落在**共享子模块** `GPUParticle` 的工作副本里（Unity 侧是
  `G:\OSGame\unity-engine-source\External\GPUParticle`，ZEngine 侧是 `engine/Libraries/GPUParticle`），
  必须先在子模块里提交推送、再两侧各更新 submodule 指针（照本段上面那条）。Unity 侧适配器
  （`GPUParticleSystem` / 转换器 / `ParticleGpu.hlsl` 之外的 shader 无改动）是 Unity 独有、不进子模块。

## 3. Build / test invocations

- Configure: pre-existing `build/` is fine; CMake presets in `CMakePresets.json`.
- Incremental Debug build:
  `cmake --build build --config Debug --target ZRuntime ZEditor`
- Editor-only script: `build_windows_editor.bat` (wraps `zbuild.py`, default
  target `ZEditor`). In Cursor/VSCode, `zbuild.py` clears `VSCODE_NLS_CONFIG`
  during configure/build so the `windows_visual_studio` preset is not disabled.
- Editor exe lives at `bin/Debug/ZEditor.exe` (NOT under `build/`).
- Demo project for smoke tests: `I:\ZEngineDemo\ZEngineDemo.zproject`.
  Launch with `-p <path-to-zproject>`.
- **CTest**: `enable_testing()` lives in the ROOT `CMakeLists.txt` (not in
  `tests/`), so any directory below can register tests. Every opt-in smoke
  test (`option(ZENGINE_BUILD_*_SMOKE_TEST ...)`) MUST call
  `add_test(NAME <target> COMMAND <target>)` right after its
  `add_executable()` -- without it `ctest` never sees the test and CI runs an
  empty suite that still exits 0. Smoke tests are judged purely by exit code
  (`EXIT_FAILURE` on the first failed check), so no `PASS_REGULAR_EXPRESSION`
  is needed. A test that needs shipped assets or a GPU device must degrade
  instead of failing when they are absent: `NavMeshTest` prints
  `(skipped: level file not found)` and `DX12BindlessSmokeTest` falls back to
  the WARP adapter, which is why both can run on a headless CI runner. Verify
  a new registration with
  `ctest --test-dir <build> -C Release -N` (listing needs no build).
- **CTest gates**: do NOT switch `BUILD_TESTING` on to get a test to run -- it
  is OFF in the cache because minizip-ng force-sets it to silence zlib-ng's own
  tests, and turning it on registers every 3rdparty suite (curl, Jolt,
  mimalloc, ...) that this build never compiles, so `ctest` reports them all as
  "Unable to find executable". Add a dedicated `option()` per test group
  instead (`ZENGINE_BUILD_*_SMOKE_TEST`,
  `ZENGINE_BUILD_ENVIRONMENT_CHECK_TESTS` for the four `environment_*` tests
  under `engine/tools/ZEnvironmentCollector`).
- **`ZEditor` compiles on macOS but does not link there.** Two things were
  conflated for a long time:
  * *Compile* used to fail because `Editor/Console/EditorConsoleCommands.cpp`
    and `Editor/Menu/MenuController.cpp` included
    `Editor/Build/AssetBundleBuilder.h`, and `engine/Source/Editor/Build/` had
    never existed in the tree (`git log --diff-filter=A -- '*AssetBundleBuilder.h'`
    was empty). That is fixed -- `Editor/Build/AssetBundleBuilder.{h,cpp}` now
    exists, and every Editor translation unit compiles on macOS.
  * *Link* still fails, for an unrelated reason: the macOS build uses the Metal
    RHI, and the Editor pulls in `RenderSystem::GetFrameDrawList`,
    `ShaderCompiler::*`, `UIPass::*`, `MainCameraPassShaderCommon::*` -- all
    defined only in the DX12/Windows sources that `Runtime/CMakeLists.txt`
    excludes on Apple. Same class of failure as `ZRuntimeShared`.
  So to validate an Editor change on macOS, build object files directly rather
  than the whole target:
  `ninja -C build engine/Source/Editor/CMakeFiles/ZEditor.dir/<Sub>/<File>.cpp.o`
  (`ninja -t commands <obj>` prints the exact compile line). ZRuntime, ZSlate
  and every smoke test do build on macOS.

## 4. Active multi-phase work

### TypeScript scripting integration (`doc/TYPESCRIPT_SCRIPTING_DESIGN.md`)

| Phase | Status | Summary |
|-------|--------|---------|
| P1 | done   | `ProjectInfo` scaffolding (Scripts/, Intermediate/, tsconfig.json, package.json, .gitignore). |
| P2 | done   | `ScriptAsset` + `ScriptRegistry` (path-deterministic GUID, content-hash rename detection, atomic JSON save) + Content Browser Scripts root. Smoke test on `I:\ZEngineDemo`: rename preserves GUID, delete removes entry, restart is idempotent. |
| P3 | done   | `TypeScriptCompiler` (Editor-only IEngineSystem). Spawns `tsc --watch` (project-local `node_modules/.bin/tsc.cmd` preferred, falls back to PATH; degraded mode if neither). Pipes tsc stdout/stderr line-by-line into ZTSC log category via a worker thread + per-frame `Tick()` drain. Generalised `FileSystemWatcher` to take a `setExtensionFilter()` (default still `{".zasset"}` for legacy callers); compiler watches `.js` under `Intermediate/Scripts/` and exposes `JsChangeHandler` hook for P4. |
| P4 | done   | Module loader API on `ScriptingManager` (folded into the existing system instead of a separate `ScriptingEngine` class - it already owned the env). `LoadModule/ReloadModule/UnloadModule/IsModuleLoaded/InvokeExportedFunction/PathToModuleId`, plus IIFE `(function(module,exports){...})` wrapper for QuickJS (no native `require`). `ScriptEnv::InstallConsoleAndDebugGlobals()` plumbs `console.{log,info,debug,warn,error}` and `Debug.{Log,LogWarning,LogError}` through native pesapi callbacks into the `ZScripting` log category. `EditorApplication` wires `TypeScriptCompiler::SetOnJsModuleChanged` -> `ReloadModule`/`LoadModule` with 200ms per-module debounce, and at startup scans `js_root/*.js` to load already-compiled modules so cold restarts aren't dependent on tsc re-emitting. Smoke test: `I:\ZEngineDemo\Scripts\P4Smoke.ts` (`console.log` + `Debug.Log` calls) appears in BqLog under `[ZScripting]` on both startup-load and hot-reload paths. |
| P5 | done   | `Behaviour` abstract Component + `TypeScriptComponent` subclass (serialises `m_script_guid` + `m_class_name` as `eastl::string`; `m_js_instance` stored as `void*` to keep pesapi out of the public header). Lifecycle mapped onto ZEngine's `Component` API: `postLoadResource` -> JS `OnAwake` + `OnStart`, `tick(dt)` -> `OnUpdate(dt)`, dtor -> `OnDestroy` then `pesapi_release_value_ref`. `ScriptEnv::CreateInstance/DestroyInstance/InvokeInstanceMethod*` work around pesapi's missing `new` operator via a JS-side `__zNewInstance(ctor)` shim, and `globalThis.Behaviour` placeholder class is injected by the same shim so user code can `class Foo extends Behaviour` without a real binding. Engine emits `Intermediate/Typings/zengine.d.ts` (declares `Behaviour`, `Debug`, `console`, `__zNewInstance`) on every project open in `ProjectInfo::ensureScriptsScaffold` -- tsc reports 0 errors. Inspector got a minimal "Add TypeScript Behaviour..." popup that lists `ScriptRegistry::getAll()` entries with non-empty `m_default_class_name`, attaches a `TypeScriptComponent` and explicitly fires `postLoadResource` (since `addComponent` doesn't). `EditorApplication::registerEdtorTickComponent("TypeScriptComponent")` makes `OnUpdate` fire in edit mode (Unity `[ExecuteAlways]` equiv). Smoke test: `I:\ZEngineDemo\Scripts\Hello.ts` extends `Behaviour`, throttled `OnUpdate` logs every 60 ticks; `module loaded: Hello` + `[Hello] OnAwake/OnStart` appear in BqLog. `m_serialized_fields` (per-instance script field overrides) deferred to P7. |
| P6 | done   | Hot reload of live `TypeScriptComponent` instances + Project-window double-click -> VSCode. `ScriptingManager` got a generic observer hook (`AddModuleReloadObserver`/`RemoveModuleReloadObserver`/`NotifyModuleReloaded`); `ReloadModule` fires it on success after `ScriptEnv::ReloadModule` finishes. Each `TypeScriptComponent` subscribes once at the end of `BindAndAwake` (so unbound components don't pay) and unsubscribes in `TearDown`/dtor; the callback filters by `m_module_id` and on a match just calls `BindAndAwake()` again -- which itself starts with `if (m_js_instance) TearDown();`, so OnDestroy(old) -> DestroyInstance -> CreateInstance(new) -> OnAwake -> OnStart all happen in one place. State is intentionally NOT preserved across reload (matches Unity MonoBehaviour); per-instance `m_serialized_fields` overrides deferred to P7. `NotifyModuleReloaded` snapshots the observer list under a mutex before iterating, so re-entrant subscribe/unsubscribe from inside a callback is safe. Cold-start batch `LoadModule` does NOT fire the observer (no components have bound yet). Added `EditorUtility::openInExternalEditor(path)` with Win/macOS impls: tries `ZENGINE_EXTERNAL_EDITOR` env override first, then `code` (Windows: `ShellExecuteW(L"open", L"code", ...)` resolves via PATH+PATHEXT to `code.cmd`; macOS: `open -a "Visual Studio Code"`), falling back to OS file association. `project_window.cpp` double-click on `.ts/.tsx/.js/.json` now routes here instead of `revealInFinder`. Smoke test on `I:\ZEngineDemo`: editing `Hello.ts` while editor runs produces `[ZTSC] Compiled module updated` -> `[ZScripting] hot-reloading module: Hello` -> `module unloaded: Hello` -> `module loaded: Hello` in BqLog within ~600ms, editor stays at 60 FPS. |
| P7 | done   | Per-instance script field overrides (`[SerializeField]` analogue). Storage: `std::vector<eastl::string> m_serialized_fields` flat alternating `[k0,v0,...]` (the only `SerializeTraits`-supported shape - no `pair<eastl::string,eastl::string>` path). `TypeScriptComponent` got `Get/Set/Remove/CountSerializedField`, `ApplySerializedFields` (called between `CreateInstance` and `OnAwake` in `BindAndAwake`, so values are reapplied on every cold-load AND every hot-reload), `LiveSetField` (Inspector edits write both to `m_serialized_fields` and live JS), and `EnumerateLiveFields` -> Inspector. Type coercion lives JS-side: `__zApplyField(instance, key, valueStr)` reads `typeof instance[key]` set by the field-initialiser and routes to `parseFloat`/`==='true'`/`String()` accordingly. `__zEnumerateFields` returns own-enumerable, non-function, non-`_`/`$`-prefixed properties packed as `name\u0001type\u0001value` records joined by `\u0002` (control chars never collide with JS identifiers, simpler than array marshalling). Inspector renders `InputDouble`/`Checkbox`/`InputText` per field with `EnterReturnsTrue` (no every-frame writeback); unbound components show stored overrides read-only. Hot-reload preservation (key user-visible difference vs P6 stateless rebuild): authoring data persists, runtime state still drops -- matches Unity Editor. Smoke: `I:\ZEngineDemo\Scripts\P7Smoke.ts` (`speed:number`, `enabled:boolean`, `label:string`, `_ticks:number`-private). Inspector lists 3 rows (filtered `_ticks`); edits round-trip through scene save/load AND through .ts hot-reload. Object/array fields out of scope (return false, log WARN). |

(The historical "P5..P8" wording in `inspector_window.cpp:5404` is pre-merge and
predates the AssetRegistry-persistence work being split out into its own track
below -- it does not refer to the P8 below.)

| P8 | done   | HMR（Hot Module Replacement）：模块顶层用 IIFE 注入的 `module.hot` 自选 `accept()` / `accept(cb)` / `dispose(cb)` / `decline()`。⚠️ 这四个入口必须是 **native** 函数（`pesapi_create_function` 注册到 globalThis 的 `__zhmr_native_accept/dispose/decline` + `__zhmr_require`）—— 早先写成纯 JS 存根，只往 JS 对象里塞，从不回调 C++，`m_HmrStates` 恒空，全程"接上了但没生效"。`require` 同时第一次真正可用（IIFE 把它声明成局部 `var`，相对说明符按当前模块目录解析，并把 parent→child 边记进 `m_ModuleDeps`）。`ScriptEnv::ReloadModule` 返回 `HmrReloadResult{ok, hot_replaced, reloaded}`：cascade = 被改模块 ∪ 传递依赖它的模块（BFS 序）；**全 accept 且无 decline** 才走热路径，否则整条回落 P6 全量重建（不做部分热替换）。热路径**不通知** P6 观察者 ⇒ `TypeScriptComponent` 不重建 ⇒ 运行时状态保留 —— 这是与 P6 唯一的用户可见差别。三个坑：①判定必须在**重新求值之后**按新代码的声明做（求值前清零 `accepted/declined`），否则"改成 decline"静默失效；②缓存要在 cascade 顺序里**边求值边换**，否则 parent 重新求值时 `require` 命中的还是旧 child ⇒ 半新半旧；③`TakeHmrCallbacks` 必须先把旧回调摘走再求值，否则新代码重新登记时会释放一份我们还持着的 ref（双释放）。验证 `ScriptHmrSmokeTest` 57 checks / 0 failures。 |

### TypeScript editor layer (`doc/editor/EDITOR_TYPESCRIPT_LAYER.md`)

Goal: "C++ core + TS editor shell", the UnityEditor.dll model. TS takes the
slot Unity gives C#; it does NOT mean rewriting renderer/AssetRegistry/Undo
core in TS. Key conventions (do not re-litigate):

- **Two VMs, one pesapi registry.** `ScriptingManager` owns the game VM
  (`ZScriptDomain::Runtime` only). `EditorScriptingManager` owns a second,
  isolated editor VM that additionally gets `ZScriptDomain::Editor` bindings.
  Game scripts must never see editor API.
- **Domain is per-module, not per-annotation.** `zengine_header_tool(... SCRIPT_DOMAIN editor)`
  on ZEditor stamps every generated binding as Editor. Annotate an editor class
  with plain `ZCLASS`/`ZFUNCTION` (static-facade pattern: `Object` subclass,
  see `Editor/Scripting/EditorEnvironment.h`).
- **Typings:** `zengine.d.ts` (Runtime) + `zengine-editor.d.ts` (Editor only).
  Editor scripts compile against both; the editor rewrites them into
  `engine/EditorScripts/typings/` (gitignored) on every start in a checkout.
- **Engine-shipped editor scripts** live in `engine/EditorScripts/src/*.ts`,
  precompiled by the `ZEditorScripts` CMake target to
  `<bin>/<config>/EditorScripts/*.js` (engine asset, no tsc needed at runtime,
  no hot reload). Entry: `Bootstrap.js` exports `OnEditorLoad`/`OnEditorUnload`.
  Override dir: `ZENGINE_EDITOR_SCRIPTS_DIR`; inspector: `ZENGINE_EDITOR_V8_DEBUG_PORT`.
- Editor-domain classes should not inherit from Runtime-domain ones until the
  Editor d.ts placeholder-super handling is revisited (see doc).

| Phase | Status | Summary |
|-------|--------|---------|
| P0 | done (build-unverified) | Domain split in `ScriptBinding`, ZHeaderTool `--script-domain`, ZEditor wired into ZHeaderTool, `EditorScriptingManager` + `EditorEnvironment`, `engine/EditorScripts` package, `ZEditorScripts` CMake target. |
| P1 | todo | Facades: `EditorSelection`, `EditorUndo`, `AssetDatabase`; TS `@MenuItem`. |
| P2 | todo | ZSlate TS bindings, runtime-registrable window registry, `EditorWindow` base. |
| P3 | todo | Pluggable `CustomEditor`/`PropertyDrawer` in the Inspector, real `SerializedObject`/`SerializedProperty`. |
| P4+ | todo | Migrate Package Manager / Console / Add Component etc.; open `Scripts/Editor/**` to users. |

### Asset registry persistence (`doc/asset_management/AUTO_IMPORT_AND_FILE_WATCHER.md`)

This is an independent track from the TypeScript scripting integration
above; it was historically logged as "P9" alongside the TS phases but
shares no code with them and gets its own row here so the numbering
gap above is intentional, not lost work.

| Phase | Status | Summary |
|-------|--------|---------|
| P9 | done   | AssetRegistry persistence + live FileSystemWatcher hook-up. `saveCache`/`loadCache` write a versioned binary cache to `<Project>/Intermediate/asset_registry.cache` via tmp-file-rename (`'ZARC'` magic + u32 version + scan_root + N x [path,guid,asset_type,mtime,size]; **v2** appends `dep_owner_count` + per-owner `[path, dep_count, dep_paths...]` before `'ZARE'` tail; `m_ReferencerMap` rebuilt on load). v1 caches still load (no deps; lazy `IndexDependenciesLocked` on first query). Writes always emit v2. `EditorAssetManager::Initialize` now does `loadCache -> scanAssetsAsync` where the async scan SKIPS the per-file header read when an in-memory entry's `(mtime,size)` already matches the on-disk file (UE-style "incremental" semantics) -- warm restarts go from O(N header reads) to O(N filesystem stats). `Shutdown` calls `stopWatching -> waitForScanComplete -> saveCache` so the cache always reflects a fully-populated map. `FileSystemWatcher` callbacks finally wired (they had been allocated with `watchDirectory` but no `setOnFileXxx`!): `onFileCreated/Changed -> AssetRegistry::refreshAsset`, `onFileDeleted -> removeAsset`. The watcher event queue is drained on the main thread by `EditorAssetManager::TickWatcher()` slotted into `EditorApplication`'s per-frame tick (between `EditorInputManager` and `TypeScriptCompiler`). Path-key invariance: `refreshAsset/removeAsset` now normalise inputs to scan-root-relative form via a private `normaliseToRegistryKey` helper, so callers can pass either absolute (watcher path) or relative (saveAsset). `EditorFileService::computeAssetTypeLabel` got a fast path `computeAssetTypeLabelFromRegistry` that hits `AssetRegistry::getAssetIndex` before falling back to the `getAssetTypeName` header read; this is the third hot-path Inspector-rebuild walk that's now O(map hit) instead of O(N header reads). Cache version is v2; versions outside `[1,2]` are rejected (full rescan). The legacy `"legacy:"`-prefixed synthetic GUID for magic-mismatch fallback files (already added in phase B) round-trips through cache cleanly because the loader uses the same `upsertEntryLocked` invariants. |

### DX12 MainCamera path B (`doc/DX12_SUBPASS_RHI.md`)

Independent from the TypeScript / AssetRegistry tracks above. DX-B0
(subpass RHI emulation) through DX-B8 (RenderPipeline parity) are **done**,
including RP1 skybox callback, bindless tonemap between RP1/RP2, RP2
post/UI chain, DX12 global IBL/LUT init + GPU cubemap mipgen (see
section 2.11). No numbered code phases remain on this track; optional
RenderDoc checklist is in `doc/DX12_SUBPASS_RHI.md` (manual only).
Cross-backend bindless backlog (Metal / WebGL2 tonemap parity, etc.) is
in `doc/BINDLESS_TEXTURE_PATH.md`, not here.

## 5. Deferred backlog (not in section 4)

Section 4 tracks numbered phases only; the items below are intentional
follow-ups documented elsewhere. Do not treat their absence from section 4
as lost work.

| Area | Doc pointer | Notes |
|------|-------------|-------|
| ShaderRegistry downstream GUID chain | AGENTS.md 2.2 | Registry + `m_ShaderGuid` landed; full downstream GUID-ification still deferred (PR-SE3c). |
| ShaderLab hot path | AGENTS.md 2.2 | **DX12 variant (2026-05)**: `ShaderLabVariant` Cartesian `multi_compile`/`shader_feature`; `ShaderLabDx12Compiler` precompiles variants to `Intermediate/Shaders/` DXIL cache with Unity-style **shader_feature strip** (`GenerateVariantCombinationsBuildStrip` + per-project `MaterialRes::m_EnabledShaderKeywords` scan in `ShaderImporter`). `multi_compile` stays full Cartesian; unused `shader_feature` combos are skipped at precompile. `DX12ShaderCompiler` + `MaterialRes` keyword macros at draw time. Vulkan `ShaderLabCompiler` still uses full Cartesian (SPIR-V). |
| Bindless Metal / WebGL2 | `doc/BINDLESS_TEXTURE_PATH.md` | WebGL2 permanently excluded; Metal deferred. |
| Route A (texture) | `doc/BINDLESS_TEXTURE_PATH.md` PR9-11 backlog | Project-window source hiding and `AssetFile` stub cleanup are **done**; `Texture` base reflection, embedded source in header, and Inspector Reimport button for textures remain open. |
| Data Inspector | AGENTS.md 2.2 Data | Row add/remove, column reorder, `.xlsx` write-back out of V1 scope. |
| RenderDoc | `doc/DX12_SUBPASS_RHI.md` | Manual checklist only. |
| World Partition V3+ | `doc/world_partition/WORLD_PARTITION_DESIGN.md` | V1+V2 landed: grid streaming, async preload, level ref-count, render delete on unload, editor cell overlay. Data layers / HLOD / per-cell `.zasset` LevelRes still open. |
| LWC (Large World Coordinates) | `doc/world_partition/LARGE_WORLD_COORDINATES.md` | L1+L2 landed: `r.LWC.Enable`, 2^21 render tiles, `Vector3d` transform/hierarchy, schema read float / write double. L3 shader types + L4 per-cell origins open. |

## 6. Tools: ZEnvironmentCollector (env collector)

`engine/tools/ZEnvironmentCollector/` ships **one** executable,
`ZEnvironmentCollector.exe`: GUI by default (three tabs — collection /
compatibility check / GPU select verification), CLI under `-batchmode`
(`--collect`, `--diagnose`, `--gpu-selection`, and the plain check). The former
separate `ZWindowsEnvironmentCheck.exe` CLI and `ZGpuSelectionInspector.exe` GUI
are gone; `-batchmode` is the Unity/UE spelling on purpose. The exe is
**WINDOWS-subsystem** (`wWinMain`), so a double-click never flashes a console —
as a CONSOLE binary the loader's console existed before any of our code ran, so
`FreeConsole()` could only close it after it had already flashed. `-batchmode`
attaches to the caller's console (`AttachConsole(ATTACH_PARENT_PROCESS)`) unless
the caller already redirected stdout to a pipe or a file, so cmd.exe/PowerShell
still get stdout and the exit code, and cmd still waits in a `.bat` (measured,
not assumed). The collector re-launches *itself* as a
Job-managed stage worker and as an elevated WER helper, so both call sites read
`collector::ToolExecutablePath()` — renaming the output must go through that
constant. It packages **game logs** together with the environment report:

- The log directory is declared per project profile, never guessed: `logRoot`
  (`localLow` default, else `localAppData` / `roaming` / `programData`) +
  `logSubPath`. OSGame = `LocalLow\Tencent\osgameCNOfficial\Cache\Logs`.
  CLI override: `--log-dir`. Empty `logSubPath` disables the stage.
- **Real OSGame logs are `.logcompr`** (compressed per-session 扁鹊 log), not
  `.log`; plain `.log` only shows up under `bqLog\ccs_log\`. A filter that
  accepts only `.log`/`.txt` silently collects nothing on a real machine.
- Caps: 8 files, 16 MiB each, 48 MiB total; incident-window files first, then
  newest; oversized files are skipped, never truncated. Logs are copied into
  the report dir (`gamelog-NN-<name>`), never moved / modified / renamed.
  Index in `game-logs.json`; missing or empty log dir is status `NO_LOGS`
  and does **not** mark the report partial.
- **Its own diagnostics go through 扁鹊** (`EnvCheckLog.h/.cpp`, category
  `ZEnvironmentCheck`): `%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_*.log`,
  same directory the engine uses. `ZSlate::SetLogSink` forwards ZSlate's own
  lines (font atlas misses, "no CJK face") there too. The manifest carries the
  path as `toolLog`; the log itself is **not** copied into the ZIP.
- **It links `ZRuntimeShared`** — see §6.1. The log instance is then the engine's
  `LogSystem` (reached over the C ABI), not a second bqlog linked into the tool.
  A standalone configure has no `ZRuntimeShared` target and falls back to its own
  `bq::engine_log`.
- **A package without an incident anchor is not an error, but must say so.**
  Missing `incidentTimeUtc` / `incidentKind` / `gameExecutable` means the package
  can only describe the machine, never attribute anything — and three field
  packages in October 2026 came back exactly like that, with the discovery left
  to whoever opened the ZIP. `DescribeAttributionAnchor()` drives a summary.txt
  header, manifest `attributionUsable` / `missingAttributionInputs`, a red line
  on the GUI form plus a confirm dialog, and a CLI stdout warning. Details and
  the `isTargetGame` field of `events-redacted.json` (schemaVersion 2) live in
  `engine/tools/ZEnvironmentCollector/README.md` §1.8.1.

**Preflight now covers third-party incompatible programs, but only as advice.**
`--preflight` takes a name-only process snapshot (`CreateToolhelp32Snapshot`)
and enumerates active driver services (`EnumServicesStatusExW`) for Sysinternals
Process Monitor (`procmon*.exe`, `PROCMONxx`). A hit becomes
`INCOMPATIBLE_PROGRAM_DETECTED` / `CLOSE_INCOMPATIBLE_PROGRAM` as an **Advise**
finding, never `BelowMinimum`: the tool never blocks a launch, and the game's
own anti-cheat check still owns the "Incompatible program ... detected" dialog.
Both scans carry known flags (a failed scan leaves the report Pass with a note,
never a false all-clear) and land in `environment.incompatibleProgramScan` in
the JSON. Keep the name list in sync with the game's own list: the report is
only useful if it agrees with that dialog. See README.md.

### 6.1 Why the collector links ZRuntimeShared, and what it must not start

The collector needs two things the engine already owns, and both are worse when
duplicated inside a tool that ships to players:

1. **Logging.** It is a WINDOWS-subsystem binary, so stderr goes nowhere when it
   is double-clicked, and the failures it exists to report are exactly the ones a
   player cannot describe.
2. **Localised UI strings.** Hard-coding 100+ Simplified Chinese literals into
   `UiLocalization.cpp` made translation a code change. They are now data: a real
   engine `DataTable` (`Runtime/Resource/Asset/Data/LocalizationTable.h`) cooked
   from `resource/ui_strings_zh-CN.csv`. A table is engine capability —
   `AssetManager` + `SerializedFile` + reflection — so it comes from the same
   place as the log.

The seam is `Runtime/ZEngineApi/ZEngineToolApi.h` (`EXPORT_RUNTIME`, i.e.
`extern "C"`). Nothing that needs an allocator crosses it: `const char*` in,
`const char*` owned by the DLL out. That is what makes an /MT executable able to
consume the DLL regardless of the DLL's own CRT choice.

**`ZRuntime_ToolInit()` starts the minimum slice and no more**:
`RegisterCore()` + `RegisterPlatform()` — reflection, memory, `FileSystem`,
`ObjectManager`, `AssetManager`, plus `LogSystem`. It deliberately does **not**
register `Application`, `WindowSystem`, any RHI, `ScriptingManager` or
`ProjectInfo`. A tool sent to diagnose a machine must not open a window or create
a GPU device on it. (Same shape as `ZEngineApi::InitMemoryProfilerData()`, minus
`ProfilerRuntime`.)

Loading takes absolute paths: `AssetManager::GetFullPath()` passes them straight
through, so a tool with no project can still read the `.zasset` it ships beside
its own exe.

Cook step: `DataTableCook` (`engine/tools/DataTableCook`) bakes
`resource/ui_strings_zh-CN.csv` -> `.zasset` at build time, driven from the
collector's POST_BUILD. It lives in Runtime (not the Editor's
`CsvSchemaRegistry`) because ZRuntime must not depend on ZEditor and the cook has
to run with no project open. The shipped tool never parses CSV.

Missing DLL = process does not start (import-lib link, by decision). Missing
`.zasset` = UI degrades to English and the tool still works.

## 7. Standalone ZSlate tools: use FSlateStandaloneApp, do not hand-roll a host

Every standalone ZSlate GUI tool (TexPreview, ClientProjBranches,
ZEnvironmentCollector — the former ZGpuSelectionInspector is now a tab inside
the collector; LuaMemPreview is no longer standalone, see §7.5) used to
carry its own copy of: an `ISlatePlatform` subclass, WNDCLASS + CreateWindow +
PeekMessage host, DPI declaration / `WM_DPICHANGED` handling, and a hardcoded
`C:\Windows\Fonts\...` atlas load. The copies drifted — two had **no DPI
support at all**, four of five hardcoded the Fonts drive letter, and the DPI
decorator was built with different pixel-snapping flags.

That is now one implementation, in ZSlate:

| Concern | Where it lives |
|---|---|
| Window + message pump + `ISlatePlatform` + D3D11 backend lifetime | `ZSlate/App/SlateStandaloneApp.h` (`FSlateStandaloneApp`) |
| DPI awareness, per-window scale, DPI-sized window framing | `ZSlate/Platform/Win32/SlateWin32Dpi.h` (`Win32::DeclareDpiAwareness`, `QueryDpiScaleForWindow`, `QuerySystemDpiScale`, `ResizeWindowForDpi`) |
| System font discovery + CJK fallback, key mapping, clipboard, UTF-8 argv | `ZSlate/Platform/Win32/SlateWin32Host.h` (`Win32::LoadSystemFonts`, `KeyFromVirtualKey`, `InstallClipboardBridge`, `CommandLineArgsUtf8`) |

**A new standalone ZSlate tool must start from `FSlateStandaloneApp`.** Doing
anything else re-introduces per-tool DPI/font bugs.

Coordinate contract (stated once, in `SlateStandaloneApp.h`): Win32 messages and
the swapchain are **physical**; the widget tree is **logical** (96-DPI units).
The shell is the only place the two meet — its WndProc divides on the way in,
`FSlateDpiScaleRenderer` multiplies on the way out.

Known exception: `CollectorGui.cpp` keeps its own window/WndProc because it
needs a Unicode window, a WM_CLOSE veto while a background collection runs,
popups that pre-empt widget input, and a `busy` input gate. It still uses
`Win32::LoadSystemFonts` / `Win32::ResizeWindowForDpi`, so no DPI/font
knowledge is duplicated. Do not "unify" it by adding shell hooks only it uses.

### 7.1 SListView is NOT broken — do not repeat the old claim

`ToolSuite/LuaMemPreview/LuaMemWidgets.h` used to justify its hand-painted 180K-row table
with "SListView's recycling is broken in two ways" (stale data in recycled rows,
and `ReleaseInvisibleWidgets()` never called → a widget retained per row).
**Both were already fixed**: the index→widget map is keyed by index, released
when rows scroll off screen, and cleared whenever the data source revision
changes. The comment was stale and kept being cited as the reason to duplicate
list widgets — it has been rewritten, and the behaviour is now pinned by
`engine/Libraries/ZSlate/tests/slate_list_view_tests.cpp`
(`-DZSLATE_BUILD_TESTS=ON`), which scrolls 180K rows and asserts bounded text
draws / bounded live widgets / fresh data after recycling / balanced clip stack.
Measured: 27 live widgets at 180K rows.

`SLuaMemTable` is still hand-painted, but now for a real reason, not a
workaround: 4 fixed columns with independent sort, a filter and type-colour
chips would cost ~135 live widgets rebuilt every scroll step if routed through
`SListView`. Do not "unify" the two tables without measuring.

Other pieces sunk into ZSlate in the same pass: `SScrollPanel`
(`ZSlate/Widgets/Layout/`, was `CollectorScrollPanel.h`) and
`Win32::EnsureConsole` / `Win32::StdOutIsRedirected`.

### 7.2 App icons for GUI tools: one generator, one CMake function

Three tools (TexPreview, ClientProjBranches, ZEnvironmentCollector) each need
an application icon, and each used to hand-roll all four pieces of it: a `.ico`, a `.rc`, an `if(MSVC)` block in its
CMakeLists, and an `#ifndef IDI_ICON1` in its `main.cpp`. That is now two
shared pieces:

| Piece | Where | What a tool does |
|---|---|---|
| `z_embed_app_icon(<target>)` | `engine/cmake/AppIcon.cmake` (+ `AppIcon.rc.in`) | one call in the tool's CMakeLists, after `set_target_properties(... OUTPUT_NAME ...)`. Embeds `resource/<OUTPUT_NAME>.ico`, defines `IDI_ICON1=101`, no-op off Windows / when RC is unavailable / before the .ico exists. `engine/CMakeLists.txt` includes it; tools that configure standalone `include()` it by path. |
| `gen_icon.ps1 -Name <Tool> -Motif <...> -OutDir <tool>/resource` | `engine/resource/gen_icon.ps1` | draws the `.ico` (16/32/48/256, PNG-compressed entries) on the shared dark rounded tile. Motifs: `branch` (ClientProjBranches), `checker` (TexPreview), `check` (ZEnvironmentCollector). |
| `z_embed_app_manifest(<target> [MANIFEST <file>])` | `engine/cmake/AppManifest.cmake` (+ `AppManifest.manifest`) | one call, same place. Declares per-monitor DPI v2 by embedding the shared manifest; `MANIFEST <file>` replaces it wholesale for a tool needing something else (execution level, comctl32 v6). MSVC-only — CMake has no mt.exe rule for MinGW — and a no-op elsewhere, so the call can be committed anywhere. Same include arrangement as `AppIcon`. |

`z_embed_app_manifest()` is **required** for a GUI that does not go through
`ZSlate::FSlateStandaloneApp` — today only `ZEnvironmentCollector`, which
registers its own `WNDCLASSEX` so `DeclareDpiAwareness()` never runs on its
path. (ZInsights used to be in that bucket, and a prior note claimed it was
covered "via `ZRuntime`'s `SetProcessDpiAwarenessContext()`" — it was not:
that call lives in `WindowsApplication::Initialize` and ZInsights never
constructs a `WindowsApplication`. **Linking ZRuntime declares nothing; only
running that code does.** Use this test, not the link line, when deciding
whether a target needs the manifest.) Tools on the shell declare the same mode
in code (`DeclareDpiAwareness()` inside `FSlateStandaloneApp::Initialize`), so
for them the manifest is redundancy that costs nothing and is slightly safer:
it is in force before `main()` runs, so it cannot be beaten by a window created
too early.

Declaring awareness is only half of a crisp GUI; the other half is **letting
the scale reach the glyph rasteriser**. `FSlateDpiScaleRenderer` multiplies
`font_size` *before* it hits the font atlas, and the atlas bakes per integer
pixel size — so at 150% a 14px label is genuinely rasterised at 21px instead of
a 14px bitmap stretched 1.5x. That is why no ZSlate tool needs a Direct2D
backend for crisp text, and why pinning a raw D2D target to 96 DPI with a
"1 DIP == 1 pixel so mouse coords line up" comment is the bug: it renders 1:1
and lets DWM do the scaling instead. If you ever do add a non-ZSlate surface,
create it in physical pixels at `GetDpiScale() * 96` and recreate it from
`OnDpiChanged`.

**The .rc alone is not enough.** It only puts the icon in the *executable*
(Explorer, Properties). The title bar, Alt-Tab entry and taskbar button come
from the **window class**, which is a separate step:

- ZSlate tools: `desc.LargeIconResId = desc.SmallIconResId = IDI_ICON1;`
- raw Win32: `WNDCLASSEXW::hIcon` / `hIconSm` — **WNDCLASSEX, not WNDCLASS**;
  the plain class has no `hIconSm`, so the taskbar keeps the default icon.

Both are guarded by `#ifdef IDI_ICON1`, so a config where the .rc was not
embedded still compiles. Do not re-add per-tool `#define IDI_ICON1 101` — the
CMake function is the only source of that id.

**The editor has two icon mechanisms, and they are easy to confuse.** They
cover different surfaces and neither alone is enough:

- **Embedded in the executable** (Explorer, Properties, taskbar):
  `engine/Source/Editor/resource/ZEditor.ico`, attached by `z_embed_app_icon()`
  in `engine/Source/Editor/CMakeLists.txt`. `AppIcon.cmake` generates the .rc
  into the *binary* dir, so the `file(GLOB ... resource/ZEditor.rc)` in that
  CMakeLists still matches nothing -- harmless, but it means dropping a
  hand-written `resource/ZEditor.rc` in there would silently take over.
- **Loaded at runtime** (window title bar, Alt-Tab): `ZEngineEditorBigIcon.png`
  and `ZEngineEditorSmallIcon.png` under `engine/resource/`, named by
  `BigIconFile` / `SmallIconFile` in `configs/{development,deployment}/ZEditor.ini`
  and read by `EditorUI.cpp: LoadWindowIcons`.

The runtime pair is PNG, so it is decoded with stb_image. Do not go back to
`LoadImage(..., IMAGE_ICON, ...)` for them: that reads .ico/.cur only and
returns NULL, which is how this path went unnoticed for so long.

`ZEngineEditorBigIcon.png` is also the particle system's logo texture
(`engine/asset/global/particle.global.json`), so it is not dead weight.

### 7.3 ZAppShell — the startup bits every executable repeats

`engine/Libraries/ZAppShell/include/ZAppShell/AppMain.h`. **Header-only, and it
must stay that way**: `ZEnvironmentCollector` ships `/MT` while the tree is
`/MD`, so anything compiled into a library serves only one of them (`LNK2038`)
— which is exactly why the collector had its own copy of the console-attach
logic instead of calling the `ZSlate` one. Inline code is compiled by whoever
includes it, with that target's own runtime library. It therefore calls nothing
but Win32 and the CRT: no ZSlate, no ZRuntime.

| What | Why it is shared |
|---|---|
| `CommandLine::Acquire()` | `CommandLineToArgvW(GetCommandLineW())` was open-coded in five places (ZEditor, ZInsights, CommandSystem, LogSystem, WEC). Owns the array and `LocalFree`s it, so call sites stop leaking it. `.Utf8()` for `std::filesystem`/printing. |
| `EnsureConsole()` | A `/SUBSYSTEM:WINDOWS` exe has no console, so printf goes nowhere and a headless mode looks like it hung. Two implementations existed (ZSlate's and WEC's) and had drifted; the ZSlate one is now gone. No-op when the caller already redirected stdout to a pipe or file, so CI captures keep working. |

**Never `LocalFree` the array yourself** — `CommandLine` owns it. ZEditor's
`resolveStartupModeFromCommandLine` had a manual `LocalFree` that became a
compile error the moment `CommandLine` took ownership.

`EnsureConsole()` must be called **only on the path that prints**. A GUI run
that called it would flash a console window on double-click — the whole reason
these executables are `/SUBSYSTEM:WINDOWS`.

**Do not read `__argv`.** It is in the ANSI codepage, so a non-ASCII path
arrives already mangled — and `LuaMemPreview -batchmode` takes a path.

**Not shared on purpose:** deciding whether a run is GUI or batch. The three
executables with a batch mode disagree — WEC takes `-batchmode` as a switch
anywhere on the line, `LuaMemPreview` takes it as a positional subcommand next
to `--stats`/`--dump-ui`, and ZEditor reads `-p`/`--rhi` to pick a startup mode.
(LuaMemPreview is now a ZEditor tool suite: `-batchmode` is ZEditor's switch and
`-suite=LuaMemPreview` selects it — see §7.5.)

### 7.4 Output & build directories for standalone GUI tools

Two directories per tool, both fixed by convention so an exe never has to be
hunted for:

| Kind | Path | Set by |
|---|---|---|
| **Output** (the `.exe`) | `<repo>/bin/Tools/<Config>[/<subdir>]/` | `z_set_tool_output_dir(<target> [SUBDIR <subdir>])` — `engine/cmake/ToolOutputDir.cmake` |
| **Build tree** (`-B`) | `<repo>/build/tools/<ToolName>/` | convention only, documented here and in each tool's README |

A tool that ships more than its `.exe` passes `SUBDIR` and gets its own level,
so its extra files cannot be mistaken for another tool's.
`ZEnvironmentCollector` is the only one today:

```
bin/Tools/Release/
├── TexPreview.exe
├── ClientProjBranches.exe
└── ZEnvironmentCollector/
    ├── ZEnvironmentCollector.exe
    ├── ZDllLoadProbe.exe          # re-launched from the exe's own directory
    ├── ZRuntimeShared.dll         # engine slice (log + localization table)
    ├── resource/ui_strings_zh-CN.zasset   # cooked by DataTableCook at build time
    ├── profiles/default.ini       # read from the exe's own directory
    └── LICENSE-*.txt
```

`z_set_tool_output_dir()` resolves the root as: `Z_ROOT_DIR` if set (full-tree
configure) → else walk up from `CMAKE_CURRENT_SOURCE_DIR` to the directory
containing `engine/CMakeLists.txt` (standalone configure, no hardcoded path) →
else `CMAKE_BINARY_DIR/bin/Tools`. If `BINARY_ROOT_DIR` is already defined it
wins, so the tools follow `ZENGINE_OUTPUT_SUBDIR` too: a cross-compile layout
`bin/win64/x64/` becomes `bin/win64/x64/Tools/<Config>/`.

**Build any of them with one command** (all four land under
`bin/Tools/<Config>/`, and the command prints the exe path when it is done):

```powershell
python zbuild.py tools                          # list them
python zbuild.py tool TexPreview --config release
python zbuild.py tool ZEnvironmentCollector     # --config defaults to release
python zbuild.py tool --all                     # all of them, stops at the first failure
```

`--all` includes the two full-tree tools, so the first run also configures the
whole engine — that configure, not the tools, is the slow part. `--reconfigure`
forces a fresh configure of either kind.

`zbuild.py` knows that the four do *not* share a build tree, only a source
directory and an output directory:

- `TexPreview`, `ZEditor` — **full tree**. They link ZRuntime, so they
  cannot configure standalone; `zbuild.py` configures the tree if it has not
  been configured yet, then builds `--target <Name>` in `build/`.
- `ClientProjBranches` — **own configure** into `build/tools/<Name>/`.
- `ZEnvironmentCollector` — **both**, and they are not equivalent:

```powershell
# engine-backed: what ships. Links ZRuntimeShared (log + DataTable localization).
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DZENGINE_STATIC_CRT=ON
cmake --build build --config Release --target ZEnvironmentCollector

# standalone: self-contained, no engine DLL. Kept so the directory still
# configures on its own; the localization table is compiled into the binary and
# logging uses a statically linked bqlog.
cmake -S engine\tools\ZEnvironmentCollector -B build\tools\ZEnvironmentCollector -G "Visual Studio 17 2022" -A x64 -DENVIRONMENT_CHECK_BUILD_GUI=ON
```

  `ClientProjBranches` stays out of the full tree on purpose (OFF there).
  `ZEnvironmentCollector` used to as well, because its GUI was `/MT` while the
  tree's ZSlate is `/MD` (`LNK2038`); `-DZENGINE_STATIC_CRT=ON` removes that
  reason — with it, tree and tool agree on /MT and the GUI can be built in the
  tree (`-DENVIRONMENT_CHECK_BUILD_GUI=ON`) alongside the engine back-end.
  `--reconfigure` forces a fresh configure of either kind.

Why a `Tools/` level instead of `bin/<Config>/` next to ZEditor: the tools share
nothing with the engine's runtime layout (no configs, no assets, no ZRuntime
DLL), and `ZEnvironmentCollector` is built `/MT` to ship to machines with no VC
redist — its output directory *is* its distribution unit, because `profiles/`
and the LICENSE files are copied next to the exe. "Zip
`bin/Tools/<Config>/ZEnvironmentCollector`" is therefore a correct packaging
step, and `install(COMPONENT EnvironmentCollector)` produces the same folder.

`scripts\build_lib\win64\win_build_zeditor.bat` builds ZEditor through the full
tree with `-DZENGINE_OUTPUT_SUBDIR=win64/<arch>`, so its output is
`bin\win64\<arch>\<Config>\ZEditor.exe`; its echoed `Output:` lines and
`OUT_DIR` must stay in sync with this rule. (It used to build the standalone
`LuaMemPreview.exe`; that tool is a suite now — §7.5.)

### 7.5 Tool suites: one ZEditor executable, N tool sets

`engine/tools/` used to hold one standalone `.exe` per GUI tool. The duplication
that mattered was not the Win32 host (§7 fixed that) but the fact that **each exe
has to be configured, built and distributed separately** — and then nobody knows
which version is on a given machine.

Tools that only need a window and do not have to ship on their own are therefore
**tool suites inside ZEditor** (`engine/Source/Editor/ToolSuite/`), not exes:

| Piece | Where |
|---|---|
| `ToolSuiteDesc` / `IToolSuite` / registry | `Source/Editor/ToolSuite/ToolSuite.h`, `ToolSuiteRegistry.cpp` |
| Level editor's own pseudo-desc (it is the editor, not a suite) | `LevelEditorSuiteDesc()` |
| Spine → `.zspg` container export | `Source/Editor/ToolSuite/SpineCook/` |
| Lua memory snapshot viewer / CSV export | `Source/Editor/ToolSuite/LuaMemPreview/` |

A suite self-registers with a file-scope object (`RegisterToolSuite`), so
**adding one is one new `.cpp`** — `Main.cpp` and the launcher do not change.
The registry's container is a function-local static, so it is constructed before
any registrar runs regardless of translation-unit order.

How a run picks a suite:

* **Launcher** (no `-p`): the picker lists the suites first (level editor
  default-selected, since that is what most launches want), then the projects.
  A suite with `needs_project == false` starts immediately on selection — making
  the user pick a `.zproject` for a baker would imply the baker is project-scoped.
* **Command line**: `ZEditor -batchmode -suite=<id> ...` runs
  `IToolSuite::RunBatch()`; `ZEditor -suite=<id> ...` runs `RunInteractive()`.

⚠️ **The `-batchmode` branch must be taken before `RegisterRuntime()`.**
`START_SYSTEM()` brings up the RHI, the world and the asset systems; a
"read file → compute → write file" baker needs none of them, and if one of them
fails to initialise an unrelated export job fails with it. The dispatch lives at
the top of `WinMain`/`main` in `Source/Editor/Main/Main.cpp`.

Which tools must **not** become suites:

* `ZEnvironmentCollector` — `/MT`, so linking ZSlate in a full tree is `LNK2038`
  (it ships to machines with no VC redist; that is a reason, not an accident).
* `DataTableCook` / `ZHeaderTool` — build-time, invoked by `POST_BUILD`; a GUI
  suite would be a detour.

Suite argument handling: `RunBatch`/`RunInteractive` receive UTF-8 strings
(`ZAppShell::CommandLine::Utf8()`), and paths go through
`ZEditor::PathFromUtf8()` / `PathToUtf8()`. **Do not** build a
`std::filesystem::path` straight from a narrow string: which codepage MSVC's
`path` uses for that is an implementation detail, and a path is exactly what
these tools take.

## 8. Where a tool lives: one home

All **standalone** GUI tools live in **one** directory, `engine/tools/`:

| Tool | Needs | Added by | In a full tree |
|---|---|---|---|
| `engine/tools/TexPreview` | ZRuntime (ASTC/BC7/ETC2 decompressors) | `engine/CMakeLists.txt`, desktop branch | always |
| `engine/tools/ZEnvironmentCollector` | ZSlate only, `/MT`, shipped to players | `engine/CMakeLists.txt` | ON, but GUI OFF (below) |
| `engine/tools/ClientProjBranches` | ZSlate only | `engine/CMakeLists.txt` | OFF by default |

`engine/tools/` is for tools that are **executables of their own**. A tool that
only needs a window and does not have to ship separately belongs in ZEditor as a
**tool suite** (`engine/Source/Editor/ToolSuite/`) — see §7.5.

They used to be split across `engine/Source/Tools/` and root `tools/`, and the
old rule ("two homes, split by dependency") is **gone** — one directory is
easier to manage than two, which is what the split was costing.

**Why `engine/tools/`, not root `tools/`:** `ENGINE_ROOT_DIR` is set at
`engine/CMakeLists.txt:1`, i.e. in the engine's own directory scope. A sibling
directory added from the root does **not** inherit it, so TexPreview /
LuaMemPreview would have had to redefine it — exactly the kind of duplicated
path this repo keeps removing. Inside `engine/` every tool inherits
`ENGINE_ROOT_DIR`, `Z_ROOT_DIR`, `BINARY_ROOT_DIR` and the `cmake/*.cmake`
helpers for free.

**The ordering constraint is still load-bearing.** The block that adds
`engine/tools/ZEnvironmentCollector` and `engine/tools/ClientProjBranches`
sits *after* `add_subdirectory(Source/Runtime)`, where `ZSlate` is defined. Both
link ZSlate, and each guards its own `add_subdirectory(.../Libraries/ZSlate)`
with `if(NOT TARGET ZSlate)` for the standalone case — move that block above
`Source/Runtime` and they would create a second `ZSlate`, colliding with the
engine's (`add_library cannot create target "ZSlate" because another target with
the same name already exists`).

`tools/` at the root is **not** a CMake directory any more and has no
`CMakeLists.txt`: it holds the renderdoc submodule, `tools/renderdoc_build`,
`tools/renderdoc-mcp` and the `analyze_*.py` capture-analysis scripts.

### Two options that must stay OFF in a full-tree configure

- `ENVIRONMENT_CHECK_BUILD_GUI` (the ZSlate front-end of
  `ZEnvironmentCollector.exe`): the tool sets `CMAKE_MSVC_RUNTIME_LIBRARY` to
  `/MT` because it ships to machines with no VC redist. Standalone, ZSlate is
  added inside that scope and inherits `/MT`. In a full tree ZSlate already
  exists as `/MD`, so linking it into a `/MT` collector fails with
  `LNK2038 (RuntimeLibrary mismatch)`. One ZSlate instance cannot be both. The
  executable is still built when this is OFF — it just swaps in
  `EnvironmentCheckGuiFallback.cpp` and serves `-batchmode` only. Build the GUI
  from its own configure:
  `cmake -S engine/tools/ZEnvironmentCollector -B <dir>`.
- `ZENGINE_BUILD_CLIENT_PROJ_BRANCHES`: a self-contained CMake project with its
  own C++ standard and output layout; normally configured standalone
  (`cmake -S engine/tools/ClientProjBranches -B build`). Available in-tree, but
  OFF so a full configure does not pick up unrelated work.




## Reflection code generation (ZHeaderTool)

Decided 2026-10-20 (phase 1 landed). UHT-style generator, **Python 3.9+,
stdlib only** (host-side, so Android / OHOS / iOS / Emscripten cross builds
need nothing extra). Do not re-litigate Python vs C++.

- Tool: `engine/tools/ZHeaderTool/` (`ZHeaderTool.py`, `zht/` lexer -> parser
  IR -> codegen). Unit tests: `python3 engine/tools/ZHeaderTool/tests/test_zht.py`.
- CMake: `cmake/ZHeaderTool.cmake`, `zengine_header_tool(MODULE ... TARGETS ...
  INCLUDE_ROOT ... HEADERS ...)`; wired for ZRuntime + ZRuntimeShared in
  `engine/Source/Runtime/CMakeLists.txt`. Outputs go to
  `<build>/Generated/ZHT/<Module>/{Inc,Src}` (Inc is a PUBLIC include dir).
- Markers (`Runtime/BaseClasses/ObjectMacros.h`): `ZCLASS(Abstract, CustomTransfer, NoSuperTransfer)`,
  `ZGENERATED_BODY(Name)` (replaces `REGISTER_CLASS` + `DECLARE_OBJECT_SERIALIZE`),
  `ZPROPERTY(Name="..", HideInEditor, ReadOnly, StrongPPtr, Deprecated,
  TreatIntegerAsBool, Hex64, Transient, meta=(Tooltip, DisplayName, Category,
  EditCondition, ClampMin, ClampMax, UIMin, UIMax))`.
- `meta=(...)` is parsed, validated at generate time AND consumed by the
  Inspector (phase 2, 2026-10-20). The generator emits a `ZPropertyMeta` table
  per class (`Runtime/BaseClasses/PropertyMeta.h`, keyed by *serialized* field
  name, registered at static-init by `ZPropertyMetaRegistrar`) and
  `ZSlateInspectorWindow` reads it while walking the TypeTree:
  `DisplayName` -> row label, `Tooltip` -> hover label (hit-tested against the
  row's cached geometry, painted in the overlay foreground layer), `Category`
  -> groups the component's depth-0 fields under a dim sub-header,
  `ClampMin`/`ClampMax` -> enforced right after every scalar commit, and
  `EditCondition="[!]m_Bool"` -> the row label dims and commits are refused
  while the named bool (a **member** name, resolved to its serialized name by
  the tool) reads false. `UIMin`/`UIMax` are reserved (parsed, not drawn).
  Generate-time validation: no `meta` on a Transient property, clamps/UI range
  only on numeric scalars, `Min <= Max`, `EditCondition` must name another
  serialized `bool` of the same class.
- Meta/serialization drift gate: `ReflectionCodegenSmokeTest
  --check-property-meta` cross-checks every generated table against the live
  TypeTree (annotated name must be a real field, EditCondition must be bool).
- `ZFUNCTION(...)` (phase 3, 2026-10-20) annotates a **member function** and
  makes it callable from TypeScript. Grammar:
  `ZFUNCTION(Name="ScriptName")` followed by a declaration (`;`) or an inline
  body (`{ ... }`). Leading `static/virtual/inline/constexpr/explicit` and
  trailing `const/noexcept/override/final` are accepted and ignored;
  constructors, destructors, operators and `= 0` are rejected at generate time.
  Overloads are disambiguated by the generated `static_cast` to the exact
  function-pointer type, but only **one** overload may claim a given script
  name. Parameter types and the return type must have a `ZScript::Converter`
  (bool / float / double / integers / size_t / std::string / eastl::string /
  Vector3 / Quaternion / `T*` with T derived from Object); anything else is a
  **compile** error by design (`static_assert`), so an unbindable signature
  cannot slip in silently.
- Generated output per class: a `ZScriptMethodEntry[]` (name, TS signature,
  isStatic, pesapi trampoline) plus a `ZClassScriptBinding`
  (`Runtime/Scripting/Binding/ScriptBinding.h`) that self-registers at
  static-init through `ZClassScriptBindingRegistrar`. Consumed twice:
  * `ScriptBindingRegistry::InstallAll(reg_api, registry)` -- called once from
    `ScriptEnv`'s init block, right after `create_registry()`. It defines every
    class with `pesapi_define_class` / `set_property_info_size` /
    `set_method_info`, and first declares any super that has no binding of its
    own (GameObject -> Object) so pesapi can resolve the chain.
  * `ScriptEnv` then calls `pesapi_set_registry(env, registry)` per env -- the
    registry was previously created but never attached to any VM, so without
    this no class object could ever resolve -- and
    `ScriptBindingRegistry::InstallGlobals(apis, env)`, which publishes each
    bound class as a **global of its script name**.
  * `ScriptBindingRegistry::AppendTypeScriptDeclarations()` appends the
    `declare class` block to `zengine.d.ts` in
    `ProjectInfo::EnsureScriptsScaffold`, so the .d.ts and the runtime globals
    agree by construction. `Vector3` / `Quaternion` are marshalled as plain JS
    objects, so the template declares them as `interface`, not class.
- Reserved-name guard: TS/JS globals (`Object`, `String`, `Math`, `console`,
  ...) are skipped by the .d.ts placeholder pass, the class-declaration pass
  **and** `InstallGlobals`; a class whose super is one of them is emitted
  without `extends`. All three go through `ScriptBindingRegistry::
  IsReservedScriptName` (exposed so the gates consult the same list instead of
  a copy that would drift), and `--check-script-bindings` fails if a bound
  class actually carries one -- it would be invisible to scripts on both
  halves. This is why `declare class GameObject` has no base even though C++
  derives from `Object`.
- `Behaviour` is **not** in `kZEngineDtsTemplate` any more: it is a synthetic
  class and `AppendTypeScriptDeclarations` emits its `declare class` from the
  same ZSyntheticProperty table C++ injects from, so keeping the hand-written
  block was a duplicate declaration (TS "Duplicate identifier").
- Binding drift gate: `ReflectionCodegenSmokeTest --check-script-bindings`
  validates every table (null callbacks/names, self-inheritance, duplicate
  script names, empty or brace-unbalanced .d.ts); `--dump-script-dts` prints
  the generated block on its own. Both are wired into CTest via `TEST_ARGS`.
- **`ZPROPERTY` -> script properties** (phase 7, 2026-10-20). A ZPROPERTY
  field whose type has a `ZScript::Converter` is published as a pesapi
  accessor property (`light.radius = 8`) alongside the bound methods.
  * The whitelist is `parser._SCRIPT_PROPERTY_TYPES` (bool / integers /
    floats / std::string / eastl::string / Vector3 / Quaternion). It is
    deliberately narrower than `_TS_TYPES` and fails **silently**: ZPROPERTY
    marks a field *serialized*, not script-facing, so an unannotated
    `enum class`, a `PPtr`, a `Res` struct or a `std::vector` field must not be
    an error. (A **ZENUM**-annotated enum does qualify -- see below.)
    `T*` is absent on purpose (needs the pointee complete + an Object check
    the parser cannot make).
  * Script names are `script_property_name(serialized_name)`: leading `m_`
    dropped, then camelCase -- `m_script_guid` -> `scriptGuid`, `clip_path`
    -> `clipPath`. Same split Unity makes between YAML and C#. Two fields
    collapsing onto one script name is a generate-time error.
  * `ZPROPERTY(ReadOnly)` yields a read-only property (no setter at all).
  * The accessors are generated as **public** `ZhtScriptGet_<member>` /
    `ZhtScriptSet_<member>` declared in `ZGEN_BODY_<Class>` and defined in the
    .gen.cpp. Public is mandatory: the .gen.cpp takes their address from file
    scope, and C++ checks access at the point of naming -- the fields are
    usually private, and only the *definition* (a member context) may touch
    them. This is why the generated body now emits a `public:` block.
  * Scope: **every** ZCLASS with a marshallable field is script-visible,
    ZFUNCTION or not. An asset or a settings object (ShaderRes, TimelineClip,
    CameraParameter) is reached through a bound getter of another class, so
    gating this on "already script-visible" left its fields unreachable from
    TypeScript even though the Inspector edits them. The predicate to change
    is still `codegen._script_field_properties` -- but note `cli.py`'s
    `bound_classes` must agree (it drives the super-chain flattening and the
    .d.ts `extends` clauses), which is why the answer comes from
    `has_script_field_properties()` rather than being re-derived.
  * `ZPROPERTY(Transient)` fields are **excluded**: Transient means "reflected
    but not serialized", and the Inspector hides it too. Publishing it as
    `obj.foo` would hand scripts a value that means something different on a
    fresh object than on a loaded one.
  * `.gen.cpp` `#include`s ScriptBinding.h whenever any class in the header
    contributes methods, ZSCRIPTPROPERTY pairs **or** field properties --
    `needs_script` in `generate_source` covers all three.
- **`ZSCRIPTPROPERTY(Name="..", Getter=".."[, Setter=".."])`** publishes an
  existing ZFUNCTION getter/setter pair as an accessor property, with no data
  member behind it. This is how `transform.position` exists at all: Transform
  has no ZPROPERTY fields (its Transfer is hand-written). The named accessors
  must be ZFUNCTION-annotated -- that is where the TypeScript type comes from
  -- and the parser rejects a getter with parameters or a void return, a
  setter without exactly one parameter, and duplicate/colliding names.
  Omitting `Setter` gives a read-only property.
- A property getter/setter is the ordinary `ZScript::MethodCallback`
  trampoline: a getter is just a zero-argument method returning a value, a
  setter a one-argument method. `InstallAll` passes `is_static=0` --
  `pesapi_set_property_info` files a static entry under `Variables` (class
  level) instead of `Properties` (prototype level), so a 1 there would
  produce a property no instance can see. A null setter makes the V8 backend
  install the accessor with `v8::ReadOnly`.
- Gate `--check-script-properties`: name/type/callback presence, readonly
  must agree with the setter, no collision with a bound method of the same
  class, and the property line must actually appear in the generated .d.ts.
  Wired into CTest via `TEST_ARGS`.
- **`ZENUM` -> script enums** (phase 8, 2026-10-20). A file-scope enum (or one
  in exactly one named namespace) may be annotated:
  `ZENUM() enum class CameraMode : unsigned char { third_person, ... };`
  * It crosses the boundary as `int32` through a generic
    `ZScript::Converter<T, enable_if<is_enum<T>>>`. That specialisation must
    stay **disjoint** from the `std::is_integral` one -- both live in the same
    `enable_if` slot, so an overlap is an ambiguous partial specialisation.
  * A number alone is not an API, so ZHeaderTool also emits a
    `ZScriptEnumEntry[]` (`.name`, `.value`) + `ZScriptEnum` + registrar.
    `InstallGlobals` publishes it as a **global object** and
    `AppendTypeScriptDeclarations` emits `declare enum CameraMode {
    ThirdPerson = 0, ... }` from the *same* table, so `camera.mode =
    CameraMode.Free` both type-checks and evaluates. A `declare enum` with no
    runtime object would compile and then be `undefined`.
  * Enumerator names are PascalCased for scripts (`third_person` ->
    `ThirdPerson`); the C++ / serialized spelling is untouched. An initializer
    must be a **literal integer** -- `1u << 0` is a generate-time error,
    because the .d.ts has to print a concrete number and the runtime object
    needs a concrete value; guessing one would desync the two.
  * A ZENUM-annotated enum is also a valid *field* type: `ZPROPERTY(Name=
    "mode") CameraMode m_Mode;` becomes `mode: CameraMode`, not `mode: number`,
    so `mode = 7` is a type error while `mode = CameraMode.Free` compiles. The
    module-wide enum table is threaded into codegen as `script_enums`
    (C++ spelling -> script name) from `cli.py`.
  * **Not** supported: enums nested in a class (`Transform::SetParentOptions`
    -- they are published as globals of their bare name, so a member enum has
    no unambiguous script name) and a ZENUM name that collides with a class
    (both become globals of the same name). Both are generate-time errors.
- Gate `--check-script-enums`: every table has a name and at least one
  enumerator, every member is a valid TypeScript identifier and appears in the
  generated .d.ts as `    <Name> = <value>,`, no duplicate members. Wired into
  CTest via `TEST_ARGS`.
- **Scalar typedefs must be listed in `_TS_TYPES`** (`zht/parser.py`). An
  unmapped bare identifier is emitted into the .d.ts **verbatim**, which is the
  right behaviour for class and struct names (`Vector3`, `Transform`) but leaks
  an undeclared type name for an integer alias -- `AudioVoiceId` (= `uint64_t`)
  used to produce `Play(): AudioVoiceId`. `GObjectID` and `AudioVoiceId` are in
  the map; add any new `using X = <integer>` there when it first crosses the
  boundary. Failure mode is silent (the .d.ts still generates), so this is not
  catchable by `--check-script-bindings`.
- Bound surface (2026-10-20): 42 class tables / 62 methods / 122 properties /
  2 enums (d.ts ~7.6 KB). 9 of those classes are hand-annotated with ZFUNCTION
  -- `GameObject`, `Component`, `Transform`, `TypeScriptComponent`,
  `LightComponent`, `CameraComponent`, `AnimationComponent`,
  `AudioSourceComponent`, `RigidBodyComponent` -- and the rest are published
  purely because they own a marshallable ZPROPERTY field (assets / settings:
  `ShaderRes`, `Texture2D`, `TimelineClip` + subclasses, `CameraParameter` +
  subclasses, `Font`, `LevelRes`, ...), plus the synthetic `Behaviour` and the
  `CameraMode` / `RigidBodyActorType` enums. Deliberately unbound:
  * `const char*` setters (a Converter would have to own a buffer that outlives
    the call; add an `eastl::string` overload instead).
  * `Transform::SetParentOptions` -- a *nested* enum, which ZENUM rejects (no
    unambiguous global name). Promote it to file scope if scripts need it.
  * `ParticleComponent` / `MeshRenderer` expose no public accessors; not worth
    a binding yet.
- **Physics API** (phase 10, 2026-10-20). `RigidBodyComponent` is the script
  face of a Jolt body; `PhysicsScene` is the C++ face.
  * `RigidBodyActorType` (`static_body` / `kinematic` / `dynamic`) is a
    **ZENUM** in `Runtime/Resource/ResType/Components/RigidBody.h`, so scripts
    see `RigidBodyActorType.Dynamic`. The serialized field stays an `int`
    (`m_ActorType`) -- changing the field type would break every level already
    on disk -- and `GetActorType()` / `SetActorType()` convert through
    `ToRigidBodyActorType` / `ToSerializedActorType`. `PhysicsScene.h` only
    carries `enum class RigidBodyActorType : unsigned char;` (an enum with a
    fixed underlying type is complete once declared, so the header does not
    pull in PPtr / Geometry / the generated header).
  * The actor type now **drives** `CreateRigidBody`: it selects
    `JPH::EMotionType` and the `Layers::MOVING` / `Layers::NON_MOVING` object
    layer, and a dynamic body with an authored `m_InverseMass > 0` gets
    `EOverrideMassProperties::CalculateInertia` + `mMassPropertiesOverride.
    mMass` (Jolt derives the inertia tensor, scaled to that mass). It used to
    be hardcoded static with a `//$TODO`; static stays the default, so
    existing levels are byte-identical in behaviour.
  * Motion type is changed through `BodyInterface::SetMotionType`, **not**
    `Body::SetMotionType` -- the Body one is only safe on an inactive body.
    Same reason `AddForce` / `AddImpulse` take a `JPH::BodyLockWrite`: they
    live on `JPH::Body`, not on `BodyInterface`. `GetBody()` returns a
    **reference** and `Succeeded()` already proved it non-null -- do not
    null-check it.
  * Every new `PhysicsScene` method returns `bool` and is a no-op on
    `s_InvalidRigidbodyId` or on a body the query does not apply to (a static
    body has no motion properties, so `GetInverseMass` reports 0 and
    `AddForce` does nothing). Nothing asserts: a body can legitimately be
    pending creation (`PostLoadResource` runs before `WorldManager` publishes
    the scene) or already torn down while a script still holds the component.
  * `RigidBodyComponent` guards everything behind `GetActivePhysicsScene()`,
    which is null when there is no body id. `GetActorType` reads the *authored*
    value so it stays meaningful while pending; `GetMass` prefers the live
    body and falls back to the authored inverse mass.
  * **`engine/Source/Platform/Web/PhysicsStubs.cpp` must be kept in lock-step**
    -- every added `PhysicsScene` method needs a stub there or the Emscripten
    build fails to link. It is only compiled with `__EMSCRIPTEN__`, so a macOS
    build will not catch the drift.
- **Generator v7 invariants** (both are easy to break silently):
  * A `.gen.cpp` `#include`s the header of every named type a ZFUNCTION
    signature mentions -- classes *and* ZENUMs (`cli.py` feeds both into
    `type_headers`). `ZScript::Converter<T*>` static_asserts on
    `std::is_base_of<Object, T>`, which is a **hard error on an incomplete
    type** -- and the reflected headers forward-declare each other in a cycle
    (Component.h / GameObject.h / Transform.h). Binding a method that returns
    a forward-declared type fails to compile without this. A scoped enum can
    likewise be forward-declared (`enum class X;`), and `std::is_enum<T>`
    needs the real definition.
  * A binding's `super` is the nearest ancestor that owns ZFUNCTIONs, not
    necessarily the immediate C++ base. `TypeScriptComponent -> Behaviour ->
    Component` would otherwise declare `Behaviour` as a root placeholder and
    silently drop every Component method from the pesapi prototype chain; the
    chain is flattened to `TypeScriptComponent -> Component`, and the .d.ts
    agrees.
- **`Behaviour` is synthetic** (script-side, no C++ binding table): the JS
  `class Foo extends Behaviour` instance is a plain object created by
  `__zNewInstance`, so the engine cannot own `this`. `TypeScriptComponent::
  BindAndAwake` instead publishes `gameObject` / `transform` / `component`
  onto every instance right after construction and *before* OnAwake, via
  `ScriptingManager::SetInstanceObject<T>(instance, name, ptr)` ->
  `ScriptEnv::SetInstanceNativeObject` (`pesapi_native_object_to_value` with
  `call_finalize=0`: JS never owns an engine object). The property names are
  `ZBehaviourScript::k{GameObject,Transform,Component}` and the matching
  `declare class Behaviour` in the .d.ts is generated from the same
  `ZSyntheticProperty` table (ScriptBinding.cpp), so the two cannot drift.
  `SetInstanceObject<T>` requires `T` to own at least one ZFUNCTION -- pesapi
  can only wrap a type id that `define_class` has seen, otherwise the property
  is published as null and an error is logged.
  * Gate `--check-behaviour-binding`: the three injected names must be
    declared, their declared type must match what C++ publishes, must be a
    registered C++ type, and must appear in the emitted .d.ts.
- `Cast<T>(p)` (phase 4, 2026-10-20) replaces `dynamic_cast` **inside the
  Object hierarchy**. `Runtime/BaseClasses/Cast.h`, global namespace, two
  overloads (`U*` and `const U*`, the latter returning `const T*`). It is an
  O(1) `Type::IsBaseOf` range test on `p->GetMostDerivedType()` plus a
  `static_cast`, and both sides are `static_assert`ed to derive from `Object`,
  so a non-Object hierarchy cannot be silently "converted". Unrelated T/U are
  accepted and always yield nullptr (the pointer conversion is guarded by
  `if constexpr`, because `static_cast` -- unlike `dynamic_cast` -- rejects
  unrelated types outright).
  * It reads the **virtual** `GetMostDerivedType()` (a public non-virtual
    wrapper over the private virtual `GetTypeVirtualInternal()`; access is
    checked on the static type, which is why the wrapper lives in `Object`),
    never `Object::GetType()`: the latter reads `m_CachedTypeIndex`, which is
    only meaningful after `InitializeRuntimeTypeInfo()` has run.
  * `CastDetail::Plain<T> = std::remove_cv<T>::type` for every lookup:
    `TypeOf<const T>` would reach a *different*, unregistered
    `TypeContainer<T>` instance. Call it as `Cast<T>(const_ptr)`, not
    `Cast<const T>(...)`.
  * Equivalence gate: `ReflectionCodegenSmokeTest --check-cast` cross-checks
    `Type::IsBaseOf` against a base-chain walk for every (runtime type, target)
    pair and spot-checks `Cast<T>` against `dynamic_cast<T*>` on 10 real pairs
    plus a null. Wired into CTest via `TEST_ARGS`.
- **Migrated to `Cast<T>`**: 63 call sites across 17 files (2026-10-20).
  **Still `dynamic_cast`** (86 sites, deliberately -- these hierarchies are not
  Object-based and `Cast<T>` would `static_assert`): `IEngineSystem` /
  `AssetManager` -> `EditorAssetManager` (SystemRegistry's `Cast<T>` reverted
  for the same reason, and `Core/Base` must not include `BaseClasses`),
  `*ImporterSettings`, `DX12RHI` / `VulkanRHI` / `RenderResource` /
  `RenderPipeline` / `DX12MainCameraPass`, `UISystem` / `BatchedUIRenderer`,
  `FAssetDragDropOp`, and the UMG widgets (`UWidget` / `UPanelWidget` /
  `UBoxPanel` / `UOverlay`). Do not "finish the job" by converting those.
- Rules: `#include "<Name>.generated.h"` (base name of the header, one per
  header even with several ZCLASSes) must be the **last** include; reflected
  classes live at global scope or in **exactly one named namespace**
  (`IMPLEMENT_REGISTER_CLASS_3` token-pastes it) and derive from an
  Object-based class; base names of reflected headers must be unique per module.
- `NoSuperTransfer`: the generated Transfer() skips `Super::Transfer()`.
  Escape hatch only; no class uses it today. Behaviour-derived components
  (TypeScriptComponent, RenderCamera, AIControllerComponent) serialize
  Component's `enabled` since 2026-10-20 -- YAML / TypeTree-backed .zasset
  written earlier load with the default (true); a TypeTree-less legacy
  StreamedBinary payload of those three types would be misread.
- `CustomTransfer`: only registration is generated; IMPLEMENT_OBJECT_SERIALIZE,
  Transfer() and INSTANTIATE stay hand-written (GameObject, Transform,
  Material, WorldRes -- WorldRes serializes the base member `Object::name`).
- Helper structs (DECLARE_SERIALIZE) whose Transfer() template is defined in
  a .cpp are no longer instantiated implicitly once the owning ZCLASS's
  Transfer() moves to its .gen.cpp: add
  `INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(Struct, , Transfer, void)` in that
  .cpp (see AnimatorController.cpp, GlobalRendering.cpp).
- Targets that compile reflected headers without linking the module (the
  shared PCH) go in `INCLUDE_CONSUMERS`.
- **Serialized order == ZPROPERTY declaration order.** When migrating a
  hand-written `Transfer()`, reorder members to match the old Transfer order and
  pin old field names with `Name="..."` (default name = snake_case of the member
  without `m_`). In-memory byte offsets may change (not persisted:
  `TypeTree::WriteTypeTree` does not write them); the serialized TypeTree and
  payload must not.
- Migration gate: `-DZENGINE_BUILD_REFLECTION_CODEGEN_SMOKE_TEST=ON`, build
  `ReflectionCodegenSmokeTest`, run `--no-offsets <Classes...>` before and
  after migrating and compare (`--golden <file>` does the diff). Goldens are
  ABI-specific, keep them local.
- Adding / removing `ZCLASS` in an existing header: the first build fails once
  with "the set of ZCLASS headers changed"; the next build re-configures
  automatically. That is expected.
- Migrated: every registered ZRuntime class (55), including the formerly
  unregistered bases `Component`, `Geometry` (Abstract), `ControllerConfig`
  (Abstract) and `RenderTexture`, plus the Timeline model (`TimelineClip` /
  `TimelineTrack` Abstract + their 4+4 concrete subclasses, `TimelineComponent`).
  Not migrated: DataTable wrappers (generated by DECLARE/IMPLEMENT_DATA_TABLE),
  and the Editor classes (`AssetImporter`, `PrefabInstance`) until ZEditor is
  wired.
- Timeline assets are object graphs (Unity .playable layout): fileID 1 =
  TimelineAsset, then each track followed by its clips, all linked by PPtr.
  Always go through `TimelineAssetIO::Save/Load`
  (Runtime/Resource/ResType/Data/TimelineAssetIO.h) -- `loadAsset<T>` /
  `ReadObject(path, type)` only hydrate fileID 1. `.timeline` = YAML authoring
  asset, `.zasset` = binary. Create tracks / clips with
  `ObjectManager::NewObject<T>()`: a `MemoryManager::CreateObject` result has
  InstanceID 0, so the PPtr stored in m_Tracks / m_Clips is null.
- Asset file extensions have one source of truth:
  `AssetManager::YamlAuthoringAssetExtensions()` /
  `RegistryIndexedAssetExtensions()` (+ `Is...Extension` / `Is...Path`).
  Never hard-code ".mat/.prefab/.scene" lists in editor scanners, watchers or
  filters.
- Type-registry invariant (checked by `ReflectionCodegenSmokeTest
  --check-registry`): no "[UNREGISTERED]" placeholder anywhere in a base
  chain, every producible type is under Object, every base's range contains
  its descendants, `RuntimeTypeArray::count` == populated slots. A class with
  REGISTER_CLASS but no IMPLEMENT_REGISTER_CLASS, or a template layer that
  relays `ThisType = itself` (DataTable<TRow> once did), breaks it.

## 9. RenderDoc (vendored) — Android server APK must match the host binary

`tools/renderdoc` is a **git submodule**, currently detached at `16a859d4f`
(upstream v1.46 + 212 commits, builds as **1.47**). The host binaries in
`tools/renderdoc/x64/<config>` and the copies in `bin/<config>` were built by
`tools/build_renderdoc.bat` **without a version hash**, so `renderdoccmd version`
prints `v1.45 built from NO_GIT_COMMIT_HASH_DEFINED_AT_BUILD_TIME`. Source
version and binary version are therefore allowed to drift — always read the
version off the binary, never assume it matches the submodule.

For Android capture the remote server APK must have **exactly** the host's
`versionCode` + `versionName` (`Android::CheckAndroidServerVersion`,
`renderdoc/android/android.cpp`: `versionCode = MAJOR+MINOR`, e.g. `"145"`;
`versionName = GIT_COMMIT_HASH`), otherwise qrenderdoc uninstalls and reinstalls
the server, which fails on MIUI/HyperOS. Build the APK from a tree at the
*host's* version:

    tools\build_renderdoc_android.ps1 [-Config Release] [-Install] [-Clean]

It parses `x64\<config>\renderdoccmd.exe version`, checks out that tag into the
scratch worktree `<engine_root>/renderdoc-android-src`, builds arm32+arm64 with
the same version string, and copies both APKs into every `plugins/android/`
next to a `renderdoc.dll` (first search path in `InstallRenderDocServer`;
**arm32** is the file probed first on 64-bit devices, because
`GetSupportedABIs` returns `{armeabi_v7a, arm64_v8a}`). Toolchain that is known
to work: NDK r25c, SDK build-tools 36 + platform android-37, JDK 11/17, ninja,
CMake 4.x. MIUI/HyperOS rejects `adb install -g`
(`INSTALL_GRANT_RUNTIME_PERMISSIONS`) and, once the package is gone, plain
`adb install` (`INSTALL_FAILED_USER_RESTRICTED`) — 开发者选项 → USB 安装 must be
on, and the script installs via `adb push` + `pm install --force-queryable`.

> 补记：下面 §2.46 / §2.47 曾被上游提交 `cf91441`（"公式层的唯一权威搬进子模块"，
> 一次并行编辑）连带删掉约 114 行时一起丢失，这里是重新补回的版本。若那次截断是有意的，
> 直接删掉这两节即可。

### 2.46 静默的失败分支：症状和"根本没写这段代码"一模一样

2026-10-30 修 iOS 上的 `ProjectInfo` 启动失败，时间几乎全耗在两个**什么都不说**的分支上：

1. `if (auto* cs = GET_SYSTEM(CommandSystem); cs != nullptr) { /* 写回工程路径 */ }`
   —— 放在 `RegisterGameRuntime()` **之前**，而 `CommandSystem` 本身就是 Runtime 系统，
   所以 `cs` 恒为 `nullptr`，**整个块被静默跳过**，日志只剩一行没头没尾的
   `[F] Failed to initialize system 'ProjectInfo', rolling back`。
2. `CommandSystem::Initialize()` 里无条件 `project_path = project_path_str;`，没有 `-p` 时
   是空串，把宿主刚写进来的值又冲掉。同样不打日志。

规则：**取不到东西的分支要打日志**（至少 WARNING，起不来的打 ERROR）。`nullptr` / 空串 /
早退不是"正常无事发生"，它们是诊断信息。**无条件覆盖**一个可能已被别人设过的字段，要想清楚
并写进注释。排查这类问题时，先**证明代码执行到了**——在分支里加一行带实际值的日志，比反复
读代码快。

配套教训：**编译得过 ≠ 起得来。** iOS 那四个运行时阻塞（UIScene 生命周期、缺 `ZEditor.ini`、
`ProjectInfo` 工程路径、粒子纹理空指针）全是编译期零报错、链接期零报错，装上才炸，而 CI 的
编译闸门一个都发现不了（见 §2.45）。沙箱里 `ps` 会报 `operation not permitted`，改用**崩溃
报告比对**（跑前跑后各取一次 `ls -t ~/Library/Logs/DiagnosticReports/ | head -1`）。

Android / OHOS 是同一个洞，目前仍然没有 job。

### 2.47 体积：先拿 map，再谈裁谁

2026-10-30 `ZRuntimeShared` 从 2.6 MB 涨到 8.1 MB，第一反应（包括我自己的）是"裁掉静态合入
的模块，比如 PuertsCore"。**按链接 map 一量，PuertsCore 只有 0.01 MB** —— 猜测和真相差两个
数量级。真实构成（活字节）：引擎自身对象 2.31 / spirv-cross 1.90 / PapiQuickjs 0.76 /
GPUSpine 0.35 / Jolt 0.21 / mimalloc 0.10 / **PuertsCore 0.01**（MB）。

三刀总共拿回 5.53 MB（8.11 → 2.58），结论按数据记：

- **别猜体积，生成链接 map**（`-DZENGINE_IOS_LINK_MAP_FILE=<path>`）。`-dead_strip` +
  导出列表之下，"把 .a 大小加起来"恒为高估，只有链接器保留下来的字节才算。
- **`option()` 默认 ON 不等于它生效了**。`ZENGINE_MOBILE_SIZE_OPTIMIZE` 对 iOS 默认就是 ON，
  但 flag 那段只判 `ANDROID OR OHOS` —— iOS Release 一直停在 `-O3`。判默认值的条件和判 flag
  的条件是两份代码，可以各说各话。补 `-Oz` 拿回 2.0 MB `__TEXT`。
- **符号表是体积的一部分**，别只盯 `__TEXT`。`-exported_symbols_list` 之后剩下的 ~2 万 LOCAL
  符号吃掉 1.9 MB `__LINKEDIT`，链接期 `-x` 拿回 1.7 MB。
- **`-x`（链接期）≠ `strip -S`（post-build）**。后者重写已链接的 Mach-O，可能破坏 iOS 16+
  dyld 要的 `LC_DYLD_INFO`；前者让链接器一次生成对。要裁符号就在链接期裁。
- **裁模块前先确认它是不是在公开 ABI 里**：PapiQuickjs 不能动，因为
  `CreateQjsPapiEnvRef` / `GetQjsFFIApi` 在 104 个导出符号中。导出表是判断依据，不是依赖图。
- **只摘库不摘引用它的 TU = 制造 0 地址调用**。`MetalRHI.mm` 引用 `CompilerMSL`，摘掉
  spirv-cross 后这些符号在 `-undefined dynamic_lookup` 下被绑成 0，调用即
  `EXC_BAD_ACCESS at 0x0`。这条正好被 `nm -m | grep -c "dynamically looked up"` 抓到 ——
  **那条断言不只是防漏链库，也是防"裁过头"**。
- **"反射 TU"是这类裁剪最容易漏的一个**：裁 `SpineRenderer.cpp` 时，真正调它成员函数
  （41 个符号）的是 `SpineRenderer.gen.cpp`，漏掉它等于白裁且留下未定义符号。
  `ZHeaderTool.cmake` 因此加了 `<Target>_ZHT_EXCLUDE_BASENAMES`。
- **`GLOB_RECURSE` 的新文件会进所有目标的源列表**：`PhysicsSceneNull.cpp` 想写
  `#error "只在 slim 下有意义"` 会把宿主 / Editor / web 全编挂 —— 必须整体 `#if` 包成空 TU。
- **`PUBLIC` 的链接库同时提供 include 目录**：想"不链 Jolt"会把 `Jolt/Jolt.h` 的 include 路径
  一起拿走，`PhysicsManager.cpp` 直接编不过。库留在链接线上、靠"没有 TU 引用它"让归档贡献
  0 字节更省事（用链接 map 证明 0 就行）。
- **动一个被广泛包含的头文件前先确认它不在 PCH 里**：`PhysicsScene.h` 若被 ZEnginePCH
  预编译，两个 target 看到不同宏就是 ODR 事故。当时是靠 grep 确认它只在 .cpp 里被直接
  include 才敢加 `ZENGINE_SHARED_NO_PHYSICS` 接缝的。

---

## OSGame 棋盘线 · AI 视觉层（2026-11-05）

用户明确纠正：AI 生成范围不只是几何风格包，**天空盒 / 后处理 / 材质 / Volume 也必须
由 AI 生成**。真源因此拆成三份、互不覆盖：`board-spec.json`（冻结核心几何）、
`style-pack.json`（外围装饰）、`visual-pack.json`
（`zengine.chessboard.visual-pack/v1`，四组视觉参数）。冻结 spec 一律不动。

- AI 素材：`assets/concept.jpg` 与 `assets/sky_panorama.jpg`。云端文生图返回的
  是 **JPEG 不是 PNG**（存成 .png 会骗过尺寸解析），且是 1024×1024 而非 2:1；
  后者经确定性「裁上部 64% → LANCZOS 到 2048×1024 → 128px 左右接缝混合」处理，
  是全景**风格图**而非严格相机投影，契约里如实写 `projection_fidelity`。
  visual-pack 记录 sha256，门禁逐字节校验，不能伪造。
- Unity 落地全部是离线纯文本（`unity_scene.py`，不碰 Editor API）：
  ① `!u!104 RenderSettings` 覆写雾 / 环境光；② `RenderSettingGroup` 下新增
  `ZEngineVolume`，复用现网 AC10 的 `RenderSettingsVolume` MonoBehaviour 结构
  （脚本 GUID `8b58b33c8d7842a44a07b29f9b2fea73`，可在
  `Program/GameScripts/Packages/Graphics/com.tencent.tmgp.yarp/Runtime/RenderSettings/RenderSettingsVolume.cs.meta`
  解析），只覆写白名单参数，并把 `UserDefinedBounds` / `Root` / `m_GameObject`
  等指向 AC10 的 fileID 清零后重指本地 ID —— 否则会重新引入 Missing；
  ③ 12 个 `.mat` 只改 `_Color` / `_BaseColor`，GUID 与 shader 不变（不要动
  `_AlbedoColor`，它是白色 tint，改了会把颜色乘暗）；④ `sky_gradient.mat` 的
  `_MainTex` / `_AlbedoMap` 绑定 AI 全景贴图（固定 GUID 由
  `assets/texture_importer.meta.tpl` + sha256 生成）。现网 `m_SkyboxMaterial`
  本来就是 `{fileID: 0}`，项目靠天穹球而非 Unity skybox material，别去改它。
- 交付场景 **89 文档 / 0 PrefabInstance / 1 Camera / 1 Artist / 15 HD parts**
  （原 86 + Volume 的 GO/Transform/MonoBehaviour）；`--no-visual` 退回纯几何场景。
- 部署清单从 6 个文件变为 **23**（+visual-pack.json、12 个 .mat、2 张 jpg、2 个 .meta）。
  `deploy_to_trunk.py` 仍默认 dry-run、不自动写 G 盘；先关稳定 Editor 再 `--apply`。
- 门禁：`verify/test_board.py` **78 tests**、`verify/test_zbrd_js.mjs` **20 checks**。
  `web/app.js` 同步消费 visual-pack 的天空色 / 材质色 / 灯光 / 曝光（缺失时降级）。
- High/Mid/Low 与自动降配：用户明确「先不管」；配置表不动。

**Blender 交付（2026-11-06，用户要求「美术会在 blender 里面调」）**：本机只有
`D:/blender/source`（源码），**没有 `blender.exe` / `bpy`**，无法直接写二进制
`.blend`。因此 `integration/chessboard/blender_export.py`（纯 stdlib）产出 3 个文件
到 `build/blender/`，并部署到 `Assets/ZEngineChessboard/Blender/`：
① `AC90_board.glb`（glTF 2.0，15 个 HD 部件各是独立 Mesh/Node，材质颜色由
visual-pack 做 sRGB→linear，节点 extras 带 zengine_part/kind/tag/lod_mask）；
② `build_ac90.py`（bpy 脚本，导入 GLB + World 环境贴图 / 太阳光 / 相机 /
色彩管理 / Compositor Glare 后 `save_as_mainfile` 成 `AC90.blend`，内嵌 AI 参数，
兼容 Blender 3.x/4.x 字段差异用 `set_if` 兜底）；
③ `sky_panorama.jpg`。美术 Run Script 一次即得到可编辑 `.blend`。
**坐标不要再手动旋转**：GLB 保持 glTF 的 Y-up，Blender 的 glTF importer 自动
转 Z-up；只有 Unity 侧才需要 `x→-x` + 反转绕序（Unity 是左手系）。
**幂等坑**：内存 `Part` 是 double、`board.zbrd` 解码后是 float32，两个入口会
写出字节不同的 GLB ⇒ `export_all` 统一先 `decode(encode(parts))` 量化。
**产物污染坑**：自动导出只挂在 `zboard.py --variants` 的正式构建（默认 `--out`、
无 `--no-style` / `--set`），否则测试里的临时构建会覆盖交付 GLB。
**材质补丁坑**：trunk 部署过 AI 颜色后 `_Color` 与期望值完全相同，
`patch_material` 不能靠「文本是否变化」判断白名单属性存在与否，必须先查属性
再覆写。部署清单 23 → **26** 个文件。

---

## OSGame 棋盘线 · 第 2 步：主题风格包 + 混元 3D 契约冻结（2026-11-06）

用户指令「做第 2 步，等 blender 编译好之后，我就部署」：**部署由用户自己做，
本仓库不得自动 `--apply`**。第 2 步 = 主题风格包 + 单体参考图 +
`external_mesh_contract/v1` 冻结，**不是**现在就跑混元 3D。

- `style-pack.json` 从 PLACEHOLDER 换成 **「星海棋亭 Starlit Pavilion」**
  （`status=AI_GENERATED`），`derived_from` 指向 `assets/concept.jpg` 与
  `visual-pack.json`；`palette` / `frame_style.color` / 6 个装饰的 `color`
  逐值对齐 visual-pack（新增门禁断言），**不另立第二套调色板**。
- **刻意保留 6 个 primitive 代理几何与全部 id**（`deco_nw/deco_se/deco_n/
  orb_s/lantern_w/lantern_e`）、`params` / `position` / `min_lod` / `tag`：
  材质名 `zboard_<id>`、12 个 `.mat`、15 个 mesh `.asset` 的 GUID 全部不变，
  已部署资产不会与 ZBRD 漂移。第 2 步只改颜色 ⇒ ZBRD / manifest 重新构建，
  **场景 `.unity`（sha256 `52bfa2c98d96c3bd`）与 GLB（311736 bytes）逐字节不变**。
  真正替换几何要等第 3 步 glb 消费打通。
- 4 张单物体参考图在 `assets/style_refs/`，全部是 1024×1024 **JPEG**
  （右下角有云端自动加的「AI 生成」水印，送图生 3D 前建议裁掉右下 ~12%，
  原始文件与 sha256 不改）：`star_pillar.jpg` / `halo_ring.jpg` /
  `moon_orrery.jpg` / `star_lantern.jpg`。第 3 张曾因
  `concurrent slot limit exceeded (2) for dimension hy-image-v3` 反复失败，
  换了时段重试成功；失败时按 `status: pending` 如实落地，**禁止伪造结果**。
- `external_mesh_contract/v1`（`zboard.py:validate_external_mesh_contract`）：
  `frozen=true` / `asset_format=glb` / `reference_root=package_root`。
  **glb 是独立单体资产，按 `instances[].anchor` 摆放，不烘进 ZBRD 共享几何**；
  anchor 必须等于对应 style item 的 `position`，`zone` / `proxy_primitive` /
  `theme_asset` 必须一致，6 个 style item 必须被恰好覆盖一次；参考图与
  `asset_ready` 的 mesh 都要有 sha256 且逐字节校验，路径只能是包内相对路径
  （`package_relative()` 拒绝绝对路径 / `..`）。状态机
  `pending → reference_ready → asset_ready`，当前 4 个都是 `reference_ready`。
  **消费侧仍 `NOT_IMPLEMENTED`**：style item 上出现 `external_mesh` 依旧被
  `validate_style_pack` 明确拒绝；manifest 新增 `external_mesh` 摘要段
  （`consumer: not_implemented`）。
- 4 张参考图由 `deploy_to_trunk.py` 按**包内相对路径镜像**到
  `Assets/ZEngineChessboard/assets/style_refs/`，保证 trunk 里 style-pack 的
  `reference_image` 也能解析；它们是美术素材、无运行时引用，Unity 自行生成
  `.meta`。部署清单 26 → **30** 个文件（7 张 .jpg / 2 个 .jpg.meta）。
- 门禁：`verify/test_board.py` **90 tests**、`verify/test_zbrd_js.mjs`
  **27 checks**、`zboard.py --check` / `unity_scene.py --check` /
  `blender_export.py --check` / `external_mesh.py --check` 全 OK、
  error 级 lint 0。
  `deploy_to_trunk.py --verify-only` 当前为
  `15 missing / 5 stale of 41 files`，属于用户部署前的正常状态。
- 下一步（需用户授权后再做）：接混元图生 3D；**3D 文件 URL 仅 24 小时
  有效，生成后必须立刻落盘并写 sha256**。消费链路见下一节。

---

## OSGame 棋盘线 · 第 3 步：glb + sha256 + 锚点三端消费（2026-11-06）

用户指令「打通 glb + sha256 + 锚点三端消费」。真源只有一份：
`external_mesh.py` 读 `style-pack.json` 的 `external_mesh_contract/v1`，
逐字节校验 glb，产出 `build/external_mesh.manifest.json`（每实例的
`anchor / rotation_y_deg / scale / pivot_offset / world_bounds`）。
Unity / Blender / Web 都只消费这份清单，**不要在三端各写一套锚点公式**。

- 统一公式：`world = anchor + rotY(rotation_y_deg) * scale * (local - pivot_offset)`。
  代理资产的局部坐标已经是「锚点相对」，`pivot_offset` 恒为 `[0,0,0]`；
  混元资产若原点不在底面中心，用 `pivot_offset` 归位，三端都会照做。
  `world_bounds` 由 Python 按此式算出并写进清单，**JS / Blender / Unity
  的门禁都拿它当对拍基准**（实测 6 个实例与 ZBRD 世界包围盒误差 0）。
- 4 个代理资产（`star_pillar` / `halo_ring` / `moon_orrery` /
  `star_lantern`，`asset_origin=proxy_zbrd`，828 三角形 / 95424 字节）是
  **用 ZBRD primitive 在局部坐标重出的真 glb**，`status=asset_ready`。
  混元就绪后：覆盖 `assets/external_meshes/<id>.glb`，把 `mesh_sha256` /
  `cache_sha256` 改成新值、`asset_origin=hunyuan_3d`，再跑
  `zboard.py --variants` + `unity_scene.py`；`external_mesh.py --check`
  会拦住任何哈希漂移，管线本身不变。
- **Unity 没有 glTF 运行库**（`Project/Packages` 与 GameScripts 里都没有
  GLTFast / UnityGLTF），所以 `external_mesh.py` 把 glb 离线转成
  `<id>.zbrd`（ZBRD v1 单部件，`cache_sha256` 锁定）。运行时
  `ZExternalMeshBinder.cs` 先真读 glb 算 SHA256 校验，再读缓存建 Mesh，
  按锚点摆放，成功后 `SetActive(false)` 关掉同名 `decoration_*` /
  `interactable_*` 代理；任何一步失败都**保留代理**，绝不空场景。
  材质直接复用代理的 AI `.mat`，避免两端调色分叉。
- 清单 / glb / zbrd 走 **StreamingAssets**（`Assets/StreamingAssets/
  ZEngineChessboard/`），不用 TextAsset GUID，也不需要 Unity 导入 glb；
  运行时用 `Path.Combine(Application.streamingAssetsPath, ...)` 直接读。
- **踩坑（2026-11-06 用户截图：Missing (Mono Script)）**：trunk 的
  `Program/GameScripts/Assets/**` **不是** Unity 直接编译的源码目录，它由
  外部编译器产出 `Assets/Plugins/GameScripts/Scripts.dll`（`Scripts.dll.meta`
  的 GUID 是 `603043be3babbf44188404cd589ac0c7`，`Project8ArtField` 的
  `m_Script` 就是它 + 类型 fileID）。因此 `.cs` 旁边的 `.meta` GUID
  **在场景里永远解析不到**，写进 `m_Script` 必定 Missing。
  **正确做法（用户 2026-11-06 指定，不要运行时做）：场景里只放纯空节点
  `ExternalMeshRoot`（无 `!u!114`），由 `ZExternalMeshEditor.cs` 的菜单
  `ZEngine/棋盘/挂载外部网格消费端` 在编辑期 `Undo.AddComponent` +
  `EditorSceneManager.SaveScene`，让 **Unity 自己**把 `m_Script` 序列化成
  `Scripts.dll` 的 GUID + 类型 fileID。**不要**再用
  `RuntimeInitializeOnLoadMethod` 去运行时找对象（已删除，JS/Python 门禁
  都会拒绝它回来）。菜单脚本用反射找 `ZEngine.Chessboard.ZExternalMeshBinder`，
  所以不依赖 Editor 程序集引用；类型不在 `Scripts.dll` 里时会弹窗提示先跑
  `GameScriptBuilder/ForceRebuildAll`。
  `unity_scene.verify` 会把旧 GUID `2391662872…` 当回归门禁直接拒绝，
  放行 `Scripts.dll` 的 `603043be…`（场景被 Unity 保存后会出现），
  且不再部署 `ZExternalMeshBinder.cs.meta`（G 盘残留的那份可手动删）。
  `deploy_to_trunk.py` 新增 `--force-scene`：检测到 ExternalMeshRoot 上已有
  `Scripts.dll` 引用时，`--verify-only` 报 `LOCAL_EDIT`、`--apply` 不覆盖场景，
  避免把美术/程序在 Unity 里挂好的组件冲掉；要重灌场景显式加 `--force-scene`。
- Blender：`build_ac90.py` 嵌入 `EXTERNAL` 表（= 清单的 asset_ready 子集），
  `import_external_meshes()` 先校验 glb sha256 再 `import_scene.gltf`，
  位置换算 `(x, y, z) -> (x, -z, y)`、`rotation_euler.z = +rotation_y_deg`，
  代理设 `hide_render=True` + `display_type="WIRE"` 方便美术替换。
  glb 会按包内相对路径 stage 到 `build/assets/external_meshes/`，
  trunk 里 `Blender/..` 与 `assets/` 同级，脚本不用改路径。
- Web：`web/external_mesh.mjs`（纯逻辑，可在 Node 里测）+ `app.js`
  用 `GLTFLoader` 加载，先 `crypto.subtle` 校验 sha256，失败保留代理；
  交互物的外部网格会接管 `state.interactive`（递归 raycast）。
  **`three/addons/` 必须写进 `index.html` 的 importmap**（three 0.160）。
- 部署清单 30 → **41** 个文件（+4 glb +4 zbrd +清单 +1 运行时脚本
  +1 菜单脚本，无 `.cs.meta`）。
  `external_mesh.py --write-proxies` 会重写 `style-pack.json`（json.dump
  统一格式化），改完必须重跑 `zboard.py --variants` 让清单/Blender 同步。
  场景因此少一个 `!u!114`，文档数 92 → **91**，sha256
  `9984b515c9e8df7c`。
- 门禁负例：篡改 glb 一个字节 → Python `verify` / Node `verifySha256` 都拒绝；
  `pivot_offset` 非 0 / 缺 `asset_origin` / `asset_ready` 缺 cache 配对
  都会被 `validate_external_mesh_contract` 拒绝。

---

## OSGame 棋盘线 · 第 4 步：混元 3D 图生 3D 接管 star_pillar（2026-11-06）

`star_pillar` 从 `proxy_zbrd` 换成 **`hunyuan_3d`**，其余 3 个仍是代理。
接管入口是新增的 `python external_mesh.py --import-glb <asset_id> --source <glb>
[--height <m>]`（`--write-proxies` 的逆操作）：**glb 字节一个都不改**
（`mesh_sha256` = 上游下载值 `60cf8395…`），尺寸/原点差异只写进契约；
不要用外部工具烘节点变换或重采样，否则「与上游产物逐字节一致」的意义就没了。

- 混元官方 CLI 只有文生 3D：`tools/hunyuan3d.py` 复用 `buddy-cloud.py` 的
  签名/代理/轮询，把 body 换成 `ImageBase64`（`SubmitHunyuanTo3DProJob` 支持），
  `--face-count` 走 `FaceCount`。**产物 URL 24 小时失效，生成后立刻落盘**。
  身份用 `connect_cloud_service` 拿到的 `BUDDY_CLOUD_TOKEN`（`$env:` 传入，
  不要回显）。
- 实测数据（别再用索引数当三角面数）：`POSITION` = 20,918 顶点，
  索引 = 90,090 → **真三角面 30,030**（=`--face-count 30000`，吻合）。
  早期把 `len(deindexed_positions)//3 = 90090` 当成面数是错的。
  1 材质（`doubleSided` + `KHR_materials_specular.specularFactor=0` +
  `metallicFactor=0`）+ 1 张 **4096² PNG baseColor（19.4 MB）**。
- **混元 glb 是 Z-up**：`nodes[0].rotation = [0.7071,0,0,0.7071]`（+90°X），
  原始 POSITION 的 `z ∈ [-1.063, 0]`。`external_mesh.glb_triangles` 会走
  `_node_matrix` 把节点变换乘进去，所以 `glb_bounds` / pivot 都在**变换之后**
  的 Y-up 局部空间；不要自己再转一次，也不要假设 glb 没节点变换。
- `pivot_offset = [0.001402, 0, -0.008539]`（bbox 的 XZ 中心 + 底面），
  `instances[].scale = 2.2987`（高度归一到代理的 2.444 m）。世界包围盒
  `x/z ±0.633 / y 0..2.444`，落在 `corner_nw|se` 槽内。
  **混元资产的宽高比和代理不同**（0.55/1.06 vs 0.68/2.44），没法既对高度又对
  直径，所以门禁从「世界包围盒 == 代理包围盒」放宽成「底面中心落在锚点 +
  站在地面 + 高度在代理 ±15%」；`asset_origin=proxy_zbrd` 的资产仍逐值相等。
- **ZBRD v1 只收三角形汤**（`zboard.encode` 强制 `index_count == vertex_count`），
  所以 30k 面资产的缓存放大到 **90,090 顶点 / 3.09 MB**。要共享顶点得改格式
  （Unity / Blender / Web 三个解析器一起改），本步没做。
- **贴图三端不一致，已强制对齐**：Unity（ZBRD 缓存 + `zboard_<id>.mat`）与
  Blender（缓存）都不读贴图；Web 用 `GLTFLoader` 会保留 `material.map`，
  于是多乘一层贴图色。`web/app.js` 现在显式 `child.material.map = null`。
  要真正用上贴图，必须先把 baseColor 抽成独立贴图文件并让 Unity 的
  `.mat` 绑定，三端一起接，**不要只让 Web 单独好看**。
- 19.4 MB 贴图仍留在 glb 里（字节保真优先）：Unity `Start()` 会整份读 20.29 MB
  算 sha256，web 还要 decode 4096²。若要瘦身，在 `import_asset` 里加
  `--texture-size` 重打包（改 sha 后要重跑 `--check` + `zboard.py --variants`），
  这是已知待办，不是遗漏。
- 门禁：`python -m unittest discover -s verify` **90 tests OK**、
  `node verify/test_zbrd_js.mjs` 全绿、`external_mesh.py --check` 4 assets /
  6 placements / 30,798 triangles / 20,379,220 bytes。

### 第 4 步续 · 贴图降采样 + 批量换完剩余 3 个（2026-11-06）

`external_mesh.py` 新增 `--texture-size`（PIL 降采样内嵌贴图并重打包 glb，
只重排 BIN chunk、几何/材质/节点全不动）、`--diameter`（归一化 XZ 直径）、
`--pivot-center`（体心 pivot）、`--resize-texture`（对已接管资产重出贴图）。
`resize_glb_images()` 是核心：`_downscale_image` 用 PIL `Resampling.LANCZOS`，
任何一张贴图没被缩小就返回原字节（sha256 不变）。

- 4 个资产已**全部换成混元 3D**（`hunyuan_3d`）：`star_pillar`（灯柱，底面
  pivot）、`halo_ring`（环，`--diameter 2.5 --pivot-center`）、`moon_orrery`
  （星象仪，`--height 1.2 --pivot-center`）、`star_lantern`（灯笼，
  `--diameter 1.1 --pivot-center`）。**悬浮物锚点在物体中心，必须 `--pivot-center`**
  （体心），地面物才用默认底面 pivot——这是「底面 pivot vs 体心 pivot」的
  关键分界，门禁已按 pivot[1] 是否等于局部底面来区分语义校验。
- 贴图 4096² → **1024**：每个 glb 从 18~24 MB 降到 2.2~2.6 MB，4 个资产合计
  `external_mesh.py --check` 输出 **120,060 triangles / 9,914,324 bytes**
  （原 20.38 MB）。原始 4096 下载副本（`*.hunyuan.glb`）在降采样接管后即删，
  参考图仍在 `assets/style_refs/`，可按需重新生成。
- 门禁：90 tests OK / 27 JS checks OK；非代理资产的不变式从「站在地面」放宽成
  「XZ 中心在锚点 + 底面 pivot 站地面 / 体心 pivot 中心在锚点」。
- **待办（非本步）**：① ZBRD v1 三角形汤仍把每个 30k 面资产放大成 90k 顶点 /
  3.09 MB 缓存（4 个 = 12 MB）；② 贴图仍未真用（三端对齐为纯色，`web/app.js`
  `map=null`）；③ 混元生成物朝向/比例需 Web/Unity 视觉确认（本机无 Unity）。

### 第 4 步再续 · Web 端 GLTFLoader 贴图解码挂起（2026-11-06 实测）

宣发页 `web/app.js` 用 `GLTFLoader.parseAsync` 加载混元 glb 时**卡死**（loading
一直不消失、深色屏、控制台无报错）。定位：混元 glb 内嵌贴图（`image/png` +
`bufferView`，无 uri）会让 GLTFLoader 走 `ImageBitmapLoader`（Chromium 支持
`createImageBitmap` 时用它，不是 `TextureLoader`）→ `fetch(blobURL)` +
`createImageBitmap`，解码 PNG 的 Promise 一直不 settle（异步挂起，不是同步死循环，
所以页面不冻结、无异常可抛）。而本页本来就用代理材质（`map=null`）、贴图毫无用处。

**修复**：`web/app.js` 移除 `GLTFLoader` 依赖，改 `parseExternalGlb()` 轻量解析——
按 accessor 直接读 `POSITION/NORMAL/TEXCOORD_0/indices`（componentType 5123/5126，
紧凑布局走 TypedArray 视图，有 stride 才逐元素），构建 `BufferGeometry` + 把
`nodes[0]` 的 TRS（混元是 `rotation=[+90°X]`）套到 mesh 上，**完全不碰贴图**。
几何读取与 `external_mesh.py` 的 `glb_triangles` 逐字节一致（Node 对照：
POSITION 20918 / indices 90090 / UV 20918）。glb 结构本身合法（accessor/Png/JSON
都验过），问题只在浏览器贴图解码这一环。

**教训**：外部 glb 只要带内嵌贴图，就别在 Web 端走 GLTFLoader 的完整贴图加载；
要么像这里自写几何解析，要么接受 GLTFLoader 但 hook 掉 `loadImageSource`。若将来
真要三端用贴图，得先把 baseColor 抽成独立文件 + 三端各接一套，而不是只靠
glb 内嵌贴图。

---

## OSGame 棋盘线 · 宣发页「不报错但不显示」修复 + Web 视觉确认（2026-11-06）

三个独立原因叠加，按发现顺序（前两个都让画面发黑，别再混为一谈）：

1. **整屏欠曝（灯光单位）**：`index.html` 锁 `three@0.160.0`，r155+ 默认
   `useLegacyLights=false`，Directional/Ambient 强度按 lux 解释，同一数值比
   Unity / 旧语义暗 π 倍。无头 Edge 同会话 A/B：中心台面 `[16,27,13]` →
   `[52,75,44]`。**不要用 `renderer.useLegacyLights=true`** —— 已废弃，
   getter/setter 都会打告警，且真机上可能仍黑屏。正确迁移是消费 JSON 强度时
   ×π（`web/app.js` 的 `LUX_SCALE`）；实测必须 `dir×π + amb×π` 才与 legacy
   参考值逐字节一致，只乘一个灯不够。
2. **彻底不显示（DOM 遮罩，用户看到的真正原因）**：`style.css` 的
   `#error { display:flex }` 压过 UA 的 `[hidden]{display:none}`，空的全屏
   `z-index:40` 不透明层把 canvas / HUD / controls 全盖住。修复
   `[hidden] { display:none !important; }`。**教训：`gl.readPixels` 只看 WebGL
   缓冲，看不到上层 DOM** ——「像素正常但人眼看不到」必须查
   `getComputedStyle` + `document.elementFromPoint`；DevTools 里
   `<div hidden>` 旁的 `flex` 徽章就是线索。查渲染别只信帧缓冲。
3. **标题重复**：style-pack 与 visual-pack 同名，拼接时去重（`stat` 仍标注
   AI 视觉层，信息不丢）。

**Web 视觉确认**（第 4 步续挂起的待办③，本机无 Unity）：无头 Edge 抓帧缓冲 ——
不要用 `Page.captureScreenshot`（抓不到合成层），在页面内 `render()` 后同任务
`gl.readPixels` → 翻转 Y → 2D canvas → `toDataURL`，产物 `build/_pw/shot.png`。
结论：核心契约正确（一整块连续台面 + 暖金包边，不是 56 格；深蓝夜空 + 星点；
6 个 glb 外部实例全部挂载可见）；台面无纹样属已知待办②（`map=null`）；
**混元 3D 资产相对台面明显偏小、呈矮柱/团块，看不出「星象灯柱 / 悬浮圆环」
的体量感，朝向与比例仍需人工确认**。门禁 90 tests / 27 JS checks 全绿。

---

## OSGame 棋盘线 · 混元 glb 朝向修复（2026-11-06）

上一节的「偏小」不是比例配错，是**躺倒**：无头实测 6 个外部资产全部 Y/Z
互换，Web 包围盒高度只有 manifest `world_bounds` 的 40~67%。

根因：4 个混元 glb 的 `nodes[0]` 恒为 `rotation=[0.7071,0,0,0.7071]`
（+90°X，Z-up→Y-up）。`web/app.js` 的 `parseExternalGlb` 原本把它设在
mesh 的 `quaternion` 上；而 `parseExternalGlb` **返回的就是那个 Mesh 本身**
（不是外面再包一层 Group），消费端 `applyTransform` 的
`object.rotation.set(0, rotY, 0)` 直接把该四元数覆盖，轴向修正丢失。

修复：用 `matrix.compose(translation, quaternion, scale)` 组成节点矩阵后
`geometry.applyMatrix4(matrix)` **烘进顶点**（three.js 同步变换 normal），
mesh transform 保持单位，再由 `applyTransform` 自由设置
position/rotation/scale。与 Python `glb_triangles`（`_apply_point` /
`_normal_matrix`）、Unity ZBRD 缓存、Blender glTF importer 口径一致。
修复后 6 个实例三轴尺寸与 manifest `world_bounds` 逐项相等（heightRatio=1）。

**比例结论**：`--import-glb --height 2.444` 归一化后灯柱世界高 2.444 m、
台面约 16×15 m，契约比例本就如此；之前显得小是躺倒的视觉假象。若仍要更醒目，
应调 `--height` / `--diameter` 目标并重跑 `--variants`（会连带刷新 manifest
`world_bounds` 与 Unity 缓存），**不要只在 Web 端放大**，否则三端立刻分叉。
`test_zbrd_js.mjs` 已加静态断言（必须有 `geometry.applyMatrix4` /
`matrix.compose`，禁止 `mesh.quaternion.fromArray`）防回退。

---

## OSGame 棋盘线 · ZBRD v1 外部缓存改为索引网格（2026-11-06）

第 4 步续的待办①：4 个 Unity 缓存各 3.24 MB（共 12.9 MB），因为
`glb_to_zbrd` 把 glb 去索引展开成「三角形汤」——每个 30k 面资产变成 90k 顶点。

**根因不是格式约束**：ZBRD v1 头里 `vertex_count` / `index_count` 本来就分开
存，Web `parseZBRD` 只校验 `indices[i] < vertexCount`，Unity
`ZExternalMeshBinder.ReadZbrd` 直接 `SetTriangles(indices, 0)`，都支持共享顶点。
是 `zboard.encode` 一条自加的 `ic != vc` 校验把**所有**部件锁成了汤。
（核心棋盘几何仍是汤：`MeshBuilder.add_tri` 每面 append 3 个顶点；本次不动它，
收益小且会改 `board.zbrd` 的 sha。）

**改动**：
- `zboard.encode` 校验放宽为 `ic % 3 == 0` 且 `0 <= i < vc`；格式说明补注。
- `external_mesh.py` 新增 `glb_indexed()`：保留 glb 的
  `POSITION/NORMAL/TEXCOORD_0` + `indices`，节点矩阵（+90°X）仍烘进顶点/法线
  （与 `glb_triangles` 同口径），`glb_to_zbrd` 改用它；`glb_triangles` 保留给
  bounds / 面数 / 对拍。
- 新增 `external_mesh.py --refresh-caches`：按现有 glb 重派生缓存并回写
  `cache_sha256`，**glb 字节与 `mesh_sha256` 一律不动**，专供格式升级。
- `ZExternalMeshBinder.cs` 的 `indexFormat` 判断从 `indexCount > 65000` 改成
  `vertexCount > 65535`：索引格式取决于**最大索引值**而不是索引条数；三角形汤
  时代两者恰好都过线，改索引网格后按旧判法会把 2 万顶点仍当 UInt32。

**结果**：4 个缓存 20,918~22,055 顶点 / ~90k 索引、各 0.98~1.02 MB，
合计约 4.0 MB（原 12.9 MB，约 1/3）。glb sha / pivot / scale / 包围盒 /
三角面数（120060）全部不变。门禁 **91 tests / 29 checks** 全绿，新增两条断言：
Python 比对 glb accessor 顶点数与 bounds，JS 用 `parseZBRD` 读缓存断言
`vertexCount < indexCount`（同时保证 Web 解析器能吃 `indexCount != vertexCount`）。
**部署提醒**：4 个 `.zbrd` + `ZExternalMeshBinder.cs` + `style-pack.json`（新
`cache_sha256`）必须一起更新，否则 Unity 端 sha 校验会失败；
`--verify-only` 当前 0 missing / 8 stale of 41 files，属用户部署前的正常状态。

---

## OSGame 棋盘线 · 三端 baseColor 贴图打通（2026-11-06）

待办②。4 个混元 glb 各内嵌 1 张 1024² PNG（1.35~1.80 MB，UV 展开图：
青蓝底 + 金色星纹 / 发光格栅）。此前 Web 主动 `map = null`、Unity 用代理
纯色材质，只有 Blender 的 glTF importer 隐式消费，三端口径不齐。

**沿用既定路径**（不要再改回 GLTFLoader）：把内嵌 PNG 抽成**独立文件** +
sha256，三端各接一套。`web/app.js` 曾因 GLTFLoader 的 ImageBitmapLoader
解码挂起而改成自写几何解析，这条路不能回退。

- 数据层：`external_mesh.py` 新增 `glb_embedded_texture()` /
  `extract_textures()`（CLI `--extract-textures`），把
  `assets/external_meshes/<asset_id>.png` 与 `texture` / `texture_sha256`
  写进契约；`_install_asset`（`--import-glb` / `--resize-texture`）自动重抽；
  `verify()` **逐字节比对独立 PNG 与 glb 内嵌字节**，不同步即报错。
  `build_manifest` 与 `deploy_to_trunk.py` 同步带上贴图。
- Web：`fetch` 独立 PNG → 校验 sha → Blob URL → `TextureLoader.loadAsync`，
  `colorSpace=SRGBColorSpace`、`flipY=false`（glTF V 向下），`map=texture`
  且 baseColorFactor 设白。加载失败降级为代理纯色，几何照常显示。
- Unity：`ZExternalAssetEntry` 加两字段；`LoadTexture` 用
  `ImageConversion.LoadImage` 解码 + sha 校验；**克隆代理材质**（不碰
  sharedMaterial，也不 `new Material(shader)` —— 自定义 SRP 下会返回 null），
  按 `_BaseMap/_MainTex/_AlbedoMap/_BaseColorMap` 探测贴图槽、
  `_BaseColor/_Color/_AlbedoColor` 设白。`ReadZbrd` 里 `v = 1 - v`：
  glTF UV 左上原点、Unity 左下原点，不翻会上下颠倒。
- Blender：`BPY_TEMPLATE` 的 `import_external_meshes()` 校验 sha 后
  `bpy.data.images.load()` 换掉 importer 的 `TEX_IMAGE` 节点 image
  （importer 已把 UV 转成 Blender 约定，换图无需改 UV）；贴图与 glb 同路径
  stage 到 `build/assets/external_meshes/`，`verify_outputs` 校验 staged sha。

**三端一致的调色约定**：`baseColor = texture × baseColorFactor`，factor 恒为
白色；代理纯色只在贴图缺失时作降级。别在某一端再乘一遍纯色。

验证：Python **92 tests** / JS **30 checks** 全绿、lint 0 error；Web 无头实测
6 个实例全部 `map=true / srgb / flipY=false / 1024² / color=(1,1,1)`、零
console 错误，帧缓冲截图 `build/_pw/shot_textured.png` 可见金色星纹。
**Unity / Blender 实机未验证**（本机无 Unity / bpy）；Blender 侧只做到生成
脚本的 marker + staged 字节校验，Unity 侧只做到静态检查。
部署清单 41 → **45 files**（+4 PNG），`--verify-only` 当前 4 missing /
8 stale；`style-pack.json` 的 texture 字段、`ZExternalMeshBinder.cs`、
manifest 必须一起更新。**台面纹样贴图是另一条线，本次没做**（台面仍是纯色）。

---

## OSGame 棋盘线 · 拆成两页：数据验证页 vs 沉浸式宣发页（2026-11-06）

用户对比 `1.png`（现网 Web 页）与 `2.png`（painting3d 走进星夜）后要求：
「拆成两页，要往 2.png 走，天空盒保持一致是完全没有问题的，`3.png`
地面保持一致也完全没问题，中间空出一块地作为棋盘就可以了」。

**差距的根因不是渲染 bug**：`web/app.js:6-7` 注释写明只参考 starry-night 的
「架构」（WebGLRenderer + ShaderMaterial 天穹 + Points 星空 + 拖拽环视），没搬美术；
验证页是 48° 俯视的物件检视视角 + HUD / 控制面板，且 `visual-pack.json` 的
panorama / bloom / fog / color grading 在 Web 端几乎没消费。前几轮修的是**正确性**
（灯光 ×π、`[hidden]` 遮罩、glb 朝向、三角形汤、三端贴图），并没有缩小视觉差距。

**现在的两页**（互相有链接，`app.js` / `style.css` 的验证逻辑零改动）：
- `web/index.html` + `app.js` + `style.css`：三端数据一致性预览（LOD / 变体 /
  glb+sha / 清单统计），保持工具定位；只加了 1 行导航链接与 favicon 占位。
- `web/starry.html` + `starry.css` + `starry.js`：沉浸式宣发页，独立 importmap /
  独立样式 / 独立脚本，不复用验证页的任何 DOM id。

**沉浸式页怎么保同源**（拆页不能变成「另起一套」）：
- 天空 = `visual-pack.json` 里 sha 锁定的 `assets/sky_panorama.jpg`（2048×1024，
  与 Unity `sky_gradient.mat` / Blender World 同一张）：`fetch` → `verifySha256`
  → Blob URL → `TextureLoader`；贴天穹球，本页不构建任何 ZBRD 部件（含 `sky@*`）。
  全景图不是严格等距柱状投影：把 texture v 压到地平线以上约 45° 的天空带
  （`clamp(0.02 + (uv.y - 0.5) * 3.9, 0.02, 1)`），否则新月跑到天顶、相机仰视
  时根本看不到；地平线以下用深色「海面」圆盘遮住，避免全景图底部的暖色被
  拉成一大片。
- 棋盘 = **画进地形贴图的一块区域**（详见本节末尾「用户纠正」），不是独立 3D
  台面。区域轮廓取 `build/board.zbrd` HD 档 `core_slab` 顶面顶点的 XZ 凸包
  （`convexHullXZ`），锚点半径与逻辑边界取自 ZBRD / manifest，不另写棋盘公式。
- 单体装饰 = `assets/external_meshes/*.zbrd`（Unity 也在读的**索引网格缓存**）
  + 同一份独立 baseColor PNG，锚点公式仍走 `external_mesh.mjs` 的
  `applyTransform`。验证页读 glb、宣发页读由同一 glb 派生的 cache，两边都校验
  sha256。`texture.flipY = false`（glTF V 向下）、`color = white` 不二次乘色；
  这些星灯 / 灯柱 / 圆环立在被划出的区域外沿，对应 4.png 里篮球架 / 长椅 /
  球车那种「站在场地边上的场景物件」。
- 灯光 / fog / bloom / exposure 全从 `visual-pack.json` 读：`LUX_SCALE = Math.PI`
  （不要 `useLegacyLights`）、`FogExp2(0.006)`、`UnrealBloomPass` +
  `EffectComposer`/`OutputPass` + ACES。addons 走动态 `import()`，CDN 拿不到时
  降级为 `renderer.render` 直出，场景照样可见。
- 地面按 `3.png` 程序化生成：圆形星夜岛（`ISLAND_R=56`）+ canvas 笔触贴图 +
  蜿蜒小路 ribbon + 蓝色树丛 / 柏树 / 屋舍亮窗 / 白色笔触（都用 InstancedMesh），
  `CLEAR_R=18` 内不放任何装饰物和路径，留给棋盘区域。地面与外部单体底部同在
  y=0；不再有共面实体台面，也就没有之前俯瞰时的 z-fighting 摩尔纹（那个
  `islandGroup.position.y = -0.30` 的补偿已随实体台面一起删除）。
- 相机两套预设：沉浸（targetY=6 / radius=32 / elevation=-5° / fov=66，天空占约
  2/3）与俯瞰（elevation=62° / radius=82，对应 3.png），平滑插值；拖动环视、
  滚轮推进、方向键漫游、自动巡游、点击发光物件出 inspect 面板。
- `starry.css` 保留 `[hidden] { display:none !important; }`（`style.css` 踩过的坑：
  作者 `display:flex` 压过 UA `[hidden]`，空的全屏 `#error` 会盖住 canvas）。

**验证**：`tools/serve.py` 起服务（Windows 上 `python -m http.server` 仍会把 `.mjs`
发成 `text/plain`），无头 Edge + `build/_pw/starry_probe.cjs` 在**同一任务内**
render + `gl.readPixels`（跨任务读前缓冲只会得到黑帧），帧缓冲截图
`build/_pw/starry.png` / `starry_overview.png`。实测：`#error` computed display
`none`、中心最上层 CANVAS、天空贴图 2048×1024 srgb、6/6 外部实例、4 张
baseColor、**0 个实体台面网格**、6 个装饰网格、15 个岛面网格、bloom/fog 生效、
28 draw calls / 209446 三角形、**console errors 0**（只有 readPixels 触发的驱动
性能 warning）。门禁：Python **92 tests OK**、JS **33 checks / 0 failures**（新增
3 条：两页独立且互链、全景图与共享资产复用、棋盘画进地形而非叠台面）。
`deploy_to_trunk.py` 不含 `web/` 文件，拆页不产生 Unity trunk 部署 drift。
**Unity / Blender 实机仍未验证**（本机无 Unity / bpy）。

### 用户纠正（同日）：棋盘必须融合在场景里，不能盖上去

用户给 `4.png`（游戏内「万象学园篮球场」）与当前沉浸式页俯瞰截图后明确：
「这个中间的棋盘去掉吧，棋盘应该是融合在场景里面的，而不是单独的一块，
也是以当前地形为基调，在此基础上画出一块区域作为棋盘，而不是把一个棋盘
盖上去」。

`4.png` 坐实了现网形态：**一整片与周围共面的连续地面**，靠白色场地线 /
半场圆 / 中圈徽记划出区域，篮球架 / 长椅 / 球车直接立在场地边沿，没有一块
凸起的台面。此前把 ZBRD `core_slab` + 金色包边渲染成浮在笔触夜地上的绿色
方块，是「把棋盘盖上去」，方向错了。

改法（`web/starry.js`，验证页 `app.js` 一字未动 —— 它是数据验证工具，展示
ZBRD 实体几何是对的）：
- `loadBoardRegion()` 只**解析** ZBRD：取 HD 档 `core_slab` 的 XZ 凸包作轮廓、
  `anchor_*` 的中心/半径、`manifest.grid.logical_bounds_xz` 作逻辑边界；
  **一个 ZBRD mesh 都不建**（`boardSlabMeshes: 0`）。`makeGeometry` 只剩给
  外部 glb 缓存用。天穹球部件也不再需要「跳过」，因为根本不遍历建 mesh。
- `boardProjection()`: `CircleGeometry` 的 uv=(x/2R+.5, y/2R+.5)，绕 X 转
  -90° 后 z=-y，CanvasTexture 默认 flipY ⇒ **画布像素 = centre + (x,z)·size/(2R)**；
  这样 ZBRD 世界米坐标直接落到地面贴图，不用另写一套棋盘公式。
- `paintBoardBase()` 在笔触**之前**铺一层径向羽化的淡暖色（clip 到轮廓），
  `paintBoardLines()` 在笔触**之后**画 4.png 那套场地线：手绘抖动的外框、
  逻辑边界虚线、半场线、两个半圆、中圈同心圆 + 八芒星 + 新月、锚点浅刻圆，
  末尾再压 150 条笔触让线像长在夜色里。**笔触必须连续穿过区域**
  （原来的 `r < 0.30 && random() < 0.94` 挖空中央已删）——这是「同一片地面」
  而不是「贴花」的关键。
- 底色 / 线的 alpha 都压得很低；不要退回纯色填充块或发光粗金框。
- 俯瞰截图 `build/_pw/starry_overview.png` 现在是笔触地面上淡淡的一块场地线
  区域，沉浸截图 `build/_pw/starry.png` 地面连续延伸到前景，没有中间方块。

**长期约定**：沉浸式 / 宣发侧表达棋盘时，一律「以地形为基调画区域」；
ZBRD 只提供轮廓 / 锚点 / 逻辑边界，不渲染成独立几何。三端同源靠的是**同一份
轮廓数据**，不是把同一块台面搬到每个场景里。台面纹样贴图（真·表面贴图）是
另一条线，本次用地面场地线替代，仍未做。

---

## OSGame 棋盘线 · 宣发页拖拽手感 + 装饰物场景化（2026-11-06）

用户同一句里报了两个问题：「鼠标轻微拖拽一下，就旋转的力度会很大，
装饰物也不匹配，就场景物件」。

### 拖拽「力度很大」是重复累加 bug，不只是灵敏度系数

`bindEvents()` 里 `dx/dy` 是**相对按下点的累积位移**，但旧代码在每个
`pointermove` 里执行 `cam.azimuth -= dx * 0.004` / `cam.elevation += dy * 0.08`
「再减一次」完整位移 ⇒ 轻轻拖 40px、发 8 个 move 事件，累计减的是
(5+10+…+40)×0.004 ≈ 2.2 rad ≈ 126°，且 elevation 瞬间撞到 ±86° 限位。
同一个错误也让 `dragging.moved += |dx|+|dy|` 把位移累加，点击判定
`moved < 5` 几像素就失效，点不中物件。**别只调系数，必须改成绝对映射**：

```js
// pointerdown 记下角度；pointermove 用「按下时角度 + 视口归一化位移」
dragging = { x, y, azimuth: cam.azimuth, elevation: cam.elevation, moved: 0 };
cam.azimuth = dragging.azimuth - (dx / innerWidth) * (100 * DEG);
cam.elevation = clamp(dragging.elevation + (dy / innerHeight) * 55, -14, 86);
dragging.moved = Math.max(dragging.moved, Math.abs(dx) + Math.abs(dy));
```

拖过整个视口宽 ≈ 100°、高 ≈ 55°，分辨率无关。无头实测 **40px → 2.78°**
（`build/_pw/starry_probe.cjs` 用 `page.mouse` 模拟 + `__zengineStarry.view()`
前后对比），旧写法同条件会到几十度。

### 装饰物 → 场景物件

围在棋盘四周的 6 个混元金色单体（星灯 / 灯柱 / 圆环 / 月相仪）在星夜
村庄里仍像「棋盘摆件」，与 4.png 场地边上的篮球架 / 长椅 / 球车那种
**场景物件**不是一类。沉浸式页默认不再陈列它们：常量
`USE_EXTERNAL_PROPS = false`，`loadExternalMeshes()` 及 sha / 锚点 / 贴图
校验代码**全部保留**，改 `true` 即可在同一页切回来验收；验证页 `app.js`、
Unity、Blender 仍照常消费这批资产，三端同源没有删掉，只是宣发页不承担
资产陈列职责。

替代物是 `buildSceneProps()`：与树丛 / 屋舍同一套冷蓝石质材质，沿 `PATHS`
（地面 ribbon 与石灯共用同一份曲线坐标）散布 ~18 盏矮石灯 + 16 个矮石座，
灯芯用 >1 的 HDR 暖色让 bloom 抓到。石灯可点击，inspect 读
`userData.hint` 显示**场景化描述**，不再回退到 `board.manifest.json` 里
「点击触发星轨高亮 / 共用同一份单体资产」那种数据验证页语言。

实测：`boardMeshes 0` / `sceneProps 55` / `useExternalProps false`、
三角形 209446 → **30752**（不再加载 6×30k 面 glb）、`console errors 0`。
门禁：Python **92 tests OK**、JS **34 checks / 0 failures**（新增一条锁住
「绝对映射 + `dragging.moved = Math.max` + 场景物件默认陈列 + `data.hint`」）。
截图 `build/_pw/starry.png` / `starry_overview.png` 已更新。

---

## OSGame 棋盘线 · 12 画境视觉包与可切换场景（2026-11-06）

用户指着 painting3d 合集问「这是一个合集，你可以都生成吗？」，选定
「两者都要（先出 12 套视觉包定调，再据此生成 12 个可切换的 3D 场景）」
+「分批高完成度（本轮先做 3~4 个到星夜页水平，后续继续直到 12 个齐）」。

### 真源与边界

- 合集真源是站点 `data/manifest.json`（首页 meta 写 11 个是旧数据）；直接
  urllib 取要带 `User-Agent: Mozilla/5.0`，否则 403；实际 **12** 个世界。
- 只引用标题 / 作者 / 年代 / 画派色彩作元数据；场景全部是程序化几何 +
  canvas 笔触重画，不复制原站模型、贴图、代码或原作构图。
- **几何仍只有一份**：`build/board.zbrd` 的 `core_slab` 顶面凸包 + anchors
  + `board.manifest.json` 逻辑边界。`web/worlds.json` 的 `overrides`
  只允许 `sky/ground/board/scenery/lighting` 五个键，不得改轮廓；
  每个世界都必须 `boardSlabMeshes === 0`。

### 数据契约

`web/worlds.json`（`zengine.chessboard.world-pack/v1`）= `defaults` + 每个
世界的 `overrides`；`web/world-pack.mjs` 做深合并，**数组整体替换**（逐项
合并会让 7 色调色板被旧世界的值垫在后面），`resolveWorldPack` 对未知
`?world=` 回退 `default`。12 套各自持有天空 / 地面笔触 / 场地线 / 场景
物件 / 灯光 / bloom / exposure / fog。切换走 `?world=<id>` 或 HUD 下拉，
**重载整页**（贴图与 InstancedMesh 都是按包一次性生成的，现场拆不干净），
URL 可直接分享某一画境。

### 落地（同日已补齐「12 个齐」）

- **12 个全部 high**：`starry` 内建于 `starry.js`（sha 锁定同源全景），
  其余 11 套在 `web/world-scenery.mjs`：首批 `wheat` / `sunrise` / `pond`，
  第二批 `cafe` / `bedroom` / `garden` / `argenteuil` / `villeneuve` /
  `marly` / `victoire` / `gauguin`。每套都是 InstancedMesh + 程序化几何 +
  该世界调色板，物件避开 `CLEAR_R` 与相机轨道，中央仍只留画进地形的
  棋盘区域。公共零件 `strokesOnGround` / `grove` / `boats` / `cottages` /
  `glowOrbs` / `interactives` 抽在文件后半段；树干与树冠、旗杆与旗面、
  屋顶与墙身一律共用位置表，否则会散出悬浮球。
- `worlds.json` 里 8 个 `tier: tone` 已升 `high` 并绑定各自 preset；
  `generic` 分支只作兼容保留，运行时已不再走通用树丛。
- 11 个非星夜天空由 `web/world-sky.mjs` 在 2:1 canvas 上程序化生成，
  不往仓库塞 11 张外部贴图，离线可复现。
- 第二批截图复核又修了两处构图：圣维克多山的远山不能拿
  `sky.fog.color` 直接当材质（午后光照把它洗成白色尖锥），要先
  `multiplyScalar(0.55)` 压暗、再把锥体压扁拉宽；阿尔勒卧室的家具
  撒到 r≈50 就只剩几个白点，必须压到内环 r≈21..24 并放大，才有房间
  围合感。

### 四个实测坑（都是看截图才发现的，别只跑断言）

1. **球面 UV 水平拉伸**：`buildSky()` 把球面 v 压约 3.9 倍，低仰角相机只看
   到画布 y≈0.3..1.0。画布里画正圆日轮 → 屏幕上横椭圆；水平细椭圆云 →
   屏幕上一条线；三段渐变之间还会出现硬地平线。修法：
   `ctx.scale(SUN_X_SQUEEZE = 0.28, 1)` + 云改竖向椭圆 + gradient stop
   0.42 / 0.72 铺满低仰角可见区。`mix()` 必须返回 `#rrggbb`
   （`rgba()` 只解析 hex），返回 `rgb(...)` 会让整片天空变黑。
2. **白昼浮尘**：星夜 `opacity 0.8` 的 AdditiveBlending 星尘在没有星空的
   世界里被 bloom 放大成「白昼下雪」；改为
   `dustOpacity = 0.25 + 0.55 * sky.stars` + `dust.visible = sky.stars > 0.05`，
   星夜仍是 0.8、无星空世界直接不渲染（`toggle-stars` 控的是整个
   `fxGroup`，语义不变；星空 Points 本身已由 `star_opacity` 关掉）。
3. **云不能用描边椭圆**：`ctx.stroke()` 的椭圆贴到球面上是一圈圈空心线圈，
   看着像泡泡而不是云；改成 `ctx.scale(rx / ry, 1)` + `createRadialGradient`
   的实心柔斑，屏幕上是柔和的横向云团。
4. **相机轨道贴脸物件 + 悬浮树冠**：沉浸式相机在半径 32 的轨道上环视，
   `annulus()` 若把 3m 树干 / 10m 柏树撒在 r≈32 附近，会贴着镜头占掉
   半个画面（麦田页实测出现棕色巨柱）；现在 `CAMERA_ORBIT_R = 32` /
   `CAMERA_CLEARANCE = 8` 强制避开轨道带。另：橄榄树树冠与树干原本各调
   一次 `annulus()` 取随机位置 ⇒ 绿球悬空且挡住棋盘区域；必须共用同一份
   `oliveTrees` 位置表，并把树冠半径收到 0.75..1.25。

### 概念图（同日已补齐，12/12 asset_ready）

云端文生图第一次失败（`invalid or expired JWT token` / 单任务上限）时
**没有伪造结果**，11 个世界先保持 `prompt_only` + 程序化天空缩略图降级；
同日重试 `image_gen` 成功：11 张 PNG 落在 `assets/worlds/<id>.png`，
sha256 同时写进 `worlds.json` 与 `assets/worlds/manifest.json`
（`zengine.chessboard.world-concept/v1`），`status` 全改 `asset_ready`；
`starry-night` 仍用 sha 锁定的 `assets/concept.jpg`。

**并发生成撞车坑**：一次并行 4 个 `image_gen` 请求里，两组不同 prompt
拿到字节完全相同的图（同 sha）⇒ 批量生成后必须逐张算 sha256 去重，
重复的**串行**重生成（本轮 `mont-sainte-victoire` / `villeneuve-bridge`
各补一张）。`prompt_only` 降级分支仍保留，未来缺图不会留死链。
概念图只作视觉定调，棋盘轮廓仍只来自 `build/board.zbrd`。

### 门禁

`verify/test_zbrd_js.mjs` **41 checks**（视觉层白名单、深合并 / 12 套
调色板指纹、12 个 high 各自唯一 preset、12 张概念图 sha + manifest 对拍、
URL 切换不重建几何、天空 UV 补偿、白昼浮尘 / 相机轨道避让、
Pages 构建必须收齐所有本地 `.mjs` 模块）；Python 92 tests 不变。
`build/_pw/worlds_probe.cjs` 逐个加载 12 个 `?world=`，实测 12/12 ok、
地面签名 12/12 不同、console errors 0，保存 12 个 high 的沉浸 / 俯瞰
共 24 张截图。`tools/serve.py` 必须显式声明 `.mjs/.jpg/.jpeg/.png` MIME
（Windows `mimetypes` 会把 `.mjs` 当 `text/plain`，ES module 被严格
MIME 检查拒绝）。

---

## OSGame 棋盘线 · 发布到 `zentia.github.io`（2026-11-06）

用户要「把 `?world=port-marly-flood` 这个页面发到
https://zentia.github.io/，仓库现有东西都不要了」。

- **不要改源码路径**。`starry.js` 里 `BUILD = "../build/"`、
  `fetchJSON("../visual-pack.json")` / `"./worlds.json"`、
  `applyWorldHud` 的 `"../" + concept.image` 都相对 `web/` 写死。
  发布产物保留 `web/ + ../build/ + ../assets/` 原层级，一个路径都不改；
  根 `index.html` 只做 `location.replace("web/starry.html?world=...")`。
- `tools/build_pages.py` 生成 `pages/`（30 文件 / 21.8 MB）：只带 starry 页
  真正 fetch 的契约、ZBRD、贴图与 11 张概念图；不发布验证页 / Unity /
  Blender / 探针截图。写 `.nojekyll` 防 Jekyll。
- **`.mjs` 一律复制成 `.js` 并重写 import**：GitHub Pages 对 `.mjs` 的
  Content-Type 不稳定，ES module 有严格 MIME 检查；本地 `tools/serve.py`
  靠显式 MIME 绕开，线上不能指望。产物里不允许残留任何 `.mjs`。
  根入口默认 `port-marly-flood`，`/?world=<id>` 可透传任意画境；
  `starry.html` 里「返回数据验证页」链接在产物中换成默认画境，避免 404。
- 验证：`build/_pw/pages_probe.cjs` 用**最普通的**
  `python -m http.server --directory pages`（不带自定义 MIME）跑根跳转 +
  12 个画境，检查概念图 `naturalWidth`、404 与 console error；实测全过。
- 目标仓库 `zentia/zentia.github.io` 默认分支是 **`master`**（用
  `git ls-remote --symref` 查的，不是 `main`）。
- **实际发布源是 `gh-pages` 分支，不是 `master`**。原仓库是 Hugo 站 +
  `.github/workflows/deploy.yml`，构建产物推到 `gh-pages`；实测线上首页与
  `raw.githubusercontent.com/.../gh-pages/index.html` 逐字节相同，只 push
  `master` 时线上仍是旧 Hugo 页（`/web/starry.html` 404）。⇒ 发布必须
  **同一份产物同时提交到 `master` 与 `gh-pages`**（各自清空旧文件后
  commit + 普通 push，不需要 force）。删掉 `deploy.yml` 后不会再有 Actions
  把 `gh-pages` 覆盖回 Hugo 产物。
- 清空旧文件 + push 是破坏性远程操作，需用户确认。旧内容仍留在 git 历史里。
- 线上回归：`PAGES_BASE_URL=https://zentia.github.io node
  build/_pw/pages_probe.cjs`（探针支持 `PAGES_BASE_URL` 跳过本地 server），
  实测 12/12 ok、0 个 404 / console error。
- three.js 仍由 `starry.html` 的 importmap 指向 `unpkg.com`；Pages 是
  HTTPS、CDN 支持 CORS，但完全离线的自托管尚未做（本机没有 three 包）。
- **移动端（用户反馈「拖拽容易触发刷新、竖屏体验差」）**：`starry.css` 加
  `overscroll-behavior:none` + `#stage canvas {touch-action:none}`、竖屏底部
  控件条与 `env(safe-area-inset-*)`；`starry.js` 加非 passive `touchmove
  preventDefault`、`gesturestart` 拦截、`pointercancel` 清理、双指 pinch，
  并由 `viewportTuning()` 在竖屏放宽 FOV(66→85.8)/radius、调小拖拽 sweep
  与点击阈值。改完**同样要双分支发布**，只重建本地 `pages/` 线上不生效。
  已发布 `master 9e7ea24` / `gh-pages 82a8a89`。
- **线上 probe 两个假失败（别当线上故障修）**：① 连续切 12 个画境时上一页
  仍在传输的概念图被导航取消，Chrome 报 `net::ERR_ABORTED`；必须从
  `requestfailed` 过滤掉（404 由 response 监听兜住），否则线上误报 7 个
  FAIL 而 `bad responses` 是 none。② 跑完 12 个 WebGL 页后横竖屏 resize
  会延迟，用 `waitForFunction(fov<80)` 等构图回落，别用固定
  `waitForTimeout(400)`。本地回归快，这两个坑只在线上暴露。

---

## OSGame 棋盘线 · 核心契约收敛为「一个方格 + 两个棋手占位」（2026-11-06 用户纠正）

用户给两张图：概念草图（最外方格 + 中线 / 两个半圆 / 中圈同心圆 / 外侧小圆）
与现网「河豚车站」皮肤（一整块连续台面 + 左下 / 右上两个棋手座位区）。
裁定：**只有最外面的方格和两个棋手占位是必须元素，其余全部按概念图输出。**

- `board-spec.json` 新增 `core_contract.required_elements`
  （`square_outline` / `player_seats`）、`square_outline`、`player_seats`；
  冻结部件 = `core_slab` + `frame_*`（方格）+ `anchor_seat_home`（左下
  `[-10.2,0,9.0]`）/ `anchor_seat_guest`（右上 `[10.2,0,-9.0]`，radius 1.9m）。
  `anchors.items` 3→2，删掉 `info_plinth`；`corner_sw` / `corner_ne` 两个
  `decoration_zones` 槽加 `reserved_by` + `max_items=0`，`validate_style_pack`
  拒绝任何风格包装饰占用。`validate_spec` 强制 2 个占位且 x/z 符号均相反。
  台面中心其实在原点 —— `center_offset_m=[-5.912,0,-6.3]` 正好把 7×8 逻辑
  网格中心抵消掉，别按「台面偏在左下」去调 anchors。
- **草图纹样不再硬编码**：`web/worlds.json` 的 `defaults.board.marks=[]`，
  `paintBoardLines` 里 `center_line` / `half_circles` / `center_emblem`
  只有概念图在 `overrides.board.marks` 显式声明才画；八芒星、新月、
  逻辑边界虚线、压线笔触已删。占位圆由 ZBRD 的 `anchor_*` 包围盒驱动，
  画成淡填充 + 双圈 + 朝向中心的短刻线。
- 数量连锁（改契约时全部要同步）：**17 parts = 11 core + 6 variable**、
  HD `scene_parts=14`、Blender glTF 14 mesh / 11 mat、`boardAnchors=2`；
  `test_board.py` 92 tests、`test_zbrd_js.mjs` 44 checks、
  `build/_pw/{worlds,pages}_probe.cjs` 的 `boardAnchors===2`。
  `web/app.js` 的 `coreCount` 按前缀动态统计，无需改。
- **`visual-pack.json` 仍保留 `anchor_info_plinth` 材质键（刻意）**：
  `validate_visual_materials` 要求视觉包材质集合与 trunk 已落地 12 个
  `.mat` 的 GUID 集合完全一致，删键会让 `unity_scene.py` / 部署清单漂移；
  该材质已无 ZBRD 部件引用、不参与渲染，等人工清理 trunk 资产时再删。
  `build/materials/` 与 `asset_guids.json` 因此都保持 12 项。

---

## OSGame 棋盘线 · 六类局内演出（2026-11-06，参考 BV1UdYj61ExH）

用户指出视频里棋盘皮肤都有：入场动画 / 回合胜利 / 回合失败 / 连续胜利 /
升级播报 / 互动彩蛋。本线按「**宿主派发事件、棋盘只负责表现**」落地：棋盘
绝不判定胜负、连胜数、等级，这些状态一律由 payload 传入。

- 唯一真源 `ceremony.json`（schema `zengine.chessboard.ceremony/v1`、
  `frozen=true`）：6 个 event id、duration_ms、payload 字段、palette_defaults、
  transport 约定（Web / Unity / Blender 同 id 同 payload）。**Unity 运行时绑定
  与游戏逻辑未实现**，此处只冻结契约，别宣称已接入项目侧。
- Web 参考实现：`web/board-ceremony.mjs` 是纯 DOM（不依赖 three），负责横幅、
  文案、时长、事件历史；`web/starry.js` 只把事件映射成一段短暂氛围脉冲
  （临时 `camera.fov` / `bloom.strength` / dust 转速），**从不写回 `cam` /
  `tween`**，所以拖拽、视角、像素指纹回归都不受扰动。
- 触发入口：`#ceremony-select`、`?event=<id>`（根 `index.html` 也透传）、
  `window.__zengineStarry.ceremony.dispatch(id, payload)`；互动彩蛋还能点
  左下/右上棋手占位（`y=0` 平面求交，坐标取 ZBRD anchors）或按 `E` 键。
- **加载完成的自动入场只显示横幅**（`payload.auto` 时跳过 3D 脉冲）：无头
  探针在 `loading.done` 后立刻取像素与拖拽基线，任何 fov/bloom 抖动都会污染
  回归。手动重播或 `?event=` 才是完整演出。
- 配色在 `web/worlds.json` 的 `defaults.scenery.ceremony`，12 画境可用
  `overrides.scenery.ceremony` 覆盖；与 `ceremony.json` 的 `palette_defaults`
  由门禁对拍。
- 门禁链：`test_zbrd_js.mjs` **45 checks**（新增 `board ceremony freezes 6
  host-dispatched events`）、`test_board.py` 92 tests、`pages_probe.cjs` 逐一
  dispatch 6 个事件验 DOM / `#ceremony.show` / `pointer-events:none`、
  `worlds_probe.cjs` 验 `probe.ceremony.events===6 && last==="entrance"`。
- 新增第 6 个本地模块后，`build_pages.py` 的 `MODULES` 与 `starry.js` import
  数量断言 **5→6** 必须同步；`ceremony.json` / `web/board-ceremony.js` 进
  `required`。根跳转 `?event=` 透传别漏。
- 横屏 390 高时新增下拉把 `#controls` 撑出视口 → 横屏媒体查询加
  `max-height: calc(100vh - 86px); overflow-y:auto` 并压缩两个 select。
- 明亮画境（马尔利港 / 阿尔让特伊的浅灰天空）下横幅文字对比度不足 →
  `#ceremony::before` 用**大范围低 alpha** 径向暗衬；小范围高 alpha 会在
  天空上留下一块硬边椭圆污渍。


---

## OSGame 棋盘线 · 第 13 画境「流金棋亭」并入十二画境体系（2026-11-09）

用户给金铲铲棋盘参考图 `huaban-7096905062.jpg` 要求复刻；先做出独立 2D
Canvas 复刻页，随后明确「和其它 12 个棋盘保持一致」⇒ **孤立页已移除**，
改为 world-pack 的第 13 个画境，走同一套 `?world=` 切换 / 门禁 / Pages 发布。
以本节为最新状态；§「12 画境视觉包与可切换场景」保留为历史记录。

### 契约与真源

- `web/worlds.json`：`source.count` / `worlds.length` 12→**13**，新增
  `jcc-golden-pavilion`（index 13 / tier high / preset `jcc`），overrides 仍是
  `sky / ground / board / scenery / lighting` 五键。`assets/worlds/manifest.json`
  `count` 11→**12**（11 张 AI 文生图 + 1 张程序化重绘）。
- **几何冻结契约一个字节没动**：`board-spec.json` / `zboard.py` / ZBRD /
  `style-pack.json` / `visual-pack.json` / Unity 场景均不改；棋盘仍是画进地形
  的一块区域（`boardSlabMeshes === 0`）。参考图是花瓣形连续台面 + 亭台 + 飘带，
  与「最外方格 + 两个棋手占位」不兼容，只能作为**视觉层**，不能成为几何源。
- 概念图 `assets/worlds/jcc-golden-pavilion.png`（1024×819 / sha 锁定）是
  参考图的**程序化 Canvas 重绘**，不是 AI 文生图、不是原图复制；用户原 JPG
  留在 `assets/worlds/jcc-golden-pavilion-reference.jpg` 作溯源，
  `build_pages.py` 只收 `.png/.json`，不发布它。`worlds.json` 的
  `source.policy` / `contract.concept_images` 已如实标注这一例外。
- `web/world-scenery.mjs` 新增 `buildJCC`：双层飞檐亭阁、悬挂宫灯、屏风牌坊、
  云片盆景、贴地光环、可点击界标，全部 InstancedMesh / 程序化原语 +
  该画境调色板。**12 个非 starry preset 各登记 3 处 feature = 36**，
  入场分镜从地标自动派生，没有手写第二份坐标表。

### ⚠️ 3D 飘带的贴脸坑（本轮实测三轮才修掉）

**不要在沉浸式 3D 场景里用「大半径 + 大角度倾斜」的 Torus 模拟飘带。**
首轮 `TorusGeometry(r=25..32, tube=0.55)` + `rotation.z` 大旋转 → 圆环近端贴到
半径 32 的相机轨道上，画面被一条淡紫竖带从上到下切成两半；第二轮缩到
r=20..23、近水平旋转，仍有一条带子斜插天空。根因与既有「相机轨道避让」教训
同源：只要环平面接近竖直且半径落在 `CAMERA_ORBIT_R ± CAMERA_CLEARANCE`
(=24..40)，低仰角相机就会被近端糊住。
**最终解**：改成贴地 `RingGeometry`，水平旋转 `rotateX(-Math.PI/2)` **烘进几何体**
（不用 object.rotation），`y≈0.09`、`opacity 0.20`。飘带意象只留在概念图里，
3D 侧与 12 画境的地面笔触同层，零贴脸风险。

### 门禁与验证（全部实跑）

- `verify/test_zbrd_js.mjs` **45 checks**：所有硬编码 12→13 / 11→12 同步
  （source.count、worlds.length、id 唯一数、index 序列、调色板指纹数、
  high 数、preset 唯一数、概念图 sha 数、地标 `feature` 计数 33→36）。
  `verify/test_board.py` **92 tests** 不受影响（几何未改）。
- `build/_pw/worlds_probe.cjs`：**13/13 ok**、地面签名 13/13 不同、console errors 0。
  `build/_pw/pages_probe.cjs`：**13 worlds + 根跳转 + 移动端全绿**。
- **探针 FOV 假失败**：入场环绕镜头会临时接管 `camera.fov`（约 44~58），
  竖屏在 `loading.done` 后立刻取基线会读到 44；横屏的等待条件 `fov < 80`
  又会被这个中间值立刻满足。修法：竖屏等 `fov > 80`（tunedPreset 85.8）、
  横屏等 `66 < fov < 80`（tunedPreset 69.96）。`build/_pw/` 被 `.gitignore`
  吃掉，该修正只在本地生效，换机器需重打。
- `tools/build_pages.py` 已本地重建 `pages/`（33 文件 / 22.7 MB / 12 张概念图），
  title / concepts 断言 11→12。**线上 master + gh-pages 双分支未推送**
  （清空旧文件 + push 是破坏性远程操作，需用户确认后再做）。

## OSGame 棋盘线 · 第 13 画境改为「枫染棋坪」实机截图原画直出（2026-11-09）

**修正**：上一节「流金棋亭」的复刻方向作废。用户后来提供了真正的金铲铲
实机棋盘截图（1920×864：红枫覆地 / 云纹浅台 / 左侧朱红鼓车 / 右侧岩壁
橙焰灯列 / 上方青石踏步 / 前景枫叶 / 中央小小英雄），指出程序化 3D 场景
「长得一点也不一样」。程序化 Canvas 重绘的概念图与 `replica/` 已删除。

- **唯一能保证「和截图一模一样」的做法 = 直接用这张图**。世界顶层新增
  `artwork.mode = pixel_faithful`：`web/starry.html` 的 `#artwork` 层把
  `assets/worlds/jcc-maple-autumn.png` 按原比例整幅居中（`object-fit: contain`、
  不裁切 / 不重绘 / 不加滤镜），底层用同图放大模糊铺底消除宽高比留白。
  `body.artwork-mode #stage canvas { opacity: 0 }` 只把 3D 画布藏起来，
  **3D 场景照常构建**，所以 13 画境各自的地面签名、`boardSlabMeshes === 0`、
  ceremony、入场分镜与 `gl.readPixels` 探针一个都不破。
- `?clean=1` 连 HUD / controls / thumb / credits / ceremony 一起隐藏，得到与
  来源截图逐像素一致的画面；默认仍保留 13 画境统一的 HUD 与画境下拉。
- 数据契约照旧：`worlds.json` 第 13 个世界（id 由 `jcc-golden-pavilion`
  改为 `jcc-maple-autumn`）、`?world=` 与下拉、`world-pack.mjs` 深合并、
  `world-scenery.mjs` 的 12 个非 starry preset（36 处 `feature`）、概念图 sha
  双写 `assets/worlds/manifest.json`、`build_pages.py` 发布 `pages/`。
  ⚠️ `artwork` 必须放在**世界顶层**而不是 `overrides`：JS 门禁钉住
  `overrides` 只允许 sky / ground / board / scenery / lighting 五个视觉层；
  同时 `concept.prompt` 对每个世界都必须存在（第 13 个写来源说明，不是文生图提示词）。
- `artwork.image` 的 sha 必须等于 `concept.sha256`，`build_pages.py` 会在
  发布产物里再校验一次；`artwork.grid`（7 列 × 8 行，quad 是原图像素四角）是
  **可选**叠加，默认关闭，用于把 board.zbrd 的逻辑格透视映射到原图棋盘区，
  不产生任何新几何。
- `buildJCC` 保留为「3D 近似」备份（关掉原画直出即可对照），它不是复刻。
  探针 `capture()` 只截 WebGL 画布，验收原画必须另用 `page.screenshot()`
  且加 `&clean=1`。

### 关于「能不能直接生成模型」

- **能，而且项目本来就有**：`style-pack.json` 的 `external_mesh_contract`
  与 `build/external_mesh.manifest.json` 里 4 个 `asset_origin=hunyuan_3d`
  的 glb（star_pillar / halo_ring / moon_orrery / ...），带 mesh / texture /
  ZBRD cache 的 sha256、pivot_offset、`instances[].anchor`，由 Unity
  （ZExternalMeshBinder）/ Blender（build_ac90.py）/ Web（external_mesh.mjs）
  三端同源消费。接管流程
  `python external_mesh.py --import-glb <asset_id> --source <glb>`。
- `web/starry.js` 的 `USE_EXTERNAL_PROPS = false` 只是沉浸式页默认**不陈列**
  那 6 个实例，数据验证页仍可验收；不是没有模型链路。
- 但契约明确禁止用外部 glb 生成「最外方格 / 包边 / 两个棋手占位 / 天穹」，
  棋盘轮廓仍只来自 `board.zbrd`；整张 2D 等距手绘棋盘要拆十几个单件模型，
  AI 生成只能语义近似，无法复现笔触 / 透视 / 光照 ⇒ 达不到「一模一样」。
- 本次也用图生 3D 生成了一个朱红鼓车单件作为能力演示，产物落在
  `build/_pw/generated/`（不进 git、不进 pages），**没有写进冻结契约**。
  若要正式接入，必须走 `--import-glb` 重算 sha / pivot / scale 并对拍三端。

**线上 master + gh-pages 双分支仍未推送**（清空旧文件 + push 属破坏性远程操作，
需用户确认后再做）。

## OSGame 棋盘线 · 第 13 画境补上真正可交互棋盘（2026-11-09）

用户追问「棋盘没有吗，只是一张图吗？」⇒ 补上从 board.zbrd 派生的棋盘层。

- **项目里本来就有真棋盘，不是只有概念图**：`build/board.zbrd` 17 parts，
  一整块 `core_slab` 连续台面 + 4 段包边 + 2 个 `anchor_*` 棋手占位；
  `board.manifest.json` 给 `grid.columns=7 / rows=8`（56 逻辑落点）、
  `logical_bounds_xz=[-8.062, 8.063, -7.2, 7.2]`、
  `anchors=[seat_home(-10.2,0,9.0), seat_guest(10.2,0,-9.0)]`。
  Unity HD 场景 / Blender / Web 三端消费同一份几何。沉浸式页刻意把棋盘
  「画进地面」（`boardSlabMeshes===0`），不是没有棋盘。
- ⚠️ **修正笔误**：`artwork.grid` 之前写成 `cols=8 / rows=7`（乘积同为 56，
  门禁只比乘积没抓到）。已改为 `cols=7 / rows=8`，JS 门禁改成逐字段对齐
  `manifest.grid.columns / rows`。
- 棋盘交互层：`#artwork-grid` canvas（内部像素 = 原图 1920×864），
  从 `state.manifest` 读行列 / 逻辑边界 / 锚点，`art.grid.quad` 只负责把
  世界坐标透视映射到原图像素；双线性四边形用牛顿迭代求逆，把鼠标位置还原成 (u,v)。
  绘制最外方格 + 7×8 参考线 + 两个棋手占位（外插到左下 / 右上对角），
  hover 高亮、点击落子（交替两色）、坐标回显（格号 + 世界坐标）。
- **默认关闭**以保住 `?clean=1` 的逐像素一致；`?board=1` 或右侧
  「显示棋盘 · 可落子」打开；两者同时给时纯图优先。
  ⚠️ `applyArtwork(pack)` 必须在 `loadBoardRegion()` **之后**调用（依赖 state.manifest）。
- probe 增加 `artwork.boardVisible` / `boardLayer{columns,rows,anchors,pieces}`；
  `worlds_probe.cjs` 在 `?board=1` 下点两格断言 `pieces===2`。
- 验证：JS 门禁 **49/0**（新增「棋盘真源」check）、Python **92 OK**、
  worlds_probe **13/13**（board layer 7/8/2/2）、pages_probe all ok、
  纯净页逐像素 **1,658,880 / 0 差异**。

### 图生 3D 模型「正式接入冻结契约」的影响面（待用户定）

- 已在 `build/_pw/generated/` 生成 `drum_cart.glb`（27.6 MB，PBR）+ OBJ +
  预览图 + `generated_model_viewer.html`；未进 git / 未进 pages。
- 走 `external_mesh.py --import-glb` 正式接入需要：在 `style-pack.json`
  新增第 5 个 `external_mesh_contract.items` + 一个 `decorations` style item，
  重跑 `zboard.py --variants` 派生 ZBRD / 4 个 Unity 场景 / Blender，
  刷新 `board.manifest.json`（parts 17→18、variable 6→7、assets 4→5、
  instances 6→7）与 `external_mesh.manifest.json`，同步改 Python / JS 门禁。
- ⚠️ **style-pack 是全局真源**：新增资产会出现在**全部 13 个画境**的外围；
  而第 13 画境走原画直出、3D 画布 opacity 0，**反而看不到这个模型**。
  所以「正式接入」对本次需求没有视觉收益，却会动冻结几何契约。
  替代：Web 端局部加载（只在第 13 画境关掉原画时显示），不动三端契约。
