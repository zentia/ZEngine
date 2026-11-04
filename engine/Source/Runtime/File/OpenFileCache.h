#pragma once

#include "File.h"
#include "Runtime/BaseClasses/Object.h"

#include <cstddef>
#include <filesystem>
#include <mutex>

// 一组按文件名缓存的已打开 File。
//
// 每个线程一份（见 AsyncReadManagerThreaded::GetThreadCache）：File::Open 会原地
// 销毁并重建 m_Entry/m_Accessor，两个线程共用一个 File 就会 double free。分线程
// 之后同一份 cache 只会被它所属的线程读写，锁只用于 ForceCloseFile 从别的线程
// 打进来时兜底。
class OpenFileCache
{
public:
    OpenFileCache() = default;
    ~OpenFileCache();

    OpenFileCache(const OpenFileCache&) = delete;
    OpenFileCache& operator=(const OpenFileCache&) = delete;

    // 打开（或命中已打开的）path，返回可读的 File；失败返回 nullptr。
    File* OpenCache(const std::filesystem::path& path);

    // 关掉缓存里所有 path 相同的槽位。
    void ForceClose(const std::filesystem::path& path);

    // 关掉全部槽位（析构 / 线程退出时用）。
    void CloseAll();

private:
    enum
    {
        kOpenedFileCacheCount = 10
    };

    int FindSlot(const std::filesystem::path& path) const;  // 命中返回下标，否则 -1
    int PickSlot();                                          // 空槽优先，否则轮转

    std::mutex m_Mutex;
    File m_Cache[kOpenedFileCacheCount];
    std::filesystem::path m_Filenames[kOpenedFileCacheCount];
    std::size_t m_NextSlot = 0;
};
