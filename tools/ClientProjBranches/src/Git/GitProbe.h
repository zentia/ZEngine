#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <Windows.h>
#include <cstdlib>
namespace fs = std::filesystem;

namespace ClientProj { namespace GitProbe {

inline std::string ResolveGitExe() {
    // PATH lookup
    char pathEnv[32768]; size_t len;
    if (getenv_s(&len, pathEnv, sizeof(pathEnv), "PATH") == 0) {
        std::string path(pathEnv);
        size_t pos = 0;
        while (pos < path.size()) {
            auto next = path.find(';', pos);
            std::string dir = path.substr(pos, next - pos);
            pos = (next == std::string::npos) ? path.size() : next + 1;
            if (dir.empty()) continue;
            auto cand = (fs::path(dir) / "git.exe").string();
            if (fs::exists(cand)) return cand;
        }
    }
    // Fallbacks
    for (auto p : {"C:\\Program Files\\Git\\cmd\\git.exe",
                   "C:\\Program Files\\Git\\bin\\git.exe"}) {
        if (fs::exists(p)) return p;
    }
    return "";
}

inline bool IsRepo(const std::string& path) {
    if (!fs::exists(path)) return false;
    fs::path d(path);
    while (!d.empty()) {
        auto dot = d / ".git";
        if (fs::exists(dot)) return true;
        d = d.parent_path();
    }
    return false;
}

struct RepoSummary {
    std::string Name, Drive, Path, Branch, Upstream;
    int Ahead = 0, Behind = 0;
    std::string LastCommitDate, LastCommitHash, Subject, Remote;
    std::string Error;
    bool Ok = false;
};

inline std::string RunGit(const std::string& gitExe, const std::string& dir,
                           const std::string& args) {
    std::string cmd = "\"" + gitExe + "\" " + args;
    std::string tmpfile = (fs::temp_directory_path() / "git_out.tmp").string();

    // Use CreateProcess to capture output to a temp file
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hFile = CreateFileA(tmpfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hFile;
    si.hStdError = hFile;

    PROCESS_INFORMATION pi = {};
    std::string cmdline = "cmd /c " + cmd + " > \"" + tmpfile + "\" 2>&1";
    char cmdBuf[4096];
    strncpy_s(cmdBuf, cmdline.c_str(), sizeof(cmdBuf) - 1);

    BOOL ok = CreateProcessA(nullptr, cmdBuf, nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    CloseHandle(hFile);

    if (!ok) return "";

    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Read output
    std::string result;
    HANDLE hRead = CreateFileA(tmpfile.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hRead != INVALID_HANDLE_VALUE) {
        char buf[4096]; DWORD read;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
            buf[read] = 0;
            result += buf;
        }
        CloseHandle(hRead);
    }
    DeleteFileA(tmpfile.c_str());
    return result;
}

inline RepoSummary Summarize(const std::string& gitExe, const std::string& name,
                              const std::string& path) {
    RepoSummary s;
    s.Name = name;
    s.Path = path;
    try { s.Drive = fs::path(path).root_path().string(); s.Drive.pop_back(); } catch(...) {}

    if (!fs::exists(path)) { s.Error = "Path not found"; return s; }
    if (!IsRepo(path)) { s.Error = "Not a git repo"; return s; }

    auto rb = RunGit(gitExe, path, "rev-parse --abbrev-ref HEAD");
    if (rb.empty() || rb.find("fatal") == 0) { s.Error = "HEAD error"; return s; }
    s.Branch = rb;
    while (!s.Branch.empty() && (s.Branch.back() == '\n' || s.Branch.back() == '\r'))
        s.Branch.pop_back();

    auto ru = RunGit(gitExe, path, "rev-parse --abbrev-ref --symbolic-full-name @{u}");
    if (!ru.empty() && ru.find("fatal") != 0) {
        s.Upstream = ru;
        while (!s.Upstream.empty() && (s.Upstream.back() == '\n' || s.Upstream.back() == '\r'))
            s.Upstream.pop_back();
    }

    if (!s.Upstream.empty()) {
        auto rc = RunGit(gitExe, path, "rev-list --left-right --count HEAD..." + s.Upstream);
        if (!rc.empty()) {
            auto sp = rc.find('\t');
            if (sp != std::string::npos)
                s.Ahead = std::stoi(rc.substr(0, sp));
            auto sp2 = rc.find('\n', sp + 1);
            if (sp2 == std::string::npos) sp2 = rc.size();
            if (sp != std::string::npos)
                s.Behind = std::stoi(rc.substr(sp + 1, sp2 - sp - 1));
        }
    }

    auto rl = RunGit(gitExe, path, "log -1 --pretty=format:%H%x1F%h%x1F%cI%x1F%s HEAD");
    if (!rl.empty()) {
        size_t p1 = rl.find('\x1F');
        size_t p2 = rl.find('\x1F', p1 + 1);
        size_t p3 = rl.find('\x1F', p2 + 1);
        if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos) {
            s.LastCommitHash = rl.substr(p1 + 1, p2 - p1 - 1);
            s.LastCommitDate = rl.substr(p2 + 1, p3 - p2 - 1);
            s.Subject = rl.substr(p3 + 1);
            while (!s.Subject.empty() && (s.Subject.back() == '\n' || s.Subject.back() == '\r'))
                s.Subject.pop_back();
        }
    }

    auto rr = RunGit(gitExe, path, "config --get remote.origin.url");
    if (!rr.empty()) {
        s.Remote = rr;
        while (!s.Remote.empty() && (s.Remote.back() == '\n' || s.Remote.back() == '\r'))
            s.Remote.pop_back();
    }

    s.Ok = true;
    return s;
}

} }
