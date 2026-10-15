#include "WerCapture.h"
#include "DiagnosisUtil.h"
#include <sddl.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <iostream>
#include <stdexcept>

namespace zengine::envcheck::collector
{
namespace
{
constexpr wchar_t LocalDumps[] = L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\";
constexpr wchar_t Backups[] = L"SOFTWARE\\ZEngine\\EnvironmentCollector\\WerBackupsV1\\";
constexpr wchar_t BackupAcl[] = L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)";
constexpr REGSAM View = KEY_WOW64_64KEY;
constexpr std::array<const wchar_t*, 3> Names{L"DumpFolder", L"DumpType", L"DumpCount"};
struct Key
{
    HKEY h = nullptr;
    ~Key() { if (h) RegCloseKey(h); }
};
struct Local
{
    HLOCAL p = nullptr;
    ~Local() { if (p) LocalFree(p); }
};
struct Handle
{
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
void Check(LSTATUS code) { if (code != ERROR_SUCCESS) throw static_cast<DWORD>(code); }
struct Value
{
    bool exists = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> bytes;
    bool operator==(const Value&) const = default;
};
Value Read(HKEY key, const std::wstring& name)
{
    Value value;
    if (!key) return value;
    DWORD size = 0;
    auto status = RegQueryValueExW(key, name.c_str(), nullptr, &value.type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    Check(status);
    if (size > 65536) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    value.bytes.resize(size);
    Check(RegQueryValueExW(key, name.c_str(), nullptr, &value.type, value.bytes.data(), &size));
    value.bytes.resize(size); value.exists = true;
    return value;
}
void Write(HKEY key, const std::wstring& name, const Value& value)
{
    if (value.exists) Check(RegSetValueExW(key, name.c_str(), 0, value.type, value.bytes.data(), static_cast<DWORD>(value.bytes.size())));
    else { auto code = RegDeleteValueW(key, name.c_str()); if (code != ERROR_FILE_NOT_FOUND) Check(code); }
}
Value Number(DWORD number)
{
    Value v{true, REG_DWORD, std::vector<BYTE>(sizeof(number))};
    memcpy(v.bytes.data(), &number, sizeof(number)); return v;
}
DWORD NumberOf(const Value& v)
{
    if (!v.exists || v.type != REG_DWORD || v.bytes.size() != sizeof(DWORD)) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    DWORD result; memcpy(&result, v.bytes.data(), sizeof(result)); return result;
}
Value Text(const std::wstring& text, DWORD type = REG_SZ)
{
    Value v{true, type, std::vector<BYTE>((text.size() + 1) * sizeof(wchar_t))};
    memcpy(v.bytes.data(), text.c_str(), v.bytes.size()); return v;
}
std::wstring TextOf(const Value& v)
{
    if (!v.exists || v.type != REG_SZ || v.bytes.size() < 2 || v.bytes.size() % 2) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    std::wstring text(v.bytes.size() / 2, L'\0'); memcpy(text.data(), v.bytes.data(), v.bytes.size());
    if (text.back() != 0) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    text.pop_back(); if (text.find(L'\0') != std::wstring::npos) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    return text;
}
void SaveValue(HKEY backup, const std::wstring& prefix, const Value& v)
{
    Write(backup, prefix + L"Exists", Number(v.exists));
    Write(backup, prefix + L"Type", Number(v.type));
    Write(backup, prefix + L"Bytes", Value{true, REG_BINARY, v.bytes});
}
Value LoadValue(HKEY backup, const std::wstring& prefix)
{
    Value v; v.exists = NumberOf(Read(backup, prefix + L"Exists")) != 0;
    v.type = NumberOf(Read(backup, prefix + L"Type"));
    const auto bytes = Read(backup, prefix + L"Bytes");
    if (!bytes.exists || bytes.type != REG_BINARY) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    v.bytes = bytes.bytes; return v;
}
void CheckBackupAcl(HKEY key)
{
    DWORD size = 0;
    RegGetKeySecurity(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, &size);
    if (!size || size > 65536) throw static_cast<DWORD>(ERROR_ACCESS_DENIED);
    std::vector<BYTE> data(size);
    Check(RegGetKeySecurity(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, data.data(), &size));
    Local expected;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(BackupAcl, SDDL_REVISION_1, &expected.p, nullptr)) throw GetLastError();
    PACL actualAcl = nullptr, expectedAcl = nullptr; PSID owner = nullptr, expectedOwner = nullptr;
    BOOL present = FALSE, def = FALSE; SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    if (!GetSecurityDescriptorControl(data.data(), &control, &revision) || !(control & SE_DACL_PROTECTED) ||
        !GetSecurityDescriptorDacl(data.data(), &present, &actualAcl, &def) || !present || !actualAcl ||
        !GetSecurityDescriptorOwner(data.data(), &owner, &def) || !owner ||
        !GetSecurityDescriptorDacl(expected.p, &present, &expectedAcl, &def) ||
        !GetSecurityDescriptorOwner(expected.p, &expectedOwner, &def) || !EqualSid(owner, expectedOwner) ||
        actualAcl->AclSize != expectedAcl->AclSize || memcmp(actualAcl, expectedAcl, actualAcl->AclSize))
        throw static_cast<DWORD>(ERROR_ACCESS_DENIED);
}
bool ValidToken(const std::wstring& token)
{
    return token.size() == 36 && std::all_of(token.begin(), token.end(), [](wchar_t c) { return iswxdigit(c) || c == L'-'; });
}
void ValidateFolder(const std::wstring& folder)
{
    if (folder.size() < 4 || folder.size() > 240 || folder[1] != L':' || folder[2] != L'\\' ||
        folder.find_first_of(L"%\"\r\n") != std::wstring::npos) throw static_cast<DWORD>(ERROR_INVALID_NAME);
    auto path = std::filesystem::path(folder);
    if (path.lexically_normal() != path) throw static_cast<DWORD>(ERROR_INVALID_NAME);
    for (auto current = path; !current.empty(); current = current == current.root_path() ? std::filesystem::path{} : current.parent_path())
    {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw static_cast<DWORD>(ERROR_ACCESS_DENIED);
    }
}
std::wstring Quote(const std::wstring& text)
{
    std::wstring out = L"\""; size_t slashes = 0;
    for (auto c : text)
    {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\'); out += c; slashes = 0;
    }
    out.append(slashes * 2, L'\\'); return out + L'\"';
}
DWORD Request(const std::wstring& args, HWND owner)
{
    struct ComScope
    {
        HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
    } com;
    const auto exe = ToolExecutablePath();
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC; launch.hwnd = owner;
    launch.lpVerb = L"runas"; launch.lpFile = exe.c_str(); launch.lpParameters = args.c_str(); launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) return GetLastError();
    Handle child{launch.hProcess};
    if (!child.h || WaitForSingleObject(child.h, INFINITE) != WAIT_OBJECT_0) return ERROR_GEN_FAILURE;
    DWORD exit = ERROR_GEN_FAILURE; GetExitCodeProcess(child.h, &exit); return exit;
}
}
std::wstring ValidateWerExecutable(const std::wstring& name)
{
    if (name.size() < 5 || name.size() > 128 || name[0] == L'.' ||
        !std::all_of(name.begin(), name.end(), [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-'; }))
        throw std::invalid_argument("WER目标必须是安全的exe文件名");
    std::wstring lower = name; std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if (!lower.ends_with(L".exe")) throw std::invalid_argument("WER目标必须以.exe结尾");
    return lower;
}
std::wstring NewWerToken()
{
    GUID id{}; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("无法建立WER会话标识");
    wchar_t text[40]{}; StringFromGUID2(id, text, 40); return std::wstring(text + 1, 36);
}
DWORD ChangeWerRegistry(HKEY hive, const std::wstring& executable, const std::wstring& folder, const std::wstring& token, bool restore)
{
    try
    {
        const auto name = ValidateWerExecutable(executable);
        if (!ValidToken(token)) return ERROR_INVALID_PARAMETER;
        Handle mutex{CreateMutexW(nullptr, FALSE, (L"Global\\ZEnvironmentCollector-WER64-" + name).c_str())};
        if (!mutex.h) return GetLastError();
        const DWORD wait = WaitForSingleObject(mutex.h, 10000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return ERROR_BUSY;
        struct Unlock { HANDLE h; ~Unlock() { ReleaseMutex(h); } } unlock{mutex.h};
        const auto targetPath = std::wstring(LocalDumps) + name, backupPath = std::wstring(Backups) + name;
        Key backup, target;
        const auto opened = RegOpenKeyExW(hive, backupPath.c_str(), 0, KEY_ALL_ACCESS | View, &backup.h);
        if (!restore && opened != ERROR_FILE_NOT_FOUND) return opened == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : opened;
        if (restore && opened == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (restore) Check(opened);
        DWORD disposition = 0;
        if (!restore)
        {
            ValidateFolder(folder);
            Local sd;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(BackupAcl, SDDL_REVISION_1, &sd.p, nullptr)) throw GetLastError();
            SECURITY_ATTRIBUTES sa{sizeof(sa), sd.p, FALSE};
            Check(RegCreateKeyExW(hive, backupPath.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | View, hive == HKEY_LOCAL_MACHINE ? &sa : nullptr, &backup.h, &disposition));
            if (disposition != REG_CREATED_NEW_KEY) return ERROR_ALREADY_EXISTS;
        }
        if (hive == HKEY_LOCAL_MACHINE) CheckBackupAcl(backup.h);
        const auto targetStatus = RegOpenKeyExW(hive, targetPath.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | View, &target.h);
        if (targetStatus != ERROR_FILE_NOT_FOUND) Check(targetStatus);
        std::array<Value, 3> original, expected;
        if (!restore)
        {
            Write(backup.h, L"Token", Text(token));
            Write(backup.h, L"TargetExisted", Number(targetStatus == ERROR_SUCCESS));
            expected = {Text(folder, REG_EXPAND_SZ), Number(2), Number(2)};
            for (size_t i = 0; i < Names.size(); ++i)
            {
                original[i] = Read(target.h, Names[i]);
                SaveValue(backup.h, L"Original" + std::to_wstring(i), original[i]);
                SaveValue(backup.h, L"Expected" + std::to_wstring(i), expected[i]);
            }
            Write(backup.h, L"Ready", Number(1)); Check(RegFlushKey(backup.h));
            if (!target.h) Check(RegCreateKeyExW(hive, targetPath.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | View, nullptr, &target.h, nullptr));
            for (size_t i = 0; i < Names.size(); ++i) Write(target.h, Names[i], expected[i]);
            Check(RegFlushKey(target.h));
            for (size_t i = 0; i < Names.size(); ++i) if (Read(target.h, Names[i]) != expected[i]) return ERROR_WRITE_FAULT;
            return ERROR_SUCCESS;
        }
        if (Read(backup.h, L"Ready") != Number(1))
        {
            // The durable ready marker always precedes the first target write.
            RegCloseKey(backup.h); backup.h = nullptr;
            Check(RegDeleteKeyExW(hive, backupPath.c_str(), View, 0)); return ERROR_SUCCESS;
        }
        if (TextOf(Read(backup.h, L"Token")) != token) return ERROR_REVISION_MISMATCH;
        for (size_t i = 0; i < Names.size(); ++i)
        {
            original[i] = LoadValue(backup.h, L"Original" + std::to_wstring(i));
            expected[i] = LoadValue(backup.h, L"Expected" + std::to_wstring(i));
            auto current = Read(target.h, Names[i]);
            if (current != original[i] && current != expected[i]) return ERROR_REVISION_MISMATCH;
        }
        if (!target.h) Check(RegCreateKeyExW(hive, targetPath.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | View, nullptr, &target.h, nullptr));
        for (size_t i = 0; i < Names.size(); ++i) Write(target.h, Names[i], original[i]);
        Check(RegFlushKey(target.h));
        for (size_t i = 0; i < Names.size(); ++i) if (Read(target.h, Names[i]) != original[i]) return ERROR_WRITE_FAULT;
        DWORD values = 0, children = 0;
        Check(RegQueryInfoKeyW(target.h, nullptr, nullptr, nullptr, &children, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr));
        const bool remove = NumberOf(Read(backup.h, L"TargetExisted")) == 0 && !values && !children;
        RegCloseKey(target.h); target.h = nullptr;
        if (remove) Check(RegDeleteKeyExW(hive, targetPath.c_str(), View, 0));
        RegCloseKey(backup.h); backup.h = nullptr;
        Check(RegDeleteKeyExW(hive, backupPath.c_str(), View, 0));
        return ERROR_SUCCESS;
    }
    catch (DWORD code) { return code; }
    catch (...) { return ERROR_INVALID_DATA; }
}
bool HasWerBackup(const std::string& executable)
{
    try
    {
        Key key; const auto path = std::wstring(Backups) + ValidateWerExecutable(diagnosis::Utf8ToWide(executable));
        auto code = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | View, &key.h);
        return code != ERROR_FILE_NOT_FOUND;
    }
    catch (...) { return true; }
}
DWORD RequestWerEnable(const std::wstring& exe, const std::filesystem::path& folder, const std::wstring& token, HWND owner)
{
    return Request(L"--wer-config enable " + Quote(ValidateWerExecutable(exe)) + L" " + Quote(token) + L" " + Quote(folder.wstring()), owner);
}
DWORD RequestWerRestoreSession(const std::wstring& exe, const std::wstring& token, HWND owner)
{
    return Request(L"--wer-config restore " + Quote(ValidateWerExecutable(exe)) + L" " + Quote(token), owner);
}
DWORD RequestWerRestore(const std::string& executable, HWND owner)
{
    try
    {
        const auto exe = ValidateWerExecutable(diagnosis::Utf8ToWide(executable));
        Key key; auto code = RegOpenKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(Backups) + exe).c_str(), 0, KEY_READ | View, &key.h);
        if (code == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        Check(code); CheckBackupAcl(key.h);
        return RequestWerRestoreSession(exe, Read(key.h, L"Ready") == Number(1) ? TextOf(Read(key.h, L"Token")) : NewWerToken(), owner);
    }
    catch (DWORD code) { return code; }
    catch (...) { return ERROR_INVALID_DATA; }
}
bool IsWerCommand(int argc, wchar_t** argv) { return argc > 1 && std::wstring(argv[1]) == L"--wer-config"; }
int RunWerCommand(int argc, wchar_t** argv)
{
    if (argc < 3) return ERROR_INVALID_PARAMETER;
    const bool restore = std::wstring(argv[2]) == L"restore";
    if ((restore && argc != 5) || (!restore && (std::wstring(argv[2]) != L"enable" || argc != 6))) return ERROR_INVALID_PARAMETER;
    HANDLE token = nullptr; TOKEN_ELEVATION elevation{}; DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return ERROR_ACCESS_DENIED;
    Handle owned{token};
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) || !elevation.TokenIsElevated) return ERROR_ELEVATION_REQUIRED;
    return static_cast<int>(ChangeWerRegistry(HKEY_LOCAL_MACHINE, argv[3], restore ? L"" : argv[5], argv[4], restore));
}
}
