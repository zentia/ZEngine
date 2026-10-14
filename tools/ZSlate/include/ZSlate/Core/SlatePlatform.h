#pragma once

// ============================================================================
// ZSlate/Core/SlatePlatform.h
//
// The host application implements ISlatePlatform and installs it once via
// ZSlate::SetPlatform(). ZSlate only ever asks for the renderer, cursor state
// and timing — it never creates windows or pumps messages itself.
// ============================================================================

#include "ZSlateTypes.h"
#include "SlatePaint.h"

namespace ZSlate
{

class ISlatePlatform
{
public:
    virtual ~ISlatePlatform() = default;

    virtual ISlateRenderer* GetRenderer() = 0;
    virtual Vector2 GetMousePosition() const = 0;
    virtual bool    IsMouseButtonDown(int button) const = 0;
    virtual bool    IsKeyDown(int keyCode) const = 0;
    virtual float   GetTimeSeconds() const = 0;
    virtual Vector2 GetWindowSize() const = 0;
};

void            SetPlatform(ISlatePlatform* platform);
ISlatePlatform* GetPlatform();

// Blinking caret helper for text widgets (1 Hz, 50 % duty cycle).
inline bool IsCaretVisible(float periodSeconds = 1.0f)
{
    ISlatePlatform* platform = GetPlatform();
    const float t = platform ? platform->GetTimeSeconds() : 0.0f;
    const float m = std::fmod(t, periodSeconds);
    return m < periodSeconds * 0.5f;
}

} // namespace ZSlate
