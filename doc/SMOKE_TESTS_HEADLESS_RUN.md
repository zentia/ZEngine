# 冒烟测试全量无头运行（web 构建 + node）

> 2026-10-25。仓库里 49 个 `*SmokeTest.cpp` 第一次被逐个编译、链接、运行。此前它们只能靠"提交时作者跑过"，
> 这台机器上没有可用的原生工具链（MSBuild 被沙箱拦截），大多数链接 ZRuntime 的测试从未在这里跑过。

## 怎么跑

```bash
python scripts/run_all_smoke_tests_web.py                 # 全部，结果写 build-web/smoke_node/all_results.json
python scripts/run_all_smoke_tests_web.py --filter Lumen  # 子串或逗号分隔的名字列表
python scripts/web_smoke_run.py <某个 SmokeTest.cpp> [--extra x.cpp] [--include-dir d] [--define N=V] [--link lib.a]
```

`web_smoke_run.py` 借用 build-web 里一个现成 smoke 目标的编译/链接命令（同一套 define / include / PCH），
把测试 `--whole-archive` 链接到 `bin/libZRuntime.a`，在 node + NODERAWFS 下运行（读写真实文件系统）。
每个测试需要的额外 TU / include / define / 静态库（对应其 CMakeLists 的 `target_*`）登记在
`run_all_smoke_tests_web.py` 顶部的 `EXTRA` / `INCLUDE_DIRS` / `DEFINES` / `LINK` 表里。

判定：`PASS` 退出 0；`FAIL` 有断言失败；`BUILD` 编不过；`ENV` 跑起来了但卡在 node/wasm 环境限制上
（不说明被测代码有问题）；`SKIP` 本就只能在某个原生后端上跑。

## 结果

| 判定 | 数量 | 测试 |
|---|---|---|
| PASS | 35 | 见下 |
| ENV | 7 | CrashLogTail、Pptr、Addressables（需要 `std::thread`，web 构建无 pthreads）；PPtrLifetime、UafDetection（要起子进程）；DerivedDataCache（见下）；LumenRenderPassWiring（37 项里 35 过，另 2 项要求 `RenderPipelineBase::UpdateLumen` 带 `Z_HAS_LUMEN` 编译，而 web 的 libZRuntime 里它是空分支，`--whole-archive` 又换不掉） |
| SKIP | 7 | DX12Bindless、VulkanBindless（要 GPU 设备）；AppleSceneBlit、MetalSceneDepth、MetalSpirvShader（Apple）；PosixCrashCapture（POSIX 信号 + fork，WSL 里跑）；VirtualTextureStreaming（`VirtualTextureManager.cpp` 无条件 include `vulkan/vulkan.h`，由 `scripts/verify_vt_wasm.sh` 用影子 VulkanRHI 覆盖） |
| FAIL / BUILD | 0 | |

（首轮：PASS 22 / FAIL 8 / BUILD 12 / SKIP 6；修完下表问题、补齐依赖登记后为上表。）

PASS 的 35 个里，**第一次真正跑通**的包括：Lumen 6 个（GI 49 项、光线追踪、反射、场景几何、表面缓存、
逐帧桥）、DirectionalLightShadow、TextureCompressor（BC7 / ASTC 真编码）、AssetBundle、Animator、
ProjectInfoPaths、RHIFactoryRegistry 的 Web 分支，以及新增的 ConsoleVariablesFile。

DerivedDataCache：LMDB 通过 `MAP_SHARED` 映射读回自己用 `pwrite()` 写入的数据；emscripten 的 mmap 是私有拷贝，
提交后的值对下一个读事务不可见（探针：put / commit 都返回 0，get 返回 `MDB_NOTFOUND`；改用 `MDB_WRITEMAP` 则
commit 时 msync 报 EINVAL）。DDC 只在编辑器的贴图导入里用，web 运行时不会打开它，所以归为环境限制。

## 跑全量时发现并修掉的问题

| 问题 | 影响 | 修法 |
|---|---|---|
| `PPtrLifetimeSmokeTest` 在 `RunChildProbe` 里用 `g_Argv0`，声明却在函数后面 | 非 Windows 平台（Linux / macOS / web）这个测试**根本编不过** | 声明挪到使用之前 |
| `RHIFactoryRegistrySmokeTest` 的平台矩阵里 `__EMSCRIPTEN__` 分支排在 Linux 分支后面 | emcc 继承 `Z_PLATFORM_LINUX=1`，web 构建永远走 Linux 分支断言 Vulkan → 2 项失败；Web 的断言一次都没执行过 | Emscripten 分支提到最前，并补"web 上不注册 Vulkan" |
| lmdb 在 web 上按 `MDB_USE_ROBUST=1` 编译 | 任何链接 `LMDBDerivedDataCache` 的 web 可执行文件链接失败（`pthread_mutexattr_setrobust` 未定义） | `engine/3rdparty/CMakeLists.txt`：Emscripten 与 OHOS 一样 `MDB_USE_ROBUST=0` |
| `ConsoleVariable<int/float>::setStringValue` 用 `std::stoi` / `std::stof` + `catch (...)` | `"12abc"` 被当成 12；web 构建关闭了异常捕获，非数字值直接让进程 abort | 改为整串校验的无异常解析（int：`std::from_chars`；float：`strtod` + 整串 / 有限值 / 范围检查） |
| float CVar 的 `getStringValue` 用 `std::to_string` | 固定 6 位小数，`1e-7` 变成 `0.000000`，存了读不回原值 | 输出能精确读回的最短 `%g` |
| 测试工具：NODERAWFS 把 Windows 风格的 `process.cwd()` 交给 wasm | `getcwd()` 失败，`std::filesystem::current_path()` / `absolute()` 抛异常（AssetBundle、Animator、ProjectInfoPaths 因此失败） | `web_smoke_run.py` 的 pre-js 把 cwd 改成不带盘符的 `/x/y` 形式 |

另外新增 CVar 存盘/读盘（`cvar.save` / `cvar.load`，见 `CONSOLE_MANAGER_USAGE.md`），测试 41 项。

2026-10-26 复跑（资产路径规范化 `AssetManager::CanonicalPathKey`、`Level` 序列化钩子、活 GameObject 分配实例 ID
之后）：结果不变，PASS 35 / ENV 7 / SKIP 7 / FAIL 0。CVar 测试随启动文件功能增至 56 项；
Prefab 端到端（`scripts/verify_prefab_e2e.sh`，不在本表的 49 个之内）增至 125 项。

2026-10-26 第二次复跑（YAML 资产改用路径派生实例 ID 之后）：34 PASS / 1 FAIL / 7 ENV / 7 SKIP，那 1 个 FAIL 是
`PlayerSettingsSmokeTest`，原因是**宿主环境**而不是代码：WorkBuddy 给 node 注入的 safe-delete 拦截层对"同一轮删除超过
50 个文件"抛异常（`SAFE_DELETE_BULK_CONFIRM_REQUIRED`），全量跑到它的 `remove_all` 时正好触顶；单独
`--filter PlayerSettingsSmokeTest` 复跑 PASS。在普通终端里跑全量不会遇到。Prefab 端到端增至 181 项。

2026-10-26 第三次复跑（`Type::isEditorOnly` 进入运行时类型、场景烘焙 SceneCook 之后，全库重编）：34 PASS / 7 ENV /
7 SKIP / 1 BUILD，BUILD 是 PrefabEndToEnd 新依赖 `Editor/Build/SceneCook.cpp` 没登记进本脚本的额外源文件表，补登记后
单独复跑 PASS（207 项），PlayerSettings 这次也 PASS。

## 局限

- 这是 **wasm 单线程 + node** 环境：凡是被测对象本身要线程、子进程、GPU、真 mmap 的，只能标 ENV / SKIP，不能当成通过。
- Lumen 测试是把 `Function/Render/Lumen/*.cpp` 额外编进测试、定义 `Z_HAS_LUMEN=1` 跑的（web 的 libZRuntime 剥掉了 Lumen）。
- 原生 Windows / Linux / macOS 构建的结果以各自工具链为准；这里只能证明"逻辑在 wasm 上成立"。
