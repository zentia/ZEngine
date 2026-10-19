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
    llm.py        LLM 决策者（零依赖 OpenAI 兼容客户端 + 可注入 transport）
    gate.py       结论硬门禁：在 finish 处拦截没有证据支撑的结论
    cli.py        **入口**：交互式命令行 agent
    eval_agent.py agent 级 eval（与流水线共用一把尺子）
    tools/
      base.py     Risk / ToolResult / Tool / ToolRegistry
      shell.py    通用命令执行（永不挂住，说清为什么返回）
      code.py     CodeIndex -> 工具（纯转发，行为与规则层一致）
      crash.py    diagnose() 降级为工具 —— 崩溃诊断只是其中一个能力
      verify_tool.py  Verifier 暴露为工具 —— 裁定权归规则，调用时机归模型
      build.py    构建/测试暴露为会发裁定的工具 —— 编辑器 AI 的验证落点
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

### 7.4 LLM 决策者（`zagent/llm.py`）

零第三方依赖：标准库 `urllib` 直连 OpenAI 兼容的 `/v1/chat/completions`，
覆盖 OpenAI / DeepSeek / Qwen / Moonshot / vLLM / Ollama 等一整套部署。
换供应商只改环境变量，不换代码。

**必须可注入 transport。** `FakeTransport` 与 `ScriptedPlanner` 是同一个
道理，但在这里还多一层意义：**eval 必须可复现**。真模型有随机性，用它跑
出来的数字无法重放，也就无法回答"改了一版是不是变好了"。默认
`temperature=0`，并允许整批响应稳定重放。

**失败一律降级为观测。** 网络错、超时、畸形 JSON、调了不存在的工具 ——
都变成 `BadStep` 而不是异常。畸形输出是 LLM 最高频的故障，而它几乎总是
可恢复的：模型看到"你上一步的 JSON 没闭合"就能自己改对。把它当异常，
等于把最常见的可恢复错误升级成致命错误。测试里失败路径的用例比开心路径
多，这是有意的。

工具调用走两条路径：原生 `function calling` 优先，解析不出时回退到从文本
里抠 JSON 块（兼容不开 function calling 的本地推理服务）。

**预期要接受的一件事**：接上 LLM 之后，eval 的 `top1` 大概率会从当前的
100% **掉下来**。现在这个 100% 是 8 条规则在 9 个故障模板上手调出来的，
不是泛化能力。这个"掉了 → 归因 → 修 → 指标变化"的过程，才是这套东西
真正值钱的部分；为了保住 100% 而不接 LLM 是本末倒置。

### 7.5 三道防呆

1. `max_steps` —— 拿不到结果的决策者会无限循环烧 token，这是 ReAct 第一号故障模式
2. 停滞检测 —— 连续失败或原样重复上一步即停，"一直在动但无进展"比卡住更贵
3. 工具调用永不抛异常 —— 错误降级为观测，模型才有机会自我纠正

### 7.6 CLI：入口是它，不是崩溃诊断

```bash
python -m zagent.cli                        # 交互
python -m zagent.cli --task "..."           # 单次执行后退出（CI）
python -m zagent.cli --root <repo> --dump <x.json> --task "定位这次崩溃"
```

斜杠命令：`/help` `/tools` `/mode plan|normal|autopilot` `/trace` `/exit`。

**没有模型也要能用。** 未配 API key 时退到 `RulePlanner`：能派发诊断、
能提问，但**如实声明自己是规则路由**、"只走了一步"。为什么不做成没 key
就退出：那会让 CLI 在离线分析机和 CI 上变成死文件。降级后仍可用、且说清
自己在降级，比"要么全有要么全无"有用得多。

**执行中可以插话。** 任务跑在后台线程，主线程同时读键盘：说话即注入
（steering），agent 提问则由主线程接过来问人。两个线程不能同时读 stdin
（会互相吃输入），所以提问走队列 + 事件：agent 阻塞等，主线程统一读。

### 7.7 崩溃诊断降级为工具（关键的一步）

`tools/crash.py` 把 `diagnose()` 包装成 `diagnose_crash`。方向很重要：
**不是把流水线改写成 agent，而是让 agent 去调用它**。流水线因此保住确定性
（可回归、可 eval），agent 因此拿到自主性。

分水岭在**版本不一致**这个场景上：

- 旧行为：`auto_checkout=False` → 硬 `return report`，到此为止。没有任何
  通道能继续 —— 这正是"自动化工具"和"agent"的分界。
- 新行为：工具返回 `blocked` 并说明原因，**agent 读到这条观测后自己决定
  下一步**：调 shell 切分支、切完再调一次。

护栏一个字没改，从"死路"变成了"可绕过的路障"。

工具**强制 `auto_checkout=False`**：自动切分支会改动开发者的 working tree，
那是破坏性操作，该由 agent 显式调 shell 去做并走审批，不能藏在"只读诊断"
里偷偷做掉。这条有测试守着（`test_never_auto_checkout`）。

### 7.8 接 eval：同一份样本、两条路径、同一把尺子

```bash
python -m examples.run_eval_agent
```

一次跑出两组数字：流水线基线 vs agent。两组用**完全相同**的 `score()`
评分，所以差异只可能来自路径本身：

```
[对照]
  top1         100.00% -> 100.00%   +0.00%
  top3         100.00% -> 100.00%   +0.00%
  honesty      100.00% -> 100.00%   +0.00%
  evidence     100.00% -> 100.00%   +0.00%
```

**这个"一致"本身就是结论**：它证明 agent 层没有引入损耗。之后接上真 LLM
时掉下来的每一个点，就都可以归因到模型，而不是管道。

没有这个对照实验会怎样？假设接 LLM 后 top1 掉到 87%，你看到"掉了 13 个
点"，**但无法归因** —— 它可能是模型不行，也可能是你的工具层有 bug。
对照实验的作用就是把这两种可能分开。

新增的只有 agent 特有的过程指标：平均步数、工具错误率、**停滞率**、
未产出报告率。它们衡量的是过程，结论质量仍由原指标负责。

### 7.9 Verifier 工具化：既自主又不幻觉

`tools/verify_tool.py` 把 8 条验证规则暴露成工具。分工是这套东西的核心：

- **模型决定何时验证**：看到可疑的地方就去调，调几次、验证什么，由它定；
- **规则决定怎样算验证过**：裁定发生在模型之外，它无法自己给自己发
  CONFIRMED。

这就是和一万个玩具 ReAct demo 的分界线 —— 玩具 agent 的"验证"是模型
自己再说一遍"我确认了"，那等于没验证。

三个必须做对的语义：

| 裁定 | `ok` | 能否进结论 | 为什么 |
|---|---|---|---|
| `CONFIRMED` | True | 是（且需可回溯证据） | 成立 |
| `REFUTED` | **True** | 否 | 验证成功了，答案是"不成立" |
| `UNPROVEN` | **True** | 否 | 无法判定是合法结论，猜一个不是 |

**REFUTED 绝不能回成 `ok=False`。** 否则模型会把"这条假设不成立"当成
工具故障去重试同一条 —— 恰恰是把正确结论当成了故障，而且会死循环。

结论门槛是 `CONFIRMED` **且** 带可回溯证据。少了后一半，一条"我说它对"
也能混进结论 —— CONFIRMED 是必要不充分条件。

台账（`conclusion_ledger`）保留全部，包括被推翻的。只留成功的台账会让
复盘变成自证；能回答"试过什么、为什么否掉"才可归因。

### 7.10 结论硬门禁（`zagent/gate.py`）

上一版只有 prompt 约束 + `conclusion_ledger` 自检，两者都依赖模型自觉 ——
**而模型不遵守 prompt 恰恰是我们要防的那种故障**。靠被防的对象自觉遵守
规则，不算门禁。现在把它变成控制流：结论被拒，这一步就结束不了。

**为什么它绕不过去**：允许收尾的结论集合 = 轨迹里被规则判为 CONFIRMED
且带可回溯证据的 claim，而模型**无法自己往这个集合里加东西** —— 它只能
调 `verify_hypothesis`，裁定权在规则手里。所以凭空写一条结论必然匹配
不上、必然被拒。这不是"希望它别瞎编"，是"编了就会被拦"。

二分规则，不给第三条路：

| 情况 | 结果 |
|---|---|
| 下结论 | 每条都要匹配 CONFIRMED + 可回溯证据，否则打回 |
| 把 REFUTED 的写进结论 | 打回（最严重，单独报） |
| 不下结论 | 必须说明没查清什么（`--strict` 下强制） |

**装在哪很关键**：门禁装在 `AgentLoop` 上，不在 planner 上。planner 可
以被替换，而这道约束必须对所有决策者都成立 —— 装在 planner 里，换一个
决策者就等于把护栏拆了。

**被拒不终结运行**：变成一条观测塞回轨迹，模型读到"为什么被拒"后可以
自己改；连续 3 次不改才判停滞。直接终结会让这一步的结论直接丢失。

**门禁靠扫描观测工作，不认识具体工具**：任何工具只要在 `ToolResult.data`
里给出 `verdict` 或 `conclusions`，就自动参与门禁。所以 `diagnose_crash`
的假设台账也能直接喂给门禁 —— 将来加新能力不用改门禁。

刻意没做的是**解析 answer 的自然语言**：从一段话里判断"它是不是在断言"
做不准，做不准的门禁会误杀正确的收尾。门禁只裁定模型**显式声明**的
`conclusions` —— 声明是它自己给的，所以判定是确定的。

```bash
python -m zagent.cli --strict ...   # 严格档位：不结论就必须说不知道
```

#### 门禁对照实验

```bash
python -m examples.run_eval_gate
```

模拟一个**幻觉率可控**的模型（默认 70%），三组对照：

```
A 门禁关                泄漏  2/2  ( 100%)   停滞  0   诚实收尾  0
B 门禁开（会改正）           泄漏  0/2  (   0%)   停滞  0   诚实收尾  2
C 门禁开（嘴硬）            泄漏  0/2  (   0%)   停滞  2   诚实收尾  0
```

**为什么用模拟模型而不是真模型**：要回答的是"门禁有没有压住幻觉"。
用真模型跑一遍，你不知道它的**基线幻觉率** —— 看到只掉 2 个点，无法
区分"门禁有效"和"这模型本来就不怎么瞎编"。幻觉率钉死之后，漏检率可以
直接算而不是靠猜。

C 组是关键：同为 0 泄漏，但代价是**停滞** —— 拦不住时退化成停滞，
而不是静默放行。可见的故障比不可见的故障好处理得多。

模拟模型证明的是**机制**有效，不证明真实模型的幻觉率是多少 —— 后者要
接真模型量。真模型能补的是我没预料到的失败模式，两者都要做。

> 注：分母是"实际发生幻觉的样本"。把无处可编的负样本算进分母会把泄漏率
> 稀释成"看起来还行"的数字 —— 指标没坏，是尺子量错了东西。

### 7.11 编辑器 AI：构建/测试即裁定（`tools/build.py`）

"跟编辑器描述一个功能，它自动实现**并验证**" —— 这里解决后半句。

一个能写代码的 agent 最常见的失败不是写不出来，是**它说"改好了"但根本
没编译过**。让 agent 直接调 shell 跑 `zbuild.py` 解决不了：shell 的结果里
**没有裁定**，"构建通过"这句话仍然是模型自己说的。

`build_project` / `run_tests` 把裁定权收进工具，写进 `data.verdict`：

| 情况 | 裁定 | 能进结论 |
|---|---|---|
| 退出码 0 **且**输出里有证据 | `confirmed` | ✓ |
| 退出码非 0 | `refuted` | ✗ |
| 退出码 0 但读不出证据 | `unproven` | ✗ |

**第三行是本工具存在的全部理由。** 三种"看起来成功、其实没验证"：

1. 构建无事可做（`up-to-date` / `Nothing to be done`）→ 退出 0
2. 测试收集到 0 个用例 → 退出 0
3. 目标拼错、没匹配到任何东西 → 有些构建系统仍退 0

若图省事改成"退出码 0 就 confirmed"，门禁依然会照常放行 —— **但它已经
什么都拦不住了，而且没有任何症状**。这种失效不会报错，只会让门禁变成装饰。

因为 `collect_claims` 扫描任意工具 `data` 里的 `verdict`，**门禁一行都不用
改**就对新领域生效。当初"不认识具体工具"这个抽象，就是为了这一天。

```bash
python -m examples.run_eval_build
```

```
A 门禁关                泄漏 2/2    停滞 否   步数 1   收尾 DONE
B 门禁开（会改正）           泄漏 0/2    停滞 否   步数 4   收尾 DONE
C 门禁开（嘴硬）            泄漏 0/0    停滞 是   步数 3   收尾 stalled
D 门禁开 + 空构建          泄漏 0/0    停滞 是   步数 6
```

D 组是关键：跑了构建，但输出是 `Nothing to be done` / `0 passed`，
仍被拦住 —— 退出码 0 不等于验证过。

执行器是注入的（`make_build_tools(runner=...)`），所以实验用假构建，
秒级可复现，不碰真实工程（真构建一次可能几十分钟）。

### 7.12 接真模型

换供应商/内网网关只改环境变量，不改代码（走 OpenAI 兼容协议）：

```bash
set ZAGENT_BASE_URL=https://api.hunyuan.cloud.tencent.com/v1
set ZAGENT_MODEL=<模型名>
set ZAGENT_API_KEY=<混元控制台创建的 key>
python -m examples.llm_check          # 先自检
python -m examples.run_eval_gate --real
```

混元的 API Key 与 CAM 密钥**不是同一个东西**，要在混元控制台 / TokenHub
单独创建。没配 key 时自动退到 `RulePlanner`，不报错退出 —— 离线分析机和
CI 上仍可用。

**先跑 `llm_check`。** 它探的不是连通性，是**端点支不支持 function
calling** —— agent 能不能干活取决于这个，不取决于能不能聊天。不支持的话
`LLMPlanner` 会回退到"从文本里抠 JSON"，那条路能用但明显更脆，而且症状
是"agent 一直在原地打转"，根因极难发现。

`--real` 会同时报**声明率**。没有它，"0% 泄漏"可能只是"模型压根没填过
conclusions" —— 那是不可证伪的指标，看着漂亮，什么都没证明。

### 7.13 接内部 stream-server（`/openapi` 那套）

内部 stream-server 的 OpenAPI 与公网混元**不是一套东西**，差异足以让标准
OpenAI 客户端直接失败：

| 差异 | 公网 OpenAI/混元 | 内部 stream-server | 处理 |
|---|---|---|---|
| 路径 | `/v1/chat/completions` | `/openapi/chat/completions` | `ZAGENT_BASE_URL` 填到 `/openapi` |
| 私有 header | 无 | **`Wsid` 必填** | `ZAGENT_EXTRA_HEADERS` |
| `stream` 默认 | `false` | **`true`** | 客户端已无条件显式写 `stream: false` |
| 长度字段 | `max_tokens` | `output_seq_len` | `ZAGENT_EXTRA_BODY` |
| 端口 | 443 | idc 81 / devcloud 1081 / **办公网 8080** | 按网络环境选 |

**`stream` 那条最阴**：OpenAI 默认 false，内网默认 true。不显式写这个字段，
拿回来的是 SSE 流（`data: {...}` 一片），而客户端按非流式 JSON 解析，
直接 `JSONDecodeError` —— 报错信息完全看不出是流式开关的锅。已无条件
写死 `stream: false`（两边都正确），并有测试守着。

**还有一条文档明说的坑**：这套实现里 `temperature=0` 会有问题（底层 C++
除零有奇怪效果）。要 greedy search 得用 `top_k=1, top_p=0`。
所以 `ZAGENT_TEMPERATURE` 可以覆盖默认值 0.0。

```bash
set ZAGENT_BASE_URL=http://<ip>:8080/openapi
set ZAGENT_API_KEY=<你的 token>
set ZAGENT_API_KEY_ENV=ZAGENT_API_KEY
set ZAGENT_MODEL=<模型服务组名>
set ZAGENT_EXTRA_HEADERS=Wsid=10103
set ZAGENT_EXTRA_BODY=openai_infer=true;output_seq_len=2048
python -m examples.llm_check
```

`ZAGENT_EXTRA_HEADERS` / `ZAGENT_EXTRA_BODY` 同时收 JSON 和 `k=v;k=v`
简写。推荐简写：JSON 在 PowerShell 里引号套引号极易写错，而写错是
**静默**的（拼错 -> 空 dict -> header 没发出去 -> 服务端 403 ->
人以为自己没权限），会白查半小时。

服务发现走北极星（`stream-server-online-sbs-{wsid}`），不要固定 IP ——
文档明确说固定同一 IP 会导致单实例过载。

**待确认**：这份文档的参数表里**没有 `tools` 字段**，也就是说 function
calling 可能不支持。不支持的话 agent 会回退到"从文本里抠 JSON"，能用但
更脆。`llm_check` 的第二步就是专门探这个的，跑一下就知道。

真 transport 用**本地 mock HTTP 服务**做了回归（`tests/test_llm_real.py`）：
鉴权头、payload 形状、4xx 不重试 / 5xx 重试、响应异常抛 `LLMError`。
这段之前是零覆盖 —— 第一次执行它就是在有人指向真实端点的那一刻，
那是发现 bug 的最糟时机。

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
- [x] `Planner` 的 LLM 实现：零第三方依赖（标准库 urllib）+ 可注入 transport
- [x] 把 `diagnose()` 整体降级为 loop 可调的一个工具
- [x] 交互式 CLI（斜杠命令、审批、中途插话、逐轮打印）
- [x] agent 级 eval：与流水线共用 `score()`，已跑对照实验（见 7.8）
- [x] `Verifier` 暴露为工具：模型决定何时验证，规则决定怎样算验证过
- [x] 结论硬门禁：装在 loop 上，`finish` 处拦截没有 CONFIRMED 支撑的结论
- [ ] 用真 LLM 跑对照实验，验证门禁是否真的压住了幻觉（对比开关门禁的 honesty）
- [ ] 上下文压缩（轨迹变长后的摘要/裁剪）
- [ ] 把工具层接到 MCP，让外部 agent 也能用这套能力
- [ ] 接入更多能力工具（编译日志、实验台跑批），验证"加能力不用改结构"
- [ ] 用真 LLM 跑对照实验，记录 top1 掉了多少并做归因 ← 见 7.8
- [ ] 用 `zlab` 批量注入故障，把对照实验的样本量堆到数百个

---

## 十、快速开始

```bash
cd agentkit
python -m examples.minimal_run
```

不需要真实 dump、不需要引擎编译，跑的是一个合成的崩溃样本，完整走完门禁 → 取事实 → 版本对齐 → 符号映射 → 假设验证 → 报告。

### 交互 agent（新入口）

```bash
python -m zagent.cli                     # 进入交互，/help 看命令
python -m zagent.cli --task "..."        # 单次执行后退出，CI 用
python -m examples.agent_demo            # 不需要 LLM 的机制演示
python -m examples.agent_llm_demo        # 有 key 时真跑，无 key 时演示
python -m examples.llm_check             # 接入自检：探 function calling
```

崩溃诊断在这个 CLI 里是一个**注册进来的能力**，不是全部：

```bash
python -m zagent.cli --root <repo> --dump <crash.json> --task "定位这次崩溃"
```

### eval 对照

```bash
python -m examples.run_eval       # 流水线基线
python -m examples.run_eval_agent # 流水线 vs agent，同一把尺子
python -m examples.run_eval_gate  # 结论门禁对照（模拟幻觉模型）
```

---

## 十一、与内部项目的关系

`agentkit/` 里的框架代码与任何闭源工程无耦合。闭源工程提供的是**领域规则**（Verifier 里的判断逻辑），那些不进这个仓库。

对外讲的是框架和 eval；领域知识是个人在具体岗位上的溢价，不需要开源。
