# zfault - 故障注入实验室

给 ZEngine 的 agent 套件提供**带精确标签的崩溃样本**。

## 它解决什么问题

agent 工程里最难的不是让 agent 跑起来，是证明它**在什么情况下会错**。
要做到这一点，需要一批「已知根因」的样本，但真实崩溃的根因要靠人去定位，
成本高且本身就可能判错。

zfault 反过来做：**先注入已知缺陷，再让它崩溃**。
这样 ground truth 是免费、精确、可无限复现的。

    常规做法：  崩溃 -> 人肉定位 -> 得到根因（可能判错）-> 作为标签
    zfault：    注入缺陷 -> 崩溃 -> 根因已知（就是注入的那行）

## 三个模块

### patterns.py - 故障模式库

8 条模式，每条都标注了**根因行**、**期望判定类别**，以及最关键的 detect_hint：
说明「agent 应该在哪里找到答案，陷阱在哪里」。

| 模式 | 类别 | 是否必崩 |
|---|---|---|
| null_deref_01 | 空指针解引用 | 是 |
| oob_write_01 | size_t 下溢导致写越界 | 是 |
| uaf_01 | 释放后使用 | 否（内存未被复用时读到旧值） |
| double_free_01 | 重复释放 | 是 |
| overflow_01 | 乘法溢出导致分配过小 | 是 |
| stack_overflow_01 | 无限递归 | 是 |
| race_01 | 数据竞争 | 否（结果错误，退出码 1） |
| leak_01 | 内存泄漏 | 否（不崩溃） |

后两条是**负样本**。必须有它们，如果样本集里全是崩溃，
agent 学会的只是「找崩溃」，而不是「找缺陷」。

### toolchain.py - 工具链探测

**探测而不是假设**。

早期版本看到 which g++ 存在就直接用，结果它指向 Strawberry Perl 附带的
g++ 4.7.3（C++11 都不完整）。教训：编译器存在不等于可用，必须实际编译探针验证。

MSVC 的处理尤其麻烦：cl 不在 PATH，且必须经 vcvarsall x64 拿到
INCLUDE/LIB 才能编译。踩过的坑见源码注释。

### process.py - 崩溃进程执行器

**为什么需要它**：直接 subprocess.run(崩溃程序) 会卡死。
进程崩溃后 Windows 错误报告（WER）弹「应用程序错误」对话框，
框不关，进程不退出，父进程无限等待。

解法是 SetErrorMode(SEM_NOGPFAULTERRORBOX)，error mode 会被子进程继承，
父进程设一次就够。**不用改注册表**：那会影响用户整台机器的行为，副作用外溢；
继承 error mode 的作用域仅限本进程树，退出即失效。

## 使用

    from zfault.patterns import get
    from zfault.toolchain import detect
    from zfault import process as zproc

    tc = detect()
    assert tc.supported

    p = get("null_deref_01")
    # ... 编译 p.source ...
    r = zproc.run([exe], timeout=30, env=tc.build_env)
    print(r.signal_desc())   # "访问违例 (STATUS_ACCESS_VIOLATION)"
    print(r.crashed())       # True

## 测试

    cd agentkit
    python -m pytest tests -q

test_patterns.py 会**真实编译并运行每一条故障模式**，不 mock。
无编译器时自动跳过，不会因为 CI 没装 VS 就让构建变红。

其中 test_pattern_behaves_as_expected 是最重要的一组：
它保证「声明为必崩」的模式真的会崩。如果它挂了，
通常意味着注入的缺陷被编译器优化掉了，或者触发条件写错，两者都必须修。

## 开发中踩到的坑（值得记住）

写故障模式时，最容易出现的是**注入的缺陷根本没有触发**，程序正常退出，
但代码注释里还写着「这里会崩」。全部靠跑出来验证，不能靠读代码判断：

1. oob_write 加了 `if (len > dstCap) return;` 作"防御"，
   下溢后 len 极大反而触发了 return，于是**根本不崩**。
2. stack_overflow 终止条件写成 `depth > limit`，配合 limit=-1 时
   `0 > -1` 恒真，函数第一次调用就返回，**一次都没递归**。
3. 同一模式下 `char local[64]` 太小，递归几十万层都不崩，验证脚本挂住。
4. 开优化（/O2）时尾递归被转成循环，栈溢出模式直接失效。

前两条都属于「注释说会崩，实际不会」，只有跑才能发现。
这也是 test_pattern_behaves_as_expected 存在的理由。
