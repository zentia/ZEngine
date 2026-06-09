#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Input/InputAction.h"
#include "Runtime/Function/Input/InputMappingContext.h"
#include "Runtime/Function/Input/InputTriggers.h"

#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// EnhancedInputSystem — replaces the old InputSystem.
//
// Architecture (UE EnhancedInput-inspired):
//
//   Raw input (GLFW) → EnhancedInputSystem::OnKey/OnCursorPos/...
//     → Accumulate raw values per InputAction (via mapping contexts)
//     → Apply modifiers
//     → Evaluate triggers
//     → Fire bound delegates
//
// The system owns a priority stack of InputMappingContexts.
// Higher-priority contexts process input first and can consume
// actions (preventing lower-priority contexts from seeing them).
//
// No 3D dependencies: cursor-to-angle conversion is now the
// consumer's responsibility (CameraComponent applies its own
// sensitivity). The system only provides raw delta values.
// ============================================================
class EnhancedInputSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "EnhancedInputSystem"; }
    std::vector<std::type_index> GetDependencies() const override;
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Platform; }

    bool Initialize() override;
    void Shutdown() override;
    void Tick();

    // ---- Raw input callbacks (registered with WindowSystem) ----
    void OnKey(int key, int scancode, int action, int mods);
    void OnCursorPos(double current_cursor_x, double current_cursor_y);
    void OnMouseButton(int button, int action, int mods);
    void OnScroll(double xoffset, double yoffset);

    // ---- Mapping context stack (UE IEnhancedInputSubsystemInterface) ----

    // Add a mapping context at the given priority. If a context with the
    // same name already exists, this replaces it.
    void AddMappingContext(std::shared_ptr<InputMappingContext> context, int priority);

    // Remove a mapping context by name.
    void RemoveMappingContext(const std::string& name);

    // Remove all mapping contexts.
    void ClearMappingContexts();

    // ---- Action registration ----

    // Register an action so the system knows about it. Not strictly required
    // but useful for iteration / debugging.
    void RegisterAction(InputAction* action);

    // ---- Query helpers (for legacy migration) ----

    // Get the current value of a named action (if any context maps it).
    InputActionValue GetActionValue(const std::string& action_name) const;

    // Quick boolean check: is the action currently actuated?
    bool IsActionActive(const std::string& action_name) const;

    // Raw cursor position (for UI hit-testing, etc.)
    double GetCursorX() const { return m_CursorX; }
    double GetCursorY() const { return m_CursorY; }

    // Raw cursor delta in pixels (cleared each frame after Tick).
    double GetCursorDeltaX() const { return m_CursorDeltaX; }
    double GetCursorDeltaY() const { return m_CursorDeltaY; }

    // Focus mode (cursor locked/hidden for FPS camera control).
    // Kept for backward compatibility; new code should use a
    // mapping context for UI vs gameplay instead.
    bool IsFocusMode() const { return m_FocusMode; }
    void SetFocusMode(bool mode);

private:
    // Rebuild the internal lookup tables when the context stack changes.
    void RebuildMappingTable();

    // Process accumulated raw values through modifiers and triggers.
    void ProcessActions(float delta_time);

    // Apply modifiers from a key mapping to a value.
    InputActionValue ApplyModifiers(const FKeyMapping& mapping, const InputActionValue& value) const;

    // Get or create the default trigger for an action type.
    std::shared_ptr<InputTrigger> GetDefaultTrigger(InputAction* action);

    // ---- Raw input state ----
    std::unordered_set<int> m_PressedKeys;  // currently-held GLFW key codes
    double m_CursorX = 0.0;
    double m_CursorY = 0.0;
    double m_CursorDeltaX = 0.0;
    double m_CursorDeltaY = 0.0;
    double m_LastCursorX = 0.0;
    double m_LastCursorY = 0.0;
    float  m_ScrollAccumX = 0.0f;
    float  m_ScrollAccumY = 0.0f;
    bool   m_FocusMode = false;
    bool   m_CursorInitialized = false;

    // ---- Mapping context stack ----
    std::vector<FMappingContextStackEntry> m_ContextStack;
    bool m_ContextDirty = true; // need RebuildMappingTable

    // ---- Rebuilt lookup tables ----
    // key_code -> list of FKeyMapping pointers (from all active contexts,
    // ordered by context priority).
    std::unordered_map<int, std::vector<FKeyMapping*>> m_KeyToMappings;
    // "mouse_move" and "mouse_scroll" are special pseudo-keys
    static constexpr int kKeyMouseMove = -1;
    static constexpr int kKeyMouseScroll = -2;
    std::vector<FKeyMapping*> m_MouseMoveMappings;
    std::vector<FKeyMapping*> m_MouseScrollMappings;

    // ---- Action state ----
    // Per-action accumulated value this frame (before trigger eval).
    // Action name -> accumulated value.
    std::unordered_map<std::string, InputActionValue> m_AccumulatedValues;

    // Per-action trigger state (for default triggers).
    // Action name -> trigger instance.
    std::unordered_map<std::string, std::shared_ptr<InputTrigger>> m_DefaultTriggers;

    // Registered actions (for query/debug).
    std::vector<InputAction*> m_RegisteredActions;
};
