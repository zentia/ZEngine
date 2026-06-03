#pragma once
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
};