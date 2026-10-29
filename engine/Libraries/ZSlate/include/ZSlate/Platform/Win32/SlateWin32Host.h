#pragma once
// ============================================================================
// SlateWin32Host — the Win32 services every standalone ZSlate tool needs
// ============================================================================
// System font discovery, virtual-key mapping, the OS clipboard bridge and
// UTF-8 command-line access.
//
// Fonts: five tools each carried their own hardcoded "C:\Windows\Fonts\..."
// candidate list, and four of the five hardcoded the drive letter — which
// breaks on any machine where Windows is not on C:. Only
// WindowsEnvironmentCheck derived the directory via GetWindowsDirectoryW().
// That version is now the only one.
//
// Key mapping: three near-identical MapVirtualKey() switch statements.
//
// Clipboard: ZSlate never touches the OS clipboard itself (see
// SlateClipboard.h); the host installs the callbacks. Only
// ClientProjBranches had an implementation, so text boxes in the other four
// tools silently could not copy.
// ============================================================================

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef DrawText  // windows.h #defines DrawText -> DrawTextA/W; clashes with ISlateRenderer

#include "ZSlate/Core/SlateKeys.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"

#include <string>
#include <vector>

namespace ZSlate
{
namespace Win32
{

// Which face to lead with. The CJK fallback chain is appended either way,
// because tool content (file paths, branch names, log lines) can contain
// Chinese regardless of the UI language.
enum class ESystemFont
{
    Sans,  // Segoe UI  — general UI text
    Mono,  // Consolas  — stacks, tables, anything column-aligned
};

// Loads the first available primary face into `atlas`, then the first
// available CJK fallback. The Fonts directory comes from
// GetWindowsDirectoryW(), never a hardcoded path.
// Returns false only when no usable face was found at all -- callers that
// treat that as fatal (WindowsEnvironmentCheck) can, callers that merely
// degrade to blocky text (TexPreview) can carry on.
bool LoadSystemFonts(ZSlateFontAtlas& atlas, ESystemFont preference);

// Win32 virtual-key -> ZSlate key identity. Letters matter for Ctrl+shortcuts;
// anything unmapped comes back as EKey::Unknown.
EKey KeyFromVirtualKey(unsigned vk);

// Installs OpenClipboard/SetClipboardData as ZSlate's clipboard backend
// (CF_UNICODETEXT, UTF-8 at the ZSlate boundary). Idempotent.
void InstallClipboardBridge();

// UTF-8 conversion helpers. Both return an empty string on failure rather
// than throwing, so they are safe to call during startup diagnostics.
std::string Utf8FromWide(const wchar_t* wide);
std::wstring WideFromUtf8(const char* utf8);

// argv in UTF-8. /SUBSYSTEM:WINDOWS executables cannot rely on the CRT's
// __argv (it is in the ANSI codepage, so non-ASCII paths arrive mangled);
// this reads GetCommandLineW() instead.
//
// Kept for ZSlate's own users. Inside ZEngine, use
// zengine::appshell::CommandLine (ZAppShell/AppMain.h) instead: it is
// header-only, so the /MT ZEnvironmentCollector can use the same source as the
// /MD tree -- which a library like this can never serve.
std::vector<std::string> CommandLineArgsUtf8();

// The console helpers that used to live here (EnsureConsole /
// StdOutIsRedirected) are gone: zengine::appshell::EnsureConsole() in
// ZAppShell/AppMain.h is the one implementation now, and it works for /MT and
// /MD alike.

}  // namespace Win32
}  // namespace ZSlate

#endif  // _WIN32
