#include "FileSystemWatcher.h"

#ifdef _WIN32
    #include <fileapi.h>
    #include <ioapiset.h>
    #include <windows.h>
#elif defined(__linux__)
    #include <limits.h>
    #include <sys/inotify.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #include <CoreServices/CoreServices.h>
    #include <sys/stat.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <thread>

namespace
{

#ifdef _WIN32
    // Windows implementation helper
    struct WindowsWatchData
    {
        HANDLE directory_handle = INVALID_HANDLE_VALUE;
        HANDLE thread_handle = nullptr;
        HANDLE cancel_event = nullptr;
        std::atomic<bool> should_stop {false};
        FileSystemWatcher* watcher = nullptr;
    };

    DWORD WINAPI WatchThreadProc(LPVOID lpParam)
    {
        WindowsWatchData* data = static_cast<WindowsWatchData*>(lpParam);
        if (!data || !data->watcher)
            return 1;

        constexpr DWORD buffer_size = 4096;
        char buffer[buffer_size];
        DWORD bytes_returned;
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent)
            return 1;

        while (!data->should_stop.load())
        {
            // Reset event for next read
            ResetEvent(overlapped.hEvent);

            // Read directory changes asynchronously
            BOOL result = ReadDirectoryChangesW(data->directory_handle,
                                                buffer,
                                                buffer_size,
                                                TRUE,  // watch subdirectories
                                                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                                &bytes_returned,
                                                &overlapped,
                                                nullptr);

            if (!result)
            {
                DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING)
                {
                    // Error occurred
                    if (data->should_stop.load())
                        break;
                    Sleep(10);
                    continue;
                }
            }

            // Wait for either the I/O to complete or cancellation
            HANDLE wait_handles[2] = {overlapped.hEvent, data->cancel_event};
            DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);

            if (wait_result == WAIT_OBJECT_0)
            {
                // I/O completed
                if (GetOverlappedResult(data->directory_handle, &overlapped, &bytes_returned, FALSE))
                {
                    if (bytes_returned > 0)
                    {
                        FILE_NOTIFY_INFORMATION* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                        do
                        {
                            std::wstring wfilename(info->FileName, info->FileNameLength / sizeof(WCHAR));
                            std::filesystem::path filepath = data->watcher->getWatchedDirectory() / wfilename;

                            if (data->watcher->ExtensionAllowed(filepath))
                            {
                                if (info->Action == FILE_ACTION_ADDED)
                                {
                                    data->watcher->EnqueueFileCreated(filepath);
                                }
                                else if (info->Action == FILE_ACTION_REMOVED)
                                {
                                    data->watcher->EnqueueFileDeleted(filepath);
                                }
                                else if (info->Action == FILE_ACTION_MODIFIED ||
                                         info->Action == FILE_ACTION_RENAMED_NEW_NAME)
                                {
                                    data->watcher->EnqueueFileChanged(filepath);
                                }
                            }

                            if (info->NextEntryOffset == 0)
                                break;
                            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(info) +
                                                                              info->NextEntryOffset);
                        } while (true);
                    }
                }
            }
            else if (wait_result == WAIT_OBJECT_0 + 1 || data->should_stop.load())
            {
                // Cancellation requested
                CancelIoEx(data->directory_handle, &overlapped);
                break;
            }
        }

        CloseHandle(overlapped.hEvent);
        return 0;
    }
#elif defined(__APPLE__)
    // macOS FSEvents callback
    void FSEventsCallback(ConstFSEventStreamRef streamRef,
                          void* clientCallBackInfo,
                          size_t numEvents,
                          void* eventPaths,
                          const FSEventStreamEventFlags eventFlags[],
                          const FSEventStreamEventId eventIds[])
    {
        FileSystemWatcher* watcher = static_cast<FileSystemWatcher*>(clientCallBackInfo);
        if (!watcher)
            return;

        char** paths = static_cast<char**>(eventPaths);
        for (size_t i = 0; i < numEvents; ++i)
        {
            std::filesystem::path filepath(paths[i]);

            if (!watcher->ExtensionAllowed(filepath))
                continue;

            if (eventFlags[i] & kFSEventStreamEventFlagItemCreated)
            {
                watcher->EnqueueFileCreated(filepath);
            }
            else if (eventFlags[i] & kFSEventStreamEventFlagItemRemoved)
            {
                watcher->EnqueueFileDeleted(filepath);
            }
            else if (eventFlags[i] & (kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemRenamed))
            {
                watcher->EnqueueFileChanged(filepath);
            }
        }
    }
#endif
}  // anonymous namespace

void FileSystemWatcher::WatchDirectory(const std::filesystem::path& directory)
{
    if (directory.empty())
        return;
    // Stop existing watch if any
    StopWatching();

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory))
    {
        std::filesystem::create_directory(directory);
    }

    m_WatchedDirectory = std::filesystem::canonical(directory);

#ifdef _WIN32
    // Windows implementation using ReadDirectoryChangesW
    std::wstring dir_path = m_WatchedDirectory.wstring();
    HANDLE dir_handle = CreateFileW(dir_path.c_str(),
                                    FILE_LIST_DIRECTORY,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);

    if (dir_handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    m_Handle = new WindowsWatchData();
    WindowsWatchData* watch_data = static_cast<WindowsWatchData*>(m_Handle);
    watch_data->directory_handle = dir_handle;
    watch_data->watcher = this;
    watch_data->should_stop = false;

    // Create cancellation event
    watch_data->cancel_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (watch_data->cancel_event == nullptr)
    {
        CloseHandle(dir_handle);
        delete watch_data;
        m_Handle = nullptr;
        return;
    }

    // Create thread to watch directory changes
    watch_data->thread_handle = CreateThread(nullptr, 0, WatchThreadProc, watch_data, 0, nullptr);
    if (watch_data->thread_handle == nullptr)
    {
        CloseHandle(watch_data->cancel_event);
        CloseHandle(dir_handle);
        delete watch_data;
        m_Handle = nullptr;
        return;
    }
    m_IsWatching = true;

#elif defined(__linux__)
    // Linux implementation using inotify
    m_InotifyFd = inotify_init1(IN_NONBLOCK);
    if (m_InotifyFd < 0)
    {
        return;
    }

    std::string dir_path = m_WatchedDirectory.string();
    int watch_descriptor = inotify_add_watch(
        m_InotifyFd, dir_path.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO);

    if (watch_descriptor < 0)
    {
        close(m_InotifyFd);
        m_InotifyFd = -1;
        return;
    }

    m_IsWatching = true;

#elif defined(__APPLE__)
    // macOS implementation using FSEvents
    CFStringRef path = CFStringCreateWithCString(nullptr, m_WatchedDirectory.string().c_str(), kCFStringEncodingUTF8);

    if (!path)
    {
        return;
    }

    CFArrayRef paths_to_watch = CFArrayCreate(nullptr, (const void**)&path, 1, nullptr);
    FSEventStreamContext context = {0, this, nullptr, nullptr, nullptr};

    FSEventStreamRef stream = FSEventStreamCreate(nullptr,
                                                  &FSEventsCallback,
                                                  &context,
                                                  paths_to_watch,
                                                  kFSEventStreamEventIdSinceNow,
                                                  0.1,  // latency in seconds
                                                  kFSEventStreamCreateFlagFileEvents);

    if (!stream)
    {
        CFRelease(paths_to_watch);
        CFRelease(path);
        return;
    }

    FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    FSEventStreamStart(stream);

    m_Stream = stream;
    m_IsWatching = true;

    CFRelease(paths_to_watch);
    CFRelease(path);
#endif
}

void FileSystemWatcher::StopWatching()
{
    if (!m_IsWatching)
        return;

#ifdef _WIN32
    if (m_Handle)
    {
        WindowsWatchData* watch_data = static_cast<WindowsWatchData*>(m_Handle);
        watch_data->should_stop = true;

        // Signal cancellation event to unblock ReadDirectoryChangesW
        if (watch_data->cancel_event != nullptr)
        {
            SetEvent(watch_data->cancel_event);
        }

        // Cancel any pending I/O operations
        if (watch_data->directory_handle != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(watch_data->directory_handle, nullptr);
        }

        // Wait for thread to exit (with timeout)
        if (watch_data->thread_handle != nullptr)
        {
            WaitForSingleObject(watch_data->thread_handle, 1000);  // 1 second timeout
            CloseHandle(watch_data->thread_handle);
            watch_data->thread_handle = nullptr;
        }

        // Close handles
        if (watch_data->cancel_event != nullptr)
        {
            CloseHandle(watch_data->cancel_event);
            watch_data->cancel_event = nullptr;
        }

        if (watch_data->directory_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(watch_data->directory_handle);
            watch_data->directory_handle = INVALID_HANDLE_VALUE;
        }

        delete watch_data;
        m_Handle = nullptr;
    }

#elif defined(__linux__)
    if (m_InotifyFd >= 0)
    {
        close(m_InotifyFd);
        m_InotifyFd = -1;
    }

#elif defined(__APPLE__)
    if (m_Stream)
    {
        FSEventStreamStop(static_cast<FSEventStreamRef>(m_Stream));
        FSEventStreamInvalidate(static_cast<FSEventStreamRef>(m_Stream));
        FSEventStreamRelease(static_cast<FSEventStreamRef>(m_Stream));
        m_Stream = nullptr;
    }
#endif

    m_IsWatching = false;
}

void FileSystemWatcher::SetOnFileChanged(std::function<void(const std::filesystem::path&)> callback)
{
    m_OnFileChanged = std::move(callback);
}

void FileSystemWatcher::SetOnFileCreated(std::function<void(const std::filesystem::path&)> callback)
{
    m_OnFileCreated = std::move(callback);
}

void FileSystemWatcher::SetOnFileDeleted(std::function<void(const std::filesystem::path&)> callback)
{
    m_OnFileDeleted = std::move(callback);
}

void FileSystemWatcher::Update()
{
    if (!m_IsWatching)
        return;

#ifdef _WIN32
    // Windows events are handled in the background thread
    // Just process the queue here

#elif defined(__linux__)
    // Linux: read inotify events
    constexpr size_t buffer_size = (sizeof(inotify_event) + NAME_MAX + 1) * 1024;
    char buffer[buffer_size];

    while (true)
    {
        ssize_t length = read(m_InotifyFd, buffer, buffer_size);
        if (length <= 0)
            break;

        size_t i = 0;
        while (i < static_cast<size_t>(length))
        {
            inotify_event* event = reinterpret_cast<inotify_event*>(&buffer[i]);

            if (event->len > 0)
            {
                std::filesystem::path filepath = getWatchedDirectory() / event->name;

                if (this->ExtensionAllowed(filepath))
                {
                    if (event->mask & (IN_CREATE | IN_MOVED_TO))
                    {
                        EnqueueFileCreated(filepath);
                    }
                    else if (event->mask & (IN_DELETE | IN_MOVED_FROM))
                    {
                        EnqueueFileDeleted(filepath);
                    }
                    else if (event->mask & IN_MODIFY)
                    {
                        EnqueueFileChanged(filepath);
                    }
                }
            }

            i += sizeof(inotify_event) + event->len;
        }
    }

#elif defined(__APPLE__)
    // macOS: process run loop (events are handled in callback)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif

    // Process all queued events
    std::lock_guard<std::mutex> lock(m_QueueMutex);

    while (!m_FileCreatedQueue.empty())
    {
        auto path = m_FileCreatedQueue.front();
        m_FileCreatedQueue.pop();
        if (m_OnFileCreated)
        {
            m_OnFileCreated(path);
        }
    }

    while (!m_FileChangedQueue.empty())
    {
        auto path = m_FileChangedQueue.front();
        m_FileChangedQueue.pop();
        if (m_OnFileChanged)
        {
            m_OnFileChanged(path);
        }
    }

    while (!m_FileDeletedQueue.empty())
    {
        auto path = m_FileDeletedQueue.front();
        m_FileDeletedQueue.pop();
        if (m_OnFileDeleted)
        {
            m_OnFileDeleted(path);
        }
    }
}

void FileSystemWatcher::EnqueueFileCreated(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    m_FileCreatedQueue.push(path);
}

void FileSystemWatcher::EnqueueFileChanged(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    m_FileChangedQueue.push(path);
}

void FileSystemWatcher::EnqueueFileDeleted(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    m_FileDeletedQueue.push(path);
}

void FileSystemWatcher::SetExtensionFilter(std::unordered_set<std::string> extensions)
{
    // Normalise to lowercase so callers can pass ".JS" / ".js" interchangeably,
    // matching how std::filesystem::path::extension() returns the case from
    // the filesystem (which on Windows is case-preserving but case-insensitive
    // for matching).
    std::unordered_set<std::string> lowered;
    lowered.reserve(extensions.size());
    for (auto& e : extensions)
    {
        std::string s = e;
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        lowered.insert(std::move(s));
    }
    m_ExtFilter = std::move(lowered);
}

bool FileSystemWatcher::ExtensionAllowed(const std::filesystem::path& path) const
{
    // Empty filter set = wildcard.
    if (m_ExtFilter.empty())
        return true;

    std::string ext = path.extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return m_ExtFilter.count(ext) > 0;
}