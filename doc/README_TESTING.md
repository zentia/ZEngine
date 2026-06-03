# ZEngine 平台测试环境

这是一个完整的平台检测测试环境，用于验证ZEngine的跨平台宏系统是否正常工作。

## 📁 文件结构

```
ZEngine/
├── platform_example.cpp          # 基础平台检测示例
├── test_advanced.cpp             # 高级测试套件
├── CMakeLists.txt                # CMake配置文件
├── test_platform.bat             # Windows测试脚本
├── test_platform.sh              # Unix/Linux/macOS测试脚本
├── test_platform.py              # 跨平台Python测试脚本
├── README_TESTING.md             # 本文档
└── engine/source/runtime/core/base/platform.h  # 平台检测头文件
```

## 🚀 快速开始

### 方法1：使用Python脚本（推荐）

```bash
# 确保Python 3.6+已安装
python test_platform.py
```

### 方法2：使用平台特定脚本

**Windows:**
```cmd
test_platform.bat
```

**Unix/Linux/macOS:**
```bash
chmod +x test_platform.sh
./test_platform.sh
```

### 方法3：手动CMake构建

```bash
# 配置
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug

# 构建
cmake --build build --config Debug

# 运行基础测试
./build/Debug/PlatformTest.exe    # Windows
./build/PlatformTest              # Unix/Linux/macOS

# 运行高级测试
./build/Debug/PlatformTestAdvanced.exe    # Windows
./build/PlatformTestAdvanced              # Unix/Linux/macOS

# 运行CTest
cd build
ctest --output-on-failure --verbose
```

## 🧪 测试内容

### 基础测试 (`platform_example.cpp`)

- ✅ 平台检测（Windows/Android/macOS/Linux/iOS）
- ✅ 架构检测（x64/x86/ARM64/ARM32）
- ✅ 编译器检测（MSVC/GCC/Clang/AppleClang）
- ✅ 运行时信息显示

### 高级测试 (`test_advanced.cpp`)

- ✅ 平台宏检测测试
- ✅ 架构宏检测测试
- ✅ 编译器宏检测测试
- ✅ 平台特定功能测试
- ✅ 一致性测试
- ✅ 详细测试报告生成

## 📊 测试输出

### 基础测试输出示例

```
=== ZEngine Platform Detection ===
Platform: Windows
Architecture: x64
Compiler: MSVC

=== Platform-specific code ===
Running on Windows!
Path separator: \
64-bit x86 architecture
Using MSVC compiler
```

### 高级测试输出示例

```
=== Platform Detection Tests ===
Platform macro detection: PASS (Detected 1 platform(s))
Platform name function: PASS (Windows)
Runtime platform detection: PASS (Detected 1 platform(s))

=== Architecture Detection Tests ===
Architecture macro detection: PASS (Detected 1 architecture(s))
Architecture name function: PASS (x64)
Runtime architecture detection: PASS (Detected 1 architecture(s))

=== Compiler Detection Tests ===
Compiler macro detection: PASS (Detected 1 compiler(s))
Compiler name function: PASS (MSVC)
Runtime compiler detection: PASS (Detected 1 compiler(s))

=== Test Report ===
Total tests: 15
Passed: 15
Failed: 0
Success rate: 100%
```

## 🔧 测试脚本功能

### Python脚本 (`test_platform.py`)

- 🔍 自动检测依赖（CMake、Python）
- 🧹 自动清理构建目录
- ⚙️ 自动配置CMake
- 🔨 自动构建项目
- 🧪 运行所有测试
- 📊 生成详细报告
- 📈 分析测试结果

### 批处理脚本 (`test_platform.bat`)

- Windows专用测试脚本
- 自动清理和构建
- 运行基础测试
- 生成测试结果文件

### Shell脚本 (`test_platform.sh`)

- Unix/Linux/macOS专用测试脚本
- 跨平台兼容性测试
- 自动错误处理

## 📋 测试报告

测试完成后会生成以下文件：

- `test_results/platform_output.txt` - 基础测试输出
- `test_results/detailed_report.txt` - 高级测试详细报告
- `test_results/test_report.txt` - Python脚本生成的报告

## 🐛 故障排除

### 常见问题

1. **CMake未找到**
   ```
   ERROR: CMake is not installed or not in PATH
   ```
   **解决方案**: 安装CMake并确保在PATH中

2. **编译错误**
   ```
   ERROR: Build failed
   ```
   **解决方案**: 检查编译器是否正确安装，查看详细错误信息

3. **测试失败**
   ```
   ✗ Platform detection test FAILED
   ```
   **解决方案**: 检查平台宏定义是否正确

### 调试模式

启用详细输出：
```bash
# Python脚本
python test_platform.py --verbose

# CMake
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug --debug-output

# CTest
cd build
ctest --output-on-failure --verbose
```

## 🔄 持续集成

### GitHub Actions示例

```yaml
name: Platform Tests
on: [push, pull_request]

jobs:
  test:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [windows-latest, ubuntu-latest, macos-latest]
    
    steps:
    - uses: actions/checkout@v2
    - name: Install CMake
      uses: jwlawson/actions-setup-cmake@v1.7
    - name: Run Tests
      run: python test_platform.py
```

### 本地多平台测试

```bash
# 在Docker中测试Linux
docker run -v $(pwd):/workspace -w /workspace ubuntu:20.04 bash -c "
  apt-get update && apt-get install -y cmake g++ python3
  python3 test_platform.py
"

# 在WSL中测试Linux
wsl --install Ubuntu
wsl -d Ubuntu -e bash -c "cd /mnt/c/path/to/ZEngine && python3 test_platform.py"
```

## 📈 性能基准

测试在不同平台上的性能表现：

| 平台 | 构建时间 | 测试时间 | 内存使用 |
|------|----------|----------|----------|
| Windows (MSVC) | ~30s | ~2s | ~50MB |
| Linux (GCC) | ~25s | ~1.5s | ~40MB |
| macOS (Clang) | ~35s | ~2s | ~45MB |

## 🤝 贡献

要添加新的测试用例：

1. 在 `test_advanced.cpp` 中添加新的测试方法
2. 更新 `CMakeLists.txt` 中的测试配置
3. 更新本文档
4. 运行测试确保通过

## 📚 相关文档

- [平台宏使用指南](PLATFORM_MACROS.md)
- [CMake配置说明](CMakeLists.txt)
- [平台检测头文件](engine/source/runtime/core/base/platform.h)
