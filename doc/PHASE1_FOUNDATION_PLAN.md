# 阶段一 · 夯地基（Foundation Hardening）规划与落地记录

> 本文档是 ZEngine "阶段一·夯地基" 的**单一源**。所有薄弱点结论、落地顺序、
> 技术方案都沉淀在这里；每个子项动手后在此追加"落地记录"，不另建文档。
>
> 结论等级：**Blocker / High / Medium / Low**（对应审计严重度）。
> 环境事实：ZEngine 已相当成熟，阶段一**不是从零建地基，而是审计现有地基后补齐薄弱点**。

---

## 0. 前提修正（重要）

规划前的只读审计推翻了两个初始假设，必须记录在案，避免后续基于错误前提动手：

1. **ZEngine 不是"早期自研引擎"**。它已有：mimalloc 内存管理器（含分配器配对守卫 +
   LLM 内存追踪）、完整的 `Object`/`ObjectManager`/`PPtr`/`ImmediatePtr` 对象生命周期体系、
   **四个** RHI 后端（Vulkan / D3D12 / Metal / WebGL2）、Windows SEH 崩溃处理器（质量很高）、
   BqLog 异步日志系统（跨平台日志目录 + 崩溃刷盘）、agentkit 崩溃归因工具链。
   → 阶段一定位必须从"建地基"改为"**审计 + 补齐薄弱点**"。

2. **agentkit 的 `zfault` 是故障注入实验台，不是崩溃归因工具**。真正消费 `crash_snap.json`
   的归因链是 `agentkit/zagent/`（`zengine_snapshot.py` + `lab_bridge.py`），且**仅支持 Windows/PDB**。

---

## 1. 薄弱点审计清单（按严重度）

> 每条都基于真实代码，给出落点文件:行号。动手前必须回读对应文件确认现状未变。

| # | 等级 | 薄弱点 | 主落点 | 状态 |
|---|------|--------|--------|------|
| 1 | 🔴 Blocker | Linux 桌面**完全不注册 RHI**，`GET_SYSTEM(RHI)==nullptr` | `Runtime/RegisterRuntime.cpp:140-142` | **已修复（第六落点，见 3.11）** |
| 2 | 🔴 Blocker | 崩溃捕获仅 Windows SEH，POSIX 是 no-op；非 Windows 入口点不安装任何处理器 | `Runtime/Core/Base/CrashHandler.h:604-608`；`Game/ZGame.cpp:288`；`Editor/Main/Main.cpp:765` | **已实现·待 POSIX 编译验证（第五落点，见 3.10）** |
| 3 | 🟠 High | `ImmediatePtr` 是无校验裸指针——悬垂指针主入口 | `Runtime/BaseClasses/ImmediatePtr.h:52` | ✅ 已修复（存 instanceID + Debug 校验，见 3.5） |
| 4 | 🟠 High | `RHIFactory` 是死代码（仅 `.mm`、无调用点、硬编码 switch、含无限递归），新增后端无注册表 | `Function/Render/Interface/RHIFactory.mm:33-64` | **已修复（第六落点，见 3.11）** |
| 5 | 🟠 High | **零 use-after-free 运行时检测**（无释放填充、无句柄校验、无 guard page） | `Runtime/Core/Memory/MemoryManager.h:118-128` | ✅ 已修复（UafReporter：instanceID 注销判据，见 3.5） |
| 6 | 🟡 Medium | `PPtr` 把"对象已销毁"静默升级为"从磁盘加载同 ID 对象"（`PPtr.h:79`），比返回 null 更危险 | `Runtime/BaseClasses/PPtr.h:73-81` | **已修复（第三落点，见 3.8）** |
| 7 | 🟡 Medium | 引用计数基础设施 95% 闲置（`AtomicRefCounter` 仅 1 个消费者），无弱引用 | `Runtime/Core/Thread/AtomicRefCounter.h:5` | **已修复（第七落点，见 3.12）** |
| 8 | 🟡 Medium | `ObjectFlags`（`OF_BeginDestroyed`/`OF_FinishDestroyed`）是死代码；两阶段销毁 `BeginDestroy`/`FinishDestroy` 是空桩 | `ObjectDefines.h:82-87`；`Object.cpp:19-20` | **已修复（第二落点）** |
| 9 | 🟡 Medium | `RHI.h` 抽象头泄漏 Vulkan VMA 头；124 个纯虚函数的胖接口 | `Function/Render/Interface/RHI.h:15-17, :179-682` | **VMA 头泄漏已修复（第八落点，见 3.13）；胖接口拆分后置** |
| 10 | 🟡 Medium | 崩溃快照 `platform` 硬编码为 `"windows"`；agentkit 无法解析非 Windows 快照 | `CrashHandler.h:508`；`agentkit/zagent/zengine_snapshot.py:112` | **已修复（随 #2，platform 运行时派生，见 3.10）** |
| 11 | 🟢 Low | `log.reliable_level=normal`；无崩溃安全环形缓冲，`force_flush` 失败则日志尾丢失 | `Runtime/Core/Log/LogSystem.cpp:165` | **已修复（第四落点）** |
| 12 | 🟢 Low | DX12 有 15 处 `// TODO: Implement`；WebGL2 仅骨架（`WEBGL2_TODO` 宏吞掉所有调用） | `DX12RHI.cpp:2399,3675,...`；`WebGL2RHI.cpp:30,388-482` | **热路径真 bug 已修复（第九落点，见 3.14）；其余 TODO 多为 DX12 不走 generic API 的 Vulkan-ism，WebGL2 全后端后置** |

---

## 2. 落地顺序（先低风险高收益、可在 Windows 本地验证的项）

原则：
- **先做纯 C++、跨平台、Windows 可编译验证、风险可控的项**（不碰 POSIX 代码、不碰需要
  Vulkan SDK 实际启动引擎的项），保证每一步都能本地跑通 CI。
- **先治"你在 OSGame 反复踩的坑"**——use-after-free / 悬垂指针 / 对象销毁后访问（`strlen(null)` 那类）。
- 涉及核心内存路径的改动，一律**只在 Debug/RelWithDebInfo 开启检测**，Release 零开销。

| 顺序 | 对应 # | 子项 | 为什么排这里 |
|------|--------|------|--------------|
| **1（当前）** | 3 + 5 | **UAF 检测 + ImmediatePtr 校验** | 直接治悬垂指针/strlen(null)；纯 C++；Windows 可验证；风险可控 |
| 2 | 8 | 落地两阶段销毁（用活 `ObjectFlags`）✅ 已完成（§3.7） | 与 #3/#5 同源，是完整生命周期模型的一部分 |
| 3 | 6 | `PPtr` 区分"已销毁"与"未解析" ✅ 已完成（§3.8） | 悬垂引用的另一主入口，与 #3 互补 |
| 4 | 11 | 日志崩溃安全环形缓冲 ✅ 已完成（§3.9） | 小而稳、跨平台可验证；为 #2 的 POSIX 崩溃捕获铺日志后路 |
| 5 | 2 | POSIX 崩溃捕获（sigaction + backtrace）✅ 已实现·待 POSIX 编译验证（§3.10） | 价值高但 POSIX 代码 Windows 编不过，需 Linux/macOS 验证，排到本地能测的项之后 |
| 6 | 1 + 4 | RHI：Linux 注册 + 后端注册表重构 ✅ 已完成（§3.11） | 需 Vulkan SDK 实际启动验证，环境成本最高 |
| 7 | 7 | 引用计数强化 + 弱引用 ✅ 已完成（§3.12） | 纯 C++、跨平台、Windows 双配置可验证；补齐句柄家族弱引用成员 |
| 8 | 9,12 | 其余 Low（#9 RHI.h VMA 头泄漏 ✅ 已修复见 §3.13，胖接口拆分后置；#12 DX12 热路径真 bug ✅ 已修复见 §3.14，其余 TODO/WebGL2 骨架后置） | VMA 头泄漏、DX12 泄漏/标记是单文件可安全修；胖接口拆分、完整 WebGL2 改动面大、后置 |

---

## 3. 第一落点：UAF 检测 + ImmediatePtr 校验（#3 + #5）

### 3.1 现状（动手前已读全相关文件）

**ImmediatePtr 是纯裸指针**（`ImmediatePtr.h`）：
- `T* m_Ptr;`（`:52`）——只存裸指针，不存 instanceID。
- `operator->()`（`:45-49`）——直接返回裸指针，**零校验**。
- `operator T*()`（`:44`）——同样零校验。
- `ImmediatePtrResolveRead`（`ImmediatePtr.cpp:40-65`）——内部**已经算出了 instanceID**
  （`:50` `LSOIToInstanceID` 或 `:54` 直接转换），但解析完就丢弃，只返回 `Object*`。

**对象销毁会注销 ID**（`Object.cpp:6-17`）：
- `~Object()` 里 `if (m_InstanceID != 0) manager->UnregisterInstanceID(m_InstanceID);`
- 即：对象一旦销毁，它的 id 从 `ObjectManager` 表里消失。
- `ObjectManager::IDToPointer(id)`（`ObjectManager.cpp:16-26`）对已注销的 id 返回 `nullptr`。

→ **天然探针**：只要 ImmediatePtr 存下目标 instanceID，访问时查 `ObjectManager` 表，
  查不到 = 对象已死 = use-after-free。素材（instanceID）在解析路径上已经算出来了。

**分配器语义在 Apple 上分裂**（`MemoryManager.cpp:24-28`）：
- 非 Apple：全局 `new`/`delete` 走 mimalloc。
- Apple：全局 `new`/`delete` **故意不接管** mimalloc（避免 dylib/host 分配器混用）。
- `Produce()`（`ObjectManager.cpp:28-47`）走 plain `new`；`NewObject<T>()`（`ObjectManager.h:93-99`）
  走 `MemoryManager::CreateObject`（mimalloc）。两条路径配对规则靠注释纪律维持。

→ **UAF 填充方案必须绕开这个坑**：不能依赖"在 `Free` 里填充"来覆盖所有对象，因为 Apple 上
  `delete` 不一定走 `MemoryManager::Free`。检测判据要用 **instanceID 是否已从表里注销**，
  而不是"内存是否被填充"。

### 3.2 方案（分两层，Debug 开启、Release 零开销）

**层 A：ImmediatePtr 存 instanceID + 访问校验（治 #3）**

1. `ImmediatePtr<T>` 增加 `int32_t m_InstanceID = 0;`，与 `m_Ptr` 并列。
2. `ImmediatePtrResolveRead` 改为同时回填 instanceID（新增出参，或返回一个
   `{Object*, int32_t instanceID}` 小结构），赋值给 `m_InstanceID`。
   - 注意：无 resolver 的 fallback 路径（`ImmediatePtr.cpp:54`）把 instanceID 塞进 pathID，
     同样能拿回 instanceID，逻辑自洽。
3. `operator->()` 和 `operator T*()` 在 **Debug/RelWithDebInfo** 下加校验：
   - 若 `m_Ptr == nullptr` → 直接断言/日志（空指针解引用）。
   - 若 `m_InstanceID != 0` 且 `ObjectManager::IDToPointer(m_InstanceID) == nullptr`
     → **对象已销毁仍在使用**，触发 UAF 报告（见层 B 的报告器）。
   - Release 下保持原始零开销路径（直接返回 `m_Ptr`）。
4. `operator=(T*)` 同步更新 `m_InstanceID`（裸赋值路径，instanceID 置 0，退化为"不校验"，
   但至少不会误报）。

**层 B：UAF 报告器（治 #5 的轻量落地）**

- 新增一个 header-only 的 `UafReporter`（放 `Runtime/Core/Memory/` 或 `Runtime/BaseClasses/`）：
  - `ReportUseAfterFree(int32_t instanceID, const char* access_site)`：
    - 累加计数（function-local static，避免 header 多 TU 各存一份——参考 `MemoryManager.cpp:145-154` 的做法）。
    - `LOG_ERROR` 打出 instanceID + 访问点，明确"这是 UAF，不是普通空指针"。
    - Debug 下 `Z_ASSERT` 直接断言中断，方便定位；Release 只计数不中断。
  - 复用现有 `LOG_ERROR` / `Z_ASSERT` 基础设施，不引入新依赖。
- **不做**释放内存填充（0xCD/0xDD）：因为 Apple 分配器分裂，填充无法覆盖所有释放路径，
  反而会引入"在 Apple 上崩在释放路径"的新风险。用 instanceID 注销作为判据更稳、更跨平台。

### 3.3 为什么这个方案对（关键判断）

- **判据选 instanceID 注销，而非内存填充**：对象销毁必然 `UnregisterInstanceID`，
  这是引擎里唯一"确定发生"的死亡信号，跨平台一致，不受 Apple 分配器分裂影响。
- **Release 零开销**：所有校验都 gate 在 `NDEBUG`/`ZENGINE_DEBUG` 之外，Release 直接返回裸指针，
  不影响线上性能——这符合引擎对性能敏感的一贯要求。
- **最小侵入**：只改 `ImmediatePtr` 一个类型 + 新增一个报告器，不碰 `ObjectManager` 表结构、
  不动 `Produce`/`NewObject` 分配路径。风险面收敛在一个文件。

### 3.4 风险与验证

- **风险点**：`ImmediatePtr` 被极广泛地 include（`Component.h` 等），改 header 要确保
  不破坏 ABI/序列化（`Transfer` 的 wire format 不变——instanceID 是运行时缓存，**不序列化**）。
- **验证方式**：
  1. `python zbuild.py configure --config debug` + `python zbuild.py build --target ZEditor`
     （或对应 smoke test target），确认编译通过。
  2. 写一个最小用例：`NewObject<T>` 创建对象 → 存进 ImmediatePtr → 销毁对象 → 再通过
     ImmediatePtr 访问 → 确认 Debug 下 UAF 报告器命中、Release 下不崩。
  3. 跑现有测试套件（`python zbuild.py test`），确认无回归。

### 3.5 落地记录

**已实现（2026-10-24）**：

改动文件：
- 新增 `engine/Source/Runtime/Core/Memory/UafReporter.h` + `UafReporter.cpp`：
  - `zengine::lifetime::IsInstanceAlive(instanceID)`——查 ObjectManager 表判断对象是否存活；
    instanceID==0 或单例未就绪时返回 true（不误报）。
  - `zengine::lifetime::ReportUseAfterFree(instanceID, access_site)`——计数（function-local static）
    + `LOG_ERROR` 明确"这是 UAF 不是空指针" + `ASSERT(false)`（Debug 中断）。
  - `GetUseAfterFreeCount()`——供测试/冒烟确认检测生效。
- 改 `engine/Source/Runtime/BaseClasses/ImmediatePtr.h`：
  - 新增 `int32_t m_InstanceID = 0;`（运行时缓存，**不进 Transfer wire format**，序列化布局不变）。
  - `operator T*()` / `operator->()` 改走 `ResolveWithCheck(site)`：`ZENGINE_LIFETIME_CHECK` 下
    指针非空+有 id+id 已注销 → 报告 UAF；Release 下直接返回裸指针（零开销）。
  - `operator=(T*)` 把 instanceID 置 0（裸赋值退化为不校验，避免强加 Object 依赖到模板头）。
  - header 加 `ZENGINE_LIFETIME_CHECK` 未定义时默认 0 的兜底，保证 header 自包含。
- 改 `engine/Source/Runtime/BaseClasses/ImmediatePtr.cpp`：
  - `ImmediatePtrResolveRead` 新增 `int32_t* out_instanceID` 出参，回填解析出的 id。
  - `Transfer` 读取分支接收并缓存到 `m_InstanceID`。
- 改 `engine/Source/Runtime/CMakeLists.txt`（两 target 都已创建之后）：
  - 注入 `ZENGINE_LIFETIME_CHECK`：多配置生成器用 `$<CONFIG:Debug/DebugV8/RelWithDebInfo>` 生成器表达式，
    单配置回退 `CMAKE_BUILD_TYPE`。**独立 gate，不依赖 NDEBUG**（RelWithDebInfo 虽定义 NDEBUG 也要开检测）。

验证结果：
- `cmake -S . -B build`（带 `VULKAN_SDK=C:/VulkanSDK/1.4.363.0`）reconfigure 成功，CMake 宏语法通过。
- `cmake --build build --target ZRuntime --config Debug` 全量编译 exit 0，`ZRuntime.lib`（1GB）生成。
  无错误；仅有 `RHI.h` 既有 C4100 未引用形参警告，与本次改动无关。
- `ZRuntime.dir/Debug/{UafReporter,ImmediatePtr}.obj` 已生成并链入 lib。
- vcxproj 确认宏注入正确：Debug/RelWithDebInfo=`1`，Release/MinSizeRel=`0`。

遗留 / 待办：
- ~~**尚未做运行期命中验证**~~ → **已完成（2026-10-24，见下）**。
- `operator=(T*)` 裸赋值路径不校验（instanceID 置 0）——这是有意的取舍（避免模板头拖进 Object 依赖）。
  若后续发现大量悬垂来自裸赋值路径，再评估用非模板基类承载校验。
- 本落点只覆盖 `ImmediatePtr`。`PPtr`（#6）是另一悬垂主入口，且语义更复杂（静默 reify 磁盘对象），
  按规划排到后续顺序 3。

### 3.6 运行期命中验证（smoke test，2026-10-24）

新增 `engine/Source/Runtime/Core/Memory/Test/UafDetectionSmokeTest.cpp` + 在
`Core/Memory/Test/CMakeLists.txt` 追加 `option(ZENGINE_BUILD_UAF_DETECTION_SMOKE_TEST)`，
并在 `Runtime/CMakeLists.txt` 对它做 main carve-out（`REMOVE_ITEM`，防 LNK2005）。

**验证目标**：证明检测**真的能抓到** UAF，而非"编译通过但形同虚设"。

设计（子进程隔离——关键）：
- 父进程跑**安全**判据（绝不触发真 UAF）：
  1. `IsInstanceAlive` 死活翻转（Destroy 后 id 从表注销 → false）——检测机制本身正确。
  2. round-trip 后的 `ImmediatePtr` 访问**活**对象 → 不误报（计数仍 0）。
  3. spawn 自身 `--uaf-probe` 子进程，Debug 期望非零退出、Release 期望零退出。
- 子进程 `--uaf-probe`（唯一触发真 UAF 处，隔离在独立进程）：
  round-trip → 访问活对象 OK → 销毁目标 → 再访问触发真 UAF。
  Debug 下 `ReportUseAfterFree` 的 `ASSERT(false)` abort → 非零退出；Release 下无断言 → 0。

为什么子进程隔离：`ASSERT(false)` = 标准 `assert()` = `abort()`。主进程里触发真 UAF 会被它自己测的
东西杀掉、无法回传结论。子进程隔离把"必崩探针"关进独立进程，父进程只观察退出码，**不改生产 API**。

踩到并修掉的两个真实坑：
1. **MSVC Debug 下 `assert(false)` 弹模态对话框**（Abort/Retry/Ignore）→ 子进程挂住 → 父进程
   `WaitForSingleObject(INFINITE)` 永久阻塞。解法：子进程在触发 UAF 前
   `_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG)` 抑制弹框（仍 abort、非零退出）；
   父进程等待加 30s 超时双保险。
2. **运行缺 `dxcompiler.dll`**：exe link 的静态库背后有 DLL 依赖，需把
   `C:/VulkanSDK/1.4.363.0/Bin` 与 `engine/3rdparty/mimalloc/bin` 加进 PATH。

构建 / 运行：
```
cmake -S . -B build -DZENGINE_BUILD_UAF_DETECTION_SMOKE_TEST=ON
cmake --build build --target UafDetectionSmokeTest --config Debug
# PATH 带 VulkanSDK/Bin + mimalloc/bin 后运行
```

验证结果（Debug，`ZENGINE_LIFETIME_CHECK=1`）——**8 check(s), 0 failure(s), exit 0**：
```
[ ok ] IsInstanceAlive == false after Destroy (id unregistered) — the death signal works
[ ok ] accessing a live object after round-trip does NOT false-positive
[ ok ] Debug: child probe aborted on real UAF (detection fires end-to-end)
```
日志出现 `Assertion failed: false ... UafReporter.cpp, line 61` = 子进程探针触发真 UAF、
ASSERT 生效的铁证（弹框已被抑制，子进程干净 abort）。至此第一落点从"编译通过"补齐到
"**运行期确认能抓到 UAF**"。

---

### 3.7 第二落点：两阶段销毁（#8）—— 让 ObjectFlags 从死代码变活

**动手前审计推翻了一处初始方案**：`BeginDestroy`/`FinishDestroy` 全代码库**无任何子类 override**
（只有 Object 基类的声明 + 空实现），`m_ObjectFlags`/`OF_*` 确实全是死代码（只有 Object.h 的
初始化，零读写）。所以接入调用链是安全的，不会突然触发意料外回调。

**发现并修正的关键设计缺陷**：最初把两阶段逻辑写进 `ObjectManager::Destroy`（delete 前调
BeginDestroySequence）。但审计 `MemoryManager::DestroyObject`（`MemoryManager.h:38`）发现它只调
`ptr->~T()`、**根本不走** `ObjectManager::Destroy`——而它才是 `NewObject` 的**主流销毁路径**。
若只挂 `ObjectManager::Destroy`，主流路径的 flags 仍不会被写，等于把"死代码"换成"半死代码"。

**最终方案**：把两阶段逻辑收敛成 `Object::BeginDestroySequence()`（`Object.cpp`）作为**唯一实现**，
两条入口都在 delete/析构之前调用它：
- `ObjectManager::Destroy(obj)`（`new` 出来的对象，Produce 路径）→ `BeginDestroySequence()` → `delete`
- `MemoryManager::DestroyObject(obj)`（mimalloc 出来的对象，NewObject 路径）→ 用
  `if constexpr (std::is_base_of_v<Object, std::remove_pointer_t<T>>)` 约束，**只对 Object 派生类**
  调 `BeginDestroySequence()`，非 Object 类型照常只释放（模板同样用来释放普通堆对象）。

`BeginDestroySequence()` 里：置 `OF_BeginDestroyed` → 调虚 `BeginDestroy()` → 置 `OF_FinishDestroyed`
→ 调虚 `FinishDestroy()`。基类两钩子保持空实现（子类 override 点）。**必须在 `~T()` 之前调用**——
此刻对象仍是最派生完整类型，虚钩子才能分发到子类 override（若在析构里调，虚表已回退到 Object，
只会调到基类空实现）。

配套：`ObjectDefines.h` 给 `enum class ObjectFlags` 补了标准位运算符 `|`/`|=`/`&`/`==`/`!=`
（强类型枚举本就不支持位组合，这是它作为 bitmask 的正当用法）；`Object.h` 加 `IsDestroying()`
（public）和 `GetFlags()`（protected）让 flags 有读者。

**验证（Debug）——新增 `ObjectLifetimeSmokeTest.cpp`，15 check(s), 0 failure(s), exit 0**：
用进程级 sink 回收钩子观测值（对象销毁后无法回传），两条路径都确认：
```
[ ok ] OM::Destroy: BeginDestroy/FinishDestroy 各调一次、顺序正确（Begin 先于 Finish）
[ ok ] OM::Destroy: 钩子内 IsDestroying()==true、OF_BeginDestroyed/OF_FinishDestroyed 已置、flags 累积
[ ok ] MemMgr::DestroyObject: 同样三次钩子 + flags 已写 —— 证明 mimalloc 主流路径不再是半死代码
[ ok ] non-Object type through DestroyObject: 只释放、无两阶段序列（SFINAE 约束生效）
```

改动文件：`Object.h`（IsDestroying/GetFlags/BeginDestroySequence 声明）、`Object.cpp`（实现）、
`ObjectManager.cpp`（Destroy 收敛调用）、`MemoryManager.h`（DestroyObject 加 if constexpr 调用）、
`ObjectDefines.h`（ObjectFlags 位运算符）；新增测试 `ObjectLifetimeSmokeTest.cpp` +
CMake 配置（option/carve-out，同 UAF 测试模式）。

至此 #8 完成：**ObjectFlags 从死代码变活，两阶段销毁覆盖两条销毁入口，运行期端到端确认**。

---

## 3.8 第三落点：PPtr 区分"已销毁"与"未解析"（#6）

### 现状（动手前已读全相关文件）

`PPtr<T>::operator T*()`（`PPtr.h:73-81`）在 `ObjectManager::IDToPointer(id)` 返回 null 时，
**无条件**掉进 `ReadObjectFromPersistentManager(m_InstanceID)`——把"表里查不到"一律当成
"磁盘上有个同 ID 对象等着被惰性加载"。问题在于"查不到"有两种截然不同的含义：

- **堆对象已销毁**：`ObjectManager` 给堆对象分配**单调递减的负 ID**（`ObjectManager.h:113`
  `m_NextHeapInstanceID` 从 -1 起，`AllocateAndAssignInstanceID` 每次 `fetch_sub(1)`），
  `~Object()` 销毁时把该负 ID 从表注销。此刻 PPtr 指向的是**已析构、内存可能已归还/复用**
  的对象——继续解析它就是 use-after-free。过去这里会掉进磁盘 reify，恰好因为 remapper 里
  没有负 ID 才"碰巧"返回 null，把 UAF 静默掩盖成一次磁盘查询。
- **磁盘对象尚未加载**：`AssetManager` 的 remapper 给持久化对象分配**单调递增的正 ID**
  （`Remapper.cpp:12` `IncreaseHighestInstanceID(2)`）。正 ID 查不到 = 该 asset 还没 hydrate，
  PPtr 的惰性加载契约就是要把它从磁盘 reify 出来——**这条路径必须保留**。

### 方案（用 ID 符号区分，返回 null 是修复本身、报告才 gate）

在 `operator T*()` 的 `IDToPointer` 未命中分支里，用 **instanceID 的符号**分流：

```cpp
if (m_InstanceID < 0)
{
#if ZENGINE_LIFETIME_CHECK
    zengine::lifetime::ReportUseAfterFree(m_InstanceID, "PPtr<T>::operator T*");
#endif
    return nullptr;   // ← 无条件：返回 null 本身就是修复
}
temp = ReadObjectFromPersistentManager(m_InstanceID);  // 正 ID：保留惰性 reify
```

**关键取舍（smoke test 实测纠正出来的）**：负 ID 未命中时**返回 null 必须无条件**，
不能 gate 在 `ZENGINE_LIFETIME_CHECK` 上。最初把整段 `if (m_InstanceID < 0) return nullptr`
都 gate 掉，结果 Release 下退回到改造前老路（掉进 `ReadObjectFromPersistentManager(负ID)`，
实测子进程 **0xC0000005 崩溃**）——证明"返回 null"对已销毁堆对象不是可开关的检测，而是
**正确性修复本身**；只有"报告"（`ReportUseAfterFree` 的断言 + 日志）才是 Debug 专属开销。
这与 `ImmediatePtr`（#3）不同：ImmediatePtr 存的是裸指针，Release 下直接返回裸指针即可；
PPtr 存的是 ID，Release 下也必须走"负 ID → null"的纠正分支，只是不报告。

符号判据的可靠性：堆 ID 恒为负、磁盘 ID 恒为正，二者永不相交（一个从 -1 递减、一个从 2 递增），
无重叠空间，判据是确定性的，不依赖"remapper 恰好没有负 ID"这类隐式不变量。

### 验证（Debug + Release）——新增 `PPtrLifetimeSmokeTest.cpp`

照 `UafDetectionSmokeTest` 的子进程隔离手法（`ReportUseAfterFree` 在 Debug 下 `ASSERT(false)`
会 abort，必崩探针必须关进独立进程）：父进程跑安全判据（活堆 PPtr 解析、空 PPtr 为 null），
spawn 自身 `--destroyed-probe` 子进程解引用销毁堆 PPtr。

```
# Debug（ZENGINE_LIFETIME_CHECK=1）：5 check(s), 0 failure(s), exit 0
[ ok ] live heap PPtr resolves to the live object (negative ID in table)
[ ok ] null PPtr (instanceID==0) resolves to null
[ ok ] child probe process spawned
[ ok ] Debug: child aborted on destroyed-heap PPtr deref (detection fires end-to-end)

# Release（ZENGINE_LIFETIME_CHECK=0）：5 check(s), 0 failure(s), exit 0
[ ok ] live heap PPtr resolves to the live object (negative ID in table)
[ ok ] null PPtr (instanceID==0) resolves to null
[ ok ] child probe process spawned
[ ok ] Release: destroyed-heap PPtr deref returns null (no dangling address, zero-overhead)
```

Release 子进程退出码从"崩溃 0xC0000005"变为"干净 0"，正是本次修复的直接证据。

改动文件：`BaseClasses/PPtr.h`（`operator T*()` 加符号分流 + `#include UafReporter.h` +
`ZENGINE_LIFETIME_CHECK` 兜底宏）；新增测试 `Core/Memory/Test/PPtrLifetimeSmokeTest.cpp` +
CMake 配置（`ZENGINE_BUILD_PPTR_LIFETIME_SMOKE_TEST` option + Runtime REMOVE_ITEM carve-out，
同 UAF/ObjectLifetime 测试模式，含 `ZENGINE_LIFETIME_CHECK` 注入）。

至此 #6 完成：**PPtr 不再把"已销毁"静默升级成磁盘 reify；负 ID 未命中一律返回 null（可报告），
正 ID 未命中保留惰性加载；Debug/Release 双配置端到端确认**。

---

## 3.9 第四落点：日志崩溃安全环形缓冲（#11）

### 根因
`log.reliable_level=normal`（`LogSystem.cpp:165`）下 BqLog 异步刷盘。崩溃时崩溃处理器调用
`force_flush_all_logs()` 把异步缓冲落盘——但这是**尽力而为**：若异步工作线程来不及排空（堆已损坏、
线程被饿死、或溢出丢记录），日志**尾部**就丢了，而那恰恰是崩溃前最后、最该看到的诊断记录。
原规划原话："无崩溃安全环形缓冲，`force_flush` 失败则日志尾丢失。"

### 修复：独立于 BqLog 的进程级环形缓冲
新增 `Runtime/Core/Log/CrashLogTail.h/.cpp`——一个进程级静态 seqlock 环形缓冲，存最近 N=256 条
**已渲染**日志，独立于 BqLog 缓冲与 LogSystem 单例（崩溃时单例可能已不存在）：
- **写（`Record`）**：日志线程同步写入。用全局自增计数器（**每次 +2**）分配唯一槽
  （`(my>>1) % kCapacity`，用满全部 256 槽）；槽的 seq 字段奇 = 写入中、偶 = 已完成。
  崩溃路径上**零分配**（只往静态数组 memcpy 预渲染文本）。
- **读/dump（`Dump`）**：崩溃处理器在 `force_flush_all_logs()` 之后调用，把缓冲落盘为
  `<dir>/crash_log_tail.txt`。双重读 seq 检测撕裂写并跳过；按 seq 升序输出（最旧→最新）。
  用 C stdio 低层写盘，容忍并发写。

### 接线
- `LogSystem::log()`：派发给 BqLog 后调 `recordTail()` 渲染并写入缓冲（渲染失败退化为只记格式串，
  崩溃路径上不抛异常）。声明自由函数 `ZEngineDumpCrashLogTail(dir)` 供崩溃路径调用（不碰单例）。
- `CrashHandler.h::ZEngineCrashHandler`：在 `ZEngineForceFlushLogs()` 之后调
  `ZEngineDumpCrashLogTail(dir)`，与 `crash_stack.txt`/`crash_snap.json` 同目录落盘。

### 踩到并修掉的关键坑（实测纠正，测试抓出来的真 bug）
1. **seqlock 奇偶标记根本性错误**：最初 `Record` 用 `+1` 计数器、以 `seq` 本身作完成标记、
   `seq|1` 作写入中标记。当 `seq` 为**奇数**时，完成标记 `store(seq)` 仍是奇数 → 读者据奇偶判定
   永久把它当成"仍在写"而跳过 ⇒ **丢掉一半记录**（所有奇数序号）。测试表现为"single-thread
   row 0 得到 rec-1 而非 rec-0"。修法：计数器改**每次 +2**，使完成标记恒为偶、写入中标记恒为奇，
   读者据奇偶判定永不误判。
2. **首条记录被吞**：计数器从 0 起时首条完成标记 `store(0)`，被 `Dump` 当成"从未写过"丢弃。
   修法：计数器初值/`ClearForTest` 归位为 2，保证首条完成标记非零偶数。
3. **命名空间限定遗漏**：`CrashLogTail.h` 定义在 `zengine::logtail`（嵌套命名空间），
   `LogSystem.h`（全局类）里最初写成裸 `logtail::` → C3861/C2653。改全 `zengine::logtail::`。
   教训：嵌套命名空间的引用必须全限定，尤其在被 PCH 拖入的 header-only 上下文里。

### 验证（新增 `Core/Log/Test/CrashLogTailSmokeTest.cpp`，Debug + Release 双配置）
零依赖（独立于 BqLog/LogSystem），无需系统 bring-up、无需子进程隔离，全平台可链可跑：
- 单线程：记 256 条 → Dump → 全部在、按写入顺序（最旧→最新）。
- 溢出回绕：记 512 条 → 仅保留最近 256 条、按序。
- 并发写不撕裂：4 线程 × 48 = 192 条（< 容量，可证无损）→ 一条不少、每条文本完整（无撕裂）。
- 空 dir 是 no-op；容量常量契约。
- **Debug + Release 均 11 check(s), 0 failure(s), exit 0**。

### 改动
- 新增 `Core/Log/CrashLogTail.h/.cpp`（seqlock 环形缓冲 + 落盘）。
- `LogSystem.h/.cpp`：`log()` 写缓冲 + `recordTail`/`toTailLevel` 辅助 + 自由函数 `ZEngineDumpCrashLogTail`。
- `CrashHandler.h`：崩溃路径 dump 日志尾。
- 新增 `Core/Log/Test/CrashLogTailSmokeTest.cpp` + `Core/Log/Test/CMakeLists.txt` + Runtime/CMakeLists.txt carve-out。

至此 #11 完成：**日志尾有了独立于 BqLog 的崩溃安全兜底，`force_flush` 失败也不再丢最近 256 条；
为 #2 的 POSIX 信号处理器铺好了日志后路**。

---

## 3.10 第五落点：POSIX 崩溃捕获（#2，顺带关闭 #10）

> ⚠️ **本落点状态：已实现·待 POSIX 编译验证**。本机（Windows）无任何 POSIX 工具链
> （无 WSL/gcc/clang/NDK，已实测确认），无法编译运行这段代码。以下结论基于**代码审查
> + Windows 侧不破坏验证**，POSIX 上的实际行为（信号是否被捕获、回溯是否可用、dladdr
> 是否带出符号）须在 Linux/macOS/Android/OHOS 构建跑过 `PosixCrashCaptureSmokeTest`
> 后才能定论。因此**不标 ✅**，与 #8/#6/#11（本机双配置端到端跑通）区分。

### 根因（两个 bug，不是一个是两个）
1. **POSIX 分支是彻底 no-op**（`CrashHandler.h:606`）：`InstallCrashHandler` 在非 Windows
   上是空函数，`sigaction` 都没装，进程踩到 SIGSEGV 直接按默认动作死掉，什么都不留。
2. **POSIX `main()` 根本没调用它**：`ZGame.cpp:288` / `Main.cpp:765` 的 `main()` 里连
   `InstallCrashHandler` 都没出现——只有 `WinMain` 分支调了。所以即便补上处理器，也得
   同时在两个入口点接线。

### 修复
**（1）`CrashHandler.h` 的 `#else` 分支**从 no-op 换成完整 POSIX 捕获：
- `sigaction(SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT, SA_SIGINFO|SA_ONSTACK)` +
  `sigaltstack`（16KB 备用栈：栈溢出时原栈已不可用，处理器必须在预留栈上跑）。
- **信号安全**是生死线：崩溃时进程状态已损坏，处理器里任何 `malloc`/`std::string`/
  `std::ofstream`/`snprintf`/`backtrace_symbols` 都可能死锁或二次踩内存，比不装更糟。
  因此只用 `write/open/close`（POSIX 保证信号安全）+ 栈上定长缓冲 + 手写 hex/dec。
- **符号化取舍（关键）**：计划原文写 `backtrace_symbols_fd`，但它在 bionic（Android）/
  musl（OHOS）上**不保证存在**，链接期缺符号会让处理器在这些目标上直接失效——而那正是
  最需要它的地方。改用 **`backtrace()`（纯栈扫描，无分配，各 libc 都有）取原始地址 +
  `dladdr()`（POSIX 标准，查 maps，无分配）补 (object, 基址, 符号名)**，配 addr2line
  离线解析。这是跨 POSIX 最稳的基线，也贴合本项目已有的 addr2line 工作流。
- 写完产物后 `signal(sig, SIG_DFL); raise(sig)`——复位默认动作再重抛，让 core dump /
  外层调试器仍收到原始信号。
- 产物 `crash_stack.txt`（人读）+ `crash_snap.json`（机读，与 Windows 同 schema），
  落在 exe 同目录，交给同一套 agentkit。

**（2）两个 POSIX `main()`** 在开头加 `InstallCrashHandler(version)`（与 `WinMain` 一致，
`#ifdef ZENGINE_PRODUCT_VERSION` 分流）。

**（3）顺带关闭 #10**：`platform` 字段过去硬编码 `"windows"`。现在 Windows / POSIX 两分支
各加 `PlatformString()`，按编译期宏派生 `windows/linux/macos/android/ohos`——OHOS/Android
的 `uname` 都报 "Linux"，必须先用宏区分，否则 agentkit 会把鸿蒙/安卓快照误当桌面 Linux。

**（4）CMake**：顶层 `CMakeLists.txt` 在非 Windows 全局加 `-fno-omit-frame-pointer
-funwind-tables`（否则 Release 省略帧指针，`backtrace()` 会断在第一帧）+ 链接期 `-rdynamic`
（否则 dladdr 解析不出本模块符号名）。Windows/MSVC 不受影响。

### 一个自我抓到的信号安全 bug
初版把日志尾 dump（`ZEngineDumpCrashLogTail(const std::string&)`，内部构造 `std::string`
= 一次 malloc）放在**写栈之前**。这违反"零 malloc"，且若它在堆已损坏时死锁，我们会连栈都丢。
修法：**重排顺序——先写 alloc-free 的 crash_stack.txt/crash_snap.json（必达保底产物），
再落盘日志尾**（此时栈已在盘上，这一步炸了也不影响）。全路径只此一次分配，且排在最后。

### 验证（Windows 侧不破坏；POSIX 待验）
- Windows：`ZRuntime.lib`（PCH 内含 `CrashHandler.h`）、`ZEnginePCH`、`render_doc`（真实消费者）
  全部重编通过——证明 POSIX 分支在 Windows 上完全惰性、不破坏任何消费者。
- `PosixCrashCaptureSmokeTest`（新增 `Core/Base/Test/`）：Windows 上编译成空壳 main（打印
  skip、exit 0）；POSIX 上 fork 子进程触发 SIGSEGV，父进程校验产物生成 + 记录到 SIGSEGV +
  含回溯帧 + dladdr 带出模块基址。CMake option `ZENGINE_BUILD_POSIX_CRASH_CAPTURE_SMOKE_TEST`
  默认 OFF，carve-out 无条件（GLOB_RECURSE 会扫到）。
- **待办**：在 Linux/macOS/Android/OHOS 上构建并运行该测试，确认信号被捕获、回溯可用后，
  再把 #2 标 ✅。

### 改动
- `Runtime/Core/Base/CrashHandler.h`：`#else` 分支实现 POSIX 捕获（sigaction + sigaltstack +
  backtrace + dladdr + 信号安全写盘）；Windows 分支加 `PlatformString()`、`WriteJson` 改用派生值。
- `Game/ZGame.cpp:288` / `Editor/Main/Main.cpp:765`：POSIX `main()` 装崩溃处理器。
- `CMakeLists.txt`（顶层）：非 Windows 加 `-fno-omit-frame-pointer -funwind-tables -rdynamic`。
- 新增 `Core/Base/Test/PosixCrashCaptureSmokeTest.cpp` + `Core/Base/Test/CMakeLists.txt` +
  Runtime/CMakeLists.txt 的 `add_subdirectory` + REMOVE_ITEM carve-out。
- `doc/PHASE1_FOUNDATION_PLAN.md`：本 §3.10；#2 标「已实现·待 POSIX 编译验证」、#10 标已修复、
  落地顺序表第 5 行标完成。

至此 #2 完成（代码层）：**非 Windows 从"崩溃什么都不留"变成"崩溃写栈 + 日志尾 + 机读快照，
产物与 Windows 同构"；`platform` 字段运行时派生**。POSIX 运行期确认待补。

---

## 3.11 第六落点：RHI 后端注册表 + Linux 注册（#1 + #4）

### 现状（动手前已读全相关文件）

**#4 `RHIFactory.mm` 是死代码**（`Function/Render/Interface/RHIFactory.mm`）：
- 只被 Apple 编译（`.mm`），其它平台根本没有这个翻译单元；全局 grep 确认**零调用点**。
- `CreateRHI()` 是硬编码 switch，`default: return CreateRHI(GetDefaultAPI(), ...)`
  在 api 不可用时调用自身 —— **无限递归**。
- 新增后端必须改这个 switch，无法自注册。

**#1 Linux 不注册 RHI**（`RegisterRuntime.cpp` 两处选择块）：
- `RegisterSystem()`（~:114-142）与 `RegisterRuntimeLight()`（~:222-248）都是
  `#if Android/OHOS … #elif Windows … #elif Apple … #elif Emscripten … #else (void)preferred_rhi; #endif`
  —— Linux 落进 `#else` 空分支，一个 RHI 都不注册，`GET_SYSTEM(RHI)==nullptr`。
- 但 `Z_HAS_VULKAN=1` 在 Linux 上**已定义**（`engine/CMakeLists.txt:238-239`，
  `ZENGINE_USE_VULKAN` 对 Linux 的 `else()` 分支默认 ON），`VulkanRHI.cpp` 能在 Linux 编译。
  即：Vulkan 后端可用，只是没被注册。

**⚠️ 动手前发现的先决问题（计划未充分说明）**：Runtime 源文件过滤链
（`Runtime/CMakeLists.txt:137-300`）只有 Android/OHOS、Apple、WIN32±Vulkan 分支，
**没有 Linux 分支**。其 `else()`（:131）只剔 WebGL2。而 `DX12RHI.cpp` 无条件
`#include <d3d12.h>`/`<d3dcompiler.h>`（无 `_WIN32` 守卫），`DX12MainCameraPass.cpp`
也无守卫 include `DX12RHI.h` —— 也就是说 **Linux 构建当前根本编不过**。#1 要真正成立，
必须同时补这个 CMake 缺口。

**注册机制**：`SystemRegistry::RegisterSystemAs<Derived, Base>()`（`SystemRegistry.h:52`）
注册具体类型并给 RHI 基类建别名。`GET_SYSTEM(RHI)` 走 `dynamic_cast` 兜底也能命中，
但 `RegisterSystemAlias<RHI>` 让 `typeid(RHI)` 直接可用（`RenderSystem::GetDependencies()`
返回 `GET_SYSTEM_TYPE(RHI)`，:980）。ZRuntime 用 `--whole-archive`/`/WHOLEARCHIVE`
（`Runtime/CMakeLists.txt:1076,1085`）链接，静态注册器会在 main 前跑。

### 改动

**#4：把死代码 `RHIFactory.mm` 换成跨平台注册表**
- 删除 `RHIFactory.mm`，新增跨平台 `RHIFactory.cpp` + 重写 `RHIFactory.h`：
  - `Registry()`：函数内 Meyers 单例 `std::map<GraphicsAPI, Entry>`（线程安全）。
  - `RegisterBackend(api, creator)`：幂等注册（后注册者覆盖）。
  - `CreateRHI(api, ws)`：查表构造；未命中退回 `GetDefaultAPI()`；默认也没有则
    **返回 nullptr（不再无限递归）**。
  - `GetDefaultAPI()`：平台"首选后端"决策（与"能否构造"分离）——Windows DX12、
    Linux/Android/OHOS Vulkan、Apple Metal、Web WebGL2。
  - `IsAPIAvailable(api)`：查注册表。
  - `RHIBackendRegistrar`：自注册助手（构造时调 `RegisterBackend`）。
- 每个后端 TU 末尾加匿名命名空间静态注册器：`VulkanRHI.cpp`、`DX12RHI.cpp`、
  `MetalRHI.mm`、`WebGL2RHI.cpp`（守卫内）。新增后端因此只需"实现 RHI + 放一个静态实例"。

**#1：Linux 注册 VulkanRHI + 补 CMake 缺口**
- `RegisterRuntime.cpp`：两处选择块各加 `#elif defined(Z_PLATFORM_LINUX) || defined(__linux__)`
  分支，`REGISTER_SYSTEM_AS(VulkanRHI, RHI)`；并加 Linux 的 `VulkanRHI.h` include。
- `Runtime/CMakeLists.txt`：在平台过滤链的 Android 块之后、`elseif(APPLE)` 之前，
  新增 `elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")` 分支，剔除 DX12/Metal 接口 TU、
  DX12-only pass（`DX12*`、`MainCameraRp2Pass`）、`ShaderLabDx12*`、Platform/Windows 文件，
  保留 VulkanRHI 及其 Vulkan 专属 TU。与 Android/Vulkan 移动端的剔除策略一致。

**验证**
- 新增 `Function/Render/Interface/Test/RHIFactoryRegistrySmokeTest.cpp`（option
  `ZENGINE_BUILD_RHI_REGISTRY_SMOKE_TEST`）+ 同目录 `CMakeLists.txt` 新块 +
  Runtime/CMakeLists.txt 的 REMOVE_ITEM carve-out。链接 ZRuntime（拿注册表 + 各后端
  注册器 + vulkan-1.lib），无头实例化后端（构造默认、析构空桩，无需 GPU）。
- 平台矩阵断言：Windows 双后端（DX12+Vulkan）都注册、未知 api（Metal）退回默认不递归；
  Linux 只 Vulkan 且 `IsAPIAvailable(Vulkan)==true`（本机 Windows 无法替 Linux 验证，
  只保证 Windows 矩阵正确，Linux 待真实机构建）。

### 结论
- **本机 Windows 双配置（Debug + Release）端到端跑通**：10 checks / 0 failures，
  覆盖 `IsAPIAvailable` / `GetDefaultAPI` / `CreateRHI`（DX12、Vulkan）/ 未知 api 退回默认
  （无递归）/ 幂等重注册。ZRuntime.lib 干净重建（仅 RHI.h 既有 C4100，与本次无关，属 #9）。
- **#1 的 Linux 部分**：代码 + CMake 已就绪，但**本机无 Linux 工具链，无法在此编译运行**，
  与 #2 同列"已实现·待真机验证"。Linux 上 `GetDefaultAPI()==Vulkan` 且注册成功的运行期确认，
  须在 Linux 构建跑 `RHIFactoryRegistrySmokeTest` 后定论。
- **#4 是跨平台改动、Windows 可完整验证**：注册表重构已在 Windows 证明两端后端都正确自注册、
  无限递归已消除。

### 改动文件清单
- `Function/Render/Interface/RHIFactory.h`（重写为注册表 + `RHIBackendRegistrar`）
- `Function/Render/Interface/RHIFactory.cpp`（新增，取代 `.mm`）
- 删除 `Function/Render/Interface/RHIFactory.mm`
- `Function/Render/Interface/Vulkan/VulkanRHI.cpp` / `DX12/DX12RHI.cpp` /
  `Metal/MetalRHI.mm` / `WebGL2/WebGL2RHI.cpp`：末尾加自注册器 + include RHIFactory.h
- `RegisterRuntime.cpp`：两处选择块加 Linux 分支 + Linux include
- `Runtime/CMakeLists.txt`：Linux 平台过滤分支 + 本测试 carve-out
- `Function/Render/Interface/Test/CMakeLists.txt`：新增 `ZENGINE_BUILD_RHI_REGISTRY_SMOKE_TEST`
- `Function/Render/Interface/Test/RHIFactoryRegistrySmokeTest.cpp`：新增
- `doc/PHASE1_FOUNDATION_PLAN.md`：本 §3.11；#1/#4 标已修复；落地顺序表第 6 行标完成。

至此 #1+#4 完成（代码层 + Windows 端到端）：**RHI 后端从"硬编码 switch + 无限递归 + 只有 Apple
有工厂"变成"跨平台自注册表 + 无递归 + Linux 桌面正确注册 Vulkan"；新增后端不再需要动工厂**。
Linux 运行期确认待补（与 #2 一并）。

---

## 3.12 第七落点：引用计数强化 + 弱引用（#7）

### 审计结论（动手前读全代码）
- **`AtomicRefCounter`（`Core/Thread/AtomicRefCounter.h`）原先只继承 `std::atomic_int`、只有强计数
  （Retain/Release/Count），全局唯一消费者是 `SerializedFile::m_RefCount`**——即规划里说的"95% 闲置"。
- **引擎对象体系不用 refcount 模型**：`Object` 的生命由 `ObjectManager` 的 instanceID 表驱动
  （NewObject→负 ID→注册；`~Object`→注销）。句柄家族原先只有"强引用/持久引用"成员——`PPtr<T>`
  （磁盘序列化引用）、`ImmediatePtr<T>`（裸指针 + UAF 校验的强引用），**独独没有弱引用**。
- 两个 gap：①引用计数设施闲置且无弱计数；②无弱引用句柄。

### 改动（纯新增、零风险、跨平台）
- **强化 `AtomicRefCounter`**（`Core/Thread/AtomicRefCounter.h`）：从 `std::atomic_int` 改为内嵌
  双原子计数（`m_Strong`/`m_Weak`）。**保持 `SerializedFile` 强计数 API 100% 兼容**（`Retain`/
  `Release`（仍返回"是否减到 0"）/`StrongCount`/`Count` 别名），新增弱计数 `AddWeakRef`（CAS 循环
  防与 Release 竞态、强计数归零则拒绝登记）/`ReleaseWeakRef`/`WeakCount`/`Expired`。
- **新增 `RefCounted<T>`（`Core/Base/RefCountedBase.h`，CRTP 混入基类）**：内嵌 `AtomicRefCounter`，
  成为其**第二个真实消费者**，直接消灭"闲置"。设计取向是 **owner-driven（持有方驱动销毁），不是
  shared_ptr 克隆**——`Release()` 返回"是否最后一个强引用"，由持有方决定销毁，与 `SerializedFile::
  Release()` 既有约定一致，也彻底规避"弱指针读已被释放的控制块"这一 shared_ptr 经典悬垂面。
- **新增 `WeakObjectPtr<T>`（`BaseClasses/WeakObjectPtr.{h,cpp}`）**：基于 `Object` instanceID 的
  弱句柄，复用 `zengine::lifetime::IsInstanceAlive`（即 `ObjectManager::IDToPointer` 查表）做存活
  探测。**关键区别**：弱引用访问已销毁对象**合法返回 null、不报告 UAF**（与 `ImmediatePtr` 强引用
  相反——强引用访问死对象才报 UAF）。header 保持轻量（解析走 `WeakObjectPtr.cpp` 的自由函数）。
  纯运行期句柄，不参与序列化（弱引用本质是运行期关系；持久化用 `PPtr<T>`）。

### 验证
- 新增两个 option-gated exe（复用 `Core/Memory/Test` 接线 + carve-out）：
  - `RefCountedSmokeTest`（`ZENGINE_BUILD_REF_COUNTED_SMOKE_TEST`）：纯计数逻辑，无需系统 bring-up。
  - `WeakObjectPtrSmokeTest`（`ZENGINE_BUILD_WEAK_OBJECT_PTR_SMOKE_TEST`）：`RegisterCore` +
    `InitializeAll` 让 `ObjectManager` 可用。
- **Windows Debug + Release 双配置端到端全通过**：RefCounted **36/36**、WeakObjectPtr **21/21**。
  ZRuntime.lib 干净重建（含 `SerializedFile.cpp`，证明强计数 API 向后兼容）。

### 踩坑
- `WeakObjectPtrResolveRaw` 在 `Resolve()` 里被调用，但只作为 `friend` 声明在类尾——MSVC 要求调用点
  可见的前置声明，否则 C3861。解法：类外前置声明该自由函数。
- 测试里 `wp = p`（`WeakObjectPtr = T*`）缺 `operator=(const T*)` → C2679。补上从 `T*` 与从
  `WeakObjectPtr` 的赋值运算符。
- 解析返回 `Object*`，模板 `T*` 需 `static_cast<T*>`（多态派生类型安全）→ 否则 C2440。

### 改动文件清单
- `Core/Thread/AtomicRefCounter.h`：强/弱双计数（保持 SerializedFile 兼容）
- `Core/Base/RefCountedBase.h`：新增（CRTP 混入基类）
- `BaseClasses/WeakObjectPtr.{h,cpp}`：新增（弱引用句柄 + 解析实现）
- `Core/Memory/Test/{RefCountedSmokeTest.cpp, WeakObjectPtrSmokeTest.cpp}`：新增
- `Core/Memory/Test/CMakeLists.txt`：两个 option 块
- `Runtime/CMakeLists.txt`：两个 carve-out
- `doc/PHASE1_FOUNDATION_PLAN.md`：本 §3.12；#7 标已修复；落地顺序表第 7 行完成。

至此 #7 完成：**引用计数设施从"只有强计数、单一消费者、无弱引用"变成"强/弱双计数 + 第二个真实
消费者（RefCounted）+ 补齐句柄家族弱引用成员（WeakObjectPtr）"；弱引用与强引用的 UAF 语义边界明确**。

---

## 3.13 第八落点：RHI.h VMA 头泄漏修复（#9 前半）

### 审计结论（动手前读全代码）
- `RHI.h:15-17` 在 `#if defined(Z_HAS_VULKAN)` 下无条件 `#include <vma/vk_mem_alloc.h>`。
  而 Windows 默认 **DX12 与 Vulkan 同时定义**（`engine/CMakeLists.txt:20`），于是所有 include
  `RHI.h` 的 50+ 个 TU（DX12 / Metal / WebGL2 / Editor / Lumen / DebugDraw / RenderGraph…）
  都被迫拖入 VMA 头——这些后端与调用点根本不用 VMA。
- RHI.h 本体**已不再引用任何 VMA 类型**：`CreateGlobalImage`/`CreateCubeMap` 的 image_allocation
  参数早已改成 `void*`（原注释 "Changed from VmaAllocation& to void*&" 就是证据），
  VMA 专属的 `CreateBuffer*VMA` 两个 override 也只在 Vulkan 后端实现（用 `void*` 签名）。
  ⇒ 这个 include 是纯粹的泄漏，删掉即可。

### 为什么不能顺手把"胖接口"也一起拆
- RHI.h 实测 **124 个纯虚 + 11 个默认实现 = 139 个虚函数**，683 行。把命令录制 / 资源创建 /
  销毁 / 查询拆成多个子接口（RHIDrawCommands / RHIResourceFactory / RHIDestroyable…）是正确方向，
  但会**触碰全部 4 个后端 + 每个调用点**，属改动面大、易引入跨后端不一致的高风险 churn，
  不符合阶段一"每步 Windows 可验证、风险可控"的原则 → 后置。
- 顺带核实：`CreateCommandPool()`（无参重载，`:339`）**不是死代码**——4 个后端 init 路径都调它
  （`VulkanRHI.cpp:243` 等），是 load-bearing 的，勿删。

### 改动（单文件、纯删除）
- `Function/Render/Interface/RHI.h`：删掉 `#if defined(Z_HAS_VULKAN) #include <vma/vk_mem_alloc.h>`
  块，替换为一段说明"为何此处不再需要 VMA 头、Vulkan 后端自行 include、消费者各有 guarded fallback"
  的注释（防回退）。

### 下游消费者核实（删前逐一确认各有自己的 VMA 来源，不依赖 RHI.h 传递）
- `RenderResource.h`：经 `RenderGpuResources.h`（含 `#if Z_HAS_VULKAN … #else VmaAllocation=void*`
  guarded fallback）拿到 `VmaAllocation`。
- `TextureStreamingManager.cpp` / `VirtualTextureTypes.cpp`：分别经 `TextureStreamingTypes.h`、
  `VulkanRHI.h` 拿到 VMA 类型。
- `DebugDrawFont.h`：自己 `#include <vma/vk_mem_alloc.h>`。
- Vulkan 后端 `VulkanRHI.h:12` 本就 include VMA 并用 `VmaAllocator` 成员。

### 验证（Windows Debug + Release 双配置端到端）
- ZRuntime.lib **Debug + Release 均干净重建**（exit 0，零 error）；RHI.h 的 50+ 消费者
  （DebugDraw / Lumen / RenderGraph / Texture / RenderResource…）obj 时间戳全部晚于 RHI.h 编辑
  时刻 ⇒ 确由本次改动触发重编、非吃旧 obj。
- 运行 `RHIFactoryRegistrySmokeTest`（#1+#4 的注册表路径）：**9/9 ALL CHECKS PASSED** ⇒
  删 VMA 头不影响 RHIFactory 后端注册 / CreateRHI 路径。

### 踩坑
- 无（本次是纯删除，且删前已核实所有消费者的 VMA 来源独立于 RHI.h）。

### 改动文件清单
- `Function/Render/Interface/RHI.h`（删 VMA include 块 + 加防回退注释）
- `doc/PHASE1_FOUNDATION_PLAN.md`：本 §3.13；#9 标"VMA 头泄漏已修复，胖接口拆分后置"。

至此 #9 的**可安全落地部分（VMA 头泄漏）**完成：抽象 RHI 头不再向 50+ 消费者泄漏 Vulkan VMA 头；
胖接口拆分作为独立的高风险重构后置。

---

## 3.14 第九落点：DX12 热路径真 bug 修复（#12 前半）

### 动手前审计：15 处 "TODO" 名不副实，逐一核实可达性
- 审计原文说 DX12 有 "15 处 `// TODO: Implement`"，但逐行读实现后发现**多数是 Vulkan-ism**——
  DX12 用**自有的** `m_CommandLists[]` / `m_Fences[]` / descriptor heap（init 时自建、全程自用），
  根本不路由 generic API，那些空桩**不是未完成功能**：
  - `AllocateCommandBuffers` / `CreateCommandPool`（无参重载）/ `CreateFence`：DX12 自建内部对象，
    generic 版本无调用点（`CreateFence` 的 generic 调用只在 ParticlePass，DX12 走自己的 pass）。
  - `AllocateDescriptorSets`（:2403）：**其实早已完整实现**，:2400 的 TODO 是错位的注释
    （属于上面的 `AllocateCommandBuffers`）。
  - `CreateSemaphore` / `CreateViewportRenderTexture` 等：DX12 无对应类型 / 未走该路径。
  - `Invalidate/FlushMappedMemoryRanges`：DX12 用 coherent upload heap，Vulkan 才需要手动 flush
    （`vkFlushMappedMemoryRanges`）；DX12 的空桩语义正确。
- **真正在 DX12 热路径上、且是"静默错/真 bug"的只有两处**（其余 WebGL2 全后端、胖接口另计）：

### 修复一：`DestroyShaderModule` 是真内存泄漏（静默错，比崩溃更危险）
- `CreateShaderModule` new 出 `DX12Shader`（持有 `ComPtr<ID3DBlob>`），但 `DestroyShaderModule`
  是空桩、从不释放。**~50 个调用点**（每个 pass、DX12 bindless blit/tonemap 管线、editor overlay、
  Nanite、Spine、debug draw）每次绘制都泄漏一个 shader 对象。
- 对齐 Vulkan：`vkDestroyShaderModule + delete`。PSO 经 `CreateGraphicsPipelineState` 已把所需
  bytecode 拷进管线状态，blob 可弃 ⇒ `delete` 安全。
- **顺手修 UB**：`RHIShader` 基类**无虚析构**，`delete` 基类指针本是未定义行为（Vulkan/WebGL2
  早就这么写、只是碰巧没炸）。给 `RHIShader` 加 `virtual ~RHIShader() = default;`——这是**正确的、
  向后兼容的**修法，让所有后端的 teardown 都良定义。
  - **踩坑（重要）**：加虚析构 = 给所有 shader 派生类型加 vptr = **改 ABI/布局**。MSBuild 的
    头文件依赖扫描**漏检**了两个 DX12 Utility TU（`BindlessTonemapPipeline.cpp` /
    `BindlessTextureBlitPipeline.cpp`，经 `RHI.h→RHIStruct.h` 间接依赖），增量构建没重编 ⇒
    潜在 vtable 布局不一致。**解法**：删掉整个 `ZRuntime.dir/{Debug,Release}` 中间目录 + lib，
    全量干净重建，逐一确认所有 `new DX12Shader/VulkanShader/...` 的 TU 都重编（obj 用嵌套
    `Function/Render/Interface/DX12/Utility/*.cpp.obj` 路径 + `.cpp.obj` 后缀，扁平搜索会漏）。
    **教训**：改基类布局（加/删虚函数、成员）后，别信增量构建，必须干净重建。

### 修复二：`PushEvent`/`PopEvent` 是热路径空桩 ⇒ DX12 无任何 GPU 调试标记
- 每个 pass（MainCameraPass/DirectionalLightPass/CombineUI/ColorGrading/Spine/ZSlate overlay…）
  在热路径调 Push/PopEvent；空桩 ⇒ DX12 完全没有 PIX 标记，GPU 调试/性能分析是瞎的。
- **踩坑（重要）**：`ID3D12GraphicsCommandList::BeginEvent` **不是**字符串标记 API——签名是
  `BeginEvent(UINT Metadata, const void* pData, UINT Size)`（时间戳事件）。直接传字符串名会
  C2660。**正确做法**：用 Windows SDK 自带的 `<pix.h>`（`PIXBeginEvent`/`PIXEndEvent`）——它是
  **header-only 内联包装**，内部发 `BeginEvent(PIX_EVENT_ANSI_VERSION, name, size)`，D3D12 运行库
  把该 metadata 常量识别为字符串标签（运行期解析、无需额外链接 DLL）。且能带 `PIX_COLOR(r,g,b)`
  颜色 metadata，忠实对齐 Vulkan `(name, color)` 签名。
- 与其它 recording PFN（`DrawIndexedInstanced`/`ResourceBarrier`）一致，操作
  `getCurrentCommandList()`（= `m_CommandLists[m_CurrentFrameIndex]`），忽略传入的逻辑
  `RHICommandBuffer*` 句柄。

### 验证（Windows Debug + Release 双配置端到端）
- ZRuntime.lib **Debug + Release 均干净全量重建**（exit 0，零 error）；所有 shader 派生类型 TU
  确认重编（新 vtable 布局一致）。
- 运行 `RHIFactoryRegistrySmokeTest`（含 DX12 后端 new/delete 路径）：**9/9 ALL CHECKS PASSED**
  ⇒ 新 `RHIShader` 布局 + DX12 后端生命周期 OK。

### 踩坑小结（本落点两条硬教训）
1. **改基类布局必须干净重建**：MSBuild 头扫描漏检间接依赖，增量构建会留下布局不一致的 stale obj。
2. **D3D12 `BeginEvent` ≠ 字符串标记**：那是时间戳事件 API；字符串作用域标记走 `<pix.h>` 的
   `PIXBeginEvent/PIXEndEvent`（header-only、运行期经 D3D12 运行库解析、可带颜色）。

### 改动文件清单
- `Runtime/Function/Render/Interface/RHIStruct.h`（`RHIShader` 加 `virtual ~RHIShader() = default;`）
- `Function/Render/Interface/DX12/DX12RHI.cpp`（`DestroyShaderModule` 实现 `delete`；
  `PushEvent`/`PopEvent` 用 `<pix.h>` 发真实 PIX 标记；新增 `#include <pix.h>`）
- `doc/PHASE1_FOUNDATION_PLAN.md`：本 §3.14；#12 标"热路径真 bug 已修复，其余后置"。

至此 #12 的**热路径真 bug 部分**完成：修掉 DX12 每次绘制泄漏 shader 的静默内存泄漏 + 补齐 GPU
调试标记。其余 "TODO" 经核实多为 DX12 不走 generic API 的 Vulkan-ism（空桩语义正确），完整
WebGL2 后端（19 处 `WEBGL2_TODO`）作为全后端级大工程后置。

---

## 4. 后续落点预案（简述，动手前再展开）

- ~~**#8 两阶段销毁**~~（✅ 已完成，见 §3.7）：在 `Object::~Object()` 前后真正调用
  `BeginDestroy()`（置 `OF_BeginDestroyed`）/ `FinishDestroy()`（置 `OF_FinishDestroyed`），
  让 `m_ObjectFlags` 从死代码变活。这是 #3/#5 生命周期模型的完整化。
- ~~**#6 PPtr 区分已销毁/未解析**~~（✅ 已完成，见 §3.8）：`PPtr.h:79` 的"从磁盘加载同 ID 对象"
  要区分"堆对象已销毁"与"尚未解析的磁盘对象"，前者应返回 null 并可报告，而不是静默 reify。
- ~~**#11 日志环形缓冲**~~（✅ 已完成，见 §3.9）：在 `LogSystem` 加一个 crash-safe 的最近 N 条
  环形缓冲，作为 `force_flush_all_logs()` 失败时的兜底。为 #2 POSIX 崩溃捕获铺日志后路。
- ~~**#2 POSIX 崩溃捕获**~~（已实现·待 POSIX 编译验证，见 §3.10）：`CrashHandler.h` 的 no-op
  换成 `sigaction(SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT, SA_SIGINFO|SA_ONSTACK)` + `sigaltstack`
  + `backtrace()` + `dladdr()`（刻意不用 `backtrace_symbols_fd`，bionic/musl 不保证存在）；
  两个 POSIX `main()` 已接线；`platform` 字段运行时派生（顺带关闭 #10）。**待 POSIX 构建跑通
  `PosixCrashCaptureSmokeTest` 后标 ✅**。
- ~~**#1+#4 RHI**~~（✅ 已完成，见 §3.11）：`RHIFactory.mm` 的硬编码 switch + 无限递归换成
  跨平台 `RegisterBackend` 注册表（各后端 `RHIBackendRegistrar` 自注册）；补 `RegisterRuntime.cpp`
  两处选择块的 Linux 分支（注册 VulkanRHI）+ `Runtime/CMakeLists.txt` 的 Linux 源文件过滤分支
  （剔除 DX12/Metal，修 Linux 编不过的先决缺口）。Windows 双配置端到端验证（10/10）；
  **Linux 运行期确认待真机构建**（与 #2 一并）。
- ~~**#7 引用计数强化 + 弱引用**~~（✅ 已完成，见 §3.12）：`AtomicRefCounter` 从"只有强计数、
  单一消费者"补成强/弱双计数（保持 `SerializedFile` 兼容）；新增 `RefCounted<T>`（CRTP 基类，
  第二个真实消费者）与 `WeakObjectPtr<T>`（基于 instanceID 的弱句柄，访问死对象合法返回 null、
  不报 UAF）。Windows 双配置端到端（RefCounted 36/36、WeakObjectPtr 21/21）。
- ~~**#9 VMA 头泄漏**~~（✅ 已修复，见 §3.13）：删 `RHI.h` 的 `#include <vma/vk_mem_alloc.h>`
  （本体已全用 `void*`，Vulkan 后端自行 include，消费者各有 guarded fallback）。Windows 双配置
  重建 + 注册表 smoke test 9/9 通过。
- **#9 胖接口拆分（后置·高风险）**：124 纯虚 + 11 默认实现的胖接口，按职责（命令录制 / 资源创建 /
  销毁 / 查询）拆成多个子接口。方向正确但触碰全部 4 个后端 + 每个调用点，需作为独立重构专项，
  不在阶段一"每步 Windows 可验证、风险可控"范围内。
- ~~**#12 DX12 热路径真 bug**~~（✅ 已修复，见 §3.14）：逐一核实 15 处 "TODO" 后，真正在 DX12
  热路径且是静默错/真 bug 的只有两处——①`DestroyShaderModule` 空桩导致每次绘制泄漏一个
  `DX12Shader`（~50 调用点），改为 `delete`（对齐 Vulkan）并顺手给无虚析构的 `RHIShader` 加
  `virtual ~RHIShader()`（修 UB）；②`PushEvent`/`PopEvent` 空桩导致 DX12 无 GPU 调试标记，改用
  `<pix.h>` 的 `PIXBeginEvent/PIXEndEvent` 发真实 PIX 标记（带颜色）。Windows 双配置**全量干净
  重建**（改基类布局必须干净重建，MSBuild 增量会漏检间接依赖）+ 注册表 smoke test 9/9 通过。
- **#12 其余 TODO / 完整 WebGL2 后端（后置）**：核实后多数是 DX12 不走 generic API 的 Vulkan-ism
  （`AllocateCommandBuffers`/`CreateCommandPool`/`CreateFence` 等——DX12 用自有内部对象，空桩语义
  正确；`AllocateDescriptorSets` 其实早已实现、:2400 是错位注释；`Invalidate/FlushMappedMemoryRanges`
  在 DX12 coherent heap 下本就该是 no-op）。完整 WebGL2 后端（19 处 `WEBGL2_TODO`）是全后端级大工程，后置。
