#pragma once

#include "Runtime/Function/Input/InputAction.h"
#include "Runtime/Function/Input/InputTriggers.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Platform key codes. We use GLFW key codes on desktop platforms.
// On other platforms these would be mapped from the platform's native
// key codes. The constants below mirror GLFW_KEY_* values so that
// FKey can be used without including GLFW headers in every consumer.
namespace KeyCodes
{
    constexpr int Unknown      = -1;
    constexpr int Space        = 32;
    constexpr int Apostrophe   = 39;
    constexpr int Comma        = 44;
    constexpr int Minus        = 45;
    constexpr int Period       = 46;
    constexpr int Slash        = 47;
    constexpr int _0           = 48;
    constexpr int _1           = 49;
    constexpr int _2           = 50;
    constexpr int _3           = 51;
    constexpr int _4           = 52;
    constexpr int _5           = 53;
    constexpr int _6           = 54;
    constexpr int _7           = 55;
    constexpr int _8           = 56;
    constexpr int _9           = 57;
    constexpr int Semicolon    = 59;
    constexpr int Equal        = 61;
    constexpr int A            = 65;
    constexpr int B            = 66;
    constexpr int C            = 67;
    constexpr int D            = 68;
    constexpr int E            = 69;
    constexpr int F            = 70;
    constexpr int G            = 71;
    constexpr int H            = 72;
    constexpr int I            = 73;
    constexpr int J            = 74;
    constexpr int K            = 75;
    constexpr int L            = 76;
    constexpr int M            = 77;
    constexpr int N            = 78;
    constexpr int O            = 79;
    constexpr int P            = 80;
    constexpr int Q            = 81;
    constexpr int R            = 82;
    constexpr int S            = 83;
    constexpr int T            = 84;
    constexpr int U            = 85;
    constexpr int V            = 86;
    constexpr int W            = 87;
    constexpr int X            = 88;
    constexpr int Y            = 89;
    constexpr int Z            = 90;
    constexpr int LeftBracket  = 91;
    constexpr int Backslash    = 92;
    constexpr int RightBracket = 93;
    constexpr int GraveAccent  = 96;
    constexpr int Escape       = 256;
    constexpr int Enter        = 257;
    constexpr int Tab          = 258;
    constexpr int Backspace    = 259;
    constexpr int Insert       = 260;
    constexpr int Delete       = 261;
    constexpr int Right        = 262;
    constexpr int Left         = 263;
    constexpr int Down         = 264;
    constexpr int Up           = 265;
    constexpr int PageUp       = 266;
    constexpr int PageDown     = 267;
    constexpr int Home         = 268;
    constexpr int End          = 269;
    constexpr int CapsLock     = 280;
    constexpr int ScrollLock   = 281;
    constexpr int NumLock      = 282;
    constexpr int PrintScreen  = 283;
    constexpr int Pause        = 284;
    constexpr int F1           = 290;
    constexpr int F2           = 291;
    constexpr int F3           = 292;
    constexpr int F4           = 293;
    constexpr int F5           = 294;
    constexpr int F6           = 295;
    constexpr int F7           = 296;
    constexpr int F8           = 297;
    constexpr int F9           = 298;
    constexpr int F10          = 299;
    constexpr int F11          = 300;
    constexpr int F12          = 301;
    constexpr int LeftShift    = 340;
    constexpr int LeftControl  = 341;
    constexpr int LeftAlt      = 342;
    constexpr int LeftSuper    = 343;
    constexpr int RightShift   = 344;
    constexpr int RightControl = 345;
    constexpr int RightAlt     = 346;
    constexpr int RightSuper   = 347;

    // Mouse button range (offset to avoid collision with key codes)
    constexpr int MouseButton1 = 1100;  // Left
    constexpr int MouseButton2 = 1101;  // Right
    constexpr int MouseButton3 = 1102;  // Middle
    constexpr int MouseButton4 = 1103;
    constexpr int MouseButton5 = 1104;
    constexpr int MouseButton6 = 1105;
    constexpr int MouseButton7 = 1106;
    constexpr int MouseButton8 = 1107;
} // namespace KeyCodes

// ============================================================
// FKey — a platform-agnostic key identifier.
// Wraps GLFW key codes but abstracts the platform layer.
// ============================================================
struct FKey
{
    int code; // Platform key code (KeyCodes::* or raw GLFW code)

    bool operator==(const FKey& o) const { return code == o.code; }
    bool operator!=(const FKey& o) const { return code != o.code; }

    // Named constants for common keys
    static FKey Invalid()      { return FKey{KeyCodes::Unknown}; }
    static FKey W()            { return FKey{KeyCodes::W}; }
    static FKey A()            { return FKey{KeyCodes::A}; }
    static FKey S()            { return FKey{KeyCodes::S}; }
    static FKey D()            { return FKey{KeyCodes::D}; }
    static FKey Space()        { return FKey{KeyCodes::Space}; }
    static FKey LeftShift()    { return FKey{KeyCodes::LeftShift}; }
    static FKey LeftControl()  { return FKey{KeyCodes::LeftControl}; }
    static FKey LeftAlt()      { return FKey{KeyCodes::LeftAlt}; }
    static FKey F()            { return FKey{KeyCodes::F}; }
    static FKey Escape()       { return FKey{KeyCodes::Escape}; }
    static FKey R()            { return FKey{KeyCodes::R}; }

    static FKey MouseLeft()    { return FKey{KeyCodes::MouseButton1}; }
    static FKey MouseRight()   { return FKey{KeyCodes::MouseButton2}; }
    static FKey MouseMiddle()  { return FKey{KeyCodes::MouseButton3}; }
};

// ============================================================
// EInputAxis — which axis a mapping contributes to.
// ============================================================
enum class EInputAxis : uint8_t
{
    None = 0,
    X,    // horizontal (right positive)
    Y,    // vertical   (forward/up positive)
};

// ============================================================
// FKeyMapping — a single key->action binding within a context.
// Analogous to UE's FEnhancedActionKeyMapping.
//
// One key can contribute to an axis of a 2D action (e.g.
// W contributes +Y to "Move"), or directly to a boolean action.
// ============================================================
struct FKeyMapping
{
    FKey key;

    // The action this key maps to
    InputAction* action = nullptr;

    // For Axis2D actions: which axis does this key contribute to?
    EInputAxis axis = EInputAxis::None;

    // Scale applied to this key's contribution (e.g. -1 for "backward")
    float scale = 1.0f;

    // Modifiers applied before the value reaches the trigger
    std::vector<std::shared_ptr<InputModifier>> modifiers;

    // Trigger that determines when this binding fires the action.
    // If null, uses the action's default trigger.
    std::shared_ptr<InputTrigger> trigger;
};

// ============================================================
// InputMappingContext — a named group of key bindings that can
// be pushed onto a priority stack. Analogous to UE's
// UInputMappingContext.
//
// Example usage:
//   auto gameplay = std::make_shared<InputMappingContext>("Gameplay");
//   gameplay->MapKey(FKey::W(),        GameActions::Move, EInputAxis::Y,  1.0f);
//   gameplay->MapKey(FKey::S(),        GameActions::Move, EInputAxis::Y, -1.0f);
//   gameplay->MapKey(FKey::A(),        GameActions::Move, EInputAxis::X, -1.0f);
//   gameplay->MapKey(FKey::D(),        GameActions::Move, EInputAxis::X,  1.0f);
//   gameplay->MapKey(FKey::Space(),    GameActions::Jump);
//   gameplay->MapKey(FKey::LeftShift(),GameActions::Sprint);
//   gameplay->MapKey(FKey::Mouse(),    GameActions::Look,  EInputAxis::X_Y);
//   gameplay->MapKey(FKey::F(),        GameActions::ToggleFreeCamera, {}, 1.0f, trigger_toggle);
//
//   system->AddMappingContext(gameplay, 0);
// ============================================================
class InputMappingContext
{
public:
    explicit InputMappingContext(const std::string& name) : m_Name(name) {}
    const std::string& GetName() const { return m_Name; }

    // --- Mapping helpers ---

    // Map a key to a boolean action (press/release)
    FKeyMapping& MapKey(FKey key, InputAction& action,
                        std::shared_ptr<InputTrigger> trigger = nullptr)
    {
        FKeyMapping mapping;
        mapping.key = key;
        mapping.action = &action;
        mapping.axis = EInputAxis::None;
        mapping.scale = 1.0f;
        mapping.trigger = trigger;
        m_Mappings.push_back(mapping);
        return m_Mappings.back();
    }

    // Map a key to an axis of a 2D action
    FKeyMapping& MapKey(FKey key, InputAction& action, EInputAxis axis, float scale = 1.0f,
                        std::shared_ptr<InputTrigger> trigger = nullptr)
    {
        FKeyMapping mapping;
        mapping.key = key;
        mapping.action = &action;
        mapping.axis = axis;
        mapping.scale = scale;
        mapping.trigger = trigger;
        m_Mappings.push_back(mapping);
        return m_Mappings.back();
    }

    // Map a key to an axis of a 2D action, with modifiers
    FKeyMapping& MapKey(FKey key, InputAction& action, EInputAxis axis, float scale,
                        std::vector<std::shared_ptr<InputModifier>> modifiers,
                        std::shared_ptr<InputTrigger> trigger = nullptr)
    {
        FKeyMapping mapping;
        mapping.key = key;
        mapping.action = &action;
        mapping.axis = axis;
        mapping.scale = scale;
        mapping.modifiers = std::move(modifiers);
        mapping.trigger = trigger;
        m_Mappings.push_back(mapping);
        return m_Mappings.back();
    }

    const std::vector<FKeyMapping>& GetMappings() const { return m_Mappings; }

private:
    std::string m_Name;
    std::vector<FKeyMapping> m_Mappings;
};

// ============================================================
// FMappingContextStackEntry — an entry in the context stack.
// Higher priority contexts process first and can consume input,
// preventing lower-priority contexts from seeing it.
// ============================================================
struct FMappingContextStackEntry
{
    std::shared_ptr<InputMappingContext> context;
    int priority; // higher = processed first
};
