// =====================================================================
// ScriptInputSmokeTest
// ---------------------------------------------------------------------
// 验证脚本输入 API 背后的查询逻辑。跑法：**直接构造一个 EnhancedInputSystem
// 实例**，手动投递 OnKey / OnMouseButton / OnScroll / OnTouch，再 Tick，然后
// 断言查询结果。
//
// 为什么不 Initialize()：那会向 WindowSystem 注册 GLFW 回调，headless 跑不了。
// 不 Initialize 时 mapping context 是空的，所以这里测的是**新加的按键状态
// 查询**（GetKey/GetKeyDown/GetKeyUp 那一层），不是 action 映射那一层 ——
// 后者需要 WindowSystem。
//
// 重点在最容易写错、且错了最难查的两件事：
//   * **边沿只活一帧**（K2/K3）：一次 PRESS 必须只在**一帧**里被 GetKeyDown
//     读到。清早了（Tick 开头）帧内投的事件会丢；清晚了（不清）按住不放会
//     每帧都跳。
//   * **REPEAT 不算按下**（K4）：长按的自动重复算进去的话，GetKeyDown 会退化
//     成 GetKey —— "按一次跳一下"变成"每帧都跳"。
// 另外钉住：脚本 Input.* 在没有输入系统时返回安全默认值而不是崩（D1），
// 以及鼠标键与键盘共用同一套状态（M1）。
//
// Own main(); excluded from ZRuntime in Runtime/CMakeLists.txt.
// Pure CPU -- no window, no GPU, no script runtime.
// =====================================================================

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Input/EnhancedInputSystem.h"
#include "Runtime/Function/Input/KeyCodes.h"
#include "Runtime/Scripting/ScriptInput/ScriptInput.h"

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

    // KeyCodes 里的动作码（GLFW: Press=1 / Release=0 / Repeat=2）。
    constexpr int kPress   = 1;
    constexpr int kRelease = 0;
    constexpr int kRepeat  = 2;

    // WindowSystem::TouchAction。测试不 include WindowSystem（那会拖进整个
    // 渲染侧），值在这里对齐：Down=0 / Move=1 / Up=2 / Cancel=3。
    constexpr int kTouchDown = 0;
    constexpr int kTouchMove = 1;
    constexpr int kTouchUp   = 2;

    const int kW     = static_cast<int>(KeyCodes::Key::W);
    const int kSpace = static_cast<int>(KeyCodes::Key::Space);
} // namespace

int main()
{
    // ---- K1: 按下之后是持续态 ----
    {
        EnhancedInputSystem sys;
        sys.OnKey(kW, 0, kPress, 0);
        sys.Tick();
        Check(sys.IsKeyDown(kW), "K1 held: IsKeyDown(W) is true");
        Check(!sys.IsKeyDown(kSpace), "K1b a key that was never pressed stays false");
    }

    // ---- K2: 按下边沿只活一帧 ----
    //
    // 一帧 = Tick() 然后 EndFrame()。EndFrame 是帧边界：它把收集器滚动成读数。
    {
        EnhancedInputSystem sys;
        sys.OnKey(kW, 0, kPress, 0);
        sys.Tick();
        Check(!sys.WasKeyPressedThisFrame(kW),
              "K2 the edge is NOT readable before EndFrame (it is still in the collector)");

        sys.EndFrame();
        Check(sys.WasKeyPressedThisFrame(kW), "K2b press edge is readable after EndFrame");

        sys.Tick();
        sys.EndFrame(); // 第二帧
        Check(!sys.WasKeyPressedThisFrame(kW), "K2c press edge is gone on the next frame");
        Check(sys.IsKeyDown(kW), "K2d the key is still held (edge rolled over, state kept)");
    }

    // ---- K3: 抬起边沿同样只活一帧 ----
    {
        EnhancedInputSystem sys;
        sys.OnKey(kW, 0, kPress, 0);
        sys.Tick();
        sys.EndFrame();

        sys.OnKey(kW, 0, kRelease, 0);
        sys.Tick();
        sys.EndFrame();
        Check(sys.WasKeyReleasedThisFrame(kW), "K3 release edge is readable after EndFrame");
        Check(!sys.IsKeyDown(kW), "K3b released key is no longer held");

        sys.Tick();
        sys.EndFrame();
        Check(!sys.WasKeyReleasedThisFrame(kW), "K3c release edge is gone on the next frame");
    }

    // ---- K4: REPEAT 不算"本帧按下" ----
    {
        EnhancedInputSystem sys;
        sys.OnKey(kW, 0, kPress, 0);
        sys.Tick();
        sys.EndFrame();
        Check(sys.WasKeyPressedThisFrame(kW), "K4 first press counts as an edge");

        sys.OnKey(kW, 0, kRepeat, 0);
        sys.Tick();
        sys.EndFrame();
        Check(sys.IsKeyDown(kW), "K4b REPEAT keeps the key held");
        Check(!sys.WasKeyPressedThisFrame(kW),
              "K4c REPEAT must NOT count as a new press (or GetKeyDown degrades to GetKey)");
    }

    // ---- M1: 鼠标键与键盘共用同一套状态，互不干扰 ----
    {
        EnhancedInputSystem sys;
        sys.OnMouseButton(0, kPress, 0);   // 左键
        sys.Tick();
        Check(sys.IsMouseButtonDown(0), "M1 left button held");
        Check(!sys.IsMouseButtonDown(1), "M1c right button untouched");
        // 鼠标左键编码成 MouseButton1+0 = 0，不能和键盘码撞（最小键码是 32）
        Check(!sys.IsKeyDown(kSpace), "M1d mouse button state does not leak into key queries");

        sys.EndFrame();
        Check(sys.WasMouseButtonPressedThisFrame(0), "M1b left button press edge");

        sys.OnMouseButton(0, kRelease, 0);
        sys.Tick();
        sys.EndFrame();
        Check(!sys.IsMouseButtonDown(0), "M1e released button is no longer down");
        Check(sys.WasMouseButtonReleasedThisFrame(0), "M1f release edge");
    }

    // ---- S1: 滚轮在 EndFrame 才滚动成读数 ----
    {
        EnhancedInputSystem sys;
        Check(Nearly(sys.GetScrollDeltaX(), 0.0f) && Nearly(sys.GetScrollDeltaY(), 0.0f),
              "S1 no scrolling: scroll delta is zero");

        sys.OnScroll(0.0, 2.5);
        sys.OnScroll(0.0, 1.5);
        Check(Nearly(sys.GetScrollDeltaY(), 0.0f), "S1b the reading is still zero before EndFrame");

        sys.Tick();
        sys.EndFrame();
        Check(Nearly(sys.GetScrollDeltaY(), 4.0f), "S1c two scroll events land as one reading");

        sys.Tick();
        sys.EndFrame();
        Check(Nearly(sys.GetScrollDeltaY(), 0.0f), "S1d and it is gone on the next frame");
    }

    // ---- T1: 触摸：计数 / 稳定索引 / 位置与位移 ----
    {
        EnhancedInputSystem sys;
        Check(sys.GetTouchCount() == 0, "T1 no fingers on screen");

        sys.OnTouch(7, kTouchDown, 100.0f, 200.0f);
        sys.OnTouch(3, kTouchDown, 400.0f, 500.0f);
        Check(sys.GetTouchCount() == 2, "T1b two fingers are tracked");

        // 按 pointer_id 升序 —— 插入顺序（7 先、3 后）不能影响索引
        Check(sys.GetTouchIdAt(0) == 3, "T1c index 0 is the LOWEST pointer id, not the first inserted");
        Check(sys.GetTouchIdAt(1) == 7, "T1d index 1 is the next id");
        Check(sys.GetTouchIdAt(2) == -1, "T1e out-of-range index returns -1");
        Check(sys.GetTouchIdAt(-1) == -1, "T1f negative index returns -1");

        float x = 0.0f;
        float y = 0.0f;
        Check(sys.GetTouchPosition(7, x, y) && Nearly(x, 100.0f) && Nearly(y, 200.0f),
              "T1g position of a tracked finger");
        Check(!sys.GetTouchPosition(99, x, y), "T1h position of an unknown finger returns false");

        sys.OnTouch(7, kTouchMove, 130.0f, 240.0f);
        float dx = 0.0f;
        float dy = 0.0f;
        Check(sys.GetTouchDelta(7, dx, dy) && Nearly(dx, 30.0f) && Nearly(dy, 40.0f),
              "T1i delta is relative to the touch-down point (virtual stick axis)");

        sys.Tick();
        Check(sys.GetTouchFrameDelta(7, dx, dy) && Nearly(dx, 30.0f) && Nearly(dy, 40.0f),
              "T1j frame delta is readable until EndFrame (scripts need it)");

        sys.EndFrame();
        Check(sys.GetTouchFrameDelta(7, dx, dy) && Nearly(dx, 0.0f) && Nearly(dy, 0.0f),
              "T1k frame delta is cleared at the frame boundary");

        sys.OnTouch(7, kTouchUp, 130.0f, 240.0f);
        Check(sys.GetTouchCount() == 1, "T1k lifting a finger drops it from the set");
    }

    // ---- D1: 脚本 API 在没有输入系统时降级，不崩 ----
    // 这个进程里从来没注册过 InputSystem，所以 Input.* 必然走 nullptr 分支。
    {
        Check(!Input::GetKey(kW), "D1 no input system: GetKey is false");
        Check(!Input::GetKeyDown(kW), "D1b no input system: GetKeyDown is false");
        Check(!Input::GetKeyUp(kW), "D1c no input system: GetKeyUp is false");
        Check(!Input::GetMouseButton(0), "D1d no input system: GetMouseButton is false");
        Check(Nearly(Input::GetMousePosition().x, 0.0f), "D1e no input system: mouse position is zero");
        Check(Nearly(Input::GetMouseScrollDelta().y, 0.0f), "D1f no input system: scroll delta is zero");
        Check(!Input::IsActionActive("Move"), "D1g no input system: IsActionActive is false");
        Check(Nearly(Input::GetActionAxis1D("Move"), 0.0f), "D1h no input system: axis1D is 0");
        Check(Nearly(Input::GetActionAxis2D("Move").y, 0.0f), "D1i no input system: axis2D is zero");
        Check(Input::GetTouchCount() == 0, "D1j no input system: touch count is 0");
        Check(Input::GetTouchIdAt(0) == -1, "D1k no input system: touch id is -1");
    }

    std::fprintf(stdout, "\nScriptInput: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
