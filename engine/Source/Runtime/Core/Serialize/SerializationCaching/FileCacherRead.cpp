#include "FileCacherRead.h"

#include "Runtime/File/AsyncReadManagerThreaded.h"

FileCacherRead::FileCacherRead(const std::filesystem::path& path, size_t cacheSize)
    : m_CacheSize(cacheSize), m_Path(path)
{
    InitializeReadCommands([&cacheSize, this](CacheBlock& block) {
        AllocateBlock(block, this->m_CacheSize);
    });
}

FileCacherRead::~FileCacherRead()
{
    for (int i = 0; i < kCacheCount; i++)
    {
        DeallocateBlock(m_ActiveBlocks[i]);
    }

    GET_SYSTEM(AsyncReadManagerThreaded)->ForceCloseFile(m_Path);
}

void FileCacherRead::LockCacheBlock(size_t block, uint8_t** startPos, uint8_t** endPos)
{
    int indexToLoad = RequestBlock(block);

    m_ActiveBlocks[indexToLoad].locked = 1;
    *startPos = m_ActiveBlocks[indexToLoad].data;
    *endPos = m_ActiveBlocks[indexToLoad].data + m_ReadCommands[indexToLoad].size;
}

void FileCacherRead::UnlockCacheBlock(size_t block)
{
    for (int i = 0; i < kCacheCount; i++)
    {
        if (static_cast<size_t>(m_ActiveBlocks[i].block) == block && m_ActiveBlocks[i].locked == 1)
        {
            m_ActiveBlocks[i].locked--;
            return;
        }
    }
}

void FileCacherRead::DirectRead(void* data, size_t position, size_t size)
{
    m_DirectReadCommands.path = m_Path;
    m_DirectReadCommands.buffer = static_cast<uint8_t*>(data);
    m_DirectReadCommands.size = size;
    m_DirectReadCommands.offset = FileSize(static_cast<uint64_t>(position));

    GET_SYSTEM(AsyncReadManagerThreaded)->SyncRequest(&m_DirectReadCommands);
}

int FileCacherRead::RequestBlock(int block)
{
    int indexToLoad = -1;
    for (int i = 0; i < kCacheCount; i++)
    {
        if (m_ReadCommands[i].status != AsyncReadCommand::kInProgress)
        {
            indexToLoad = i;
            break;
        }
    }

    if (indexToLoad == -1)
        indexToLoad = 0;

    Request(block, indexToLoad, m_ActiveBlocks[indexToLoad], true);
    return indexToLoad;
}

void FileCacherRead::AllocateBlock(CacheBlock& block, size_t sizeToUse)
{
    block.data = static_cast<uint8_t*>(MemoryManager::Malloc(sizeToUse));
}

void FileCacherRead::DeallocateBlock(CacheBlock& block)
{
    MemoryManager::Free(block.data);
    block.data = nullptr;
}

bool FileCacherRead::Request(int block, int readCmdIndex, CacheBlock& cacheBlock, bool sync)
{
    size_t startBlockOffset = block * GetCacheSize();
    if (startBlockOffset >= m_FileSize)
        return false;

    size_t size = std::min<size_t>(m_FileSize - startBlockOffset, GetCacheSize());
    AsyncReadCommand& readCommand = m_ReadCommands[readCmdIndex];
    readCommand.path = m_Path;
    readCommand.buffer = cacheBlock.data;
    readCommand.size = size;
    readCommand.offset = FileSize(static_cast<uint64_t>(block * m_CacheSize));
    GET_SYSTEM(AsyncReadManagerThreaded)->SyncRequest(&readCommand);
    return true;
}

void FileCacherRead::InitializeReadCommands(const std::function<void(CacheBlock& block)>& allocateBlock)
{
    FileSystemEntry fsEntry(m_Path);
    m_FileSize = fsEntry.Size().Cast<size_t>();

    for (int i = 0; i < kCacheCount; i++)
    {
        allocateBlock(m_ActiveBlocks[i]);
        m_ActiveBlocks[i].block = -1;
    }
}