#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Audio/AudioCommon.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

// =============================================================================
// WwiseAudioSystem: optional Audiokinetic Wwise backend, living *beside* the
// built-in AudioSystem (mixer + WASAPI), not replacing it.
//
//   Internal backend : AudioSystem -> AudioMixer -> IAudioDevice (WASAPI/Null)
//   Wwise backend    : WwiseAudioSystem -> Wwise sound engine -> platform sink
//
// Which one actually owns the output is decided once, in AudioSystem::Initialize:
// when this system is active, AudioSystem deliberately opens the null device so
// the two never drive the same speakers. Everything here reports its real state
// -- there is no "Wwise-ish" mode. Three independent things can be false:
//
//   IsCompiledIn()  -- the build linked the Wwise SDK at all
//                      (false when ZENGINE_WWISE_SDK_DIR was not configured)
//   IsActive()      -- the sound engine is up and owns the output
//   a call result   -- the specific bank/event/game object action succeeded
//
// Enable at runtime with ZENGINE_WWISE_ENABLE=1 (plus ZENGINE_WWISE_BANK_DIR to
// point at the GeneratedSoundBanks/<Platform> folder). Disabled by default, and
// ignored entirely when ZENGINE_AUDIO_DISABLE=1, so CI is unaffected.
//
// This header is deliberately free of Wwise headers: only POD types cross the
// boundary, so a build without the SDK still compiles every caller.
// =============================================================================

class WwiseAudioSystem : public IEngineSystem
{
public:
    // Defined out of line: both the constructor and the destructor touch
    // std::unique_ptr<Impl>, and Impl is only complete inside the .cpp. Leaving
    // either one implicit makes every translation unit that merely *declares* a
    // WwiseAudioSystem (RegisterRuntime.cpp, the smoke test) fail with C2027 /
    // C2338 "can't delete an incomplete type".
    WwiseAudioSystem();
    ~WwiseAudioSystem() override;

    std::string GetName() const override { return "WwiseAudioSystem"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }

    // ---- availability (never lies about what is really wired up) ----
    /// True only when this build was configured against a real Wwise SDK.
    static bool IsCompiledIn();
    /// True only when the Wwise sound engine is initialised and owns the output.
    bool IsActive() const;
    /// "Wwise <version>" when active, otherwise why it is not. Never null.
    const char* GetBackendName() const;

    // ---- sound banks ----
    void SetBankDirectory(std::filesystem::path in_directory);
    const std::filesystem::path& GetBankDirectory() const { return m_BankDirectory; }
    /// Loads <bank directory>/<name>.bnk. Returns false (and logs) on failure.
    bool LoadBank(const std::string& in_name);
    bool UnloadBank(const std::string& in_name);
    size_t GetLoadedBankCount() const;

    // ---- events ----
    /// Returns kInvalidPlayingId when the event could not be posted.
    uint64_t PostEvent(const char* in_event_name, uint64_t in_game_object);
    /// kInvalidGameObject stops everything, a real id stops only that emitter.
    void StopAll(uint64_t in_game_object = kInvalidGameObject);

    // ---- game objects / listener ----
    bool RegisterGameObject(uint64_t in_id, const char* in_debug_name);
    void UnregisterGameObject(uint64_t in_id);
    bool SetGameObjectTransform(uint64_t in_id, const AudioVec3& in_position, const AudioVec3& in_forward, const AudioVec3& in_up);
    bool SetListener(const AudioListenerData& in_listener);

    // ---- authoring-side controls ----
    bool SetRTPC(const char* in_name, float in_value, uint64_t in_game_object = kInvalidGameObject);
    bool SetSwitch(const char* in_group, const char* in_value, uint64_t in_game_object);
    bool SetState(const char* in_group, const char* in_value);

    /// Per-frame pump. Cheap no-op: Wwise runs its own audio thread by default,
    /// but the call is required once the engine is configured otherwise.
    void RenderAudio();

    static constexpr uint64_t kInvalidGameObject = 0ull;
    static constexpr uint64_t kInvalidPlayingId = 0ull;
    /// Reserved for the single default listener. ZEngine has no split-screen audio.
    static constexpr uint64_t kDefaultListenerId = 1ull;

protected:
    bool Initialize() override;
    void Shutdown() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;

    std::filesystem::path m_BankDirectory;
    bool m_Active {false};
};
