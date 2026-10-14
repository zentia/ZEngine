#pragma once

// ============================================================================
// ZSlate/Core/SlateClipboard.h
//
// Optional clipboard bridge. ZSlate never touches the OS clipboard itself —
// the host application installs a sink here. When no sink is installed the
// framework simply reports "clipboard unavailable".
// ============================================================================

#include <string>

namespace ZSlate
{

// Called with UTF-8 text. `userData` is whatever the host stored below.
extern void (*GSlateSetClipboard)(const char* text, void* userData);
extern void* GSlateClipboardUserData;

inline bool  IsClipboardAvailable() { return GSlateSetClipboard != nullptr; }

inline void  SetClipboardText(const std::string& text)
{
    if (GSlateSetClipboard) GSlateSetClipboard(text.c_str(), GSlateClipboardUserData);
}

// Convenience RAII-less installer (host owns the function).
inline void InstallClipboardSink(void (*fn)(const char*, void*), void* userData)
{
    GSlateSetClipboard     = fn;
    GSlateClipboardUserData = userData;
}

} // namespace ZSlate
