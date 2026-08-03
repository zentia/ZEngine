# ZRuntimeShared iOS 构建

本目录提供在 macOS/Xcode 上构建 iOS 版 `ZRuntimeShared` 的脚本。

## 前置条件

- macOS + Xcode，且 `xcodebuild` 可用
- `cmake` 可用
- 可用的 `VULKAN_SDK`，至少需要 `bin/glslangValidator`
  - 脚本会优先使用环境变量 `VULKAN_SDK`
  - 若未设置，会尝试 `$HOME/VulkanSDK/local` 和 `build/vulkan-sdk`
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

在当前本机测试中，`Release` 产物约为：

- 初始未 strip：约 `6.7 MB`
- 仅 strip 后：约 `5.6 MB`
- 导出列表 + dead strip + strip 后：约 `1.0 MB`
- 导出符号数量：约从 `8322` 降到 `104`

后续仍有进一步优化空间，主要方向：

1. 减少 `ZRuntimeShared` 静态合入的模块和第三方库。
2. 对 iOS 单独裁剪未使用的 Runtime 子系统。
3. 为 iOS Release 增加更激进的符号隐藏和 dead strip 配置。
4. 审查 `PuertsCore`、`PapiLua`、`Jolt`、`mimalloc` 等依赖是否需要全部进入动态库。
