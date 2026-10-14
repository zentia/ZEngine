#include "SvnProbe.h"
#include <cstdio>
#include <regex>
#include <stack>
#include <utility>

namespace ClientProj { namespace Svn {

std::string FindSvnExe() {
    // Try common paths
    std::vector<std::string> candidates = {"svn.exe"};
    char pathEnv[32768]; size_t len;
    if (getenv_s(&len, pathEnv, sizeof(pathEnv), "PATH") == 0) {
        std::string path(pathEnv);
        size_t pos = 0;
        while (pos < path.size()) {
            auto end = path.find(';', pos);
            if (end == std::string::npos) end = path.size();
            std::string dir = path.substr(pos, end - pos);
            std::string candidate = dir + "\\svn.exe";
            if (std::filesystem::exists(candidate)) return candidate;
            pos = end + 1;
        }
    }
    return "svn.exe";  // fallback to PATH lookup
}

std::string RunSvn(const std::string& args, const std::string& workingCopy, int timeoutMs) {
    std::string svnExe = FindSvnExe();
    std::string cmd = "\"" + svnExe + "\" " + args + " \"" + workingCopy + "\"";
    
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};

    std::string result;
    if (CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        WaitForSingleObject(pi.hProcess, timeoutMs);
        
        char buf[4096]; DWORD read;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
            buf[read] = 0;
            result += buf;
        }
        
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(hWrite);
    }
    CloseHandle(hRead);

    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

std::vector<SvnRoot> FindSvnRoots(const std::string& dir) {
    std::vector<SvnRoot> roots;
    if (!std::filesystem::exists(dir)) return roots;

    // Helper: returns true if `path` is itself an SVN working copy (contains a .svn dir)
    auto isWcRoot = [](const std::filesystem::path& path) -> bool {
        std::error_code ec;
        auto svnDir = path / ".svn";
        return std::filesystem::is_directory(svnDir, ec);
    };

    // 1) If `dir` itself is a WC root, take it directly.
    if (isWcRoot(dir)) {
        SvnRoot root;
        root.Path = dir;
        root.Url = RunSvn("info --show-item url", root.Path);
        root.RootUrl = RunSvn("info --show-item repos-root-url", root.Path);
        roots.push_back(root);
        return roots;
    }

    // 2) Otherwise, manually walk sub-directories but DO NOT descend into any
    //    .svn directory (which contains tens of thousands of pristine files
    //    that blow up the scan time and never contain a real WC). Also cap
    //    recursion depth to 4 levels so large trees finish fast.
    constexpr int kMaxDepth = 4;
    struct WalkState { std::filesystem::path p; int depth; };
    std::stack<WalkState> stack;
    stack.push({std::filesystem::path(dir), 0});

    while (!stack.empty()) {
        auto [p, depth] = stack.top();
        stack.pop();

        std::error_code ec;
        std::filesystem::directory_iterator it(p, ec);
        if (ec) continue;
        for (const auto& entry : it) {
            std::error_code ec2;
            if (!entry.is_directory(ec2)) continue;
            const auto& sub = entry.path();
            const std::string name = sub.filename().string();

            // Skip .svn entirely (and other VCS / build / cache folders)
            if (name == ".svn" || name == ".git" || name == "node_modules" ||
                name == "Library" || name == "Temp" || name == "Build" ||
                name == "obj" || name == "bin")
                continue;

            if (isWcRoot(sub)) {
                SvnRoot root;
                root.Path = sub.string();
                root.Url = RunSvn("info --show-item url", root.Path);
                root.RootUrl = RunSvn("info --show-item repos-root-url", root.Path);
                roots.push_back(root);
                // Don't recurse further into a WC's own sub-dirs
                continue;
            }

            if (depth < kMaxDepth) {
                stack.push({sub, depth + 1});
            }
        }
    }
    return roots;
}

SvnItem GetSvnInfo(const std::string& workingCopy) {
    SvnItem item;
    item.Path = workingCopy;
    item.Drive = workingCopy.substr(0, 1);

    item.Url = RunSvn("info --show-item url", workingCopy);
    item.RelativeUrl = RunSvn("info --show-item relative-url", workingCopy);
    item.RepositoryRoot = RunSvn("info --show-item repos-root-url", workingCopy);
    item.Revision = RunSvn("info --show-item revision", workingCopy);

    // Classify branch type from relative URL
    std::string rel = item.RelativeUrl;
    std::regex branchRe(R"(/branches/([^/]+))", std::regex::icase);
    std::regex tagRe(R"(/tags/([^/]+))", std::regex::icase);
    std::regex trunkRe(R"(/trunk)", std::regex::icase);
    std::smatch m;

    if (std::regex_search(rel, m, branchRe)) {
        item.BranchType = "branches";
        item.BranchName = m[1].str();
    } else if (std::regex_search(rel, m, tagRe)) {
        item.BranchType = "tags";
        item.BranchName = m[1].str();
    } else if (std::regex_search(rel, trunkRe)) {
        item.BranchType = "trunk";
        item.BranchName = "trunk";
    } else {
        item.BranchType = "unknown";
        item.BranchName = rel;
    }

    return item;
}

std::vector<SvnItem> GetAllBranches(const std::vector<SvnRoot>& roots) {
    std::vector<SvnItem> items;
    for (const auto& root : roots) {
        items.push_back(GetSvnInfo(root.Path));
    }
    return items;
}

} }
