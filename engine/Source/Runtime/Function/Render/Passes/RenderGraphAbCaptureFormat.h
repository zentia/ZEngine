#pragma once

// =====================================================================================
// RenderGraphAbCaptureFormat -- the on-disk format of the render-graph A/B capture
// (DX12RenderGraphAbCapture) and the helpers that produce it.
// -------------------------------------------------------------------------------------
// Header-only, standard library only, so the writer can be exercised headless (see
// scripts/rg_ab_selftest.sh) and the reader (scripts/rg_ab_compare.py) is checked
// against the exact bytes the engine writes.
//
// .zcap layout (little endian):
//   offset  size  field
//   0       4     magic "ZCAP"
//   4       4     version (= 1)
//   8       4     width
//   12      4     height
//   16      4     bytes per pixel
//   20      32    format name, NUL padded (e.g. "R16G16B16A16_SFLOAT", "R8G8B8A8_UNORM")
//   52      ...   height rows of width * bpp bytes, TIGHTLY packed (no GPU row pitch)
// =====================================================================================

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace RenderGraphAbCapture
{
    constexpr uint32_t kZcapVersion = 1;
    constexpr size_t kZcapFormatNameBytes = 32;
    constexpr size_t kZcapHeaderBytes = 4 + 4 * 4 + kZcapFormatNameBytes;  // 52

    /// D3D12 places every row of a texture->buffer copy at a 256-byte aligned pitch
    /// (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); DX12RHI::CmdCopyImageToBuffer uses exactly
    /// AlignTo(width * bpp, 256), so the readback buffer must be sized and read with it.
    inline uint64_t Dx12ReadbackRowPitch(uint32_t width, uint32_t bytes_per_pixel)
    {
        const uint64_t tight = static_cast<uint64_t>(width) * bytes_per_pixel;
        return (tight + 255u) & ~static_cast<uint64_t>(255u);
    }

    inline void PutU32(std::vector<uint8_t>& out, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
        }
    }

    /// Serialises one capture. `src` holds `height` rows `src_row_pitch` bytes apart
    /// (the GPU readback layout); the file gets them tightly packed.
    inline std::vector<uint8_t> EncodeZcap(uint32_t width,
                                           uint32_t height,
                                           uint32_t bytes_per_pixel,
                                           const char* format_name,
                                           const uint8_t* src,
                                           uint64_t src_row_pitch)
    {
        std::vector<uint8_t> out;
        const uint64_t row_bytes = static_cast<uint64_t>(width) * bytes_per_pixel;
        out.reserve(kZcapHeaderBytes + static_cast<size_t>(row_bytes * height));
        out.push_back('Z');
        out.push_back('C');
        out.push_back('A');
        out.push_back('P');
        PutU32(out, kZcapVersion);
        PutU32(out, width);
        PutU32(out, height);
        PutU32(out, bytes_per_pixel);
        char name[kZcapFormatNameBytes] = {};
        if (format_name != nullptr)
        {
            std::strncpy(name, format_name, kZcapFormatNameBytes - 1);
        }
        out.insert(out.end(), name, name + kZcapFormatNameBytes);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = src + static_cast<size_t>(src_row_pitch * y);
            out.insert(out.end(), row, row + static_cast<size_t>(row_bytes));
        }
        return out;
    }

    inline bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error)
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr)
        {
            if (error != nullptr)
            {
                *error = "cannot open " + path + " for writing";
            }
            return false;
        }
        const size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
        const bool closed = std::fclose(file) == 0;
        if (written != bytes.size() || !closed)
        {
            if (error != nullptr)
            {
                *error = "short write to " + path;
            }
            return false;
        }
        return true;
    }

    /// One captured image, as listed in manifest.json.
    struct ManifestImage
    {
        std::string name;      // e.g. "rp1.handwritten.backup_odd"
        std::string file;      // file name relative to the manifest
        uint32_t width {0};
        uint32_t height {0};
        std::string format;
        bool written {false};
        std::string error;     // non-empty when the readback or the write failed
    };

    /// Which stage ran which path. `graph_used == false` for a "graph" run means the
    /// graph rejected the frame and the hand-written chain drew it instead -- then the
    /// two images are trivially identical and the comparison proves nothing, so the
    /// compare script treats that as a failure, not a pass.
    struct ManifestStage
    {
        std::string stage;     // "rp1" / "rp2"
        bool attempted {false};
        bool graph_used {false};
    };

    inline std::string JsonEscape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size() + 2);
        for (const char c : text)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                        out += buffer;
                    }
                    else
                    {
                        out += c;
                    }
            }
        }
        return out;
    }

    inline std::string EncodeManifest(uint64_t frame,
                                      const std::vector<ManifestStage>& stages,
                                      const std::vector<ManifestImage>& images,
                                      const std::vector<std::string>& notes)
    {
        std::string json = "{\n  \"format\": \"zengine-rg-ab/1\",\n";
        json += "  \"frame\": " + std::to_string(frame) + ",\n  \"stages\": [";
        for (size_t i = 0; i < stages.size(); ++i)
        {
            const ManifestStage& s = stages[i];
            json += std::string(i == 0 ? "\n" : ",\n") + "    {\"stage\": \"" + JsonEscape(s.stage) +
                    "\", \"attempted\": " + (s.attempted ? "true" : "false") +
                    ", \"graph_used\": " + (s.graph_used ? "true" : "false") + "}";
        }
        json += "\n  ],\n  \"images\": [";
        for (size_t i = 0; i < images.size(); ++i)
        {
            const ManifestImage& m = images[i];
            json += std::string(i == 0 ? "\n" : ",\n") + "    {\"name\": \"" + JsonEscape(m.name) + "\", \"file\": \"" +
                    JsonEscape(m.file) + "\", \"width\": " + std::to_string(m.width) +
                    ", \"height\": " + std::to_string(m.height) + ", \"format\": \"" + JsonEscape(m.format) +
                    "\", \"written\": " + (m.written ? "true" : "false") + ", \"error\": \"" +
                    JsonEscape(m.error) + "\"}";
        }
        json += "\n  ],\n  \"notes\": [";
        for (size_t i = 0; i < notes.size(); ++i)
        {
            json += std::string(i == 0 ? "\n" : ",\n") + "    \"" + JsonEscape(notes[i]) + "\"";
        }
        json += "\n  ]\n}\n";
        return json;
    }
}  // namespace RenderGraphAbCapture
