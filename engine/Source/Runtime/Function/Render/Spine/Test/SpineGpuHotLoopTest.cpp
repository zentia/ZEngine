// =====================================================================
// GPU-Spine 热路径测试：**稳态每帧 0 堆分配** + 曲线缓存（规格 §17）
// ---------------------------------------------------------------------
// 为什么单独一个 exe（不并入 SpineGpuImportTest）：
//   这条断言要**替换全局 `operator new`** 才能数得准，而 ZRuntime 的
//   MemoryManager 已经把全局 new/delete 接管到 mimalloc ⇒ 链 ZRuntime 的 exe
//   再定义一份就是 LNK2005。所以这里和基准一样，**只编 Spine 核心那几个 .cpp**
//   （它们只依赖标准库 + rapidjson），不链 ZRuntime。
//
// ⚠️ 本目标整体以 `_ITERATOR_DEBUG_LEVEL=0` 编译（见 Test/CMakeLists.txt）：
//   MSVC 的 Debug 迭代器调试会给**每个**容器的构造单独 malloc 一个 16 字节的
//   `_Container_proxy`。那不是数据缓冲，Release 下根本不存在，但不关掉它就让
//   "稳态 0 分配"在 Debug 下稳定红（实测 8 次/帧：apply 7 + pose 1）。本 exe
//   不链任何预编译库，整目标统一关掉不会有 _IDL 级别混用的 ABI 问题。
//   ⇒ 谁把这条 define 摘了，Debug 下这条断言就会以假阳性失败。
//
// 钉的是什么：
//   热路径里每帧 malloc/free 是这套代码犯过的最贵的一类错 —— deform 展开
//   （每条时间轴每帧 2~4 次 `std::vector`）、裁剪几何（每个被裁槽位每帧 4 次：
//   世界坐标 / UV / 三角形 / 拼错误信息）、曲线求值（每次求值重建 18 float 的表 +
//   O(段数) 的段号扫描）。它们**不报错、不画错**，唯一表现就是"慢"
//   ⇒ 只能靠断言挡住：谁在热路径里加回一个局部 `std::vector`，这条就红。
//
// Build / run:
//   cmake -B build -DZENGINE_BUILD_SPINE_IMPORT_TEST=ON
//   cmake --build build --target SpineGpuHotLoopTest --config Release
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuAnimSolver.h"
#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"
#include "Runtime/Function/Render/Spine/SpineGpuBatch.h"
#include "Runtime/Function/Render/Spine/SpineGpuClipper.h"
#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"
#include "Runtime/Function/Render/Spine/SpineGpuSkeletonState.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// =====================================================================
// 每帧堆分配计数：替换全局 operator new/delete，只在开关打开时累加
// =====================================================================
namespace
{
bool g_alloc_counting = false;
long long g_alloc_calls = 0;
}   // namespace

void* operator new(std::size_t n)
{
    void* p = std::malloc(n != 0u ? n : 1u);
    if (p == nullptr)
    {
        throw std::bad_alloc();
    }
    if (g_alloc_counting)
    {
        ++g_alloc_calls;
    }
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }

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

// ---- 夹具：一份 3.8 json，故意把三类热路径都塞进去 ----
//   * rotate 3 帧（stepped + bezier）          ⇒ 曲线求值
//   * weighted mesh 上的 2 帧 deform           ⇒ deform 稀疏增量展开
//   * 一条裁剪附件，作用域盖到那个 mesh        ⇒ 裁剪几何
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

const char* kSkeleton = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "clip" },
    { "name": "s2", "bone": "hip", "attachment": "mesh" }
  ],
  "skins": { "default": {
    "s0": { "quad": { "x": 0, "y": 0, "width": 64, "height": 32, "rotation": 0 } },
    "s1": { "clip": { "type": "clipping", %ENDFIELD%%CLIPVERTS% } },
    "s2": { "mesh": {
      "type": "mesh",
      "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
      "triangles": [ 0,1,2, 0,2,3 ],
      "vertices": [ 1, 0,-10,-5,1,
                    2, 1, 20,10,0.6, 0,5,7,0.4,
                    1, 1, 3,4,1,
                    1, 0, 8,2,1 ]
    } }
  } },
  "animations": { "walk": {
    "bones": { "root": { "rotate": [
      { "time": 0, "angle": 10, "curve": "stepped" },
      { "time": 0.5, "angle": 190, "curve": [ 0.25, 0.5, 0.75, 0.8 ] },
      { "time": 1, "angle": -20 }
    ] } },
    "deform": { "default": { "s2": { "mesh": [
      { "time": 0, "offset": 0, "vertices": [ 0,0, 1,1, 2,2, 3,3, 4,4 ] },
      { "time": 1, "offset": 0, "vertices": [ 5,5, 6,6, 7,7, 8,8, 9,9 ] }
    ] } } }
  } }
})";

// 三角形：3 个顶点 ⇒ 三角化出 1 块，**不会**触发"合并后删空块"那条路径。
const char* kClipVertsTriangle = "\"vertexCount\": 3, \"vertices\": [ 0,0, 10,0, 5,10 ]";
// 四边形：4 个顶点 ⇒ 2 块，且会走合并 / 删空块（`Decompose` 的 `resize` 会真的释放空块
// 的缓冲，下一帧再补回来 ⇒ 稳态每帧还剩个位数次分配）。留一个"预算"断言把它钉住。
const char* kClipVertsQuad = "\"vertexCount\": 4, \"vertices\": [ 0,0, 10,0, 10,10, 0,10 ]";

std::string BuildSkeleton(const char* clip_verts, const char* end_field = "\"end\": \"s2\", ")
{
    std::string s(kSkeleton);
    size_t at = s.find("%CLIPVERTS%");
    if (at == std::string::npos)
    {
        return std::string();
    }
    s.replace(at, std::string("%CLIPVERTS%").size(), clip_verts);
    at = s.find("%ENDFIELD%");
    if (at == std::string::npos)
    {
        return std::string();
    }
    s.replace(at, std::string("%ENDFIELD%").size(), end_field);
    return s;
}

// 跑 `frames` 帧，只数**后半段**（前半段留给"首次分配扩容"）⇒ 数出来就是稳态。
// 返回稳态窗口内的分配次数。
long long RunHotLoop(const SpineGpuAsset& a, bool with_clip, uint32_t frames, std::string& error)
{
    SpineGpuRuntimeData data;
    SpineGpuSkeletonState state;
    SpineGpuPoseResult pose;
    SpineGpuSkeletonClipping clipper;
    SpineGpuClipFrameResult clip;
    SpineImportResult r = SpineGpuBuildRuntimeData(a, data);
    if (r.ok)
    {
        r = SpineGpuInitSkeletonState(a, data, state);
    }
    if (!r.ok)
    {
        error = r.error;
        return -1;
    }
    SpineGpuAnimationState anim(a, data, nullptr);
    anim.SetAnimation(0, 0, true);
    const uint32_t half = frames / 2u;
    for (uint32_t f = 0; f < frames; ++f)
    {
        if (f == half)
        {
            g_alloc_calls = 0;
            g_alloc_counting = true;
        }
        anim.Update(1.0f / 60.0f);
        r = anim.Apply(state);
        if (!r.ok)
        {
            g_alloc_counting = false;
            error = "apply: " + r.error;
            return -1;
        }
        r = SpineGpuUpdateWorld(a, data, state, pose);
        if (!r.ok)
        {
            g_alloc_counting = false;
            error = "pose: " + r.error;
            return -1;
        }
        if (with_clip)
        {
            r = SpineGpuClipFrame(a, state, pose, clipper, clip);
            if (!r.ok)
            {
                g_alloc_counting = false;
                error = "clip: " + r.error;
                return -1;
            }
        }
    }
    g_alloc_counting = false;
    return g_alloc_calls;
}
}   // namespace

int main()
{
    std::printf("=== GPU-Spine hot loop test (steady-state allocations + curve cache) ===\n");

    SpineAtlas atlas;
    SpineImportResult ra = ParseSpineAtlas(kAtlas, std::string(kAtlas).size(), atlas);
    Check(ra.ok, "atlas 解析成功");

    const std::string skel_tri = BuildSkeleton(kClipVertsTriangle);
    const std::string skel_quad = BuildSkeleton(kClipVertsQuad);
    Check(!skel_tri.empty() && !skel_quad.empty(), "夹具替换成功");

    SpineSourceSkeleton src;
    SpineImportResult rj = ParseSpineSkeletonJson(skel_tri.data(), skel_tri.size(), &atlas, src);
    Check(rj.ok, "json 解析成功");
    if (!rj.ok)
    {
        std::printf("  error: %s\n", rj.error.c_str());
        return 1;
    }

    SpineGpuAsset asset;
    SpineImportResult rb = BakeSpineGpuAsset(src, asset);
    Check(rb.ok, "bake 成功");
    if (!rb.ok)
    {
        std::printf("  error: %s\n", rb.error.c_str());
        return 1;
    }

    // ---- 夹具自证：三类热路径都在 ----
    {
        uint32_t n_deform = 0, n_rotate = 0;
        for (const SpineGpuTimeline& t : asset.timelines)
        {
            if (t.type == static_cast<uint16_t>(SpineTimelineType::kDeform))
            {
                ++n_deform;
            }
            if (t.type == static_cast<uint16_t>(SpineTimelineType::kRotate) && t.bezier_count >= 1u)
            {
                ++n_rotate;
            }
        }
        CheckEq(static_cast<long long>(n_deform), 1LL, "夹具: 1 条 deform 时间轴");
        CheckEq(static_cast<long long>(n_rotate), 1LL, "夹具: 1 条带 bezier 的 rotate 时间轴");
        CheckEq(static_cast<long long>(asset.clip_polygons.size()), 1LL, "夹具: 1 条裁剪多边形");
    }

    // ---- 1. 曲线缓存：加载时展开一次，求值时只读 ----
    // 2026-10-23 之前求值路径每次都要：从段 0 数到当前段（O(段数)）+ 重建 18 个 float
    // 的前向差分表 ⇒ anim 段比参考实现慢 1.8x。
    {
        CheckEq(static_cast<long long>(asset.bezier_tables.size()),
                static_cast<long long>(asset.beziers.size()) * static_cast<long long>(kSpineBezierTableStride),
                "缓存: bezier_tables = beziers × 18");
        CheckEq(static_cast<long long>(asset.curve_bezier_index.size()),
                static_cast<long long>(asset.curve_types.size()), "缓存: 段→表下标与 curve_types 一一对应");

        // 表本身：手算 9 个采样点的第 1 个（t = 0.1），控制点 (0.25, 0.5) / (0.75, 0.8)
        //   x(0.1) = 3·0.81·0.1·0.25 + 3·0.9·0.01·0.75 + 0.001 = 0.082
        //   y(0.1) = 3·0.81·0.1·0.5  + 3·0.9·0.01·0.8  + 0.001 = 0.1441
        if (asset.bezier_tables.size() >= kSpineBezierTableStride)
        {
            Check(std::fabs(asset.bezier_tables[0] - 0.082f) < 1e-6f, "缓存: 表[0] = x(0.1) = 0.082");
            Check(std::fabs(asset.bezier_tables[1] - 0.1441f) < 1e-6f, "缓存: 表[1] = y(0.1) = 0.1441");
            Check(asset.bezier_tables[0] < asset.bezier_tables[2] &&
                      asset.bezier_tables[2] < asset.bezier_tables[16],
                  "缓存: 表的 x 单调递增（前向差分展开错了不单调，且不报错）");
        }

        // 求值时真的用上了表：time = 0.75 ⇒ percent = 1 - (0.75 - 1)/(0.5 - 1) = 0.5，
        // bezier 在 t = 0.5 处 y = 3·0.25·0.5·0.5 + 3·0.5·0.25·0.8 + 0.125 = 0.6125
        uint32_t tli = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < asset.timelines.size(); ++i)
        {
            const SpineGpuTimeline& t = asset.timelines[i];
            if (t.type == static_cast<uint16_t>(SpineTimelineType::kRotate) && t.frame_count >= 3u &&
                t.bezier_count >= 1u)
            {
                tli = i;
                break;
            }
        }
        Check(tli != 0xFFFFFFFFu, "缓存: 找到带 bezier 的 rotate 时间轴");
        if (tli != 0xFFFFFFFFu)
        {
            const SpineGpuTimeline& tl = asset.timelines[tli];
            const uint32_t entries = SpineTimelineEntries(SpineTimelineType::kRotate);
            const uint32_t cur =
                SpineGpuBinarySearchFrames(asset.frames.data() + tl.frame_first, tl.frame_count, entries, 0.75f);
            float got = -1.0f;
            const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tli, cur, 0.75f, got);
            Check(cr.ok, "缓存: bezier 段求值成功");
            if (!cr.ok)
            {
                std::printf("  error: %s\n", cr.error.c_str());
            }
            Check(std::fabs(got - 0.6125f) < 2e-3f,
                  "缓存: percent 0.5 处 = 0.6125（手算；不是 0.5 ⇒ 曲线真的生效了）");

            // 没建缓存 ⇒ **必须报错**。静默退回"从段 0 扫一遍"只是慢，而"慢"正是这类
            // bug 的唯一表现 ⇒ 让它变成一条错误信息。
            SpineGpuAsset nocache = asset;
            nocache.curve_bezier_index.clear();
            float dummy = -1.0f;
            const SpineImportResult nr = SpineGpuCurvePercentAt(nocache, tl, tli, cur, 0.75f, dummy);
            Check(!nr.ok, "缓存: 没建缓存 ⇒ 报错（不静默退回 O(段数) 的慢路径）");
            std::printf("  (预期报错) %s\n", nr.error.c_str());
        }

        // 表是**派生**数据、不落盘：容器里只有 4 个控制点 ⇒ 读回来必须重建出一模一样的表
        std::vector<uint8_t> bytes;
        Check(WriteSpineGpuContainer(asset, bytes).ok, "缓存: 写容器");
        SpineGpuAsset back;
        Check(!bytes.empty() && ReadSpineGpuContainer(bytes.data(), bytes.size(), back).ok, "缓存: 读回容器");
        Check(back.bezier_tables == asset.bezier_tables, "缓存: 读回来重建的表逐值相同");
        Check(back.curve_bezier_index == asset.curve_bezier_index, "缓存: 读回来重建的段→表下标相同");
    }

    // ---- 2. 稳态每帧 0 堆分配 ----
    constexpr uint32_t kFrames = 60u;              // 数后 30 帧
    constexpr long long kBudgetPerFrame = 2LL;     // 见下面"四边形"那条（实测 3 次 / 30 帧）
    {
        std::string error;
        const long long n_noclip = RunHotLoop(asset, false, kFrames, error);
        Check(n_noclip >= 0, "0 分配: 动画 + 骨架跑通");
        if (n_noclip < 0)
        {
            std::printf("  error: %s\n", error.c_str());
        }
        else
        {
            CheckEq(n_noclip, 0LL, "0 分配: 动画 + 骨架，稳态 30 帧（deform 展开 / 曲线求值）");
        }

        error.clear();
        const long long n_tri = RunHotLoop(asset, true, kFrames, error);
        Check(n_tri >= 0, "0 分配: 裁剪（三角形）跑通");
        if (n_tri < 0)
        {
            std::printf("  error: %s\n", error.c_str());
        }
        else
        {
            CheckEq(n_tri, 0LL, "0 分配: 动画 + 骨架 + 裁剪（三角形），稳态 30 帧（裁剪几何）");
        }

        // 四边形：会走"合并后删空块"（`Decompose()` 末尾的 `resize` 会真的释放空块的缓冲，
        // 下一帧再 `emplace_back()` 补回来）⇒ 稳态还剩一点点残余（实测 3 次 / 30 帧）。
        // 留个预算只是为了挡住"退回每帧几十次"那种回归 —— 修复前实测是 25 次/帧。
        SpineSourceSkeleton src_q;
        SpineGpuAsset quad;
        const bool quad_ok = ParseSpineSkeletonJson(skel_quad.data(), skel_quad.size(), &atlas, src_q).ok &&
                             BakeSpineGpuAsset(src_q, quad).ok;
        Check(quad_ok, "0 分配: 四边形裁剪夹具 bake 成功");
        if (quad_ok)
        {
            error.clear();
            const long long n_quad = RunHotLoop(quad, true, kFrames, error);
            Check(n_quad >= 0, "0 分配: 裁剪（四边形）跑通");
            if (n_quad >= 0)
            {
                Check(n_quad <= kBudgetPerFrame * static_cast<long long>(kFrames / 2u),
                      "0 分配: 裁剪（四边形）稳态 ≤ 6 次/帧（空块槽位的已知残余）");
                std::printf("  四边形裁剪：稳态 %lld 次 / 30 帧\n", n_quad);
            }
            else
            {
                std::printf("  error: %s\n", error.c_str());
            }
        }
    }

    std::printf("=== verdict: %s (%d/%d checks) ===\n", g_failed == 0 ? "PASS" : "FAIL", g_checks - g_failed,
                g_checks);
    return g_failed == 0 ? 0 : 1;
}
