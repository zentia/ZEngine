# RenderGraph RP1/RP2 默认开启：A/B 验证与回归

> 本文档固化「RenderGraph 接管 RP1 / RP2」的验证方法。两个 pass 现已**默认开启**
> （commit `871878e3` RP1、`c0d1bcfc` RP2，均 2026-10-26），手写链保留作回落与 A/B 比对。
>
> 关联：设计与无头断言见 `doc/RENDER_GRAPH.md`；翻默认前的可行性调查见
> `doc/PHASE2_STEP2.1_RENDERGRAPH.md`；本文件只管**真机逐帧等价性验证 + 回归**。

---

## 0. 一句话结论

**图驱动路径与手写链在真机 DX12 上逐位等价**，可用同帧 A/B 抓帧客观证明，无需肉眼比对。
验证只需两条命令：编辑器带 `ZENGINE_RG_AB_CAPTURE=<帧号>` 跑一帧 → `scripts/rg_ab_compare.py`
出判定（退出码 0=PASS / 1=FAIL / 2=INCONCLUSIVE）。默认开启的稳定性由一次 soak 兜底。

---

## 1. 适用范围与前提

| 项 | 说明 |
| --- | --- |
| 后端 | **仅 DX12**。RP1 的图路径有 DX12-only gate（`SetRenderGraphPathEnabled` 非 DX12 返回 false 并告警回落）；RP2 无此 gate（恒 true）。两者在 Vulkan/Metal 上都不会走图路径，本验证不覆盖。 |
| 需要的构建 | `ZEditor.exe`（Release，DX12）。构建命令见 §5。 |
| 需要的示例工程 | 任意能出画面的项目，本验证用 `I:\ZEngineDemo\ZEngineDemo.zproject`。 |
| 无需 GPU 目视 | A/B 抓帧读回像素后由脚本逐字节比对，**无显示器也能跑**（但编辑器本身要能起 DX12 设备）。 |

**环境变量速查**（都在 `DX12RenderGraphAbCapture.cpp` / `MainCameraRp{1,2}Pass.cpp`）：

| 变量 | 作用 | 默认 |
| --- | --- | --- |
| `ZENGINE_RG_RP1` | RP1 图路径开关。`0`=强制手写链；其它/未设=图路径（默认开） | 默认开 |
| `ZENGINE_RG_RP2` | RP2 图路径开关。`0`=强制手写链；其它/未设=图路径（默认开） | 默认开 |
| `ZENGINE_RG_AB_CAPTURE` | 设为帧号 `>=1`：那一帧每个 stage 先手写、再走图，各读回一张 `.zcap` | 未设（不抓） |
| `ZENGINE_RG_AB_DIR` | 抓帧输出目录（Win32 路径，反斜杠） | 工作目录下 `rg_ab_capture` |

> `ZENGINE_RG_AB_CAPTURE` 的抓帧**独立于** `ZENGINE_RG_RP1/RP2`：它自己强制两条路径各跑一遍再复位，
> 所以抓帧时不设 `ZENGINE_RG_RP1/RP2` 也能同时验证两个 pass（`DX12RenderGraphAbCapture.h:23`）。

---

## 2. 判定标准（PASS 的四条硬指标）

一次 A/B 抓帧要判 PASS，`scripts/rg_ab_compare.py` 必须退出码 0，且同时满足：

1. **每个 stage `graph_used=true`**（manifest.json）。若某 stage 的图路径回落手写，
   A==B 恒成立，比对失去意义 → 脚本判 INCONCLUSIVE（退出码 2），**不算 PASS**。
2. **每对图像逐位相同**（`IDENTICAL`）。默认容差 0：两次跑的是同一帧、同一输入，正确实现应逐字节相等。
3. **抓全图像**：manifest 里每个 `written=true`，无 `error`。
4. **debug layer 全程 0 条 ERROR/CORRUPTION**（DX12 debug layer 启动时无条件开启，
   `DX12RHI.cpp:2171 EnableDebugLayer()`，消息经 `DrainDebugLayerMessages()` `DX12RHI.cpp:1749` 进日志）。

退出码语义（`rg_ab_compare.py:14-18`）：`0`=PASS、`1`=有像素超容差、`2`=抓帧证明不了任何事
（图缺/读不出/图路径回落）。

---

## 3. 验证步骤 A：同帧 A/B 抓帧（等价性，必做）

### 3.1 跑一帧抓帧

编辑器是**纯 GUI**，不支持 `-batchmode`、不会自己退出（`Main.cpp:141`）。所以流程是：
后台起编辑器 → 轮询日志等抓帧完成 → 杀进程。

```bash
# Git Bash（注意：用 export 不用 set；路径用反斜杠，工具按 Win32 写盘）
export ZENGINE_RG_AB_CAPTURE=150                 # 抓第 150 帧（>=1 即可，避开首帧资源预热）
export ZENGINE_RG_AB_DIR='E:\tmp\rg_ab_rp2'      # 独立目录，避免覆盖
cd /e/Engine/ZEngine/bin/Release
./ZEditor.exe -p 'I:\ZEngineDemo\ZEngineDemo.zproject'   # 用 run_in_background 起
```

### 3.2 等抓帧完成

轮询输出目录，出现 `manifest.json` 且 `images[].written` 全 true 即完成（约 30–60s）：

```bash
sleep 20
cat /e/tmp/rg_ab_rp2/manifest.json     # 确认 "graph_used": true 两处都在
```

manifest 就绪后立即杀编辑器（后台 shell 会因此返回"failed"退出码——那是被 `taskkill`
终止的退出码，**不是真失败**）：

```bash
taskkill //F //IM ZEditor.exe
```

期望产物（RP1+RP2 各一对）：

```
manifest.json
rp1.handwritten.backup_odd.zcap   rp1.graph.backup_odd.zcap
rp2.handwritten.backup_even.zcap  rp2.graph.backup_even.zcap
rp2.handwritten.backbuffer.zcap   rp2.graph.backbuffer.zcap
```

### 3.3 客观比对

```bash
cd /e/Engine/ZEngine
python scripts/rg_ab_compare.py 'E:\tmp\rg_ab_rp2'
echo "exit=$?"      # 0=PASS
```

期望输出：

```
render-graph A/B capture: E:/tmp/rg_ab_rp2 (frame 150)
  rp1.backup_odd               3840x2126 R16G16B16A16_SFLOAT  IDENTICAL
  rp2.backbuffer               3840x2126 R8G8B8A8_UNORM       IDENTICAL
  rp2.backup_even              3840x2126 R16G16B16A16_SFLOAT  IDENTICAL
RESULT: PASS
```

> 排障可加 `--png`：在 `<dir>/compare/` 生成 handwritten/graph/diff 三张 PNG（HDR 会 tonemap），
> 直观看差异区域。加 `--json <file>` 落报告便于归档。

---

## 4. 验证步骤 B：默认开启 soak（稳定性，翻默认后必做）

翻默认后，用**默认开**（不设 `ZENGINE_RG_RP1/RP2`）跑一段时间，确认接管后无异常。

```bash
# 不设任何 ZENGINE_RG_* ，直接默认开跑
cd /e/Engine/ZEngine/bin/Release
./ZEditor.exe -p 'I:\ZEngineDemo\ZEngineDemo.zproject'    # run_in_background
```

跑数十秒到数分钟（数百帧以上），然后检查（编辑器日志在
`<project>/saved/logs/ZEditor_YYYYMMDD_N.log`，**追加跨会话**，查当前会话要按时间戳过滤）：

```bash
# 进程还活着？
tasklist //FI 'IMAGENAME eq ZEditor.exe' | grep -i ZEditor

LOG='I:\ZEngineDemo\saved\logs\ZEditor_20261026_1.log'
grep -c "RP2 is driven by the render graph" "$LOG"      # >=1：确认 RP2 真被图接管
grep -ic "CORRUPTION" "$LOG"                             # 0：debug layer 无损坏
grep -c  "device removed" "$LOG"                         # 0：无设备丢失
```

判据：进程稳定（内存不持续暴涨）、`RP2 is driven…` 出现、`CORRUPTION`=0、`device removed`=0、
无 crash/hang。

> **已知噪声（不算回归）**：
> - `CreateComputePipelineState failed HRESULT=0x80004005` —— Nanite GPU-cull 计算管线在当前
>   环境取不到，回落到 CPU culling，与 render pass 无关。
> - `IBL textures unavailable (device removed?)` —— 括号里的 "device removed" 只是提示文案，
>   不是真设备丢失；按时间戳过滤后当前会话应为 0。
> - AutoReimport 的 `source file missing` —— 示例工程引用了 D 盘已不存在的源文件，与渲染无关。

---

## 5. 复现构建命令

改动 render pass 后重建（native，DX12）：

```bash
unset http_proxy https_proxy
export VULKAN_SDK='C:\VulkanSDK\1.4.363.0'
export PATH="/c/Program Files/CMake/bin:$PATH"
MSYS_NO_PATHCONV=1 cmake --build 'E:\Engine\ZEngine\build' --config Release --target ZEditor -- -m -v:minimal
```

> `MSYS_NO_PATHCONV=1` + 构建目录用**反斜杠 Windows 路径**，否则 `/e/...` 会被判成"不是目录"。
> `engine_log.h` 若被 native configure 重新生成，会作废 web PCH，需回 build-web 跑
> `ninja ZEnginePCH libZRuntime.a`（见各盘 memory）。

---

## 6. 回归测试用例

分两层：**无头自测**（每次改动可跑，秒级）+ **真机 A/B**（翻默认/改图路径时必跑）。

### 6.1 无头自测：`scripts/rg_ab_selftest.sh`（离线，无需 GPU）

覆盖**抓帧编解码 + 比对脚本**这一半（`RenderGraphAbCaptureFormat.h` 的 `.zcap`/manifest 编码器
对 `DX12RenderGraphAbCapture` 实际运行的代码；不含 GPU 的 `CmdCopyImageToBuffer`/布局恢复）。

```bash
cd /e/Engine/ZEngine
bash scripts/rg_ab_selftest.sh        # 需 emsdk@C:/emsdk、node、python3
```

用例矩阵（脚本内 `expect`，退出码必须精确匹配）：

| 场景 | 期望退出码 | 验证点 |
| --- | --- | --- |
| `identical` | 0 | 两跑全等 → PASS；`--png` 生成合法 PNG |
| `one_pixel` | 1 | 恰好 1 个不同像素，bbox=(123,45)，max diff=999-123，其余两对仍 IDENTICAL |
| `tiny_diff` | 1 / `--tolerance 0.01`→0 | 4×2 块、max diff=1/1024：超默认容差判 FAIL，容差 0.01 内判 PASS |
| `fell_back` | 2 | 图路径回落手写 → INCONCLUSIVE（防"假 PASS"） |
| `missing` | 2 | 图像缺失/读不出 → INCONCLUSIVE |
| NaN 同行 | 1 | 两跑**相同 NaN 位**视为相等（NaN≠NaN 的浮点陷阱），同行另一处真差异仍被检出且仅 1 个 |

并校验：`.zcap` 头 round-trip（HDR 300×70×8 / LDR 300×70×4）、行 padding 被剥离（每行首尾像素精确）、
manifest JSON 转义 round-trip。**这一层不过夜、不依赖显示器，应作为每次改动的默认回归。**

### 6.2 真机 A/B 回归清单（改图路径/翻默认时必跑）

在 §3、§4 基础上固化为勾选清单：

- [ ] **RP1 等价**：`ZENGINE_RG_AB_CAPTURE=<帧>` 抓帧，`rg_ab_compare.py` 退出码 0，
      `rp1.backup_odd` IDENTICAL，manifest `rp1.graph_used=true`。
- [ ] **RP2 等价**：同次抓帧 `rp2.backup_even` / `rp2.backbuffer` 全 IDENTICAL，
      `rp2.graph_used=true`。
- [ ] **debug layer 干净**：抓帧全程 `CORRUPTION`=0。
- [ ] **默认开 soak**：不设 `ZENGINE_RG_*` 跑数百帧，进程稳、`RP1/RP2 is driven…` 出现、
      `CORRUPTION`=0、`device removed`=0、无 crash/hang。
- [ ] **回落仍可用**：`ZENGINE_RG_RP1=0`（或 RP2）时手写链照常出画面（目视或再抓一帧确认非黑）。

### 6.3 何时必须重跑

| 触发 | 跑哪层 |
| --- | --- |
| 改 `MainCameraRp{1,2}Pass` 的图路径 / `DrawRP*WithGraph` | 6.1 + 6.2 全跑 |
| 改 `RenderGraph*`（编译器/执行层/资源池） | 6.1 + 6.2 全跑 + `RENDER_GRAPH.md` 的无头断言 |
| 改 `DX12RenderGraphAbCapture` / `RenderGraphAbCaptureFormat.h` / `rg_ab_compare.py` | 6.1 必跑 |
| 仅改注释/文档 | 无需 |

---

## 7. 已知边界（本验证不覆盖）

- **仅 DX12**：Vulkan/Metal 的图路径未启用（RP1 有后端 gate，RP2 虽无 gate 但图路径同样只在 DX12 验证过）。
- **GPU 侧抓帧时序**：`rg_ab_selftest.sh` 用合成 `.zcap` 覆盖编解码，真正的 `CmdCopyImageToBuffer`、
  读回时机、layout 恢复只在真机 A/B（§3）里被覆盖。
- **其它 pass**：TAA / Bloom 的图路径仍是 opt-in（见 `TAAPass.h:22`、`BloomPass.h:14`），
  本验证只覆盖 RP1/RP2 主链。
