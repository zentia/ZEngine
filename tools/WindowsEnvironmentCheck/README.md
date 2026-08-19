# Windows Environment Check

`ZWindowsEnvironmentCheck` 是独立的 Windows x64 环境兼容性检查工具，用于在不启动 Unity 客户端的情况下收集硬件信息、检查 DLL 构建配置，并通过隔离子进程验证 `GameCore.dll` 能否加载。

## 产物

Release 构建后需要一起分发：

```text
ZWindowsEnvironmentCheck.exe
ZDllLoadProbe.exe
LICENSE-Zydis.txt
LICENSE-Zycore.txt
```

- `ZWindowsEnvironmentCheck.exe`：主检查程序。
- `ZDllLoadProbe.exe`：无 C/C++ CRT、无 AVX/AVX2 的隔离 DLL 加载探针。
- `LICENSE-*.txt`：静态链接的 Zydis/Zycore MIT 许可证。

不要单独分发主程序，否则动态探针会报告 `DLL_PROBE_INFRASTRUCTURE_FAILED`。

## 检查内容

### 系统与硬件

- Windows 版本及 x64 架构
- CPU 型号、Family/Model/Stepping、逻辑处理器数量
- SSE4.1、SSE4.2、AVX、AVX2、FMA、F16C、AVX-512
- AVX/AVX-512 的 OSXSAVE 与 XCR0 状态
- 物理内存、可用内存、页面文件、系统运行时间
- GPU 型号、PCI ID、显存、驱动版本和驱动日期
- 显示器、分辨率、刷新率和色深
- 物理磁盘型号、总线类型和容量
- 卷、文件系统、总容量及剩余空间

### DLL 静态检查

工具递归扫描自身目录、当前工作目录及其子目录中的所有 `.dll`：

- PE 文件有效性
- x64 架构
- 导入库
- Debug CRT：`MSVCP*D.dll`、`VCRUNTIME*D.dll`、`ucrtbased.dll` 等
- 使用 Zydis 扫描 x64 Runtime Function 范围中的 AVX、AVX2、FMA、F16C、AVX-512 指令
- 输出指令 RVA、文件偏移、机器码及反汇编文本样例

静态包含 AVX/AVX2 不代表旧 CPU 一定执行该指令。受 CPUID 保护的优化路径允许存在，因此静态扫描只提供风险信息，不直接判定加载失败。

### DLL 动态探针

以下文件会自动进入隔离动态探针：

```text
GameCore.dll
GameCore_Standalone.dll
```

也可以通过 `--probe-dll` 显式指定。探针执行：

1. 在独立进程中调用 `LoadLibraryExW`。
2. 对 GameCore 默认查找并调用 `OSG_LocalPredictHardwareCapable()`。
3. 捕获非法指令、访问违规、栈溢出和 DLL 初始化异常。
4. 将 Win32 错误、异常码、导出函数返回值和 runner 输出写入报告。

## 基本使用

将工具与待测 DLL 放在同一目录，直接运行：

```powershell
.\ZWindowsEnvironmentCheck.exe --output-dir .\EnvironmentReport
```

默认支持无 AVX 的旧 CPU。如果产品明确要求某些指令集，可以配置：

```powershell
.\ZWindowsEnvironmentCheck.exe `
  --require avx,avx2,fma,f16c `
  --min-memory-gb 8 `
  --output-dir .\EnvironmentReport
```

只检查指定 DLL：

```powershell
.\ZWindowsEnvironmentCheck.exe `
  --check-dll .\GameCore.dll `
  --output-dir .\EnvironmentReport
```

显式执行隔离加载：

```powershell
.\ZWindowsEnvironmentCheck.exe `
  --require none `
  --probe-dll .\GameCore.dll `
  --output-dir .\EnvironmentReport
```

## Intel SDE 模拟旧 CPU

Intel SDE 不随本工具分发，需要从 Intel 官方网站下载并接受其许可协议：

<https://www.intel.com/content/www/us/en/download/684897/intel-software-development-emulator.html>

模拟 Nehalem（例如 Intel Core i5-760）：

```powershell
.\ZWindowsEnvironmentCheck.exe `
  --require none `
  --probe-runner "C:\Tools\IntelSDE\sde.exe" `
  --probe-runner-arg "-nhm" `
  --output-dir .\SdeReport
```

当前 SDE 中 `-nhm` 已同时配置 Nehalem CPUID 和 chip-check，不要额外传递无参数的 `-chip-check`。

若需要其他 CPU 模型，先查看：

```powershell
C:\Tools\IntelSDE\sde.exe -help
```

SDE 的 stdout/stderr 会写入报告中的 `runnerOutput`。关键字段：

```json
{
  "loadSucceeded": true,
  "exceptionCodeHex": "0x00000000",
  "runnerReportedIsaViolation": false,
  "runnerReportedInternalError": false,
  "exportCalled": true,
  "exportResult": 0
}
```

`OSG_LocalPredictHardwareCapable()` 在旧 CPU 上返回 `0` 表示本地预测优化不可用；只要 DLL 成功加载且没有非法指令，就不视为加载失败。

## 报告

工具生成：

```text
OSGameEnvironmentReport.txt
OSGameEnvironmentReport.json
```

当前 JSON 版本：

```json
{
  "schemaVersion": 5,
  "toolVersion": "1.4.0"
}
```

## 退出码

| 退出码 | 含义 |
|---:|---|
| `0` | 检查通过，可能包含非阻断警告 |
| `10` | CPU 不满足显式配置的最低 ISA |
| `11` | 不是受支持的 Windows x64 环境 |
| `12` | 物理内存不足 |
| `20` | DLL 依赖不可分发的 Debug CRT |
| `21` | DLL/PE 无效、架构不支持或静态检查失败 |
| `23` | DLL 加载、隔离探针、导出调用或非法指令失败 |
| `24` | 探针程序或 runner 基础设施不可用 |
| `30` | 主工具内部错误 |

常见 `issueCode`：

- `DLL_DEBUG_RUNTIME_UNSUPPORTED`
- `DLL_LOAD_FAILED`
- `DLL_PROBE_ILLEGAL_INSTRUCTION`
- `DLL_PROBE_EXCEPTION`
- `DLL_PROBE_TIMEOUT`
- `DLL_PROBE_EXPORT_NOT_FOUND`
- `DLL_PROBE_INFRASTRUCTURE_FAILED`

## 构建

独立配置和构建：

```powershell
cmake -S tools\WindowsEnvironmentCheck `
  -B build\WindowsEnvironmentCheck `
  -G "Visual Studio 17 2022" `
  -A x64

cmake --build build\WindowsEnvironmentCheck `
  --config Release `
  --target ZWindowsEnvironmentCheck
```

构建使用固定版本及 SHA-256 的 Zydis 4.1.1 和匹配的 Zycore。首次配置需要网络下载依赖，之后可复用 CMake FetchContent 缓存。
