#pragma once

// Single source of truth for the editor chrome above the dock host (menu bar + playback toolbar).
// Must match MenuController and DefaultLayout dock sizing.
namespace EditorLayoutConstants
{
    constexpr float kMainMenuBarHeight = 18.0f;
    constexpr float kPlaybackToolbarHeight = 38.0f;
    constexpr float kReservedTopHeight = kMainMenuBarHeight + kPlaybackToolbarHeight;
}  // namespace EditorLayoutConstants
