#include "Runtime/File/FileSystem.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherRead.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherWrite.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Core/Serialize/WriteData.h"

#if Z_PLATFORM_WINDOWS
    #include "Platform/Win/LocalFileSystemWindows.h"
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
#elif Z_PLATFORM_LINUX
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