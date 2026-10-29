#pragma once

#include "ZSlate/Core/ZSlateTypes.h"

// Clipboard support for ZSlate widgets (SEditableTextBox etc.)
// Process-wide callbacks set by the host during initialization.
// This avoids pulling GLFW/WindowSystem dependencies into Slate widget headers.
namespace ZSlate
{
using SlateGetClipboardFunc = const char* (*)(void* userdata);
using SlateSetClipboardFunc = void (*)(const char*, void* userdata);

extern SlateGetClipboardFunc GSlateGetClipboard;
extern SlateSetClipboardFunc GSlateSetClipboard;
extern void* GSlateClipboardUserData;  // typically GLFWwindow*
}  // namespace ZSlate
