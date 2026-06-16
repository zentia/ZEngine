#pragma once

// Clipboard support for ZSlate widgets (SEditableTextBox etc.)
// Process-wide callbacks set by the host during initialization.
// This avoids pulling GLFW/WindowSystem dependencies into Slate widget headers.
namespace ZSlate
{
typedef const char* (*SlateGetClipboardFunc)(void* userdata);
typedef void (*SlateSetClipboardFunc)(const char*, void* userdata);

extern SlateGetClipboardFunc GSlateGetClipboard;
extern SlateSetClipboardFunc GSlateSetClipboard;
extern void* GSlateClipboardUserData;  // typically GLFWwindow*
}  // namespace ZSlate
