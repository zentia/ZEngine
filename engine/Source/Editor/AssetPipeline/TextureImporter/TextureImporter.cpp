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
//     SerializedFile-based pipeline, the same one PrefabAsset / MaterialRes /
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
#include "Runtime/Resource/Asset/AssetManager.h"
#include "TextureImporterSettings.h"

// Note: STB_IMAGE_IMPLEMENTATION is already defined in ZRuntime's render_resource_base.cpp
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>
#include <stb_image.h>
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

    uint32_t resolveStoredFormat(const TextureImporterSettings::PlatformSettings& settings)
    {
        switch (settings.format)
        {
            case TextureImporterSettings::Format::RGBA8:
                return static_cast<uint32_t>(settings.sRGB ? RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB
                                                           : RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM);
            case TextureImporterSettings::Format::RGB8:
                return static_cast<uint32_t>(settings.sRGB ? RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB
                                                           : RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM);
            case TextureImporterSettings::Format::RGBA16F:
                return static_cast<uint32_t>(RHIFormat::RHI_FORMAT_R16G16B16A16_SFLOAT);
            case TextureImporterSettings::Format::BC7:
            case TextureImporterSettings::Format::BC1:
            case TextureImporterSettings::Format::BC3:
                // Block-compressed payloads are not cooked yet; stamp UNORM RGBA8
                // so consumers stay valid until a BC encoder lands.
                return static_cast<uint32_t>(settings.sRGB ? RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB
                                                           : RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM);
            default:
                return static_cast<uint32_t>(RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM);
        }
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

    // --- 3. Construct Texture2D and copy pixels in --------------------------
    // We MUST go through ObjectManager::Produce -- direct `new Texture2D` is
    // illegal because ObjectManager owns InstanceID assignment / lifetime
    // bookkeeping (mirrors xlsx_importer / project_window's MaterialRes
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
    texture->m_Format = resolveStoredFormat(effective);
    texture->m_Pixels = std::move(pixel_blob);

    // --- 4. Serialise to .zasset via the working SerializedFile pipeline ----
    // This is the same path PrefabAsset / MaterialRes / XlsxImporter use --
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
    if (import_settings.generate_guid && !import_settings.custom_guid.empty())
    {
        out_metadata.guid = import_settings.custom_guid;
    }
    else
    {
        out_metadata.guid = generateGUID();
    }
    out_metadata.source_file_path = source_path.generic_string();
    {
        std::error_code ec;
        out_metadata.source_file_time = std::filesystem::last_write_time(source_path, ec);
    }
    out_metadata.dependencies.clear();
    out_metadata.custom_metadata.clear();

    // --- 6. Cleanup and report ----------------------------------------------
    MemoryManager::DestroyObject(produced);

    if (ok)
    {
        LOG_INFO(ZEditor,
                 "TextureImporter: Imported {} -> {} ({}x{}, max_size={}, fmt={})",
                 source_path.generic_string(),
                 output_path.generic_string(),
                 out_width,
                 out_height,
                 effective.max_size,
                 TextureImporterSettings::FormatDisplayName(effective.format));
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
