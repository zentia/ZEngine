"""Mutation harness for the enableCollision / currentForce slice (#59).

What is under test
------------------
`PhysicsScene` grew `SetJointEnableCollision` / `GetJointEnableCollision` /
`GetJointForce`, plus the `GroupFilterTable` bookkeeping behind them (a 64-slot
sub-group pool, returned on both "switch it back on" and `RemoveJoint`).
`JointComponent` grew the `enableCollision` / `currentForce` script surface.

Why these particular mutants
----------------------------
* **"It returned true" is not evidence.** Every setter here can return `true`
  while doing nothing, so the gates are behavioural: EC3a watches whether an
  overlapping pair is actually pushed apart, EC10a/EC11a/EC14a/EC15a compare
  against a *known* load (1000 kg in a 9.81 m/s^2 -Z gravity = 9810 N).
* **Jolt's `CanCollide` order is counter-intuitive** (`GroupFilterTable`):
  invalid group -> collide; different group -> collide; *different filter
  pointer -> do NOT collide*; same sub-group -> do not collide; else the table.
  ECM02/ECM03/ECM04 break one of those three requirements; each of them leaves
  the pair colliding, which is exactly what EC3a sees.
* **A sub-group is a rented resource.** Two ways to leak it (never return on
  "enable", never return on `RemoveJoint`) and each needs its own gate: EC6
  (140 toggles) and EC7 (80 create+destroy cycles). 64 slots is the pool size,
  so both counts are deliberately above it.
* **"World space or constraint space" is a branch, not a convention.**
  `SixDOFConstraint::GetTotalLambdaPosition()` picks one of two
  implementations, so *both* directions of that branch need a fixture: EC11 is
  fully constrained (must NOT rotate), EC14 is not (must rotate). One fixture
  alone leaves one of ECM22/ECM23 alive.
* **The distance path has a scalar lambda and no stored axis.** The direction
  has to be recomputed from the two anchors every call; ECM25 replaces it with
  a constant and ECM26 drops the normalize.
* **EC46 is a *negative* gate** (currentTorque must stay unpublished), so its
  mutant is an *addition* -- ECM17 publishes it. A gate that can only ever pass
  is not a gate either.

Verdict (IMPORTANT)
-------------------
The baseline is NOT clean on this machine: UnityParitySmokeTest has 4
pre-existing audio-probe failures (AX14/AX15/AX16/AW1), so its exit code is
always 1. Judging "killed" by `rc == 0` therefore reports every mutant as
caught, including no-ops. The verdict is instead: **did the run introduce a
FAIL id that is absent from the baseline FAIL-id set**. The harness collects
that set at startup, re-runs the baseline to prove it is stable, and prints
only the newly-appeared failures for each mutant.

The generated `zengine.d.ts` is a second piece of mutable state: the three
DTS_MUTANTS leave a *mutated* published surface on disk, and later mutants do
not refresh it, so EC40a/EC42/EC46 stay red and would disguise any surviving
mutant as caught. The harness snapshots the clean d.ts bytes at baseline and
writes them back after every mutant (including on SIGTERM/SIGINT).

Run:  py .mutate_ec.py            (all)
      py .mutate_ec.py ECM01      (subset)
      py .mutate_ec.py --resume   (skip mutants that already have a verdict)
"""
import io
import os
import re
import shutil
import signal
import subprocess
import sys
import time

# path -> backup，供信号处理器在中途被杀时还原（见 main 里的注释）。
RESTORE = {}

# ⚠️ 基线（未变异）时两个 exe 各自**本来就会红**的 FAIL id 集合。
# 本机 UnityParitySmokeTest 基线是 3517 checks / 4 failures，4 条全是
# AX14/AX15/AX16/AW1（音频 probe asset，与 Joint 切片无关）⇒ rc 恒为 1。
# 早期版本拿 `rc == 0` 判 killed，结果 25 个变异体（哪怕 no-op）全被误报
# caught。正确判据是「有没有冒出基线里没有的 FAIL id」，见 run()。
BASELINE = {"parity": None, "comp": None}
FAIL_RE = re.compile(r"\[FAIL\]\s+(\S+)")

# 基线（干净源码）生成出来的三份 d.ts 内容。每跑完一个变异体都要写回去 ——
# ECM14/ECM15/ECM17 会改脚本发布面，`regen_dts()` 把*变异后*的 d.ts 留在盘上，
# 而后续变异体 NEED_DTS=False 不刷新 ⇒ EC40a/EC42/EC46 从 ECM16 起恒红。
# 那会把任何**存活**的变异体伪装成 caught（又一种假绿）。
DTS_CLEAN = {}

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


SCENE_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsScene.cpp")
NULL_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsSceneNull.cpp")
JOINT_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/JointComponent.cpp")
JOINT_H = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/JointComponent.h")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

END_OF_FUNC = "\n}\n"
END_OF_CLASS_FUNC = "\n    }\n"


def marker_for(path):
    return END_OF_CLASS_FUNC if path.endswith((".h",)) else END_OF_FUNC


TARGETS = ("ZRuntime", "ScriptTsChainSmokeTest", "UnityParitySmokeTest", "ScriptComponentSmokeTest")

# 判一个**后端 / 组件**变异体只需要这两个。另外两个（各要再 link 一遍 ZRuntime）
# 只在变异体碰得到脚本发布面时才编 —— 见 build_and_run 里的 NEED_SCRIPT。
BASE_TARGETS = ("ZRuntime", "UnityParitySmokeTest")

DTS_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptTsChainSmokeTest.exe"
DTS_PATHS = (
    r"engine\EditorScripts\typings\zengine.d.ts",
    r"engine\Intermediate\Typings\zengine.d.ts",
    r"engine\Source\Runtime\Scripting\ScriptVM\Test\Typings\zengine.d.ts",
)

# 只有这三个变异体会改动 d.ts 的内容（其余改的是行为或桩）。d.ts 是生成物、
# 不入 git，所以刷新它必须伴随一次 ScriptTsChainSmokeTest 的重建。
DTS_MUTANTS = {"ECM14", "ECM15", "ECM17"}

NEED_RELINK = False
NEED_DTS = False
NEED_SCRIPT = False


def force_relink():
    """清增量链接状态文件（.ilk）。

    现在三个 exe 在 CMake 里都挂了 `$<$<CONFIG:Debug>:/INCREMENTAL:NO>`，这里是
    no-op，留着只为清旧 build 目录的残留。只删 .ilk，用 os.remove 直接删，
    不进回收站。
    """
    for rel in (
        r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptTsChainSmokeTest.ilk",
        r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.ilk",
        r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.ilk",
    ):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass


def regen_dts():
    """刷 d.ts：只跑一遍 `--dump-dts` 再 copy 两份（原先是起三遍 QuickJS）。"""
    first = DTS_PATHS[0]
    r = subprocess.run([os.path.join(ROOT, DTS_EXE), "--dump-dts", os.path.join(ROOT, first)],
                       cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        print("  DTS REGEN FAILED rc=%d %s" % (r.returncode, (r.stdout or "")[:120]))
        return False
    for rel in DTS_PATHS[1:]:
        shutil.copy2(os.path.join(ROOT, first), os.path.join(ROOT, rel))
    return True


def snapshot_dts_clean():
    """基线编完、d.ts 刷好之后，把三份内容记下来，供每个变异体收尾时写回。"""
    DTS_CLEAN.clear()
    for rel in DTS_PATHS:
        with io.open(os.path.join(ROOT, rel), "rb") as f:
            DTS_CLEAN[rel] = f.read()


def restore_dts_clean():
    """把三份 d.ts 逐字节写回基线内容。

    ⚠️ 不能用"重新跑一遍生成器"来还原：DTS 变异体之后 `ScriptTsChainSmokeTest.exe`
    是用**变异后**的头文件编出来的，后续 NEED_DTS=False 的变异体不会重建它，
    生成器自己就是脏的 ⇒ 越刷越脏。直接写回内容才与生成器状态解耦。
    """
    for rel, data in DTS_CLEAN.items():
        p = os.path.join(ROOT, rel)
        with io.open(p, "wb") as f:
            f.write(data)
        os.utime(p, None)


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    if old == new:
        raise SystemExit("NO-OP MUTANT in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else marker_for(path)
    j = text.find(marker, i)
    if j < 0:
        raise SystemExit("END OF FUNC NOT FOUND: %r" % func_sig)
    region = text[i:j]
    if old not in region:
        raise SystemExit("PATTERN NOT FOUND in %s [%s]:\n%r" % (path, func_sig, old))
    if old == new:
        raise SystemExit("NO-OP MUTANT in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(
        text[:i] + region.replace(old, new, 1) + text[j:])


def run(exe, baseline=None):
    """跑一次 exe，返回 (rc, fail_ids, has_summary, new_ids)。

    ⚠️ **调用方绝不能只看 rc**：基线本身就有 4 条 AX* 失败，rc 恒为 1。
    只判 rc 的话，任何变异体（包括等价于 no-op 的）都会变成 "caught"。
    `baseline` 为 None 时是基线采集，会把所有 FAIL id 都打出来；否则只打印
    **新增**的那些 —— 25 个变异体跑下来，输出里只该看到该变异体真正触发的门。
    """
    r = subprocess.run([os.path.join(ROOT, exe)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, errors="replace", timeout=300)
    lines = (r.stdout or "").splitlines()
    ids = frozenset(m.group(1) for l in lines for m in (FAIL_RE.search(l),) if m)
    tail = [l for l in lines if ("checks," in l or "项检查" in l)]
    has_summary = bool(tail)
    base = baseline if baseline is not None else frozenset()
    new = sorted(ids - base)
    print("  %-30s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    if baseline is not None and not new:
        print("      (no NEW failures -- only the %d known baseline ones)" % len(base))
    shown = 0
    for l in lines:
        m = FAIL_RE.search(l)
        if m and m.group(1) in new:
            print("      " + l.strip())
            shown += 1
            if shown >= 6:
                break
    return r.returncode, ids, has_summary, new


def _cmake_build(targets, parallel):
    argv = ["cmake", "--build", ".", "--target"] + list(targets) + ["--config", "Debug"]
    if parallel:
        # 用 CMake 自己的开关，不要手写 `-- /m`：Git Bash 会把 `/m` 转成路径，
        # MSBuild 直接 MSB1008。
        argv.append("--parallel")
    return subprocess.run(argv, cwd=os.path.join(ROOT, "build"),
                          env=ENV, capture_output=True, text=True, timeout=1800)


# ⚠️ 构建失败分两类，混在一起重试就是白烧时间：
#   * 真编译/链接错误 —— 确定的，重试多少次都一样（CFM02 曾按老逻辑烧 12 次）。
#   * 文件锁 / 拷贝竞态 —— 重试能过，这才是串行兜底该出手的地方。
FLAKY = ("LNK1168", "LNK1104", "LNK1201",          # 文件被占 / 打不开
         "MSB3021", "MSB3026", "MSB3027",           # copy 失败
         "MSB3491", "MSB3721",
         "The process cannot access the file")
HARD_ERROR = (": error C", ": error LNK", ": error MSB", "fatal error C")


def _out(p):
    return (p.stdout or "") + (p.stderr or "")


def _classify(p):
    """'flaky' = 重试有意义；'hard' = 编译/链接错，再试也是同一个错。"""
    out = _out(p)
    if any(s in out for s in FLAKY):
        return "flaky"
    if any(s in out for s in HARD_ERROR):
        return "hard"
    return "unknown"


def build(refresh_dts=True, targets=None):
    """一次 `cmake --build` 编完 target，并行。

    只在**值得**的时候重试：编译错直接放弃，文件锁/拷贝竞态才走串行兜底。
    """
    targets = tuple(targets) if targets else TARGETS
    last = None
    if NEED_RELINK:
        force_relink()
    for attempt in range(2):
        p = _cmake_build(targets, parallel=True)
        last = p
        if p.returncode == 0:
            if refresh_dts and not regen_dts():
                return last
            return last
        if "LNK1236" in _out(p):
            _cmake_build(("ZRuntime",), parallel=True)
        kind = _classify(p)
        if kind == "hard":
            # 编不过就是编不过 —— 这个变异体判 "invalid (does not build)"。
            return last
        if attempt == 0:
            continue
        print("  (build flaked -- falling back to sequential)")
        for target in targets:
            p = _cmake_build((target,), parallel=False)
            last = p
            if p.returncode != 0:
                break
        else:
            if refresh_dts and not regen_dts():
                return last
        return last
    return last


def build_and_run():
    # 只有可能改动脚本发布面的变异体才需要那两个脚本 exe —— 它们要 link 一遍
    # ZRuntime，而后端/组件的变异体用 UnityParitySmokeTest 一个就能判。
    targets = TARGETS if NEED_SCRIPT else BASE_TARGETS
    p = build(refresh_dts=NEED_DTS, targets=targets)
    if p is None or p.returncode != 0:
        print("  BUILD FAILED rc=%d" % (p.returncode if p is not None else -1))
        return "BUILD FAILED"
    _, ids_parity, ok_parity, new_parity = run(PARITY_EXE, BASELINE["parity"])
    ok_comp, new_comp = True, []
    if NEED_SCRIPT:
        _, _, ok_comp, new_comp = run(COMP_EXE, BASELINE["comp"])
    if not ok_parity or not ok_comp:
        # 崩溃 / 没打汇总行：与基线不同，算 caught，但要单独标出来，别混进
        # "被某条门抓住"里 —— 崩溃也可能是 harness 自己把 exe 弄坏了。
        print("  >>> CAUGHT (no summary) <<<")
        return "caught (no summary)"
    if not new_parity and not new_comp:
        # ⚠️ 这里**不能**改成看 rc：基线 rc=1，任何变异体都会走 else 分支。
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


# ---------------------------------------------------------------------------
# 变异体
# ---------------------------------------------------------------------------
MUTATIONS = [
    # =========================================================================
    # enableCollision：三条 CanCollide 前提（同 filter / 同 group / 同 sub-group）
    # =========================================================================
    ("ECM01 the filter bookkeeping happens but the two bodies never get a collision group",
     SCENE_CPP, "bool PhysicsScene::ApplyJointCollisionFilter(uint32_t joint_id, bool in_enable)",
     "    const JPH::CollisionGroup group(m_JointGroupFilter.GetPtr(), kJointCollisionGroup, slot);\n"
     "    body_interface.SetCollisionGroup(JPH::BodyID(body_a), group);\n"
     "    body_interface.SetCollisionGroup(JPH::BodyID(body_b), group);",
     "    const JPH::CollisionGroup group(m_JointGroupFilter.GetPtr(), kJointCollisionGroup, slot);\n"
     "    (void)group;"),

    ("ECM02 only body A gets the group (different filter pointer => Jolt makes them collide anyway)",
     SCENE_CPP, "bool PhysicsScene::ApplyJointCollisionFilter(uint32_t joint_id, bool in_enable)",
     "    body_interface.SetCollisionGroup(JPH::BodyID(body_a), group);\n"
     "    body_interface.SetCollisionGroup(JPH::BodyID(body_b), group);",
     "    body_interface.SetCollisionGroup(JPH::BodyID(body_a), group);"),

    ("ECM03 the two bodies land in different GROUP ids (Jolt: different group => collide)",
     SCENE_CPP, "bool PhysicsScene::ApplyJointCollisionFilter(uint32_t joint_id, bool in_enable)",
     "    body_interface.SetCollisionGroup(JPH::BodyID(body_b), group);",
     "    body_interface.SetCollisionGroup(\n"
     "        JPH::BodyID(body_b),\n"
     "        JPH::CollisionGroup(m_JointGroupFilter.GetPtr(), kJointCollisionGroup + 1, slot));"),

    ("ECM04 the two bodies land in different SUB-GROUPs (the table defaults to all-enabled)",
     SCENE_CPP, "bool PhysicsScene::ApplyJointCollisionFilter(uint32_t joint_id, bool in_enable)",
     "    body_interface.SetCollisionGroup(JPH::BodyID(body_b), group);",
     "    body_interface.SetCollisionGroup(\n"
     "        JPH::BodyID(body_b),\n"
     "        JPH::CollisionGroup(m_JointGroupFilter.GetPtr(), kJointCollisionGroup,\n"
     "                            (slot + 1) % kMaxJointSubGroups));"),

    # =========================================================================
    # sub-group 是租来的：两条泄漏路径各要一条门
    # =========================================================================
    ("ECM05 switching enableCollision back on forgets to return the slot",
     SCENE_CPP, "bool PhysicsScene::ApplyJointCollisionFilter(uint32_t joint_id, bool in_enable)",
     "        m_SubGroupUsed[it->second] = false;\n"
     "        m_JointSubGroups.erase(it);",
     "        m_JointSubGroups.erase(it);"),

    ("ECM06 RemoveJoint forgets to return the slot (80 create+destroy cycles drain 64 slots)",
     SCENE_CPP, "bool PhysicsScene::RemoveJoint(uint32_t joint_id)",
     "    if (m_JointSubGroups.find(joint_id) != m_JointSubGroups.end())\n"
     "    {\n"
     "        ApplyJointCollisionFilter(joint_id, true);\n"
     "    }",
     "    if (false)\n"
     "    {\n"
     "        ApplyJointCollisionFilter(joint_id, true);\n"
     "    }"),

    # =========================================================================
    # 读 / 写访问器
    # =========================================================================
    ("ECM07 GetJointEnableCollision always reports true (an off joint reads back as on)",
     SCENE_CPP, "bool PhysicsScene::GetJointEnableCollision(uint32_t joint_id, bool& out_enable) const",
     "    out_enable = (m_JointSubGroups.find(joint_id) == m_JointSubGroups.end());",
     "    out_enable = true;"),

    ("ECM08 SetJointEnableCollision ignores the flag and always turns collision OFF",
     SCENE_CPP, "bool PhysicsScene::SetJointEnableCollision(uint32_t joint_id, bool in_enable)",
     "    return ApplyJointCollisionFilter(joint_id, in_enable);",
     "    return ApplyJointCollisionFilter(joint_id, false);"),

    # =========================================================================
    # JointComponent
    # =========================================================================
    ("ECM09 PushEnableCollision is a no-op (the component remembers the field, the backend never hears it)",
     JOINT_CPP, "void JointComponent::PushEnableCollision()",
     "    physics_scene->SetJointEnableCollision(m_JointId, m_JointRes.m_EnableCollision);",
     "    if (true) { (void)0; } else physics_scene->SetJointEnableCollision(m_JointId, m_JointRes.m_EnableCollision);"),

    ("ECM10 BuildJoint stops pushing enableCollision (Unity's default is OFF, the backend default is ON)",
     JOINT_CPP, "bool JointComponent::BuildJoint()",
     "    PushEnableCollision();\n"
     "    ApplyJointSettings();",
     "    ApplyJointSettings();"),

    ("ECM11 SetEnableCollision pushes to the backend but forgets to store the field",
     JOINT_CPP, "void JointComponent::SetEnableCollision(bool enable)",
     "    m_JointRes.m_EnableCollision = enable;",
     "    (void)enable;"),

    ("ECM12 SetEnableCollision rebuilds the joint (it must not -- the backend patches two collision groups)",
     JOINT_CPP, "void JointComponent::SetEnableCollision(bool enable)",
     "    m_JointRes.m_EnableCollision = enable;\n"
     "    // 不重建：后端是事后改两个 body 的碰撞组。\n"
     "    PushEnableCollision();",
     "    m_JointRes.m_EnableCollision = enable;\n"
     "    RebuildJoint();"),

    ("ECM13 the component default flips to true (Unity's default is false)",
     JOINT_H, None,
     "    bool m_EnableCollision {false};",
     "    bool m_EnableCollision {true};"),

    # =========================================================================
    # 发布面 / 接线
    # =========================================================================
    ("ECM14 the enableCollision script property is renamed away",
     JOINT_H, None,
     'ZSCRIPTPROPERTY(Name="enableCollision"',
     'ZSCRIPTPROPERTY(Name="enableCollisionRenamed"'),

    ("ECM15 the currentForce script property is renamed away",
     JOINT_H, None,
     'ZSCRIPTPROPERTY(Name="currentForce"',
     'ZSCRIPTPROPERTY(Name="currentForceRenamed"'),

    ("ECM16 the no-physics build loses GetJointForce",
     NULL_CPP, None,
     "PhysicsScene::GetJointForce(uint32_t, Vector3& out_force) const",
     "PhysicsScene::GetJointForceRenamed(uint32_t, Vector3& out_force) const"),

    # ⚠️ EC46 是条**否定门**（currentTorque 刻意不发布），所以它的变异体是一次
    # *添加*。只能通过的门不算门。
    ("ECM17 currentTorque gets published after all (its direction is not recoverable)",
     JOINT_H, None,
     'ZSCRIPTPROPERTY(Name="currentForce", Getter="GetCurrentForce")',
     'ZSCRIPTPROPERTY(Name="currentTorque", Getter="GetCurrentForce")\n'
     '    ZSCRIPTPROPERTY(Name="currentForce", Getter="GetCurrentForce")'),

    # =========================================================================
    # currentForce：量纲 / 符号
    # =========================================================================
    ("ECM20 currentForce forgets to divide by the step (it reports an impulse, ~163 N)",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "    out_force = (-toVec3(world_impulse)) / step;",
     "    out_force = -toVec3(world_impulse);"),

    ("ECM21 currentForce has the sign the other way round (it is the force on the OTHER body)",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "    out_force = (-toVec3(world_impulse)) / step;",
     "    out_force = toVec3(world_impulse) / step;"),

    ("ECM22 the step is the frequency, not 1/frequency",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "    const float step = 1.f / m_Config.m_UpdateFrequency;",
     "    const float step = m_Config.m_UpdateFrequency;"),

    ("ECM23 an unknown joint answers 'zero force' instead of being refused",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "    const JPH::Constraint* jph_constraint = JointAt(joint_id);\n"
     "    if (jph_constraint == nullptr)\n"
     "    {\n"
     "        return false;\n"
     "    }",
     "    const JPH::Constraint* jph_constraint = JointAt(joint_id);\n"
     "    if (jph_constraint == nullptr)\n"
     "    {\n"
     "        out_force = Vector3::ZERO;\n"
     "        return true;\n"
     "    }"),

    # =========================================================================
    # SixDOF 那条"世界空间 / 约束空间"二选一：两个方向都要有夹具
    # =========================================================================
    ("ECM24 the six-DOF branch always rotates (a fully-constrained joint is ALREADY in world space)",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "            if (!fully_constrained)",
     "            if (true)"),

    ("ECM25 the six-DOF branch never rotates (a partially-constrained joint stays in constraint space)",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "            if (!fully_constrained)",
     "            if (false)"),

    # =========================================================================
    # distance：只有一个标量，方向要自己取
    # =========================================================================
    ("ECM26 the distance path uses a constant axis instead of the anchor line",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "                world_impulse = axis.Normalized() * distance->GetTotalLambdaPosition();",
     "                world_impulse = JPH::Vec3::sAxisY() * distance->GetTotalLambdaPosition();"),

    ("ECM27 the distance direction is not normalized (a 2 m anchor line doubles the force)",
     SCENE_CPP, "bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const",
     "                world_impulse = axis.Normalized() * distance->GetTotalLambdaPosition();",
     "                world_impulse = axis * distance->GetTotalLambdaPosition();"),
]

LOCK = os.path.join(BAK, "zengine_mutate_ec.lock")


def lock_is_stale():
    """锁文件存在**不代表真有人在跑** —— 上次被 SIGTERM 打断时锁就留下了。
    判据两条：PID 还在不在，以及锁的年龄（PID 可能被复用，所以要有时间兜底）。"""
    try:
        with io.open(LOCK, "r", encoding="utf-8") as f:
            pid = int((f.read().strip() or "0"))
    except (OSError, ValueError):
        return True
    if pid <= 0:
        return True
    try:
        os.kill(pid, 0)          # 存在性探测，不真的发信号
    except OSError:
        return True              # 进程已经没了
    return (time.time() - os.path.getmtime(LOCK)) > 6 * 3600


def main():
    if os.path.exists(LOCK):
        if lock_is_stale():
            print("! removing a stale lock (%s) -- a previous run was killed" % LOCK)
            os.remove(LOCK)
        else:
            raise SystemExit("another .mutate_ec.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    args = sys.argv[1:]
    # `--resume`：日志里已经有结论的变异体跳过。中途被打断（或者只想重跑存活的
    # 那几个）时不用从头再来。
    resume = "--resume" in args
    only = set(a for a in args if not a.startswith("--"))
    done = set()
    if resume:
        log = os.path.join(ROOT, ".mutate_ec.log")
        if os.path.exists(log):
            for line in io.open(log, "r", encoding="utf-8"):
                parts = line.split()
                if parts:
                    done.add(parts[0])
            print("! --resume: %d mutants already have a verdict, skipping them" % len(done))
    results = []
    touched = set()

    # ⚠️ 内层的 `finally` 挡不住 SIGTERM / Ctrl-C：信号直接终止进程，不抛异常，
    # 于是**变异体就留在源码里了**。上一次这么留下的残留害得白改四轮夹具，所以
    # 这里显式接管信号，死之前一定把源码还原回去。
    def _restore_and_die(signum, _frame):
        sys.stderr.write("\n!! signal %d -- restoring sources before exit\n" % signum)
        for path, bak in RESTORE.items():
            try:
                shutil.copy2(bak, path)
                os.utime(path, None)
            except OSError:
                pass
        try:
            restore_dts_clean()
        except OSError:
            pass
        try:
            os.remove(LOCK)
        except OSError:
            pass
        sys.exit(1)

    signal.signal(signal.SIGTERM, _restore_and_die)
    signal.signal(signal.SIGINT, _restore_and_die)

    try:
        # 基线：先把源码按**未变异**的状态编一遍并刷一次 d.ts。既然多数变异体
        # 都不刷 d.ts 了，这一遍就是整个 run 里唯一保证 d.ts 与当前源码一致的机会。
        print("=== baseline (clean sources) ===")
        b = build(refresh_dts=True)
        if b is None or b.returncode != 0:
            raise SystemExit("! baseline build FAILED -- aborting (results would be meaningless)")
        _, ids_p, ok_p, _ = run(PARITY_EXE)
        _, ids_c, ok_c, _ = run(COMP_EXE)
        if not ok_p or not ok_c:
            raise SystemExit("! baseline exe did not print a summary -- aborting")
        snapshot_dts_clean()
        BASELINE["parity"] = ids_p
        BASELINE["comp"] = ids_c
        print("  baseline FAIL ids: parity=%s comp=%s" %
              (sorted(ids_p), sorted(ids_c)))
        # 稳定性自检：同一份源码连跑两次，FAIL 集合必须**完全一致**。只要有一条
        # 门是抖的，后面"新增了 FAIL id"就可能是抖动而非变异体 ⇒ 全部结论作废。
        _, ids_p2, _, _ = run(PARITY_EXE, ids_p)
        if ids_p2 != ids_p:
            raise SystemExit("! baseline FAIL set is flaky (%s vs %s) -- aborting" %
                             (sorted(ids_p), sorted(ids_p2)))
        print("  (a mutant is killed ONLY if it introduces a FAIL id absent from those sets)")
        print()

        for entry in MUTATIONS:
            end_marker = None
            if len(entry) == 6:
                name, path, func_sig, old, new, end_marker = entry
            elif len(entry) == 5:
                name, path, func_sig, old, new = entry
            else:
                name, path, old, new = entry
                func_sig = None
            if only and name.split()[0] not in only:
                continue
            if name.split()[0] in done:
                continue
            print("=== %s ===" % name)
            global NEED_DTS, NEED_SCRIPT
            NEED_DTS = name.split()[0] in DTS_MUTANTS
            NEED_SCRIPT = NEED_DTS
            bak = os.path.join(BAK, os.path.basename(path) + ".ecbak")
            shutil.copy2(path, bak)
            RESTORE[path] = bak
            try:
                try:
                    if func_sig is not None:
                        mutate_in(path, func_sig, old, new, end_marker)
                    else:
                        mutate(path, old, new)
                except SystemExit as e:
                    print("  HARNESS ERROR: %s" % e)
                    results.append((name, "harness error (anchor not found)"))
                    continue
                verdict = "BUILD FAILED"
                for _ in range(3):
                    verdict = build_and_run()
                    if verdict != "BUILD FAILED":
                        break
                    print("  (retrying -- flaky MSBuild)")
                if verdict == "BUILD FAILED":
                    verdict = "invalid (does not build)"
                results.append((name, verdict))
                with io.open(os.path.join(ROOT, ".mutate_ec.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
                # 源码还原了，d.ts 也要还原：DTS 变异体把*变异后*的发布面留在
                # 盘上，后面每个 NEED_DTS=False 的变异体都会读到它 ⇒ EC40a /
                # EC42 / EC46 恒红 ⇒ 存活的变异体被伪装成 caught。
                restore_dts_clean()
            print()
        for path in touched:
            os.utime(path, None)
        build(refresh_dts=True)      # 收尾：源码已还原，重建 + 刷回正确的 d.ts
        print("---- summary ----")
        for n, r in results:
            print("%-70s %s" % (n, r))
        killed = [n for n, r in results
                  if r == "invalid (does not build)" or r.startswith("caught")]
        bad = [n for n, r in results if n not in killed]
        print("\n%d/%d killed (%d caught by a test, %d do not build, %d crashed)" %
              (len(killed), len(results),
               len([n for n, r in results if r == "caught"]),
               len([n for n, r in results if r == "invalid (does not build)"]),
               len([n for n, r in results if r == "caught (no summary)"])))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
