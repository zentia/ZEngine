"""
实验台的验收测试。

这些测试跑真实的编译、崩溃、栈展开与符号化，不 mock。
实验台的价值全在于「真实崩溃能被正确定位」，mock 掉等于没测。

无编译器 / 无符号化工具时整组跳过（skip 而非 fail）。
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zfault.patterns import PATTERNS, all_patterns
from zfault.toolchain import detect
from zlab.lab import Lab
from zlab.probe import inject_probe, parse_probe_output
from zlab.symbols import MsSymbolResolver

_TC = detect()
_NEEDS_CXX = pytest.mark.skipif(
    not _TC.supported, reason=f"无可用 C++ 编译器: {'; '.join(_TC.notes)}")


def _make_lab():
    """
    用工程内的固定工作目录，不用 tempfile.mkdtemp()。

    实测：系统 Temp 目录下的 exe 首次运行会被防病毒实时扫描挂住，
    第二次运行的崩溃路径还可能不同（oob_write 在 Temp 下触发的是
    STATUS_STACK_BUFFER_OVERRUN 而非访问违例 —— 栈 cookie 检查会
    直接终止进程，不给栈展开机会）。这类环境噪声会让测试随机失败。

    下方所有 build 也必须用 _build_dir()，不能各用各的临时目录，
    否则同一个测试里 lab 和 build 落在不同位置，行为不一致。
    """
    repo_root = Path(__file__).resolve().parent.parent
    wd = repo_root / "_lab_work"
    wd.mkdir(parents=True, exist_ok=True)
    return Lab(workdir=wd)


def _build_dir(pattern_id: str) -> Path:
    """每个模式一个稳定的构建目录（与基线脚本保持一致）。"""
    repo_root = Path(__file__).resolve().parent.parent
    d = repo_root / "_lab_work" / f"t_{pattern_id}"
    d.mkdir(parents=True, exist_ok=True)
    return d


# ------------------------------------------------------- 纯静态检查

def test_probe_injection_keeps_root_marker():
    """注入探测器后，根因标记必须还在 —— 否则 ground truth 无从计算。"""
    for p in all_patterns():
        injected = inject_probe(p.source)
        assert p.root_cause_line_marker in injected, f"{p.id} 注入后丢失根因标记"


def test_probe_injection_installs_handler():
    """探测器必须真的被安装到 main 里，否则运行时不会有输出。"""
    for p in all_patterns():
        injected = inject_probe(p.source)
        assert "AddVectoredExceptionHandler" in injected, f"{p.id} 未安装 handler"


def test_probe_injection_preserves_crash_semantics():
    """
    回归测试：handler 必须返回 CONTINUE_SEARCH，让进程真的崩。

    如果返回 CONTINUE_EXECUTION 把异常吞掉，样本就不崩了，
    整个故障注入失去意义 —— 而且这种失效很隐蔽，
    表面看程序正常运行退出，只有 eval 时才发现全部样本都没崩。
    """
    for p in all_patterns():
        injected = inject_probe(p.source)
        assert "EXCEPTION_CONTINUE_SEARCH" in injected


def test_probe_output_parser_handles_missing():
    """缺字段返回 None，不填默认值。"""
    out = parse_probe_output("")
    assert out["ip"] is None and out["base"] is None and out["frames"] == []


def test_probe_output_parser_parses_all():
    text = ("ZLAB_CRASHIP=7ff700001234\nZLAB_BASE=7ff700000000\n"
            "ZLAB_CODE=c0000005\nZLAB_FRAME0=7ff700001999\n"
            "ZLAB_FRAME1=7ff700001aaa\n")
    out = parse_probe_output(text)
    assert out["ip"] == 0x7FF700001234
    assert out["base"] == 0x7FF700000000
    assert out["code"] == 0xC0000005
    assert [a for _, a in out["frames"]] == [0x7FF700001999, 0x7FF700001AAA]


# ------------------------------------------------------- 符号化

def test_normalize_rejects_out_of_module():
    """
    回归测试：模块外地址不得归一化。

    这是本项目最重要的一个陷阱。堆破坏在 ntdll 里被检测到，
    崩溃 IP 属于系统 DLL，与自身 base 不同模块。
    套用归一化公式会算出无意义地址（实测 0x3728c7eb5），
    符号化后返回一个毫不相干的函数名 —— 看起来解析成功了，实际全错。
    """
    r = MsSymbolResolver(Path("dummy.exe"))
    r._static_base = 0x140000000
    assert r.normalize(0x7FF9C13D7EB5, 0x7FF78EB10000) is None


def test_normalize_returns_none_without_base():
    """拿不到基址就不归一化，不假装。"""
    r = MsSymbolResolver(Path("dummy.exe"))
    r._static_base = 0x140000000
    assert r.normalize(0x140001234, None) is None


def test_normalize_within_module():
    r = MsSymbolResolver(Path("dummy.exe"))
    r._static_base = 0x140000000
    # runtime_base 0x7ff700000000, addr 偏移 0x1234
    assert r.normalize(0x7FF700001234, 0x7FF700000000) == 0x140001234


def test_parse_preferred_base():
    assert MsSymbolResolver.parse_preferred_base(
        "Preferred load address is 0000000140000000") == 0x140000000
    assert MsSymbolResolver.parse_preferred_base("nothing here") is None


# ------------------------------------------------------- 需要编译器

@_NEEDS_CXX
@pytest.mark.parametrize("pid", sorted(PATTERNS.keys()))
def test_injected_pattern_still_compiles(pid):
    """注入探测器后仍必须能编译 —— 注入不能产出坏代码。"""
    p = PATTERNS[pid]
    lab = _make_lab()
    d = _build_dir(p.id)
    ok, exe, err = lab.build(p, d)
    assert ok, f"{pid} 注入后编译失败: {err[:400]}"


@_NEEDS_CXX
def test_crash_capture_yields_normalized_address():
    """
    端到端：崩溃要能拿到归一化后的地址。

    归一化前是运行时地址（含 ASLR 随机基址），
    直接符号化会得到一个完全无关的函数名且不报错 ——
    这种假成功比失败危险得多，所以必须验证归一化确实生效。
    """
    lab = _make_lab()
    p = PATTERNS["null_deref_01"]
    d = _build_dir(p.id)
    ok, exe, err = lab.build(p, d)
    assert ok, err[:300]
    cap = lab.crash(p.id, exe)

    assert cap.crashed, "样本未崩溃"
    assert cap.raw_address is not None, "未捕获崩溃地址"
    assert cap.crash_address is not None, "地址未归一化"
    # 归一化后应落在静态基址空间（0x140000000 附近），而非运行时空间
    assert cap.crash_address < 0x0000000800000000, (
        f"地址 {hex(cap.crash_address)} 不在静态空间，归一化未生效")


@_NEEDS_CXX
def test_stack_unwind_reaches_project_frame():
    """
    栈展开必须穿透到项目代码帧，而不只是停在检测点。

    这是实验台存在的核心理由：越界写崩在 CRT 的 memset、
    栈溢出崩在 chkstk.asm —— 崩溃 IP 都不在项目代码里。
    只靠崩溃 IP 永远定位不到根因。
    """
    lab = _make_lab()
    p = PATTERNS["stack_overflow_01"]
    d = _build_dir(p.id)
    ok, exe, err = lab.build(p, d)
    assert ok, err[:300]
    cap = lab.crash(p.id, exe)

    # 栈溢出能展开出多帧递归
    assert len(cap.frames) > 1, (
        f"只展开出 {len(cap.frames)} 帧，栈展开未穿透")


@_NEEDS_CXX
def test_end_to_end_locate_null_deref():
    """
    端到端定位：null_deref 必须命中注入的根因行。

    ground truth 由 lab 基于注入后的源码重算，
    不是原始 pattern 的行号 —— 探测器注入会让行号整体下移。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["null_deref_01"])

    assert run.capture.crashed, "样本未崩溃"
    assert run.report is not None, "未产出报告"
    assert run.report.location is not None, "未定位到源码位置"
    assert run.hit, (
        f"定位到 {run.located_line}，根因在 {run.expected_line}")


@_NEEDS_CXX
def test_recursive_stack_picks_loop_frame_not_entry():
    """
    回归测试：递归栈必须选"循环点"而非"函数入口"作为根因行。

    曾经无脑取 locations[0]，结果 stack_overflow 稳定偏差 16 行：
    帧 0 指向函数入口（第 60 行），而 31/34 帧都指向递归调用点（第 76 行）。
    众数投票修复后偏差降为 0。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["stack_overflow_01"])

    assert run.report is not None and run.report.location is not None
    assert run.hit, f"定位到 {run.located_line}，根因在 {run.expected_line}"


@_NEEDS_CXX
def test_recursion_evidence_is_not_deduplicated():
    """
    回归测试：喂给流水线的帧**不能去重**。

    递归栈里同一个地址会重复几十次 —— 那个"重复"就是循环点的证据。
    曾经做了 `if a not in addrs` 去重，34 帧塌成 4 个地址，
    众数投票因此失效（只有 1 票，达不到阈值）。
    """
    lab = _make_lab()
    p = PATTERNS["stack_overflow_01"]
    d = _build_dir(p.id)
    ok, exe, err = lab.build(p, d)
    assert ok, err[:300]
    cap = lab.crash(p.id, exe)

    # 递归样本必须产生足够多的重复帧供投票
    assert len(cap.frames) >= 10, (
        f"只回溯出 {len(cap.frames)} 帧，不足以识别循环点")


@_NEEDS_CXX
def test_odd_return_address_is_accepted():
    """
    回归测试：不得用"地址是否为奇数"判断栈污染。

    x86-64 指令长度可变，返回地址指向"下一条指令"，完全可以是奇数
    （0x7570 处一条 7 字节指令，返回地址就是 0x7577）。
    曾经写了 `if norm % 2 != 0: break`，把 oob_write 的第一帧
    0x140007577 当成污染帧直接 break，导致该样本稳定 0 帧。

    用 double_free 而非 oob_write：double_free 的崩溃路径稳定
    （5 次运行均为 0xC0000374 + 4 帧），且其第 2 帧 0x140007491 是奇数。
    若奇偶判据回归，会在第 2 帧 break，帧数塌成 1 —— 这里断言 >= 2 即可捕获。

    不用 oob_write 是因为它的崩溃路径不确定：既可能是访问违例（有栈），
    也可能触发 GS 栈 cookie 检查（0xC0000409，直接终止，无栈），
    在防病毒首次扫描干扰下更容易走后者。不稳定的样本做不了回归断言。
    """
    lab = _make_lab()
    p = PATTERNS["double_free_01"]
    d = _build_dir(p.id)
    ok, exe, err = lab.build(p, d)
    assert ok, err[:300]
    cap = lab.crash(p.id, exe)

    assert len(cap.frames) >= 2, (
        f"只回溯出 {len(cap.frames)} 帧 —— 若奇偶判据回归，"
        f"会在第 2 个奇数地址处 break，这里会先失败")


# ============================================================ 堆追踪

@_NEEDS_CXX
def test_leak_is_detected_without_crash():
    """
    泄漏**不崩溃**，这是它最容易被漏掉的原因。

    检测靠的是退出时清点未释放的块，而不是等崩溃。
    守的是两件事：一是能报出泄漏，二是**不把它当成崩溃**
    （泄漏程序是正常退出，只是退出码为 -1）。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["leak_01"])

    assert run.capture.leak_count > 0, "应检测到未释放的块"
    assert run.capture.leak_alloc_ras, "应持有泄漏块的分配点"
    # 泄漏不是崩溃：它正常走完 main 并 return -1
    assert not run.capture.crashed, (
        "泄漏程序是正常退出（return -1），不应被判为崩溃")


@_NEEDS_CXX
def test_leak_locates_the_early_return_line():
    """
    泄漏的定位必须落在**早退那一行**，而不是分配点。

    分配本身没错，错的是"有一条路径没释放就 return"。
    两者实测差 2 行，而命中判定是严格行相等 —— 差一行就是 MISS。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["leak_01"])

    assert run.located_line is not None, "leak_01 应能定位"
    assert run.hit, (
        f"应命中早退行（期望 {run.expected_line}，实际 {run.located_line}）")


def test_leak_requires_trigger_input():
    """
    leak_01 不带参数运行时**根本不泄漏**。

    main 写的是 `BuildContext(&ctx, argc > 1 ? 1 : 0)`，
    无参数走成功路径，正常分配正常释放。
    不传触发参数，泄漏检测做得再对也只能报告"零泄漏"，
    而这个"零"会被当成程序没问题的证据。
    """
    from zlab.lab import TRIGGER_ARGV
    assert TRIGGER_ARGV.get("leak_01"), "leak_01 必须有触发输入"


def test_exit_code_minus_one_is_not_a_crash():
    """
    `return -1` 的退出码（0xFFFFFFFF）**不是崩溃**。

    Windows 上它落在"退出码很大"的范围里，容易被当成崩溃。
    实测 leak_01 与 overflow_01 都是这个值，两者都是程序自己
    return -1 正常退出。真正的崩溃以 NTSTATUS 形式返回（高 4 位为 0xC）。

    这条守的是基线数字的诚实性：误判会让"成功崩溃"虚高两个。
    """
    from zlab.lab import CrashCapture

    assert not CrashCapture(returncode=0xFFFFFFFF).crashed, (
        "return -1 是正常退出，不是崩溃")
    assert CrashCapture(returncode=0xC0000005).crashed, (
        "访问违例必须判为崩溃")
    assert CrashCapture(returncode=0xC00000FD).crashed, (
        "栈溢出必须判为崩溃")
    assert not CrashCapture(returncode=1).crashed
    assert not CrashCapture(returncode=None).crashed


def test_leak_rule_refutes_when_no_early_return():
    """
    LeakRule 必须可证伪：没有早退分支就 REFUTED，不硬凑位置。
    """
    from zlab.lab import LabCodeIndex
    from zagent.verify import LeakRule
    from zagent.types import SourceLocation
    from pathlib import Path
    import tempfile

    src = (
        "int f(int n) {\n"
        "    char* p = ZLabAlloc(64);\n"
        "    ZLabFree(p);\n"
        "    return 0;\n"
        "}\n"
    )
    d = Path(tempfile.mkdtemp())
    p = d / "f.cpp"
    p.write_text(src, encoding="utf-8")

    rule = LeakRule(SourceLocation(path=p, line=2))
    got = rule.verify(rule.to_hypothesis(), LabCodeIndex(d))
    assert got.verdict.value == "refuted", f"无早退分支应 REFUTED，实际 {got.verdict}"


@_NEEDS_CXX
def test_race_is_detected_without_crash():
    """
    竞争**不崩溃**，也没有栈。

    两个线程并发写同一个变量，谁都没做错什么事，结果只是偶尔不对。
    守的是：能观测到并发访问，且**不把它当成崩溃**。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["race_01"])

    assert run.capture.race_threads >= 2, (
        "应观测到至少两个线程访问同一共享变量")
    assert run.capture.race_conflict_ra is not None, "应持有冲突现场地址"
    assert not run.capture.crashed, "竞争不是崩溃"


@_NEEDS_CXX
def test_race_locates_the_unsynchronized_access():
    """
    竞争的定位必须落在**那条未加保护的访问**上。

    race_01 多次运行结果不同（观察到的线程数会在 2~4 之间波动），
    但定位行必须恒定 —— 因为判定竞争只需要"至少两个线程"，
    不需要等到全部四个线程都到齐。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["race_01"])

    assert run.located_line is not None, "race_01 应能定位"
    assert run.hit, (
        f"应命中未同步访问行（期望 {run.expected_line}，实际 {run.located_line}）")


def test_race_watch_only_activates_with_threads():
    """
    把全局变量放进 guard 页会让**每次访问都进一次异常**。

    单线程程序这么做毫无意义 —— 没有第二个线程就不可能有竞争，
    只会白白拖慢。所以只在源码真的用了 std::thread 时才启用。
    """
    from zlab.probe import inject_race_watch

    single = "long g_x = 0;\nint main() { return 0; }\n"
    src, watched = inject_race_watch(single)
    assert not watched, "无线程时不应启用观测"
    assert src == single, "无线程时不应改动源码"

    threaded = "long g_x = 0;\nint main() { std::thread t(f); }\n"
    _, watched2 = inject_race_watch(threaded)
    assert watched2 == ["g_x"], "有线程时应监视全局计数器"


def test_race_watch_does_not_touch_locals():
    """
    栈上变量跨不了线程，不该被当成共享变量监视。
    """
    from zlab.probe import inject_race_watch

    src = ("int main() {\n"
           "    std::thread t(f);\n"
           "    long local_x = 0;\n"
           "    return 0;\n"
           "}\n")
    out, watched = inject_race_watch(src)
    assert not watched, "局部变量不应被监视"


@_NEEDS_CXX
def test_race_is_confirmed_not_just_coincidentally_right():
    """
    race_01 曾经是**虚命中**：定位对上 432，但报告里零 CONFIRMED 结论、
    置信度只有 0.15 —— 只是运行时证据恰好指对了行号，验证层没参与。

    现在必须有 race 类型的证实结论。这是"证实"与"恰好对上"的分界。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["race_01"])

    confirmed = [c for c in (run.report.conclusions or [])
                 if getattr(c.verdict, "value", None) == "confirmed"]
    assert any(c.kind == "race" for c in confirmed), (
        f"race_01 应有 race 类型的证实结论，实际："
        f"{[(c.kind, getattr(c.verdict,'value',None)) for c in (run.report.conclusions or [])]}")
    assert run.report.confidence > 0.15, "置信度应高于补规则前的 0.15"


@_NEEDS_CXX
def test_stack_overflow_is_confirmed_not_just_coincidentally_right():
    """
    stack_overflow_01 与 race_01 曾是同一类虚命中：
    定位对上 446，但零结论、置信度 0.0。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["stack_overflow_01"])

    confirmed = [c for c in (run.report.conclusions or [])
                 if getattr(c.verdict, "value", None) == "confirmed"]
    assert any(c.kind == "recursion" for c in confirmed), (
        f"应有 recursion 类型的证实结论，实际："
        f"{[(c.kind, getattr(c.verdict,'value',None)) for c in (run.report.conclusions or [])]}")
    assert run.located_line == run.expected_line, (
        f"自递归的引入点就是递归调用那一行，不该被推到函数定义行"
        f"（期望 {run.expected_line}，实际 {run.located_line}）")


def test_recursion_rule_ignores_conditions_in_comments():
    """
    注释里的伪代码不能被当成真实终止条件。

    stack_overflow_01 的注释写着「最初写成 `if (depth > limit) return 0;`」
    —— 那是一段**已被弃用的代码**，却长得和真代码一模一样。
    不剥注释的话，规则的证据会指到注释行，判据本身建立在说明文字上。
    """
    from zagent.verify import RecursionRule

    stripped = RecursionRule._strip_comment("if (x) // if (depth > limit)")
    assert "depth > limit" not in stripped, "注释内容必须被剥离"
    assert "if (x)" in stripped, "注释之前的代码必须保留"


def test_new_rules_are_falsifiable_on_innocent_code():
    """
    两条新规则都必须能被推翻。

    一个只会被证实的规则等于没有规则 ——
    它只会把所有样本都标记成"已验证"，让置信度失去意义。
    """
    from zagent.verify import RuleVerifier
    from zagent.types import Hypothesis

    class _Code:
        def __init__(self, text):
            self._lines = text.splitlines()

        def read_lines(self, path):
            return self._lines

        def read_context(self, loc, radius=0):
            lo = max(0, loc.line - 1 - radius)
            hi = min(len(self._lines), loc.line + radius)
            return "\n".join(f"{i+1}: {self._lines[i]}" for i in range(lo, hi))

    verifier = RuleVerifier()

    # 单线程里的局部变量自增：既不共享，也没有线程入口
    innocent = "\n".join([
        "void Helper() {",
        "    int n = 0;",
        "    n++;",
        "}",
    ])
    h = Hypothesis(claim="", kind="race",
                   data={"path": "x.cpp", "line": 3, "variable": "n"})
    out = verifier.check(h, _Code(innocent))
    assert getattr(out.verdict, "value", None) == "refuted", (
        f"局部变量自增应被推翻，实际 {getattr(out.verdict,'value',None)}")

    # 常量封顶的递归：一定会终止
    safe_rec = "\n".join([
        "int Walk(int depth) {",
        "    if (depth >= 100) return 0;",
        "    return Walk(depth + 1) + 1;",
        "}",
    ])
    h2 = Hypothesis(claim="", kind="recursion",
                    data={"path": "x.cpp", "line": 3, "func_name": "Walk"})
    out2 = verifier.check(h2, _Code(safe_rec))
    assert getattr(out2.verdict, "value", None) == "refuted", (
        f"常量封顶的递归应被推翻，实际 {getattr(out2.verdict,'value',None)}")


@_NEEDS_CXX
def test_uaf_becomes_observable():
    """
    UAF 本来**根本不崩**，这是它能被定位的前提。

    释放后的内存通常仍然可读，悬垂访问静默返回垃圾值。
    探测器的隔离分配（独占页 + 释放后 PAGE_NOACCESS）让它必然触发
    访问违例 —— UAF 才从"安静地错"变成"响亮地崩"。
    没有这一步，堆追踪记录了分配点也没人来问，等于没记录。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["uaf_01"])

    assert run.capture.crashed, (
        "uaf_01 必须崩溃：隔离分配应让悬垂访问触发访问违例")
    assert run.capture.uaf_free_ra is not None, (
        "应记录到悬垂块的释放点返回地址")


@_NEEDS_CXX
def test_double_free_reports_second_free():
    """
    重复释放要能定位到**第二次**释放那一行。

    接管分配器之后，CRT 自带的重复释放校验失效（free 不再走 CRT），
    校验的责任落在探测器身上：ZLabFree 发现重复释放时主动崩溃，
    并直接输出调用点地址（不依赖栈 —— 崩溃落在探测代码内）。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["double_free_01"])

    assert run.capture.crashed, "重复释放必须崩溃（接管后由探测器自行校验）"
    assert run.capture.doublefree_ra is not None, "应记录第二次释放的调用点"
    assert run.report is not None and run.report.location is not None

    # 定位结果应落在第二次释放附近，而不是探测代码里
    assert run.located_line is not None
    assert run.located_line > 100, (
        f"定位落到了探测代码区域（第 {run.located_line} 行）")


def test_heap_replacement_balances_parens():
    """
    注入替换必须做括号配平。

    朴素替换 `std::malloc(` -> `ZLabAlloc(std::malloc(` 会在
    sizeof(GameObject) 这类嵌套调用上少一个右括号，直接编译失败。
    """
    from zlab.probe import _wrap_calls
    src = 'void f() { void* p = std::malloc(sizeof(GameObject)); }'
    out = _wrap_calls(src, "malloc", "ZLabAlloc",
                      returns_value=True, start=0, replace_with="ZLabAlloc")
    # 括号必须配平，且接管后不应再出现 std::malloc
    assert out.count("(") == out.count(")"), f"括号不配平：{out}"
    assert "std::malloc" not in out, "接管分配后不应保留 std::malloc"


def test_void_returning_call_is_not_wrapped_as_argument():
    """
    std::free 返回 void，**不能**包成 ZLabFree(std::free(x))。

    必须用逗号表达式达成"先登记再释放"，否则编译不过。
    """
    from zlab.probe import _wrap_calls
    src = 'void f(char* b) { std::free(b); }'
    out = _wrap_calls(src, "free", "ZLabFree",
                      returns_value=False, start=0, replace_with="ZLabFree")
    assert out.count("(") == out.count(")"), f"括号不配平：{out}"
    assert "ZLabFree" in out


def test_probe_code_itself_is_not_rewritten():
    """
    注入的探测代码不得被自己的替换规则改写。

    探测代码里也有 malloc / memset 调用，
    被替换会让 ZLabAlloc 递归调用自己。
    """
    from zlab.probe import inject_probe, WINDOWS_PROBE
    src = "#include <cstdio>\nvoid* p = std::malloc(8);\n"
    out = inject_probe(src)
    # 探测代码段内部不应出现被替换的痕迹
    probe_part = out[:out.find("void* p")]
    assert "ZLabAlloc(std::malloc" not in probe_part


@_NEEDS_CXX
def test_out_of_module_crash_still_uses_stack_frames():
    """
    回归测试：崩溃点落在系统模块时，不得丢弃栈回溯帧。

    double_free 原本崩在 ntdll（crash_address 为 None），
    栈里回溯出的 f.cpp 帧是唯一定位依据。
    曾经因为 crash_address 为 None 就整条降级返回，4 帧全丢。

    **前提已被堆追踪改变**（2026-10-13）：探测器接管了分配器，
    重复释放现在表现为"写入已置 NOACCESS 的块"，崩在自身模块里，
    crash_address 不再为 None。这条测试仍要守住的是后半段：
    栈帧不得被丢弃、且必须能定位到源码。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["double_free_01"])

    # 崩溃点可能在模块外（CRT 堆校验）也可能在模块内（NOACCESS 访问违例），
    # 两种都合法；唯一不变的要求是栈帧不能丢。
    assert len(run.capture.frames) > 0, "堆破坏类崩溃不应丢弃栈帧"
    assert run.report is not None and run.report.location is not None, (
        "double_free 应能定位到源码")


@_NEEDS_CXX
def test_crash_retries_on_timeout():
    """
    回归测试：超时必须重试，不能记成"未崩溃"。

    放在系统 Temp 目录下的 exe，首次运行会被防病毒实时扫描挂住
    （30 秒不崩溃也不退出）。若不重试，样本被记成假 MISS，
    看起来是定位能力不行，实际是环境噪声。
    """
    lab = _make_lab()
    p = PATTERNS["null_deref_01"]
    d = _build_dir(p.id)
    ok, exe, err = lab.build(p, d)
    assert ok, err[:300]
    cap = lab.crash(p.id, exe, retries=1)

    assert cap.crashed, "重试后仍未能捕获崩溃"


@_NEEDS_CXX
def test_evidence_locator_matches_report_location():
    """
    证据里的位置必须与报告定位一致。

    **这条是被 eval 逼出来的。** 实测 stack_overflow_01 出现
    「report.location=446 判定 HIT，但 eval 判定位错误」——
    同一个样本两个相反结论。

    根因是管线里两处口径不一致：
    - `report.location` 走 `_pick_location`，对递归栈用**众数投票**
      选出递归调用点（446）；
    - `propose` 拿的是原始 `locations`，取 `locations[0]`，
      那是栈帧 0 的**函数入口**（430）。

    `_pick_location` 的注释明确写了"不能无脑取 locations[0]"，
    propose 却恰恰那么做了。于是假设与证据全建立在函数入口上。

    而 `Report.top()` 取的是**证据里的 locator**，eval 用它算 top1 ——
    所以它看到的是 430，与 ground truth 446 不符。

    修法是让 propose 建立在最终定位上。这里守的是：两者必须同值。
    """
    lab = _make_lab()
    for pid in ("stack_overflow_01", "race_01", "null_deref_01"):
        r = lab.run(PATTERNS[pid]).report
        if r.location is None:
            continue
        locators = [e.locator for h in (r.conclusions or [])
                    for e in (h.evidence or [])
                    if getattr(getattr(h, "verdict", None), "value", None)
                    == "confirmed"]
        assert locators, f"{pid} 应有证实结论的证据"
        for loc in locators:
            assert loc.endswith(f":{r.location.line}"), (
                f"{pid}: 证据位置 {loc} 与报告定位 "
                f"{r.location.path}:{r.location.line} 不一致 —— "
                f"假设建立在错误的位置上")


@_NEEDS_CXX
def test_alloc_site_does_not_override_crash_site():
    """
    **分配点不能顶替崩溃点成为报告定位。**

    这是我给探测器加"写穿活跃块"观测时**自己引入的回归**。

    当时的推理是：把分配点插在崩溃地址之前，截断时就不会丢。
    但 `_pick_location` 在非递归栈下直接返回 locations[0] ——
    于是分配点顶替崩溃点成了报告定位。

    实测 oob_write_01：根因是 479 行的 `len = r.end - r.start` 下溢，
    却定位到 487 行的 `ZLabAlloc(64)`，偏差 8 行。
    在基线里它只表现为"少一个 HIT"，很容易被当成既有的老问题放过。

    修法是引入 role：崩溃点（crash）是实测事实，分配点（alloc）
    只是推断线索，没资格当 locations[0]。

    **位置顺序不该承载语义** —— 这条守的就是这件事。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["oob_write_01"])
    assert run.hit, (
        f"oob_write_01 未命中：located={run.located_line} "
        f"expected={run.expected_line}")

    # 479 是下溢那一行；487 是 main 里的 ZLabAlloc(64)。
    # 若分配点抢占了定位，这里会稳定得到 487。
    assert run.located_line == run.expected_line, (
        f"定位落在 {run.located_line} 行而非根因 {run.expected_line} 行 —— "
        f"若它是 487，说明分配点顶替了崩溃点（回归）")

    # 且必须由下溢类结论证实，不能只是碰巧行号对上
    r = run.report
    assert r is not None
    confirmed = [c for c in (r.conclusions or [])
                 if getattr(getattr(c, "verdict", None), "value", None)
                 == "confirmed"]
    assert any(c.kind == "unsigned_underflow" for c in confirmed), (
        f"oob_write_01 缺少 unsigned_underflow 证实结论"
        f"（现有：{[c.kind for c in confirmed]}）")


@_NEEDS_CXX
def test_overflow_is_confirmed_not_degraded():
    """
    overflow_01 必须被**证实**，而不是走降级路径兜过去。

    它曾经长期被当成负样本：走降级、report 为 None。
    当时把它解释成"探测不到就克制"，但那是把能力不足包装成了美德 ——
    detect_hint 明明白白写着根因在乘法，只是没有任何设施够得着它。

    真正的根因有两个，缺一不可：
    1. 样本写错了：`(size_t)count * sizeof(T)` 先提升到 64 位再乘，
       count 是 int（上限 2^31）时乘积上限约 2^39，
       在 64 位 size_t 下**永远不可能溢出**。
       实测结果是申请 2GB 失败、malloc 返回 NULL、return -1 退出，
       连循环都没进 —— 一个"应该崩"的样本安静地返回了 -1。
       真漏洞的形态是**窄宽度乘法先溢出，再赋给宽类型**。
    2. 没有运行时证据：崩溃在循环写入，栈只能指到 memset，
       够不到分配点的乘法。

    这里守的是：必须有 overflow 类型的 CONFIRMED 结论，且定位到乘法行。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["overflow_01"])
    assert run.hit, (
        f"overflow_01 未命中：located={run.located_line} "
        f"expected={run.expected_line}")

    r = run.report
    assert r is not None, "overflow_01 走降级路径，说明越界块探测失效"

    confirmed = [c for c in (r.conclusions or [])
                 if getattr(getattr(c, "verdict", None), "value", None)
                 == "confirmed"]
    assert any(c.kind == "overflow" for c in confirmed), (
        f"overflow_01 缺少 overflow 类型的证实结论（现有："
        f"{[c.kind for c in confirmed]}）—— 若只是靠别的规则碰对行号，"
        f"换个样本就会露馅")


@_NEEDS_CXX
def test_clean_code_is_not_flagged():
    """
    **干净代码不能被报出根因。**

    这条守的是整套评估里最容易被牺牲的那一半。
    只测"能不能找到问题"的评估会奖励一个对所有输入都报"找到根因"的
    agent —— 那样的 agent 在 8 个全正样本的集子里能拿满分。

    clean_01 刻意带上乘法与分配（与 overflow 同形），
    但宽度正确、无溢出。若 OverflowRule 漏掉宽度判据，
    就会在这段干净代码上误报。

    正确行为是：不崩、无定位、不给高置信结论。
    """
    lab = _make_lab()
    run = lab.run(PATTERNS["clean_01"])
    assert not run.hit, (
        f"clean_01 是无缺陷样本，却被判定命中 {run.located_line} 行 —— "
        f"规则在猜，不是在识别")

    r = run.report
    if r is None:
        # 降级路径：不给结论。但**这不足以证明规则不误报** ——
        # 它只说明这次没走到规则。真正的验证在下面。
        pass
    else:
        confirmed = [c for c in (r.conclusions or [])
                     if getattr(getattr(c, "verdict", None), "value", None)
                     == "confirmed"]
        assert not confirmed, (
            f"clean_01 上出现了证实结论 {[c.kind for c in confirmed]} —— 误报")
        assert r.confidence <= 0.35, (
            f"clean_01 置信度 {r.confidence} 过高，应保持在低位")

    # ---- 真正守"不误报"的是这一段 ----
    #
    # 实测发现：clean_01 走降级路径，report 为 None，
    # 规则**根本没被调用**。上面的断言因此是空转的 ——
    # 把宽度判据整个删掉，它照样通过。
    #
    # 一个只在"规则恰好没被触发"时才通过的测试，等于没有测试。
    # 所以这里直接把干净代码喂给规则，绕过运行时的偶然性。
    from zagent.verify import OverflowRule, RuleVerifier
    from zagent.types import Hypothesis

    class _Code:
        def __init__(self, text):
            self._lines = text.splitlines()

        def read_lines(self, path):
            return self._lines

        def read_context(self, loc, radius=0):
            lo = max(0, loc.line - 1 - radius)
            hi = min(len(self._lines), loc.line + radius)
            return "\n".join(f"{i+1}: {self._lines[i]}"
                             for i in range(lo, hi))

    # 与 overflow_01 同形：有乘法、有 sizeof、有分配，
    # 但用 size_t（先提升再乘），count 为 int 时不可能溢出。
    clean_src = "\n".join([
        "struct Entry { char payload[256]; };",
        "Entry* MakeTable(int count) {",
        "    size_t bytes = (size_t)count * sizeof(Entry);",
        "    return (Entry*)std::malloc(bytes);",
        "}",
    ])
    out = OverflowRule.from_data({"path": "x.cpp", "line": 4}).verify(
        Hypothesis(claim="", kind="overflow",
                   data={"path": "x.cpp", "line": 4}),
        _Code(clean_src))
    assert getattr(out.verdict, "value", None) == "refuted", (
        f"size_t 宽度的乘法在 64 位下不会溢出，应被推翻，"
        f"实际 {getattr(out.verdict,'value',None)} —— "
        f"若漏掉宽度判据，规则会在所有正常分配上误报")


@_NEEDS_CXX
def test_no_sample_is_hit_without_confirmed_conclusion():
    """
    **命中必须附带证实结论，虚命中不算命中。**

    这一条守的是整套评估的可信度。基线表只统计"定位行 == 期望行"，
    它分不清「推理层证实了」和「运行时证据恰好指对了行号，验证层没参与」。

    后者是虚命中，换个样本就会露馅。实测补规则之前：
      stack_overflow_01  置信度 0.0、零结论
      race_01            置信度 0.15、零证实
    两个都"命中"了。

    `overflow_01` 例外：它是纯整数溢出、无运行时证据，
    报告为 None，正确行为就是不给结论。它不该出现在这份核对里。
    """
    lab = _make_lab()
    for pid in PATTERNS:
        run = lab.run(PATTERNS[pid])
        r = run.report
        if r is None:
            # 降级路径：明确不给结论。
            #
            # 允许的只有两类：
            # - clean_01：压根没有缺陷，不给结论是**期望行为**；
            # - overflow_01：一旦它重新走降级，说明越界探测失效了，
            #   所以这里仍然放它过，但由另一条测试专门守它必须 HIT。
            assert pid in ("clean_01", "overflow_01"), (
                f"{pid} 走降级路径却没有运行时证据，"
                f"应检查探测器是否失效")
            continue
        if not run.hit:
            continue
        confirmed = [c for c in (r.conclusions or [])
                     if getattr(getattr(c, "verdict", None), "value", None)
                     == "confirmed"]
        assert confirmed, (
            f"{pid} 命中 {run.located_line} 行但没有任何 CONFIRMED 结论 "
            f"（置信度 {getattr(r, 'confidence', None)}）—— 这是虚命中，"
            f"不是真的定位到了根因")


def test_lab_default_workdir_is_not_system_temp():
    """
    Lab 的默认工作目录不能是系统 Temp。

    防病毒对系统 Temp 下未知 exe 的实时扫描会：挂住进程
    （表现为假的"未崩溃"）、锁住文件句柄导致清理时 PermissionError、
    让第二次运行走不同的崩溃路径。

    这是 README 点名的环境噪声源。不传 workdir 也必须绕开它，
    否则任何一处忘了显式传参的调用都会把噪声引进来。
    """
    import tempfile
    from zlab.lab import Lab

    lab = Lab()
    system_temp = Path(tempfile.gettempdir()).resolve()
    assert system_temp not in lab.workdir.resolve().parents, (
        f"Lab 默认工作目录落在系统 Temp 下（{lab.workdir}），"
        f"会引入防病毒噪声")


@_NEEDS_CXX
def test_ground_truth_recount_after_injection():
    """
    回归测试：ground truth 必须基于注入后的源码重算。

    曾经直接用 pattern.root_cause_line()，结果 eval 稳定报"定位错误"，
    但定位其实是对的，只是标签被注入代码推移了行号。
    这类假失败会诱导你去修一个根本没坏的模块。
    """
    lab = _make_lab()
    original = PATTERNS["null_deref_01"].root_cause_line()

    outdir = _build_dir("null_deref_01")
    lab.build(PATTERNS["null_deref_01"], outdir)
    written = outdir / "f.cpp"
    recount = lab._recount_root_line(written, PATTERNS["null_deref_01"])

    assert recount > original, (
        f"重算行号 {recount} 应大于原始行号 {original}（注入使行号下移）")
