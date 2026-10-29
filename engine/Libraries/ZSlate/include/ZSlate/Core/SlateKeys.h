#pragma once

#include "ZSlate/Core/ZSlateTypes.h"

namespace ZSlate
{
// Backend-agnostic key identities routed to focused widgets. The host maps its
// platform/ImGui key codes onto these before calling SlateInputRouter::ProcessKey.
enum class EKey
{
    Unknown,
    Backspace,
    Delete,
    Enter,
    Escape,
    Tab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Space,

    // Letter keys (for Ctrl+ shortcuts: A=SelectAll, C=Copy, V=Paste, X=Cut, Z=Undo, Y=Redo)
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
};
}  // namespace ZSlate
