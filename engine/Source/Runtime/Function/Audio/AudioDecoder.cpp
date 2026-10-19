#include "Runtime/Function/Audio/AudioDecoder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace
{
    constexpr uint16_t kWaveFormatPcm = 0x0001u;
    constexpr uint16_t kWaveFormatIeeeFloat = 0x0003u;
    constexpr uint16_t kWaveFormatExtensible = 0xFFFEu;

    uint16_t ReadU16LE(const uint8_t* in_data)
    {
        return static_cast<uint16_t>(in_data[0] | (in_data[1] << 8));
    }

    uint32_t ReadU32LE(const uint8_t* in_data)
    {
        return static_cast<uint32_t>(in_data[0]) | (static_cast<uint32_t>(in_data[1]) << 8) |
               (static_cast<uint32_t>(in_data[2]) << 16) | (static_cast<uint32_t>(in_data[3]) << 24);
    }

    bool MatchesTag(const uint8_t* in_data, const char* in_tag)
    {
        return std::memcmp(in_data, in_tag, 4) == 0;
    }

    void WriteU16LE(uint8_t* out_data, uint16_t in_value)
    {
        out_data[0] = static_cast<uint8_t>(in_value & 0xFFu);
        out_data[1] = static_cast<uint8_t>((in_value >> 8) & 0xFFu);
    }

    void WriteU32LE(uint8_t* out_data, uint32_t in_value)
    {
        for (int byte_index = 0; byte_index < 4; ++byte_index)
        {
            out_data[byte_index] = static_cast<uint8_t>((in_value >> (8 * byte_index)) & 0xFFu);
        }
    }

    /// Convert one source sample to normalised float.
    float ConvertSample(const uint8_t* in_data, uint16_t in_format, uint16_t in_bits_per_sample)
    {
        if (in_format == kWaveFormatIeeeFloat)
        {
            if (in_bits_per_sample == 32)
            {
                float value = 0.0f;
                std::memcpy(&value, in_data, sizeof(float));
                return value;
            }
            if (in_bits_per_sample == 64)
            {
                double value = 0.0;
                std::memcpy(&value, in_data, sizeof(double));
                return static_cast<float>(value);
            }
            return 0.0f;
        }

        // Integer PCM.
        switch (in_bits_per_sample)
        {
            case 8:
                // 8-bit WAV is unsigned with a 128 bias.
                return (static_cast<float>(in_data[0]) - 128.0f) / 128.0f;
            case 16:
            {
                const int16_t raw = static_cast<int16_t>(ReadU16LE(in_data));
                return static_cast<float>(raw) / 32768.0f;
            }
            case 24:
            {
                const uint32_t raw = static_cast<uint32_t>(in_data[0]) | (static_cast<uint32_t>(in_data[1]) << 8) |
                                     (static_cast<uint32_t>(in_data[2]) << 16);
                // Sign-extend 24 -> 32.
                const int32_t signed_raw = (raw & 0x800000u) ? static_cast<int32_t>(raw) - 0x1000000 : static_cast<int32_t>(raw);
                return static_cast<float>(signed_raw) / 8388608.0f;
            }
            case 32:
            {
                int32_t raw = 0;
                std::memcpy(&raw, in_data, sizeof(int32_t));
                return static_cast<float>(raw) / 2147483648.0f;
            }
            default:
                return 0.0f;
        }
    }
}  // namespace

bool DecodeWav(const uint8_t* in_data, size_t in_size, AudioClipData& out_clip, std::string* out_error)
{
    out_clip = AudioClipData();

    if (in_data == nullptr || in_size < 44u)
    {
        if (out_error)
        {
            *out_error = "WAV: buffer too small";
        }
        return false;
    }

    if (!MatchesTag(in_data, "RIFF") || !MatchesTag(in_data + 8, "WAVE"))
    {
        if (out_error)
        {
            *out_error = "WAV: not a RIFF/WAVE file";
        }
        return false;
    }

    bool found_fmt = false;
    uint16_t audio_format = 0u;
    uint16_t channels = 0u;
    uint32_t sample_rate = 0u;
    uint16_t bits_per_sample = 0u;
    uint16_t block_align = 0u;

    const uint8_t* data_chunk = nullptr;
    uint32_t data_size = 0u;

    size_t cursor = 12u;
    while (cursor + 8u <= in_size)
    {
        const uint8_t* chunk_header = in_data + cursor;
        const uint32_t chunk_size = ReadU32LE(chunk_header + 4);
        const uint8_t* chunk_body = chunk_header + 8;
        const size_t body_offset = cursor + 8u;

        // A chunk may claim more bytes than the file actually has (truncated
        // writes); clamp so we never read past the buffer.
        const uint32_t usable_size = static_cast<uint32_t>(std::min<size_t>(chunk_size, in_size - body_offset));

        if (MatchesTag(chunk_header, "fmt "))
        {
            if (usable_size < 16u)
            {
                if (out_error)
                {
                    *out_error = "WAV: fmt chunk too small";
                }
                return false;
            }

            audio_format = ReadU16LE(chunk_body);
            channels = ReadU16LE(chunk_body + 2);
            sample_rate = ReadU32LE(chunk_body + 4);
            block_align = ReadU16LE(chunk_body + 12);
            bits_per_sample = ReadU16LE(chunk_body + 14);

            if (audio_format == kWaveFormatExtensible && usable_size >= 40u)
            {
                // SubFormat GUID starts with the real format tag.
                audio_format = ReadU16LE(chunk_body + 24);
                const uint16_t valid_bits = ReadU16LE(chunk_body + 18);
                if (valid_bits != 0u)
                {
                    bits_per_sample = valid_bits;
                }
            }
            found_fmt = true;
        }
        else if (MatchesTag(chunk_header, "data"))
        {
            data_chunk = chunk_body;
            data_size = usable_size;
        }

        // Advance past this chunk: 8 header bytes + body, word-aligned (odd
        // sizes are followed by a pad byte). Forgetting the 8 header bytes here
        // walks into the middle of the fmt body and every later chunk is lost.
        const size_t advance = 8u + static_cast<size_t>(chunk_size) + (chunk_size % 2u);
        if (advance >= in_size)
        {
            break;  // Last chunk, or a size that would run past the buffer.
        }
        cursor += advance;
    }

    if (!found_fmt)
    {
        if (out_error)
        {
            *out_error = "WAV: missing fmt chunk";
        }
        return false;
    }

    if (audio_format != kWaveFormatPcm && audio_format != kWaveFormatIeeeFloat)
    {
        if (out_error)
        {
            *out_error = "WAV: unsupported format tag " + std::to_string(audio_format);
        }
        return false;
    }

    if (channels == 0u || sample_rate == 0u || bits_per_sample == 0u || (bits_per_sample % 8u) != 0u)
    {
        if (out_error)
        {
            *out_error = "WAV: invalid fmt parameters";
        }
        return false;
    }

    if (data_chunk == nullptr || data_size == 0u)
    {
        if (out_error)
        {
            *out_error = "WAV: missing data chunk";
        }
        return false;
    }

    const uint32_t bytes_per_sample = bits_per_sample / 8u;
    const uint32_t natural_frame_size = bytes_per_sample * channels;
    // Trust block_align only when it is self-consistent; corrupt exporters are
    // common enough that falling back silently is better than decoding garbage.
    const uint32_t frame_size = (block_align >= natural_frame_size && (block_align % channels) == 0u) ? block_align
                                                                                                     : natural_frame_size;

    const uint32_t frame_count = data_size / frame_size;
    if (frame_count == 0u)
    {
        if (out_error)
        {
            *out_error = "WAV: no complete frames";
        }
        return false;
    }

    out_clip.m_SampleRate = sample_rate;
    out_clip.m_Channels = channels;
    out_clip.m_Samples.resize(static_cast<size_t>(frame_count) * channels);

    for (uint32_t frame = 0u; frame < frame_count; ++frame)
    {
        const uint8_t* frame_start = data_chunk + (static_cast<size_t>(frame) * frame_size);
        float* out_frame = out_clip.m_Samples.data() + (static_cast<size_t>(frame) * channels);
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            out_frame[channel] = ConvertSample(frame_start + (channel * bytes_per_sample), audio_format, bits_per_sample);
        }
    }

    return out_clip.IsValid();
}

bool DecodeAudioBuffer(const std::string& in_extension, const uint8_t* in_data, size_t in_size, AudioClipData& out_clip, std::string* out_error)
{
    std::string extension = in_extension;
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char in_char) {
        return static_cast<char>(std::tolower(in_char));
    });

    if (extension == ".wav" || extension == ".wave")
    {
        return DecodeWav(in_data, in_size, out_clip, out_error);
    }

    if (out_error)
    {
        *out_error = "Audio: unsupported format '" + extension + "' (V1 supports .wav only)";
    }
    return false;
}

bool DecodeAudioFile(const std::string& in_path, AudioClipData& out_clip, std::string* out_error)
{
    std::ifstream file(in_path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        if (out_error)
        {
            *out_error = "Audio: cannot open '" + in_path + "'";
        }
        return false;
    }

    const std::streamoff file_size = file.tellg();
    if (file_size <= 0)
    {
        if (out_error)
        {
            *out_error = "Audio: '" + in_path + "' is empty";
        }
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    const size_t dot_position = in_path.find_last_of('.');
    const std::string extension = (dot_position == std::string::npos) ? std::string() : in_path.substr(dot_position);

    if (!DecodeAudioBuffer(extension, buffer.data(), buffer.size(), out_clip, out_error))
    {
        return false;
    }

    const size_t slash_position = in_path.find_last_of("/\\");
    out_clip.m_Name = (slash_position == std::string::npos) ? in_path : in_path.substr(slash_position + 1u);
    return true;
}

bool EncodeWavS16(const AudioClipData& in_clip, std::vector<uint8_t>& out_data, std::string* out_error)
{
    if (!in_clip.IsValid())
    {
        if (out_error)
        {
            *out_error = "WAV: cannot encode an invalid clip";
        }
        return false;
    }

    const uint64_t frame_count = in_clip.GetFrameCount();
    const uint32_t channels = in_clip.m_Channels;
    const uint32_t bytes_per_sample = 2u;
    const uint32_t frame_size = bytes_per_sample * channels;
    const uint64_t data_bytes = frame_count * frame_size;
    if (data_bytes > static_cast<uint64_t>(0xFFFFFFFFu) - 44u)
    {
        if (out_error)
        {
            *out_error = "WAV: clip too large for a single RIFF chunk";
        }
        return false;
    }

    const uint32_t data_size = static_cast<uint32_t>(data_bytes);
    out_data.assign(44u + data_size, 0u);

    uint8_t* header = out_data.data();
    std::memcpy(header, "RIFF", 4);
    WriteU32LE(header + 4, 36u + data_size);
    std::memcpy(header + 8, "WAVE", 4);

    std::memcpy(header + 12, "fmt ", 4);
    WriteU32LE(header + 16, 16u);
    WriteU16LE(header + 20, kWaveFormatPcm);
    WriteU16LE(header + 22, static_cast<uint16_t>(channels));
    WriteU32LE(header + 24, in_clip.m_SampleRate);
    WriteU32LE(header + 28, in_clip.m_SampleRate * frame_size);
    WriteU16LE(header + 32, static_cast<uint16_t>(frame_size));
    WriteU16LE(header + 34, 16u);

    std::memcpy(header + 36, "data", 4);
    WriteU32LE(header + 40, data_size);

    uint8_t* samples = out_data.data() + 44u;
    for (uint64_t index = 0u; index < in_clip.m_Samples.size(); ++index)
    {
        const float clamped = ClampAudioFloat(in_clip.m_Samples[static_cast<size_t>(index)], -1.0f, 1.0f);
        const int16_t raw = static_cast<int16_t>(clamped * 32767.0f);
        WriteU16LE(samples + (static_cast<size_t>(index) * sizeof(int16_t)), static_cast<uint16_t>(raw));
    }

    return true;
}
