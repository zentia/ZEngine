#include "Runtime/File/FileSystem.h"

#include "Runtime/AssetBundle/AssetBundleManager.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherRead.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherWrite.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Core/Serialize/WriteData.h"

#if Z_PLATFORM_WINDOWS
    #include "Platform/Win/LocalFIleSystemWindows.h"
    #define OS_FILE_SYSTEM_HANDLER LocalFileSystemWindows
#elif Z_PLATFORM_ANDROID
    #include "Platform/Android/LocalFileSystemAndroid.h"
    #define OS_FILE_SYSTEM_HANDLER LocalFileSystemAndroid
#elif Z_PLATFORM_OHOS
    #include "Platform/OHOS/LocalFileSystemOHOS.h"
    #define OS_FILE_SYSTEM_HANDLER LocalFileSystemOHOS
#elif Z_PLATFORM_IOS || Z_PLATFORM_MACOS
    #include "Platform/Android/LocalFileSystemAndroid.h"
    #define OS_FILE_SYSTEM_HANDLER LocalFileSystemAndroid
#elif Z_PLATFORM_LINUX && !defined(__EMSCRIPTEN__)
    // Emscripten 会继承 Z_PLATFORM_LINUX=1（顶层 CMake 的 UNIX 分支），但它有
    // 专属的 Web 文件系统实现（LocalFileSystemWeb，基于 Emscripten FS）。若不
    // 排除，这里会选中 LocalFileSystemLinux 导致 vtable 未定义（该 TU 未编入
    // Web 目标）。与 RegisterRuntime.cpp 的 Vulkan 排除同旨。
    #include "Platform/Linux/LocalFileSystemLinux.h"
    #define OS_FILE_SYSTEM_HANDLER LocalFileSystemLinux
#elif defined(__EMSCRIPTEN__)
    #include "Platform/Web/LocalFileSystemWeb.h"
    #define OS_FILE_SYSTEM_HANDLER LocalFileSystemWeb
#endif

FileSystem::FileSystem()
{
    m_LocalFileSystem = MemoryManager::CreateObject<OS_FILE_SYSTEM_HANDLER>();
    m_DefaultFileSystem = m_LocalFileSystem;
}

bool FileSystem::ReadAssetText(const std::filesystem::path& asset_path, std::string& out_text)
{
    // PR-PAK1 follow-up (defect #7): the JSON/YAML readers in FileSystem.h are
    // the last asset readers that used to go straight to fopen(). In an
    // installed player the engine-global configs (particle/rendering) and the
    // component-referenced *.material.json / *.skeleton.json only exist inside
    // a .zbundle, so they were reported as "open file ... failed" -- while a
    // dev machine silently read the source tree instead. Ask the mounted-bundle
    // registry first, exactly like SerializedFile::InitializeRead() and
    // AssetManager::ReadObjectsFromYaml(). In the editor no bundle is mounted,
    // so this is a cheap miss and the disk path below stays the normal case.
    if (auto* bundles = GET_SYSTEM(AssetBundleManager); bundles != nullptr)
    {
        std::vector<uint8_t> bytes;
        if (bundles->TryReadAssetBytes(asset_path, bytes))
        {
            out_text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            LOG_INFO(ZPak, "FileSystem: '{}' served from a mounted bundle ({} bytes)",
                     asset_path.generic_string(), bytes.size());
            return true;
        }
    }

    FILE* f = fopen(asset_path.string().c_str(), "rb");
    if (!f)
    {
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out_text.clear();
    if (size > 0)
    {
        out_text.resize(static_cast<size_t>(size));
        const size_t read_bytes = fread(out_text.data(), 1, static_cast<size_t>(size), f);
        out_text.resize(read_bytes);
    }
    fclose(f);
    return true;
}

std::vector<std::filesystem::path> FileSystem::GetFiles(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> files;
    for (auto const& directory_entry : std::filesystem::recursive_directory_iterator {directory})
    {
        if (directory_entry.is_regular_file())
        {
            files.push_back(directory_entry);
        }
    }
    return files;
}

FileSystemHandler* FileSystem::GetHandlerForPath(std::filesystem::path& path) const
{
    return m_DefaultFileSystem;
}

bool FileSystem::ReadStringFromFile(std::string* outData, std::filesystem::path& path)
{
    FileSystemEntry e(path);

    FileAccessor reader;
    if (!reader.Open(e, kReadPermission))
        return false;

    const size_t fileSize = reader.Size().Cast<size_t>();

    outData->resize(fileSize);

    bool success = true;
    uint64_t actual = 0;
    if (fileSize > 0)
        success = reader.Read(fileSize, outData->data(), &actual);

    reader.Close();

    if (!success || actual != fileSize)
    {
        outData->clear();
        return false;
    }

    return true;
}