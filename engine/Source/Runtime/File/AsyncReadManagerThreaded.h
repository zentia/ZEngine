#pragma once

#include <filesystem>
#include <mutex>
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
    void Shutdown() override {}

private:
    OpenFileCache m_SyncOpenFilesCache;
    // SyncRequest 会被多个 worker 线程（以及主线程）同时调用，而 OpenFileCache 只有
    // 一个 File 槽位，File::Open 内部又会先销毁旧的 m_Entry/m_Accessor。没有这把锁，
    // 两个线程会同时改同一个 File，触发 double free（表现为 ~filesystem::path 崩）。
    std::mutex m_FileCacheMutex;
};