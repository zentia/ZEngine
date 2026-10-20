#pragma once
#include "CrashCapture.h"

namespace zengine::envcheck::collector
{
struct WerOptions
{
    Options environment;
    std::filesystem::path gameExecutable;
    std::filesystem::path importDump;
    bool memoryConsent = false;
    bool importConfirmed = false;
    unsigned waitSeconds = 3600;
    HWND owner = nullptr;
};
bool IsWerCommand(int argc, wchar_t** argv);
int RunWerCommand(int argc, wchar_t** argv);
DWORD RequestWerRestore(const std::string& executable, HWND owner);
bool HasWerBackup(const std::string& executable);
CaptureResult CaptureWer(const WerOptions&, const std::atomic_bool& stop,
    const std::function<void(const std::string&, bool)>& progress);

// The CLI always uses HKLM. HKCU is used only by isolated registry regression tests.
DWORD ChangeWerRegistry(HKEY hive, const std::wstring& executable, const std::wstring& folder,
    const std::wstring& token, bool restore);
std::wstring ValidateWerExecutable(const std::wstring& executable);
std::wstring NewWerToken();
DWORD RequestWerEnable(const std::wstring& executable, const std::filesystem::path& folder,
    const std::wstring& token, HWND owner);
DWORD RequestWerRestoreSession(const std::wstring& executable, const std::wstring& token, HWND owner);
}
