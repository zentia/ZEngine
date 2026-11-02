"""Mutation harness for the "platform + facade" slice (#50).

Covers the batch that landed together:

    SystemInfo / QualitySettings / Cursor (+ WindowsWindow backend)
    TimeState gates (maximumDeltaTime, captureFramerate, timeSinceLevelLoad)
    Texture2D / RenderTexture pixel+desc facades
    ParticleComponent play-state machine
    Application extras (OpenURL, identifier, isFocused, backgroundLoadingPriority)

Why these particular mutants
----------------------------
None of this code is algorithmically deep -- the failure mode that matters here
is **a plausible value standing in for an unknown one**, and **a plausible
neighbouring policy standing in for the chosen one**:

* `SystemInfo` is deliberately full of sentinels (`"Unknown"` / `0` / `-1`).
  Every one of them has a *tempting* neighbour: `batteryLevel = 0` ("no power"),
  `supportsInstancing = true` ("all modern backends do"), `graphicsMemorySize =
  2048` ("a reasonable number"). All three are indistinguishable from real data
  downstream, which is exactly why they are dangerous.
* `SnapDownTo` vs snap-up: "I asked for 3x MSAA" becoming 4x is a silent
  *upgrade* -- the setting quietly exceeds the budget the user picked.
* `TimeState`: clamp-then-scale vs scale-then-clamp differ only when
  `timeScale != 1`, which is the slow-motion case nobody eyeballs.
* `Texture2D.SetPixel` rounding (`floor(v*255+0.5)`) vs truncation: one bit at
  0.5, invisible on a screenshot, drifts on a round-trip through
  `ToHtmlStringRGB`.
* `Cursor`: `ShowCursor` is a **refcount**, so "call it once" looks right and
  only breaks after a nested hide/show pair.
* `OpenURL`: the allow-list/blacklist are only observable through source gates,
  because a rejected URL and a URL the OS happened to fail on both return
  `false`.

Run:  py .mutate_pf.py            (all)
      py .mutate_pf.py PF01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
S = lambda p: os.path.join(ROOT, p.replace("/", os.sep))

SI_CPP = S(r"engine/Source/Runtime/Scripting/ScriptSystemInfo/ScriptSystemInfo.cpp")
QS_CPP = S(r"engine/Source/Runtime/Scripting/ScriptQuality/ScriptQuality.cpp")
CU_CPP = S(r"engine/Source/Runtime/Scripting/ScriptCursor/ScriptCursor.cpp")
WIN_CPP = S(r"engine/Source/Runtime/Function/Render/Platform/Windows/WindowsWindow.cpp")
TS_CPP = S(r"engine/Source/Runtime/Core/Time/TimeState.cpp")
TX_CPP = S(r"engine/Source/Runtime/Function/Render/Texture/Texture2D.cpp")
RT_CPP = S(r"engine/Source/Runtime/Function/Render/Texture/RenderTexture.cpp")
PC_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Particle/ParticleComponent.cpp")
AP_CPP = S(r"engine/Source/Runtime/Application/Application.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# 文件作用域的函数（默认）。匿名 namespace 内 / `namespace Runtime` 内的函数体
# 是 4 空格收尾的 `}`，要显式给 end_marker，否则 `find("\n}\n")` 会一路跑到
# 下一个顶格函数去，把无关代码也改掉。
END_OF_FUNC = "\n}\n"
END_OF_NS_FUNC = "\n    }\n"

# ⚠️ 一次只编一个 target：同时给 `--target A B` 会让 MSBuild 并行跑，某个 target
# 的 post-build 往 bin/Debug/ZRuntime.lib 拷贝时正好撞上另一个的链接 —— 间歇性
# LNK1236。
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
    marker = end_marker if end_marker is not None else END_OF_FUNC
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


# ⚠️ kForbidden 的字面量里全是反斜杠（"\t" 在**文件里**是两个字符），用 raw
# 字符串拼，别手写成真的制表符。
_KFORBIDDEN_DECL = (r'    static const std::string kForbidden = " \t\r\n\"'
                    + "'"
                    + r'`$\\;|&<>(){}[]*?!#\n";')

MUTATIONS = [
    # =========================================================================
    # SystemInfo：哨兵值 vs "看起来合理"的假值
    # =========================================================================
    # 0 会被当成"没电了"，1 会被当成"满电"。两种都是假信息，而 -1 是 Unity 的
    # "本平台不支持" —— 唯一能被脚本识别为"别信这个值"的那个。
    ("PF01 batteryLevel reports 0 instead of the -1 'unsupported' sentinel",
     SI_CPP, "float SystemInfo::GetBatteryLevel(",
     "    return kUnknownLevel;",
     "    return 0.0f;"),

    ("PF02 batteryStatus reports Full instead of Unknown",
     SI_CPP, "BatteryStatus SystemInfo::GetBatteryStatus(",
     "    return BatteryStatus::Unknown;",
     "    return BatteryStatus::Full;"),

    # "所有现代后端都支持实例化" 是句真话，但 headless 下**没有**后端 —— 那里
    # 说 true 就是承诺一件做不到的事。
    ("PF03 supportsInstancing is unconditionally true (RHI may not exist)",
     SI_CPP, "bool SystemInfo::GetSupportsInstancing(",
     "    return TryGetRHI() != nullptr;",
     "    return true;"),

    ("PF04 graphicsMemorySize invents 2048 MB (RHI exposes no memory query)",
     SI_CPP, "int SystemInfo::GetGraphicsMemorySize(",
     "    return kUnknownSize;",
     "    return 2048;"),

    ("PF05 graphicsShaderLevel answers 50 without checking for an RHI",
     SI_CPP, "int SystemInfo::GetGraphicsShaderLevel(",
     "        return kUnknownSize;",
     "        return 50;"),

    ("PF06 maxTextureSize falls back to 4096 instead of the 0 sentinel",
     SI_CPP, "int SystemInfo::GetMaxTextureSize(",
     "        return kUnknownSize;",
     "        return 4096;"),

    ("PF07 deviceUniqueIdentifier drops the platform prefix",
     SI_CPP, "eastl::string SystemInfo::GetDeviceUniqueIdentifier(",
     '    return Format("%s-%s-%d", platform, QueryHostName().c_str(), GetProcessorCount());',
     '    return Format("%s-%d", QueryHostName().c_str(), GetProcessorCount());'),

    ("PF08 deviceType reports Unknown instead of Desktop",
     SI_CPP, "DeviceType SystemInfo::GetDeviceType(",
     "    return DeviceType::Desktop;",
     "    return DeviceType::Unknown;"),

    ("PF09 operatingSystemFamily reports Other instead of Windows",
     SI_CPP, "OperatingSystemFamily SystemInfo::GetOperatingSystemFamily(",
     "    return OperatingSystemFamily::Windows;",
     "    return OperatingSystemFamily::Other;"),

    # ⚠️ 最像真的那一条：桌面上"机型"拿不到，但主机名**有** —— 于是拿它顶上。
    # 上报 / 分档逻辑会把它当成真机型，而它其实只是这台电脑的名字。
    ("PF10 deviceModel substitutes the host name for the 'Unknown' sentinel",
     SI_CPP, "eastl::string SystemInfo::GetDeviceModel(",
     "    return eastl::string(kUnknown);",
     "    return QueryHostName();"),

    # =========================================================================
    # QualitySettings
    # =========================================================================
    # ⚠️ 头条：向上取。"我要 3x MSAA" 会被变成 4x —— 那是**升级**，不是取最近。
    ("PF11 SnapDownTo snaps UP instead of down (3x MSAA silently becomes 4x)",
     QS_CPP, "int SnapDownTo(int v, const int* legal, std::size_t count)",
     "            if (legal[i] <= v)\n            {\n                result = legal[i];\n            }",
     "            if (legal[i] >= v)\n            {\n                result = legal[i];\n                break;\n            }",
     END_OF_NS_FUNC),

    # Unity 是"越界忽略"，不是"越界夹紧"。夹紧会让"我传了一个非法档位"看起来
    # 像是生效了 —— 调用方拿不到任何出错信号。
    ("PF12 SetQualityLevel clamps out-of-range levels instead of ignoring them",
     QS_CPP, "void QualitySettings::SetQualityLevel(int level)",
     "    if (level < 0 || level >= kLevelCount)\n    {\n        return;\n    }",
     "    if (level < 0) { level = 0; }\n    if (level >= kLevelCount) { level = kLevelCount - 1; }"),

    ("PF13 ApplyPreset does not apply antiAliasing",
     QS_CPP, "void ApplyPreset(int level)",
     "        state.anti_aliasing      = preset.anti_aliasing;",
     "        state.anti_aliasing      = 0;",
     END_OF_NS_FUNC),

    ("PF14 ApplyPreset does not move the level itself",
     QS_CPP, "void ApplyPreset(int level)",
     "        state.level              = level;",
     "        state.level              = 2;",
     END_OF_NS_FUNC),

    ("PF15 masterTextureLimit clamps to 0..2 instead of Unity's 0..3",
     QS_CPP, "void QualitySettings::SetMasterTextureLimit(int limit)",
     "    State().master_texture_limit = ClampInt(limit, 0, 3);",
     "    State().master_texture_limit = ClampInt(limit, 0, 2);"),

    ("PF16 SetVSyncCount does not clamp (a negative count reaches the swapchain)",
     QS_CPP, "void QualitySettings::SetVSyncCount(int count)",
     "    State().vsync_count = ClampInt(count, 0, 4);",
     "    State().vsync_count = count;"),

    ("PF17 SetShadowDistance accepts a negative distance",
     QS_CPP, "void QualitySettings::SetShadowDistance(float distance)",
     "    State().shadow_distance = ClampFloat(distance, 0.0f, 100000.0f);",
     "    State().shadow_distance = ClampFloat(distance, -100000.0f, 100000.0f);"),

    ("PF18 an out-of-range shadowResolution falls back to Low, not Medium",
     QS_CPP, "void QualitySettings::SetShadowResolution(ShadowResolution resolution)",
     "            State().shadow_resolution = ShadowResolution::Medium;",
     "            State().shadow_resolution = ShadowResolution::Low;"),

    ("PF19 an out-of-range skinWeights falls back to OneBone, not FourBones",
     QS_CPP, "void QualitySettings::SetSkinWeights(SkinWeights weights)",
     "            State().skin_weights = SkinWeights::FourBones;",
     "            State().skin_weights = SkinWeights::OneBone;"),

    ("PF20 ResetToDefaults leaves the current state in place",
     QS_CPP, "void QualitySettings::ResetToDefaults()",
     "    State() = QualityState {};",
     "    ;"),

    ("PF21 GetNameForLevel returns the first name for an out-of-range level",
     QS_CPP, "eastl::string QualitySettings::GetNameForLevel(int level)",
     "        return eastl::string();",
     "        return eastl::string(kLevelNames[0]);"),

    # 档位表与名字表靠下标对齐 —— 错位只能靠"最高两档的名字"看出来。
    ("PF22 the top two level names are swapped (Very High / Ultra)",
     QS_CPP, None,
     '        "Very Low", "Low", "Medium", "High", "Very High", "Ultra",',
     '        "Very Low", "Low", "Medium", "High", "Ultra", "Very High",'),

    # =========================================================================
    # Cursor
    # =========================================================================
    ("PF23 SetVisible does not record the state (headless reads a stale value)",
     CU_CPP, "void Cursor::SetVisible(bool visible)",
     "    State().visible = visible;",
     "    ;"),

    ("PF24 SetLockState does not record the state",
     CU_CPP, "void Cursor::SetLockState(CursorLockMode mode)",
     "    State().lock_state = mode;",
     "    ;"),

    # 只设一半：`SetCursor(false, Locked)` 之后光标藏了但没锁，鼠标能移出窗口。
    ("PF25 SetCursorState applies only the visibility half",
     CU_CPP, "void Cursor::SetCursorState(bool visible, CursorLockMode mode)",
     "    SetVisible(visible);\n    SetLockState(mode);",
     "    SetVisible(visible);"),

    ("PF26 ResetToDefaults hides and locks instead of showing and unlocking",
     CU_CPP, "void Cursor::ResetToDefaults()",
     "    SetCursorState(true, CursorLockMode::None);",
     "    SetCursorState(false, CursorLockMode::Locked);"),

    # ⚠️ 头条：ShowCursor 是**引用计数**。调一次 FALSE 只把计数减一；嵌套调用后
    # 计数 -2，之后一次 TRUE 仍不显示 —— 经典的"光标不见了"。
    ("PF27 Windows SetCursorVisible calls ShowCursor once instead of looping past zero",
     WIN_CPP, "void WindowsWindow::SetCursorVisible(bool visible)",
     "        while (::ShowCursor(TRUE) < 0)\n        {\n        }",
     "        ::ShowCursor(TRUE);"),

    # GetClientRect 给的是客户区坐标，ClipCursor 要屏幕坐标。差一个标题栏高度。
    ("PF28 Windows clips the cursor with client (not screen) coordinates",
     WIN_CPP, "void WindowsWindow::SetCursorLockMode(GenericWindow::CursorLockMode mode)",
     "    ::ClientToScreen(m_Hwnd, &top_left);\n    ::ClientToScreen(m_Hwnd, &bottom_right);",
     "    ;"),

    # =========================================================================
    # TimeState 闸门
    # =========================================================================
    # ⚠️ 头条：先缩放再夹。timeScale=0.1 时上限变成 0.0333 秒，慢放被切成碎片
    # —— 而且只在 timeScale != 1 时才看得出来，正常速度下两种实现完全一致。
    ("PF29 BeginFrame scales before clamping instead of clamping before scaling",
     TS_CPP, "void TimeState::BeginFrame(float simulation_delta_time, float frame_delta_time, bool should_update_logic)",
     "    sim_delta = std::min(sim_delta, m_MaximumDeltaTime);\n\n"
     "    m_UnscaledDeltaTime = frame_delta;\n\n"
     "    m_LogicUpdating = should_update_logic;\n\n"
     "    if (should_update_logic)\n    {\n        const float scaled = sim_delta * m_TimeScale;",
     "    sim_delta = std::min(sim_delta * m_TimeScale, m_MaximumDeltaTime);\n\n"
     "    m_UnscaledDeltaTime = frame_delta;\n\n"
     "    m_LogicUpdating = should_update_logic;\n\n"
     "    if (should_update_logic)\n    {\n        const float scaled = sim_delta;"),

    # 传 0 是明确的"关掉"。夹到 1 会变成"每帧 1 秒"的慢动作，看起来像引擎挂了。
    ("PF30 SetCaptureFramerate clamps 0 up to 1 fps instead of switching off",
     TS_CPP, "void TimeState::SetCaptureFramerate(int capture_framerate)",
     "    m_CaptureFramerate = capture_framerate > 0 ? capture_framerate : 0;",
     "    m_CaptureFramerate = capture_framerate > 0 ? capture_framerate : 1;"),

    ("PF31 NotifyLevelLoaded resets the level clock to zero instead of to now",
     TS_CPP, "void TimeState::NotifyLevelLoaded()",
     "    m_LevelLoadTime = m_Time;",
     "    m_LevelLoadTime = 0.0f;"),

    # 那就是 `Time.time`，不是 `timeSinceLevelLoad` —— 关卡计时器永远不清零。
    ("PF32 GetTimeSinceLevelLoad ignores the level-load timestamp",
     TS_CPP, "float TimeState::GetTimeSinceLevelLoad() const",
     "    const float elapsed = m_Time - m_LevelLoadTime;",
     "    const float elapsed = m_Time;"),

    # 上限 0 会把每一帧的 sim delta 夹成 0 —— 世界静止，而且不报任何错。
    ("PF33 SetMaximumDeltaTime accepts 0 (every frame is clamped to nothing)",
     TS_CPP, "void TimeState::SetMaximumDeltaTime(float maximum_delta_time)",
     "    m_MaximumDeltaTime = std::max(maximum_delta_time, kMinMaximumDeltaTime);",
     "    m_MaximumDeltaTime = maximum_delta_time;"),

    # =========================================================================
    # Texture2D
    # =========================================================================
    # 只按尺寸判：数据只有一半时也报"有像素"，GetPixel 于是读到 vector 外面。
    ("PF34 GetHasUncompressedPixels only checks the dimensions, not the byte count",
     TX_CPP, "bool Texture2D::GetHasUncompressedPixels() const",
     "    return m_Pixels.size() >= expected;",
     "    return m_Width > 0 && m_Height > 0;"),

    # ⚠️ 夹紧到边缘像素：越界读返回一个"像邻居"的值，于是越界的 bug 变成
    # "纹理边缘被拉了一条"，而不是"这里读空了"。
    ("PF35 GetPixel clamps out-of-range coordinates to the edge pixel",
     TX_CPP, "Color Texture2D::GetPixel(int x, int y) const",
     "    const auto ux = static_cast<uint32_t>(x);\n"
     "    const auto uy = static_cast<uint32_t>(y);\n"
     "    if (ux >= m_Width || uy >= m_Height)\n    {\n        return ::Color32(0, 0, 0, 0);\n    }",
     "    auto ux = static_cast<uint32_t>(x);\n"
     "    auto uy = static_cast<uint32_t>(y);\n"
     "    if (ux >= m_Width) { ux = m_Width - 1; }\n"
     "    if (uy >= m_Height) { uy = m_Height - 1; }"),

    # 0.5 -> 127 而不是 128。一位，看不见，但和 ToHtmlStringRGB 来回转换会漂。
    ("PF36 SetPixel truncates channels instead of rounding them",
     TX_CPP, "bool Texture2D::SetPixel(int x, int y, const Color& color)",
     "        const float scaled = v * 255.0f + 0.5f;",
     "        const float scaled = v * 255.0f;"),

    # 写 2.0 得到 254/255 而不是 1.0 —— uint8_t 回绕。
    ("PF37 SetPixel wraps out-of-range channels instead of clamping them",
     TX_CPP, "bool Texture2D::SetPixel(int x, int y, const Color& color)",
     "        if (scaled >= 255.0f)\n        {\n            return 255;\n        }\n"
     "        return static_cast<uint8_t>(scaled);",
     "        if (scaled >= 255.0f)\n        {\n"
     "            return static_cast<uint8_t>(static_cast<int>(scaled) & 0xFF);\n        }\n"
     "        return static_cast<uint8_t>(scaled);"),

    ("PF38 Resize accepts a zero / negative dimension",
     TX_CPP, "bool Texture2D::Resize(int width, int height)",
     "    if (width <= 0 || height <= 0)",
     "    if (false)"),

    ("PF39 ClearPixels zeroes alpha instead of writing the requested colour's alpha",
     TX_CPP, "bool Texture2D::ClearPixels(const Color& color)",
     "                                 to_byte(color.a)};",
     "                                 0};"),

    # =========================================================================
    # RenderTexture
    # =========================================================================
    # 自相矛盾的状态：没有 mip 链却要自动生成 mip。
    ("PF40 turning useMipMap off leaves autoGenerateMips on",
     RT_CPP, "void RenderTexture::SetUseMipMap(bool use_mip_map)",
     "    if (!use_mip_map)",
     "    if (false)"),

    ("PF41 turning autoGenerateMips on does not force useMipMap on",
     RT_CPP, "void RenderTexture::SetAutoGenerateMips(bool auto_generate)",
     "    if (auto_generate)",
     "    if (false)"),

    # 释放掉的 RT 继续当活动目标：之后的渲染往一张没有资源的 RT 上画。
    ("PF42 Release leaves the released RT as the active target",
     RT_CPP, "void RenderTexture::Release()",
     "    if (s_Active == this)",
     "    if (false)"),

    ("PF43 Create reports success even for a zero-sized descriptor",
     RT_CPP, "void RenderTexture::Create()",
     "    if (m_Desc.width <= 0 || m_Desc.height <= 0)",
     "    if (false)"),

    ("PF44 RenderTexture's SnapDownTo snaps UP (3x MSAA becomes 4x, depth 20 becomes 24)",
     RT_CPP, "int SnapDownTo(int v, const int* legal, int count)",
     "            if (legal[i] <= v)\n            {\n                result = legal[i];\n            }",
     "            if (legal[i] >= v)\n            {\n                result = legal[i];\n                break;\n            }",
     END_OF_NS_FUNC),

    # =========================================================================
    # ParticleComponent 播放状态机
    # =========================================================================
    ("PF45 Tick advances the clock while paused",
     PC_CPP, "void ParticleComponent::Tick(float delta_time)",
     "        if (m_PlayState == ParticlePlayState::Playing)\n        {\n"
     "            m_Time += delta_time > 0.0f ? delta_time : 0.0f;\n        }",
     "        if (m_PlayState != ParticlePlayState::Stopped)\n        {\n"
     "            m_Time += delta_time > 0.0f ? delta_time : 0.0f;\n        }",
     END_OF_NS_FUNC),

    # ⚠️ 不是"去掉 Stopped 早退"（那在 headless 下是等价变异：时钟累加本来就
    # 只在 Playing 分支里，去掉早退只影响下面那几行，而那里没有 RenderSystem
    # 根本走不到）。真正脆的是**顺序**：判空一旦挪到时钟之前，裸组件（还没
    # PostLoadResource / 单测里直接构造出来的）就永远推进不了时钟 —— 而这也
    # 是 headless 下唯一能验证时钟的那条路（PP19）。
    ("PF46 Tick's null-parent guard is hoisted above the clock",
     PC_CPP, "void ParticleComponent::Tick(float delta_time)",
     "        if (m_PlayState == ParticlePlayState::Stopped)\n        {\n            return;\n        }",
     "        if (m_ParentObject == nullptr)\n        {\n            return;\n        }\n"
     "        if (m_PlayState == ParticlePlayState::Stopped)\n        {\n            return;\n        }",
     END_OF_NS_FUNC),

    ("PF47 Pause turns a stopped system into a paused one",
     PC_CPP, "void ParticleComponent::Pause()",
     "        if (m_PlayState == ParticlePlayState::Playing)",
     "        if (m_PlayState != ParticlePlayState::Paused)",
     END_OF_NS_FUNC),

    ("PF48 Clear stops the emitter but leaves the clock where it was",
     PC_CPP, "void ParticleComponent::Clear()",
     "        m_Time      = 0.0f;",
     "        ;",
     END_OF_NS_FUNC),

    # 越界档位不回落：后面每个 switch 都走 default，状态机读出来是垃圾。
    ("PF49 SetPlayState stores an out-of-range state verbatim",
     PC_CPP, "void ParticleComponent::SetPlayState(ParticlePlayState state)",
     "                m_PlayState = ParticlePlayState::Stopped;",
     "                m_PlayState = state;",
     END_OF_NS_FUNC),

    # =========================================================================
    # Application
    # =========================================================================
    # ⚠️ 允许一切 scheme：正则式"放宽白名单"最常见的形态。只有源码门看得见 ——
    # 行为上"被拒"与"OS 恰好打不开"都返回 false。
    ("PF50 OpenURL's scheme allow-list is widened to everything",
     AP_CPP, None,
     '        "http://",\n        "https://",\n        "mailto:",\n        "file://",',
     '        "",'),

    ("PF51 OpenURL's shell metacharacter blacklist is emptied",
     AP_CPP, None,
     _KFORBIDDEN_DECL,
     r'    static const std::string kForbidden = "\t";'),

    # headless 下 false 会把整个世界冻住（很多系统用 isFocused 做暂停）。
    ("PF52 GetIsFocused reports false when there is no window system",
     AP_CPP, "bool Application::GetIsFocused()",
     "        return true;",
     "        return false;"),

    ("PF53 GetIdentifier drops the reversed-domain 'com.' prefix",
     AP_CPP, "std::string Application::GetIdentifier()",
     '    return "com." + tail;',
     "    return tail;"),

    # 不回落：一次算错的枚举值把加载线程优先级永久顶到最高，且无日志。
    ("PF54 an out-of-range backgroundLoadingPriority is kept instead of falling back",
     AP_CPP, "void Application::SetBackgroundLoadingPriority(ThreadPriority priority)",
     "            s_BackgroundLoadingPriority = ThreadPriority::Normal;",
     "            ;"),
]

LOCK = os.path.join(BAK, "zengine_mutate_pf.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_pf.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".pfbak")
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
                with io.open(os.path.join(ROOT, ".mutate_pf.log"), "a", encoding="utf-8") as lg:
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
