# ZEngine Agent Kit

> 把 ZEngine 从「一个游戏引擎」改造成「一个自带 AI 可观测性与 agent 工具链的开源引擎」。
> 附带一套可复现的崩溃根因诊断流水线与 eval 框架。

---

## 一、为什么是 ZEngine

做 agent 工程最难的不是写编排，是**证明它有效**。而证明有效需要 ground truth，ground truth 在真实项目里极贵。

ZEngine 恰好把这个最贵的东西变成了几乎免费：

```
在自己的引擎里注入一个已知 bug → 编译 → 触发崩溃 →
你必然知道根因的文件和行号 → 这就是一个带精确标签的样本
```

这是「引擎即 eval 生成器」。引擎不再只是被分析的对象，它同时是**标签工厂**。这个身份是别的 agent 项目拿不到的——他们要么用公开数据集（分布不匹配），要么人工标注（规模上不去）。

配合已有的三平台 CI（Windows / Linux / macOS），故障注入可以整夜批量跑，一晚上产出上百个带标签样本。

**这就是整个项目的定位：**

> 一个自带故障注入与 eval harness 的开源游戏引擎 + agent 诊断套件。
> 别人卖的是「我的 agent 能查崩溃」，这里证明的是「我知道我的 agent 在哪些情况下会查错」。

---

## 二、四层结构

```
┌─────────────────────────────────────────────────────────┐
│  L4 编排层  Orchestration                                 │
│     诊断流水线 / 假设验证 / 证据等级                        │
│     zagent/pipeline.py                                   │
├─────────────────────────────────────────────────────────┤
│  L3 验证层  Verification                                  │
│     调用链规则 / 空指针规则 / 生命周期规则                  │
│     zagent/verify.py                                     │
├─────────────────────────────────────────────────────────┤
│  L2 工具层  Tooling (MCP)                                 │
│     renderdoc-mcp（已有，42 工具）                         │
│     engine-mcp（待建：控制引擎 / 抓帧 / 读状态 / 注入故障）  │
├─────────────────────────────────────────────────────────┤
│  L1 引擎层  Instrumentation                               │
│     结构化崩溃快照 / 帧数据导出 / Profiler & Insights 对接   │
│     engine/Source/Runtime/..., engine/Source/Insights/     │
└─────────────────────────────────────────────────────────┘
```

L2 已有 `tools/renderdoc-mcp`（42 个工具），说明这条路已经走了一半——那个项目本质就是「让 AI 能读 GPU 帧」。缺的是把它接进一个带验证的闭环。

---

## 三、目录

```
agentkit/
  zagent/
    types.py      核心数据类型：Facts / Hypothesis / Verdict / Evidence / Report
    tracing.py    结构化轨迹，JSONL 落盘
    protocols.py  可替换适配器接口
    adapters.py   参考实现：git 代码索引、本地 dump、addr2line 符号化
    verify.py     Verifier 与 8 条验证规则
    pipeline.py   diagnose() 主流水线
    eval.py       样本集 / 指标 / runner（EvalReport）
    loop.py       Agent loop：思考 -> 行动 -> 观测（含防呆与可中断）
    policy.py     风险分级与自治档位（plan / normal / autopilot）
    tools/
      base.py     Risk / ToolResult / Tool / ToolRegistry
      shell.py    通用命令执行（永不挂住，说清为什么返回）
      code.py     CodeIndex -> 工具（纯转发，行为与规则层一致）
  zfault/
    patterns.py   故障模式库（9 个模板）
    variants.py   难度变体生成器：depth / decoy，正负样本同源
    process.py    崩溃进程捕获
  zlab/
    lab.py        实验台：编译 -> 崩溃 -> 符号化 -> 诊断
    probe.py      运行时探针注入（堆证据 / 崩溃点 / 分配点）
    symbols.py    PDB 符号化
  tools/
    check_variants.py   变体自检：注入 -> 编译 -> 实际运行
    crash_verify.cpp    崩溃真实性校验
    verify_build_id.py  构建身份校验
  examples/
    minimal_run.py         最小可跑闭环（不需要真实 dump）
    run_eval.py            合成样本 eval
    run_eval_lab.py        真实样本 eval（8 个手写模板）
    run_eval_lab_scale.py  大规模 eval（变体，接 EvalReport 全指标）
    agent_demo.py          agent loop 演示（不需要 LLM）
  tests/
```

---

## 四、三条贯穿全局的约束

这三条是从真实崩溃分析流程里抽象出来的，是整套东西真正值钱的部分。

### 1. 版本对齐门禁

用错版本的代码做分析，结论全废，而且**错得很隐蔽**——符号能对上、行号看着合理、推断自洽，全是假的。所以版本不对齐必须硬停，不允许静默切换分支或 revert 本地改动。

### 2. 证据等级三态

```
CONFIRMED   有实证（文件:行号 / 命令输出 / 手工计算）
REFUTED     被证据推翻 —— 保留在台账里，体现纠偏过程
UNPROVEN    兼容但无法锁定 —— 允许存在，但必须标注，且拉低置信度
```

比二值判断诚实。允许「看起来对但没法逐字节确认」这种中间态，是长期做非确定性排查的人才会有的设计。

### 3. 未验证不进结论

硬护栏，不是建议。`UNPROVEN` 可以出现在报告里，`REFUTED` 绝对不能变成结论。

---

## 五、Eval 指标

| 指标 | 定义 | 为什么重要 |
|---|---|---|
| `top1 / top3` | 根因位置准确率（文件/函数/行 三档） | 基础能力 |
| `false_positive_rate` | 结论中被推翻假设占比 | 越低越好 |
| **`honesty`** | 负样本上主动声明 unknown 的比例 | **最关键**：不会就说不会，比总给错误答案有价值得多 |
| `evidence_completeness` | 结论中带可回溯证据的假设比例 | 防幻觉 |
| `stability` | 同样本跑 3 次结论一致率 | 非确定性系统的可用性底线 |

`honesty` 单独强调：它直接对应「未验证不下结论」这条设计，而且是可量化的。一个在信息不足时说「我不知道」的系统，工程价值远高于一个总能给出错误答案的系统。

---

## 六、大规模 eval 的口径

`examples/run_eval_lab_scale.py` 把模板扩成难度变体后跑全指标。有三个
口径问题容易被忽略，它们都直接决定数字能不能信：

**正负样本按「是否拿到运行时证据」划分，不按「是否崩溃」。**
泄漏与竞争本来就**不该崩溃**：它们的证据是堆分配点 / 冲突现场，不是异常。
实测 `leak_01` / `race_01` 都不崩但定位正确（453/453）。若用"没崩"当
负样本判据，会把两个真命中记成"克制正确"——既虚增 honesty 分子，又把它
们从 top1 分子里扣掉。

**负样本同样带难度维度。** 只拿一个 clean_01 测 honesty，测的是「最干净
的干净」；系统在这种样本上克制，说明不了它在「干净但调用链很深、隔壁就是
一段高度相似但有正确检查的死代码」时是否还克制。所以负样本也按 depth /
decoy 展开，honesty 同样能按难度分组。

**stability 未测量时显示 n/a，不是 0.00%。** `repeat<=1` 时稳定性分支根本
没跑，此时渲染成 0.00% 是把「未测量」伪装成「0 分」。

当前实测（`--per 12`，56 个样本，Windows/MSVC）：

```
top1 100%   top3 100%   honesty 100%   evidence 100%
按 depth: 0/1/2/3 各档均 100%
按 decoy: 有/无 各 100%
```

读这个 100% 时要记住两件事：故障语义只有 9 个模板，变体改的是定位难度
不是缺陷本质；且全部样本都在同一台机器上产生。

---
## 七、从流水线到 Agent loop

`diagnose()` 是一条**写死的五阶段流水线**：步骤确定、可回归、可 eval。
它的代价是自主性为零 —— 决策者只能接受流水线喂给它的东西，没有"选一个
工具去查"的能力，于是也没有等待管理、自主决策、与人交互的必要。

新增的 `zagent/loop.py` + `zagent/tools/` 补齐了另一半。分工是刻意的：

| | 流水线 `diagnose()` | loop `AgentLoop` |
|---|---|---|
| 适用 | 步骤确定、要可回归 | 步骤不确定 |
| 谁决定下一步 | 代码写死 | 决策者（`Planner`） |
| 领域知识在哪 | 阶段内部 | **仍在工具里**（Verifier） |

**关键：Verifier 不进 loop。** "什么时候该验证"交给模型决定，"怎样算验证
过"仍由那 8 条规则把持 —— 这就是既自主又不幻觉的结构。下一步应把
`diagnose()` 整体降级成 loop 可以调用的一个工具，而不是改写它。

### 7.1 命令不再"一直等待"

`tools/shell.py` 针对五类成因分别处理，并**把原因结构化返回**：

| 成因 | 对策 | 返回状态 |
|---|---|---|
| 没设超时 | wall-clock timeout | `timeout` |
| 在等 stdin | `stdin=DEVNULL`，立刻 EOF | `nonzero_exit` |
| 分页器 | `PAGER=cat` / `TERM=dumb` / `GIT_TERMINAL_PROMPT=0` | — |
| 只杀父进程 | 杀进程树 | — |
| 输出一次性收回 | 线程边跑边收 | **超时也保留部分输出** |

最后一条最致命也最易漏：编译失败时前 200 行就是错误信息。丢掉它，模型
只能原样重试 —— 而原样重试正是"一直等待"的观感来源。

`awaiting_input` 必须单独成档：模型看到它会去换非交互写法，看到
`timeout` 会去加长时限。笼统回 `failed`，两者都只能重试。

### 7.2 风险分级取代全局开关

`auto_checkout=False` 那种布尔开关，是用一个值同时回答了三个不同风险
等级的问题。改为按**可挽回性**分级（read / search / write / exec /
destructive / network / irreversible），再叠会话档位：

- `plan` 只读与检索，其余先问
- `normal` 本地可撤销动作自动，破坏/出网/不可逆先问
- `autopilot` 除不可逆外全部自动

另有一条：**低置信度本身也触发询问**。这把「不确定性」从报告里的一个
标注，变成了控制流的一部分 —— 模型说没把握，系统就真的去问人。

### 7.3 交互是决策者的一种输出

`AskUser` 与 `Action`、`Finish` 并列，是决策者**主动选择**的 step。
区别在于：阻塞是系统替模型决定"到此为止"，这里是模型自己判断"我需要人"。
前者做不出 agent。`AgentLoop.push()` 另提供中途插话（steering）通道。

### 7.4 三道防呆

1. `max_steps` —— 拿不到结果的决策者会无限循环烧 token，这是 ReAct 第一号故障模式
2. 停滞检测 —— 连续失败或原样重复上一步即停，"一直在动但无进展"比卡住更贵
3. 工具调用永不抛异常 —— 错误降级为观测，模型才有机会自我纠正

---
## 八、CI 的平台分工

两个工作流，职责分开：

`agentkit.yml` 跑三平台（ubuntu / macos / windows）× Python 3.11/3.12 的
平台无关测试，外加一个 windows-only 的 `lab` job 跑真实实验台（真编译、
真崩溃、真符号化）。`agentkit-eval.yml` 跑大规模 eval，只在 push main 与
每晚定时触发 —— 它要跑上百次编译+崩溃，本机实测 5 分钟以上，放进 PR
门禁会把每次改动都拖到十分钟。

**关键在于「能编译」和「能跑实验台」不是一回事。**

`Toolchain.supported` 只表示找到一个能编 C++17 的编译器，Linux 与 macOS
的 clang++/g++ 都满足。但 zlab 需要的是 MSVC + PDB：探针注入的是
`#include <windows.h>`，崩溃捕获依赖 SEH 与 RtlVirtualUnwind，符号化走
`MsSymbolResolver` 读 PDB。

于是 `supported=True` 时实验台照样跑不了。这个区分由
`Toolchain.can_run_lab` 显式表达，测试据此 skip。此前判据写的是
`skipif(not supported)`，在 Linux CI 上会**不 skip 而硬跑** Windows 专用
代码然后失败 —— 看起来像「测试坏了」，实际是平台不支持，很难归因。

两个工作流都带了守卫，防止「全绿但全是 skip」：用 `--junit-xml` 拿结构化
计数，实际执行数低于阈值就报错。这里刻意不解析 pytest 的文本输出 —— 在
`-rA` 模式下输出很长，容易截丢汇总行，而 grep 失败回退成 0 又会造成误报。
一个自己会失效的守卫比没有守卫更糟，因为它给的是假的安全感。

---
## 九、路线图

- [x] L4 骨架：流水线 + tracing + 证据等级 + 简化 Verifier
- [x] L3 扩展：已注册 8 条规则 —— callchain / nullcheck / unsigned_underflow / double_free / leak / recursion / overflow / race
- [x] L1 引擎侧：结构化崩溃快照导出 —— crash_snap.json (schema zengine-crash/1) + PDB GUID/Age 构建身份 + 模块基址 + 线程 ID；修掉十六进制与十进制的千分位污染。已用 MSVC 真实编译+运行验证。
- [ ] L2 engine-mcp：控制引擎、抓帧、读运行时状态
- [x] **故障注入器**：9 个模板 × depth(0-3) × decoy 难度变体，正负样本同源；真实编译+崩溃+符号化
- [x] CI 批量产样本：.github/workflows/agentkit.yml（三平台测试 + Windows 真实实验台）与 gentkit-eval.yml（大规模 eval，push main / 每晚定时）← 差异化核心
- [ ] eval 报告站：命中率 / 诚实度 / 失败案例库公开

**Agent 运行时**（第七节）：

- [x] 工具层：`ToolRegistry` + `Risk` 分级；工具调用永不抛异常，错误降级为观测
- [x] 通用命令执行：五类挂死成因分别处理；超时保留部分输出；`awaiting_input` 单独成档
- [x] `AgentLoop`：思考->行动->观测，含 max_steps / 停滞 / 重复三道防呆
- [x] `Policy`：三档自治（plan / normal / autopilot）+ 低置信度触发询问
- [x] HITL：`AskUser` 作为一等 step + 中途插话（steering）
- [x] `CodeIndex` 映射为工具（纯转发，不改动验证层行为）
- [ ] `Planner` 的 LLM 实现（当前只有 `ScriptedPlanner` 确定性基线）
- [ ] 把 `diagnose()` 整体降级为 loop 可调的一个工具
- [ ] `Verifier` 暴露为工具：模型决定何时验证，规则决定怎样算验证过
- [ ] 上下文压缩（轨迹变长后的摘要/裁剪）
- [ ] 把工具层接到 MCP，让外部 agent 也能用这套能力

---

## 十、快速开始

```bash
cd agentkit
python -m examples.minimal_run
```

不需要真实 dump、不需要引擎编译，跑的是一个合成的崩溃样本，完整走完门禁 → 取事实 → 版本对齐 → 符号映射 → 假设验证 → 报告。

---

## 十一、与内部项目的关系

`agentkit/` 里的框架代码与任何闭源工程无耦合。闭源工程提供的是**领域规则**（Verifier 里的判断逻辑），那些不进这个仓库。

对外讲的是框架和 eval；领域知识是个人在具体岗位上的溢价，不需要开源。
