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
bin/ios/PLATFORM/CONFIG/libZRuntimeShared.dylib.dSYM
```

例如：

```text
bin/ios/OS64/Release/libZRuntimeShared.dylib
bin/ios/OS64/Release/libZRuntimeShared.dylib.dSYM
```

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

## 实测（2026-10-30，`--config Release`，arm64）

- 产物：`bin/ios/OS64/Release/libZRuntimeShared.dylib` 约 **8.1 MB**
- 导出符号：**104** 个（导出列表生效）
- `otool -l`：有 `LC_DYLD_INFO_ONLY`、无 `LC_DYLD_CHAINED_FIXUPS` → `-no_fixup_chains` 生效
- 同时产出 `ZRuntimeShared.framework`（Headers / Modules / Info.plist / ad-hoc 签名）

⚠️ **脚本不再做 post-build strip**：iOS 16+ 的 dyld 要求 `LC_DYLD_INFO_ONLY`，
`strip -S` 可能破坏它，导致加载时报 `missing LC_DYLD_INFO load command`。死代码裁剪
已由链接期 `-dead_strip` 处理，所以 post-build strip 既多余又有害。
因此下面这些"strip 后"的数字是**历史值**，当前 Release 产物是未 strip 的 8.1 MB：

- 历史：初始未 strip 约 `6.7 MB` / 仅 strip 约 `5.6 MB` / 导出列表 + dead strip + strip 约 `1.0 MB`
- 导出符号数量：约从 `8322` 降到 `104`（这条仍然成立，实测 104）

后续仍有进一步优化空间，主要方向：

1. 减少 `ZRuntimeShared` 静态合入的模块和第三方库。
2. 对 iOS 单独裁剪未使用的 Runtime 子系统。
3. 为 iOS Release 增加更激进的符号隐藏和 dead strip 配置。
4. 审查 `PuertsCore`、`PapiLua`、`Jolt`、`mimalloc` 等依赖是否需要全部进入动态库。
