#pragma once

#include "Runtime/Function/Audio/AudioCommon.h"

#include <cstddef>
#include <string>

// =============================================================================
// Audio decoding (V1: WAV / PCM).
//
// The decoder is intentionally a free function set rather than an IAssetImporter
// subclass: it has no dependency on Object / Transfer / AssetManager, so the
// mixer smoke test can feed it synthetic buffers. The engine-facing layer that
// turns a project path into an AudioClipData is AudioSystem::GetClip().
//
// Compressed formats (OGG/Vorbis, MP3) are the obvious next step; they need a
// vendored decoder (stb_vorbis / dr_mp3) which is not in engine/3rdparty yet,
// so V1 reports them as unsupported instead of silently playing nothing.
// =============================================================================

/// Decode a WAV (RIFF) buffer into interleaved float32.
///
/// Supports PCM 8/16/24/32-bit integer, 32/64-bit IEEE float, any channel
/// count, and WAVE_FORMAT_EXTENSIBLE (as long as the sub-format is one of the
/// above -- the channel mask is ignored, V1 has no speaker-matrix routing).
bool DecodeWav(const uint8_t* in_data, size_t in_size, AudioClipData& out_clip, std::string* out_error = nullptr);

/// Extension dispatch. Returns false and fills out_error for anything but .wav.
bool DecodeAudioBuffer(const std::string& in_extension, const uint8_t* in_data, size_t in_size, AudioClipData& out_clip, std::string* out_error = nullptr);

/// Read a file from disk and decode it. Convenience wrapper used by AudioSystem.
bool DecodeAudioFile(const std::string& in_path, AudioClipData& out_clip, std::string* out_error = nullptr);

/// Encode interleaved float32 back into a 16-bit PCM WAV.
///
/// Mostly a test helper (round-trip in AudioMixerSmokeTest) but also handy for
/// baking procedural clips to disk.
bool EncodeWavS16(const AudioClipData& in_clip, std::vector<uint8_t>& out_data, std::string* out_error = nullptr);
