#pragma once

// =============================================================================
// Texture2D
// -----------------------------------------------------------------------------
// UE-style 2D texture **asset**. Produced by TextureImporter from a source
// image (.png / .jpg / .jpeg / .tga / .bmp / .dds) and serialised to a
// `.zasset` via `AssetManager::WriteObjectToDiskThreadSafe` (the same Unity-
// style binary SerializedFile path that PrefabAsset / MaterialRes use).
//
// Layering rationale (route B per doc/BINDLESS_TEXTURE_PATH.md):
//
//   * `Texture` (base class, runtime/function/render/texture/texture.h) is
//     currently a placeholder with two private legacy fields and no
//     reflection/serialization. We deliberately do NOT call Super::Transfer
//     here -- those fields would need to become protected and the base would
//     need its own Transfer<TF> first. That refactor is queued under route A
//     follow-ups; until then, Texture2D owns its own state.
//
//   * Storage shape:
//       - `std::vector<uint8_t> m_Pixels` -- raw decoded pixel blob (NOT
//         eastl::vector; SerializeTraits is only specialised for std::vector,
//         see Runtime/Core/Serialize/SerializeTraits.h, mirrors the convention
//         documented in AGENTS.md §2.3 / DataTable layout).
//       - `m_Width` / `m_Height` describe the blob's layout in pixels.
//       - `m_Format` is stored as `uint32_t` (the underlying RHIFormat enum
//         ordinal) to keep the on-disk representation stable across enum
//         reorderings. RHI types are intentionally NOT pulled into this
//         header so Texture2D can sit in any module without dragging the RHI
//         surface in.
//
//   * **Backend-agnostic**: this class deliberately holds NO RHI state
//     (no RHIImage*, no bindless slot). GPU upload + descriptor allocation
//     is the consumer's responsibility (e.g. the editor Preview window's
//     texture path). This keeps Texture2D free of any DX12 / Vulkan / Metal
//     include leakage and makes it cheap to deserialise in a headless /
//     cooker context.
//
//     If future material binding code needs an "every Texture2D auto-uploads
//     itself once" policy, that's a route-A follow-up: introduce a runtime
//     GPUResourceManager that observes asset-load events and pulls
//     `m_Pixels` into RHI state on demand.
//
// Cross-references:
//   - doc/BINDLESS_TEXTURE_PATH.md  (route B section)
//   - PrefabAsset.h / Material.h    -- structurally identical asset class shape
//   - render_type.h (TextureData)   -- the POD this used to be serialised as
//                                      before route B; kept around because
//                                      the importer still pivots through it
//                                      during decode (PR10).
// =============================================================================

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Function/Render/Texture/Texture.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class Texture2D : public Texture
{
    REGISTER_CLASS(Texture2D);
    DECLARE_OBJECT_SERIALIZE(Texture2D);

public:
    Texture2D() = default;
    ~Texture2D() override = default;

    // A non-owning view into one mip level inside m_Pixels.
    struct MipSpan
    {
        const uint8_t* data {nullptr};
        size_t size {0};
    };

    // -------- Serialised state (the on-disk payload) -------------------------
    // Width / height in pixels of mip 0.
    uint32_t m_Width {0};
    uint32_t m_Height {0};

    // RHIFormat ordinal of the stored pixel blob. Stored as raw uint32_t so
    // the on-disk representation does not break if the C++ enum is ever
    // reordered. The importer stamps this from `RHIFormat` (RGBA8 for legacy /
    // uncompressed assets, BC1/BC3/BC7 or ASTC for cooked variants); consumers
    // cast back to `RHIFormat` when feeding RHI::CreateGlobalImage. RHI types
    // are intentionally NOT pulled into this header, so whether the format is
    // block-compressed is decided by the GPU-side consumer, not here.
    uint32_t m_Format {0};

    // Pixel payload. Mip 0 first, then each successive (half-size, floored)
    // mip concatenated. For an uncompressed RGBA8 mip i its byte length is
    // max(1,w>>i) * max(1,h>>i) * 4; for a block-compressed format it is the
    // block-aligned size for that mip. The byte boundaries live in
    // m_MipOffsets (below) so this class needs no format math to slice mips.
    std::vector<uint8_t> m_Pixels;

    // Byte offset of each mip level's first byte within m_Pixels. Its SIZE is
    // the authoritative mip count (offsets[0] is always 0). Appended after the
    // legacy "pixels" field, so an old single-mip .zasset (written before this
    // schema bump, no "mip_offsets" node) reads back via SafeBinaryRead's
    // kNotFound path as an EMPTY vector -- the accessors below then treat the
    // whole m_Pixels blob as a single mip 0. New cooked assets always write a
    // fully-populated table. (This is the drift-free single-source-of-truth
    // equivalent of a separate m_MipCount + m_MipOffsets pair: offsets.size()
    // IS the count.)
    std::vector<uint32_t> m_MipOffsets;

    // -------- Authoring / consumer helpers -----------------------------------
    bool IsValid() const { return m_Width > 0 && m_Height > 0 && !m_Pixels.empty(); }

    // Normalised mip count: explicit table size when present, else 1 for a
    // non-empty legacy blob, else 0.
    uint32_t GetMipCount() const
    {
        if (!m_MipOffsets.empty())
        {
            return static_cast<uint32_t>(m_MipOffsets.size());
        }
        return m_Pixels.empty() ? 0u : 1u;
    }

    // View into mip level `level` (0-based). Out-of-range or empty -> {nullptr,0}.
    MipSpan GetMipSpan(uint32_t level) const
    {
        const uint32_t count = GetMipCount();
        if (level >= count || m_Pixels.empty())
        {
            return {};
        }

        // Legacy single-mip blob (no offset table): the whole payload is mip 0.
        if (m_MipOffsets.empty())
        {
            return MipSpan {m_Pixels.data(), m_Pixels.size()};
        }

        const size_t begin = m_MipOffsets[level];
        const size_t end = (level + 1 < count) ? static_cast<size_t>(m_MipOffsets[level + 1]) : m_Pixels.size();
        if (begin > m_Pixels.size() || end > m_Pixels.size() || end < begin)
        {
            return {};  // corrupt offset table -- fail closed rather than read OOB
        }
        return MipSpan {m_Pixels.data() + begin, end - begin};
    }
};
