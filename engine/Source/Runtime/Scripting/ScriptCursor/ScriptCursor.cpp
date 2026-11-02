#include "ScriptCursor.h"

#include "Runtime/Core/Base/SystemRegistry.h"

namespace
{
    // ---------------------------------------------------------------------------
    // 进程级回退状态
    //
    // 有两份状态：窗口上的（真正生效的那份）和这里的。原因是 **headless** ——
    // 没有 WindowSystem 时也得能设能读，否则"进游戏锁光标"这段脚本在 CI 里
    // 直接崩在空指针上。
    //
    // 有窗口时两边同步写，读的是这里（与刚设的值一致）；没有窗口时只动这里。
    // 与 `Gizmos.color` / `TimeState` 同一类进程级状态，放在函数内 static 避开
    // 静态初始化顺序。
    // ---------------------------------------------------------------------------
    struct CursorState
    {
        bool           visible {true};
        CursorLockMode lock_state {CursorLockMode::None};
    };

    CursorState& State()
    {
        static CursorState s_State;
        return s_State;
    }

    GenericWindow* TryGetMainWindow()
    {
        WindowSystem* window_system = GET_SYSTEM(WindowSystem);
        if (window_system == nullptr)
        {
            return nullptr;
        }
        return window_system->GetMainWindow();
    }
}  // namespace

bool Cursor::GetVisible()
{
    return State().visible;
}

void Cursor::SetVisible(bool visible)
{
    State().visible = visible;
    if (GenericWindow* window = TryGetMainWindow())
    {
        window->SetCursorVisible(visible);
    }
}

CursorLockMode Cursor::GetLockState()
{
    return State().lock_state;
}

void Cursor::SetLockState(CursorLockMode mode)
{
    State().lock_state = mode;
    if (GenericWindow* window = TryGetMainWindow())
    {
        // 两个枚举取值刻意一致（见 GenericWindow.h 的注释），转换是无损的。
        // 用 switch 而不是裸 static_cast：将来任一侧加了档位，这里会编译失败
        // 而不是静默错位。
        switch (mode)
        {
            case CursorLockMode::None:
                window->SetCursorLockMode(GenericWindow::CursorLockMode::None);
                break;
            case CursorLockMode::Locked:
                window->SetCursorLockMode(GenericWindow::CursorLockMode::Locked);
                break;
            case CursorLockMode::Confined:
                window->SetCursorLockMode(GenericWindow::CursorLockMode::Confined);
                break;
            default:
                window->SetCursorLockMode(GenericWindow::CursorLockMode::None);
                break;
        }
    }
}

void Cursor::SetCursorState(bool visible, CursorLockMode mode)
{
    SetVisible(visible);
    SetLockState(mode);
}

void Cursor::ResetToDefaults()
{
    SetCursorState(true, CursorLockMode::None);
}
