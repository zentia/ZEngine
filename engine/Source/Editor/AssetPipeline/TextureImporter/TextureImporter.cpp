// =============================================================================
// TextureImporter
// -----------------------------------------------------------------------------
// Decodes a source image (.png / .jpg / .jpeg / .tga / .bmp / .dds) and writes
// a Texture2D `.zasset`.
//
// Route B history (see doc/BINDLESS_TEXTURE_PATH.md):
//   * Pre-route B, this importer wrote a `TextureData` POD via
//     `AssetFile::saveAsset`. That code path was *half-implemented* -- the
//     header was stamped but the actual pixel/metadata serialisation lines
//     were commented out, so every imported `.zasset` was a 176-byte stub
//     with no body. PR8c worked around this by feeding the inspector preview
//     directly from the source `.png`.
//
//   * Route B (this file) replaces that path with the engine's working
//     SerializedFile-based pipeline, the same one PrefabAsset / Material /
//     XlsxImporter use:
//
//         RHIImage <-- (lazy upload at preview time, owned by consumer)
//             ^
//         Texture2D (Object subclass, REGISTER_CLASS)
//             ^
//         AssetManager::WriteObjectToDiskThreadSafe(zasset_path, *texture)
//
//   * Reimport now decodes the source image fresh and overwrites the .zasset.
//     There is no metadata round-trip yet (no metadata persistence layer
//     exists -- the legacy `AssetFile::writeMetadata` was a no-op
//     return-true and has since been removed in P2 #9).
//     The reimport API takes the source path explicitly until a metadata
//     persistence layer lands in route A.
// =============================================================================

#include "TextureImporter.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Cache/DerivedDataCacheAccessor.h"
#include "TextureCompressor.h"
#include "TextureImporterSettings.h"

// Note: STB_IMAGE_IMPLEMENTATION is already defined in ZRuntime's render_resource_base.cpp
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <stb_image.h>
#include <unordered_set>
#include <vector>

namespace
{
    // Placeholder GUID generator. Returned in `out_metadata.guid` so the upstream
    // AssetImporter contract continues to compile, but ZEngine has no metadata
    // persistence layer wired up today (the legacy `AssetFile::writeMetadata`
    // was a no-op and has been removed; `AssetImporter::tryImport` drops the
    // metadata on the floor). Once a proper metadata persistence is added
    // under route A, this can either feed the metadata sidecar or be replaced
    // by a deterministic path-based hash; see
    // doc/BINDLESS_TEXTURE_PATH.md route-A backlog ("GUID story for SerializedFile
    // .zasset").
    std::string generateGUID()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        std::uniform_int_distribution<> dis2(8, 11);

        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; ++i)
            ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 4; ++i)
            ss << dis(gen);
        ss << "-4";
        for (int i = 0; i < 3; ++i)
            ss << dis(gen);
        ss << "-";
        ss << dis2(gen);
        for (int i = 0; i < 3; ++i)
            ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 12; ++i)
            ss << dis(gen);
        return ss.str();
    }

    // Map TextureImporterSettings::Format + sRGB flag to the RHIFormat ordinal we
    // stamp into Texture2D::m_Format. Must stay in sync with whichever format the
    // pixel decode produced -- we always force-decode to 4-channel RGBA8 below, so
    // any non-RGBA target is currently a TODO (left out per route B's "minimum
    // viable slice" scope).
    void downscaleRGBA8ToMaxSize(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, int max_size)
    {
        if (max_size <= 0 || pixels.empty() || width == 0 || height == 0)
        {
            return;
        }

        const uint32_t max_edge = std::max(width, height);
        if (max_edge <= static_cast<uint32_t>(max_size))
        {
            return;
        }

        const float scale = static_cast<float>(max_size) / static_cast<float>(max_edge);
        const uint32_t new_w = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(width) * scale)));
        const uint32_t new_h = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(height) * scale)));

        std::vector<uint8_t> resized(static_cast<size_t>(new_w) * static_cast<size_t>(new_h) * 4u);
        for (uint32_t y = 0; y < new_h; ++y)
        {
            for (uint32_t x = 0; x < new_w; ++x)
            {
                const float src_x = (static_cast<float>(x) + 0.5f) * static_cast<float>(width) / static_cast<float>(new_w) - 0.5f;
                const float src_y = (static_cast<float>(y) + 0.5f) * static_cast<float>(height) / static_cast<float>(new_h) - 0.5f;
                const int ix = std::clamp(static_cast<int>(std::lround(src_x)), 0, static_cast<int>(width) - 1);
                const int iy = std::clamp(static_cast<int>(std::lround(src_y)), 0, static_cast<int>(height) - 1);
                const size_t src_index =
                    (static_cast<size_t>(iy) * static_cast<size_t>(width) + static_cast<size_t>(ix)) * 4u;
                const size_t dst_index =
                    (static_cast<size_t>(y) * static_cast<size_t>(new_w) + static_cast<size_t>(x)) * 4u;
                resized[dst_index + 0] = pixels[src_index + 0];
                resized[dst_index + 1] = pixels[src_index + 1];
                resized[dst_index + 2] = pixels[src_index + 2];
                resized[dst_index + 3] = pixels[src_index + 3];
            }
        }

        pixels = std::move(resized);
        width = new_w;
        height = new_h;
    }

    // Map the import-settings format enum onto the TextureCompressor backend
    // family. BC* for desktop / WebGL; ASTC for mobile cooks (Android / iOS /
    // OHOS). RGB8 / RGBA16F decode to RGBA8 (stb force-4-channel) and pass
    // through uncompressed.
    TextureCompressor::Format mapToCompressorFormat(TextureImporterSettings::Format f)
    {
        switch (f)
        {
            case TextureImporterSettings::Format::BC7: return TextureCompressor::Format::BC7;
            case TextureImporterSettings::Format::BC1: return TextureCompressor::Format::BC1;
            case TextureImporterSettings::Format::BC3: return TextureCompressor::Format::BC3;
            case TextureImporterSettings::Format::ASTC4x4: return TextureCompressor::Format::ASTC_4x4;
            case TextureImporterSettings::Format::ASTC6x6: return TextureCompressor::Format::ASTC_6x6;
            case TextureImporterSettings::Format::ASTC8x8: return TextureCompressor::Format::ASTC_8x8;
            case TextureImporterSettings::Format::RGBA8:
            case TextureImporterSettings::Format::RGB8:
            case TextureImporterSettings::Format::RGBA16F:
            default:                                   return TextureCompressor::Format::RGBA8;
        }
    }

    // FNV-1a 64 over the cook-affecting settings fields. Folded into the DDC
    // cache key so a settings edit (format / mips / sRGB / max_size / quality)
    // invalidates the cached variant. Stable across runs and machines.
    uint64_t hashEffectiveSettings(const TextureImporterSettings::PlatformSettings& s)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) {
            for (int i = 0; i < 8; ++i)
            {
                h ^= (v & 0xFFull);
                h *= 1099511628211ull;
                v >>= 8;
            }
        };
        mix(static_cast<uint64_t>(s.format));
        mix(s.generate_mipmaps ? 1ull : 0ull);
        mix(s.sRGB ? 1ull : 0ull);
        mix(static_cast<uint64_t>(static_cast<uint32_t>(s.max_size)));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(s.compression_quality)));
        return h;
    }

    bool isSourceImageExtension(std::string ext)
    {
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
    }

    std::string normalisePathKey(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec)
        {
            canonical = std::filesystem::absolute(path, ec);
            if (ec)
            {
                canonical = path;
            }
        }
        return canonical.generic_string();
    }

    // Source path: SourceAssetRegistry first, then sibling image next to the
    // .zasset (pre-PR-AI3 imports that never recorded a registry entry).
    std::optional<std::filesystem::path> resolveSourcePathForZasset(
        const std::filesystem::path& zasset_abs,
        const SourceAssetRegistry* source_registry)
    {
        if (source_registry != nullptr)
        {
            if (const std::optional<SourceAssetRegistry::Entry> entry = source_registry->Lookup(zasset_abs))
            {
                if (!entry->source_path.empty())
                {
                    const std::filesystem::path src(entry->source_path);
                    std::error_code ec;
                    if (std::filesystem::exists(src, ec) && !ec)
                    {
                        return src;
                    }
                }
            }
        }

        static const char* kSiblingExts[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
        std::error_code ec;
        for (const char* ext : kSiblingExts)
        {
            std::filesystem::path candidate = zasset_abs;
            candidate.replace_extension(ext);
            if (std::filesystem::exists(candidate, ec) && !ec)
            {
                return candidate;
            }
        }
        return std::nullopt;
    }

    TextureImporterSettings::PlatformSettings effectiveSettingsForCook(
        const std::filesystem::path& zasset_abs,
        TextureImporterSettings::BuildTarget target,
        EditorAssetManager* editor_mgr,
        const TextureImporterSettings& defaults)
    {
        if (editor_mgr != nullptr)
        {
            if (const std::optional<TextureImporterSettings> stored =
                    editor_mgr->GetTextureImportSettingsRegistry().Lookup(zasset_abs))
            {
                return stored->GetEffective(target);
            }
        }
        return defaults.GetEffective(target);
    }

    // -------- DDC value blob (cooked Texture2D payload) ----------------------
    // Compact serialisation of a cooked variant for the Derived Data Cache:
    //   'TXDC' | u32 version | u32 width | u32 height | u32 rhi_format
    //          | u32 mipCount | mipCount*u32 offsets | pixels[...]
    // This is independent of the .zasset SerializedFile format on purpose -- the
    // DDC stores raw cook artifacts, the .zasset stores the engine Object.
    constexpr uint32_t kDdcBlobMagic = 0x43445854u;  // 'TXDC' little-endian
    constexpr uint32_t kDdcBlobVersion = 1u;

    void putU32(std::vector<uint8_t>& b, uint32_t v)
    {
        b.push_back(static_cast<uint8_t>(v & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }

    bool getU32(const std::vector<uint8_t>& b, size_t& off, uint32_t& out)
    {
        if (off + 4 > b.size())
            return false;
        out = static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
              (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
        off += 4;
        return true;
    }

    std::vector<uint8_t> packCookedBlob(const TextureCompressor::CompressedTexture& c)
    {
        std::vector<uint8_t> b;
        b.reserve(24 + c.mip_offsets.size() * 4 + c.pixels.size());
        putU32(b, kDdcBlobMagic);
        putU32(b, kDdcBlobVersion);
        putU32(b, c.width);
        putU32(b, c.height);
        putU32(b, c.rhi_format);
        putU32(b, static_cast<uint32_t>(c.mip_offsets.size()));
        for (uint32_t off : c.mip_offsets)
            putU32(b, off);
        b.insert(b.end(), c.pixels.begin(), c.pixels.end());
        return b;
    }

    bool unpackCookedBlob(const std::vector<uint8_t>& b, TextureCompressor::CompressedTexture& out)
    {
        size_t off = 0;
        uint32_t magic = 0, version = 0, mip_count = 0;
        if (!getU32(b, off, magic) || magic != kDdcBlobMagic)
            return false;
        if (!getU32(b, off, version) || version != kDdcBlobVersion)
            return false;
        if (!getU32(b, off, out.width) || !getU32(b, off, out.height) || !getU32(b, off, out.rhi_format))
            return false;
        if (!getU32(b, off, mip_count))
            return false;
        out.mip_offsets.resize(mip_count);
        for (uint32_t i = 0; i < mip_count; ++i)
        {
            if (!getU32(b, off, out.mip_offsets[i]))
                return false;
        }
        out.pixels.assign(b.begin() + static_cast<std::ptrdiff_t>(off), b.end());
        out.format = TextureCompressor::FromRhiFormatOrdinal(out.rhi_format);
        return true;
    }

    std::optional<std::filesystem::path> cookedOutputPathForZasset(const std::filesystem::path& zasset_abs,
                                                                   const std::filesystem::path& platform_root,
                                                                   const std::filesystem::path& content_root)
    {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(zasset_abs, content_root, ec);
        if (ec || rel.empty())
        {
            return std::nullopt;
        }
        return platform_root / rel;
    }

    bool writeCookedTextureObject(const TextureCompressor::CompressedTexture& cooked,
                                  const std::filesystem::path& out_path,
                                  const std::string& source_guid,
                                  AssetManager& asset_mgr)
    {
        auto* object_manager = GET_SYSTEM(ObjectManager).get();
        if (object_manager == nullptr)
        {
            return false;
        }
        Object* produced = object_manager->Produce(TypeOf<Texture2D>(), /*instanceID=*/0);
        if (produced == nullptr)
        {
            return false;
        }
        auto* texture = static_cast<Texture2D*>(produced);
        texture->m_Width = cooked.width;
        texture->m_Height = cooked.height;
        texture->m_Format = cooked.rhi_format;
        texture->m_Pixels = cooked.pixels;
        texture->m_MipOffsets = cooked.mip_offsets;

        const bool ok = asset_mgr.WriteObjectToDiskWithGuid(out_path, *texture, source_guid);
        MemoryManager::DestroyObject(produced);
        return ok;
    }

    // Reuse the editor-platform .zasset bytes when cooking for the same target
    // the editor imported with (Standalone on desktop). No source file required.
    bool copyEditorZassetToCooked(const std::filesystem::path& zasset_abs,
                                  const std::filesystem::path& out_path,
                                  const std::string& platform_tag)
    {
        std::error_code ec;
        if (!out_path.parent_path().empty())
        {
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }
        std::filesystem::copy_file(zasset_abs, out_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            LOG_ERROR(ZEditor,
                      "TextureImporter::CookProjectTextures: copy editor zasset failed {} -> {} ({})",
                      zasset_abs.generic_string(),
                      out_path.generic_string(),
                      ec.message());
            return false;
        }
        LOG_INFO(ZEditor,
                 "TextureImporter: cooked [{}] {} -> {} (editor-zasset-copy)",
                 platform_tag,
                 zasset_abs.generic_string(),
                 out_path.generic_string());
        return true;
    }

    // Cook one Texture2D .zasset to Intermediate/Cooked/<platform>/...
    // Input priority: DDC hit -> editor-zasset copy (same build target) -> encode from source file.
    bool cookTextureToPlatform(const std::filesystem::path& zasset_abs,
                               const std::optional<std::filesystem::path>& source_path,
                               const std::string& source_guid,
                               const TextureImporterSettings::PlatformSettings& effective,
                               TextureImporterSettings::BuildTarget cook_target,
                               const std::string& platform_tag,
                               const std::filesystem::path& platform_root,
                               const std::filesystem::path& content_root,
                               AssetManager& asset_mgr)
    {
        const std::optional<std::filesystem::path> out_path_opt =
            cookedOutputPathForZasset(zasset_abs, platform_root, content_root);
        if (!out_path_opt.has_value())
        {
            LOG_ERROR(ZEditor,
                      "TextureImporter::CookProjectTextures: cannot compute cooked path for {}",
                      zasset_abs.generic_string());
            return false;
        }
        const std::filesystem::path& out_path = *out_path_opt;

        std::error_code ec;
        if (!out_path.parent_path().empty())
        {
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }

        const uint64_t settings_hash = hashEffectiveSettings(effective);
        const std::string cache_key =
            Runtime::MakeDDCCacheKey(platform_tag, settings_hash, TextureCompressor::EncoderVersion());
        const Runtime::DDCKey ddc_key {"Texture", source_guid, cache_key};

        TextureCompressor::CompressedTexture cooked;
        bool cooked_from_cache = false;
        if (Runtime::IDerivedDataCache* ddc = Runtime::GetDerivedDataCache())
        {
            Runtime::DDCValue cached;
            if (ddc->get(ddc_key, cached) && unpackCookedBlob(cached.data, cooked))
            {
                cooked_from_cache = true;
            }
        }
        if (cooked_from_cache)
        {
            if (!writeCookedTextureObject(cooked, out_path, source_guid, asset_mgr))
            {
                LOG_ERROR(ZEditor, "TextureImporter::CookProjectTextures: write failed for {}", out_path.generic_string());
                return false;
            }
            LOG_INFO(ZEditor,
                     "TextureImporter: cooked [{}] {} -> {} ({}, {} mips, guid={}, ddc-hit)",
                     platform_tag,
                     zasset_abs.generic_string(),
                     out_path.generic_string(),
                     TextureCompressor::ToString(cooked.format),
                     cooked.mip_offsets.size(),
                     source_guid);
            return true;
        }

        if (cook_target == TextureImporterSettings::EditorPreviewBuildTarget())
        {
            return copyEditorZassetToCooked(zasset_abs, out_path, platform_tag);
        }

        if (!source_path.has_value())
        {
            LOG_WARNING(ZEditor,
                        "TextureImporter::CookProjectTextures: no source for {} (need source file, DDC, or "
                        "cook for editor platform)",
                        zasset_abs.generic_string());
            return false;
        }

        int width = 0, height = 0, channels = 0;
        unsigned char* image_data =
            stbi_load(source_path->generic_string().c_str(), &width, &height, &channels, 4);
        if (image_data == nullptr || width <= 0 || height <= 0)
        {
            LOG_ERROR(ZEditor, "TextureImporter::CookProjectTextures: decode failed for {}",
                      source_path->generic_string());
            if (image_data != nullptr)
            {
                stbi_image_free(image_data);
            }
            return false;
        }
        uint32_t out_width = static_cast<uint32_t>(width);
        uint32_t out_height = static_cast<uint32_t>(height);
        std::vector<uint8_t> pixel_blob(image_data,
                                        image_data + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        stbi_image_free(image_data);
        downscaleRGBA8ToMaxSize(pixel_blob, out_width, out_height, effective.max_size);

        TextureCompressor::Options cook_opts;
        cook_opts.format = mapToCompressorFormat(effective.format);
        cook_opts.generate_mips = effective.generate_mipmaps;
        cook_opts.srgb = effective.sRGB;

        if (!TextureCompressor::Compress(pixel_blob.data(), out_width, out_height, cook_opts, cooked))
        {
            LOG_ERROR(ZEditor, "TextureImporter::CookProjectTextures: encode failed for {}",
                      source_path->generic_string());
            return false;
        }
        if (Runtime::IDerivedDataCache* ddc = Runtime::GetDerivedDataCache())
        {
            Runtime::DDCValue value;
            value.data = packCookedBlob(cooked);
            value.timestamp = std::time(nullptr);
            value.version = TextureCompressor::EncoderVersion();
            ddc->Put(ddc_key, value);
        }

        if (!writeCookedTextureObject(cooked, out_path, source_guid, asset_mgr))
        {
            LOG_ERROR(ZEditor, "TextureImporter::CookProjectTextures: write failed for {}", out_path.generic_string());
            return false;
        }

        LOG_INFO(ZEditor,
                 "TextureImporter: cooked [{}] {} -> {} ({}, {} mips, guid={}, encoded)",
                 platform_tag,
                 source_path->generic_string(),
                 out_path.generic_string(),
                 TextureCompressor::ToString(cooked.format),
                 cooked.mip_offsets.size(),
                 source_guid);
        return true;
    }
}  // namespace

std::vector<std::string> TextureImporter::GetSupportedExtensions() const
{
    // NOTE: ".tag" in the legacy list was a typo for ".tga"; corrected here.
    // Adding .jpeg / .bmp because stb_image handles them and the project
    // window already shows them.
    return {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds"};
}

bool TextureImporter::Import(const std::filesystem::path& source_path,
                             const std::filesystem::path& output_path,
                             const AssetImporterSettings& import_settings,
                             AssetMetadata& out_metadata)
{
    if (!std::filesystem::exists(source_path))
    {
        LOG_ERROR(ZEditor, "TextureImporter: Source file does not exist: {}", source_path.string());
        return false;
    }

    // --- 1. Settings cast ---------------------------------------------------
    const TextureImporterSettings* texture_settings =
        dynamic_cast<const TextureImporterSettings*>(&import_settings);
    if (!texture_settings)
    {
        LOG_ERROR(ZEditor, "TextureImporter: Invalid import settings type");
        return false;
    }

    const TextureImporterSettings::PlatformSettings effective =
        texture_settings->GetEffective(TextureImporterSettings::EditorPreviewBuildTarget());

    // --- 2. Decode pixels to RGBA8 ------------------------------------------
    // stb_image's force-channels=4 path is the most reliable across .png/.jpg/
    // .tga/.bmp; .dds is unsupported by stb (TODO: dedicated DDS decode if
    // .dds shows up in real demo content -- right now nobody ships .dds).
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* image_data = stbi_load(source_path.generic_string().c_str(),
                                          &width,
                                          &height,
                                          &channels,
                                          4);
    if (!image_data || width <= 0 || height <= 0)
    {
        LOG_ERROR(ZEditor, "TextureImporter: Failed to load image: {}", source_path.string());
        if (image_data)
            stbi_image_free(image_data);
        return false;
    }

    uint32_t out_width = static_cast<uint32_t>(width);
    uint32_t out_height = static_cast<uint32_t>(height);
    std::vector<uint8_t> pixel_blob(image_data, image_data + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    stbi_image_free(image_data);
    image_data = nullptr;

    downscaleRGBA8ToMaxSize(pixel_blob, out_width, out_height, effective.max_size);

    // --- 2b. Cook: mip chain + block encode for the editor preview target ----
    // Resolve a stable asset GUID first (custom > settings-derived > random) so
    // the DDC key is reproducible across reimports of the same asset.
    std::string asset_guid;
    if (import_settings.generate_guid && !import_settings.custom_guid.empty())
    {
        asset_guid = import_settings.custom_guid;
    }
    else
    {
        asset_guid = generateGUID();
    }

    TextureCompressor::Options cook_opts;
    cook_opts.format = mapToCompressorFormat(effective.format);
    cook_opts.generate_mips = effective.generate_mipmaps;
    cook_opts.srgb = effective.sRGB;

    const std::string platform_tag =
        TextureImporterSettings::BuildTargetDisplayName(TextureImporterSettings::EditorPreviewBuildTarget());
    const uint64_t settings_hash = hashEffectiveSettings(effective);
    const std::string cache_key =
        Runtime::MakeDDCCacheKey(platform_tag, settings_hash, TextureCompressor::EncoderVersion());
    const Runtime::DDCKey ddc_key {"Texture", asset_guid, cache_key};

    TextureCompressor::CompressedTexture cooked;
    bool cooked_from_cache = false;

    // DDC fast path: reuse a previously cooked variant for this (guid, settings,
    // platform, encoder) tuple. A miss (or no project / closed cache) falls
    // through to a fresh encode below.
    if (Runtime::IDerivedDataCache* ddc = Runtime::GetDerivedDataCache())
    {
        Runtime::DDCValue cached;
        if (ddc->get(ddc_key, cached) && unpackCookedBlob(cached.data, cooked))
        {
            cooked_from_cache = true;
            LOG_INFO(ZEditor,
                     "TextureImporter: DDC hit for {} ({}, {} mips)",
                     source_path.generic_string(),
                     TextureCompressor::ToString(cooked.format),
                     cooked.mip_offsets.size());
        }
    }

    if (!cooked_from_cache)
    {
        if (!TextureCompressor::Compress(pixel_blob.data(), out_width, out_height, cook_opts, cooked))
        {
            LOG_ERROR(ZEditor, "TextureImporter: cook/encode failed for {}", source_path.generic_string());
            return false;
        }
        if (Runtime::IDerivedDataCache* ddc = Runtime::GetDerivedDataCache())
        {
            Runtime::DDCValue value;
            value.data = packCookedBlob(cooked);
            value.timestamp = std::time(nullptr);
            value.version = TextureCompressor::EncoderVersion();
            ddc->Put(ddc_key, value);
        }
    }

    // --- 3. Construct Texture2D and copy pixels in --------------------------
    // We MUST go through ObjectManager::Produce -- direct `new Texture2D` is
    // illegal because ObjectManager owns InstanceID assignment / lifetime
    // bookkeeping (mirrors xlsx_importer / project_window's Material
    // creation paths).
    auto* object_manager = GET_SYSTEM(ObjectManager).get();
    if (object_manager == nullptr)
    {
        LOG_ERROR(ZEditor, "TextureImporter: ObjectManager unavailable");
        return false;
    }

    Object* produced = object_manager->Produce(TypeOf<Texture2D>(), /*instanceID=*/0);
    if (produced == nullptr)
    {
        LOG_ERROR(ZEditor, "TextureImporter: Failed to allocate Texture2D");
        return false;
    }
    auto* texture = static_cast<Texture2D*>(produced);

    texture->m_Width = out_width;
    texture->m_Height = out_height;
    texture->m_Format = cooked.rhi_format;
    texture->m_Pixels = std::move(cooked.pixels);
    texture->m_MipOffsets = std::move(cooked.mip_offsets);

    // --- 4. Serialise to .zasset via the working SerializedFile pipeline ----
    // This is the same path PrefabAsset / Material / XlsxImporter use --
    // see PrefabUtility::SaveAsPrefab and project_window.cpp's
    // material-from-shader code. WriteObjectToDiskThreadSafe writes a single
    // top-level Object to a fresh `.zasset` file; the SerializedFile header
    // contains a type table, an object directory, and the binary-encoded
    // Transfer<> stream produced by Texture2D::Transfer.
    auto asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        MemoryManager::DestroyObject(produced);
        LOG_ERROR(ZEditor, "TextureImporter: AssetManager unavailable");
        return false;
    }

    {
        std::error_code ec;
        if (!output_path.parent_path().empty())
            std::filesystem::create_directories(output_path.parent_path(), ec);
        // Non-fatal; WriteObjectToDiskThreadSafe will surface the real error
        // if the directory still doesn't exist.
    }

    const bool ok = asset_manager->WriteObjectToDiskThreadSafe(output_path, *texture);

    // --- 5. Fill out_metadata for the AssetImporter contract ----------------
    // Note: AssetImporter::tryImport currently drops `out_metadata` on the
    // floor (see asset_importer.cpp). We still populate it correctly so
    // future metadata persistence (route A) doesn't have to revisit every
    // importer. GUID is generated fresh per import; if the user later wires
    // up a stable path-based GUID derivation, replace `generateGUID()` here.
    out_metadata.guid = asset_guid;
    out_metadata.source_file_path = source_path.generic_string();
    {
        std::error_code ec;
        out_metadata.source_file_time = std::filesystem::last_write_time(source_path, ec);
    }
    out_metadata.dependencies.clear();
    out_metadata.custom_metadata.clear();

    // --- 6. Cleanup and report ----------------------------------------------
    const size_t cooked_mip_count = texture->m_MipOffsets.size();
    const TextureCompressor::Format cooked_format = cooked.format;
    MemoryManager::DestroyObject(produced);

    if (ok)
    {
        LOG_INFO(ZEditor,
                 "TextureImporter: Imported {} -> {} ({}x{}, max_size={}, cook={}, mips={}, {})",
                 source_path.generic_string(),
                 output_path.generic_string(),
                 out_width,
                 out_height,
                 effective.max_size,
                 TextureCompressor::ToString(cooked_format),
                 cooked_mip_count,
                 cooked_from_cache ? "ddc-hit" : "encoded");
    }
    else
    {
        LOG_ERROR(ZEditor, "TextureImporter: Failed to write zasset: {}", output_path.string());
    }

    return ok;
}

bool TextureImporter::Reimport(const std::filesystem::path& zasset_path,
                               const AssetImporterSettings& import_settings)
{
    // PR-AI3: source path lives in SourceAssetRegistry, not in the .zasset
    // header. Delegate to EditorAssetManager::reimportAsset when available.
    if (auto editor_mgr = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager)))
    {
        return editor_mgr->reimportAsset(zasset_path.generic_string(), &import_settings);
    }

    LOG_WARNING(ZEditor,
                "TextureImporter::Reimport({}): EditorAssetManager unavailable",
                zasset_path.string());
    return false;
}

std::unique_ptr<AssetImporterSettings> TextureImporter::GetDefaultSettings() const
{
    return std::make_unique<TextureImporterSettings>();
}

int TextureImporter::ImportProjectTextures()
{
    const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->project_path.empty())
    {
        return 0;
    }

    const std::filesystem::path content_root = project_info->GetProjectContent();
    if (content_root.empty())
    {
        return 0;
    }

    std::error_code ec;
    if (!std::filesystem::exists(content_root, ec) || ec)
    {
        return 0;
    }

    TextureImporter importer;
    const std::unique_ptr<AssetImporterSettings> default_settings = importer.GetDefaultSettings();

    auto editor_mgr = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager));
    EditorAssetManager* editor_asset_mgr = editor_mgr.get();

    int imported = 0;

    // Primary: SourceAssetRegistry entries (imported textures). Re-seed a missing
    // .zasset on disk without scanning the whole Assets tree.
    if (editor_asset_mgr != nullptr)
    {
        editor_asset_mgr->GetSourceAssetRegistry().ForEach(
            [&](const std::filesystem::path& zasset_abs, const SourceAssetRegistry::Entry& entry) {
                if (entry.source_path.empty())
                {
                    return;
                }
                if (std::filesystem::exists(zasset_abs, ec) && !ec)
                {
                    return;
                }

                std::filesystem::path output_path = zasset_abs;
                const std::filesystem::path source_path(entry.source_path);
                if (!std::filesystem::exists(source_path, ec) || ec)
                {
                    LOG_WARNING(ZEditor,
                                "TextureImporter::ImportProjectTextures: registry source missing for {} ({})",
                                zasset_abs.generic_string(),
                                source_path.generic_string());
                    return;
                }

                AssetMetadata metadata;
                if (importer.Import(source_path, output_path, *default_settings, metadata))
                {
                    ++imported;
                }
            });
    }

    // Legacy: first-time seeding for source images under Assets/ with no
    // .zasset sibling yet (not in source_registry until Import records them).
    for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(content_root, ec))
    {
        if (ec)
        {
            break;
        }
        if (!dir_entry.is_regular_file())
        {
            continue;
        }
        if (!isSourceImageExtension(dir_entry.path().extension().string()))
        {
            continue;
        }

        std::filesystem::path output_path = dir_entry.path();
        output_path.replace_extension(".zasset");
        if (std::filesystem::exists(output_path, ec))
        {
            continue;
        }

        AssetMetadata metadata;
        if (importer.Import(dir_entry.path(), output_path, *default_settings, metadata))
        {
            ++imported;
        }
    }

    if (imported > 0)
    {
        LOG_INFO(ZEditor, "TextureImporter: ImportProjectTextures seeded {} editor-platform texture(s)",
                 imported);
    }
    return imported;
}

int TextureImporter::CookProjectTextures(TextureImporterSettings::BuildTarget target)
{
    const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->project_path.empty())
    {
        return 0;
    }

    const std::filesystem::path content_root = project_info->GetProjectContent();
    const std::filesystem::path cooked_root = project_info->GetIntermediateCookedRoot();
    if (content_root.empty() || cooked_root.empty())
    {
        return 0;
    }

    std::error_code ec;
    if (!std::filesystem::exists(content_root, ec) || ec)
    {
        return 0;
    }

    const std::string platform_tag = TextureImporterSettings::BuildTargetDisplayName(target);
    const std::filesystem::path platform_root = cooked_root / platform_tag;
    std::filesystem::create_directories(platform_root, ec);

    const std::shared_ptr<AssetManager> asset_mgr = GET_SYSTEM(AssetManager);
    if (asset_mgr == nullptr)
    {
        return 0;
    }

    TextureImporterSettings default_texture_settings;
    {
        TextureImporter defaults_probe;
        if (const auto* typed_defaults =
                dynamic_cast<const TextureImporterSettings*>(defaults_probe.GetDefaultSettings().get()))
        {
            default_texture_settings = *typed_defaults;
        }
    }

    auto editor_mgr = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager));
    EditorAssetManager* editor_asset_mgr = editor_mgr.get();
    const SourceAssetRegistry* source_registry =
        editor_asset_mgr != nullptr ? &editor_asset_mgr->GetSourceAssetRegistry() : nullptr;

    std::unordered_set<std::string> processed_zassets;

    auto tryCookZasset = [&](const std::filesystem::path& zasset_abs) -> bool {
        const std::string key = normalisePathKey(zasset_abs);
        if (!processed_zassets.insert(key).second)
        {
            return false;
        }

        std::error_code exists_ec;
        if (!std::filesystem::exists(zasset_abs, exists_ec) || exists_ec)
        {
            return false;
        }

        std::string source_guid;
        std::string source_type;
        asset_mgr->GetAssetGuidAndType(zasset_abs, source_guid, source_type);
        if (source_guid.empty())
        {
            LOG_WARNING(ZEditor,
                        "TextureImporter::CookProjectTextures: no GUID for {} -- import first; skipping",
                        zasset_abs.generic_string());
            return false;
        }

        const std::optional<std::filesystem::path> source_path =
            resolveSourcePathForZasset(zasset_abs, source_registry);

        const TextureImporterSettings::PlatformSettings effective =
            effectiveSettingsForCook(zasset_abs, target, editor_asset_mgr, default_texture_settings);

        return cookTextureToPlatform(zasset_abs,
                                     source_path,
                                     source_guid,
                                     effective,
                                     target,
                                     platform_tag,
                                     platform_root,
                                     content_root,
                                     *asset_mgr);
    };

    int cooked_count = 0;

    // Primary: AssetRegistry index of Texture2D .zassets under Assets/.
    std::vector<std::filesystem::path> texture_zassets;
    if (editor_asset_mgr != nullptr)
    {
        texture_zassets = editor_asset_mgr->GetAssetsByType("Texture2D", content_root);
    }
    else
    {
        texture_zassets = asset_mgr->GetAssetsByType("Texture2D", content_root);
    }

    for (const std::filesystem::path& zasset_abs : texture_zassets)
    {
        if (tryCookZasset(zasset_abs))
        {
            ++cooked_count;
        }
    }

    // Legacy: sibling source images whose .zasset was not returned by the registry
    // index yet (warm-up race) or pre-index imports.
    for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(content_root, ec))
    {
        if (ec)
        {
            break;
        }
        if (!dir_entry.is_regular_file() || !isSourceImageExtension(dir_entry.path().extension().string()))
        {
            continue;
        }

        std::filesystem::path editor_zasset = dir_entry.path();
        editor_zasset.replace_extension(".zasset");
        if (tryCookZasset(editor_zasset))
        {
            ++cooked_count;
        }
    }

    LOG_INFO(ZEditor, "TextureImporter: CookProjectTextures({}) wrote {} cooked texture(s) to {}",
             platform_tag, cooked_count, platform_root.generic_string());
    return cooked_count;
}
