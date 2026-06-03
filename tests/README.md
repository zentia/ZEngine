# ZEngine Platform Tests

这个目录包含了ZEngine的所有平台检测测试。

## 📁 文件结构

```
tests/
├── CMakeLists.txt           # 测试CMake配置
├── test_simple.cpp          # 简化测试（已验证工作）
├── platform_example.cpp     # 基础示例
├── test_advanced.cpp        # 高级测试套件
└── README.md               # 本文件
```

## 🧪 测试文件说明

### `test_simple.cpp`
- **用途**: 简化的平台检测测试
- **特点**: 快速验证基本功能
- **状态**: ✅ 已验证工作

### `platform_example.cpp`
- **用途**: 基础平台检测示例
- **特点**: 展示基本用法
- **状态**: 🔄 待测试

### `test_advanced.cpp`
- **用途**: 高级测试套件
- **特点**: 全面的测试覆盖
- **状态**: 🔄 待测试

## 🚀 使用方法

### 构建测试
```bash
# 从项目根目录
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

### 运行测试
```bash
# 简化测试
.\build\tests\Debug\platform_test_simple.exe

# 基础测试
.\build\tests\Debug\platform_test.exe

# 高级测试
.\build\tests\Debug\platform_test_advanced.exe
```

### 使用测试脚本
```bash
# Windows
..\test_platform.bat

# Unix/Linux/macOS
../test_platform.sh

# 跨平台Python
python ../test_platform.py
```

## 📊 测试输出示例

```
=== ZEngine Platform Detection Test ===
Platform: Windows
Architecture: x64
Compiler: MSVC

=== Macro Tests ===
Z_PLATFORM_WINDOWS: defined
Z_PLATFORM_ANDROID: not defined
Z_ARCH_X64: defined
Z_COMPILER_MSVC: defined

=== Runtime Tests ===
Running on Windows!
64-bit x86 architecture
Using MSVC compiler

=== Test Complete ===
```

## 🔧 添加新测试

要添加新的平台测试：

1. 在`tests/`目录中创建新的`.cpp`文件
2. 在`tests/CMakeLists.txt`中添加新的可执行文件
3. 添加相应的测试配置
4. 更新测试脚本路径

## 📝 注意事项

- 所有测试文件都包含`engine/source/runtime/core/base/platform.h`
- 测试路径相对于项目根目录
- 确保在正确的构建配置下运行测试
