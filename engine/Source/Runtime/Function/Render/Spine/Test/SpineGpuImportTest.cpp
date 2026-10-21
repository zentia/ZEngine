// =====================================================================
// GPU-Spine 导入器测试
// ---------------------------------------------------------------------
// 夹具是一份手写的 3.8 json + atlas，故意覆盖：
//   * 一个 region（rigid，4 顶点）
//   * 一个 mesh，4 个顶点里第 2 个是 2 影响（1 影响 / 2 影响 / 1 影响 / 1 影响）
//   * 权重 0.6 + 0.4（量化后应恰好 153 + 102 = 255）
// 然后钉死"静默就会错"的规则：影响数超限、权重和、段对齐、版本。
//
// Build / run:
//   cmake -B build -DZENGINE_BUILD_SPINE_IMPORT_TEST=ON
//   cmake --build build --target SpineGpuImportTest --config Debug
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_checks = 0;
int g_failed = 0;

void Check(bool cond, const char* what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failed;
        std::printf("  [FAIL] %s\n", what);
    }
}

void CheckEq(long long got, long long want, const char* what)
{
    ++g_checks;
    if (got != want)
    {
        ++g_failed;
        std::printf("  [FAIL] %s (got %lld, want %lld)\n", what, got, want);
    }
}

// ---- 夹具 ----
const char* kAtlas = "page0.png\n"
                     "size: 256,256\n"
                     "format: RGBA8888\n"
                     "filter: Linear,Linear\n"
                     "repeat: none\n"
                     "quad\n"
                     "  rotate: false\n"
                     "  xy: 0, 0\n"
                     "  size: 64, 32\n"
                     "mesh\n"
                     "  rotate: false\n"
                     "  xy: 0, 40\n"
                     "  size: 64, 32\n";

// mesh 的 vertices 是 spine 的扁平格式：每顶点 [n, (boneIdx, x, y, w) × n]
//   v0: 1 影响   v1: 2 影响(0.6/0.4)   v2: 1 影响   v3: 1 影响
// uvs 8 个 = 4 顶点 × 2；vertices 24 个 ≠ 8 ⇒ 判为 weighted（参考实现的判据）
const char* kSkeleton = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "mesh" }
  ],
  "skins": [ { "name": "default", "attachments": {
    "s0": { "quad": { "x": 0, "y": 0, "width": 64, "height": 32, "rotation": 0 } },
    "s1": { "mesh": {
      "type": "mesh",
      "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
      "triangles": [ 0,1,2, 0,2,3 ],
      "vertices": [ 1, 0,-10,-5,1,
                    2, 1, 20,10,0.6, 0,5,7,0.4,
                    1, 1, 3,4,1,
                    1, 0, 8,2,1 ]
    } }
  } } ]
})";

}   // namespace

int main()
{
    std::printf("=== GPU-Spine importer test ===\n");

    // ---- 1. atlas ----
    SpineAtlas atlas;
    SpineImportResult ra = ParseSpineAtlas(kAtlas, std::string(kAtlas).size(), atlas);
    Check(ra.ok, "atlas 解析成功");
    if (!ra.ok)
    {
        std::printf("  error: %s\n", ra.error.c_str());
        return 1;
    }
    CheckEq(static_cast<long long>(atlas.pages.size()), 1, "atlas: 1 页");
    CheckEq(static_cast<long long>(atlas.regions.size()), 2, "atlas: 2 个 region");
    CheckEq(atlas.pages[0].width, 256, "atlas: 页宽 256");
    Check(atlas.regions.size() == 2 && atlas.regions[1].name == "mesh", "atlas: 第二个 region 是 mesh");

    // ---- 2. json ----
    SpineSourceSkeleton src;
    SpineImportResult rj = ParseSpineSkeletonJson(kSkeleton, std::string(kSkeleton).size(), &atlas, src);
    Check(rj.ok, "json 解析成功");
    if (!rj.ok)
    {
        std::printf("  error: %s\n", rj.error.c_str());
        return 1;
    }
    CheckEq(static_cast<long long>(src.bones.size()), 2, "json: 2 根骨骼");
    CheckEq(static_cast<long long>(src.slots.size()), 2, "json: 2 个槽位");
    CheckEq(static_cast<long long>(src.attachments.size()), 2, "json: 2 个附件");
    Check(src.attachments.size() == 2 && !src.attachments[0].IsWeighted(), "region 是 rigid");
    Check(src.attachments.size() == 2 && src.attachments[1].IsWeighted(), "mesh 是 weighted");
    Check(!src.has_clipping, "夹具没有裁剪");
    Check(!src.has_draw_order_timeline, "夹具没有绘制顺序时间轴");

    // ---- 3. bake ----
    SpineGpuAsset asset;
    SpineImportResult rb = BakeSpineGpuAsset(src, asset);
    Check(rb.ok, "bake 成功");
    if (!rb.ok)
    {
        std::printf("  error: %s\n", rb.error.c_str());
        return 1;
    }

    CheckEq(asset.header.magic, SpineGpuConstants::kMagic, "header.magic");
    CheckEq(asset.header.spine_runtime, SpineGpuConstants::kSpineRuntime38, "header.spine_runtime = 30800");
    CheckEq(asset.header.bone_count, 2, "header.bone_count");
    CheckEq(asset.header.slot_count, 2, "header.slot_count");
    CheckEq(asset.header.draw_item_count, 2, "header.draw_item_count");
    CheckEq(asset.rigid_vertex_count, 4, "rigid 顶点数 = 4（一个 region）");
    CheckEq(asset.weighted_vertex_count, 4, "weighted 顶点数 = 4");
    CheckEq(asset.header.vertex_count, 8, "header.vertex_count = 8");
    // region 2 个三角形（6 索引）+ mesh 2 个三角形（6 索引）
    CheckEq(static_cast<long long>(asset.index_buffer.size()), 12, "索引数 = 6 + 6");

    // weighted 段起点必须 8 B 对齐（Load2 的前提，规格 §3.1）
    CheckEq(asset.weighted_segment_offset % SpineGpuConstants::kVertexSegmentAlignment, 0,
            "weighted 段起点 8 B 对齐");

    // 顶点缓冲大小 = weighted 段起点 + weighted 字节数
    CheckEq(static_cast<long long>(asset.vertex_buffer.size()),
            static_cast<long long>(asset.weighted_segment_offset) +
                4LL * SpineGpuConstants::kWeightedVertexStride,
            "顶点缓冲大小");

    // ---- 4. 权重：每顶点 4 个 unorm8 的和必须恰好 255 ----
    const uint8_t* wbase = asset.vertex_buffer.data() + asset.weighted_segment_offset;
    for (uint32_t v = 0; v < asset.weighted_vertex_count; ++v)
    {
        const SpineWeightedVertex* wv =
            reinterpret_cast<const SpineWeightedVertex*>(wbase + static_cast<size_t>(v) *
                                                                    SpineGpuConstants::kWeightedVertexStride);
        const int sum = wv->weight[0] + wv->weight[1] + wv->weight[2] + wv->weight[3];
        CheckEq(sum, 255, "权重和为 255");
    }
    // 第 2 个顶点是 0.6 / 0.4 → 153 / 102
    {
        const SpineWeightedVertex* wv =
            reinterpret_cast<const SpineWeightedVertex*>(wbase + SpineGpuConstants::kWeightedVertexStride);
        CheckEq(wv->weight[0], 153, "0.6 → 153");
        CheckEq(wv->weight[1], 102, "0.4 → 102");
        CheckEq(wv->weight[2], 0, "未用槽位权重为 0");
        CheckEq(wv->bone_idx[0], 1, "第 0 影响的骨骼下标");
        CheckEq(wv->bone_idx[1], 0, "第 1 影响的骨骼下标");
    }

    // ---- 5. 报错路径：影响数超限 ----
    {
        const char* bad = R"({
          "skeleton": { "spine": "3.8.99" },
          "bones": [ { "name": "root" } ],
          "slots": [ { "name": "s0", "bone": "root" } ],
          "skins": [ { "name": "default", "attachments": {
            "s0": { "mesh": {
              "type": "mesh",
              "uvs": [ 0,0 ],
              "triangles": [],
              "vertices": [ 5, 0,1,2,0.2, 0,3,4,0.2, 0,5,6,0.2, 0,7,8,0.2, 0,9,10,0.2 ]
            } }
          } } ]
        })";
        SpineSourceSkeleton bad_src;
        SpineImportResult r1 = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, bad_src);
        Check(r1.ok, "5 影响的 json 能解析（烘焙时才报错）");
        SpineGpuAsset bad_asset;
        SpineImportResult r2 = BakeSpineGpuAsset(bad_src, bad_asset);
        Check(!r2.ok, "影响数 5 > 4 ⇒ bake 必须失败");
        Check(r2.error.find("mesh") != std::string::npos, "错误信息里带附件名");
        std::printf("  (预期报错) %s\n", r2.error.c_str());
    }

    // ---- 6. 报错路径：版本不是 3.8 ----
    {
        const char* v4 = R"({ "skeleton": { "spine": "4.2.11" }, "bones": [], "slots": [] })";
        SpineSourceSkeleton s4;
        SpineImportResult r4 = ParseSpineSkeletonJson(v4, std::string(v4).size(), &atlas, s4);
        Check(!r4.ok, "spine 4.2 ⇒ 必须报错（布局不同）");
        std::printf("  (预期报错) %s\n", r4.error.c_str());
    }

    // ---- 7. 序列化 → 反序列化 往返 ----
    std::vector<uint8_t> bytes;
    SpineImportResult rw = WriteSpineGpuContainer(asset, bytes);
    Check(rw.ok, "序列化成功");
    if (!rw.ok)
    {
        std::printf("  error: %s\n", rw.error.c_str());
        return 1;
    }

    // 7 个 chunk：strings / slot_bone / vertex / index / draw_items / attachments / atlas_pages
    //
    // 不写死总字节数：总长里含对齐填充，填充规则一改常数就失效，而它失效时并不代表出错。
    // 真正该钉死的是 chunk 表本身 —— 净载荷大小与填充无关。
    auto CStrBytes = [](const std::vector<std::string>& v) -> long long
    {
        long long n = 0;
        for (const std::string& s : v)
        {
            n += static_cast<long long>(s.size()) + 1;   // '\0' 结尾
        }
        return n;
    };
    const long long want_payload =
        CStrBytes(asset.bone_names) + CStrBytes(asset.slot_names) + CStrBytes(asset.attachment_names) +
        CStrBytes(asset.atlas_page_names) +                                          // id=1 strings
        static_cast<long long>(asset.slot_bone.size()) * 2LL +                       // id=2
        static_cast<long long>(asset.vertex_buffer.size()) +                         // id=3
        static_cast<long long>(asset.index_buffer.size()) * 2LL +                    // id=4
        static_cast<long long>(asset.draw_items.size()) *
            static_cast<long long>(sizeof(SpineDrawItem)) +                          // id=5（20 B/项）
        static_cast<long long>(asset.attachment_names.size()) *
            static_cast<long long>(sizeof(SpineGpuAttachmentRecord)) +               // id=6（4 B/项）
        CStrBytes(asset.atlas_page_names);                                           // id=7

    auto RdU32 = [&bytes](size_t off) -> uint32_t
    {
        return static_cast<uint32_t>(bytes[off]) | (static_cast<uint32_t>(bytes[off + 1]) << 8) |
               (static_cast<uint32_t>(bytes[off + 2]) << 16) | (static_cast<uint32_t>(bytes[off + 3]) << 24);
    };
    const uint32_t nchunk = RdU32(offsetof(SpineGpuHeader, chunk_count));
    CheckEq(static_cast<long long>(nchunk), 7, "字节流里 chunk_count = 7（夹具没有 clip_data）");

    long long payload_sum = 0;
    long long last_end = 0;
    uint32_t prev_offset = 0;
    for (uint32_t i = 0; i < nchunk; ++i)
    {
        const size_t at = static_cast<size_t>(SpineGpuConstants::kHeaderSize) +
                          static_cast<size_t>(i) * SpineGpuConstants::kChunkTableEntrySize;
        const uint32_t id = RdU32(at);
        const uint32_t off = RdU32(at + 4);
        const uint32_t sz = RdU32(at + 8);
        CheckEq(static_cast<long long>(id), static_cast<long long>(i) + 1, "chunk 表: id 按 1..7 升序");
        CheckEq(static_cast<long long>(off % SpineGpuConstants::kChunkAlignment), 0, "chunk 表: 起点 4 B 对齐");
        if (id == static_cast<uint32_t>(SpineGpuChunkId::kChunkVertexBuffer))
        {
            CheckEq(static_cast<long long>(off % SpineGpuConstants::kVertexSegmentAlignment), 0,
                    "chunk 表: 顶点 chunk 起点 8 B 对齐（Load2 的前提）");
        }
        Check(static_cast<long long>(off) >=
                  static_cast<long long>(SpineGpuConstants::kHeaderSize) + nchunk * SpineGpuConstants::kChunkTableEntrySize,
              "chunk 表: 起点不落在 header/表里面");
        Check(static_cast<long long>(off) + static_cast<long long>(sz) <= static_cast<long long>(bytes.size()),
              "chunk 表: 起点+长度不越界");
        if (i > 0)
        {
            Check(static_cast<long long>(off) >= static_cast<long long>(prev_offset), "chunk 表: 起点单调不减");
        }
        prev_offset = off;
        payload_sum += static_cast<long long>(sz);
        last_end = static_cast<long long>(off) + static_cast<long long>(sz);
    }
    CheckEq(payload_sum, want_payload, "chunk 表里的净载荷总和（与对齐填充无关）");

    // 总长只能比"净载荷 + header + 表"多出对齐填充，每个 chunk 最多 align-1，取 8 作上界。
    const long long base = static_cast<long long>(SpineGpuConstants::kHeaderSize) +
                           static_cast<long long>(nchunk) * SpineGpuConstants::kChunkTableEntrySize + want_payload;
    Check(static_cast<long long>(bytes.size()) >= base, "容器总长 ≥ header + 表 + 净载荷");
    Check(static_cast<long long>(bytes.size()) <= base + 8LL * static_cast<long long>(nchunk),
          "容器总长 ≤ 净载荷 + 对齐填充上界");
    // 最后一个 chunk 必须正好收在文件尾：不留尾巴，也没被截断。
    CheckEq(static_cast<long long>(bytes.size()), last_end, "容器总长 = 最后一个 chunk 的结尾（无尾部冗余）");

    SpineGpuAsset back;
    SpineImportResult rr = ReadSpineGpuContainer(bytes.data(), bytes.size(), back);
    Check(rr.ok, "反序列化成功");
    if (!rr.ok)
    {
        std::printf("  error: %s\n", rr.error.c_str());
        return 1;
    }

    CheckEq(back.header.magic, SpineGpuConstants::kMagic, "往返: magic");
    CheckEq(back.header.chunk_count, 7, "往返: chunk_count = 7（夹具没有 clip_data）");
    CheckEq(back.header.bone_count, asset.header.bone_count, "往返: bone_count");
    CheckEq(back.header.slot_count, asset.header.slot_count, "往返: slot_count");
    CheckEq(back.header.draw_item_count, asset.header.draw_item_count, "往返: draw_item_count");
    CheckEq(back.header.vertex_count, asset.header.vertex_count, "往返: vertex_count");
    CheckEq(back.header.index_count, asset.header.index_count, "往返: index_count");
    CheckEq(back.header.atlas_page_count, asset.header.atlas_page_count, "往返: atlas_page_count");
    CheckEq(back.rigid_vertex_count, asset.rigid_vertex_count, "往返: rigid_vertex_count");
    CheckEq(back.weighted_vertex_count, asset.weighted_vertex_count, "往返: weighted_vertex_count");
    CheckEq(back.weighted_segment_offset, asset.weighted_segment_offset, "往返: weighted_segment_offset");

    Check(back.vertex_buffer == asset.vertex_buffer, "往返: 顶点缓冲逐字节一致");
    Check(back.index_buffer == asset.index_buffer, "往返: 索引一致");
    CheckEq(static_cast<long long>(back.draw_items.size()),
            static_cast<long long>(asset.draw_items.size()), "往返: 绘制项数一致");
    if (back.draw_items.size() == asset.draw_items.size())
    {
        for (size_t i = 0; i < asset.draw_items.size(); ++i)
        {
            Check(std::memcmp(&back.draw_items[i], &asset.draw_items[i], sizeof(SpineDrawItem)) == 0,
                  "往返: 绘制项逐字节一致（含 2 B 尾部填充）");
        }
    }
    CheckEq(static_cast<long long>(back.slot_bone.size()), static_cast<long long>(asset.slot_bone.size()),
            "往返: slot_bone 数量");
    Check(back.bone_names == asset.bone_names, "往返: 骨骼名");
    Check(back.slot_names == asset.slot_names, "往返: 槽位名");
    Check(back.attachment_names == asset.attachment_names, "往返: 附件名");
    Check(back.atlas_page_names == asset.atlas_page_names, "往返: 图集页名");

    // ---- 8. 报错路径：版本不符必须拒绝，不做静默兼容 ----
    {
        std::vector<uint8_t> bad = bytes;
        uint32_t* ver = reinterpret_cast<uint32_t*>(bad.data() + 4);
        *ver = 999;
        SpineGpuAsset dummy;
        SpineImportResult r = ReadSpineGpuContainer(bad.data(), bad.size(), dummy);
        Check(!r.ok, "版本不符 ⇒ 读回必须失败（规格 §11）");
        std::printf("  (预期报错) %s\n", r.error.c_str());
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad[0] = 'X';
        SpineGpuAsset dummy;
        SpineImportResult r = ReadSpineGpuContainer(bad.data(), bad.size(), dummy);
        Check(!r.ok, "魔数不对 ⇒ 读回必须失败");
    }

    std::printf("=== verdict: %s (%d/%d checks) ===\n", g_failed == 0 ? "PASS" : "FAIL",
                g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
