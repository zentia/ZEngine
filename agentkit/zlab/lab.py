"""
实验台：把 zfault 的故障样本接成 zagent 流水线的真实输入。

**这一步是整个项目从"框架"变成"可验证系统"的分界**

之前 zagent 跑的是合成 JSON（栈帧由人手填）。那只能证明流水线不崩，
不能证明它有用 —— ground truth 是人填进去的，属于自证。

zlab 的做法：
  注入缺陷 -> 编译 -> 真的崩 -> 捕获崩溃地址 -> 真的符号化 -> 喂给流水线
每一步都是实测。ground truth 来自「我注入的是哪一行」，与流水线无关。
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from zagent.pipeline import diagnose
from zagent.protocols import CheckoutResult, Context, Revision, VersionResolver
from zagent.tracing import Tracer
from zagent.protocols import Symbol
from zagent.types import Facts, Report, SourceLocation, StackFrame
from zagent.verify import HeuristicReasoner, RuleVerifier

from zfault import process as zproc
from zfault.patterns import FaultPattern
from zfault.toolchain import detect, Toolchain

from .probe import inject_probe, parse_probe_output
from .symbols import MsSymbolResolver

# 喂给流水线的最大帧数。
# 递归栈能展开出几十上百帧，全喂进去没意义（符号化开销大），
# 但也不能太少：众数投票需要足够的重复样本才能识别循环点。
MAX_FRAMES = 64


# 某些样本的缺陷**只在特定输入下才发生**。
#
# leak_01 的 main 写的是 `BuildContext(&ctx, argc > 1 ? 1 : 0)`：
# 不带参数运行时 failAt=0，走的是成功路径，
# BuildContext 正常返回、FreeContext 正常释放 —— **一次都不泄漏**。
# 也就是说，如果不传参，泄漏检测做得再对也只能报告"零泄漏"，
# 而这个"零"会被当成程序没问题的证据。
#
# 这正是泄漏类缺陷最阴的地方：它不崩溃，
# 默认输入下甚至不表现出任何异常。
# "没有崩溃就没问题"在这里会直接把真缺陷判成没问题。
#
# 所以这里显式给出触发输入。放在实验台而不是样本定义里，
# 是为了不让运行参数污染 ground truth 数据。
TRIGGER_ARGV: dict[str, tuple[str, ...]] = {
    "leak_01": ("trigger",),     # argc>1 -> failAt=1 -> 早退泄漏 ctx->a
}


# ============================================================ 崩溃捕获

@dataclass
class CrashCapture:
    """一次真实崩溃的捕获结果。"""

    pattern_id: str = ""
    returncode: int | None = None
    signal: str = ""
    crash_address: int | None = None      # 崩溃指令地址（已归一化）
    raw_address: int | None = None        # 运行时原始地址
    fault_address: int | None = None      # 被访问的内存地址
    exception_code: int | None = None
    runtime_base: int | None = None
    frames: list[int] = field(default_factory=list)      # 归一化后的栈帧
    raw_frames: list[int] = field(default_factory=list)  # 运行时原始栈帧
    stdout: str = ""
    timed_out: bool = False
    symbolized: bool = False
    # 堆追踪证据（由探测器的 ZLabAlloc / ZLabFree 记录，运行时产出）
    uaf_alloc_ra: int | None = None    # 悬垂块的分配点返回地址
    uaf_free_ra: int | None = None     # 悬垂块的释放点返回地址
    doublefree_ra: int | None = None   # 重复释放中"第二次释放"的调用点
    # 泄漏：退出时仍未释放的块，及其分配点
    leak_alloc_ras: list[int] = field(default_factory=list)
    leak_count: int = 0
    # 竞争：共享变量被多个线程访问，冲突现场的指令地址
    race_conflict_ra: int | None = None
    race_first_ra: int | None = None
    race_threads: int = 0
    # 写穿活跃块：越界写撞上块尾隔离页。hit_alloc_ra 是**该块的分配点**，
    # 符号化后落在"大小计算"那一行 —— 整数溢出的根因所在。
    hit_alloc_ra: int | None = None
    hit_size: int | None = None      # 该块实际容量（字节）
    hit_off: int | None = None       # 故障地址相对块基址的偏移
    notes_outside_module: bool = False   # 崩溃点不在自身模块（检测点≠破坏点）
    frames_dropped: int = 0              # 因落在其他模块而丢弃的帧数
    notes: list[str] = field(default_factory=list)

    @property
    def crashed(self) -> bool:
        """
        是否真的崩溃。

        **为什么不能只看退出码范围**

        Windows 上 `return -1` 的退出码是 0xFFFFFFFF，
        它落在"大于 255"的范围里，于是被当成崩溃。
        实测 leak_01 与 overflow_01 都是这个值 ——
        两者都是程序**自己 return -1 正常退出**，一次都没崩。
        它们因此被误计入"成功崩溃"，基线虚高了两个。

        真正的崩溃以 **NTSTATUS** 形式返回，高 4 位是 0xC：
            0xC0000005  访问违例
            0xC00000FD  栈溢出
        据此判断，而不是"退出码是不是很大"。

        这个修正会让基线数字**下降** —— 但下降的是虚高的部分，
        不是能力退化。宁可要一个诚实的 5/8，也不要一个注水的 7/8。
        """
        if self.returncode is None:
            return False
        return (self.returncode & 0xF0000000) == 0xC0000000


# ============================================================ 代码索引

class LabCodeIndex:
    """
    实验台专用代码索引。

    不用 git（样本源码在临时目录，不在版本库里），
    直接以「注入时的源码文件」为唯一真相。

    版本对齐语义：样本自带 revision，code 侧回显同一个 id，
    因此阶段 2 门禁在实验台内恒通过 —— 这是设计如此：
    实验室测的是定位能力，版本对齐已有独立测试覆盖。
    """

    def __init__(self, root: Path, project_files: set[str] | None = None) -> None:
        self.root = Path(root)
        self.project_files = project_files or {"f.cpp"}
        self._revision = Revision(id="lab", note="injected sample")

    def current_revision(self) -> Revision:
        return self._revision

    def checkout(self, rev: Revision, allow_dirty: bool = True) -> CheckoutResult:
        return CheckoutResult(True, message="lab: 单版本，无切换")

    def is_project_code(self, frame: StackFrame) -> bool:
        if frame.file:
            return Path(frame.file).name in self.project_files
        return True

    def map_to_local(self, sym: Symbol):
        if not sym.file:
            return None
        p = Path(sym.file)
        if not p.exists():
            cand = self.root / Path(sym.file).name
            if not cand.exists():
                return None
            p = cand
        return SourceLocation(path=p, line=sym.line or 1, symbol=sym.function)

    def read_lines(self, path: Path) -> list[str]:
        try:
            return Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return []

    def read_context(self, loc: SourceLocation, radius: int = 40) -> str:
        lines = self.read_lines(loc.path)
        if not lines:
            return ""
        lo = max(0, loc.line - 1 - radius)
        hi = min(len(lines), loc.line + radius)
        return "\n".join(f"{i+1}: {lines[i]}" for i in range(lo, hi))

    def grep(self, pattern: str, limit: int = 200):
        try:
            rx = re.compile(pattern)
        except re.error:
            return []
        out = []
        for f in sorted(self.root.rglob("*")):
            if not f.is_file() or f.suffix not in {".cpp", ".h", ".hpp", ".c"}:
                continue
            try:
                for i, line in enumerate(
                        f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                    if rx.search(line):
                        out.append((f, i, line))
                        break
            except OSError:
                continue
            if len(out) >= limit:
                break
        return out


class LabVersionResolver:
    def resolve(self, product_version: str):
        if not product_version:
            return None
        return Revision(id=product_version, note="lab sample")


# ============================================================ 实验台

@dataclass
class LabRun:
    """一次实验的完整记录。"""

    pattern_id: str
    capture: CrashCapture
    report: Report | None = None
    expected_line: int = 0
    notes: list[str] = field(default_factory=list)

    @property
    def located_line(self) -> int | None:
        return self.report.location.line if (self.report and self.report.location) else None

    @property
    def hit(self) -> bool:
        """是否命中注入的根因行。"""
        return self.located_line == self.expected_line

    @property
    def off_by(self) -> int | None:
        if self.located_line is None or not self.expected_line:
            return None
        return abs(self.located_line - self.expected_line)


class Lab:
    """实验台：编译 -> 崩溃 -> 符号化 -> 诊断。"""

    def __init__(self, workdir: Path | None = None) -> None:
        self.tc: Toolchain = detect()
        # 默认目录**不能**是系统 Temp。防病毒对未知 exe 的实时扫描会
        # 挂住进程（表现为假的"未崩溃"）、锁住文件句柄导致清理时
        # PermissionError、甚至让第二次运行走不同的崩溃路径。
        # 这是 README 里点名的环境噪声源，默认就绕开它。
        default = Path(__file__).resolve().parent.parent / "_lab_work"
        self.workdir = Path(workdir) if workdir else default
        self.workdir.mkdir(parents=True, exist_ok=True)

    @property
    def supported(self) -> bool:
        return self.tc.supported

    def build(self, pattern: FaultPattern, outdir: Path,
              with_probe: bool = True) -> tuple[bool, Path | None, str]:
        """
        编译一条故障模式。

        /Zi 生成 PDB（符号化必需）
        /Od 关闭优化（防止缺陷被优化掉）
        /Fm 生成 map（归一化需要 static base，能力探测需要已知地址）
        """
        outdir.mkdir(parents=True, exist_ok=True)
        source = inject_probe(pattern.source) if with_probe else pattern.source
        src = outdir / "f.cpp"
        src.write_text(source, encoding="utf-8")

        exe = outdir / "f.exe"
        obj = outdir / "f.obj"
        mapf = outdir / "f.map"

        cmd = [self.tc.cxx, "/nologo", "/std:c++17", "/EHsc", "/Zi", "/Od",
               f"/Fe:{exe}", f"/Fo:{obj}", f"/Fm:{mapf}", str(src)]

        r = zproc.run(cmd, timeout=180, env=self.tc.build_env)
        if r.returncode != 0 or not exe.exists():
            return False, None, (r.stdout or "") + (r.stderr or "")
        return True, exe, ""

    @staticmethod
    def _recount_root_line(written: Path, pattern: FaultPattern) -> int:
        """在注入后的源码里重新定位根因行。找不到则回退到原始行号。

        复用 pattern.root_cause_line_in，与原始定位共用同一判据
        （含 anchor），避免两处逻辑漂移。
        """
        try:
            found = pattern.root_cause_line_in(
                written.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            found = -1
        return found if found > 0 else pattern.root_cause_line()

    def crash(self, pattern_id: str, exe: Path, retries: int = 1,
              argv: tuple[str, ...] = ()) -> CrashCapture:
        """
        运行崩溃程序并捕获地址。

        **为什么要重试**

        实测发现：放在系统 Temp 目录下的 exe，首次运行会挂住 30 秒以上
        不崩溃也不退出（防病毒对临时目录里的未知 exe 做实时扫描），
        第二次运行就正常。如果不重试，这类样本会被记成"未崩溃"，
        产生假的 MISS —— 看起来是定位能力不行，实际是环境噪声。

        所以超时时重试一次，并把重试事实写进 notes，
        而不是静默地把它当成一个普通失败。
        """
        cap = self._crash_once(pattern_id, exe, argv)

        # 预热：同一 exe 首次运行常被防病毒实时扫描挂住。
        # 先跑一次丢弃结果，让扫描缓存生效，再取正式结果。
        # 这比"超时后重试"更可靠 —— 预热能让后续运行走正常的崩溃路径，
        # 而不是在超时后碰运气。
        if cap.timed_out or cap.returncode is None:
            self._crash_once(pattern_id, exe, argv)
            cap = self._crash_once(pattern_id, exe, argv)
            cap.notes.append("首次运行异常（防病毒扫描/超时），已预热后重跑")

        for _ in range(retries):
            if not cap.timed_out and cap.returncode is not None:
                break
            retry = self._crash_once(pattern_id, exe, argv)
            retry.notes.append("运行异常，已重试")
            cap = retry
        return cap

    def _crash_once(self, pattern_id: str, exe: Path,
                    argv: tuple[str, ...] = ()) -> CrashCapture:
        """运行崩溃程序并捕获结果。"""
        r = zproc.run([str(exe), *argv], timeout=60, env=self.tc.build_env)
        cap = CrashCapture(
            pattern_id=pattern_id,
            returncode=r.returncode,
            signal=r.signal_desc(),
            stdout=r.stdout,
            timed_out=r.timed_out,
        )

        parsed = parse_probe_output(r.stdout)
        cap.raw_address = parsed.get("ip")
        cap.fault_address = parsed.get("fault_addr")
        cap.exception_code = parsed.get("code")
        cap.runtime_base = parsed.get("base")

        # 堆追踪证据。这是 UAF/重复释放能定位的关键输入 ——
        # 它们要么不崩，要么崩在探测代码里，栈对定位没有帮助。
        cap.uaf_alloc_ra = parsed.get("uaf_alloc_ra")
        cap.uaf_free_ra = parsed.get("uaf_free_ra")
        cap.doublefree_ra = parsed.get("doublefree_ra")

        # 泄漏证据：程序**正常退出**时清点出来的未释放块。
        # 它与崩溃类证据互斥 —— 崩掉的进程不执行静态析构。
        cap.leak_alloc_ras = parsed.get("leak_alloc_ras") or []
        cap.leak_count = parsed.get("leak_count") or 0
        if cap.leak_count:
            cap.notes.append(f"检测到 {cap.leak_count} 个未释放的块（泄漏）")

        # 写穿活跃块：越界写撞上块尾隔离页。
        # 这是整数溢出唯一的运行时信号 —— 崩溃在循环写入，
        # 但根因在分配时的乘法上，栈永远指不到那里。
        cap.hit_alloc_ra = parsed.get("hit_alloc_ra")
        cap.hit_size = parsed.get("hit_size")
        cap.hit_off = parsed.get("hit_off")

        # 竞争证据：共享变量被多个线程无保护地访问。
        cap.race_conflict_ra = parsed.get("race_conflict_ra")
        cap.race_first_ra = parsed.get("race_first_ra")
        cap.race_threads = parsed.get("race_threads") or 0
        if cap.race_threads >= 2:
            cap.notes.append(
                f"检测到 {cap.race_threads} 个线程并发访问同一共享变量")

        res = MsSymbolResolver(exe)
        base = cap.runtime_base

        # 栈回溯：逐帧归一化。
        #
        # **这里曾经有个典型错误，值得记下来。**
        # 原先写了 `if norm % 2 != 0: break`，用"地址奇数 = 栈被污染"做判据。
        # 结果是 oob_write 稳定 0 帧：它的第一帧返回地址是 0x140007577 ——
        # 末位是 7，被当成污染帧直接 break。
        # 但 x86-64 指令长度可变，返回地址指向"下一条指令"，
        # 完全可以是奇数（0x7570 处一条 7 字节指令，返回地址就是 0x7577）。
        # 用奇偶判断污染，会把合法帧全杀掉。
        #
        # 教训：**不要用地址的数值特征去猜语义**。
        # 地址是否合法，只有符号化能回答 —— 让专业的模块做专业的判断。
        # normalize 只负责它真正知道的事：地址是否落在自身模块内。
        # 符号化失败的帧会在流水线里被丢弃，不需要在这里预判。
        for _, raw in parsed.get("frames", []):
            cap.raw_frames.append(raw)
            norm = res.normalize(raw, base)
            if norm is None:
                cap.frames_dropped += 1   # 模块外：检测点（CRT / ntdll）
                continue
            cap.frames.append(norm)

        # 崩溃点单独处理：它才是第一帧，回溯帧跟在后面。
        if cap.raw_address is not None:
            normalized = res.normalize(cap.raw_address, base)
            if normalized is None:
                cap.notes_outside_module = True
            cap.crash_address = normalized

        return cap

    def run(self, pattern: FaultPattern, with_probe: bool = True) -> LabRun:
        """
        完整跑一遍：编译 -> 崩溃 -> 符号化 -> 诊断。

        **ground truth 必须基于注入后的源码重新计算**，这一点很关键。

        探测器注入会在源码里插入几十行（在最后一个 #include 之后），
        导致根因行整体下移。若仍用原始 pattern.root_cause_line()，
        eval 会稳定报「定位错误」——但定位其实是对的，只是标签过期。

        这类假失败比真失败更危险：它会诱导你去"修"一个根本没坏的模块。
        所有对源码做过变换的环节，都必须重新计算 ground truth。
        """
        outdir = self.workdir / pattern.id
        expected = pattern.root_cause_line()

        ok, exe, err = self.build(pattern, outdir, with_probe=with_probe)
        if not ok:
            cap = CrashCapture(pattern_id=pattern.id, signal="编译失败")
            return LabRun(pattern.id, cap, None, expected, [f"编译失败: {err[:200]}"])

        # 用实际写入磁盘的源码重算根因行（注入后行号已改变）
        written = outdir / "f.cpp"
        if written.exists():
            expected = self._recount_root_line(written, pattern)

        cap = self.crash(pattern.id, exe,
                         argv=TRIGGER_ARGV.get(pattern.id, ()))

        #
        # **这里修掉一个会掩盖真实能力的逻辑缺陷。**
        # 原来写的是「crash_address 为 None 就直接降级返回」，
        # 理由是"没拿到崩溃地址"。但崩溃点落在 ntdll / CRT 里
        # （堆破坏、双重释放的典型表现）时，crash_address 必然为 None ——
        # 而此时栈回溯帧恰恰是唯一的救命稻草。
        # 结果 double_free 明明回溯出 4 帧（含 f.cpp:78 的 Process），
        # 却被整条丢掉，记成一个干瘪的 MISS。
        #
        # 判据应该是「有没有任何可用帧」，而不是「崩溃点能不能归一化」。
        # 崩溃点拿不到只是说明"检测点不在项目代码"，
        # 这个事实要记下来，但不能因此放弃回溯帧。
        #
        # 现在还要加上堆追踪证据：重复释放崩在 ZLabFree 内部，
        # 栈符号化只能指向探测代码，但 ZLAB_DOUBLEFREE_RA 直接给出了
        # 业务代码里"第二次释放"的位置，这是比栈更精确的定位输入。
        has_heap_evidence = (cap.doublefree_ra is not None
                             or cap.uaf_free_ra is not None
                             or bool(cap.leak_alloc_ras)
                             or cap.race_conflict_ra is not None
                             or cap.hit_alloc_ra is not None)
        if cap.crash_address is None and not cap.frames and not has_heap_evidence:
            return LabRun(pattern.id, cap, None, expected,
                          ["崩溃点不在自身模块且无可用栈帧，走降级路径"] +
                          list(cap.notes))

        # 构造栈帧。崩溃点是第一帧，回溯调用链跟在后面。
        #
        # 顺序很重要：流水线的 locations[0] 即"崩溃位置"。
        # 早期版本把 handler 帧排在最前，定位结果就成了探测代码里的行，
        # 看起来是"定位到了"，实际指到了我们自己的注入物上。
        #
        # **这里不能去重。** 递归栈里同一个地址会重复几十次 ——
        # 实测 stack_overflow 展开出 34 帧，去重后只剩 4 个地址，
        # 其中 31 帧是同一个递归调用点。那个"重复"恰恰是最强的根因信号：
        # 执行流在这里循环。去重会把它抹掉，只留下函数入口帧。
        #
        # 但也不能无限保留：栈被污染时会产生大量垃圾帧。
        # 去重地截到 MAX_FRAMES，保序 —— 栈深度本身是信息，
        # 且保留重复才能让下游做众数投票。
        # 堆证据在这里只登记待归一化，真正的地址换算在拿到 resolver 后做
        # —— 顺序不能反：resolver 与 runtime_base 都要先就位。
        pending_doublefree: list[int] = (
            [cap.doublefree_ra] if cap.doublefree_ra is not None else []
        )
        # 泄漏：未释放块的分配点。
        # leak 不崩溃，栈是空的 —— 这是唯一的定位输入。
        pending_leak: list[int] = list(cap.leak_alloc_ras)
        # 竞争：冲突现场的指令地址。race 同样不崩溃，栈也是空的。
        pending_race: list[int] = (
            [cap.race_conflict_ra] if cap.race_conflict_ra is not None else []
        )
        # 整数溢出：崩溃在循环写入，但根因在分配点的乘法。
        # 这一条与 doublefree 同理 —— 崩溃行是"检测点"，分配行才是"引入点"。
        pending_hit: list[int] = (
            [cap.hit_alloc_ra] if cap.hit_alloc_ra is not None else []
        )
        addrs: list[int] = []

        # resolver 必须在这里就位：堆证据要先归一化再插进 addrs，
        # 而 addrs 紧接着就要用来构造 frames。
        # 曾经把它放在 frames 构造之后，插入的地址根本没被用上，
        # double_free 因此稳定定位到探测代码（偏 112 行）。
        resolver = MsSymbolResolver(exe)

        # **堆证据只在崩溃点失效时才替代它。**
        #
        # 重复释放崩在 ZLabFree 里（我们注入的探测代码），
        # 栈符号化只能指向自己的注入物 —— 定位毫无意义。
        # 此时 ZLAB_DOUBLEFREE_RA 才是真正的答案：
        # 它记录的是业务代码里"第二次释放"那一行的返回地址。
        #
        # UAF 则相反：崩溃点就在业务代码的悬垂访问处，
        # 那本身就是 ground truth，不该被"释放点"覆盖掉。
        # 早期版本把两种堆证据都插到首位，
        # 结果 double_free 拿 uaf_free_ra（第一次释放）当答案，偏了 102 行。
        #
        # 判据：只有 doublefree_ra 才替换首帧。
        for ra in pending_doublefree:
            norm = resolver.normalize(ra, cap.runtime_base)
            if norm is None:
                cap.notes.append(f"堆证据地址落在模块外，丢弃（{ra:#x}）")
                continue
            addrs.append(norm)
            cap.notes.append(f"堆证据定位（返回地址 {ra:#x}）")

        # 泄漏块的分配点。leak 没有栈、没有崩溃地址，
        # 它唯一的信号就是"这块内存到退出时还没还"。
        for ra in pending_leak:
            norm = resolver.normalize(ra, cap.runtime_base)
            if norm is None:
                cap.notes.append(f"泄漏块地址落在模块外，丢弃（{ra:#x}）")
                continue
            addrs.append(norm)
            cap.notes.append(f"泄漏块分配点（返回地址 {ra:#x}）")

        # 竞争冲突现场：那条并发访问共享变量的指令。
        for ra in pending_race:
            norm = resolver.normalize(ra, cap.runtime_base)
            if norm is None:
                cap.notes.append(f"竞争地址落在模块外，丢弃（{ra:#x}）")
                continue
            addrs.append(norm)
            cap.notes.append(f"竞争冲突现场（指令地址 {ra:#x}）")

        # 整数溢出的分配点。
        #
        # **不再靠"插在前面"来保证它参与决策。**
        # 曾经把它排在崩溃地址之前，理由是"截断时不会丢"。
        # 但 `_pick_location` 在非递归栈下直接返回 locations[0] ——
        # 于是分配点顶替崩溃点成了报告定位。
        # oob_write_01 因此被带偏 8 行：根因是 479 的下溢，
        # 却定位到 487 的 `ZLabAlloc(64)`。
        #
        # 位置顺序不该承载语义。现在改成显式标记 role="alloc"：
        # 它照常参与验证与 refine，但没资格当 locations[0]。
        hit_addr_idx = None
        for ra in pending_hit:
            norm = resolver.normalize(ra, cap.runtime_base)
            if norm is None:
                cap.notes.append(f"越界块分配点落在模块外，丢弃（{ra:#x}）")
                continue
            addrs.append(norm)
            hit_addr_idx = len(addrs) - 1
            cap.notes.append(f"越界块分配点（返回地址 {ra:#x}）")

        if cap.crash_address is not None:
            addrs.append(cap.crash_address)
        addrs.extend(cap.frames)
        addrs = addrs[:MAX_FRAMES]

        frames = [
            StackFrame(module=exe.name, address=a, function="", file="", line=None,
                       # 分配点：推断线索，不是崩溃现场，不能当 locations[0]
                       role=("alloc" if i == hit_addr_idx else "crash"))
            for i, a in enumerate(addrs)
        ]

        facts = Facts(
            ref=pattern.id,
            platform="windows",
            product_version="lab",
            exception_type=_code_to_type(cap.exception_code),
            exception_message=cap.signal,
            stack=frames,
        )
        if not frames:
            facts.note_missing("stack")

        code = LabCodeIndex(outdir)


        reasoner = HeuristicReasoner(code)
        verifier = RuleVerifier()

        ctx = Context(
            source=_StaticSource(facts),
            version=LabVersionResolver(),
            code=code,
            symbols=resolver,
            reasoner=reasoner,
            verifier=verifier,
        )

        tracer = Tracer(run_id=pattern.id)
        report = diagnose(pattern.id, ctx, tracer=tracer)
        cap.symbolized = report.location is not None

        return LabRun(pattern.id, cap, report, expected)


class _StaticSource:
    """把已构造好的 Facts 直接喂给流水线（替代从文件读）。"""

    def __init__(self, facts: Facts) -> None:
        self._facts = facts
        self.name = "lab_injected"

    def ready(self) -> bool:
        return True

    def fetch(self, ref: str) -> Facts:
        return self._facts


def _code_to_type(code: int | None) -> str:
    known = {
        0xC0000005: "EXCEPTION_ACCESS_VIOLATION",
        0xC0000374: "STATUS_HEAP_CORRUPTION",
        0xC00000FD: "STATUS_STACK_OVERFLOW",
        0xC0000094: "STATUS_INTEGER_DIVIDE_BY_ZERO",
        0xC0000409: "STATUS_STACK_BUFFER_OVERRUN",
    }
    return known.get(code or 0, "")
