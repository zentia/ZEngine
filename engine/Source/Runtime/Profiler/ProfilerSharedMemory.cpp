#include "ProfilerSharedMemory.h"

#include "Runtime/Core/Base/Platform.h"

#include <algorithm>
#include <cstring>

#ifdef Z_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

ProfilerSharedMemory::ProfilerSharedMemory()
    : m_SharedData(nullptr), m_Initialized(false), m_IsWriter(false)
#ifdef Z_PLATFORM_WINDOWS
      ,
      m_FileMappingHandle(nullptr)
#else
      ,
      m_ShmFd(-1), m_SharedMemoryPtr(nullptr)
#endif
{
}

ProfilerSharedMemory::~ProfilerSharedMemory()
{
    Shutdown();
}

bool ProfilerSharedMemory::InitializeAsWriter()
{
    if (m_Initialized)
    {
        return true;
    }

    m_IsWriter = true;
    return CreateSharedMemory();
}

bool ProfilerSharedMemory::InitializeAsReader()
{
    if (m_Initialized)
    {
        return true;
    }

    m_IsWriter = false;
    return OpenSharedMemory();
}

void ProfilerSharedMemory::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    CloseSharedMemory();
    m_Initialized = false;
    m_IsWriter = false;
}

void ProfilerSharedMemory::WriteData(const std::vector<ProfileSample>& results,
                                     float frame_time_ms,
                                     std::uint32_t frame_number)
{
    if (!m_Initialized || !m_IsWriter || !m_SharedData)
    {
        return;
    }

    // Get current write index and calculate next frame slot (ring buffer)
    std::uint32_t current_write_index = m_SharedData->write_index;
    std::uint32_t frame_slot = current_write_index % ProfilerSharedData::MAX_FRAMES;

    // Get reference to the frame data slot
    auto& frame = m_SharedData->frames[frame_slot];

    // Limit to MAX_SAMPLES
    std::size_t count = std::min(results.size(), static_cast<std::size_t>(ProfilerSharedData::FrameData::MAX_SAMPLES));

    // Write frame time and frame number
    frame.frame_time_ms = frame_time_ms;
    frame.sample_count = static_cast<std::uint32_t>(count);
    frame.frame_number = frame_number;

    // Write samples
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& src = results[i];
        auto& dst = frame.samples[i];

        // Copy name (with null termination safety)
        std::strncpy(dst.name, src.name.c_str(), sizeof(dst.name) - 1);
        dst.name[sizeof(dst.name) - 1] = '\0';

        dst.duration_ms = src.duration_ms;
        dst.call_count = src.call_count;
    }

    // Clear remaining samples
    for (std::size_t i = count; i < ProfilerSharedData::FrameData::MAX_SAMPLES; ++i)
    {
        frame.samples[i].name[0] = '\0';
        frame.samples[i].duration_ms = 0.0f;
        frame.samples[i].call_count = 0;
    }

    // Increment write index to signal new data (this will wrap around due to ring buffer)
    // Use memory barrier to ensure frame data is written before incrementing index
#ifdef Z_PLATFORM_WINDOWS
    MemoryBarrier();  // Ensure all writes to frame data are visible before updating index
    m_SharedData->write_index = current_write_index + 1;
    MemoryBarrier();  // Ensure write_index update is visible
#else
    __sync_synchronize();  // Memory barrier to ensure all writes are visible
    m_SharedData->write_index = current_write_index + 1;
    __sync_synchronize();  // Memory barrier after writing
#endif
}

bool ProfilerSharedMemory::ReadData(std::vector<ProfileSample>& results,
                                    float& frame_time_ms,
                                    std::uint32_t& frame_number)
{
    if (!m_Initialized || m_IsWriter || !m_SharedData)
    {
        return false;
    }

    // Safely read write_index first to check for new data
    // Use memory barrier to ensure we get the latest value (for cross-process synchronization)
#ifdef Z_PLATFORM_WINDOWS
    MemoryBarrier();  // Memory barrier before reading
    volatile std::uint32_t current_write_index = m_SharedData->write_index;
    volatile std::uint32_t current_read_index = m_SharedData->read_index;
    MemoryBarrier();  // Memory barrier after reading
#else
    __sync_synchronize();  // Memory barrier before reading
    volatile std::uint32_t current_write_index = m_SharedData->write_index;
    volatile std::uint32_t current_read_index = m_SharedData->read_index;
    __sync_synchronize();  // Memory barrier after reading
#endif

    // Check if new data is available
    // Since we're using a ring buffer, we need to check if there are unread frames
    if (current_write_index == current_read_index)
    {
        return false;  // No new data
    }

    // Calculate how many frames are available to read
    // write_index and read_index are monotonically increasing (not modulo)
    // The actual frame slot is calculated using modulo
    std::uint32_t frames_available = current_write_index - current_read_index;

    // Limit to MAX_FRAMES (ring buffer can only hold MAX_FRAMES)
    // If writer has written more than MAX_FRAMES since last read, some frames are lost
    // With increased buffer size (60 frames), this should happen less frequently
    if (frames_available > ProfilerSharedData::MAX_FRAMES)
    {
        // Too many frames skipped - update read_index to catch up
        // This prevents the buffer from being permanently behind
        current_read_index = current_write_index - ProfilerSharedData::MAX_FRAMES;
    }

    // Read the latest frame (most recent one)
    // The latest frame is at (write_index - 1) % MAX_FRAMES
    std::uint32_t latest_frame_index = (current_write_index - 1) % ProfilerSharedData::MAX_FRAMES;
    const auto& frame = m_SharedData->frames[latest_frame_index];

    // Read frame time and frame number
    frame_time_ms = frame.frame_time_ms;
    frame_number = frame.frame_number;

    // Read sample count (validate to prevent buffer overflow)
    std::uint32_t sample_count = frame.sample_count;
    if (sample_count > ProfilerSharedData::FrameData::MAX_SAMPLES)
    {
        sample_count = ProfilerSharedData::FrameData::MAX_SAMPLES;
    }

    // Read samples
    results.clear();
    results.reserve(sample_count);

    for (std::uint32_t i = 0; i < sample_count; ++i)
    {
        const auto& src = frame.samples[i];

        ProfileSample sample;
        sample.name = src.name;
        sample.duration_ms = src.duration_ms;
        sample.call_count = src.call_count;
        results.push_back(sample);
    }

    // Update read index to the frame we just read
    // Use memory barrier to ensure reads are complete before updating index
#ifdef Z_PLATFORM_WINDOWS
    MemoryBarrier();  // Ensure all reads are complete before updating index
    m_SharedData->read_index = current_write_index;
    MemoryBarrier();  // Ensure read_index update is visible
#else
    __sync_synchronize();  // Memory barrier before writing
    m_SharedData->read_index = current_write_index;
    __sync_synchronize();  // Memory barrier after writing
#endif

    return true;
}

bool ProfilerSharedMemory::CreateSharedMemory()
{
#ifdef Z_PLATFORM_WINDOWS
    // Create file mapping
    m_FileMappingHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(SHARED_MEMORY_SIZE), SHARED_MEMORY_NAME);

    if (m_FileMappingHandle == nullptr)
    {
        return false;
    }

    // Check if it already existed (ERROR_ALREADY_EXISTS)
    bool already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);

    // Map view of file
    m_SharedData = static_cast<ProfilerSharedData*>(
        MapViewOfFile(m_FileMappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEMORY_SIZE));

    if (m_SharedData == nullptr)
    {
        CloseHandle(m_FileMappingHandle);
        m_FileMappingHandle = nullptr;
        return false;
    }

    // Initialize if we created it
    if (!already_exists)
    {
        std::memset(m_SharedData, 0, SHARED_MEMORY_SIZE);
        m_SharedData->write_index = 0;
        m_SharedData->read_index = 0;
    }

    m_Initialized = true;
    return true;
#elif defined(Z_PLATFORM_ANDROID) || defined(Z_PLATFORM_OHOS)
    // Mobile: 暂不启用进程间 profiler 共享内存通道，避免依赖桌面/高版本 POSIX shm 能力
    return false;
#else
    // Unix/Linux: Use POSIX shared memory
    m_ShmFd = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);
    if (m_ShmFd == -1)
    {
        return false;
    }

    // Set size
    if (ftruncate(m_ShmFd, SHARED_MEMORY_SIZE) == -1)
    {
        close(m_ShmFd);
        m_ShmFd = -1;
        return false;
    }

    // Map shared memory
    m_SharedMemoryPtr = mmap(nullptr, SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_ShmFd, 0);
    if (m_SharedMemoryPtr == MAP_FAILED)
    {
        close(m_ShmFd);
        m_ShmFd = -1;
        return false;
    }

    m_SharedData = static_cast<ProfilerSharedData*>(m_SharedMemoryPtr);

    // Initialize if we created it
    std::memset(m_SharedData, 0, SHARED_MEMORY_SIZE);
    m_SharedData->write_index = 0;
    m_SharedData->read_index = 0;

    m_Initialized = true;
    return true;
#endif
}

bool ProfilerSharedMemory::OpenSharedMemory()
{
#ifdef Z_PLATFORM_WINDOWS
    // Open existing file mapping with read/write access (reader needs to write read_index)
    m_FileMappingHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHARED_MEMORY_NAME);

    if (m_FileMappingHandle == nullptr)
    {
        return false;
    }

    // Map view of file with read/write access
    m_SharedData = static_cast<ProfilerSharedData*>(
        MapViewOfFile(m_FileMappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEMORY_SIZE));

    if (m_SharedData == nullptr)
    {
        CloseHandle(m_FileMappingHandle);
        m_FileMappingHandle = nullptr;
        return false;
    }

    m_Initialized = true;
    return true;
#elif defined(Z_PLATFORM_ANDROID)
    return false;
#else
    // Unix/Linux: Open existing shared memory with read/write access (reader needs to write read_index)
    m_ShmFd = shm_open(SHARED_MEMORY_NAME, O_RDWR, 0666);
    if (m_ShmFd == -1)
    {
        return false;
    }

    // Map shared memory with read/write access
    m_SharedMemoryPtr = mmap(nullptr, SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_ShmFd, 0);
    if (m_SharedMemoryPtr == MAP_FAILED)
    {
        close(m_ShmFd);
        m_ShmFd = -1;
        return false;
    }

    m_SharedData = static_cast<ProfilerSharedData*>(m_SharedMemoryPtr);

    m_Initialized = true;
    return true;
#endif
}

void ProfilerSharedMemory::CloseSharedMemory()
{
    if (m_SharedData == nullptr)
    {
        return;
    }

#ifdef Z_PLATFORM_WINDOWS
    if (m_SharedData)
    {
        UnmapViewOfFile(m_SharedData);
        m_SharedData = nullptr;
    }

    if (m_FileMappingHandle)
    {
        CloseHandle(m_FileMappingHandle);
        m_FileMappingHandle = nullptr;
    }
#elif defined(Z_PLATFORM_ANDROID) || defined(Z_PLATFORM_OHOS)
    m_SharedData = nullptr;
    m_SharedMemoryPtr = nullptr;
    m_ShmFd = -1;
#else
    if (m_SharedMemoryPtr && m_SharedMemoryPtr != MAP_FAILED)
    {
        munmap(m_SharedMemoryPtr, SHARED_MEMORY_SIZE);
        m_SharedMemoryPtr = nullptr;
    }

    if (m_ShmFd != -1)
    {
        close(m_ShmFd);
        m_ShmFd = -1;
    }

    // If we're the writer, unlink the shared memory
    if (m_IsWriter)
    {
        shm_unlink(SHARED_MEMORY_NAME);
    }
#endif
}