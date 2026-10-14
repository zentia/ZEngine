#pragma once
#include <string>
#include <filesystem>

namespace ClientProj { namespace Paths {

inline std::string ConfigDir() {
    auto dir = std::filesystem::path(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : ".")
            / ".client_proj_branches";
    std::filesystem::create_directories(dir);
    return dir.string();
}

inline std::string BranchPaths()    { return (std::filesystem::path(ConfigDir()) / "paths.json").string(); }
inline std::string GitRepos()       { return (std::filesystem::path(ConfigDir()) / "git_repos.json").string(); }
inline std::string GitTags()        { return (std::filesystem::path(ConfigDir()) / "git_tags.json").string(); }
inline std::string Urls()           { return (std::filesystem::path(ConfigDir()) / "urls.json").string(); }
inline std::string Notes()          { return (std::filesystem::path(ConfigDir()) / "notes.json").string(); }
inline std::string NotesArchive()   { return (std::filesystem::path(ConfigDir()) / "notes_archive.json").string(); }
inline std::string NotesArchiveCopyTemplate() { return (std::filesystem::path(ConfigDir()) / "notes_archive_copy_template.txt").string(); }
inline std::string Tips()           { return (std::filesystem::path(ConfigDir()) / "tips.json").string(); }
inline std::string GridLayout()     { return (std::filesystem::path(ConfigDir()) / "grid_layout.json").string(); }
inline std::string IdePreference()  { return (std::filesystem::path(ConfigDir()) / "ide_preference.json").string(); }

} }
