// =====================================================================
// ScriptTimeSmokeTest
// ---------------------------------------------------------------------
// 验证脚本时间 API（Time.*）背后的引擎时间状态。Time 的每个方法都是静态
// 的，所以这里直接调用，完全不经过脚本运行时 —— 纯 CPU、headless。
//
// 为什么这个测试存在：脚本的 `OnUpdate(dt)` 本来就收到本帧 delta，所以
// Time 不是"让脚本能算东西"的那类 API。它补的是 dt 参数给不了的量
// （累计时间、帧数、帧率、timeScale），而其中 **timeScale 是真会改变世界
// 推进速度的状态** —— 写错就是"物理和脚本各按一套速度走"这种最难查的
// 不一致。所以这里的重点不是"Time 转发对了没有"，而是：
//
//   * timeScale 缩放后的 delta 就是引擎驱动世界用的那个数（T4 / T5 / T10）；
//   * 不推进逻辑的帧（编辑器暂停）delta 必须是 0，但帧数照涨（T7）；
//   * 负的 delta / 负的 timeScale 都被夹到 0 —— 时间不能倒流（T6 / T8）；
//   * Reset 与 ResetTimeScale 的区别（T11 / T12），退出 Play 靠它把
//     timeScale 还回去。
//
// 刻意不测：fps 的精确值。它是 EMA（alpha = 1/100），只断言恒定帧率下收敛
// 到 60 附近（±1），避免把浮点截断（int）的边界钉死。
//
// Own main(); excluded from ZRuntime in Runtime/CMakeLists.txt.
// Pure CPU -- no window, no GPU, no script runtime.
// =====================================================================

#include "Runtime/Core/Time/TimeState.h"
#include "Runtime/Scripting/ScriptTime/ScriptTime.h"

#include <cmath>
#include <cstdio>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            std::fprintf(stderr, "  [FAIL] %s\n", name);
            ++g_Failures;
        }
        else
        {
            std::fprintf(stdout, "  [ ok ] %s\n", name);
        }
    }

    bool Nearly(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) < tolerance; }

    /// 推进 n 帧。sim / frame 分开传，才能覆盖"编辑器暂停"这种两者不等的
    /// 情形。
    void RunFrames(int count, float simulation_delta, float frame_delta, bool should_update_logic)
    {
        for (int i = 0; i < count; ++i)
        {
            TimeState::Get().BeginFrame(simulation_delta, frame_delta, should_update_logic);
        }
    }
} // namespace

int main()
{
    TimeState& ts = TimeState::Get();

    // ---- T1: 初始状态 ----
    ts.Reset();
    Check(Nearly(ts.GetDeltaTime(), 0.0f), "T1 fresh state: deltaTime == 0");
    Check(Nearly(ts.GetTime(), 0.0f), "T1b fresh state: time == 0");
    Check(Nearly(ts.GetUnscaledTime(), 0.0f), "T1c fresh state: unscaledTime == 0");
    Check(ts.GetFrameCount() == 0, "T1d fresh state: frameCount == 0");
    Check(Nearly(ts.GetTimeScale(), 1.0f), "T1e fresh state: timeScale == 1");
    Check(ts.GetFps() == 0, "T1f fresh state: fps == 0 (no frame yet)");

    // ---- T2: 单帧 ----
    ts.Reset();
    ts.BeginFrame(0.016f, 0.016f, true);
    Check(Nearly(ts.GetDeltaTime(), 0.016f), "T2 one frame: deltaTime == the frame delta");
    Check(Nearly(ts.GetTime(), 0.016f), "T2b one frame: time accumulated");
    Check(Nearly(ts.GetUnscaledTime(), 0.016f), "T2c one frame: unscaledTime accumulated");
    Check(ts.GetFrameCount() == 1, "T2d one frame: frameCount == 1");
    Check(ts.IsLogicUpdating(), "T2e one frame: IsLogicUpdating == true");

    // ---- T3: 多帧累加 ----
    ts.Reset();
    RunFrames(10, 0.01f, 0.01f, true);
    Check(Nearly(ts.GetTime(), 0.1f), "T3 ten frames of 10ms: time == 100ms");
    Check(Nearly(ts.GetUnscaledTime(), 0.1f), "T3b ten frames: unscaledTime == 100ms");
    Check(ts.GetFrameCount() == 10, "T3c ten frames: frameCount == 10");

    // ---- T4: timeScale 0.5 ----
    ts.Reset();
    ts.SetTimeScale(0.5f);
    RunFrames(10, 0.01f, 0.01f, true);
    Check(Nearly(ts.GetDeltaTime(), 0.005f), "T4 timeScale 0.5: deltaTime halved");
    Check(Nearly(ts.GetTime(), 0.05f), "T4b timeScale 0.5: time halves");
    Check(Nearly(ts.GetUnscaledTime(), 0.1f), "T4c timeScale 0.5: unscaledTime unaffected");
    Check(ts.GetFrameCount() == 10, "T4d timeScale 0.5: frameCount unaffected");

    // ---- T5: timeScale 0 冻结逻辑 ----
    ts.Reset();
    ts.SetTimeScale(0.0f);
    RunFrames(10, 0.01f, 0.01f, true);
    Check(Nearly(ts.GetDeltaTime(), 0.0f), "T5 timeScale 0: deltaTime == 0 (logic frozen)");
    Check(Nearly(ts.GetTime(), 0.0f), "T5b timeScale 0: time does not advance");
    Check(Nearly(ts.GetUnscaledTime(), 0.1f), "T5c timeScale 0: real time still advances");
    Check(ts.GetFrameCount() == 10, "T5d timeScale 0: frames still tick");

    // ---- T6: 负 timeScale 夹到 0（时间不能倒流）----
    ts.Reset();
    ts.SetTimeScale(-1.0f);
    Check(Nearly(ts.GetTimeScale(), 0.0f), "T6 negative timeScale is clamped to 0");
    ts.BeginFrame(0.01f, 0.01f, true);
    Check(Nearly(ts.GetDeltaTime(), 0.0f), "T6b negative timeScale: deltaTime stays 0");
    Check(Nearly(ts.GetTime(), 0.0f), "T6c negative timeScale: time never goes backwards");

    // ---- T7: 不推进逻辑的帧（编辑器暂停）----
    ts.Reset();
    RunFrames(5, 0.01f, 0.02f, false);
    Check(Nearly(ts.GetDeltaTime(), 0.0f), "T7 paused frames: deltaTime == 0");
    Check(Nearly(ts.GetTime(), 0.0f), "T7b paused frames: time does not advance");
    Check(Nearly(ts.GetUnscaledTime(), 0.1f), "T7c paused frames: unscaledTime advances (5 x 20ms)");
    Check(Nearly(ts.GetUnscaledDeltaTime(), 0.02f), "T7d paused frames: unscaledDeltaTime is the real frame delta");
    Check(ts.GetFrameCount() == 5, "T7e paused frames: frameCount advances");
    Check(!ts.IsLogicUpdating(), "T7f paused frames: IsLogicUpdating == false");

    // ---- T8: 负 delta 夹到 0 ----
    ts.Reset();
    ts.BeginFrame(-1.0f, -1.0f, true);
    Check(Nearly(ts.GetDeltaTime(), 0.0f), "T8 negative simulation delta is clamped to 0");
    Check(Nearly(ts.GetUnscaledDeltaTime(), 0.0f), "T8b negative frame delta is clamped to 0");
    Check(Nearly(ts.GetUnscaledTime(), 0.0f), "T8c negative frame delta: unscaledTime does not go backwards");
    Check(ts.GetFrameCount() == 1, "T8d negative delta: the frame still counts");

    // ---- T9: fps / smoothDeltaTime（恒定帧率，只断言收敛到 60 附近）----
    ts.Reset();
    RunFrames(300, 1.0f / 60.0f, 1.0f / 60.0f, true);
    Check(ts.GetFps() >= 59 && ts.GetFps() <= 61, "T9 300 frames at 60Hz: fps converges to ~60");
    Check(Nearly(ts.GetSmoothDeltaTime(), 1.0f / 60.0f), "T9b constant frame rate: smoothDeltaTime == the frame delta");

    // ---- T10: timeScale 2 加速 ----
    ts.Reset();
    ts.SetTimeScale(2.0f);
    RunFrames(10, 0.01f, 0.01f, true);
    Check(Nearly(ts.GetDeltaTime(), 0.02f), "T10 timeScale 2: deltaTime doubled");
    Check(Nearly(ts.GetTime(), 0.2f), "T10b timeScale 2: time doubles");
    Check(Nearly(ts.GetUnscaledTime(), 0.1f), "T10c timeScale 2: unscaledTime unaffected");

    // ---- T11: Reset 一并恢复 timeScale ----
    ts.Reset();
    ts.SetTimeScale(0.3f);
    RunFrames(3, 0.01f, 0.01f, true);
    ts.Reset();
    Check(Nearly(ts.GetTimeScale(), 1.0f), "T11 Reset restores timeScale to 1");
    Check(ts.GetFrameCount() == 0, "T11b Reset zeroes frameCount");
    Check(Nearly(ts.GetTime(), 0.0f), "T11c Reset zeroes time");

    // ---- T12: ResetTimeScale 只还 timeScale ----
    ts.Reset();
    RunFrames(4, 0.01f, 0.01f, true);
    ts.SetTimeScale(0.25f);
    ts.ResetTimeScale();
    Check(Nearly(ts.GetTimeScale(), 1.0f), "T12 ResetTimeScale restores timeScale to 1");
    Check(ts.GetFrameCount() == 4, "T12b ResetTimeScale leaves frameCount alone");
    Check(Nearly(ts.GetTime(), 0.04f), "T12c ResetTimeScale leaves accumulated time alone");

    // ---- T13: 脚本 API 与内部状态一致（Time.* 就是转发 TimeState）----
    ts.Reset();
    ts.SetTimeScale(0.5f);
    ts.BeginFrame(0.02f, 0.04f, true);
    Check(Nearly(Time::GetDeltaTime(), ts.GetDeltaTime()), "T13 Time.GetDeltaTime matches TimeState");
    Check(Nearly(Time::GetUnscaledDeltaTime(), ts.GetUnscaledDeltaTime()), "T13b Time.GetUnscaledDeltaTime matches");
    Check(Nearly(Time::GetTime(), ts.GetTime()), "T13c Time.GetTime matches");
    Check(Nearly(Time::GetUnscaledTime(), ts.GetUnscaledTime()), "T13d Time.GetUnscaledTime matches");
    Check(Time::GetFrameCount() == ts.GetFrameCount(), "T13e Time.GetFrameCount matches");
    Check(Time::GetFps() == ts.GetFps(), "T13f Time.GetFps matches");
    Check(Nearly(Time::GetTimeScale(), ts.GetTimeScale()), "T13g Time.GetTimeScale matches");
    Check(Time::IsLogicUpdating() == ts.IsLogicUpdating(), "T13h Time.IsLogicUpdating matches");
    Check(Nearly(Time::GetDeltaTime(), 0.01f), "T13i script-facing deltaTime is the SCALED one (0.02 x 0.5)");

    // ---- T14: 经脚本 API 设 timeScale 真的作用到状态上 ----
    ts.Reset();
    Time::SetTimeScale(0.5f);
    Check(Nearly(ts.GetTimeScale(), 0.5f), "T14 Time.SetTimeScale writes through to TimeState");
    Time::ResetTimeScale();
    Check(Nearly(ts.GetTimeScale(), 1.0f), "T14b Time.ResetTimeScale writes through to TimeState");

    // 收尾：把全局单例留在干净状态（这测试自己就是唯一会动它的东西）
    ts.Reset();

    std::fprintf(stdout, "\nScriptTime: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
