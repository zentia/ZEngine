#pragma once
#pragma execution_character_set("utf-8")

// Key code definitions compatible with GLFW key codes.
// This allows removing the GLFW dependency while keeping existing
// key mappings intact.
//
// Reference: GLFW 3.x glfw3.h
//
// Printable keys (letters, digits, punctuation) use ASCII values.
// Function and special keys use values >= 256.

namespace KeyCodes
{
    // ===== Printable keys (ASCII-compatible) =====

    enum class Key : int
    {
        Space            = 32,  // ' '
        Apostrophe      = 33,  // '''
        Comma            = 44,  // ','
        Minus            = 45,  // '-'
        Period           = 46,  // '.'
        Slash            = 47,  // '/'
        D0               = 48,  // '0'
        D1               = 49,  // '1'
        D2               = 50,  // '2'
        D3               = 51,  // '3'
        D4               = 52,  // '4'
        D5               = 53,  // '5'
        D6               = 54,  // '6'
        D7               = 55,  // '7'
        D8               = 56,  // '8'
        D9               = 57,  // '9'
        Semicolon       = 59,  // ';'
        Equal            = 61,  // '='
        A                = 65,  // 'A'
        B                = 66,  // 'B'
        C                = 67,  // 'C'
        D                = 68,  // 'D'
        E                = 69,  // 'E'
        F                = 70,  // 'F'
        G                = 71,  // 'G'
        H                = 72,  // 'H'
        I                = 73,  // 'I'
        J                = 74,  // 'J'
        K                = 75,  // 'K'
        L                = 76,  // 'L'
        M                = 77,  // 'M'
        N                = 78,  // 'N'
        O                = 79,  // 'O'
        P                = 80,  // 'P'
        Q                = 81,  // 'Q'
        R                = 82,  // 'R'
        S                = 83,  // 'S'
        T                = 84,  // 'T'
        U                = 85,  // 'U'
        V                = 86,  // 'V'
        W                = 87,  // 'W'
        X                = 88,  // 'X'
        Y                = 89,  // 'Y'
        Z                = 90,  // 'Z'
        LeftBracket      = 91,  // '['
        Backslash        = 92,  // '\'
        RightBracket     = 93,  // ']'
        GraveAccent     = 96,  // '`'
        World1           = 161, // '§' (non-US #1)
        World2           = 162, // '¶' (non-US #2)

        // ===== Function keys =====

        Escape           = 256,
        Enter            = 257,
        Tab              = 258,
        Backspace        = 259,
        Insert           = 260,
        Delete           = 261,
        Right            = 262,
        Left             = 263,
        Down             = 264,
        Up               = 265,
        PageUp           = 266,
        PageDown         = 267,
        Home             = 268,
        End              = 269,
        CapsLock        = 280,
        ScrollLock      = 281,
        NumLock         = 282,
        PrintScreen     = 283,
        Pause            = 284,
        F1               = 290,
        F2               = 291,
        F3               = 292,
        F4               = 293,
        F5               = 294,
        F6               = 295,
        F7               = 296,
        F8               = 297,
        F9               = 298,
        F10              = 299,
        F11              = 300,
        F12              = 301,
        F13              = 302,
        F14              = 303,
        F15              = 304,
        F16              = 305,
        F17              = 306,
        F18              = 307,
        F19              = 308,
        F20              = 309,
        F21              = 310,
        F22              = 311,
        F23              = 312,
        F24              = 313,
        F25              = 314,

        // ===== Keypad keys =====

        KP0              = 320,
        KP1              = 321,
        KP2              = 322,
        KP3              = 323,
        KP4              = 324,
        KP5              = 325,
        KP6              = 326,
        KP7              = 327,
        KP8              = 328,
        KP9              = 329,
        KPDecimal        = 330,
        KPDivide         = 331,
        KPMultiply       = 332,
        KPSubtract       = 333,
        KPAdd            = 334,
        KPEnter          = 335,
        KPEqual          = 336,

        // ===== Lock/modifier keys =====

        LeftShift        = 340,
        LeftControl      = 341,
        LeftAlt          = 342,
        LeftSuper        = 343,
        RightShift       = 344,
        RightControl     = 345,
        RightAlt         = 346,
        RightSuper       = 347,
        Menu             = 348,

        // ===== Mouse buttons (GLFW-compatible) =====

        MouseButton1     = 0,   // Left
        MouseButton2     = 1,   // Right
        MouseButton3     = 2,   // Middle
        MouseButton4     = 3,
        MouseButton5     = 4,
        MouseButton6     = 5,
        MouseButton7     = 6,
        MouseButton8     = 7,
    };

    // Convenience aliases for mouse buttons
    constexpr Key MouseButtonLast   = Key::MouseButton8;
    constexpr Key MouseButtonLeft   = Key::MouseButton1;
    constexpr Key MouseButtonRight  = Key::MouseButton2;
    constexpr Key MouseButtonMiddle = Key::MouseButton3;

    // ===== Action codes (GLFW-compatible) =====

    enum class Action : int
    {
        Press            = 1,
        Release         = 0,
        Repeat          = 2,
    };

    // ===== Modifier key flags (GLFW-compatible, bitmask) =====

    enum class Mod : int
    {
        SHIFT       = 0x0001,
        CONTROL    = 0x0002,
        ALT         = 0x0004,
        SUPER      = 0x0008,
        CAPS_LOCK  = 0x0010,
        NUM_LOCK   = 0x0020,
    };

    // Helper: check if a modifier flag is set
    inline bool HasMod(int mods, Mod flag) { return (mods & static_cast<int>(flag)) != 0; }

    // Backwards-compatible names (match GLFW_MOD_* pattern)
    // NOTE: Names use _VAL suffix to avoid PCH cache mismatch (MSVC PCH
    //       caches old static_cast<int> syntax that fails C2059).
    constexpr int MOD_SHIFT_VAL     = 0x0001;
    constexpr int MOD_CONTROL_VAL  = 0x0002;
    constexpr int MOD_ALT_VAL       = 0x0004;
    constexpr int MOD_SUPER_VAL    = 0x0008;
    constexpr int MOD_CAPS_LOCK_VAL = 0x0010;
    constexpr int MOD_NUM_LOCK_VAL  = 0x0020;

    // Backwards-compatible key code aliases (match GLFW_KEY_* pattern)
    // Printable keys
    constexpr int KEY_Space        = 32;
    constexpr int KEY_Apostrophe    = 33;
    constexpr int KEY_Comma        = 44;
    constexpr int KEY_Minus        = 45;
    constexpr int KEY_Period       = 46;
    constexpr int KEY_Slash        = 47;
    constexpr int KEY_D0           = 48;
    constexpr int KEY_D1           = 49;
    constexpr int KEY_D2           = 50;
    constexpr int KEY_D3           = 51;
    constexpr int KEY_D4           = 52;
    constexpr int KEY_D5           = 53;
    constexpr int KEY_D6           = 54;
    constexpr int KEY_D7           = 55;
    constexpr int KEY_D8           = 56;
    constexpr int KEY_D9           = 57;
    constexpr int KEY_Semicolon   = 59;
    constexpr int KEY_Equal        = 61;
    constexpr int KEY_A            = 65;
    constexpr int KEY_B            = 66;
    constexpr int KEY_C            = 67;
    constexpr int KEY_D            = 68;
    constexpr int KEY_E            = 69;
    constexpr int KEY_F            = 70;
    constexpr int KEY_G            = 71;
    constexpr int KEY_H            = 72;
    constexpr int KEY_I            = 73;
    constexpr int KEY_J            = 74;
    constexpr int KEY_K            = 75;
    constexpr int KEY_L            = 76;
    constexpr int KEY_M            = 77;
    constexpr int KEY_N            = 78;
    constexpr int KEY_O            = 79;
    constexpr int KEY_P            = 80;
    constexpr int KEY_Q            = 81;
    constexpr int KEY_R            = 82;
    constexpr int KEY_S            = 83;
    constexpr int KEY_T            = 84;
    constexpr int KEY_U            = 85;
    constexpr int KEY_V            = 86;
    constexpr int KEY_W            = 87;
    constexpr int KEY_X            = 88;
    constexpr int KEY_Y            = 89;
    constexpr int KEY_Z            = 90;
    constexpr int KEY_LeftBracket  = 91;
    constexpr int KEY_Backslash    = 92;
    constexpr int KEY_RightBracket = 93;
    constexpr int KEY_GraveAccent = 96;
    // Function keys
    constexpr int KEY_Escape       = 256;
    constexpr int KEY_Enter        = 257;
    constexpr int KEY_Tab          = 258;
    constexpr int KEY_Backspace    = 259;
    constexpr int KEY_Insert       = 260;
    constexpr int KEY_Delete       = 261;
    constexpr int KEY_Right        = 262;
    constexpr int KEY_Left         = 263;
    constexpr int KEY_Down         = 264;
    constexpr int KEY_Up           = 265;
    constexpr int KEY_PageUp       = 266;
    constexpr int KEY_PageDown     = 267;
    constexpr int KEY_Home         = 268;
    constexpr int KEY_End          = 269;
    constexpr int KEY_CapsLock    = 280;
    constexpr int KEY_ScrollLock  = 281;
    constexpr int KEY_NumLock     = 282;
    constexpr int KEY_Pause       = 284;
    constexpr int KEY_F1           = 290;
    constexpr int KEY_F2           = 291;
    constexpr int KEY_F3           = 292;
    constexpr int KEY_F4           = 293;
    constexpr int KEY_F5           = 294;
    constexpr int KEY_F6           = 295;
    constexpr int KEY_F7           = 296;
    constexpr int KEY_F8           = 297;
    constexpr int KEY_F9           = 298;
    constexpr int KEY_F10          = 299;
    constexpr int KEY_F11          = 300;
    constexpr int KEY_F12          = 301;
    // Modifier keys
    constexpr int KEY_LeftShift   = 340;
    constexpr int KEY_LeftControl = 341;
    constexpr int KEY_LeftAlt     = 342;
    constexpr int KEY_LeftSuper   = 343;
    constexpr int KEY_RightShift  = 344;
    constexpr int KEY_RightControl= 345;
    constexpr int KEY_RightAlt    = 346;
    constexpr int KEY_RightSuper = 347;
    constexpr int KEY_Menu        = 348;

    // Keypad keys
    constexpr int KEY_KP0          = 320;
    constexpr int KEY_KP1          = 321;
    constexpr int KEY_KP2          = 322;
    constexpr int KEY_KP3          = 323;
    constexpr int KEY_KP4          = 324;
    constexpr int KEY_KP5          = 325;
    constexpr int KEY_KP6          = 326;
    constexpr int KEY_KP7          = 327;
    constexpr int KEY_KP8          = 328;
    constexpr int KEY_KP9          = 329;
    constexpr int KEY_KPDecimal   = 330;
    constexpr int KEY_KPDivide    = 331;
    constexpr int KEY_KPMultiply   = 332;
    constexpr int KEY_KPSubtract   = 333;
    constexpr int KEY_KPAdd       = 334;
    constexpr int KEY_KPEnter     = 335;
    constexpr int KEY_KPEqual     = 336;

    // Backwards-compatible action aliases (match GLFW_PRESS etc.)
    constexpr int PRESS   = 1;
    constexpr int RELEASE = 0;
    constexpr int REPEAT  = 2;
}
