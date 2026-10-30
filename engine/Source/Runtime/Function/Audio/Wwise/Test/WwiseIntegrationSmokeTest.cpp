// =============================================================================
// Wwise backend smoke test.
//
//   cmake -B build -DZENGINE_BUILD_WWISE_SMOKE_TEST=ON
//   cmake --build build --target WwiseIntegrationSmokeTest
//   ctest --test-dir build -C Debug -R WwiseIntegrationSmokeTest
//
//   # real sound-engine run (needs an audio device + a bank directory):
//   WwiseIntegrationSmokeTest --with-device \
//       --bank-dir E:/Agent2/SVN/Wwise/.../GeneratedSoundBanks/Windows
//
// Four groups, in order of how much they can prove:
//
//   A. No fake support. WwiseAudioSystem is compiled unconditionally, so the
//      dangerous state is "the API exists but does nothing". A组 pins
//      IsCompiledIn()/GetBackendName()/every entry point to the real build
//      state. Runs everywhere, needs no device.
//
//   B. Wiring gate. The backend can be fully implemented and still never be
//      reached -- that compiles, runs and stays silent. B组 is source-level and
//      pins the four things that actually connect it: registration order
//      (Wwise must decide before AudioSystem), AudioSystem asking the
//      question, GetBackendName reporting Wwise, and file playback refusing
//      instead of silently rendering into the null device.
//
//   C. Real run: init -> load Init.bnk -> register a game object -> post an
//      event -> pump -> term. Needs a Wwise-linked build, a bank directory and
//      an audio output device, so it only runs under --with-device. Skipping is
//      printed with the number of checks skipped; it is never silent.
//
//   D. AudioSourceComponent routing. The backend can be perfectly wired (B) and
//      still leave every emitter silent, because the two backends address
//      content differently: files vs. named events. D1..D7 test that decision
//      as a pure function (runs anywhere), D8..D16 pin that the component
//      really routes through it.
// =============================================================================

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Audio/AudioCommon.h"
#include "Runtime/Function/Audio/Wwise/WwiseAudioSystem.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(ZENGINE_HAS_WWISE)
static constexpr bool kCompiledIn = true;
#else
static constexpr bool kCompiledIn = false;
#endif

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool ok, const char* id, const char* what)
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %-4s %s\n", id, what);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s\n", id, what);
        }
    }

    std::string ReadFileText(const std::filesystem::path& in_path)
    {
        std::ifstream in(in_path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// Counts occurrences of a token, skipping `//`-commented lines: comments
    /// are where a forbidden call survives review ("we used to call X").
    size_t CountOccurrencesInCode(const std::string& in_text, const char* in_token)
    {
        size_t count = 0;
        size_t line_begin = 0;
        while (line_begin <= in_text.size())
        {
            size_t line_end = in_text.find('\n', line_begin);
            if (line_end == std::string::npos)
            {
                line_end = in_text.size();
            }
            std::string_view line(in_text.data() + line_begin, line_end - line_begin);

            size_t trimmed = line.find_first_not_of(" \t\r");
            const bool is_comment = (trimmed != std::string_view::npos) &&
                                    (line.size() - trimmed >= 2) &&
                                    (line[trimmed] == '/') && (line[trimmed + 1] == '/');

            if (!is_comment)
            {
                size_t at = 0;
                while ((at = line.find(in_token, at)) != std::string_view::npos)
                {
                    ++count;
                    at += std::strlen(in_token);
                }
            }

            if (line_end == in_text.size())
            {
                break;
            }
            line_begin = line_end + 1;
        }
        return count;
    }

    void SetEnvVar(const char* in_name, const char* in_value)
    {
#if defined(_WIN32)
        ::_putenv_s(in_name, in_value);
#else
        ::setenv(in_name, in_value, 1);
#endif
    }
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("\n=== WwiseIntegration smoke test ===\n\n");

    bool with_device = false;
    std::string bank_dir;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--with-device")
        {
            with_device = true;
        }
        else if (arg == "--bank-dir" && (i + 1) < argc)
        {
            bank_dir = argv[++i];
        }
    }

    const std::filesystem::path engine_root = ZENGINE_ENGINE_ROOT_DIR;
    const std::filesystem::path source_root = engine_root / "Source";

    // -------------------------------------------------------------------------
    // A. No fake support: what the class reports must match how it was built.
    // -------------------------------------------------------------------------
    std::printf("-- A. availability is reported truthfully --\n");

    Check(WwiseAudioSystem::IsCompiledIn() == kCompiledIn,
          "A1",
          kCompiledIn ? "IsCompiledIn() == true and ZENGINE_HAS_WWISE is defined"
                      : "IsCompiledIn() == false and ZENGINE_HAS_WWISE is not defined");

    WwiseAudioSystem wwise;
    const std::string backend_name = wwise.GetBackendName();
    Check(backend_name.find("Wwise") != std::string::npos, "A2", "GetBackendName() identifies the backend");
    Check(!wwise.IsActive(), "A3", "a freshly constructed backend is not active");

    if (kCompiledIn)
    {
        Check(backend_name.find("not active") != std::string::npos,
              "A4",
              "compiled in but not initialised => the name says 'not active'");
    }
    else
    {
        Check(backend_name.find("not compiled in") != std::string::npos,
              "A4",
              "not compiled in => the name says so (never claims a working backend)");

        // Every entry point must fail rather than pretending. This is the whole
        // point of compiling the class unconditionally.
        Check(!wwise.LoadBank("Init"), "A5", "LoadBank() fails when not compiled in");
        Check(wwise.PostEvent("Anything", 1) == WwiseAudioSystem::kInvalidPlayingId,
              "A6", "PostEvent() fails when not compiled in");
        Check(!wwise.RegisterGameObject(1, "obj"), "A7", "RegisterGameObject() fails when not compiled in");
        Check(!wwise.SetRTPC("Any", 1.0f), "A8", "SetRTPC() fails when not compiled in");
        Check(!wwise.SetState("Any", "Any"), "A9", "SetState() fails when not compiled in");
        Check(wwise.GetLoadedBankCount() == 0, "A10", "no banks are loaded");
    }

    // -------------------------------------------------------------------------
    // B. Wiring gate: the backend exists, but is it reachable?
    // -------------------------------------------------------------------------
    std::printf("\n-- B. wiring gate (source level) --\n");

    const std::string register_text = ReadFileText(source_root / "Runtime" / "RegisterRuntime.cpp");
    Check(!register_text.empty(), "B1", "RegisterRuntime.cpp is readable");

    // Order is load-bearing: AudioSystem::Initialize() asks Wwise whether it
    // owns the output. Register Wwise second and the answer is always "no",
    // with no error anywhere.
    const size_t wwise_reg = register_text.find("REGISTER_SYSTEM(WwiseAudioSystem)");
    const size_t audio_reg = register_text.find("REGISTER_SYSTEM(AudioSystem)");
    Check(wwise_reg != std::string::npos, "B2", "WwiseAudioSystem is registered");
    Check(wwise_reg != std::string::npos && audio_reg != std::string::npos && wwise_reg < audio_reg,
          "B3", "WwiseAudioSystem is registered before AudioSystem");

    const std::string audio_system_text = ReadFileText(source_root / "Runtime" / "Function" / "Audio" / "AudioSystem.cpp");
    Check(!audio_system_text.empty(), "B4", "AudioSystem.cpp is readable");
    Check(CountOccurrencesInCode(audio_system_text, "QueryWwiseOwnsOutput") >= 1,
          "B5", "AudioSystem asks whether Wwise owns the output");
    Check(CountOccurrencesInCode(audio_system_text, "GetBackendName") >= 2 &&
              audio_system_text.find("m_WwiseOwnsOutput") != std::string::npos,
          "B6", "AudioSystem::GetBackendName() reports Wwise instead of the null device");
    Check(CountOccurrencesInCode(audio_system_text, "WarnOrProceedForFilePlayback") >= 3,
          "B7", "file-clip playback is guarded (declaration + Play + PlayOneShot)");

    const std::string wwise_text = ReadFileText(source_root / "Runtime" / "Function" / "Audio" / "Wwise" / "WwiseAudioSystem.cpp");
    Check(!wwise_text.empty(), "B8", "WwiseAudioSystem.cpp is readable");

    // Z-up trap: leaving the floor plane at Wwise's default (XZ) mirrors every
    // 3D sound. No error, wrong panning.
    Check(CountOccurrencesInCode(wwise_text, "AkFloorPlane_XY") >= 1,
          "B9", "the floor plane is set for a Z-up world (AkFloorPlane_XY)");

    // AK::SoundEngine::SetBusVolume does not exist before Wwise 2021. Any call
    // to it means the code was written against a newer SDK and will not compile
    // (or, worse, was stubbed out).
    Check(CountOccurrencesInCode(wwise_text, "SetBusVolume") == 0,
          "B10", "no SetBusVolume call (absent from Wwise 2019.2; buses go through RTPCs)");

    const std::string runtime_cmake = ReadFileText(engine_root / "Source" / "Runtime" / "CMakeLists.txt");
    Check(runtime_cmake.find("if(TARGET WwiseSDK)") != std::string::npos,
          "B11", "Runtime links Wwise only when the SDK target exists");

    // -------------------------------------------------------------------------
    // D. AudioSourceComponent can actually reach Wwise.
    //
    // B组 can be fully green and every emitter in the game still silent. The two
    // backends address content in incompatible ways -- the internal one decodes
    // a *file*, Wwise plays a *named event* out of a loaded bank -- so a component
    // that only knows about clip paths has nothing to play the moment Wwise owns
    // the output. Nothing throws: Play() returns "no route" and the game simply
    // has no sound.
    //
    // Two halves, because either can be true on its own:
    //   D1..D7  the route decision itself, as a pure function -- no engine, no
    //           device, no bank, and no Wwise-linked build needed.
    //   D8..    the component really routes through it and really has a Wwise
    //           branch, rather than a helper nobody calls.
    // -------------------------------------------------------------------------
    std::printf("\n-- D. AudioSourceComponent routing --\n");

    Check(ResolveAudioPlayRoute(false, "asset/audio/foo.wav", "") == AudioPlayRoute::kClipFile,
          "D1", "internal backend + clip path -> clip route");
    Check(ResolveAudioPlayRoute(false, "", "Play_Foo") == AudioPlayRoute::kNone,
          "D2", "internal backend ignores a Wwise event (there is no clip to decode)");
    Check(ResolveAudioPlayRoute(false, "", "") == AudioPlayRoute::kNone,
          "D3", "internal backend + nothing configured -> no route");
    Check(ResolveAudioPlayRoute(true, "asset/audio/foo.wav", "Play_Foo") == AudioPlayRoute::kWwiseEvent,
          "D4", "Wwise backend prefers the event even when a clip path is also filled in");
    Check(ResolveAudioPlayRoute(true, "asset/audio/foo.wav", "") == AudioPlayRoute::kNone,
          "D5", "Wwise backend + no event -> no route; a clip path is NOT a fallback");
    Check(ResolveAudioPlayRoute(true, "", "Play_Foo") == AudioPlayRoute::kWwiseEvent,
          "D6", "Wwise backend + event only -> event route");
    Check(ResolveAudioPlayRoute(false, "asset/audio/foo.wav", "Play_Foo") == AudioPlayRoute::kClipFile &&
              ResolveAudioPlayRoute(true, "asset/audio/foo.wav", "Play_Foo") == AudioPlayRoute::kWwiseEvent,
          "D7", "which backend is active decides the route -- not which field happens to be filled");

    const std::string asc_text =
        ReadFileText(source_root / "Runtime" / "Function" / "Framework" / "Component" / "Audio" / "AudioSourceComponent.cpp");
    Check(!asc_text.empty(), "D8", "AudioSourceComponent.cpp is readable");
    // A helper nobody calls is the exact shape of "implemented but not wired".
    Check(CountOccurrencesInCode(asc_text, "ResolveAudioPlayRoute") >= 1,
          "D9", "AudioSourceComponent picks its route through ResolveAudioPlayRoute()");
    Check(CountOccurrencesInCode(asc_text, "PostEvent") >= 1,
          "D10", "the Wwise route really posts an event");
    // Definition + the post-time push + the per-frame refresh. Fewer than three
    // means a moving emitter keeps the position it was posted from, which sounds
    // like the sound is welded to a wall.
    Check(CountOccurrencesInCode(asc_text, "PushWwiseEmitterTransform") >= 3,
          "D11", "the emitter transform is pushed at post time and refreshed while playing");
    Check(CountOccurrencesInCode(asc_text, "StopPlayingId") >= 1,
          "D12", "Stop() releases the Wwise playing id, not only the mixer voice");
    Check(CountOccurrencesInCode(asc_text, "IsPlayingId") >= 2,
          "D13", "IsPlaying()/Tick() ask Wwise whether the event is really still going");

    const std::string asc_header =
        ReadFileText(source_root / "Runtime" / "Function" / "Framework" / "Component" / "Audio" / "AudioSourceComponent.h");
    Check(!asc_header.empty(), "D14", "AudioSourceComponent.h is readable");
    Check(asc_header.find("m_WwiseEvent") != std::string::npos,
          "D15", "the Wwise event is its own serialized field, not derived from the clip path");

    // IsPlayingId() has no Wwise query to answer from in 2019.2 -- without the
    // end-of-event notification it could only ever report "posted once", which
    // keeps a finished one-shot "playing" forever.
    Check(CountOccurrencesInCode(wwise_text, "AK_EndOfEvent") >= 1,
          "D16", "PostEvent asks for end-of-event so IsPlayingId() can be honest");

    // -------------------------------------------------------------------------
    // C. Real sound-engine run.
    // -------------------------------------------------------------------------
    std::printf("\n-- C. live sound engine --\n");

    if (!with_device)
    {
        std::printf("[SKIP] C1..C12 skipped: pass --with-device --bank-dir <GeneratedSoundBanks/Windows>\n");
        std::printf("       (12 checks need a Wwise-linked build, a bank directory and an audio device)\n");
    }
    else if (!kCompiledIn)
    {
        // Loud, not skipped: --with-device on a build without Wwise is a real
        // misconfiguration, and silently passing it is how it reaches CI.
        Check(false, "C1", "--with-device requires a build configured with -DZENGINE_WWISE_SDK_DIR=...");
    }
    else
    {
        Check(!bank_dir.empty(), "C1", "--bank-dir was given");
        if (bank_dir.empty())
        {
            std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
            return 1;
        }

        SetEnvVar("ZENGINE_WWISE_ENABLE", "1");
        SetEnvVar("ZENGINE_WWISE_BANK_DIR", bank_dir.c_str());

        auto& registry = SystemRegistry::GetInstance();
        registry.RegisterSystem(new WwiseAudioSystem());
        registry.InitializeAll(false);

        WwiseAudioSystem* live = GET_SYSTEM(WwiseAudioSystem);
        Check(live != nullptr, "C2", "WwiseAudioSystem is reachable from the registry");
        Check(live != nullptr && live->IsActive(), "C3", "the Wwise sound engine initialised");

        if (live != nullptr && live->IsActive())
        {
            std::error_code bank_dir_error;
            Check(std::filesystem::is_directory(std::filesystem::path(bank_dir), bank_dir_error), "C4", "the bank directory exists");
            Check(live->LoadBank("Init"), "C5", "Init.bnk loads");
            Check(live->GetLoadedBankCount() == 1, "C6", "exactly one bank is loaded");

            const uint64_t emitter = 100u;
            Check(live->RegisterGameObject(emitter, "SmokeEmitter"), "C7", "a game object registers");
            Check(live->SetGameObjectTransform(emitter, AudioVec3(1.0f, 2.0f, 3.0f), AudioVec3(0.0f, -1.0f, 0.0f), AudioVec3(0.0f, 0.0f, 1.0f)),
                  "C8", "a game object position is accepted");
            Check(live->SetListener(AudioListenerData{}), "C9", "the listener transform is accepted");

            // "Play" exists in the demo banks; an unknown event must return
            // kInvalidPlayingId rather than pretending to have played.
            Check(live->PostEvent("ThisEventDoesNotExist", emitter) == WwiseAudioSystem::kInvalidPlayingId,
                  "C10", "an unknown event reports failure instead of faking it");

            live->RenderAudio();
            Check(true, "C11", "RenderAudio() pumps without crashing");

            live->StopAll();
            Check(live->UnloadBank("Init"), "C12", "Init.bnk unloads");
        }

        registry.ShutdownAll();
    }

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
