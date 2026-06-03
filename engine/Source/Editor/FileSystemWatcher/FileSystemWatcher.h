#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_set>

class FileSystemWatcher
{
public:
    FileSystemWatcher() = default;
    ~FileSystemWatcher() { StopWatching(); }

    void WatchDirectory(const std::filesystem::path& directory);

    void StopWatching();

    void SetOnFileChanged(std::function<void(const std::filesystem::path&)> callback);
    void SetOnFileCreated(std::function<void(const std::filesystem::path&)> callback);
    void SetOnFileDeleted(std::function<void(const std::filesystem::path&)> callback);

    /// Override the set of file extensions the watcher emits events for.
    /// Each entry must include the leading dot (e.g. ".zasset", ".js").
    /// If never called, the watcher defaults to {".zasset"} - the original
    /// asset-pipeline behaviour. Empty set = accept all extensions.
    /// Call this BEFORE WatchDirectory(); changes after that are ignored.
    void SetExtensionFilter(std::unordered_set<std::string> extensions);

    /// Internal: returns true if the given path passes the current filter.
    /// Public so the platform-specific code can test it; not a stable API.
    bool ExtensionAllowed(const std::filesystem::path& path) const;

    void Update();

    bool isWatching() const { return m_IsWatching; }
    const std::filesystem::path& getWatchedDirectory() const { return m_WatchedDirectory; }

    // Internal methods for platform-specific callbacks
    void EnqueueFileCreated(const std::filesystem::path& path);
    void EnqueueFileChanged(const std::filesystem::path& path);
    void EnqueueFileDeleted(const std::filesystem::path& path);

private:
#ifdef _WIN32
    void* m_Handle = nullptr;  // Windows: HANDLE
#elif defined(__linux__)
    int m_InotifyFd = -1;
#elif defined(__APPLE__)
    void* m_Stream = nullptr;  // macOS: FSEventStreamRef
#endif

    std::filesystem::path m_WatchedDirectory;
    bool m_IsWatching = false;

    // Extension filter. Default {".zasset"} preserves legacy behaviour for
    // call sites that don't set it. An empty set means "accept all".
    std::unordered_set<std::string> m_ExtFilter {".zasset"};

    std::function<void(const std::filesystem::path&)> m_OnFileChanged;
    std::function<void(const std::filesystem::path&)> m_OnFileCreated;
    std::function<void(const std::filesystem::path&)> m_OnFileDeleted;

    std::queue<std::filesystem::path> m_FileChangedQueue;
    std::queue<std::filesystem::path> m_FileCreatedQueue;
    std::queue<std::filesystem::path> m_FileDeletedQueue;
    std::mutex m_QueueMutex;
};