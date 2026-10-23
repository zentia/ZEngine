// =====================================================================
// GPU-Spine 性能基准（CPU 侧，单线程）：我们的每帧路径 vs spine-cpp 3.8 的 CPU 蒙皮路径
// ---------------------------------------------------------------------
// 用法：SpineGpuBenchmark <资产目录> [--frames N] [--limit N] [--csv out.csv] [--top N] [--verbose]
//   目录里找 `*.skel.bytes` / `*.json` + 同名 `.atlas.txt` / `.atlas`（与 SpineGpuRefParityTest 同口径）。
//
// 每个资产两边吃**同一份**脚本：dt = 1/60，default mix 0.2，轨道 0 循环播第 0 个动画，每 60 帧
// setAnimation 到下一个动画（循环取）——覆盖"正常播放 + 混合"这两种游戏里最常见的状态。
//
// 我们（= SpineGpuRenderer::Impl 每帧对一个世界实例做的事，逐个函数调同一份代码）：
//   anim  : SpineGpuAnimationState::Update + Apply
//   pose  : SpineGpuUpdateWorld（骨骼世界变换 + 槽位颜色 / 附件 / 绘制顺序 + 生效的 mesh deform）
//   geo   : SpineGpuClipFrame（裁剪作用域内的项在 CPU 上蒙皮 + 裁）+ SpineGpuAppendBatchInstance（拼包）
//   上传  : 合包各数组的字节数（bones / draws / insts / deform / cpu_verts / cpu_index）——每帧要传给 GPU 的量
//
// spine-cpp（= spine-sfml / spine-cocos2dx / spine-unity 的 CPU 网格路径）：
//   anim  : AnimationState::update + apply
//   pose  : Skeleton::updateWorldTransform
//   geo   : 按 drawOrder 逐槽位 computeWorldVertices（region 4 顶点 / mesh 全部顶点）+ SkeletonClipping，
//           写成交错顶点（pos float2 + uv float2 + rgba8 [+ dark rgba8]）+ u16 索引 —— 这是**最省**的写法
//           （spine-unity 实际是 pos float3 + color + uv，更大），偏向参考那边
//   上传  : 顶点 + 索引字节数
//
// 输出的是**每实例**的成本：单线程、单实例。N 个实例 ≈ N 倍（两边都没有跨实例共享的每帧工作）。
// 编出来必须是 Release / RelWithDebInfo（Debug 的数字没有意义，main() 里会警告）。
// stdout 不缓冲：经 MSYS 管道重定向时，缓冲的输出在进程退出时会丢（实测）。
// ⚠️ 绝对值受机器负载影响很大（同一份二进制连跑三次能漂 50%），**只能看比值**；
//    最新数字与结论记在 `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` §17（改口径要同步改那一节）。
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"
#include "Runtime/Function/Render/Spine/SpineGpuBatch.h"
#include "Runtime/Function/Render/Spine/SpineGpuClipper.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"
#include "Runtime/Function/Render/Spine/SpineGpuSkeletonState.h"

#include "SpineGpuRefRuntime.h"

#include <spine/Animation.h>
#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/Bone.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonClipping.h>
#include <spine/SkeletonData.h>
#include <spine/SkeletonJson.h>
#include <spine/Slot.h>
#include <spine/SlotData.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
using Clock = std::chrono::steady_clock;
inline double UsSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

bool ReadAll(const fs::path& p, std::vector<char>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// 一个资产在一边上跑完脚本后的结果（每帧平均）
struct SideResult
{
    double anim_us = 0.0;
    double pose_us = 0.0;
    double geo_us = 0.0;     // 我们：clip + batch；参考：顶点生成 + 裁剪
    double total_us = 0.0;
    double upload_bytes = 0.0;
    double vertices = 0.0;   // 参考：每帧生成的顶点数；我们：CPU 裁剪顶点数
};

struct AssetResult
{
    std::string name;
    uint32_t bones = 0;
    uint32_t slots = 0;
    uint32_t anims = 0;
    uint32_t iks = 0;
    uint32_t transforms = 0;
    uint32_t paths = 0;
    uint64_t static_gpu_bytes = 0;   // 我们一次性上传、常驻显存的静态数据（VB + 影响流 + UV + IB + 绘制项）
    SideResult ours;
    SideResult ref;
};

constexpr float kDt = 1.0f / 60.0f;
constexpr uint32_t kSwitchEvery = 60;

// ---- 我们 ----
bool RunOurs(const SpineGpuAsset& asset, uint32_t frames, SideResult& out, std::string& error)
{
    SpineGpuRuntimeData data;
    SpineImportResult r = SpineGpuBuildRuntimeData(asset, data);
    if (!r.ok)
    {
        error = r.error;
        return false;
    }
    SpineGpuSkeletonState state;
    r = SpineGpuInitSkeletonState(asset, data, state);
    if (!r.ok)
    {
        error = r.error;
        return false;
    }
    SpineGpuBatchAssetInfo info;
    SpineGpuBuildBatchAssetInfo(asset, info);
    SpineGpuAnimationStateData mix;
    mix.default_mix = 0.2f;
    SpineGpuAnimationState anim(asset, data, &mix);
    const uint32_t anim_count = static_cast<uint32_t>(asset.anims.size());
    anim.SetAnimation(0, 0, true);
    SpineGpuPoseResult pose;
    SpineGpuSkeletonClipping clipper;
    SpineGpuClipFrameResult clip;
    SpineGpuBatch batch;
    SpineGpuGpuInst inst {};
    for (int i = 0; i < 16; i += 5)
    {
        inst.clip_from_world[i] = 1.0f;   // 单位阵：拼包只抄进去，值不影响成本
    }
    inst.root_abcd[0] = inst.root_abcd[3] = 1.0f;
    const bool tint_black = (asset.header.flags & SpineGpuFlags::kUsesTintBlack) != 0u;
    double anim_us = 0, pose_us = 0, geo_us = 0, bytes = 0, verts = 0;
    for (uint32_t f = 0; f < frames; ++f)
    {
        if (f > 0 && f % kSwitchEvery == 0 && anim_count > 1)
        {
            anim.SetAnimation(0, (f / kSwitchEvery) % anim_count, true);
        }
        auto t0 = Clock::now();
        anim.Update(kDt);
        r = anim.Apply(state);
        anim_us += UsSince(t0);
        if (!r.ok)
        {
            error = "apply: " + r.error;
            return false;
        }
        t0 = Clock::now();
        r = SpineGpuUpdateWorld(asset, data, state, pose);
        pose_us += UsSince(t0);
        if (!r.ok)
        {
            error = "pose: " + r.error;
            return false;
        }
        t0 = Clock::now();
        r = SpineGpuClipFrame(asset, state, pose, clipper, clip);
        if (r.ok)
        {
            batch.Clear();
            SpineGpuBatchRange range;
            r = SpineGpuAppendBatchInstance(asset, info, pose, &clip, inst, tint_black, batch, range);
        }
        geo_us += UsSince(t0);
        if (!r.ok)
        {
            error = "clip/batch: " + r.error;
            return false;
        }
        bytes += static_cast<double>(batch.bones.size() * sizeof(SpineGpuBoneSample) +
                                     batch.draws.size() * sizeof(SpineGpuGpuDraw) +
                                     batch.insts.size() * sizeof(SpineGpuGpuInst) + batch.deform.size() * sizeof(float) +
                                     batch.cpu_verts.size() * sizeof(SpineGpuCpuVert) +
                                     batch.cpu_index.size() * sizeof(uint16_t));
        verts += static_cast<double>(batch.cpu_verts.size());
    }
    const double n = frames;
    out.anim_us = anim_us / n;
    out.pose_us = pose_us / n;
    out.geo_us = geo_us / n;
    out.total_us = out.anim_us + out.pose_us + out.geo_us;
    out.upload_bytes = bytes / n;
    out.vertices = verts / n;
    return true;
}

// ---- spine-cpp 的 CPU 网格路径 ----
struct RefVertex
{
    float x, y, u, v;
    uint32_t rgba;
};
inline uint32_t PackColor(float r, float g, float b, float a)
{
    const auto c = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

bool RunRef(spine::SkeletonData* data, uint32_t frames, SideResult& out, std::string& error)
{
    spine::Skeleton skeleton(data);
    spine::AnimationStateData state_data(data);
    state_data.setDefaultMix(0.2f);
    spine::AnimationState state(&state_data);
    spine::Vector<spine::Animation*>& anims = data->getAnimations();
    if (anims.size() == 0)
    {
        error = "no animations";
        return false;
    }
    state.setAnimation(0, anims[0], true);
    spine::SkeletonClipping clipper;
    spine::Vector<float> world;
    std::vector<RefVertex> vb;
    std::vector<uint16_t> ib;
    std::vector<uint32_t> dark;   // 有暗色的槽位多一个 rgba8（spine 的 two-color tint）
    static unsigned short kQuad[6] = {0, 1, 2, 2, 3, 0};
    const uint32_t anim_count = static_cast<uint32_t>(anims.size());
    double anim_us = 0, pose_us = 0, geo_us = 0, bytes = 0, verts = 0;
    for (uint32_t f = 0; f < frames; ++f)
    {
        if (f > 0 && f % kSwitchEvery == 0 && anim_count > 1)
        {
            state.setAnimation(0, anims[(f / kSwitchEvery) % anim_count], true);
        }
        auto t0 = Clock::now();
        state.update(kDt);
        state.apply(skeleton);
        anim_us += UsSince(t0);
        t0 = Clock::now();
        skeleton.updateWorldTransform();
        pose_us += UsSince(t0);

        t0 = Clock::now();
        vb.clear();
        ib.clear();
        dark.clear();
        const spine::Color& sc = skeleton.getColor();
        spine::Vector<spine::Slot*>& order = skeleton.getDrawOrder();
        for (size_t i = 0; i < order.size(); ++i)
        {
            spine::Slot& slot = *order[i];
            spine::Attachment* att = slot.getAttachment();
            if (att == nullptr)
            {
                clipper.clipEnd(slot);
                continue;
            }
            if (slot.getColor().a == 0.0f)
            {
                clipper.clipEnd(slot);
                continue;
            }
            float* positions = nullptr;
            float* uvs = nullptr;
            unsigned short* tris = nullptr;
            size_t vcount = 0;
            size_t tcount = 0;
            const spine::Color* ac = nullptr;
            if (att->getRTTI().isExactly(spine::RegionAttachment::rtti))
            {
                auto* ra = static_cast<spine::RegionAttachment*>(att);
                world.setSize(8, 0.0f);
                ra->computeWorldVertices(slot.getBone(), world, 0, 2);
                positions = world.buffer();
                uvs = ra->getUVs().buffer();
                tris = kQuad;
                vcount = 4;
                tcount = 6;
                ac = &ra->getColor();
            }
            else if (att->getRTTI().isExactly(spine::MeshAttachment::rtti))
            {
                auto* ma = static_cast<spine::MeshAttachment*>(att);
                const size_t len = ma->getWorldVerticesLength();
                world.setSize(len, 0.0f);
                ma->computeWorldVertices(slot, 0, len, world, 0, 2);
                positions = world.buffer();
                uvs = ma->getUVs().buffer();
                tris = ma->getTriangles().buffer();
                vcount = len / 2;
                tcount = ma->getTriangles().size();
                ac = &ma->getColor();
            }
            else if (att->getRTTI().isExactly(spine::ClippingAttachment::rtti))
            {
                clipper.clipStart(slot, static_cast<spine::ClippingAttachment*>(att));
                continue;
            }
            else
            {
                clipper.clipEnd(slot);
                continue;
            }
            if (clipper.isClipping())
            {
                clipper.clipTriangles(positions, tris, tcount, uvs, 2);
                positions = clipper.getClippedVertices().buffer();
                uvs = clipper.getClippedUVs().buffer();
                tris = clipper.getClippedTriangles().buffer();
                vcount = clipper.getClippedVertices().size() / 2;
                tcount = clipper.getClippedTriangles().size();
            }
            const spine::Color& lc = slot.getColor();
            const uint32_t rgba = PackColor(sc.r * lc.r * ac->r, sc.g * lc.g * ac->g, sc.b * lc.b * ac->b,
                                            sc.a * lc.a * ac->a);
            const uint16_t base = static_cast<uint16_t>(vb.size());
            for (size_t v = 0; v < vcount; ++v)
            {
                vb.push_back(RefVertex {positions[v * 2], positions[v * 2 + 1], uvs[v * 2], uvs[v * 2 + 1], rgba});
            }
            if (slot.hasDarkColor())
            {
                const spine::Color& dc = slot.getDarkColor();
                dark.insert(dark.end(), vcount, PackColor(dc.r, dc.g, dc.b, 1.0f));
            }
            for (size_t t = 0; t < tcount; ++t)
            {
                ib.push_back(static_cast<uint16_t>(base + tris[t]));
            }
            clipper.clipEnd(slot);
        }
        clipper.clipEnd();
        geo_us += UsSince(t0);
        bytes += static_cast<double>(vb.size() * sizeof(RefVertex) + dark.size() * sizeof(uint32_t) +
                                     ib.size() * sizeof(uint16_t));
        verts += static_cast<double>(vb.size());
    }
    const double n = frames;
    out.anim_us = anim_us / n;
    out.pose_us = pose_us / n;
    out.geo_us = geo_us / n;
    out.total_us = out.anim_us + out.pose_us + out.geo_us;
    out.upload_bytes = bytes / n;
    out.vertices = verts / n;
    return true;
}

double Percentile(std::vector<double> v, double p)
{
    if (v.empty())
    {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    return v[i];
}

}   // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 不缓冲：见文件头
#if defined(_DEBUG)
    std::printf("⚠️ Debug 构建：下面的数字没有意义（STL 迭代器检查 + 无优化）。用 --config Release 编。\n");
#endif
    if (argc < 2)
    {
        std::printf("usage: SpineGpuBenchmark <asset dir> [--frames N] [--limit N] [--csv out.csv] [--top N] [--verbose]\n");
        return 2;
    }
    const fs::path dir = argv[1];
    uint32_t frames = 360;
    size_t limit = 0;
    size_t top = 10;
    std::string csv_path;
    bool verbose = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
        {
            frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        }
        else if (a == "--limit" && i + 1 < argc)
        {
            limit = static_cast<size_t>(std::atoi(argv[++i]));
        }
        else if (a == "--csv" && i + 1 < argc)
        {
            csv_path = argv[++i];
        }
        else if (a == "--verbose")
        {
            verbose = true;
        }
        else if (a == "--top" && i + 1 < argc)
        {
            top = static_cast<size_t>(std::atoi(argv[++i]));
        }
    }

    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    {
        if (ec || !it->is_regular_file(ec))
        {
            continue;
        }
        const std::string n = it->path().filename().string();
        const bool is_skel = n.size() > 11 && n.compare(n.size() - 11, 11, ".skel.bytes") == 0;
        const bool is_json = n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0;
        if (is_skel || is_json)
        {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<AssetResult> results;
    size_t skipped = 0;
    std::string first_skip;
    const auto t_all = Clock::now();
    for (const fs::path& skel_path : files)
    {
        if (limit != 0 && results.size() >= limit)
        {
            break;
        }
        const std::string fname = skel_path.filename().string();
        if (verbose)
        {
            std::fprintf(stderr, "[bench] %s\n", skel_path.string().c_str());
            std::fflush(stderr);
        }
        const bool is_skel = fname.size() > 11 && fname.compare(fname.size() - 11, 11, ".skel.bytes") == 0;
        const std::string stem = fname.substr(0, fname.size() - (is_skel ? 11 : 5));
        fs::path atlas_path = skel_path.parent_path() / (stem + ".atlas.txt");
        if (!fs::exists(atlas_path, ec))
        {
            atlas_path = skel_path.parent_path() / (stem + ".atlas");
        }
        std::vector<char> skel_bytes;
        std::vector<char> atlas_bytes;
        if (!ReadAll(skel_path, skel_bytes) || !ReadAll(atlas_path, atlas_bytes))
        {
            continue;   // json 但不是骨架（没有同名图集）⇒ 不算资产
        }
        skel_bytes.push_back('\0');   // spine-cpp 的 json 读端要 C 串
        const size_t skel_len = skel_bytes.size() - 1;

        // 我们：解析 → 烘焙 → 写 → 读（= 运行时读 .zspg 的形态）
        SpineAtlas atlas;
        SpineSourceSkeleton src;
        SpineGpuAsset baked;
        std::vector<uint8_t> container;
        SpineGpuAsset asset;
        SpineImportResult r = ParseSpineAtlas(atlas_bytes.data(), atlas_bytes.size(), atlas);
        if (r.ok)
        {
            r = is_skel ? ParseSpineSkeletonBinary(reinterpret_cast<const uint8_t*>(skel_bytes.data()), skel_len, &atlas, src)
                        : ParseSpineSkeletonJson(skel_bytes.data(), skel_len, &atlas, src);
        }
        if (r.ok)
        {
            r = BakeSpineGpuAsset(src, baked);
        }
        if (r.ok)
        {
            r = WriteSpineGpuContainer(baked, container);
        }
        if (r.ok)
        {
            r = ReadSpineGpuContainer(container.data(), container.size(), asset);
        }
        if (!r.ok || asset.anims.empty())
        {
            ++skipped;
            if (first_skip.empty())
            {
                first_skip = fname + ": " + (r.ok ? std::string("no animations") : r.error);
            }
            continue;
        }

        RefAttachmentLoader loader(&atlas);
        spine::SkeletonBinary bin(&loader);
        spine::SkeletonJson json(&loader);
        spine::SkeletonData* ref = is_skel ? bin.readSkeletonData(reinterpret_cast<const unsigned char*>(skel_bytes.data()),
                                                                  static_cast<int>(skel_len))
                                           : json.readSkeletonData(skel_bytes.data());
        if (ref == nullptr)
        {
            ++skipped;
            continue;
        }

        AssetResult ar;
        ar.name = skel_path.filename().string();
        ar.bones = asset.header.bone_count;
        ar.slots = asset.header.slot_count;
        ar.anims = static_cast<uint32_t>(asset.anims.size());
        ar.iks = static_cast<uint32_t>(asset.iks.size());
        ar.transforms = static_cast<uint32_t>(asset.transforms.size());
        ar.paths = static_cast<uint32_t>(asset.paths.size());
        ar.static_gpu_bytes = asset.vertex_buffer.size() + asset.influence_bind.size() * sizeof(float) +
                              asset.influence_bone.size() * sizeof(SpineInfluenceBone) +
                              asset.uv_buffer.size() * sizeof(float) + asset.index_buffer.size() * sizeof(uint16_t) +
                              asset.draw_items.size() * sizeof(SpineDrawItem);
        std::string err;
        // 先各跑一遍热身（指令 / 数据缓存、分配器），再正式计时
        SideResult warm;
        const bool ok_ours = RunOurs(asset, 30, warm, err) && RunOurs(asset, frames, ar.ours, err);
        std::string err_ref;
        const bool ok_ref = ok_ours && RunRef(ref, 30, warm, err_ref) && RunRef(ref, frames, ar.ref, err_ref);
        delete ref;
        if (!ok_ours || !ok_ref)
        {
            ++skipped;
            if (first_skip.empty())
            {
                first_skip = fname + ": " + (ok_ours ? err_ref : err);
            }
            continue;
        }
        results.push_back(std::move(ar));
    }
    const double wall_s = std::chrono::duration<double>(Clock::now() - t_all).count();

    if (results.empty())
    {
        std::printf("没有可测的资产（跳过 %zu：%s）\n", skipped, first_skip.c_str());
        return 1;
    }

    // ---- 汇总 ----
    const auto collect = [&](auto get) {
        std::vector<double> v;
        v.reserve(results.size());
        for (const AssetResult& a : results)
        {
            v.push_back(get(a));
        }
        return v;
    };
    const auto sum = [](const std::vector<double>& v) {
        double s = 0;
        for (double x : v)
        {
            s += x;
        }
        return s;
    };
    const auto row = [&](const char* label, auto get_ours, auto get_ref, const char* unit, double scale) {
        const std::vector<double> o = collect(get_ours);
        const std::vector<double> rf = collect(get_ref);
        const double so = sum(o), sr = sum(rf);
        std::printf("  %-14s  我们 中位 %9.2f  p90 %9.2f  合计 %11.1f | spine-cpp 中位 %9.2f  p90 %9.2f  合计 %11.1f %s | 比值(参考/我们) %.2fx\n",
                    label, Percentile(o, 0.5) * scale, Percentile(o, 0.9) * scale, so * scale,
                    Percentile(rf, 0.5) * scale, Percentile(rf, 0.9) * scale, sr * scale, unit, so > 0 ? sr / so : 0.0);
    };
    std::printf("=== GPU-Spine CPU 基准：%zu 个资产（跳过 %zu），每资产 %u 帧 @60Hz，墙钟 %.1f s ===\n", results.size(),
                skipped, frames, wall_s);
    if (!first_skip.empty())
    {
        std::printf("  第一个跳过的：%s\n", first_skip.c_str());
    }
    std::printf("每实例每帧（µs；合计 = 每个资产各放 1 个实例时一帧的总成本）：\n");
    row("动画 anim", [](const AssetResult& a) { return a.ours.anim_us; }, [](const AssetResult& a) { return a.ref.anim_us; },
        "µs", 1.0);
    row("骨骼 pose", [](const AssetResult& a) { return a.ours.pose_us; }, [](const AssetResult& a) { return a.ref.pose_us; },
        "µs", 1.0);
    row("几何 geo", [](const AssetResult& a) { return a.ours.geo_us; }, [](const AssetResult& a) { return a.ref.geo_us; },
        "µs", 1.0);
    row("总计 total", [](const AssetResult& a) { return a.ours.total_us; },
        [](const AssetResult& a) { return a.ref.total_us; }, "µs", 1.0);
    std::printf("每实例每帧上传（KB）：\n");
    row("上传 upload", [](const AssetResult& a) { return a.ours.upload_bytes; },
        [](const AssetResult& a) { return a.ref.upload_bytes; }, "KB", 1.0 / 1024.0);
    row("CPU 顶点", [](const AssetResult& a) { return a.ours.vertices; }, [](const AssetResult& a) { return a.ref.vertices; },
        "个", 1.0);
    {
        const std::vector<double> st = collect([](const AssetResult& a) { return static_cast<double>(a.static_gpu_bytes); });
        std::printf("我们的静态显存（一次性上传，同资产实例共享）：中位 %.1f KB  p90 %.1f KB  合计 %.1f MB\n",
                    Percentile(st, 0.5) / 1024.0, Percentile(st, 0.9) / 1024.0, sum(st) / (1024.0 * 1024.0));
    }
    {
        // 1 ms 预算能放多少个实例（单线程，按合计 / 资产数的平均实例算）
        const double mo = sum(collect([](const AssetResult& a) { return a.ours.total_us; })) / results.size();
        const double mr = sum(collect([](const AssetResult& a) { return a.ref.total_us; })) / results.size();
        const double uo = sum(collect([](const AssetResult& a) { return a.ours.upload_bytes; })) / results.size();
        const double ur = sum(collect([](const AssetResult& a) { return a.ref.upload_bytes; })) / results.size();
        std::printf("平均实例：我们 %.2f µs + %.1f KB/帧，spine-cpp %.2f µs + %.1f KB/帧 ⇒ 1 ms 单线程预算 ≈ 我们 %.0f 个 / spine-cpp %.0f 个；"
                    "100 个实例 @60Hz 上传 ≈ 我们 %.1f MB/s / spine-cpp %.1f MB/s\n",
                    mo, uo / 1024.0, mr, ur / 1024.0, 1000.0 / mo, 1000.0 / mr, uo * 100 * 60 / (1024.0 * 1024.0),
                    ur * 100 * 60 / (1024.0 * 1024.0));
    }
    // 最重的几个（按 spine-cpp 总成本）
    std::vector<const AssetResult*> sorted;
    for (const AssetResult& a : results)
    {
        sorted.push_back(&a);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const AssetResult* a, const AssetResult* b) { return a->ref.total_us > b->ref.total_us; });
    std::printf("最重的 %zu 个（按 spine-cpp 总成本）：\n", std::min(top, sorted.size()));
    for (size_t i = 0; i < std::min(top, sorted.size()); ++i)
    {
        const AssetResult& a = *sorted[i];
        std::printf("  %-40s 骨 %3u 槽 %3u ik/tf/path %u/%u/%u | 我们 %7.2f µs（pose %6.2f）%6.1f KB | spine-cpp %7.2f µs（pose %5.2f）%6.1f KB（%5.0f 顶点）\n",
                    a.name.substr(0, 40).c_str(), a.bones, a.slots, a.iks, a.transforms, a.paths, a.ours.total_us,
                    a.ours.pose_us, a.ours.upload_bytes / 1024.0, a.ref.total_us, a.ref.pose_us, a.ref.upload_bytes / 1024.0,
                    a.ref.vertices);
    }
    if (!csv_path.empty())
    {
        std::ofstream csv(csv_path, std::ios::trunc);
        csv << "asset,bones,slots,anims,iks,transforms,paths,static_gpu_bytes,ours_anim_us,ours_pose_us,ours_geo_us,ours_total_us,ours_upload_bytes,"
               "ours_cpu_verts,ref_anim_us,ref_pose_us,ref_geo_us,ref_total_us,ref_upload_bytes,ref_verts\n";
        for (const AssetResult& a : results)
        {
            csv << a.name << ',' << a.bones << ',' << a.slots << ',' << a.anims << ',' << a.iks << ',' << a.transforms
                << ',' << a.paths << ',' << a.static_gpu_bytes << ','
                << a.ours.anim_us << ',' << a.ours.pose_us << ',' << a.ours.geo_us << ',' << a.ours.total_us << ','
                << a.ours.upload_bytes << ',' << a.ours.vertices << ',' << a.ref.anim_us << ',' << a.ref.pose_us << ','
                << a.ref.geo_us << ',' << a.ref.total_us << ',' << a.ref.upload_bytes << ',' << a.ref.vertices << '\n';
        }
        std::printf("逐资产明细：%s\n", csv_path.c_str());
    }
    return 0;
}
