# zlab - 故障注入实验台

把 zfault 的故障样本接成 zagent 流水线的**真实**输入。

## 它解决什么问题

到此为止，zagent 跑的是合成 JSON —— 栈帧由人手填进去。
那只能证明流水线不崩，不能证明它有用，因为 ground truth 是人给的，属于自证。

zlab 反过来：

    注入缺陷 -> 编译 -> 真的崩 -> 抓崩溃现场 -> 真的符号化 -> 喂给流水线

每一步都是实测。ground truth 来自「我注入的是哪一行」，与流水线完全无关。

## 三个模块

### probe.py - 崩溃现场探测器

注入到故障源码里，在崩溃瞬间抓取指令地址与调用链。

**五条铁律，每一条都是实测踩出来的：**

1. handler 必须返回 `EXCEPTION_CONTINUE_SEARCH`，让进程真的崩。
   吞掉异常会让样本失效，而且这种失效很隐蔽 —— 程序正常退出，
   只有 eval 时才发现所有样本都没崩。
2. 用 `RtlVirtualUnwind` 手写展开，**不能用 dbghelp**。
   最初用 `StackWalk64 + SymInitialize`，oob_write 超时（60 秒不返回）、
   stack_overflow 无任何输出。根因是 `SymInitialize` 在崩溃上下文里
   加载符号会死锁 —— 栈溢出时栈已耗尽，没有余量给符号加载。
   教训：**崩溃 handler 里不能做任何可能分配内存或加载文件的操作**。
3. 不能用 `RtlCaptureStackBackTrace`。在 VEH handler 里它抓的是
   异常分发路径自己的栈，第一帧永远是 handler 自身，穿透不到崩溃点。
4. 栈回溯不可全信。越界写会污染栈 —— 实测第 3 帧之后全是
   `0xabababababababab`，正是我们的填充字节。
5. **检测点 ≠ 破坏点**。这是本模块存在的全部意义。

### symbols.py - 符号化与地址归一化

用 Visual Studio 自带的 `llvm-symbolizer` 解析 MSVC 的 PDB。

**ASLR 归一化是这里最大的坑。** 运行时地址带随机基址，
PDB 记录的是静态 preferred base。直接拿运行时地址去符号化**不会报错**，
它会返回一个完全无关的函数名（实测返回 `__guard_memcpy_fptr`，行号 0）。

这种假结果比直接失败危险得多 —— 它看起来是解析成功的。
所以：`addr - runtime_base + static_base`，且地址不在自身模块内时
直接返回 None。

### lab.py - 实验台

串起 编译 -> 崩溃 -> 符号化 -> 诊断。

**ground truth 必须基于注入后的源码重算。** 探测器注入会插入几十行，
导致根因行整体下移。若仍用原始 `pattern.root_cause_line()`，
eval 会稳定报「定位错误」—— 但定位其实是对的，只是标签过期。
这类假失败会诱导你去修一个根本没坏的模块。



## 当前基线（连续三次运行完全一致）

```
样本                类别              期望  定位  偏差  帧数  结果
null_deref_01      null_deref       442   442    0     3    HIT
oob_write_01       oob_write        458   458    0     3    HIT
uaf_01             use_after_free   448   448    0     0    HIT
double_free_01     double_free      446   446    0     3    HIT
overflow_01        overflow         431     -    -     0    MISS
stack_overflow_01  stack_overflow   446   446    0    34    HIT
race_01            race             432   432    0     0    HIT
leak_01            leak             432   432    0     0    HIT

成功崩溃      5/8
能定位到行    7/8   (88%)
命中根因行    7/8   (88%)
命中根因函数  6/8   (75%)
定位偏差中位数 0
```

七个样本精确命中，偏差为 0。剩下唯一的负样本是 `overflow`（纯整数溢出，
本就不崩），它是有意保留的对照。

## 基线的每一行都必须有 CONFIRMED 结论

**命中不等于证实。** 这是本轮最重要的一条。

基线表只统计"定位行 == 期望行"，它分不清两种情况：
- 探测到证据，推理层**证实**了它；
- 运行时证据恰好指对了行号，验证层**根本没参与**。

后者是虚命中，换个样本就会露馅。实测补规则之前：

```
stack_overflow_01  446  446  置信度 0.0   结论数 0     <- 虚命中
race_01            432  432  置信度 0.15  结论数 1(unproven)  <- 虚命中
```

两个都"命中"了，但报告里没有任何 CONFIRMED 结论。
所以现在逐样本核对 `conclusions` 里的证实类型：

```
样本                定位  置信度  证实类型
null_deref_01       442   0.7    nullcheck
oob_write_01        458   0.6    unsigned_underflow
uaf_01              448   0.6    nullcheck
double_free_01      446   0.6    double_free
stack_overflow_01   446   0.7    recursion
race_01             432   0.6    race
leak_01             432   0.6    leak
overflow_01          -     -     （无，正确克制）
```

**七个 HIT 全部有对应的证实结论。** 这才是能拿出去的数字。

## 本轮：RaceRule 与 RecursionRule

### RaceRule

判据三条同时成立才 CONFIRMED，缺一条即 REFUTED：
1. 该行对一个变量做**读改写**（`++`/`--`/`+=`/`x = x + ...`）。
   单纯的赋写不构成竞争 —— 成因正是"读-改-写"之间能插入别的线程。
2. 该变量是**文件作用域**共享变量，且所在函数被**多线程入口**触及。
3. 访问路径上**没有同步原语**（mutex / lock_guard / atomic）。

不做 happens-before（TSan 那套要维护向量时钟、拦截每次同步操作）。
定位只需要"是不是无保护的共享读改写"这一个判断，不需要穷举竞争对。

触发不能用 `exception_type` 门控 —— 竞争不崩溃、栈也是空的，
与 leak 同理。门控用"文件里存在线程入口"而不是"include 了 <thread>"：
后者太宽，单线程程序也可能 include 却从不启动。

### RecursionRule

判据两条：
1. 该行调用了**所在函数自身**；
2. 终止条件**依赖函数参数**，而不是编译期常量上限。

第 2 条是关键。`if (depth >= kMaxDepth)` 一定会停，
`if (limit >= 0 && depth >= limit)` 则取决于 main 传什么（实测传 -1）。

### 两个规则各自踩到的坑

**注释里的伪代码被当成了真代码。** stack_overflow_01 的注释写着
「最初写成 `if (depth > limit) return 0;`」—— 那是一段**已被弃用的代码**，
却长得和真代码一模一样。不剥注释的话，证据会指到注释行（439），
判据本身建立在一说明文字上。加了 `_strip_comment` 后才指向真实的 443 行。

**自递归的引入点就在原地。** `_refine_location` 只采纳更早的行
（引入点必然在崩溃点之前），但无限递归没有"更早的引入点" ——
根因行就是那条递归调用本身。沿用旧规则会把定位从 446 推到函数定义行 430，
**把本来对的答案改错**。现在 race 与 recursion 两类跳过精化：
它们的规则只负责**证实**当前位置，不负责移动它。

这是第三种数据流方向。之前已有两种：崩溃类取更早（先算错再崩）、
泄漏类取更晚（先分配再漏掉）。递归是哪都不去。

## 上一轮：拿下 race（探测侧）与 leak

### race 比泄漏更隐蔽

泄漏至少还留下"没还的块"这个实体，race 连这个都没有 ——
两个线程并发写同一个变量，谁都没做错什么事，结果只是偶尔不对。
实测 race_01 稳定返回 rc=1（竞争确实在发生），
但栈是空的、没有异常、没有任何地址可供定位。

**检测：让共享变量所在的页变成"会报警的页"。** 把变量放进单独一页并加上
`PAGE_GUARD`：任何访问都会触发 `STATUS_GUARD_PAGE_VIOLATION`，
异常信息里带着**触发访问的指令地址** —— 那正是 `g_counter++` 编译出来的
那条读/写指令。于是"哪个地址在被并发访问"和"在哪一行"同时拿到了。

`PAGE_GUARD` 的关键性质：触发一次后系统**自动清除** guard 标志，
handler 里必须重新加回去，否则只能捕获第一次访问。
也正因为会自动放行，不需要单步（Trap Flag）—— 比手工单步稳健得多。

**性能：抓到证据后必须停止观测。** 每次 guard 命中都要进内核再回用户态，
race_01 是 4 线程 × 20 万次累加 = 80 万次访问，一路武装下去要跑几十秒。
而竞争通常在前几百次访问内就暴露了，拿到 `conflict_ra` 之后继续观测
没有额外信息、只有额外开销。

**只认文件作用域的全局变量。** 栈上局部变量跨不了线程，
放进 guard 页只会让每次访问都进异常。这是测试逼出来的：
最初的正只看"行首缩进后的类型声明"，把 `long local_x = 0;` 也监视了。
现在用大括号深度区分，depth==0 才算文件作用域。

race_01 五次运行观测到的线程数在 2~4 之间波动，但定位行**恒定**是 432。
判定竞争只需要"至少两个线程"，不需要等四个都到齐。

### leak：分配点不是根因行

leak 的 main 写的是 `BuildContext(&ctx, argc > 1 ? 1 : 0)`。
**不带参数运行时 failAt=0，走成功路径，一次都不泄漏。**
不传触发输入，泄漏检测做得再对也只能报告"零泄漏"，
而这个"零"会被当成程序没问题的证据。所以实验台显式给出触发输入。

检测在**程序正常退出时**清点记录表，凡未释放的块就是泄漏，
且直接持有分配点。用**全局对象析构**而非 atexit：静态对象析构明确发生在
main 之后、堆被拆除之前；且**崩溃的进程不执行静态析构**，
所以 UAF / 重复释放完全不受影响。

检测给出的是分配点（430 行），但根因在 `return -1;`（432 行）——
分配本身没错，错的是"有一条路径没释放就 return"。差 2 行，
而命中判定严格行相等、零容差。所以加了 `LeakRule`，且泄漏的精化方向
**取更晚**（数据流方向与崩溃类相反）。

### 顺带修掉一个虚高：`return -1` 不是崩溃

Windows 上 `return -1` 的退出码是 0xFFFFFFFF，落在"退出码很大"的范围里
被当成崩溃。真正的崩溃以 NTSTATUS 形式返回（高 4 位为 0xC）。
修正让"成功崩溃"从 6/8 降到 5/8 —— 下降的是虚高的部分。

## 再上一轮：让 UAF 从"不可观测"变成"可定位"

那一轮的边界是「栈之外没有运行时信息」。攻下 UAF 用的是**运行时设施**：
探测器接管堆。

**第一步：隔离分配。** 每次分配独占若干页，尾部再留一页不可访问的隔离带。
**第二步：释放后置为 PAGE_NOACCESS。** 光 hook malloc/free 记录地址不够 ——
释放后的内存通常**仍然可读**，悬垂访问静默返回垃圾值，
UAF 因此永远是负样本（实测 uaf_01 跑出来 crashed=False）。
**第三步：崩溃时匹配已释放块。**

### 连带后果都要处理

**接管分配器会让 CRT 的重复释放校验失效。** 所以 ZLabFree 必须自己校验，
发现重复释放就主动崩溃并直接 printf 调用点地址 —— 不依赖栈。

**注入改变了调用名，规则必须跟上。** DoubleFreeRule 原来只认 `std::free(`，
注入后源码里是 `ZLabFree(buf)`，规则因此被 REFUTED。

### 实现上的坑

**`_ReturnAddress()` 必须在函数体内直接调用。** 最初用
RtlCaptureContext + RtlVirtualUnwind 展开固定层数回业务代码：
展开 2 层落在 ZLabFree 内部，改 3 层仍不对。
根因是层数依赖编译器的内联、帧指针与尾调用决策。
换成 `_ReturnAddress()` 才彻底消除这层脆弱性 ——
但**多包一层函数就失效**。

**`std::free` 返回 void，不能包成实参。** 用逗号表达式达成"先登记再释放"。
**替换必须做括号配平**，朴素替换会在 `sizeof(GameObject)` 上少一个右括号。
另外替换范围是**探测代码之后**，不是 main 之后 —— 业务函数常在 main 之前。

## 仍然做不到什么

`overflow` 是纯整数溢出，无异常无栈，本就不崩。它是有意保留的负样本 ——
一个从不失手的检测器，通常是把难题藏起来了。
不过要分清楚：它现在的"不给结论"是探测不到时的兜底，不是真的识别出了溢出。

## 环境噪声（会让基线失真，必须知道）

防病毒对系统 Temp 目录下未知 exe 的实时扫描会：
挂住进程（30 秒不崩溃也不退出），表现为假的"未崩溃"；
锁住文件句柄，导致 `TemporaryDirectory` 清理时 PermissionError；
让第二次运行走**不同的崩溃路径**。

所以工具链探测、实验台工作目录、测试构建目录**全部固定在工程内**
`_lab_work`，不用 `tempfile.mkdtemp()`。
注意 `examples/run_eval.py` 目前仍在用系统 Temp 且只跑 4 个合成样本，
与这里的 8 样本基线是两套互不相通的评估。

## eval 已接到真实样本上

**此前有两套互不相通的评估。** `examples/run_eval.py` 跑 4 个合成样本
（源码是 `minimal_run.py` 现编的假仓库，崩溃是凭空造的 JSON，栈帧是填的数字），
与 zlab 这 8 个真实编译、真实崩溃、真实符号化的样本毫无关系。
README 里的"7/8"和"eval 通路 100%"是两个各自好看、谁都没覆盖对方的数字。

新增 `examples/run_eval_lab.py`，把 zlab 的 8 个样本接进 eval 通路：

```
  top1            100.00%
  top3            100.00%
  honesty         100.00%   <- 负样本上的克制率，最关键
  evidence        100.00%
  blocked         0
  stability       100.00%
```

跑法：`python -m examples.run_eval_lab`

### 三个坑，都不是靠想能做对的

**一、负样本要动态标注。** `overflow_01` 在 patterns 里是正样本
（有 ground truth 431），但它是纯整数溢出：不崩、无栈、无异常，
流水线走降级路径、report 为 None —— 这是**真实且正确的行为**。
硬当正样本，它只是个永恒的 MISS，而 honesty 指标会因为
没有负样本而失去意义。所以按"是否拿到运行时证据"动态标注：
拿不到证据 == 负样本，正确行为就是克制。

**二、ground truth 的路径必须取自报告侧。** `score()` 用 `_normalize`
比对，它取路径最后两级（如 `null_deref_01/f.cpp:442`）。
最初我填 `"f.cpp"`，归一化后与报告侧尾部不匹配，
**7 个本来全对的样本被判成"定位错误"，top1 直接归零**。
这正是 `_normalize` 注释里警告的那类假失败。

但**行号绝不能取自报告** —— 那是用报告自己的答案当期望值，
永远 100%，比上面的假失败更隐蔽。ground truth 必须由
注入时写在源码里的根因标记独立算出（`run.expected_line`）。
只有路径取自报告，目的仅是对齐比对格式。

**三、`propose` 与 `report.location` 口径不一致（管线的真 bug）。**
实测 stack_overflow_01 出现「report.location=446 判定 HIT，
但 eval 判定位错误」——同一个样本两个相反结论。

根因：
- `report.location` 走 `_pick_location`，对递归栈用**众数投票**
  选出递归调用点（446）；
- `propose` 拿原始 `locations`，取 `locations[0]`，那是栈帧 0 的
  **函数入口**（430）。

`_pick_location` 的注释明确写了"不能无脑取 locations[0]"，
propose 却恰恰那么做了。而 `Report.top()` 取的是**证据里的 locator**，
eval 用它算 top1 —— 所以它看到 430，与 ground truth 446 不符。

修法是让假设建立在最终定位上。这也是为什么修完之后
stability 从偶发的 87.5% 变成稳定的 100%。

### Lab 的默认目录不再是系统 Temp

`Lab.__init__` 原先默认 `tempfile.mkdtemp(prefix="zlab_")`。
那是 README 点名的防病毒噪声源 —— 任何一处忘了显式传 workdir 的
调用都会把噪声引进来。现在默认落在工程内 `_lab_work`。


## overflow 不再是"克制"，而是真的被识别了

**此前把它解释错了。** overflow_01 长期走降级路径、report 为 None，
当时把这种表现记成"探测不到就克制，是正确的"。
但 detect_hint 明明白白写着根因在乘法 —— 那是把能力不足包装成了美德。

真正的根因有两个，缺一不可。

**一、样本写错了。** 原式是 `(size_t)count * sizeof(BigEntry)`：
count 是 int（上限 2^31），先提升到 64 位 size_t 再乘，
乘积上限约 2^39 —— 在 64 位下**永远不可能溢出**。
实测结果是申请 2GB 失败、malloc 返回 NULL、走 `return -1` 退出，
**连循环都没进**。一个"应该崩"的样本安静地返回了 -1。

真实漏洞的形态是**窄宽度乘法先溢出，再赋给宽类型**：

```
unsigned int bytes32 = (unsigned int)count * (unsigned int)sizeof(BigEntry);
```

count = 16777220 时，32 位乘积回绕成 1024（仅 4 个条目），
而循环要写 1677 万个 —— malloc 成功返回，然后必然写穿。

**二、没有够得着根因的运行时证据。** 崩溃在 `memset` 的循环写入，
栈只能指到那里，够不到分配点的乘法。

于是给探测器加了"写穿活跃块"的观测：故障地址落在块尾隔离页时，
输出 `ZLAB_HIT_ALLOC_RA`（该块的**分配点**返回地址）。
符号化后落在乘法所在的函数体内，再交给新增的 `OverflowRule` 证实。

判据三条同时成立才 CONFIRMED：窄宽度整型乘法 + 乘数含 sizeof +
结果作为分配大小。**宽度是关键**：`(size_t)count * sizeof(T)` 先提升再乘，
必须被推翻 —— 否则规则会在所有正常分配上误报。

现在 overflow_01 从 MISS 变成 HIT，定位 464 精确命中，置信度 0.6。

## 补了一个"没有缺陷"的样本

overflow 修好之后，8 个样本**全部成了正样本**，负样本归零 ——
honesty 这个最关键的指标因除零变成 0%，失去意义。

这暴露一个更本质的问题：只测"能不能找到问题"的评估，
会奖励一个对所有输入都报"找到根因"的 agent ——
它在全正样本的集子里能拿满分。

所以新增 `clean_01`：一段确实没有缺陷的代码，
**刻意带上乘法与分配**（与 overflow 同形），
但用 size_t、宽度正确。规则若漏掉宽度判据，就会在它上面误报。

**测试不能只在"规则恰好没被触发"时才通过。**
第一版 clean 测试实测是空转的：把宽度判据整个删掉，它照样通过 ——
因为 clean_01 走降级路径，规则根本没被调用。
现在改成直接把干净代码喂给 OverflowRule，绕过运行时的偶然性；
删掉宽度判据后它会立刻变红。

## oob_write 的偏差是我上一轮自己引入的回归

修完 overflow 后，基线里 `oob_write_01` 仍偏差 8 行。
当时把它当成"既有状况"，但**它是我上一轮改出来的**。

我给探测器加"写穿活跃块"观测时，把分配点插在了崩溃地址**之前**，
理由是"截断时不会丢"。但 `_pick_location` 在非递归栈下
直接返回 `locations[0]` —— 于是分配点顶替崩溃点成了报告定位。

实测：根因是 479 行的 `len = r.end - r.start` 下溢，
却定位到 487 行的 `ZLabAlloc(64)`。
在基线里它只表现为"少一个 HIT"，很容易被当成老问题放过。

**根子是位置顺序承载了语义。** 谁排前面谁就是 locations[0]，
而"崩溃点"和"分配点"本不该同等竞争 ——
前者是实测事实，后者只是推断线索。

修法是给 `SourceLocation` 加 `role`：
只有 `crash` 帧有资格当 locations[0]，
分配点标 `alloc`，照常参与验证与 refine，但不抢占定位。
`OverflowRule` 改为用分配点位置触发（乘法在 AllocateTable，
崩溃在 FillTable 的 memset，是**两个不同函数**，
拿崩溃点去界定函数体根本看不到乘法）。

修完 oob_write_01 从 487 修正到 479，且拿到了
`unsigned_underflow` 的 CONFIRMED 结论（此前只有一条 UNPROVEN 的 nullcheck）。

**位置顺序不该承载语义** —— `test_alloc_site_does_not_override_crash_site`
守的就是这件事。实测它有效：把 role 过滤改回 `list(locations)`，
该测试立刻变红。

## 运行

```bash
cd agentkit
python examples/run_baseline.py        # 9 个真实样本，输出明细到 baseline.json
python -m examples.run_eval_lab        # 同上，但走 eval 通路并给出指标
python -m pytest tests -q              # 102 项测试
```

两套跑的是同一批样本，从此任何一个数字都由同一批真实崩溃支撑。

注意：基线分母是 9（含 clean_01 这个负样本），
"命中根因行 7/9"里的 7 与旧版 8 样本时的 7 是同一批命中，
百分比下降只是分母变化，不是回归。

## 测试

`tests/test_lab.py` 跑真实的编译、崩溃、栈展开与符号化，不 mock。
实验台的价值全在于「真实崩溃能被正确定位」，mock 掉等于没测。

- `test_overflow_is_confirmed_not_degraded` —— overflow 必须有 overflow 类型的
  CONFIRMED 结论，不能靠降级兜过去。
- `test_clean_code_is_not_flagged` —— 干净代码不能被报出根因。
  直接测规则，不依赖运行时是否恰好触发。
- `test_no_sample_is_hit_without_confirmed_conclusion` —— 守的是整套评估的
  可信度：任何 HIT 都必须附带 CONFIRMED 结论。它把"虚命中"从
  一个观察变成了断言。
- `test_evidence_locator_matches_report_location` —— 证据位置必须等于报告
  定位。上面第三个坑的回归测试。
- `test_lab_default_workdir_is_not_system_temp` —— 默认目录不能污染噪声。
- `test_race_is_confirmed_not_just_coincidentally_right` —— race_01 必须有
  race 类型的 CONFIRMED 结论。它曾经虚命中（置信度 0.15）。
- `test_stack_overflow_is_confirmed_not_just_coincidentally_right` —— 同上，
  它曾经零结论、置信度 0.0。
- `test_new_rules_are_falsifiable_on_innocent_code` —— 两条新规则都必须
  能被推翻。只会被证实的规则等于没有规则。
- `test_recursion_rule_ignores_conditions_in_comments` —— 注释里的弃用伪代码
  不能被当成真实终止条件。
- `test_alloc_site_does_not_override_crash_site` —— 分配点不能顶替崩溃点。位置顺序不该承载语义，role 才是显式约定。
- `test_uaf_becomes_observable` —— UAF **必须**崩。隔离分配一旦失效，
  它会静默退化成负样本，在基线里只表现为"少一个 HIT"。
- `test_exit_code_minus_one_is_not_a_crash` —— 守基线数字的诚实性。
- `test_race_watch_does_not_touch_locals` —— 守性能。栈上局部变量不该监视。

