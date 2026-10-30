# ZRuntimeShared iOS 构建

本目录提供在 macOS/Xcode 上构建 iOS 版 `ZRuntimeShared` 的脚本。

## 前置条件

- macOS + Xcode，且 `xcodebuild` 可用
- `cmake` 可用
- 可用的 `VULKAN_SDK`，至少需要 `bin/glslangValidator`
  - 脚本会优先使用环境变量 `VULKAN_SDK`
  - 若未设置，会尝试 `$HOME/VulkanSDK/local` 和 `build/vulkan-sdk`
  - **不必装完整 Vulkan SDK**：iOS 走 Metal，`find_package(Vulkan REQUIRED)` 在
    `engine/CMakeLists.txt` 的 `if(ZENGINE_USE_VULKAN)` 里，iOS 不会走到。所以只需要
    `bin/glslangValidator` 可用即可（2026-10-30 实测）。最小做法：

    ```bash
    mkdir -p build/vulkan-sdk/bin
    ln -s "$(command -v glslangValidator)" build/vulkan-sdk/bin/glslangValidator
    ```

    脚本的第二个候选路径正好是 `build/vulkan-sdk`，放好就不用传 `--vulkan-sdk`。
- iOS toolchain 默认使用：
  - `engine/3rdparty/bqlog/build/lib/ios/ios.toolchain.cmake`

> 说明：iOS Runtime 渲染后端使用 Metal；`VULKAN_SDK` 当前主要用于工程内 shader 生成链路。

## 用法

```bash
scripts/build_lib/ios/macos_build_zruntime_shared.sh \
  [--config Release|Debug|MinSizeRel|RelWithDebInfo] \
  [--all-configs] \
  [--platform OS64] \
  [--deployment-target 16.0] \
  [--jobs N] \
  [--vulkan-sdk PATH] \
  [--toolchain PATH] \
  [--papi quickjs|v8] \
  [--slim] [--no-slim] \
  [--clean] \
  [--strip] \
  [--no-strip] \
  [--no-export-list]
```

## 示例

构建默认 `Release`：

```bash
scripts/build_lib/ios/macos_build_zruntime_shared.sh
```

构建 `Debug`：

```bash
scripts/build_lib/ios/macos_build_zruntime_shared.sh --config Debug --jobs 8
```

构建全部配置：

```bash
scripts/build_lib/ios/macos_build_zruntime_shared.sh --all-configs --jobs 8
```

指定 Vulkan SDK、iOS 平台和部署版本：

```bash
scripts/build_lib/ios/macos_build_zruntime_shared.sh \
  --vulkan-sdk "$HOME/VulkanSDK/local" \
  --platform OS64 \
  --deployment-target 16.0 \
  --config Release
```

清理后重新构建：

```bash
scripts/build_lib/ios/macos_build_zruntime_shared.sh --clean --config Release
```

显式开启或关闭 strip：

```bash
# 强制 strip，包括 RelWithDebInfo/Debug 等配置
scripts/build_lib/ios/macos_build_zruntime_shared.sh --config Release --strip

# 不 strip，保留 dylib 符号
scripts/build_lib/ios/macos_build_zruntime_shared.sh --config Release --no-strip
```

## 输出

```text
bin/ios/PLATFORM/CONFIG/libZRuntimeShared.dylib
bin/ios/PLATFORM/CONFIG/ZRuntimeShared.framework
```

例如：

```text
bin/ios/OS64/Release/libZRuntimeShared.dylib
bin/ios/OS64/Release/ZRuntimeShared.framework
```

⚠️ **Release 实测不产出 dSYM**（2026-10-30 确认：Xcode 生成器下
`DEBUG_INFORMATION_FORMAT = dwarf-with-dsym` 已设置，但 `CMAKE_CXX_FLAGS_RELEASE`
是 `-O3 -DNDEBUG`、没有 `-g`，构建日志里也没有 `dsymutil`）。需要符号就自己跑
`xcrun dsymutil`，或者用带 `-g` 的配置。

## 体积优化

脚本默认启用两类安全优化：

1. 链接阶段启用 `-dead_strip` / `-dead_strip_dylibs`。
2. 自动生成 `exported_symbols_list`，仅导出 `EXPORT_RUNTIME` 和 `ZENGINE_API_EXPORT` 声明的 C API；如需完整符号导出，可使用 `--no-export-list`。
3. 对 `ZRuntimeShared` 目标启用导出列表链接选项，减少 `__LINKEDIT` 并让 dead strip 更有效。
4. 禁用 chained fixups（`-no_fixup_chains`）：
   - iOS 16+ 的 ld64 链接器默认使用 chained fixups 格式（`LC_DYLD_CHAINED_FIXUPS`
     + `LC_DYLD_EXPORTS_TRIE`）替代旧式 `LC_DYLD_INFO_ONLY`
   - 但 UnityFramework 内嵌的 dyld 不认识 chained fixups，加载时直接拒绝：
     `missing LC_DYLD_INFO load command`
   - 修复：`target_link_options` 添加 `LINKER:-no_fixup_chains`（CMakeLists.txt），
     同时在构建脚本 `cmake` 命令行追加 `-DCMAKE_XCODE_ATTRIBUTE_LD_NO_FIXUP_CHAINS=YES`
   - 验证：`xcrun otool -l ZRuntimeShared.framework/ZRuntimeShared | grep LC_DYLD_INFO`
     应输出 `LC_DYLD_INFO_ONLY`
5. **iOS 也走移动端体积优化**（2026-10-30 修）：`ZENGINE_MOBILE_SIZE_OPTIMIZE`
   对 iOS 默认就是 ON，但那段 flag 原先只判 `ANDROID OR OHOS`，所以 iOS Release 一直
   停在 CMake 默认的 `-O3`，只有 Android 拿到 `-Oz`。现在 iOS 也会加
   `-Oz` + `-ffunction-sections` + `-fdata-sections`（Apple 侧的 section GC 由
   `-dead_strip` 负责，GNU 的 `--gc-sections` 仍然只在非 Apple 上用）。
   - `-fvisibility=hidden` **故意不加在 Apple 上**：共享库是 `-flat_namespace` +
     `-undefined dynamic_lookup` 链的，libc++ 的 VTT/vtable 要在运行时绑到宿主
     （UnityFramework）里那份；编译期隐藏会让它们变成本地副本，跨边界的 type identity 就断了。
6. **链接期 `-x` 去掉局部符号**（`ZENGINE_IOS_SHARED_STRIP_LOCAL_SYMBOLS`，移动端默认 ON）：
   `-exported_symbols_list` 之后剩下约 2 万个 LOCAL 符号，纯符号表开销，实测吃掉
   `__LINKEDIT` 里的 ~1.3 MB。
   - 这是 **ld64 的链接期 `-x`，不是 post-build `strip -S`**：`strip` 会重写已链接好的
     Mach-O，可能破坏 iOS 16+ dyld 需要的 `LC_DYLD_INFO`（见最后的 ⚠️）。
   - `Debug` / `RelWithDebInfo` 不加，否则 `dsymutil` 拿不到局部符号、dSYM 会退化。

## 体积归因怎么做（别猜，用链接 map）

`-dead_strip` + `-exported_symbols_list` 之下，**"把 .a 文件大小加起来"是错的**——只有
链接器实际保留下来的字节才算。唯一诚实的办法是生成 map：

```bash
cmake -S . -B build/lib/ios/OS64 -DZENGINE_IOS_LINK_MAP_FILE=/tmp/zrs.map
cmake --build build/lib/ios/OS64 --config Release --target ZRuntimeShared
```

map 里 `# Object files:` 给 `[编号] 路径`，`# Symbols:` 给 `地址 大小 [编号] 名字`；
按编号聚合成路径桶即可得到每个归档 / 目标文件的活字节。

## 实测（2026-10-30，`--config Release`，arm64）

| 版本 | dylib 大小 | `__TEXT` | `__LINKEDIT` | 符号数 | 导出数 |
| --- | --- | --- | --- | --- | --- |
| 2026-08-10 线上版（r757786） | 2.69 MB | 1.83 MB | 0.59 MB | 7366 | 104 |
| 2026-10-30 修之前 | **8.11 MB** | 5.09 MB | 1.92 MB | 20970 | 104 |
| 2026-10-30 构建配置修完 | **4.42 MB** | 3.90 MB | 0.22 MB | 4688 | 104 |
| 2026-10-30 再开 `--slim` | **2.58 MB** | — | — | — | 104 |

framework（含 ad-hoc 签名）：修完构建配置是 **4.45 MB**，`--slim` 是 **2.61 MB**
—— 已经比 2026-08 线上那份更小。

- 导出符号：**104** 个（导出列表生效）
- `otool -l`：有 `LC_DYLD_INFO_ONLY`、无 `LC_DYLD_CHAINED_FIXUPS` → `-no_fixup_chains` 生效
- `LC_BUILD_VERSION` platform = **2**（iOS）
- `nm -m | grep -c "dynamically looked up"` = **0**
- `--slim` 之后同样全绿：导出仍 104、`dynamically looked up` 仍 0，
  `nm -a | grep -ci "JPH\|spine\|spirv_cross"` = 0
- Release iOS 模拟器仍能启动：`scripts/verify_ios_boot.sh` → PASS
- 宿主回归不受影响：`scripts/verify_prefab_native.sh all` → PASS（472/0）

⚠️ **脚本不再做 post-build strip**：iOS 16+ 的 dyld 要求 `LC_DYLD_INFO_ONLY`，
`strip -S` 可能破坏它，导致加载时报 `missing LC_DYLD_INFO load command`。死代码裁剪
已由链接期 `-dead_strip` 处理，所以 post-build strip 既多余又有害。
因此下面这些"strip 后"的数字是**历史值**：

- 历史：初始未 strip 约 `6.7 MB` / 仅 strip 约 `5.6 MB` / 导出列表 + dead strip + strip 约 `1.0 MB`
- 导出符号数量：约从 `8322` 降到 `104`（这条仍然成立，实测 104）

### 那 5.5 MB 到底是从哪来的

链接 map 的归因（活字节）：

| 模块 | 修之前 | 修之后 |
| --- | --- | --- |
| 引擎自身对象 | 2.31 MB | 1.72 MB |
| spirv-cross（msl+glsl+core） | 1.90 MB | 1.17 MB |
| PapiQuickjs | 0.76 MB | 0.44 MB |
| Jolt | 0.21 MB | 0.18 MB |
| GPUSpine | 0.35 MB | 0.17 MB |
| mimalloc | 0.10 MB | 0.08 MB |
| **PuertsCore** | **0.01 MB** | **0.01 MB** |

结论和当初的猜测不一样，按数据记三条：

1. **裁 `PuertsCore` 减不了体积**：它只有 0.01 MB。之前把它列为主要嫌疑是错的。
2. 旧的 2.69 MB 是 **2026-08-10 的引擎版本**编出来的（SVN r757786）。对比符号：
   `spirv_cross` 旧 0 / 新 4866，`Spine` 旧 0 / 新 589，`MetalRHI` 旧 0 / 新 178。
   所以"回归"里相当一部分其实是这两个半月新长出来的引擎能力，不是构建开关退化。
3. 能安全拿回来的 3.69 MB 全部来自构建配置（`-Oz` 拿回 2.0 MB `__TEXT`、
   链接期 `-x` 拿回 1.7 MB `__LINKEDIT`），一行业务代码都没动。

### `--slim`：裁掉 SPIRV-Cross / GPUSpine / Jolt（−1.84 MB）

```bash
scripts/build_lib/ios/macos_build_zruntime_shared.sh --slim --papi quickjs
```

即 CMake 的 `ZENGINE_IOS_SHARED_SLIM_MODULES`（默认 **OFF**）。它是 opt-in 不是因为它有
风险没验，而是因为它改的是**这个 dylib 是什么**，不只是"怎么编"：

- 它**只作用于 `ZRuntimeShared`**。静态库 `ZRuntime`（iOS 播放器、Editor、宿主工具）一行
  没变 —— 所以 iOS 播放器仍然有 Metal 后端、Spine 和物理。
- 前提：**104 个导出符号里没有任何渲染 / Spine / 物理入口**（只有 MemoryProfiler 的 10 个
  函数 + pesapi）。宿主拿不到被裁掉的那些能力。
- 每裁一刀都靠 `nm -m | grep -c "dynamically looked up"` == **0** 兜底。这条断言不是"防漏链
  库"，它是**防"裁过头"**：只摘库不摘引用它的 TU，符号会被 `-undefined dynamic_lookup`
  绑成 0，第一次调用就是 `EXC_BAD_ACCESS at 0x0`。

三刀分别怎么落的：

| 裁掉 | 做法 | 大小 |
| --- | --- | --- |
| SPIRV-Cross | `MetalRHI.mm` 是唯一引用者（13 个符号），`CreateShaderModule` 在 `ZENGINE_SHARED_NO_SPIRV_CROSS` 下编译期返回 nullptr 并打 WARNING | −1.29 MB |
| GPUSpine | `SpineRenderer.cpp` / `SpineGpuRenderer.cpp` / `RuntimeConsoleCommands.cpp` 移出 shared 源列表，**外加** `SpineRenderer.gen.cpp`（反射 TU 才是真正调那些成员函数的地方，`ZHeaderTool` 因此多了 `<Target>_ZHT_EXCLUDE_BASENAMES` 钩子）；`WorldManager` / `RenderSystem` / `RenderScene` / `MainCameraRp1Pass` 共 5 处调用点加守卫 | −0.28 MB |
| Jolt | `PhysicsScene.cpp` / `PhysicsCharacter.cpp` / `Jolt/Utils.cpp` 换成 `PhysicsSceneNull.cpp`（约 20 个方法全部"无此刚体"）；`PhysicsScene.h` 加 `ZENGINE_SHARED_NO_PHYSICS` 接缝，去掉只有 Jolt 才需要的成员 | −0.27 MB |

⚠️ **Jolt 仍然留在链接命令行上**：它是 `PUBLIC`，同时提供 Jolt 的 **include 目录**，而没被
排除的 `PhysicsManager.cpp` 还要用 `Jolt/Utils.h` 里的 `toVec3/toQuat` 内联助手。因为已经
没有任何 TU 引用 JPH 符号，ld64 从归档里取 0 个成员 —— 链接 map 确认 `libJolt.a` 贡献
**0 字节**。想改回"直接不链"的话，必须先把 include 目录补回去，否则 `PhysicsManager.cpp`
会报 `'Jolt/Jolt.h' file not found`。

⚠️ **`PhysicsSceneNull.cpp` 是整体 `#if` 包起来的，不是 `#error`**：`Runtime/CMakeLists.txt`
用 `GLOB_RECURSE *.cpp`，它会进**每一个**目标（宿主 / Editor / web / 静态库）的源列表，
非 slim 构建里必须编译成空 TU。

剩下的构成（slim 后，链接 map 活字节）：

| 模块 | 大小 |
| --- | --- |
| 引擎自身对象 | 1.61 MB |
| PapiQuickjs | 0.44 MB |
| mimalloc | 0.08 MB |
| zlib | 0.02 MB |
| PuertsCore | 0.01 MB |
| BqLog | ~0.08 MB |

**PapiQuickjs 不能裁**：`CreateQjsPapiEnvRef` / `GetQjsFFIApi` / `GetQjsPapiVersion` 在 104
个导出符号里，是公开 ABI。**PuertsCore 只有 0.01 MB**，裁它没有意义。
