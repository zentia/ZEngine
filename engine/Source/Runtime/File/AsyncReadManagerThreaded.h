#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "AsyncReadManager.h"
#include "OpenFileCache.h"
#include "Runtime/Core/Base/EngineSystem.h"

class AsyncReadManagerThreaded : public IEngineSystem
{
public:
    void SyncRequest(AsyncReadCommand* request);
    void ForceCloseFile(const std::filesystem::path& path);

protected:
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    bool Initialize() override { return true; }
    void Shutdown() override;

private:
    // 当前线程的缓存，按 thread id 懒创建。
    OpenFileCache& GetThreadCache();

    // 每线程一份 OpenFileCache。
    //
    // File::Open 进入 OpenFileSystemEntry 时会先销毁旧的 m_Entry/m_Accessor 再原地
    // 重建，所以两个线程共用一个 File 必然 double free（堆被踩坏后要等到某个不相干
    // 的析构才崩，栈里只剩 ~filesystem::path）。之前用一把大锁把所有同步读串行化
    // 只是把问题挡住了；改成每线程一份之后，I/O 重新并行，锁只保护下面这张表本身。
    //
    // 每个 cache 自带 10 个槽位 + 文件名命中判断，所以同一个文件在同一线程内只打开
    // 一次；不同线程各自持有自己的句柄（文件描述符数量 = 线程数 x 打开的文件数）。
    std::mutex m_CacheTableMutex;
    std::unordered_map<std::thread::id, std::unique_ptr<OpenFileCache>> m_ThreadCaches;
};
