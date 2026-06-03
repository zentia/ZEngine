# ZEngine 构建系统文档

## 概述

ZEngine 使用 CMake 作为底层构建系统，并提供了一个统一的 Python 构建工具层 (`zbuild.py`) 来简化跨平台构建流程。这个设计类似于 Unreal Engine 的 UAT (Unreal Automation Tool)，但更轻量级。

## 快速开始

### 使用统一构建工具 (推荐)

```bash
# 检查构建环境
python zbuild.py check

# 配置项目
python zbuild.py configure --config debug

# 构建项目
python zbuild.py build --config debug

# 构建特定目标
python zbuild.py build --target ZEditor

# 清理构建目录
python zbuild.py clean

# 运行测试
python zbuild.py test

# 安装构建产物
python zbuild.py install --config release
```

### 使用平台特定脚本

**Windows:**
```cmd
build_windows.bat debug
build_windows.bat release --target ZEditor --jobs 8
```

**Linux:**
```bash
./build_linux.sh debug
./build_linux.sh release ninja --target ZEditor --jobs 8
```

**macOS:**
```bash
./build_macos.sh debug
./build_macos.sh release xcode --target ZEditor
```

## 构建系统架构

```
┌─────────────────────────────────────┐
│  统一构建工具层 (zbuild.py)         │
│  - 跨平台接口                       │
│  - 参数解析                         │
│  - 错误处理                         │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│  CMake Presets                      │
│  - Windows/Linux/macOS 预设         │
│  - 不同配置 (Debug/Release)         │
│  - 不同生成器 (VS/Ninja/Make/Xcode) │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│  CMake 构建系统                      │
│  - 项目配置                         │
│  - 依赖管理                         │
│  - 编译规则                         │
└─────────────────────────────────────┘
```

## CMake Presets

CMake Presets 提供了一组预定义的配置，简化了构建过程。

### Windows Presets

- `windows_visual_studio` - 使用 Visual Studio 2022 生成器
- `windows_ninja` - 使用 Ninja 生成器
- `windows_vscode_debug` - VSCode 调试配置
- `windows_vscode_release` - VSCode 发布配置

### Linux Presets

- `linux_make_debug` - 使用 Make 的调试配置
- `linux_make_release` - 使用 Make 的发布配置
- `linux_ninja_debug` - 使用 Ninja 的调试配置
- `linux_ninja_release` - 使用 Ninja 的发布配置

### macOS Presets

- `macos_xcode_debug` - 使用 Xcode 的调试配置
- `macos_xcode_release` - 使用 Xcode 的发布配置
- `macos_ninja_debug` - 使用 Ninja 的调试配置
- `macos_ninja_release` - 使用 Ninja 的发布配置

### 列出所有预设

```bash
python zbuild.py list-presets
```

## 构建配置

### Debug 配置

- 包含调试符号
- 优化关闭
- 断言启用
- 适合开发和调试

```bash
python zbuild.py configure --config debug
python zbuild.py build --config debug
```

### Release 配置

- 优化开启
- 调试符号移除（或最小化）
- 断言可能禁用
- 适合最终发布

```bash
python zbuild.py configure --config release
python zbuild.py build --config release
```

### RelWithDebInfo 配置

- 优化开启
- 保留调试符号
- 适合性能分析和调试发布版本

```bash
python zbuild.py configure --config relwithdebinfo
python zbuild.py build --config relwithdebinfo
```

## 性能优化

### 构建缓存 (ccache)

CMake 配置会自动检测并使用 `ccache`（如果可用）来加速重复构建。

**启用 ccache (Linux/macOS):**
```bash
# 安装 ccache
sudo apt install ccache  # Ubuntu/Debian
brew install ccache      # macOS

# CMake 会自动检测并使用
python zbuild.py configure --config debug
```

**Windows:**
Windows 上可以使用 Visual Studio 的增量编译，或者安装 ccache 的 Windows 版本。

### Unity Builds (可选)

Unity Builds 可以将多个源文件合并编译，减少编译时间。注意：这可能会增加增量编译时间。

在 CMake 配置时启用：
```bash
cmake -S . -B build -DUSE_UNITY_BUILD=ON
```

### 并行构建

构建系统会自动检测 CPU 核心数并启用并行构建：

```bash
# 自动使用所有可用核心
python zbuild.py build

# 手动指定并行数
python zbuild.py build --jobs 8
```

## 高级用法

### 使用特定预设

```bash
# 使用特定预设配置
python zbuild.py configure --preset windows_visual_studio
python zbuild.py build --preset windows_editor
```

### 传递额外 CMake 参数

```bash
python zbuild.py configure --config debug --extra-args -DUSE_UNITY_BUILD=ON
```

### 构建特定目标

```bash
# 只构建编辑器
python zbuild.py build --target ZEditor

# 只构建运行时
python zbuild.py build --target ZRuntime

# 只构建测试
python zbuild.py build --target PlatformTest
```

### 清理构建

```bash
# 清理构建目录
python zbuild.py clean

# 然后重新配置和构建
python zbuild.py configure --config debug
python zbuild.py build
```

## 构建选项

CMake 支持以下构建选项（通过 `-D` 传递）：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `USE_CCACHE` | ON | 使用 ccache 加速构建 |
| `USE_UNITY_BUILD` | OFF | 启用 Unity Builds |
| `CMAKE_EXPORT_COMPILE_COMMANDS` | ON | 导出编译命令（用于 IDE） |
| `ENABLE_PHYSICS_DEBUG_RENDERER` | OFF | 启用物理调试渲染器（仅 Windows） |

示例：
```bash
python zbuild.py configure --config debug --extra-args -DUSE_UNITY_BUILD=ON
```

## 故障排除

### 常见问题

**1. CMake 未找到**
```bash
# 检查 CMake 版本
cmake --version

# 如果未安装，请安装 CMake 3.19 或更高版本
```

**2. Python 未找到**
```bash
# 检查 Python 版本
python --version  # 或 python3 --version

# 确保 Python 3.6+ 已安装并在 PATH 中
```

**3. 编译器未找到**

**Windows:**
- 确保已安装 Visual Studio 2022 或更高版本
- 确保安装了 C++ 工作负载

**Linux:**
- 安装 Clang: `sudo apt install clang`
- 或使用 GCC: `sudo apt install g++`

**macOS:**
- 安装 Xcode Command Line Tools: `xcode-select --install`

**4. 构建失败**

检查构建日志：
```bash
# 查看详细构建输出
python zbuild.py build --config debug 2>&1 | tee build.log
```

**5. 预设未找到**

列出可用预设：
```bash
python zbuild.py list-presets
```

### 调试构建

**启用详细输出:**
```bash
# CMake 配置时显示详细信息
cmake -S . -B build --preset windows_visual_studio --verbose

# 构建时显示详细信息
cmake --build build --verbose
```

## 与 IDE 集成

### Visual Studio

1. 打开项目根目录
2. Visual Studio 会自动检测 `CMakePresets.json`
3. 选择预设并开始构建

### Visual Studio Code

1. 安装 CMake Tools 扩展
2. 打开项目根目录
3. 使用命令面板选择预设：`CMake: Select a Kit`

### CLion

1. 打开项目根目录
2. CLion 会自动检测 CMake 配置
3. 在设置中选择 CMake 预设

## CI/CD 集成

### GitHub Actions 示例

```yaml
name: Build ZEngine

on: [push, pull_request]

jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - uses: actions/setup-python@v4
        with:
          python-version: '3.x'
      - name: Configure
        run: python zbuild.py configure --preset windows_ci
      - name: Build
        run: python zbuild.py build --preset windows_ci
```

## 最佳实践

1. **使用预设**: 优先使用 CMake Presets 而不是手动配置
2. **启用缓存**: 安装并使用 ccache 加速构建
3. **并行构建**: 使用 `--jobs` 参数充分利用多核 CPU
4. **清理构建**: 遇到问题时先清理再重新构建
5. **版本控制**: 不要提交 `build/` 目录到版本控制

## 迁移指南

### 从旧脚本迁移

**之前:**
```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

**现在:**
```bash
python zbuild.py configure --preset windows_visual_studio
python zbuild.py build --preset windows_editor
```

或者更简单：
```bash
python zbuild.py configure --config debug
python zbuild.py build --config debug
```

## 参考资源

- [CMake 官方文档](https://cmake.org/documentation/)
- [CMake Presets 文档](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
- [ccache 文档](https://ccache.dev/documentation.html)

## 贡献

如果发现构建系统的问题或有改进建议，请提交 Issue 或 Pull Request。

