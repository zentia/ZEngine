#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Math.h"

enum class GameCommand : unsigned int
{
    forward = 1 << 0,                  // W
    backward = 1 << 1,                 // S
    left = 1 << 2,                     // A
    right = 1 << 3,                    // D
    jump = 1 << 4,                     // SPACE
    squat = 1 << 5,                    // not implemented yet
    sprint = 1 << 6,                   // LEFT SHIFT
    fire = 1 << 7,                     // not implemented yet
    free_carema = 1 << 8,              // F
    invalid = (unsigned int)(1 << 31)  // lost focus
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
    void clear();

    int m_CursorDeltaX {0};
    int m_CursorDeltaY {0};

    Radian m_CursorDeltaYaw {0};
    Radian m_CursorDeltaPitch {0};

    void resetGameCommand() { m_GameCommand = 0; }
    unsigned int getGameCommand() const { return m_GameCommand; }

private:
    void OnKeyInGameMode(int key, int scancode, int action, int mods);

    void CalculateCursorDeltaAngles();

    unsigned int m_GameCommand {0};

    int m_LastCursorX {0};
    int m_LastCursorY {0};
};