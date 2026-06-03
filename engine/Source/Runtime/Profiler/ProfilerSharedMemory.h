#pragma once

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Profiler/Profiler.h"

#include <cstdint>
#include <string>

// Shared memory structure for profiler data
// This structure is shared between Editor and ProfilerUI processes
struct ProfilerSharedData
{
    // Synchronization
    volatile std::uint32_t write_index;  // Writer increments this (mod MAX_FRAMES)
    volatile std::uint32_t read_index;   // Reader tracks this (mod MAX_FRAMES)

    // Frame data structure
    struct FrameData
    {
        float frame_time_ms;
        std::uint32_t sample_count;
        std::uint32_t frame_number;  // Frame number for identification

        // Sample data (fixed size array for simplicity)
        static constexpr std::size_t MAX_SAMPLES = 1024;
        struct Sample
        {
            char name[256];
            float duration_ms;
            std::int32_t call_count;
        } samples[MAX_SAMPLES];
    };

    // Multi-frame buffer (ring buffer)
    // Increased from 5 to 60 to reduce frame skipping when UI reads slower than game updates
    static constexpr std::size_t MAX_FRAMES = 60;  // Cache 60 frames (~1 second at 60 FPS)
    FrameData frames[MAX_FRAMES];
};

/**
 * @brief Shared memory manager for inter-process profiler communication
 *
 * This class manages shared memory between the Editor process (writer)
 * and the ProfilerUI process (reader).
 */
class ProfilerSharedMemory
{
public:
    ProfilerSharedMemory();
    ~ProfilerSharedMemory();

    /**
     * @brief Initialize as writer (Editor process)
     * @return true if successful, false otherwise
     */
    bool InitializeAsWriter();

    /**
     * @brief Initialize as reader (ProfilerUI process)
     * @return true if successful, false otherwise
     */
    bool InitializeAsReader();

    /**
     * @brief Shutdown and cleanup
     */
    void Shutdown();

    /**
     * @brief Write profiler data to shared memory (Writer only)
     * @param results Profiler results to write
     * @param frame_time_ms Frame time in milliseconds
     * @param frame_number Frame number for identification
     */
    void WriteData(const std::vector<ProfileSample>& results, float frame_time_ms, std::uint32_t frame_number);

    /**
     * @brief Read profiler data from shared memory (Reader only)
     * @param results Output vector for profiler results
     * @param frame_time_ms Output frame time in milliseconds
     * @param frame_number Output frame number
     * @return true if new data was read, false otherwise
     */
    bool ReadData(std::vector<ProfileSample>& results, float& frame_time_ms, std::uint32_t& frame_number);

    /**
     * @brief Check if shared memory is initialized
     */
    bool isInitialized() const { return m_Initialized; }

    /**
     * @brief Check if this is a writer instance
     */
    bool isWriter() const { return m_IsWriter; }

private:
    bool CreateSharedMemory();
    bool OpenSharedMemory();
    void CloseSharedMemory();

#ifdef Z_PLATFORM_WINDOWS
    static constexpr const char* SHARED_MEMORY_NAME = "ZEngine_Profiler_SharedMemory";
#else
    static constexpr const char* SHARED_MEMORY_NAME = "/ZEngine_Profiler_SharedMemory";  // POSIX requires leading '/'
#endif
    static constexpr std::size_t SHARED_MEMORY_SIZE = sizeof(ProfilerSharedData);

    ProfilerSharedData* m_SharedData;
    bool m_Initialized;
    bool m_IsWriter;

#ifdef Z_PLATFORM_WINDOWS
    void* m_FileMappingHandle;  // HANDLE
#else
    int m_ShmFd;
    void* m_SharedMemoryPtr;
#endif
};