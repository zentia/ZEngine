# Resharper 配置指南

本文档说明如何配置 Resharper 以使用项目中的 `.clang-tidy` 和 `.clang-format` 配置文件。

## 自动配置

项目已包含以下配置文件：
- `.editorconfig` - EditorConfig 配置，Resharper 会自动读取
- `ZEngine.DotSettings` - Resharper 项目设置文件

## 手动配置步骤

如果自动配置不生效，请按照以下步骤手动配置：

### 1. 启用 Clang-Format

1. 打开 Visual Studio
2. 进入 `ReSharper` → `Options` (或 `Extensions` → `ReSharper` → `Options`)
3. 导航至 `Code Editing` → `C++` → `Formatting Style` → `General`
4. 将 `C++ Formatting Engine` 设置为 `Clang-Format`
5. 确保 `Use Clang-Format style for code formatting` 已勾选
6. 点击 `Apply` 和 `Save`

### 2. 启用 Clang-Tidy

1. 在 ReSharper 选项中，导航至 `Code Editing` → `C++` → `Clang-Tidy`
2. 确保 `Enable Clang-Tidy support` 已勾选
3. Resharper 会自动查找项目根目录下的 `.clang-tidy` 文件
4. 点击 `Apply` 和 `Save`

### 3. 验证配置

1. 打开任意 C++ 源文件（`.cpp` 或 `.h`）
2. 使用 `Ctrl+K, Ctrl+D` 格式化代码，应该使用 `.clang-format` 的规则
3. 查看代码检查提示，应该显示 `.clang-tidy` 的检查结果

## 配置文件位置

- `.clang-format` - 位于项目根目录（Resharper 会自动查找）
- `.clang-tidy` - 位于项目根目录（Resharper 会自动查找）
- `.editorconfig` - 位于项目根目录（Resharper 会自动查找）
- `ZEngine.DotSettings` - 需要放在解决方案文件（`.sln`）旁边

### 重要提示

`.DotSettings` 文件需要与 Visual Studio 解决方案文件（`.sln`）放在同一目录。由于项目的解决方案文件在 `build/ZEngine.sln`（由 CMake 生成），您需要：

**选项 1：手动复制（推荐）**
1. 将 `ZEngine.DotSettings` 复制到 `build/` 目录
2. 或者创建符号链接：`mklink build\ZEngine.DotSettings ZEngine.DotSettings`（Windows）

**选项 2：在 Visual Studio 中保存设置**
1. 打开 `build/ZEngine.sln`
2. 在 ReSharper 选项中配置 clang-format 和 clang-tidy（见上方手动配置步骤）
3. ReSharper 会自动创建 `build/ZEngine.DotSettings` 文件
4. 可以将该文件复制回项目根目录并提交到版本控制

## 注意事项

1. **配置文件优先级**：`.clang-format` 文件中的设置会覆盖 Resharper 和 Visual Studio 选项中的设置，以及 EditorConfig 样式。

2. **自动检测**：Resharper 会自动在以下位置查找配置文件：
   - 当前文件所在目录
   - 父目录（向上递归到项目根目录）
   - 项目根目录

3. **重新加载**：如果修改了 `.clang-format` 或 `.clang-tidy` 文件，可能需要：
   - 重新打开文件
   - 或重启 Visual Studio

## 故障排除

如果 Resharper 仍然没有使用项目的 clang 配置：

1. **检查文件位置**：确保 `.clang-format` 和 `.clang-format` 在项目根目录
2. **检查文件格式**：确保配置文件格式正确（YAML 格式）
3. **重新加载项目**：关闭并重新打开 Visual Studio 解决方案
4. **检查 Resharper 版本**：确保使用支持 Clang-Format/Clang-Tidy 的 Resharper 版本（2021.3+）
5. **查看 Resharper 日志**：在 `Help` → `ReSharper` → `Show ReSharper Log` 中查看是否有错误信息

## 相关文档

- [ReSharper Clang-Format 文档](https://www.jetbrains.com/help/resharper/Using_Clang_Format.html)
- [ReSharper Clang-Tidy 文档](https://www.jetbrains.com/help/resharper/Using_Clang_Tidy.html)
- 项目代码风格指南：`doc/CODING_STYLE.md`

