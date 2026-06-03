# ZEngine 编译速度优化指南

## 🚀 快速优化（已自动启用）

### 1. 并行编译（已优化 ✅）

**Windows:**
- MSVC 编译器已启用 `/MP` 标志（并行编译单个文件）
- CMake 构建现在会自动使用所有 CPU 核心并行构建多个目标
- 默认行为：自动检测 CPU 核心数并使用

**使用方法：**
```cmd
REM 自动使用所有核心（推荐）
build_windows.bat debug

REM 手动指定核心数
build_windows.bat debug --jobs 8

REM 使用 Python 工具
python zbuild.py build --jobs 8
```

### 2. 预编译头文件（已启用 ✅）

ZEngine 已为 `ZRuntime` 和 `ZEditor` 配置了预编译头文件（PCH），可以显著加快编译速度。

**位置：**
- `engine/source/runtime/pch.h` / `pch.cpp`
- `engine/source/editor/pch.h` / `pch.cpp`

**注意：** 预编译头文件会自动使用，无需手动配置。

## ⚡ 进一步优化建议

### 1. 使用 Ninja 构建器（推荐）

Ninja 构建器通常比 Visual Studio 生成器更快：

```cmd
REM 使用 Ninja 预设
python zbuild.py configure --preset windows_ninja
python zbuild.py build --preset windows_ninja
```

**优势：**
- 更快的增量构建
- 更好的并行支持
- 更少的构建系统开销

### 2. 启用 Unity Builds（可选）

Unity Builds 可以将多个源文件合并编译，减少编译时间。**注意：** 这可能会增加增量编译时间。

```cmd
python zbuild.py configure --config debug --extra-args -DUSE_UNITY_BUILD=ON
python zbuild.py build --config debug
```

**适用场景：**
- 首次完整编译
- CI/CD 构建
- 不推荐用于日常开发（增量编译可能变慢）

### 3. 使用 RelWithDebInfo 配置

`RelWithDebInfo` 配置在保持优化的情况下保留调试信息，编译速度通常比 Debug 快：

```cmd
build_windows.bat relwithdebinfo
```

### 4. 只构建需要的目标

只构建你正在开发的目标，而不是整个项目：

```cmd
REM 只构建编辑器
python zbuild.py build --target ZEditor

REM 只构建运行时
python zbuild.py build --target ZRuntime
```

### 5. 使用增量编译

- 避免频繁清理构建目录
- 只在必要时执行完整重建
- Visual Studio 会自动使用增量编译

### 6. 优化磁盘性能

- 将构建目录放在 SSD 上
- 确保有足够的磁盘空间
- 关闭实时防病毒扫描（对构建目录）

### 7. 增加内存

- 确保有足够的内存（推荐 16GB+）
- 关闭不必要的应用程序释放内存

## 📊 性能对比

| 优化项 | 首次编译 | 增量编译 | 推荐度 |
|--------|---------|---------|--------|
| 并行编译（已启用） | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ✅ 必须 |
| 预编译头（已启用） | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ✅ 必须 |
| Ninja 构建器 | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ✅ 推荐 |
| Unity Builds | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⚠️ 可选 |
| RelWithDebInfo | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ✅ 推荐 |
| 只构建目标 | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ✅ 推荐 |

## 🔧 故障排除

### 并行编译没有生效

检查是否传递了 `-j` 参数：
```cmd
REM 查看构建命令
python zbuild.py build --jobs 8
```

### 编译速度仍然很慢

1. **检查 CPU 使用率**
   - 如果 CPU 使用率不高，可能是 I/O 瓶颈
   - 考虑使用 SSD 或增加内存

2. **检查磁盘空间**
   - 确保有足够的磁盘空间（至少 10GB+）

3. **检查防病毒软件**
   - 将构建目录添加到防病毒软件排除列表

4. **使用性能分析工具**
   - Visual Studio 的构建分析器
   - CMake 的 `--profiling-output` 选项

## 📝 最佳实践

1. **日常开发：**
   ```cmd
   REM 使用 Debug 配置 + 并行编译 + 只构建目标
   python zbuild.py build --target ZEditor --jobs 8
   ```

2. **性能测试：**
   ```cmd
   REM 使用 RelWithDebInfo 配置
   python zbuild.py build --config relwithdebinfo --target ZEditor
   ```

3. **发布构建：**
   ```cmd
   REM 使用 Release 配置 + Ninja
   python zbuild.py configure --preset windows_ninja --config release
   python zbuild.py build --preset windows_ninja --config release
   ```

## 🎯 总结

**已完成的优化：**
- ✅ Windows 上自动启用并行编译
- ✅ 自动检测 CPU 核心数
- ✅ CMake 预设中添加并行选项
- ✅ 构建脚本自动使用并行编译

**推荐操作：**
1. 使用 `build_windows.bat` 或 `python zbuild.py build`（已自动优化）
2. 考虑切换到 Ninja 构建器以获得更好性能
3. 只构建需要的目标而不是整个项目
4. 使用 RelWithDebInfo 配置进行日常开发

