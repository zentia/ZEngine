#pragma once
// =============================================================================
// LocalFileSystemWeb
// -----------------------------------------------------------------------------
// Minimal stub LocalFileSystem implementation for the Emscripten / WebGL2 build.
//
// On the web there is no real OS file system available to a wasm module.
// Persistent storage in the browser is exposed through Emscripten's virtual
// FS (MEMFS / IDBFS / FETCHFS). For now we just provide a working but no-op
// implementation that satisfies all FileSystemHandler virtuals so the engine
// can boot. Real asset loading on Web should go through an HTTP/fetch-based
// path or Emscripten's preload-file mechanism instead of this handler.
// =============================================================================
#include "Runtime/VirtualFileSystem/LocalFileSystem.h"

class LocalFileSystemWeb : public LocalFileSystemHandler
{
public:
    bool Open(FileEntryData& data, FilePermission permissions, FileAutoBehavior behavior) override;
    bool Close(FileEntryData& data) override;
    bool Read(FileEntryData& data, FileSize from, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags) override;
    bool Read(FileEntryData& data, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags) override;
    bool Write(FileEntryData& data, FileSize at, uint64_t size, const void* buffer, uint64_t* actual) override;
    bool Write(FileEntryData& data, uint64_t size, const void* buffer, uint64_t* actual) override;
    FileSize Size(const FileEntryData& data) const override;
};
