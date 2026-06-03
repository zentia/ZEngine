#pragma once

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
    Left,
    Right,
    Home,
    End,
    Space,
};
}  // namespace ZSlate
