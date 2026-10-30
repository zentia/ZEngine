// =============================================================================
// Wwise backend implementation.
//
// Everything that knows about Wwise lives in this one translation unit. The
// header is Wwise-free, so a build without the SDK still compiles every caller
// -- but it also means there is no partial/fake variant: when
// ZENGINE_HAS_WWISE is not defined, every entry point here reports failure and
// GetBackendName() says why.
// =============================================================================

#include "Runtime/Function/Audio/Wwise/WwiseAudioSystem.h"

#include "Runtime/Core/Base/Macro.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(ZENGINE_HAS_WWISE)
#include <AK/MusicEngine/Common/AkMusicEngine.h>
#include <AK/SoundEngine/Common/AkCallback.h>
#include <AK/SoundEngine/Common/AkModule.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <AK/SoundEngine/Common/AkStreamMgrModule.h>

#include <atomic>
#endif

namespace
{
    bool EnvFlagIsSet(const char* in_name)
    {
        const char* value = std::getenv(in_name);
        return (value != nullptr) && (value[0] != '\0') && (std::string(value) != "0");
    }

    std::string EnvOr(const char* in_name, const char* in_fallback)
    {
        const char* value = std::getenv(in_name);
        return (value != nullptr && value[0] != '\0') ? std::string(value) : std::string(in_fallback);
    }
}  // namespace

#if defined(ZENGINE_HAS_WWISE)
namespace
{
    /// Blocking low-level I/O hook and file location resolver.
    ///
    /// Wwise never calls fopen() itself: bank and media reads come back here.
    /// Two deliberate choices:
    ///
    ///  * Blocking, not deferred. Bank loads are synchronous in ZEngine and the
    ///    deferred hook only pays off with dozens of concurrent streams.
    ///  * Wide-character open on Windows (AkOSChar is wchar_t there). Wwise
    ///    hands us the path it was given, so a bank directory with non-ASCII
    ///    characters keeps working -- the same reason the glTF reader parses
    ///    memory instead of calling cgltf_parse_file().
    ///
    /// Base paths replace Wwise's removed SetBasePath(): a bank is requested by
    /// bare name ("Init"), and we try each registered directory in order.
    class WwiseBlockingFileHook : public AK::StreamMgr::IAkFileLocationResolver
                                , public AK::StreamMgr::IAkIOHookBlocking
    {
    public:
        WwiseBlockingFileHook() = default;

        void AddBasePath(const std::filesystem::path& in_path)
        {
            m_BasePaths.push_back(in_path);
        }

        bool Init()
        {
            AkDeviceSettings settings;
            AK::StreamMgr::GetDefaultDeviceSettings(settings);
            settings.uSchedulerTypeFlags = AK_SCHEDULER_BLOCKING;

            m_DeviceId = AK::StreamMgr::CreateDevice(settings, this);
            if (m_DeviceId == AK_INVALID_DEVICE_ID)
            {
                return false;
            }

            AK::StreamMgr::SetFileLocationResolver(this);
            return true;
        }

        void Term()
        {
            if (m_DeviceId != AK_INVALID_DEVICE_ID)
            {
                AK::StreamMgr::DestroyDevice(m_DeviceId);
                m_DeviceId = AK_INVALID_DEVICE_ID;
            }
            AK::StreamMgr::SetFileLocationResolver(nullptr);
        }

        // ---- IAkFileLocationResolver ----
        AKRESULT Open(const AkOSChar* in_pszFileName,
                      AkOpenMode in_eOpenMode,
                      AkFileSystemFlags* /*in_pFlags*/,
                      bool& io_bSyncOpen,
                      AkFileDesc& out_fileDesc) override
        {
            io_bSyncOpen = true;  // Opening a bank is a handful of microseconds.

            if (in_pszFileName == nullptr || in_pszFileName[0] == 0)
            {
                return AK_InvalidParameter;
            }
            if (in_eOpenMode != AK_OpenModeRead)
            {
                // ZEngine never writes through Wwise streams; saying so beats
                // silently handing out a read-only handle.
                LOG_ERROR(ZAudio, "Wwise low-level I/O is read-only; refusing a write-mode open");
                return AK_Fail;
            }

            const std::filesystem::path requested(in_pszFileName);
            std::error_code ignored;
            std::filesystem::path resolved;

            if (std::filesystem::is_regular_file(requested, ignored))
            {
                resolved = requested;
            }
            else
            {
                for (const auto& base : m_BasePaths)
                {
                    const std::filesystem::path candidate = base / requested;
                    if (std::filesystem::is_regular_file(candidate, ignored))
                    {
                        resolved = candidate;
                        break;
                    }
                }
            }

            if (resolved.empty())
            {
                return AK_FileNotFound;
            }

            const uintmax_t size = std::filesystem::file_size(resolved, ignored);

            std::FILE* file = nullptr;
#if defined(_WIN32)
            file = ::_wfopen(resolved.wstring().c_str(), L"rb");
#else
            file = std::fopen(resolved.string().c_str(), "rb");
#endif
            if (file == nullptr)
            {
                return AK_Fail;
            }

            // AkSafeFileHandle is void**: the SDK wants a stable slot holding the
            // real handle, exactly like CAkDefaultIOHookBlocking allocates.
            out_fileDesc.hSafeFile = new (std::nothrow) void*[1];
            if (out_fileDesc.hSafeFile == nullptr)
            {
                std::fclose(file);
                return AK_InsufficientMemory;
            }
            *out_fileDesc.hSafeFile = file;

            out_fileDesc.iFileSize = static_cast<AkInt64>(size);
            out_fileDesc.uSector = 0u;
            out_fileDesc.deviceID = m_DeviceId;
            out_fileDesc.pCustomParam = nullptr;
            out_fileDesc.uCustomParamSize = 0u;
            out_fileDesc.pPackage = nullptr;
            out_fileDesc.bReadOnly = true;
            return AK_Success;
        }

        /// File-ID (file package) resolution is not wired up: ZEngine loads
        /// loose .bnk files, so an ID can only ever mean "not found".
        AKRESULT Open(AkFileID /*in_fileID*/,
                      AkOpenMode /*in_eOpenMode*/,
                      AkFileSystemFlags* /*in_pFlags*/,
                      bool& io_bSyncOpen,
                      AkFileDesc& /*out_fileDesc*/) override
        {
            io_bSyncOpen = true;
            return AK_FileNotFound;
        }

        // ---- IAkLowLevelIOHook ----
        AKRESULT Close(AkFileDesc& in_fileDesc) override
        {
            if (std::FILE* file = FileFrom(in_fileDesc))
            {
                std::fclose(file);
            }
            delete[] in_fileDesc.hSafeFile;
            in_fileDesc.hSafeFile = nullptr;
            return AK_Success;
        }

        AkUInt32 GetBlockSize(AkFileDesc& /*in_fileDesc*/) override
        {
            // No alignment requirement: every byte offset is a legal position.
            return 1u;
        }

        void GetDeviceDesc(AkDeviceDesc& out_deviceDesc) override
        {
            out_deviceDesc.deviceID = m_DeviceId;
            out_deviceDesc.bCanRead = true;
            out_deviceDesc.bCanWrite = false;
            out_deviceDesc.uStringSize = 0u;
            out_deviceDesc.szDeviceName[0] = 0;
        }

        AkUInt32 GetDeviceData() override { return 0u; }

        // ---- IAkIOHookBlocking ----
        AKRESULT Read(AkFileDesc& in_fileDesc,
                      const AkIoHeuristics& /*in_heuristics*/,
                      void* out_pBuffer,
                      AkIOTransferInfo& io_transferInfo) override
        {
            std::FILE* file = FileFrom(in_fileDesc);
            if (file == nullptr)
            {
                return AK_Fail;
            }

            const auto block_size = static_cast<AkUInt64>(GetBlockSize(in_fileDesc));
            const AkUInt64 position = (static_cast<AkUInt64>(in_fileDesc.uSector) * block_size) + io_transferInfo.uFilePosition;

#if defined(_WIN32)
            if (::_fseeki64(file, static_cast<long long>(position), SEEK_SET) != 0)
            {
                return AK_Fail;
            }
#else
            if (std::fseeko(file, static_cast<off_t>(position), SEEK_SET) != 0)
            {
                return AK_Fail;
            }
#endif
            const size_t read = std::fread(out_pBuffer, 1, static_cast<size_t>(io_transferInfo.uRequestedSize), file);
            if (read != static_cast<size_t>(io_transferInfo.uRequestedSize))
            {
                return AK_Fail;
            }
            return AK_Success;
        }

        AKRESULT Write(AkFileDesc& /*in_fileDesc*/,
                       const AkIoHeuristics& /*in_heuristics*/,
                       void* /*in_pData*/,
                       AkIOTransferInfo& /*io_transferInfo*/) override
        {
            return AK_Fail;
        }

    private:
        static std::FILE* FileFrom(const AkFileDesc& in_fileDesc)
        {
            if (in_fileDesc.hSafeFile == nullptr)
            {
                return nullptr;
            }
            return static_cast<std::FILE*>(*in_fileDesc.hSafeFile);
        }

        AkDeviceID m_DeviceId {AK_INVALID_DEVICE_ID};
        std::vector<std::filesystem::path> m_BasePaths;
    };

    AkVector ToAk(const AudioVec3& in_vector)
    {
        AkVector result;
        result.X = in_vector.x;
        result.Y = in_vector.y;
        result.Z = in_vector.z;
        return result;
    }
}  // namespace
#endif  // ZENGINE_HAS_WWISE

struct WwiseAudioSystem::Impl
{
#if defined(ZENGINE_HAS_WWISE)
    WwiseBlockingFileHook io;
    std::vector<std::pair<std::string, AkBankID>> loaded_banks;

    // ---- end-of-event tracking -------------------------------------------
    // Written from the Wwise audio thread (the PostEvent callback), read from
    // the main thread. It must not take a lock: a mutex on the audio thread can
    // stall the whole mix, and the symptom would be a glitch in every sound in
    // the game, not just the one that finished.
    //
    // A ring of atomics is therefore lossy on purpose: after kFinishedCapacity
    // events finish between two reads, the oldest ids are forgotten again and
    // IsPlayingId() can answer true for a sound that already ended. That is the
    // trade worth making -- the alternative is unbounded memory, and the only
    // caller re-checks its own id every frame, so a stale entry self-heals on
    // the next PostEvent (which overwrites the id it is asked about).
    static constexpr size_t kFinishedCapacity = 256u;
    std::atomic<uint64_t> finished_ring[kFinishedCapacity] {};
    std::atomic<size_t> finished_next {0u};

    void MarkFinished(uint64_t in_playing_id)
    {
        if (in_playing_id == WwiseAudioSystem::kInvalidPlayingId)
        {
            return;
        }
        const size_t slot = finished_next.fetch_add(1u, std::memory_order_relaxed) % kFinishedCapacity;
        finished_ring[slot].store(in_playing_id, std::memory_order_release);
    }

    bool HasFinished(uint64_t in_playing_id) const
    {
        for (size_t i = 0; i < kFinishedCapacity; ++i)
        {
            if (finished_ring[i].load(std::memory_order_acquire) == in_playing_id)
            {
                return true;
            }
        }
        return false;
    }

    /// Runs on the Wwise audio thread. Keep it trivially small and allocation-free.
    ///
    /// It has to be a member rather than a file-scope function: Impl is private
    /// to WwiseAudioSystem, so only a member can name the type it casts the
    /// cookie to.
    ///
    /// AK_CALLBACK() only declares the *typedef* form, so a definition uses the
    /// underlying calling-convention macro directly: handing Wwise a cdecl
    /// function where it expects AKSOUNDENGINE_CALL corrupts the stack on 32-bit
    /// builds and looks fine on 64-bit, which is exactly the kind of bug that
    /// ships.
    static void AKSOUNDENGINE_CALL EndOfEventCallback(AkCallbackType in_type, AkCallbackInfo* in_info)
    {
        if (in_type != AK_EndOfEvent || in_info == nullptr)
        {
            return;
        }
        auto* impl = static_cast<Impl*>(in_info->pCookie);
        if (impl == nullptr)
        {
            return;
        }
        // AK_EndOfEvent means in_info is really an AkEventCallbackInfo, and
        // playingID lives on that derived struct -- the base carries only the
        // cookie and the game object id.
        const auto* event_info = static_cast<const AkEventCallbackInfo*>(in_info);
        impl->MarkFinished(static_cast<uint64_t>(event_info->playingID));
    }
#else
    int unused {0};
#endif
};

WwiseAudioSystem::WwiseAudioSystem() = default;
WwiseAudioSystem::~WwiseAudioSystem() = default;

bool WwiseAudioSystem::IsCompiledIn()
{
#if defined(ZENGINE_HAS_WWISE)
    return true;
#else
    return false;
#endif
}

bool WwiseAudioSystem::IsActive() const
{
    return m_Active;
}

const char* WwiseAudioSystem::GetBackendName() const
{
    if (m_Active)
    {
        return "Wwise 2019.2.15";
    }
    return IsCompiledIn() ? "Wwise (compiled in, not active)" : "Wwise (not compiled in)";
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool WwiseAudioSystem::Initialize()
{
    m_Active = false;

#if !defined(ZENGINE_HAS_WWISE)
    LOG_INFO(ZAudio,
             "Wwise backend not compiled in (configure with -DZENGINE_WWISE_SDK_DIR=<sdk>); "
             "the internal audio backend owns the output");
    return true;
#else
    if (EnvFlagIsSet("ZENGINE_AUDIO_DISABLE"))
    {
        // Same contract as AudioSystem: disabled audio must not fail startup.
        LOG_INFO(ZAudio, "Wwise backend skipped: ZENGINE_AUDIO_DISABLE is set");
        return true;
    }

    if (!EnvFlagIsSet("ZENGINE_WWISE_ENABLE"))
    {
        LOG_INFO(ZAudio, "Wwise backend compiled in but not enabled (set ZENGINE_WWISE_ENABLE=1 to use it)");
        return true;
    }

    m_Impl = std::make_unique<Impl>();

    if (m_BankDirectory.empty())
    {
        const std::string configured = EnvOr("ZENGINE_WWISE_BANK_DIR", "asset/soundbanks/Windows");
        m_BankDirectory = std::filesystem::path(configured);
    }

    AkMemSettings memory_settings;
    AK::MemoryMgr::GetDefaultSettings(memory_settings);
    if (AK::MemoryMgr::Init(&memory_settings) != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise: AK::MemoryMgr::Init failed; falling back to the internal audio backend");
        m_Impl.reset();
        return true;
    }

    AkStreamMgrSettings stream_settings;
    AK::StreamMgr::GetDefaultSettings(stream_settings);
    if (AK::StreamMgr::Create(stream_settings) == nullptr)
    {
        LOG_ERROR(ZAudio, "Wwise: AK::StreamMgr::Create failed; falling back to the internal audio backend");
        AK::MemoryMgr::Term();
        m_Impl.reset();
        return true;
    }

    if (!m_Impl->io.Init())
    {
        LOG_ERROR(ZAudio, "Wwise: streaming device creation failed; falling back to the internal audio backend");
        if (AK::IAkStreamMgr::Get() != nullptr)
        {
            AK::IAkStreamMgr::Get()->Destroy();
        }
        AK::MemoryMgr::Term();
        m_Impl.reset();
        return true;
    }

    m_Impl->io.AddBasePath(m_BankDirectory);

    AkInitSettings init_settings;
    AK::SoundEngine::GetDefaultInitSettings(init_settings);
    // ZEngine is Z-up, so the ground plane is XY. Leaving this at the Wwise
    // default (XZ) mirrors every 3D sound silently -- no error, wrong panning.
    init_settings.eFloorPlane = AkFloorPlane_XY;

    AkPlatformInitSettings platform_settings;
    AK::SoundEngine::GetDefaultPlatformInitSettings(platform_settings);
    if (AK::SoundEngine::Init(&init_settings, &platform_settings) != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise: AK::SoundEngine::Init failed; falling back to the internal audio backend");
        m_Impl->io.Term();
        if (AK::IAkStreamMgr::Get() != nullptr)
        {
            AK::IAkStreamMgr::Get()->Destroy();
        }
        AK::MemoryMgr::Term();
        m_Impl.reset();
        return true;
    }

    AkMusicSettings music_settings;
    AK::MusicEngine::GetDefaultInitSettings(music_settings);
    if (AK::MusicEngine::Init(&music_settings) != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise: AK::MusicEngine::Init failed; falling back to the internal audio backend");
        AK::SoundEngine::Term();
        m_Impl->io.Term();
        if (AK::IAkStreamMgr::Get() != nullptr)
        {
            AK::IAkStreamMgr::Get()->Destroy();
        }
        AK::MemoryMgr::Term();
        m_Impl.reset();
        return true;
    }

    AK::SoundEngine::RegisterGameObj(kDefaultListenerId, "Default Listener");
    const AkGameObjectID listener = static_cast<AkGameObjectID>(kDefaultListenerId);
    AK::SoundEngine::SetDefaultListeners(&listener, 1u);

    m_Active = true;

    std::error_code ignored;
    const bool bank_dir_exists = std::filesystem::is_directory(m_BankDirectory, ignored);
    LOG_INFO(ZAudio,
             "Wwise 2019.2.15 sound engine ready; bank directory '{}' ({})",
             m_BankDirectory.generic_string(),
             bank_dir_exists ? "found" : "MISSING - LoadBank() will fail until banks are generated");

    if (!bank_dir_exists)
    {
        // Loud, because every downstream symptom (silent game, PostEvent
        // returning AK_INVALID_PLAYING_ID) looks like a code bug otherwise.
        LOG_WARNING(ZAudio,
                    "No SoundBank directory at '{}'. Banks come from Wwise Authoring "
                    "(GeneratedSoundBanks/<Platform>); point ZENGINE_WWISE_BANK_DIR at them.",
                    m_BankDirectory.generic_string());
    }

    return true;
#endif
}

void WwiseAudioSystem::Shutdown()
{
#if defined(ZENGINE_HAS_WWISE)
    if (m_Impl != nullptr)
    {
        if (m_Active)
        {
            AK::SoundEngine::StopAll();
            AK::MusicEngine::Term();
            AK::SoundEngine::Term();
        }

        m_Impl->io.Term();

        if (AK::IAkStreamMgr::Get() != nullptr)
        {
            AK::IAkStreamMgr::Get()->Destroy();
        }
        if (AK::MemoryMgr::IsInitialized())
        {
            AK::MemoryMgr::Term();
        }
        m_Impl.reset();
    }
#endif

    if (m_Active)
    {
        LOG_INFO(ZAudio, "Wwise sound engine shut down");
    }
    m_Active = false;
}

// ---------------------------------------------------------------------------
// banks
// ---------------------------------------------------------------------------

void WwiseAudioSystem::SetBankDirectory(std::filesystem::path in_directory)
{
    m_BankDirectory = std::move(in_directory);
#if defined(ZENGINE_HAS_WWISE)
    if (m_Impl != nullptr)
    {
        m_Impl->io.AddBasePath(m_BankDirectory);
    }
#endif
}

bool WwiseAudioSystem::LoadBank(const std::string& in_name)
{
    if (in_name.empty())
    {
        return false;
    }

    if (!m_Active)
    {
        LOG_ERROR(ZAudio, "Wwise LoadBank('{}') ignored: the Wwise sound engine is not active ({})", in_name, GetBackendName());
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    for (const auto& loaded : m_Impl->loaded_banks)
    {
        if (loaded.first == in_name)
        {
            return true;
        }
    }

    AkBankID bank_id = 0u;
    const AKRESULT result = AK::SoundEngine::LoadBank(in_name.c_str(), bank_id);
    if (result != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise LoadBank('{}') failed: AKRESULT {} (looked under '{}')", in_name, static_cast<int>(result), m_BankDirectory.generic_string());
        return false;
    }

    m_Impl->loaded_banks.emplace_back(in_name, bank_id);
    LOG_INFO(ZAudio, "Wwise LoadBank('{}') ok, id {:#x}", in_name, static_cast<uint32_t>(bank_id));
    return true;
#else
    return false;
#endif
}

bool WwiseAudioSystem::UnloadBank(const std::string& in_name)
{
    if (!m_Active)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    for (auto it = m_Impl->loaded_banks.begin(); it != m_Impl->loaded_banks.end(); ++it)
    {
        if (it->first != in_name)
        {
            continue;
        }
        const AKRESULT result = AK::SoundEngine::UnloadBank(it->second, nullptr);
        m_Impl->loaded_banks.erase(it);
        if (result != AK_Success)
        {
            LOG_ERROR(ZAudio, "Wwise UnloadBank('{}') failed: AKRESULT {}", in_name, static_cast<int>(result));
            return false;
        }
        return true;
    }

    LOG_WARNING(ZAudio, "Wwise UnloadBank('{}') ignored: that bank was never loaded", in_name);
    return false;
#else
    return false;
#endif
}

size_t WwiseAudioSystem::GetLoadedBankCount() const
{
#if defined(ZENGINE_HAS_WWISE)
    return (m_Impl != nullptr) ? m_Impl->loaded_banks.size() : 0u;
#else
    return 0u;
#endif
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

uint64_t WwiseAudioSystem::PostEvent(const char* in_event_name, uint64_t in_game_object)
{
    if (in_event_name == nullptr || in_event_name[0] == '\0')
    {
        LOG_ERROR(ZAudio, "Wwise PostEvent ignored: empty event name");
        return kInvalidPlayingId;
    }

    if (!m_Active)
    {
        LOG_ERROR(ZAudio, "Wwise PostEvent('{}') ignored: the Wwise sound engine is not active ({})", in_event_name, GetBackendName());
        return kInvalidPlayingId;
    }

#if defined(ZENGINE_HAS_WWISE)
    // Always ask for AK_EndOfEvent: IsPlayingId() has no other way to be honest,
    // and the notification costs one callback per finished event. The cookie is
    // the Impl, which is where the finished-id ring lives.
    const AkPlayingID playing_id = AK::SoundEngine::PostEvent(in_event_name,
                                                              static_cast<AkGameObjectID>(in_game_object),
                                                              AK_EndOfEvent,
                                                              &Impl::EndOfEventCallback,
                                                              m_Impl.get());
    if (playing_id == AK_INVALID_PLAYING_ID)
    {
        // Almost always one of: the event is not in any loaded bank, or the game
        // object was never registered. Both are silent in a release Wwise build.
        LOG_WARNING(ZAudio,
                    "Wwise PostEvent('{}') on game object {} returned no playing id: the event is not in a loaded bank, "
                    "or the game object is not registered",
                    in_event_name,
                    in_game_object);
        return kInvalidPlayingId;
    }
    return static_cast<uint64_t>(playing_id);
#else
    return kInvalidPlayingId;
#endif
}

void WwiseAudioSystem::StopAll(uint64_t in_game_object)
{
    if (!m_Active)
    {
        return;
    }

#if defined(ZENGINE_HAS_WWISE)
    if (in_game_object == kInvalidGameObject)
    {
        AK::SoundEngine::StopAll();
        return;
    }
    AK::SoundEngine::StopAll(static_cast<AkGameObjectID>(in_game_object));
#endif
}

void WwiseAudioSystem::StopPlayingId(uint64_t in_playing_id, float in_fade_out_seconds)
{
    if (!m_Active || in_playing_id == kInvalidPlayingId)
    {
        return;
    }

#if defined(ZENGINE_HAS_WWISE)
    const AkTimeMs fade_ms = static_cast<AkTimeMs>(in_fade_out_seconds > 0.0f ? in_fade_out_seconds * 1000.0f : 0.0f);
    // AkActionOnEventType lives inside AK::SoundEngine, unlike AkPlayingID /
    // AkTimeMs which are at the AK root -- the SDK is not consistent about it.
    AK::SoundEngine::ExecuteActionOnPlayingID(AK::SoundEngine::AkActionOnEventType_Stop,
                                              static_cast<AkPlayingID>(in_playing_id),
                                              fade_ms);
#endif
}

bool WwiseAudioSystem::SetPlayingPaused(uint64_t in_playing_id, bool in_paused)
{
    if (!m_Active || in_playing_id == kInvalidPlayingId)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    AK::SoundEngine::ExecuteActionOnPlayingID(
        in_paused ? AK::SoundEngine::AkActionOnEventType_Pause : AK::SoundEngine::AkActionOnEventType_Resume,
        static_cast<AkPlayingID>(in_playing_id));
    return true;
#else
    return false;
#endif
}

bool WwiseAudioSystem::IsPlayingId(uint64_t in_playing_id) const
{
    // Only meaningful for an id that came from PostEvent(). Reporting "playing"
    // for an id we never handed out would be a lie, and returning false for one
    // is the safe direction: the caller drops a handle it cannot verify.
    if (in_playing_id == kInvalidPlayingId)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    return (m_Impl != nullptr) && !m_Impl->HasFinished(in_playing_id);
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// game objects / listener
// ---------------------------------------------------------------------------

bool WwiseAudioSystem::RegisterGameObject(uint64_t in_id, const char* in_debug_name)
{
    if (!m_Active || in_id == kInvalidGameObject)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    const AKRESULT result = AK::SoundEngine::RegisterGameObj(static_cast<AkGameObjectID>(in_id), in_debug_name != nullptr ? in_debug_name : "");
    if (result != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise RegisterGameObj({}) failed: AKRESULT {}", in_id, static_cast<int>(result));
        return false;
    }
    return true;
#else
    return false;
#endif
}

void WwiseAudioSystem::UnregisterGameObject(uint64_t in_id)
{
    if (!m_Active || in_id == kInvalidGameObject)
    {
        return;
    }

#if defined(ZENGINE_HAS_WWISE)
    AK::SoundEngine::UnregisterGameObj(static_cast<AkGameObjectID>(in_id));
#endif
}

bool WwiseAudioSystem::SetGameObjectTransform(uint64_t in_id, const AudioVec3& in_position, const AudioVec3& in_forward, const AudioVec3& in_up)
{
    if (!m_Active || in_id == kInvalidGameObject)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    AkSoundPosition position;
    position.Set(ToAk(in_position), ToAk(in_forward.Normalised()), ToAk(in_up.Normalised()));
    const AKRESULT result = AK::SoundEngine::SetPosition(static_cast<AkGameObjectID>(in_id), position);
    if (result != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise SetPosition({}) failed: AKRESULT {}", in_id, static_cast<int>(result));
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool WwiseAudioSystem::SetListener(const AudioListenerData& in_listener)
{
    if (!m_Active)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    AkListenerPosition listener;
    listener.Set(ToAk(in_listener.m_Position), ToAk(in_listener.m_Forward.Normalised()), ToAk(in_listener.m_Up.Normalised()));
    const AKRESULT result = AK::SoundEngine::SetPosition(kDefaultListenerId, listener);
    if (result != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise SetPosition(listener) failed: AKRESULT {}", static_cast<int>(result));
        return false;
    }
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// authoring-side controls
// ---------------------------------------------------------------------------

bool WwiseAudioSystem::SetRTPC(const char* in_name, float in_value, uint64_t in_game_object)
{
    if (!m_Active || in_name == nullptr || in_name[0] == '\0')
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    const AkGameObjectID object = (in_game_object == kInvalidGameObject)
                                      ? AK_INVALID_GAME_OBJECT
                                      : static_cast<AkGameObjectID>(in_game_object);
    const AKRESULT result = AK::SoundEngine::SetRTPCValue(in_name, static_cast<AkRtpcValue>(in_value), object);
    if (result != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise SetRTPCValue('{}', {}) failed: AKRESULT {}", in_name, in_value, static_cast<int>(result));
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool WwiseAudioSystem::SetSwitch(const char* in_group, const char* in_value, uint64_t in_game_object)
{
    if (!m_Active || in_group == nullptr || in_value == nullptr || in_game_object == kInvalidGameObject)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    const AKRESULT result = AK::SoundEngine::SetSwitch(in_group, in_value, static_cast<AkGameObjectID>(in_game_object));
    if (result != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise SetSwitch('{}', '{}') failed: AKRESULT {}", in_group, in_value, static_cast<int>(result));
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool WwiseAudioSystem::SetState(const char* in_group, const char* in_value)
{
    if (!m_Active || in_group == nullptr || in_value == nullptr)
    {
        return false;
    }

#if defined(ZENGINE_HAS_WWISE)
    const AKRESULT result = AK::SoundEngine::SetState(in_group, in_value);
    if (result != AK_Success)
    {
        LOG_ERROR(ZAudio, "Wwise SetState('{}', '{}') failed: AKRESULT {}", in_group, in_value, static_cast<int>(result));
        return false;
    }
    return true;
#else
    return false;
#endif
}

void WwiseAudioSystem::RenderAudio()
{
    if (!m_Active)
    {
        return;
    }

#if defined(ZENGINE_HAS_WWISE)
    AK::SoundEngine::RenderAudio();
#endif
}
