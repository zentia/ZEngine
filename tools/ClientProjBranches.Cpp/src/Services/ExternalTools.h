#pragma once
#include <string>
#include <cstdlib>
#include <filesystem>
#include <Windows.h>
#include <shellapi.h>
#include <cctype>
#include <string>
#include "IdeSettings.h"
namespace fs = std::filesystem;

namespace ClientProj { namespace Tools {

// Resolve an exe on PATH
inline std::string ResolveOnPath(const std::string& exeName) {
    char pathEnv[32768]; size_t len;
    if (getenv_s(&len, pathEnv, sizeof(pathEnv), "PATH") != 0) return "";
    std::string path(pathEnv);
    size_t pos = 0;
    while (pos < path.size()) {
        auto next = path.find(';', pos);
        std::string dir = path.substr(pos, next - pos);
        pos = (next == std::string::npos) ? path.size() : next + 1;
        if (dir.empty()) continue;
        auto cand = (fs::path(dir) / exeName).string();
        if (fs::exists(cand)) return cand;
    }
    return "";
}

inline std::string ResolveSvn() {
    auto f = ResolveOnPath("svn.exe");
    if (!f.empty()) return f;
    auto fb = "C:\\Program Files\\TortoiseSVN\\bin\\svn.exe";
    return fs::exists(fb) ? fb : "";
}

inline std::string ResolveGit() {
    auto f = ResolveOnPath("git.exe");
    if (!f.empty()) return f;
    const char* fb[] = {
        "C:\\Program Files\\Git\\cmd\\git.exe",
        "C:\\Program Files\\Git\\bin\\git.exe",
    };
    for (auto p : fb) if (fs::exists(p)) return p;
    return "";
}

inline std::string ResolveUgit() {
    char localAppData[32768]; size_t len;
    if (getenv_s(&len, localAppData, sizeof(localAppData), "LOCALAPPDATA") != 0)
        localAppData[0] = 0;
    std::string local(localAppData);
    const char* cands[] = {
        (local + "\\UGit\\bin\\ugit.bat").c_str(),
        (local + "\\UGit\\bin\\ugit.exe").c_str(),
        "C:\\Program Files\\UGit\\bin\\ugit.bat",
        "C:\\Program Files\\UGit\\bin\\ugit.exe",
    };
    // Need static storage for the local paths... just search
    for (auto& s : {local + "\\UGit\\bin\\ugit.bat", local + "\\UGit\\bin\\ugit.exe"}) {
        if (fs::exists(s)) return s;
    }
    for (auto s : {"C:\\Program Files\\UGit\\bin\\ugit.bat", "C:\\Program Files\\UGit\\bin\\ugit.exe"}) {
        if (fs::exists(s)) return s;
    }
    return ResolveOnPath("ugit.bat");
}

inline std::string ResolveCodeBuddy() {
    char localAppData[32768];
    getenv_s(nullptr, localAppData, sizeof(localAppData), "LOCALAPPDATA");
    std::string local(localAppData);
    for (auto s : {local + "\\Programs\\CodeBuddy CN\\bin\\buddycn.cmd",
                   local + "\\Programs\\CodeBuddy\\bin\\buddy.cmd"}) {
        if (fs::exists(s)) return s;
    }
    return ResolveOnPath("buddycn.cmd");
}

inline std::string ResolveTortoiseProc() {
    auto f = ResolveOnPath("TortoiseProc.exe");
    if (!f.empty()) return f;
    auto fb = "C:\\Program Files\\TortoiseSVN\\bin\\TortoiseProc.exe";
    return fs::exists(fb) ? fb : "";
}

inline void OpenExplorer(const std::string& path) {
    ShellExecuteA(nullptr, "open", "explorer.exe", ("\"" + path + "\"").c_str(), nullptr, SW_SHOW);
}

inline void OpenUrl(const std::string& url) {
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
}

inline std::string ResolveCursor() {
    char localAppData[32768];
    getenv_s(nullptr, localAppData, sizeof(localAppData), "LOCALAPPDATA");
    std::string local(localAppData);
    for (auto s : {local + "\\Programs\\Cursor\\Cursor.exe",
                   local + "\\Programs\\cursor\\Cursor.exe"}) {
        if (fs::exists(s)) return s;
    }
    return ResolveOnPath("Cursor.exe");
}

inline void OpenInIde(const std::string& folderPath) {
    // Single source of truth for the preference (C#-compatible storage).
    const std::string ide = IdeSettings::Load();

    if (ide == "cursor") {
        auto cursor = ResolveCursor();
        if (!cursor.empty()) {
            ShellExecuteA(nullptr, "open", cursor.c_str(),
                         ("\"" + folderPath + "\"").c_str(), nullptr, SW_SHOW);
            return;
        }
        // Cursor not installed — fall through to the other IDE.
        auto fallback = ResolveCodeBuddy();
        if (!fallback.empty()) {
            ShellExecuteA(nullptr, "open", fallback.c_str(),
                         ("\"" + folderPath + "\"").c_str(), nullptr, SW_SHOW);
            return;
        }
    } else {
        auto codebuddy = ResolveCodeBuddy();
        if (!codebuddy.empty()) {
            ShellExecuteA(nullptr, "open", codebuddy.c_str(),
                         ("\"" + folderPath + "\"").c_str(), nullptr, SW_SHOW);
            return;
        }
        auto fallback = ResolveCursor();
        if (!fallback.empty()) {
            ShellExecuteA(nullptr, "open", fallback.c_str(),
                         ("\"" + folderPath + "\"").c_str(), nullptr, SW_SHOW);
            return;
        }
    }

    // Neither IDE installed: at least show the folder.
    OpenExplorer(folderPath);
}

inline void OpenPowerShellConsole(const std::string& workingDir, const std::string& command, const std::string& title) {
    std::string escDir = workingDir;
    for (size_t p = 0; (p = escDir.find('\'', p)) != std::string::npos; p += 2)
        escDir.insert(p, "'");
    std::string escCmd = command;
    for (size_t p = 0; (p = escCmd.find('"', p)) != std::string::npos; p += 2)
        escCmd.insert(p, "\\");

    std::string args = "-NoExit -NoProfile -Command \"Set-Location -LiteralPath '" + escDir + "'; " + escCmd + "; Write-Host ''; Write-Host 'Done. Press Enter to close.' -ForegroundColor Yellow; [void](Read-Host)\"";
    ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOW);
}

inline bool IsExistingDirectory(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_directory(path, ec);
}

inline std::string QuotePath(const std::string& path) { return "\"" + path + "\""; }

// Port of C# SvnActions.RunTortoiseVerb: TortoiseProc with /closeonend:0,
// falling back to a visible PowerShell console running svn.exe when
// TortoiseSVN is not installed (only update / cleanup / log have that
// fallback in the C# version too).
inline bool RunTortoiseVerb(const std::string& workingCopy,
                            const std::string& command,
                            const std::string& extraArgs = "",
                            const std::string& svnArgs   = "")
{
    if (!IsExistingDirectory(workingCopy)) return false;

    const std::string tortoise = ResolveTortoiseProc();
    if (!tortoise.empty() && fs::exists(tortoise)) {
        std::string args = "/command:" + command + " /path:" + QuotePath(workingCopy);
        if (!extraArgs.empty()) args += " " + extraArgs;
        args += " /closeonend:0";
        ShellExecuteA(nullptr, "open", tortoise.c_str(), args.c_str(), nullptr, SW_SHOW);
        return true;
    }

    const std::string svn = ResolveSvn();
    if (svn.empty()) return false;

    const std::string body = "& '" + svn + "' "
                           + (svnArgs.empty() ? command : svnArgs)
                           + " '" + workingCopy + "'";
    OpenPowerShellConsole(workingCopy, body, "SVN " + command);
    return true;
}

inline void OpenTortoiseProc(const std::string& command, const std::string& path) {
    RunTortoiseVerb(path, command);
}

inline void OpenTortoiseLog(const std::string& path) {
    RunTortoiseVerb(path, "log", "", "log -l 20");
}

// HEAD / BASE / PREV / COMMITTED, or a positive integer (C# UpdateToRevision).
inline bool IsValidSvnRevision(const std::string& revision) {
    if (revision.empty()) return false;

    std::string upper;
    upper.reserve(revision.size());
    for (char c : revision) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "HEAD" || upper == "BASE" || upper == "PREV" || upper == "COMMITTED") return true;

    try {
        if (std::stoll(revision) > 0) return true;
    } catch (...) { /* not a number */ }
    return false;
}

inline void KillProcess(const std::string& name) {
    std::string cmd = "taskkill /f /im " + name + " 2>nul";
    system(cmd.c_str());
}

} }
