#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <Windows.h>

namespace ClientProj { namespace Svn {

struct SvnItem {
    std::string Drive;
    std::string BranchType;  // trunk, branches, tags, unknown
    std::string BranchName;
    std::string Revision;
    std::string RelativeUrl;
    std::string Url;
    std::string RepositoryRoot;
    std::string Path;
};

struct SvnRoot {
    std::string Path;        // local working copy root
    std::string Url;         // repository URL
    std::string RootUrl;     // repository root URL
};

// Find SVN working copies under a directory
std::vector<SvnRoot> FindSvnRoots(const std::string& dir);

// Get branch info for a working copy
SvnItem GetSvnInfo(const std::string& workingCopy);

// Get all SVN branches for a set of roots
std::vector<SvnItem> GetAllBranches(const std::vector<SvnRoot>& roots);

// Run svn command and return stdout
std::string RunSvn(const std::string& args, const std::string& workingCopy,
                   int timeoutMs = 8000);

// Find svn.exe on PATH
std::string FindSvnExe();

} }
