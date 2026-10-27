#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Input/EnhancedInputSystem.h"

// ============================================================
// Legacy InputSystem — backward-compatible wrapper that
// delegates to EnhancedInputSystem.
//
// The old GameCommand bitfield API is preserved so that
// MotorComponent, CameraComponent, and Character continue
// to compile unchanged. These consumers will be migrated
// to the new action-based API in follow-up work.
// ============================================================
enum class GameCommand : unsigned int
{
    forward = 1 << 0,
    backward = 1 << 1,
    left = 1 << 2,
    right = 1 << 3,
    jump = 1 << 4,
    squat = 1 << 5,
    sprint = 1 << 6,
    fire = 1 << 7,
    free_carema = 1 << 8,
    invalid = (unsigned int)(1 << 31)
};
extern unsigned int k_complement_control_command;

class InputSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "InputSystem"; }
    std::vector<std::type_index> GetDependencies() const override;
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Platform; }

    void OnKey(int key, int scancode, int action, int mods);
    void OnCursorPos(double current_cursor_x, double current_cursor_y);

    bool Initialize() override;
    void Shutdown() override {}
    void Tick();

    /// 帧末滚动输入状态（按键边沿 / 光标位移 / 滚轮 / 触摸帧位移）。由
    /// Application::TickOneFrame 每帧调用一次，位置在脚本 tick 之后、
    /// PollEvents 之前。详见 EnhancedInputSystem::EndFrame。
    void EndFrame();

    void clear();

    int m_CursorDeltaX {0};
    int m_CursorDeltaY {0};

    Radian m_CursorDeltaYaw {0};
    Radian m_CursorDeltaPitch {0};

    void resetGameCommand() { m_GameCommand = 0; }
    unsigned int getGameCommand() const { return m_GameCommand; }

    // ---- Unattended test hook ----------------------------------------------
    // MotorComponent drives movement purely from getGameCommand(), and in a
    // headless PIE run there is no real keyboard, so the command is always 0 and
    // the character never moves -- a run like that "passes" without ever
    // exercising CharacterController::move(). Setting this forces a command to
    // be reported to every consumer, which makes the movement pipeline
    // (MotorComponent -> Controller::move -> PhysicsScene::IsOverlap) genuinely
    // run. Zero means "no override, use real input".
    void SetForcedTestCommand(unsigned int command) { m_ForcedTestCommand = command; }
    unsigned int GetForcedTestCommand() const { return m_ForcedTestCommand; }

    // Access the underlying EnhancedInputSystem
    EnhancedInputSystem* GetEnhancedInput() const { return m_EnhancedInput; }

private:
    void CalculateCursorDeltaAngles();

    unsigned int m_GameCommand {0};

    // See SetForcedTestCommand(): non-zero overrides the command reported by
    // getGameCommand() so unattended PIE runs can drive movement for real.
    unsigned int m_ForcedTestCommand {0};

    int m_LastCursorX {0};
    int m_LastCursorY {0};

    // The real input system
    EnhancedInputSystem* m_EnhancedInput = nullptr;

    // Cached action query state (synced from EnhancedInputSystem each tick)
    void SyncGameCommands();
};
