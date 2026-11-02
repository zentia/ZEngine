"""Mutation harness for the AudioSource slice (AW / AV checks).

Same contract as the other .mutate_*.py: every mutation must make the smoke
test go red. Restore ends with `touch` -- a fast mutate/build/restore round
trip can otherwise leave MSBuild believing a TU is up to date, and the
resulting crash has nothing to do with the mutation (see .mutate_pm.py).

Cost note
---------
Mutations in `AudioSourceComponent.cpp` / `AudioMixer.cpp` are cheap (one TU
+ link). The single mutation in `AudioSourceComponent.h` is NOT: ZHeaderTool
rewrites every generated header, so all of ZRuntime recompiles (~1h). It is
worth it once, because the AV wiring gate greps that header and a gate that
has never been seen to fail has not been shown to work.

Run a subset with `python .mutate_au.py M1`.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_SRC_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Audio\AudioSourceComponent.cpp")
F_SRC_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Audio\AudioSourceComponent.h")
F_MIX_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Audio\AudioMixer.cpp")
EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


def mutate(path, old, new):
    text = io.open(path, encoding="utf-8").read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def build_and_run():
    p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0 and ("LNK1236" in p.stdout or "LNK1000" in p.stdout or "LNK1314" in p.stdout):
        subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
        p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                           cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in p.stdout.splitlines()[-6:]:
            print("    " + line)
        return "build-failed"
    r = subprocess.run([os.path.join(ROOT, EXE)], cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=300)
    fails = [l for l in r.stdout.splitlines() if "FAIL" in l]
    tail = [l for l in r.stdout.splitlines() if "checks," in l]
    print("  rc=%d  %s" % (r.returncode, tail[-1] if tail else "(crashed -- no summary)"))
    for l in fails[:8]:
        print("    " + l.strip())
    if r.returncode == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # ---- AudioSourceComponent.cpp ----------------------------------------

    ("M1 mute is dropped from the play params", F_SRC_CPP,
     "        params.m_Volume = m_Mute ? 0.0f : m_Volume;",
     "        params.m_Volume = m_Volume;"),

    ("M2 the 2D early return in Tick is back", F_SRC_CPP,
     "        audio_system->UpdateVoice(m_VoiceId, BuildPlayParams());",
     "        if (m_SpatialBlend <= 0.0f) { return; }\n"
     "        audio_system->UpdateVoice(m_VoiceId, BuildPlayParams());"),

    ("M3 Tick forgets a paused voice is still alive", F_SRC_CPP,
     "                                 (audio_system->IsPlaying(m_VoiceId) || audio_system->IsVoicePaused(m_VoiceId));",
     "                                 (audio_system->IsPlaying(m_VoiceId));"),

    ("M4 PlayWithDelay drops the delay on the floor", F_SRC_CPP,
     "        params.m_DelaySeconds = in_delay_seconds;",
     "        params.m_DelaySeconds = 0.0f;"),

    ("M5 GetClipLength ignores pitch", F_SRC_CPP,
     "        const float pitch = (m_Pitch > 0.0f) ? m_Pitch : 1.0f;\n"
     "        return clip->GetDuration() / pitch;",
     "        const float pitch = (m_Pitch > 0.0f) ? m_Pitch : 1.0f;\n"
     "        (void)pitch;\n"
     "        return clip->GetDuration();"),

    ("M6 GetTime hands the -1 sentinel to script", F_SRC_CPP,
     "        return (seconds > 0.0) ? static_cast<float>(seconds) : 0.0f;",
     "        return static_cast<float>(seconds);"),

    ("M7 IsPaused never reports paused", F_SRC_CPP,
     "        return (audio_system != nullptr) && (m_VoiceId != kInvalidAudioVoiceId) && audio_system->IsVoicePaused(m_VoiceId);",
     "        return false;"),

    ("M8 Pause() resumes instead of pausing", F_SRC_CPP,
     "    void AudioSourceComponent::Pause()\n    {\n        SetPaused(true);",
     "    void AudioSourceComponent::Pause()\n    {\n        SetPaused(false);"),

    ("M9 Play() does not release the previous voice", F_SRC_CPP,
     "        // One voice per source on whichever route is live: stop first, then start.\n        Stop();",
     "        // One voice per source on whichever route is live: stop first, then start.\n        (void)0;"),

    ("M10 PlayOneShot inherits loop", F_SRC_CPP,
     "        params.m_Loop = false;",
     "        params.m_Loop = true;"),

    ("M11 Stop() leaves a pending Wwise delay queued", F_SRC_CPP,
     "        m_HasWwisePendingDelay = false;\n        m_WwisePendingDelay = 0.0f;\n\n        // One route owns this source",
     "        // One route owns this source"),

    # ---- AudioMixer.cpp ---------------------------------------------------

    ("M12 Render plays while the delay is still counting down", F_MIX_CPP,
     "            voice.m_DelayRemaining -= static_cast<float>(in_frame_count) / static_cast<float>(in_sample_rate);\n"
     "            ++voice_index;\n"
     "            continue;",
     "            voice.m_DelayRemaining -= static_cast<float>(in_frame_count) / static_cast<float>(in_sample_rate);"),

    ("M13 seeking clamps to the frame count (reads past the end)", F_MIX_CPP,
     "    const double last_playable_frame = (source_frames > 0.0) ? (source_frames - 1.0) : 0.0;",
     "    const double last_playable_frame = source_frames;"),

    ("M14 the mixer duration ignores pitch", F_MIX_CPP,
     "    const double pitch = (voice->m_Params.m_Pitch > 0.0f) ? static_cast<double>(voice->m_Params.m_Pitch) : 1.0;\n"
     "    return voice->m_Clip->GetDuration() / pitch;",
     "    const double pitch = (voice->m_Params.m_Pitch > 0.0f) ? static_cast<double>(voice->m_Params.m_Pitch) : 1.0;\n"
     "    (void)pitch;\n"
     "    return voice->m_Clip->GetDuration();"),

    ("M15 a dead voice reports 0 instead of -1", F_MIX_CPP,
     "    const Voice* voice = FindVoiceLocked(in_voice);\n    if (voice == nullptr)\n    {\n"
     "        return kUnknownPlaybackTime;\n    }\n    return VoicePlaybackSecondsLocked(*voice);",
     "    const Voice* voice = FindVoiceLocked(in_voice);\n    if (voice == nullptr)\n    {\n"
     "        return 0.0;\n    }\n    return VoicePlaybackSecondsLocked(*voice);"),

    ("M16 seeking does not clamp negatives", F_MIX_CPP,
     "    if (frames < 0.0)\n    {\n        frames = 0.0;\n    }\n",
     ""),

    ("M17 Play() never seeds the pending delay", F_MIX_CPP,
     "    voice.m_DelayRemaining = (in_params.m_DelaySeconds > 0.0f) ? in_params.m_DelaySeconds : 0.0f;",
     "    voice.m_DelayRemaining = 0.0f;"),

    # ---- AudioSourceComponent.h (EXPENSIVE: ZHeaderTool -> full rebuild) ---

    ("M18 mute is no longer a serialized field", F_SRC_H,
     "        ZPROPERTY(meta=(Tooltip=\"Silences the source without changing its volume.\"))\n"
     "        bool m_Mute {false};",
     "        bool m_Mute {false};"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".aubak")
        shutil.copy2(path, bak)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
            os.utime(path, None)
            touched.add(path)
        print()
    for path in touched:
        os.utime(path, None)
    print("---- summary ----")
    for n, r in results:
        print("%-62s %s" % (n, r))


if __name__ == "__main__":
    main()
