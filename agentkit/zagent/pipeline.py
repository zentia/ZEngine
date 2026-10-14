"""
诊断主流水线。

五个阶段 + 三条硬性约束：

  阶段 0  门禁    能力不具备就 fail-fast。带残缺工具跑到底只会产出假结论，
                  而假结论比没有结论更糟——它看起来是完整的。
  阶段 1  取事实  事实与推断严格分离。缺字段显式记录，不用默认值蒙混。
  阶段 2  版本对齐 用错版本的代码分析，结论全废而且错得隐蔽。硬停。
  阶段 3  符号化  地址 -> 符号 -> 本地源码。映射不了就降级，不猜。
  阶段 4  假设验证 核心。未验证不进结论；被推翻的保留在台账里。
"""

from __future__ import annotations

from pathlib import Path

from .protocols import Context, Symbol
from .tracing import Tracer
from .types import (Evidence, Facts, Hypothesis, Report, Revision, SourceLocation,
                    StackFrame, Verdict, _parse_locator, compute_confidence,
                    now_ms)


def diagnose(ref: str, ctx: Context, tracer: Tracer | None = None,
             auto_checkout: bool = False) -> Report:
    """
    执行一次诊断。

    auto_checkout=False（默认）：版本不一致时阻塞并上报，不自动切换代码。
    这是护栏，不是便利开关——默认必须保守。
    """
    t0 = now_ms()
    tracer = tracer or Tracer(run_id="anon")
    report = Report()

    # ===== 阶段 0：门禁 =====
    tracer.begin("gate")
    missing_caps = ctx.ready_check()
    if missing_caps:
        tracer.end("gate", "blocked", missing=missing_caps)
        report.status = "blocked"
        report.block_reason = f"能力不可用: {', '.join(missing_caps)}"
        report.elapsed_ms = now_ms() - t0
        tracer.flush(outcome=_outcome(report), ledger=report.ledger)
        return report
    tracer.end("gate", "ok", tools=["source", "version", "code"])

    # ===== 阶段 1：取事实 =====
    tracer.begin("fetch")
    facts = ctx.source.fetch(ref)
    report.facts = facts
    tracer.end("fetch",
               "degraded" if facts.missing else "ok",
               missing=facts.missing,
               frames=len(facts.stack))

    if not facts.has_frames() and not facts.missing:
        facts.note_missing("stack")

    # ===== 阶段 2：版本对齐 =====
    tracer.begin("version")
    target = ctx.version.resolve(facts.product_version)
    local = ctx.code.current_revision()

    if target is None:
        tracer.end("version", "degraded", reason="无法从产品版本解析 revision")
        facts.note_missing("revision")
    elif target.id != local.id:
        if not auto_checkout:
            tracer.end("version", "blocked", target=target.id, local=local.id)
            report.status = "blocked"
            report.block_reason = (
                f"版本不一致：崩溃发生在 {target.id}，本地为 {local.id}。"
                f"未自动切换（不 revert 本地改动、不静默换分支）。"
            )
            report.unknowns = list(facts.missing)
            report.elapsed_ms = now_ms() - t0
            tracer.flush(outcome=_outcome(report), ledger=report.ledger)
            return report

        res = ctx.code.checkout(target, allow_dirty=False)
        if not res.ok:
            tracer.end("version", "blocked", conflicts=res.conflicts)
            report.status = "blocked"
            report.block_reason = f"版本切换失败: {res.message}"
            report.unknowns = list(facts.missing)
            report.elapsed_ms = now_ms() - t0
            tracer.flush(outcome=_outcome(report), ledger=report.ledger)
            return report
        tracer.end("version", "ok", target=target.id, previous=local.id)
    else:
        tracer.end("version", "ok", revision=local.id)

    # ===== 阶段 3：符号化与本地化 =====
    tracer.begin("localize")
    locations: list[SourceLocation] = []
    unmapped = 0

    for frame in facts.stack:
        frame.is_project_code = ctx.code.is_project_code(frame)

        sym: Symbol | None = None
        if ctx.symbols is not None:
            sym = ctx.symbols.resolve(frame, target or local)
        elif frame.file:
            # 没有符号解析器时直接用栈帧自带信息（合成样本场景）
            sym = Symbol(function=frame.function, file=frame.file, line=frame.line)

        if sym is None:
            unmapped += 1
            continue

        loc = ctx.code.map_to_local(sym)
        if loc is None:
            unmapped += 1
            continue

        loc.context = ctx.code.read_context(loc, radius=40)
        # 帧的身份要跟着走：分配点是推断线索，不能冒充崩溃现场。
        loc.role = getattr(frame, "role", "crash")
        locations.append(loc)

    tracer.end("localize",
               "degraded" if unmapped else "ok",
               mapped=len(locations), unmapped=unmapped)

    if not locations:
        tracer.note("没有任何栈帧能映射到本地源码")
        report.status = "ok"
        report.unknowns = list(facts.missing) + ["无法映射任何栈帧到本地源码"]
        report.confidence = 0.0
        report.elapsed_ms = now_ms() - t0
        tracer.flush(outcome=_outcome(report), ledger=report.ledger)
        return report

    report.location = _pick_location(locations, tracer)

    # ===== 阶段 4：假设验证 =====
    tracer.begin("verify")
    hypotheses: list[Hypothesis] = []

    if ctx.reasoner is not None:
        # **propose 必须用与 report.location 同一个位置，不能另用 locations[0]。**
        #
        # `_pick_location` 的注释已经写明"不能无脑取 locations[0]"：
        # 递归栈里帧 0 是函数入口，众数投票选出的才是递归调用点。
        # 但这里原先传的是原始 locations，propose 取 locations[0] ——
        # 于是报告定位是 446（调用点），而假设/证据全建立在 430（函数入口）上。
        #
        # 后果不只是数字不一致：`Report.top()` 取的是**证据里的 locator**，
        # eval 用它算 top1。于是实测出现「report.location=446 命中，
        # 但 eval 判定位错误」——同一个样本两个相反结论。
        #
        # 修法是让假设建立在最终定位上，其余候选仍作为备选传给 propose。
        focused = [report.location] + [x for x in locations
                                       if x is not report.location]
        hypotheses = ctx.reasoner.propose(facts, focused)

    if ctx.verifier is not None:
        for h in hypotheses:
            ctx.verifier.check(h, ctx.code)

    report.ledger = hypotheses
    report.conclusions = [h for h in hypotheses if h.is_conclusion]

    # **让验证结论修正定位**
    #
    # 到此为止 report.location 是"崩溃点"。但真正的根因常常在别处：
    # oob_write 崩在 memset（90 行），缺陷是上一行的 len 下溢（88 行）。
    # 只报崩溃点，用户拿到的还是"在哪崩的"，不是"为什么崩"。
    #
    # 所以当某条假设被实证（CONFIRMED）且它的证据指向更早的位置时，
    # 把定位修正到那里 —— 这才是"根因定位"而非"崩溃点定位"。
    #
    # 只认 CONFIRMED：UNPROVEN 不允许动定位，宁可保守。
    report.location = _refine_location(report.location, report.conclusions, tracer)

    confirmed = sum(1 for h in hypotheses if h.verdict == Verdict.CONFIRMED)
    refuted = sum(1 for h in hypotheses if h.verdict == Verdict.REFUTED)
    tracer.end("verify", "ok",
               total=len(hypotheses), confirmed=confirmed, refuted=refuted)

    # ===== 输出 =====
    unproven_claims = [h.claim for h in report.conclusions
                       if h.verdict == Verdict.UNPROVEN]
    report.unknowns = list(facts.missing) + unproven_claims
    report.confidence = compute_confidence(report.conclusions, facts.missing)
    report.elapsed_ms = now_ms() - t0

    tracer.flush(outcome=_outcome(report), ledger=report.ledger)
    return report


def _pick_location(locations: list[SourceLocation], tracer) -> SourceLocation:
    """
    从已映射的位置里挑出最可能是根因的那个。

    **为什么不能无脑取 locations[0]**

    栈的第一帧是"崩溃指令所在的函数入口"，不是"出问题的那行"。
    在递归栈里这两者差得很远：实测 stack_overflow 样本展开出 34 帧，
    帧 0 指向函数入口（第 60 行），而 31 个帧都指向递归调用点（第 76 行）——
    后者才是根因。取第一帧会稳定偏差 16 行。

    **众数投票的判据**

    同一个 (file, line) 反复出现，说明执行流在这里循环/递归。
    真实崩溃里这是极强的根因信号，比"第一帧"可靠得多。

    但要设阈值，不能有个重复就认：只有两三个帧时，
    重复往往只是"函数入口 + 一层调用"的巧合。
    要求：该行出现 >= 3 次，且占比 >= 50%，才认定它是循环点。

    **什么时候仍然取第一帧**

    不满足上述条件时说明是普通的非递归栈（或者帧太少），
    此时第一帧仍是最合理的猜测 —— 崩溃指令就在那。
    不改变原有行为，只在递归场景下做修正。

    **但"第一帧"必须是真正的崩溃现场**

    曾经这里无脑取 locations[0]，而 locations 的排布取决于调用方
    把各类地址塞进栈的顺序。于是分配点一旦被排在崩溃点之前，
    就顶替它成了报告定位。

    实测 oob_write_01：根因是 479 行的 `len = r.end - r.start` 下溢，
    却定位到了 487 行的 `ZLabAlloc(64)` —— 偏差 8 行。
    原因就是给探测器加了"写穿活跃块"观测之后，分配点被插在了最前面，
    而它只是**推断线索**，不是崩溃现场。

    所以这里先按 role 过滤：只有 crash 帧才有资格当 locations[0]。
    分配点仍然留在列表里 —— 它要参与后续 refine，只是不能抢占定位。
    """
    if not locations:
        raise ValueError("locations 为空，调用方应先判空")

    # 分配点只是推断线索，不能冒充崩溃现场。
    # 但若**只有**分配点（崩溃帧一个都没映射上），仍得用它，否则无解。
    crash_locs = [x for x in locations if x.role == "crash"]
    primary = crash_locs or list(locations)

    if len(primary) < 3:
        return primary[0]

    counts: dict[tuple, int] = {}
    for loc in primary:
        key = (str(loc.path), loc.line)
        counts[key] = counts.get(key, 0) + 1

    top_key, top_n = max(counts.items(), key=lambda kv: (kv[1], -kv[0][1]))

    if top_n >= 3 and top_n / len(primary) >= 0.5:
        for loc in primary:
            if (str(loc.path), loc.line) == top_key:
                tracer.note(
                    f"检测到递归/循环栈：{top_n}/{len(primary)} 帧集中在 "
                    f"{loc.path}:{loc.line}，以它替代首帧作为根因位置")
                return loc

    return primary[0]


def _refine_location(current: SourceLocation,
                     conclusions: list[Hypothesis],
                     tracer) -> SourceLocation:
    """
    用实证结论把定位从"崩溃点"推进到"引入点"。

    **为什么需要这一步**

    栈只能回答"在哪崩的"。oob_write 的答案是 memset 那一行，
    但根因是传给它的 len 发生了 size_t 下溢 —— 在上一行。
    这两行的差别，就是"知道在哪崩"和"知道为什么崩"的差别。

    **为什么只看 CONFIRMED**

    UNPROVEN 表示"兼容但没锁定"。用它改定位等于用猜测覆盖实测，
    那还不如不改 —— 崩溃点至少是确定的事实。宁可保守。

    **为什么只取"更早"的位置**

    引入点必然在崩溃点之前（数据流上先算错，再用错）。
    若结论指向更靠后的行，说明它不是引入点，不该采纳。

    **但泄漏是反的**

    崩溃类：先算错（引入点），再崩 —— 引入点在**前**。
    泄漏类：先分配，再在某条分支上没释放就返回 ——
    泄漏点（那条早退 return）在分配点**之后**。

    实测 leak_01：分配点 278 行，泄漏点 `return -1;` 在 280 行。
    沿用"只取更早"会把结论直接丢掉，定位停在分配点，差 2 行即 MISS。

    所以方向由假设类型决定：leak 取更靠后的行，其余取更早的。
    这不是放宽判据，而是承认两类缺陷的数据流方向本就不同。
    """
    if current is None:
        return current

    for h in conclusions:
        if h.verdict != Verdict.CONFIRMED:
            continue
        # 泄漏的"泄漏点"在分配点之后，其余的"引入点"在崩溃点之前。
        want_later = (h.kind == "leak")

        # **但递归是第三种情形：引入点就在原地。**
        #
        # 无限递归的根因行就是那条递归调用本身 ——
        # 栈溢出时栈顶已经精确落在它上面，没有"更早的引入点"可言。
        # 沿用它取更早的规则，会把定位从递归调用行（446）
        # 推到函数定义行（430），把本来对的答案改错。
        # 竞争同理：冲突现场就是根因行。
        #
        # 这两类的规则只负责**证实**当前位置，不负责移动它。
        if h.kind in ("recursion", "race"):
            continue
        for ev in h.evidence:
            cand = _parse_locator(ev.locator)
            if cand is None:
                continue
            if not cand.same_file(current):
                continue
            if (cand.line > current.line) if want_later else (cand.line < current.line):
                tracer.note(
                    f"根因修正：{'分配点' if want_later else '崩溃点'} "
                    f"{current.line} 行 -> "
                    f"{'泄漏点' if want_later else '引入点'} "
                    f"{cand.line} 行（{h.claim}）")
                cand.context = current.context
                cand.symbol = current.symbol
                return cand
    return current


def _outcome(report: Report) -> dict:
    return {
        "status": report.status,
        "block_reason": report.block_reason,
        "confidence": report.confidence,
        "conclusions": len(report.conclusions),
        "unknowns": len(report.unknowns),
        "location": str(report.location) if report.location else None,
        "elapsed_ms": report.elapsed_ms,
    }


# ---------------------------------------------------------------- 报告渲染

def render(report: Report) -> str:
    """人类可读输出。顺序参考真实分析习惯：先给位置，再说机制。"""
    lines: list[str] = []

    if report.status == "blocked":
        lines.append(f"[阻塞] {report.block_reason}")
        return "\n".join(lines)

    lines.append(f"run: {report.run_id}  ({report.elapsed_ms} ms)")
    lines.append("")

    if report.location:
        lines.append(f"崩溃位置: {report.location}")
        if report.location.context:
            head = report.location.context.splitlines()[:3]
            lines.append("  " + "\n  ".join(x.strip() for x in head))
    else:
        lines.append("崩溃位置: 未能定位")
    lines.append("")

    if report.conclusions:
        lines.append("结论:")
        for h in report.conclusions:
            mark = "[实证]" if h.verdict == Verdict.CONFIRMED else "[未锁定]"
            lines.append(f"  {mark} {h.claim}")
            for e in h.evidence:
                lines.append(f"        └ {e}")
    else:
        lines.append("结论: 无（没有假设通过验证）")
    lines.append("")

    refuted = [h for h in report.ledger if h.verdict == Verdict.REFUTED]
    if refuted:
        lines.append("已推翻（保留以体现纠偏过程）:")
        for h in refuted:
            lines.append(f"  [推翻] {h.claim}")
            for e in h.evidence:
                lines.append(f"        └ {e}")
        lines.append("")

    if report.unknowns:
        lines.append("未知项:")
        for u in report.unknowns:
            lines.append(f"  - {u}")
        lines.append("")

    lines.append(f"置信度: {report.confidence:.2f}")
    return "\n".join(lines)
