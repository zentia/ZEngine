"""Mutation harness for the Unity `Input` slice (#51).

What is under test
------------------
The backend (`EnhancedInputSystem`) gained text input, touch phases, tap
counting and `ResetInputAxes`; the facade (`ScriptInput`) gained Unity's legacy
`GetAxis` / `GetButton` surface plus the device-capability and settings
properties.

Why these particular mutants
----------------------------
* **UTF-8 by hand is a shift-by-one factory.** `0xC0 | (cp >> 6)` vs `>> 5` still
  produces a "valid looking" byte for some ranges and is invisible on ASCII --
  only a 2-byte and a 4-byte sample pin the boundaries down (EN1/EN4/EN42).
* **`OnChar` must drop control characters and lone surrogates.** Keeping them
  turns a WM_CHAR stream into an input box full of newlines, and encoding a
  surrogate produces a sequence no decoder will accept.
* **`Stationary` vs `Moved`.** Hosts push a move event every frame whether the
  finger moved or not; without the "did it actually move" flag every resting
  finger reports Moved forever, and `deltaTime`-driven logic never settles.
* **Ended touches must live exactly one frame.** Drop them immediately and
  `touchCount` never sees the release; keep them and `touchCount` only grows.
* **`GetAxis` vs `GetAxisRaw` is the whole point of having two methods.** Zeroing
  Gravity or Sensitivity collapses them into the same function -- which is the
  most likely way to "simplify" this code later.
* **`ResetInputAxes` zeroes axes, not keys.** Clearing `m_PressedKeys` as well
  would drop every held key on the frame a menu opens.
* **The capability flags are platform facts.** `touchSupported = true` on desktop
  sends scripts down a path that can never receive an event.

Run:  py .mutate_ip.py            (all)
      py .mutate_ip.py IP01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


EN_CPP = S(r"engine/Source/Runtime/Function/Input/EnhancedInputSystem.cpp")
EN_H = S(r"engine/Source/Runtime/Function/Input/EnhancedInputSystem.h")
IN_H = S(r"engine/Source/Runtime/Scripting/ScriptInput/ScriptInput.h")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# .cpp 里的目标函数在文件作用域，函数收尾是顶格的 `}`，默认标记就行。
END_OF_FUNC = "\n}\n"
# ⚠️ ScriptInput.h 是个**类**：函数体是 8 空格，函数收尾是 4 空格的 `}`。用顶格
# 标记会一路跑到下一个顶格 `}`（也就是类收尾之外），把无关代码也改掉。
END_OF_CLASS_FUNC = "\n    }\n"

# ⚠️ 一次只编一个 target：同时给 `--target A B` 会让 MSBuild 并行跑，某个 target
# 的 post-build 往 bin/Debug/ZRuntime.lib 拷贝时正好撞上另一个的链接 —— 间歇性
# LNK1236。撞上了就先单编一次 ZRuntime 再重来。
TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    if end_marker is not None:
        marker = end_marker
    elif path.endswith("ScriptInput.h"):
        marker = END_OF_CLASS_FUNC
    else:
        marker = END_OF_FUNC
    j = text.find(marker, i)
    if j < 0:
        raise SystemExit("END OF FUNC NOT FOUND: %r" % func_sig)
    region = text[i:j]
    if old not in region:
        raise SystemExit("PATTERN NOT FOUND in %s [%s]:\n%r" % (path, func_sig, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(
        text[:i] + region.replace(old, new, 1) + text[j:])


def run(exe):
    r = subprocess.run([os.path.join(ROOT, exe)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, errors="replace", timeout=300)
    out = r.stdout or ""
    fails = [l for l in out.splitlines() if "FAIL" in l]
    tail = [l for l in out.splitlines() if ("checks," in l or "项检查" in l)]
    print("  %-30s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    for l in fails[:4]:
        print("      " + l.strip())
    return r.returncode


def build():
    last = None
    for _ in range(3):
        ok = True
        for target in TARGETS:
            p = subprocess.run(["cmake", "--build", ".", "--target", target,
                                "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                               env=ENV, capture_output=True, text=True)
            last = p
            if p.returncode != 0:
                ok = False
                # LNK1236 是 ZRuntime.lib 的 COFF 段被写坏了（MSBuild 并行撞
                # 车），不是代码错。单编一次 ZRuntime 再继续。
                if "LNK1236" in (p.stdout or ""):
                    subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime",
                                    "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                                   env=ENV, capture_output=True, text=True)
                break
        if ok:
            return last
    return last


def build_and_run():
    p = build()
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # =========================================================================
    # OnChar —— 手写 UTF-8 编码
    # =========================================================================
    ("IP01 OnChar keeps lone surrogates (encodes them into a broken sequence)",
     EN_CPP, "void EnhancedInputSystem::OnChar(unsigned int codepoint)",
     "    if (codepoint >= 0xD800u && codepoint <= 0xDFFFu)\n    {\n        return;\n    }",
     "    if (false)\n    {\n        return;\n    }"),

    ("IP02 OnChar lets control characters into inputString",
     EN_CPP, "void EnhancedInputSystem::OnChar(unsigned int codepoint)",
     "    if (codepoint < 0x20u || codepoint == 0x7Fu)\n    {\n        return;\n    }",
     "    if (false)\n    {\n        return;\n    }"),

    # 位移错一格：2 字节档变成 C3 之外的东西，ASCII 与 3 字节档都测不出来。
    ("IP03 the 2-byte UTF-8 form shifts by 5 instead of 6",
     EN_CPP, "void EnhancedInputSystem::OnChar(unsigned int codepoint)",
     '        m_InputStringAccum.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));',
     '        m_InputStringAccum.push_back(static_cast<char>(0xC0u | (codepoint >> 5)));'),

    ("IP04 the 3-byte UTF-8 form shifts by 13 instead of 12",
     EN_CPP, "void EnhancedInputSystem::OnChar(unsigned int codepoint)",
     '        m_InputStringAccum.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));',
     '        m_InputStringAccum.push_back(static_cast<char>(0xE0u | (codepoint >> 13)));'),

    ("IP05 the 4-byte UTF-8 form shifts by 17 instead of 18",
     EN_CPP, "void EnhancedInputSystem::OnChar(unsigned int codepoint)",
     '        m_InputStringAccum.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));',
     '        m_InputStringAccum.push_back(static_cast<char>(0xF0u | (codepoint >> 17)));'),

    ("IP06 EndFrame does not clear the inputString accumulator",
     EN_CPP, "void EnhancedInputSystem::EndFrame()",
     "    m_InputStringAccum.clear();",
     "    ;"),

    # =========================================================================
    # 触摸
    # =========================================================================
    ("IP07 a press never counts as more than tap 1 (no double-tap detection)",
     EN_CPP, "void EnhancedInputSystem::OnTouch(int pointer_id, int action, float x, float y)",
     "                point.tap_count         = ResolveTapCount(pointer_id, x, y, now_s);",
     "                point.tap_count         = 1;"),

    ("IP08 a move event always reports Moved (moved_this_frame is never set)",
     EN_CPP, "void EnhancedInputSystem::OnTouch(int pointer_id, int action, float x, float y)",
     "                    it->second.moved_this_frame = true;",
     "                    ;"),

    ("IP09 an ended touch is dropped immediately (invisible on the release frame)",
     EN_CPP, "void EnhancedInputSystem::OnTouch(int pointer_id, int action, float x, float y)",
     "                m_EndedThisFrame.emplace_back(pointer_id, ended);",
     "                ;"),

    ("IP10 a cancel is reported as Ended",
     EN_CPP, "void EnhancedInputSystem::OnTouch(int pointer_id, int action, float x, float y)",
     "                ended.phase = (touch_action == TouchAction::Up) ? TouchPhase::Ended\n"
     "                                                                : TouchPhase::Canceled;",
     "                ended.phase = TouchPhase::Ended;"),

    ("IP11 EndFrame keeps the ended touches forever (touchCount only grows)",
     EN_CPP, "void EnhancedInputSystem::EndFrame()",
     "    m_EndedThisFrame.clear();",
     "    ;"),

    ("IP12 EndFrame never collapses a non-moving Moved into Stationary",
     EN_CPP, "void EnhancedInputSystem::EndFrame()",
     "        if (entry.second.phase == TouchPhase::Moved && !entry.second.moved_this_frame)",
     "        if (false)"),

    ("IP13 the tap slop is ignored (a press across the screen still counts as tap 2)",
     EN_CPP, "int EnhancedInputSystem::ResolveTapCount(int pointer_id, float x, float y, double now_s) const",
     "    if (dx * dx + dy * dy > kDoubleTapSlopPx * kDoubleTapSlopPx)",
     "    if (false)"),

    ("IP14 the double-tap window is ignored (any later press keeps counting up)",
     EN_CPP, "int EnhancedInputSystem::ResolveTapCount(int pointer_id, float x, float y, double now_s) const",
     "    if (now_s - time_it->second > kDoubleTapSeconds)",
     "    if (false)"),

    ("IP15 the tap counter does not advance (always restarts at 1)",
     EN_CPP, "int EnhancedInputSystem::ResolveTapCount(int pointer_id, float x, float y, double now_s) const",
     "    return previous >= 10 ? 10 : previous + 1;",
     "    return previous >= 10 ? 10 : previous;"),

    ("IP16 GetTouchAt enumerates ended touches before live ones",
     EN_CPP, "bool EnhancedInputSystem::GetTouchAt(int index, Touch& out_touch) const",
     "    if (index < static_cast<int>(m_Touches.size()))",
     "    if (false)"),

    ("IP17 MakeTouch reports pressure 0 on devices that cannot report it",
     EN_CPP, "Touch EnhancedInputSystem::MakeTouch(int pointer_id, const TouchPoint& point)",
     "    touch.pressure      = point.pressure;",
     "    touch.pressure      = 0.0f;"),

    ("IP18 MakeTouch zeroes deltaPosition",
     EN_CPP, "Touch EnhancedInputSystem::MakeTouch(int pointer_id, const TouchPoint& point)",
     "    touch.deltaPosition = Vector2(point.frame_dx, point.frame_dy);",
     "    touch.deltaPosition = Vector2::ZERO;"),

    # =========================================================================
    # 轴 / 任意键
    # =========================================================================
    ("IP19 ResetInputAxes also releases every held key",
     EN_CPP, "void EnhancedInputSystem::ResetInputAxes()",
     "    m_AccumulatedValues.clear();",
     "    m_AccumulatedValues.clear();\n    m_PressedKeys.clear();"),

    ("IP20 ResetInputAxes clears only the accumulator, not the reading",
     EN_CPP, "void EnhancedInputSystem::ResetInputAxes()",
     "    m_ScrollPrevX  = 0.0f;\n    m_ScrollPrevY  = 0.0f;",
     "    ;"),

    # ⚠️ 头条：Gravity=0 会让 GetAxis 与 GetAxisRaw 变成同一个函数 —— 这是
    # "简化这段代码"时最可能的改法。
    ("IP21 AdvanceAxis has no Gravity (GetAxis becomes GetAxisRaw)",
     IN_H, None,
     "    static constexpr float kAxisGravity     = 3.0f;",
     "    static constexpr float kAxisGravity     = 0.0f;"),

    ("IP22 AdvanceAxis has no Sensitivity (the axis never ramps up)",
     IN_H, None,
     "    static constexpr float kAxisSensitivity = 3.0f;",
     "    static constexpr float kAxisSensitivity = 0.0f;"),

    ("IP23 AdvanceAxis drops the dead zone (a resting axis keeps jittering)",
     IN_H, "static float AdvanceAxis(float current, float raw, float dt)",
     "        if (std::fabs(next) < kAxisDead)\n        {\n            next = 0.0f;\n        }",
     "        ;"),

    ("IP24 AdvanceAxis does not clamp to [-1,1]",
     IN_H, "static float AdvanceAxis(float current, float raw, float dt)",
     "        return ClampAxis(next);",
     "        return next;"),

    ("IP25 AdvanceAxis decays past zero (overshoots into the other sign)",
     IN_H, "static float AdvanceAxis(float current, float raw, float dt)",
     "            if (next > 0.0f)\n            {\n"
     "                next = std::max(0.0f, next - kAxisGravity * dt);\n            }",
     "            if (next > 0.0f)\n            {\n"
     "                next = next - kAxisGravity * dt;\n            }"),

    # =========================================================================
    # 门面：默认值与能力位
    # =========================================================================
    ("IP26 IsAnyKeyDown reads the press-edge set instead of the held set",
     EN_H, None,
     "    bool IsAnyKeyDown() const { return !m_PressedKeys.empty(); }",
     "    bool IsAnyKeyDown() const { return !m_PressedPrevFrame.empty(); }"),

    ("IP27 GetTouch's out-of-range sentinel is 0 instead of -1",
     IN_H, "static Touch GetTouch(int index)",
     "        touch.fingerId = -1;",
     "        touch.fingerId = 0;"),

    ("IP28 mousePresent reports false on desktop",
     IN_H, "static bool GetMousePresent()",
     "        return true;",
     "        return false;"),

    ("IP29 touchSupported reports true on desktop (a path that can never fire)",
     IN_H, "static bool GetTouchSupported()",
     "        return false;",
     "        return true;"),

    ("IP30 multiTouchEnabled defaults to false (Unity defaults it to true)",
     IN_H, None,
     "        bool               multi_touch_enabled {true};",
     "        bool               multi_touch_enabled {false};"),

    ("IP31 SetMultiTouchEnabled is a no-op",
     IN_H, None,
     "    static void SetMultiTouchEnabled(bool enabled) { Settings().multi_touch_enabled = enabled; }",
     "    static void SetMultiTouchEnabled(bool enabled) { (void)enabled; }"),

    ("IP32 GetJoystickNames invents one joystick",
     IN_H, "static std::vector<std::string> GetJoystickNames()",
     "        return {};",
     '        return {std::string("Player 1")};'),

    ("IP33 deviceOrientation invents Portrait on desktop",
     IN_H, "static DeviceOrientation GetDeviceOrientation()",
     "        return DeviceOrientation::Unknown;",
     "        return DeviceOrientation::Portrait;"),

    # =========================================================================
    # 接线门：文本输入那条回调
    # =========================================================================
    ("IP34 Initialize() never subscribes to the text-input callback",
     EN_CPP, None,
     "    GET_SYSTEM(WindowSystem)\n        ->RegisterOnCharFunc([this](unsigned int codepoint) {\n"
     "            OnChar(codepoint);\n        });",
     "    ;"),
]

LOCK = os.path.join(BAK, "zengine_mutate_ip.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_ip.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
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
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".ipbak")
            shutil.copy2(path, bak)
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
                with io.open(os.path.join(ROOT, ".mutate_ip.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        print("---- summary ----")
        for n, r in results:
            print("%-70s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
