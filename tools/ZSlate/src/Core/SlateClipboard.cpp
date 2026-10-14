#include "ZSlate/Core/SlateClipboard.h"

namespace ZSlate
{

// No sink installed by default — the framework reports "clipboard unavailable"
// until the host installs one.
void (*GSlateSetClipboard)(const char* text, void* userData) = nullptr;
void* GSlateClipboardUserData = nullptr;

} // namespace ZSlate
