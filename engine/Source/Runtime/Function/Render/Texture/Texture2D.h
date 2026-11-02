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
#include "Runtime/Core/Color/Color.h"
#include "Runtime/Function/Render/Texture/Texture.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include "Texture2D.generated.h"

ZCLASS()
class Texture2D : public Texture
{
    ZGENERATED_BODY(Texture2D)

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
    ZPROPERTY()
    uint32_t m_Width {0};
    ZPROPERTY()
    uint32_t m_Height {0};

    // RHIFormat ordinal of the stored pixel blob. Stored as raw uint32_t so
    // the on-disk representation does not break if the C++ enum is ever
    // reordered. The importer stamps this from `RHIFormat` (RGBA8 for legacy /
    // uncompressed assets, BC1/BC3/BC7 or ASTC for cooked variants); consumers
    // cast back to `RHIFormat` when feeding RHI::CreateGlobalImage. RHI types
    // are intentionally NOT pulled into this header, so whether the format is
    // block-compressed is decided by the GPU-side consumer, not here.
    ZPROPERTY()
    uint32_t m_Format {0};

    // Pixel payload. Mip 0 first, then each successive (half-size, floored)
    // mip concatenated. For an uncompressed RGBA8 mip i its byte length is
    // max(1,w>>i) * max(1,h>>i) * 4; for a block-compressed format it is the
    // block-aligned size for that mip. The byte boundaries live in
    // m_MipOffsets (below) so this class needs no format math to slice mips.
    ZPROPERTY()
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
    // Appended after "pixels" for schema evolution: an old single-mip .zasset
    // has no "mip_offsets" node, so SafeBinaryRead returns kNotFound and leaves
    // this empty -- GetMipCount()/GetMipSpan() then treat m_Pixels as one mip.
    ZPROPERTY()
    std::vector<uint32_t> m_MipOffsets;

    // =====================================================================
    // 脚本门面
    // =====================================================================
    //
    // 与 Unity 的 `Texture2D` 对位，但**只覆盖 CPU 侧那一半**：读宽高、读写
    // 像素。没有 `Apply()` / `LoadImage()` / `EncodeToPNG()`，理由见下面各自
    // 的位置。
    //
    // 为什么像素访问要求"未压缩 RGBA8"
    // --------------------------------
    // `m_Format` 存的是 RHIFormat 的**序号**（见上面 m_Format 的注释：这个
    // 头文件刻意不依赖 RHI）。门面这边没法判定"这个序号是不是每像素 4 字节"
    // —— 而猜错的后果是静默读错位（BC7 上一个像素 1 字节，按 4 字节读就越界）。
    //
    // 所以判据是**可观测的事实而不是格式号**：`m_Pixels.size() == w * h * 4`
    // 就按 RGBA8 读写；不成立就拒绝（`SetPixel` 返回 false，`GetPixel` 前提
    // 供 `hasUncompressedPixels` 先查）。这比"信格式号"稳 —— cook 出来的
    // 资产格式号写错，或者在 importer 之外手搓的 Texture2D，都不会读坏内存。

    // ⚠️ **没有** `width` / `height` / `format` 这三个 ZSCRIPTPROPERTY ——
    // 上面的 `ZPROPERTY() m_Width` / `m_Height` / `m_Format` **已经**把它们
    // 发布成脚本属性了（`m_` 前缀在脚本侧被剥掉，见 zht/parser.py 的
    // `script_property_name`）。再挂一份会撞名，ZHeaderTool 直接报错。
    //
    // 也就是说脚本写 `tex.width` / `tex.height` / `tex.format` 就够了，
    // 而且那三个还是**可写的**（序列化的公开字段）。

    /// mip 层数。Unity 的 `Texture2D.mipmapCount`。
    ZFUNCTION()
    uint32_t GetMipmapCount() const { return GetMipCount(); }
    ZSCRIPTPROPERTY(Name = "mipmapCount", Getter = "GetMipmapCount")

    /// 尺寸与像素数据是否都有。Unity 没有这一项（它的 Texture2D 建出来就一定
    /// 有数据），这里加是因为 **反序列化可能得到空壳** —— 空壳上读像素会
    /// 拿到 0 而不是崩，那种"看起来正常其实是空的"很难查。
    ZFUNCTION()
    bool GetIsValid() const { return IsValid(); }
    ZSCRIPTPROPERTY(Name = "isValid", Getter = "GetIsValid")

    /// 像素缓冲是不是"每像素 4 字节"的布局。
    ///
    /// 用 `GetPixel` / `SetPixel` 之前先看这个。压缩格式（BC / ASTC / ETC2）
    /// 上它是 false，那两个函数也不会生效。
    ZFUNCTION()
    bool GetHasUncompressedPixels() const;
    ZSCRIPTPROPERTY(Name = "hasUncompressedPixels", Getter = "GetHasUncompressedPixels")

    /// 读 mip 0 的一个像素。Unity 的 `Texture2D.GetPixel(x, y)`。
    ///
    /// 越界返回**透明黑**（`r=g=b=a=0`）而不是夹紧：夹紧会让 `GetPixel(-1, 0)`
    /// 悄悄返回边缘像素，把"索引算错了"变成"画面上有个奇怪的边"。
    /// 想先知道能不能读，用 `hasUncompressedPixels` + 自己比 width/height。
    ZFUNCTION()
    Color GetPixel(int x, int y) const;

    /// 写 mip 0 的一个像素。返回是否真的写了。
    ///
    /// ⚠️ **不上传 GPU**（见下面"没有 Apply()"）—— 它改的是 CPU 侧那份
    /// `m_Pixels`。
    ///
    /// ⚠️ 改完**不会**重算 mip：多 mip 的贴图改了 mip 0 之后 mip 1..n 是旧的。
    /// 这是 Unity 的行为（Unity 也要 `Apply(true)` 才重算），这里没有 Apply，
    /// 所以只在单 mip 的贴图（或新 `Resize` 出来的）上改像素。
    ZFUNCTION()
    bool SetPixel(int x, int y, const Color& color);

    /// 用同一个颜色填满 mip 0。返回是否成功（判据同 `SetPixel`）。
    ZFUNCTION()
    bool ClearPixels(const Color& color);

    /// 重设尺寸并清空像素（全 0），丢掉全部 mip（变成单 mip）。
    /// Unity 的 `Texture2D.Resize(w, h)`。
    ///
    /// ⚠️ Unity 的 `Resize` **保留**原像素并允许再传 format / hasMipMap；
    /// 这里做不到前者（"保留"在尺寸变化时的插值规则 Unity 也没定义清楚），
    /// 于是语义是"重开一张空图"。要保留内容就自己 GetPixel / SetPixel 搬。
    ZFUNCTION()
    bool Resize(int width, int height);

    // ---- 没有 `Apply()` / `LoadImage()` / `EncodeToPNG()` -------------------
    //
    // Unity 的这三个都依赖"贴图在 GPU 上有对应资源"：
    //   * `Apply()`     —— 把 CPU 像素上传 +（可选）重算 mip
    //   * `LoadImage()` —— 解码一张 png/jpg 塞进贴图
    //   * `EncodeToPNG()` —— 编码回 png
    //
    // Texture2D 刻意**不持有 RHI 状态**（见文件头：它要能在 cooker / headless
    // 里反序列化）。上传这件事归未来的 GPUResourceManager，解码 / 编码归
    // importer。在门面上补一个 `Apply()` 空实现，会让脚本以为"改完像素屏幕
    // 上就变了" —— 那是本仓库最忌讳的假成功。

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
