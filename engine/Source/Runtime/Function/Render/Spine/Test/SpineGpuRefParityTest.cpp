// =====================================================================
// GPU-Spine 骨骼解算 ↔ 参考实现（spine-cpp **3.8**）逐骨骼对拍
// ---------------------------------------------------------------------
// 为什么必须有这一条：
//   S4b 抄的是 IK / transform / path 三套**迭代求解器**。它们的期望值
//   **手算不出来**（2 骨骼 IK 是一元二次方程 + 椭圆极值 + 分支），
//   所以"夹具里写死期望值"只会把实现的行为抄回给自己 —— 正是规格 §12 里
//   "期望值手算，不从实现里抄回去"要防的那种循环论证。
//   ⇒ 唯一能真正钉住转录错误的是**把参考实现跑起来比**。
//
// 参考实现来源：G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp（3.8），
// 由 `build/spine_ref_probe` 编成 `SpineReference.lib`。
//
// 用法：
//   SpineGpuRefParityTest <目录> [--limit N] [--verbose]
// 目录里找 `*.skel.bytes`，配同名 `.atlas`；能烘焙的才参与对拍
// （烘焙失败是**资产**问题，不计入对拍失败 —— 另有 782 资产回归盯着它）。
//
// 比的是什么：每个动画 × 多个时间点 × 每根骨骼的 (a,b,c,d,worldX,worldY)。
// =====================================================================

#include <spine/Extension.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonJson.h>
#include <spine/AttachmentLoader.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>
#include <spine/BoundingBoxAttachment.h>
#include <spine/PathAttachment.h>
#include <spine/PointAttachment.h>
#include <spine/ClippingAttachment.h>
#include <spine/IkConstraint.h>
#include <spine/TransformConstraint.h>
#include <spine/PathConstraint.h>
#include <spine/Updatable.h>
#include <spine/IkConstraintData.h>
#include <spine/TransformConstraintData.h>
#include <spine/PathConstraintData.h>
#include <spine/Animation.h>
#include <spine/AnimationState.h>
#include <spine/AtlasAttachmentLoader.h>
#include <spine/Bone.h>
#include <spine/BoneData.h>
#include <spine/MixBlend.h>
#include <spine/MixDirection.h>
#include <spine/PathConstraint.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonData.h>
#include <spine/Slot.h>
#include <spine/SlotData.h>
#include <spine/Skin.h>

#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------
// getDefaultExtension()
//
// spine-cpp 把"默认分配器由宿主提供"做成了一个**未定义的 extern**：
// `Extension.cpp:49` 只调用它，实现留给了引擎各自 runtime（Unity 那边在
// 别处给）。它不在 SpineReference.lib 里 ⇒ 宿主（本测试）必须给一个。
// `DefaultSpineExtension` 是随库实现的（malloc/free + fopen），直接用。
// ---------------------------------------------------------------------
namespace spine
{
SpineExtension* getDefaultExtension()
{
    static DefaultSpineExtension g_default_extension;
    return &g_default_extension;
}

// -------------------------------------------------------------------
// AtlasAttachmentLoader 的"永不调用"桩
//
// SkeletonBinary.cpp:98 的 `SkeletonBinary(Atlas*)` 里 `new AtlasAttachmentLoader(...)`。
// 我们只用 `SkeletonBinary(AttachmentLoader*)`，但 Debug 下 MSVC 链接器按 **.obj**
// 粒度拉符号 ⇒ 那个构造函数还是得能解析。而 `AtlasAttachmentLoader.cpp` 依赖
// `Atlas.cpp`，后者吃 Unity 的 baselib 头（该头文件在引擎里根本不存在）⇒ 编不过。
// ⇒ 这里补一份空实现；它对应的构造路径我们**永远不走**。
// -------------------------------------------------------------------
AtlasAttachmentLoader::AtlasAttachmentLoader(Atlas*) : _atlas(nullptr) {}
RegionAttachment* AtlasAttachmentLoader::newRegionAttachment(Skin&, const String&, const String&)
{
    return nullptr;
}
MeshAttachment* AtlasAttachmentLoader::newMeshAttachment(Skin&, const String&, const String&) { return nullptr; }
BoundingBoxAttachment* AtlasAttachmentLoader::newBoundingBoxAttachment(Skin&, const String&) { return nullptr; }
PathAttachment* AtlasAttachmentLoader::newPathAttachment(Skin&, const String&) { return nullptr; }
PointAttachment* AtlasAttachmentLoader::newPointAttachment(Skin&, const String&) { return nullptr; }
ClippingAttachment* AtlasAttachmentLoader::newClippingAttachment(Skin&, const String&) { return nullptr; }
void AtlasAttachmentLoader::configureAttachment(Attachment*) {}
AtlasRegion* AtlasAttachmentLoader::findRegion(const String&) { return nullptr; }
const SpineRTTI AtlasAttachmentLoader::rtti(6);
const SpineRTTI& AtlasAttachmentLoader::getRTTI() const { return rtti; }
}   // namespace spine

// ---------------------------------------------------------------------
// 参考侧的"空"附件加载器
//
// 我们只比**骨骼**，不需要图集 ⇒ 不需要 `AtlasAttachmentLoader`。
// 但 path 约束要读 `PathAttachment` 的顶点与 lengths —— 那部分由
// `SkeletonBinary::readVertices()` 填，跟图集无关 ⇒ 空加载器照样对。
// ---------------------------------------------------------------------
class RefAttachmentLoader : public spine::AttachmentLoader
{
public:
    spine::RegionAttachment* newRegionAttachment(spine::Skin& skin, const spine::String& name,
                                                 const spine::String& path) override
    {
        (void)skin;
        spine::RegionAttachment* a = new (__FILE__, __LINE__) spine::RegionAttachment(name);
        a->setPath(path);
        return a;
    }
    spine::MeshAttachment* newMeshAttachment(spine::Skin& skin, const spine::String& name,
                                             const spine::String& path) override
    {
        (void)skin;
        spine::MeshAttachment* a = new (__FILE__, __LINE__) spine::MeshAttachment(name);
        a->setPath(path);
        return a;
    }
    spine::BoundingBoxAttachment* newBoundingBoxAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::BoundingBoxAttachment(name);
    }
    spine::PathAttachment* newPathAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::PathAttachment(name);
    }
    spine::PointAttachment* newPointAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::PointAttachment(name);
    }
    spine::ClippingAttachment* newClippingAttachment(spine::Skin& skin, const spine::String& name) override
    {
        (void)skin;
        return new (__FILE__, __LINE__) spine::ClippingAttachment(name);
    }
    void configureAttachment(spine::Attachment* attachment) override { (void)attachment; }
};

namespace
{

bool ReadFile(const fs::path& p, std::vector<char>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
    {
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    out.resize(n);
    if (n != 0)
    {
        f.read(out.data(), static_cast<std::streamsize>(n));
    }
    return true;
}

// ulp 差：两端都按 float 算，但中间表达式的**结合顺序**不完全一样
// （参考实现写 `1 - (time - frameTime) / (prevTime - frameTime)`，我们用标准写法），
// 所以是"接近逐位"而不是逐位。用相对容差 + 绝对地板。
// 逐步对拍（--trace）可以用 --tol 收紧，用来找"最早出现的浮点分叉"。
float g_tolerance = 1.0e-4f;

bool CloseEnough(float a, float b, float& worst_rel)
{
    if (std::isnan(a) && std::isnan(b))
    {
        return true;
    }
    if (std::isnan(a) || std::isnan(b))
    {
        worst_rel = 1.0e30f;
        return false;
    }
    const float scale = std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));
    const float rel = std::fabs(a - b) / scale;
    if (rel > worst_rel)
    {
        worst_rel = rel;
    }
    // 1e-4 相对：足够抓"用了 3.7 的数学 / 遍历顺序写反 / 约束没跑"，
    // 又不会因为最后几个 ulp 的浮点噪声失败。
    return rel <= g_tolerance;
}

// ---------------------------------------------------------------------
// 逐帧目视验证：把「我们」和「参考实现」的骨架叠在同一张 SVG 上，一帧一帧翻
//
// 为什么值得单独做一个：
//   数值对拍只能告诉你"第 10 根骨骼的 worldY 差 199"，看不出**姿势**错成什么样。
//   而姿势错了（比如 IK 方向反了、path 把尾巴摆到头顶）肉眼**一眼**就看见，
//   并且能立刻指回是哪些骨骼、哪个约束。两端都在同一个进程里 ⇒ 叠图是免费的。
//
// 画什么：**骨骼段**（父骨骼世界坐标 → 自己的世界坐标）。
//   不画 mesh：mesh 要 GPU 蒙皮路径（S6 之后才有），而且 deform 还没解算。
//   骨骼是"所有可见错误的公共上游" —— 骨骼对了，剩下的错误只在顶点级。
// ---------------------------------------------------------------------
struct FlipFrame
{
    float time = 0.0f;
    // 每根骨骼：(worldX, worldY)
    std::vector<float> ours;
    std::vector<float> ref;
    float max_dist = 0.0f;   // 该帧两端同下标骨骼的最大距离（像素/世界单位）
};

bool WriteFlipBook(const fs::path& out_path, const std::string& title, size_t anim_index,
                   const std::vector<std::string>& bone_names, const std::vector<int16_t>& parents,
                   const std::vector<FlipFrame>& frames)
{
    if (frames.empty())
    {
        return false;
    }
    const size_t bone_count = frames[0].ours.size() / 2;

    // 包围盒：两端都算进去，否则其中一端会画出界
    float min_x = 1.0e30f;
    float min_y = 1.0e30f;
    float max_x = -1.0e30f;
    float max_y = -1.0e30f;
    for (const FlipFrame& f : frames)
    {
        for (size_t i = 0; i < bone_count; ++i)
        {
            for (int side = 0; side < 2; ++side)
            {
                const float* src = side == 0 ? f.ours.data() : f.ref.data();
                const float x = src[i * 2];
                const float y = src[i * 2 + 1];
                min_x = std::fmin(min_x, x);
                min_y = std::fmin(min_y, y);
                max_x = std::fmax(max_x, x);
                max_y = std::fmax(max_y, y);
            }
        }
    }
    const float pad = std::fmax(20.0f, (max_x - min_x) * 0.05f);
    min_x -= pad;
    min_y -= pad;
    max_x += pad;
    max_y += pad;

    const double view_w = 900.0;
    const double view_h = 620.0;
    const double sx = view_w / static_cast<double>(max_x - min_x);
    const double sy = view_h / static_cast<double>(max_y - min_y);
    const double s = std::fmin(sx, sy);
    // spine 是 Y 向上，SVG 是 Y 向下 ⇒ 翻 Y
    auto px = [&](float x) { return (static_cast<double>(x) - min_x) * s; };
    auto py = [&](float y) { return (static_cast<double>(max_y) - static_cast<double>(y)) * s; };

    std::string svg;
    for (size_t fi = 0; fi < frames.size(); ++fi)
    {
        const FlipFrame& f = frames[fi];
        char head[256];
        std::snprintf(head, sizeof(head),
                      "<g class=\"frame\" data-f=\"%zu\" data-t=\"%.4f\" data-d=\"%.3f\"%s>", fi,
                      static_cast<double>(f.time), static_cast<double>(f.max_dist),
                      fi == 0 ? "" : " style=\"display:none\"");
        svg += head;
        for (size_t i = 0; i < bone_count; ++i)
        {
            const int16_t p = parents[i];
            if (p < 0 || static_cast<size_t>(p) >= bone_count)
            {
                continue;
            }
            // 参考实现（橙、虚线）在下，我们（蓝、实线）在上
            svg += "<line class=\"ref\" x1=\"" + std::to_string(px(f.ref[p * 2])) + "\" y1=\"" +
                   std::to_string(py(f.ref[p * 2 + 1])) + "\" x2=\"" + std::to_string(px(f.ref[i * 2])) +
                   "\" y2=\"" + std::to_string(py(f.ref[i * 2 + 1])) + "\"/>";
            svg += "<line class=\"ours\" x1=\"" + std::to_string(px(f.ours[p * 2])) + "\" y1=\"" +
                   std::to_string(py(f.ours[p * 2 + 1])) + "\" x2=\"" + std::to_string(px(f.ours[i * 2])) +
                   "\" y2=\"" + std::to_string(py(f.ours[i * 2 + 1])) + "\"/>";
        }
        for (size_t i = 0; i < bone_count; ++i)
        {
            svg += "<circle class=\"joint\" cx=\"" + std::to_string(px(f.ours[i * 2])) + "\" cy=\"" +
                   std::to_string(py(f.ours[i * 2 + 1])) + "\" r=\"1.6\"><title>" +
                   std::to_string(i) + " " + bone_names[i] + "</title></circle>";
        }
        svg += "</g>";
    }

    std::string html =
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\"><title>flip-book: " + title +
        "</title><style>"
        "body{background:#f7f8fa;color:#1c1e21;font:13px/1.5 system-ui,sans-serif;margin:0;padding:16px}"
        "h2{margin:0 0 4px;font-size:15px}"
        ".meta{color:#6b7280;margin-bottom:10px}"
        "svg{background:#fff;border:1px solid #d7dbe0;border-radius:6px;display:block}"
        "line.ref{stroke:#e8590c;stroke-width:2.2;stroke-dasharray:5 3;opacity:.85}"
        "line.ours{stroke:#1c7ed6;stroke-width:1.4}"
        "circle.joint{fill:#1c7ed6}"
        ".bar{display:flex;align-items:center;gap:10px;margin:10px 0}"
        "input[type=range]{flex:1}"
        "button{padding:4px 10px;border:1px solid #c4c9cf;background:#fff;border-radius:4px;cursor:pointer}"
        ".legend span{display:inline-block;margin-right:14px}"
        ".sw{display:inline-block;width:16px;height:3px;vertical-align:middle;margin-right:5px}"
        ".num{font-variant-numeric:tabular-nums}"
        "</style></head><body>"
        "<h2>" +
        title + "</h2><div class=\"meta\">动画 #" + std::to_string(anim_index) + " · " +
        std::to_string(frames.size()) + " 帧 · 骨骼 " + std::to_string(bone_count) +
        " 根 · 蓝=我们，橙虚线=参考实现（spine-cpp 3.8）</div>"
        "<svg id=\"cv\" width=\"" +
        std::to_string(view_w) + "\" height=\"" + std::to_string(view_h) + "\" viewBox=\"0 0 " +
        std::to_string(view_w) + " " + std::to_string(view_h) + "\">" + svg +
        "</svg>"
        "<div class=\"bar\"><button id=\"prev\">◀</button><input type=\"range\" id=\"r\" min=\"0\" max=\"" +
        std::to_string(frames.empty() ? 0 : frames.size() - 1) +
        "\" value=\"0\"><button id=\"next\">▶</button><button id=\"play\">播放</button>"
        "<span class=\"num\">帧 <b id=\"fi\">0</b>/" + std::to_string(frames.size() - 1) +
        " · t=<b id=\"ft\">0</b>s · 最大骨骼偏差 <b id=\"fd\">0</b></span></div>"
        "<div class=\"legend\"><span><i class=\"sw\" style=\"background:#1c7ed6\"></i>我们</span>"
        "<span><i class=\"sw\" style=\"background:#e8590c\"></i>参考（3.8）</span>"
        "<span>← → 翻帧，空格播放/暂停</span></div>"
        "<script>"
        "var gs=[].slice.call(document.querySelectorAll('.frame'));"
        "var r=document.getElementById('r'),fi=document.getElementById('fi'),"
        "ft=document.getElementById('ft'),fd=document.getElementById('fd');"
        "var timer=null;"
        "function show(i){i=Math.max(0,Math.min(gs.length-1,i|0));"
        "gs.forEach(function(g,k){g.style.display=k===i?'':'none'});"
        "r.value=i;fi.textContent=i;"
        "ft.textContent=(+gs[i].dataset.t).toFixed(3);"
        "fd.textContent=(+gs[i].dataset.d).toFixed(3);}"
        "r.oninput=function(){show(+r.value)};"
        "document.getElementById('prev').onclick=function(){show(+r.value-1)};"
        "document.getElementById('next').onclick=function(){show(+r.value+1)};"
        "document.getElementById('play').onclick=toggle;"
        "function toggle(){if(timer){clearInterval(timer);timer=null;}"
        "else{timer=setInterval(function(){show((+r.value+1)%gs.length)},100)}}"
        "document.addEventListener('keydown',function(e){"
        "if(e.key==='ArrowLeft'){show(+r.value-1);e.preventDefault()}"
        "if(e.key==='ArrowRight'){show(+r.value+1);e.preventDefault()}"
        "if(e.key===' '){toggle();e.preventDefault()}});"
        "show(0);"
        "</script></body></html>";

    std::ofstream f(out_path, std::ios::binary);
    if (!f)
    {
        return false;
    }
    f.write(html.data(), static_cast<std::streamsize>(html.size()));
    return true;
}

struct Stats
{
    size_t files_seen = 0;
    size_t files_baked = 0;
    size_t files_bake_failed = 0;
    size_t files_ref_failed = 0;
    size_t files_compared = 0;
    size_t files_bad = 0;
    size_t files_bad_deform_on_path = 0;
    size_t anim_samples = 0;
    size_t anim_duration_checks = 0;   // 动画时长与参考逐个相等
    size_t bone_checks = 0;
    size_t mismatches = 0;
    float worst_rel = 0.0f;
    std::string first_bad_bones;
    std::vector<std::string> details;
    std::vector<std::string> bad_files;
};

}   // namespace

int main(int argc, char** argv)
{
    std::string dir;
    size_t limit = 0;   // 0 = 不限
    size_t max_details = 1;
    bool verbose = false;
    std::string flip_book;      // 非空 = 出一份逐帧叠图 HTML
    std::string flip_asset;     // 只对路径含这个子串的资产生成（空 = 第一个烘焙成功的）
    size_t flip_anim = 0;
    size_t flip_frames = 60;
    float trace_time = -1.0f;   // >= 0 = 逐步对拍模式（--trace <t>，配 --asset / --anim）
    bool uv_scan = false;       // 统计绘制项的"索引最小值"与 UV 寻址（渲染侧前置核对）
    size_t uv_items = 0, uv_items_base_nonzero = 0, uv_items_oob_abs = 0;
    size_t blend_count[4] = {0, 0, 0, 0}, page_nonzero = 0, fallback_items = 0;
    size_t vis_found = 0, vis_missing = 0, vis_examples = 0, vis_missing_with_candidates = 0;
    size_t anim_dur_pos = 0, anim_dur_zero = 0, anim_dur_examples = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc)
        {
            limit = static_cast<size_t>(std::atoi(argv[++i]));
        }
        else if (a == "--flip-book" && i + 1 < argc)
        {
            flip_book = argv[++i];
        }
        else if (a == "--asset" && i + 1 < argc)
        {
            flip_asset = argv[++i];
        }
        else if (a == "--anim" && i + 1 < argc)
        {
            flip_anim = static_cast<size_t>(std::atoi(argv[++i]));
        }
        else if (a == "--frames" && i + 1 < argc)
        {
            flip_frames = static_cast<size_t>(std::atoi(argv[++i]));
        }
        else if (a == "--details" && i + 1 < argc)
        {
            max_details = static_cast<size_t>(std::atoi(argv[++i]));
        }
        else if (a == "--tol" && i + 1 < argc)
        {
            g_tolerance = static_cast<float>(std::atof(argv[++i]));
        }
        else if (a == "--trace" && i + 1 < argc)
        {
            trace_time = static_cast<float>(std::atof(argv[++i]));
        }
        else if (a == "--uv-scan")
        {
            uv_scan = true;
        }
        else if (a == "--verbose")
        {
            verbose = true;
        }
        else if (!a.empty() && a[0] != '-')
        {
            dir = a;
        }
    }
    if (dir.empty())
    {
        std::printf("用法: SpineGpuRefParityTest <目录> [--limit N] [--details N] [--verbose]\n"
                    "      [--flip-book <out.html> [--asset <名字子串>] [--anim N] [--frames N]]\n");
        return 2;
    }

    // Unity 工程里骨架是 `<name>.skel.bytes`（二进制）或 `<name>.json`，
    // 图集是同名的 `<name>.atlas.txt`（**带 .txt** —— Unity 不认无名后缀）。
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it{fs::path(dir)}, end{}; it != end; ++it)
    {
        if (!it->is_regular_file())
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

    Stats st;
    for (const fs::path& skel_path : files)
    {
        if (limit != 0 && st.files_seen >= limit)
        {
            break;
        }
        ++st.files_seen;

        const std::string fname = skel_path.filename().string();
        const bool is_skel = fname.size() > 11 && fname.compare(fname.size() - 11, 11, ".skel.bytes") == 0;
        const std::string stem = fname.substr(0, fname.size() - (is_skel ? 11 : 5));
        // Unity 里图集后缀是 `.atlas.txt`；裸 `.atlas` 也认（我们的夹具是后者）
        fs::path atlas_path = skel_path.parent_path() / (stem + ".atlas.txt");
        if (!fs::exists(atlas_path))
        {
            const fs::path alt = skel_path.parent_path() / (stem + ".atlas");
            if (fs::exists(alt))
            {
                atlas_path = alt;
            }
        }

        std::vector<char> skel_bytes;
        std::vector<char> atlas_bytes;
        if (!ReadFile(skel_path, skel_bytes))
        {
            continue;
        }
        if (!ReadFile(atlas_path, atlas_bytes))
        {
            ++st.files_bake_failed;   // 缺图集 ⇒ 我们的导入器必然失败，跳过
            continue;
        }

        // ---- 我们的路径：解析 → 烘焙 → 写容器 → 读回来（连往返一起验）----
        SpineAtlas atlas;
        SpineImportResult r = ParseSpineAtlas(atlas_bytes.data(), atlas_bytes.size(), atlas);
        if (!r.ok)
        {
            ++st.files_bake_failed;
            continue;
        }
        SpineSourceSkeleton src;
        r = is_skel ? ParseSpineSkeletonBinary(reinterpret_cast<const uint8_t*>(skel_bytes.data()), skel_bytes.size(),
                                                &atlas, src)
                    : ParseSpineSkeletonJson(skel_bytes.data(), skel_bytes.size(), &atlas, src);
        if (!r.ok)
        {
            ++st.files_bake_failed;
            continue;
        }
        SpineGpuAsset asset;
        r = BakeSpineGpuAsset(src, asset);
        if (!r.ok)
        {
            ++st.files_bake_failed;
            continue;
        }
        std::vector<uint8_t> bytes;
        r = WriteSpineGpuContainer(asset, bytes);
        if (!r.ok)
        {
            ++st.files_bake_failed;
            continue;
        }
        SpineGpuAsset loaded;
        r = ReadSpineGpuContainer(bytes.data(), bytes.size(), loaded);
        if (!r.ok)
        {
            ++st.files_bake_failed;
            continue;
        }
        ++st.files_baked;

        if (uv_scan)
        {
            // 索引是"段内绝对下标"（= 附件本地下标 + vertex_base）。UV 只按附件本地下标排，
            // 所以 shader 若写 uv[uv_offset + index] 只在 vertex_base == 0 时对。
            const size_t uv_count = loaded.uv_buffer.size() / 2u;
            for (const SpineDrawItem& di : loaded.draw_items)
            {
                if (di.index_count == 0)
                {
                    continue;
                }
                uint32_t mn = 0xFFFFFFFFu, mx = 0;
                for (uint32_t q = 0; q < di.index_count; ++q)
                {
                    const uint32_t v = loaded.index_buffer[static_cast<size_t>(di.index_offset) + q];
                    mn = std::min(mn, v);
                    mx = std::max(mx, v);
                }
                ++uv_items;
                ++blend_count[static_cast<size_t>(di.blend_mode) & 3u];
                page_nonzero += di.atlas_page != 0 ? 1u : 0u;
                fallback_items += di.kind == SpineDrawItemKind::kFallback ? 1u : 0u;
                if (mn != 0u)
                {
                    ++uv_items_base_nonzero;
                }
                if (static_cast<size_t>(di.uv_offset) + mx >= uv_count)
                {
                    ++uv_items_oob_abs;
                }
            }
            // 渲染侧"本帧画哪些项"的解析（与 SpineGpuRenderer::Draw 同一规则）：
            // 绘制顺序里每个挂着附件的槽位，按"槽位 + 附件名 + skin 0"找绘制项。
            for (size_t an_i = 0; an_i < loaded.anims.size(); ++an_i)
            {
                const SpineGpuAnim& an = loaded.anims[an_i];
                if (verbose)
                {
                    std::printf("[uv-scan]   动画 #%zu '%s'：时长 %.4f，时间轴 %u 条\n", an_i,
                                an_i < loaded.anim_names.size() ? loaded.anim_names[an_i].c_str() : "?", an.duration,
                                an.timeline_count);
                }
                ++(an.duration > 0.0f ? anim_dur_pos : anim_dur_zero);
                if (an.duration <= 0.0f && an.timeline_count > 0 && anim_dur_examples < 5)
                {
                    ++anim_dur_examples;
                    std::printf("[uv-scan] duration = 0 但有 %u 条时间轴：%s\n", an.timeline_count,
                                skel_path.string().c_str());
                }
            }
            if (!loaded.anims.empty())
            {
                SpineGpuSlotSampleResult slots;
                if (SpineGpuSampleSlots(loaded, 0, 0.0f, slots).ok)
                {
                    const std::vector<uint8_t> blob = BuildSpineStringBlob(loaded);
                    for (uint32_t s : slots.draw_order)
                    {
                        const uint32_t off = slots.slots[s].attachment_name_offset;
                        if (off == SpineGpuConstants::kNoAttachmentName)
                        {
                            continue;
                        }
                        const char* name = SpineGpuStringAt(blob, off);
                        bool found = false;
                        for (const SpineDrawItem& di : loaded.draw_items)
                        {
                            if (di.slot_index == s && di.skin_id == SpineGpuConstants::kDefaultSkinId &&
                                di.attachment_id < loaded.attachment_names.size() && name != nullptr &&
                                loaded.attachment_names[di.attachment_id] == name)
                            {
                                found = true;
                                break;
                            }
                        }
                        ++(found ? vis_found : vis_missing);
                        if (!found)
                        {
                            // 槽位里**有**可画项、却没有这个名字的 ⇒ 真丢（该报）；
                            // 槽位里一个可画项都没有 ⇒ 挂的是裁剪 / point / bbox / path 这类不画的附件
                            for (const SpineDrawItem& di : loaded.draw_items)
                            {
                                if (di.slot_index == s && di.skin_id == SpineGpuConstants::kDefaultSkinId)
                                {
                                    ++vis_missing_with_candidates;
                                    break;
                                }
                            }
                        }
                        if (!found && vis_examples < 8)
                        {
                            ++vis_examples;
                            std::string cands;
                            for (const SpineDrawItem& di : loaded.draw_items)
                            {
                                if (di.slot_index == s)
                                {
                                    cands += " [skin " + std::to_string(di.skin_id) + " '" +
                                             (di.attachment_id < loaded.attachment_names.size()
                                                  ? loaded.attachment_names[di.attachment_id]
                                                  : std::string("<id越界>")) +
                                             "']";
                                }
                            }
                            std::printf("[uv-scan] 找不到绘制项：%s 槽位 %u 要 '%s'（名字表 %zu 条），候选：%s\n",
                                        skel_path.string().c_str(), s, name != nullptr ? name : "<偏移越界>",
                                        loaded.attachment_names.size(), cands.c_str());
                        }
                    }
                }
            }
            continue;
        }

        // ---- 参考侧 ----
        RefAttachmentLoader ref_loader;
        spine::SkeletonBinary ref_binary(&ref_loader);
        spine::SkeletonJson ref_json(&ref_loader);
        spine::SkeletonData* ref_data =
            is_skel ? ref_binary.readSkeletonData(reinterpret_cast<const unsigned char*>(skel_bytes.data()),
                                                  static_cast<int>(skel_bytes.size()))
                    : ref_json.readSkeletonData(skel_bytes.data());
        if (ref_data == nullptr)
        {
            ++st.files_ref_failed;
            if (verbose)
            {
                std::printf("[ref] 解析失败: %s\n", skel_path.string().c_str());
            }
            continue;
        }
        spine::Skeleton ref_skel(ref_data);

        // 约束构成（诊断用：坏了才知道该怀疑哪套求解器）
        const size_t n_ik = ref_data->getIkConstraints().size();
        const size_t n_tr = ref_data->getTransformConstraints().size();
        const size_t n_pa = ref_data->getPathConstraints().size();

        if (ref_data->getAnimations().size() != loaded.anims.size())
        {
            std::printf("[对不上] %s: 动画数 参考 %zu vs 我们 %zu\n", skel_path.string().c_str(),
                        ref_data->getAnimations().size(), loaded.anims.size());
            ++st.mismatches;
            continue;
        }


        // ---- 逐步对拍（--trace <t>）----
        // 只在命中的第一个文件上跑一次：先比时间轴写完后的**局部**值，再沿参考实现的
        // `getUpdateCacheList()` 逐步 update、每步后比全体骨骼世界变换，报出**第一个
        // 分叉的步骤** —— 区分"时间轴采样错"与"某个约束解算错"，以及是哪一个约束。
        if (trace_time >= 0.0f && loaded.anims.size() > flip_anim &&
            (flip_asset.empty() || skel_path.string().find(flip_asset) != std::string::npos))
        {
            SpineGpuPoseResult pose;
            SpineGpuPoseTrace tr;
            const SpineImportResult pr =
                SpineGpuSamplePose(loaded, static_cast<uint32_t>(flip_anim), trace_time, pose, &tr);
            if (!pr.ok)
            {
                std::printf("[trace] 我们的解算失败: %s\n", pr.error.c_str());
                return 1;
            }
            spine::Animation* ta = ref_data->getAnimations()[flip_anim];
            ref_skel.setToSetupPose();
            ta->apply(ref_skel, 0.0f, trace_time, false, nullptr, 1.0f, spine::MixBlend_First,
                      spine::MixDirection_In);
            spine::Vector<spine::Bone*>& rb = ref_skel.getBones();
            std::printf("[trace] %s 动画 #%zu '%s' t=%g，骨骼 %zu，参考 updateCache %zu 步 / 我们 %zu 步\n",
                        skel_path.string().c_str(), flip_anim, ta->getName().buffer(),
                        static_cast<double>(trace_time), rb.size(), ref_skel.getUpdateCacheList().size(),
                        tr.after_step.size());
            // 1) 局部值
            size_t local_bad = 0;
            for (size_t bi = 0; bi < rb.size() && bi < tr.local.size(); ++bi)
            {
                const SpineGpuPoseTrace::Local& l = tr.local[bi];
                const float ov[7] = {l.x, l.y, l.rotation, l.scale_x, l.scale_y, l.shear_x, l.shear_y};
                const float rv[7] = {rb[bi]->getX(),      rb[bi]->getY(),      rb[bi]->getRotation(),
                                     rb[bi]->getScaleX(), rb[bi]->getScaleY(), rb[bi]->getShearX(),
                                     rb[bi]->getShearY()};
                float w = 0.0f;
                bool ok = true;
                for (int k = 0; k < 7; ++k)
                {
                    ok = CloseEnough(ov[k], rv[k], w) && ok;
                }
                if (!ok && local_bad++ < 8)
                {
                    std::printf("  局部值不同 骨骼 %zu '%s': 我们 (x=%g y=%g r=%g sx=%g sy=%g hx=%g hy=%g) "
                                "参考 (x=%g y=%g r=%g sx=%g sy=%g hx=%g hy=%g)\n",
                                bi, rb[bi]->getData().getName().buffer(), ov[0], ov[1], ov[2], ov[3], ov[4], ov[5],
                                ov[6], rv[0], rv[1], rv[2], rv[3], rv[4], rv[5], rv[6]);
                }
            }
            std::printf("  局部值不同的骨骼: %zu\n", local_bad);
            // 2) 复位：参考的 `_updateCacheReset` 是私有的；对**全部**骨骼复位与之等价
            //    （不在复位表里的骨骼，其 applied 值只在它自己 update() 之后才会被读）。
            for (size_t bi = 0; bi < rb.size(); ++bi)
            {
                spine::Bone& b = *rb[bi];
                b.setAX(b.getX());
                b.setAY(b.getY());
                b.setAppliedRotation(b.getRotation());
                b.setAScaleX(b.getScaleX());
                b.setAScaleY(b.getScaleY());
                b.setAShearX(b.getShearX());
                b.setAShearY(b.getShearY());
                b.setAppliedValid(true);
            }
            spine::Vector<spine::Updatable*>& cache = ref_skel.getUpdateCacheList();
            const size_t steps = std::min(cache.size(), tr.after_step.size());
            std::vector<SpineGpuPoseTrace::Local> ref_prev_applied(rb.size());
            std::vector<uint8_t> ref_prev_valid(rb.size());
            for (size_t si = 0; si < steps; ++si)
            {
                for (size_t bi = 0; bi < rb.size(); ++bi)
                {
                    spine::Bone& b = *rb[bi];
                    ref_prev_applied[bi] = {b.getAX(), b.getAY(), b.getAppliedRotation(), b.getAScaleX(),
                                            b.getAScaleY(), b.getAShearX(), b.getAShearY()};
                    ref_prev_valid[bi] = b.isAppliedValid() ? 1u : 0u;
                }
                cache[si]->update();
                const char* kind_name = "bone";
                std::string ref_what;
                if (cache[si]->getRTTI().instanceOf(spine::Bone::rtti))
                {
                    ref_what = std::string("bone '") + static_cast<spine::Bone*>(cache[si])->getData().getName().buffer() + "'";
                }
                else if (cache[si]->getRTTI().instanceOf(spine::IkConstraint::rtti))
                {
                    ref_what = std::string("ik '") +
                               static_cast<spine::IkConstraint*>(cache[si])->getData().getName().buffer() + "'";
                }
                else if (cache[si]->getRTTI().instanceOf(spine::TransformConstraint::rtti))
                {
                    ref_what = std::string("transform '") +
                               static_cast<spine::TransformConstraint*>(cache[si])->getData().getName().buffer() + "'";
                }
                else if (cache[si]->getRTTI().instanceOf(spine::PathConstraint::rtti))
                {
                    ref_what = std::string("path '") +
                               static_cast<spine::PathConstraint*>(cache[si])->getData().getName().buffer() + "'";
                }
                switch (tr.step_kind[si])
                {
                    case SpineGpuUpdateKind::kUpdateIk: kind_name = "ik"; break;
                    case SpineGpuUpdateKind::kUpdateTransform: kind_name = "transform"; break;
                    case SpineGpuUpdateKind::kUpdatePath: kind_name = "path"; break;
                    default: break;
                }
                size_t bad = 0;
                std::string bad_list;
                std::vector<size_t> bad_set;
                for (size_t bi = 0; bi < rb.size() && bi < tr.after_step[si].size(); ++bi)
                {
                    const SpineGpuBoneSample& o = tr.after_step[si][bi];
                    const float ov[6] = {o.a, o.b, o.c, o.d, o.world_x, o.world_y};
                    const float rv[6] = {rb[bi]->getA(), rb[bi]->getB(), rb[bi]->getC(),
                                         rb[bi]->getD(), rb[bi]->getWorldX(), rb[bi]->getWorldY()};
                    float w = 0.0f;
                    bool ok = true;
                    for (int k = 0; k < 6; ++k)
                    {
                        ok = CloseEnough(ov[k], rv[k], w) && ok;
                    }
                    if (!ok)
                    {
                        bad_set.push_back(bi);
                        if (bad < 6)
                        {
                            char b2[512];
                            std::snprintf(b2, sizeof(b2),
                                          "\n      骨骼 %zu '%s' 我们 (a=%g b=%g c=%g d=%g wx=%g wy=%g) 参考 "
                                          "(a=%g b=%g c=%g d=%g wx=%g wy=%g)",
                                          bi, rb[bi]->getData().getName().buffer(), ov[0], ov[1], ov[2], ov[3],
                                          ov[4], ov[5], rv[0], rv[1], rv[2], rv[3], rv[4], rv[5]);
                            bad_list += b2;
                        }
                        ++bad;
                    }
                }
                if (bad != 0)
                {
                    std::printf("  第一个分叉：第 %zu 步  我们 %s #%u / 参考 %s  ⇒ %zu 根骨骼不同%s\n", si, kind_name,
                                tr.step_index[si], ref_what.c_str(), bad, bad_list.c_str());
                    // 这一步**之前**的 applied 值（约束的真正输入），全体骨骼里不一致的都列出来
                    if (si > 0)
                    {
                        const auto& oap = tr.applied_after_step[si - 1];
                        const auto& oav = tr.applied_valid_after_step[si - 1];
                        size_t shown = 0;
                        std::vector<size_t> focus;
                        for (size_t bb : bad_set)
                        {
                            focus.push_back(bb);
                            if (rb[bb]->getParent() != nullptr)
                            {
                                for (size_t q = 0; q < rb.size(); ++q)
                                {
                                    if (rb[q] == rb[bb]->getParent())
                                    {
                                        focus.push_back(q);
                                    }
                                }
                            }
                        }
                        for (size_t bi : focus)
                        {
                            if (bi >= oap.size())
                            {
                                continue;
                            }
                            const SpineGpuPoseTrace::Local& o = oap[bi];
                            const SpineGpuPoseTrace::Local& r = ref_prev_applied[bi];
                            const float ov[7] = {o.x, o.y, o.rotation, o.scale_x, o.scale_y, o.shear_x, o.shear_y};
                            const float rv[7] = {r.x, r.y, r.rotation, r.scale_x, r.scale_y, r.shear_x, r.shear_y};
                            float w = 0.0f;
                            bool ok = oav[bi] == ref_prev_valid[bi];
                            for (int k = 0; k < 7; ++k)
                            {
                                ok = CloseEnough(ov[k], rv[k], w) && ok;
                            }
                            if (shown++ < 8)
                            {
                                std::printf("      步前 applied%s 骨骼 %zu '%s': 我们 valid=%d (ax=%g ay=%g ar=%g asx=%g asy=%g ahx=%g ahy=%g)"
                                            " 参考 valid=%d (ax=%g ay=%g ar=%g asx=%g asy=%g ahx=%g ahy=%g)\n",
                                            ok ? "（一致）" : "【不同】", bi, rb[bi]->getData().getName().buffer(), oav[bi], ov[0], ov[1], ov[2], ov[3],
                                            ov[4], ov[5], ov[6], ref_prev_valid[bi], rv[0], rv[1], rv[2], rv[3], rv[4],
                                            rv[5], rv[6]);
                            }
                        }

                    }
                    return 1;
                }
            }
            std::printf("  逐步全部一致（%zu 步）\n", steps);
            return 0;
        }

        // ---- 逐帧叠图（--flip-book）----
        // 只在命中的第一个文件上生成一次，然后直接收工（这是目视工具，不是回归）。
        if (!flip_book.empty() && loaded.anims.size() > flip_anim &&
            (flip_asset.empty() || skel_path.string().find(flip_asset) != std::string::npos))
        {
            const float dur = loaded.anims[flip_anim].duration;
            const size_t nf = flip_frames < 2 ? 2 : flip_frames;
            std::vector<FlipFrame> frames;
            std::vector<int16_t> parents;
            for (const SpineGpuBone& b : loaded.bones)
            {
                parents.push_back(b.parent);
            }
            float worst = 0.0f;
            for (size_t k = 0; k < nf; ++k)
            {
                const float t = dur * static_cast<float>(k) / static_cast<float>(nf - 1);
                FlipFrame fr;
                fr.time = t;
                SpineGpuPoseResult pose;
                const SpineImportResult pr = SpineGpuSamplePose(loaded, static_cast<uint32_t>(flip_anim), t, pose);
                if (!pr.ok)
                {
                    std::printf("[flip-book] 我们的解算失败 t=%g: %s\n", static_cast<double>(t), pr.error.c_str());
                    return 1;
                }
                fr.ours.reserve(pose.bones.size() * 2);
                for (const SpineGpuBoneSample& s : pose.bones)
                {
                    fr.ours.push_back(s.world_x);
                    fr.ours.push_back(s.world_y);
                }
                spine::Animation* fa = ref_data->getAnimations()[flip_anim];
                ref_skel.setToSetupPose();
                fa->apply(ref_skel, 0.0f, t, false, nullptr, 1.0f, spine::MixBlend_First, spine::MixDirection_In);
                ref_skel.updateWorldTransform();
                spine::Vector<spine::Bone*>& rb = ref_skel.getBones();
                fr.ref.reserve(rb.size() * 2);
                for (size_t bi = 0; bi < rb.size(); ++bi)
                {
                    fr.ref.push_back(rb[bi]->getWorldX());
                    fr.ref.push_back(rb[bi]->getWorldY());
                }
                for (size_t bi = 0; bi < pose.bones.size() && bi < rb.size(); ++bi)
                {
                    const float dx = fr.ours[bi * 2] - fr.ref[bi * 2];
                    const float dy = fr.ours[bi * 2 + 1] - fr.ref[bi * 2 + 1];
                    fr.max_dist = std::fmax(fr.max_dist, std::sqrt(dx * dx + dy * dy));
                }
                worst = std::fmax(worst, fr.max_dist);
                frames.push_back(std::move(fr));
            }
            if (!WriteFlipBook(fs::path(flip_book), skel_path.filename().string(), flip_anim, loaded.bone_names,
                               parents, frames))
            {
                std::printf("[flip-book] 写文件失败: %s\n", flip_book.c_str());
                return 1;
            }
            std::printf("[flip-book] %s 动画 #%zu，%zu 帧，骨骼 %zu 根\n", flip_book.c_str(), flip_anim,
                        frames.size(), loaded.bones.size());
            std::printf("[flip-book] 逐帧最大骨骼偏差（世界单位）: %.4f\n", static_cast<double>(worst));
            return 0;
        }

        bool file_ok = true;
        bool deform_on_path = false;   // 首个坏样本里，path 约束的目标槽位是否带 deform
        bool deform_scanned = false;
        for (size_t ai = 0; ai < loaded.anims.size() && file_ok; ++ai)
        {
            const float duration = loaded.anims[ai].duration;
            // 时长也要对拍：下面的采样点是按**我们的**时长取的 —— 我们若算成 0 而参考不是，
            // 这个动画就只在 t=0 被比过，对拍照样"全过"（渲染侧一跑就是静止画面）。
            {
                const float ref_duration = ref_data->getAnimations()[ai]->getDuration();
                ++st.anim_duration_checks;
                if (duration != ref_duration)
                {
                    std::printf("[对不上] %s 动画 %zu 时长：我们 %g vs 参考 %g\n", skel_path.string().c_str(), ai,
                                static_cast<double>(duration), static_cast<double>(ref_duration));
                    ++st.mismatches;
                    file_ok = false;
                    break;
                }
            }
            // 5 个采样点：0 / 1/4 / 1/2 / 3/4 / 末帧
            const float times[5] = {0.0f, duration * 0.25f, duration * 0.5f, duration * 0.75f, duration};
            for (float t : times)
            {
                ++st.anim_samples;

                SpineGpuPoseResult ours;
                const SpineImportResult sr = SpineGpuSamplePose(loaded, static_cast<uint32_t>(ai), t, ours);
                if (!sr.ok)
                {
                    std::printf("[对不上] %s 动画 %zu t=%g: 我们的解算失败: %s\n", skel_path.string().c_str(), ai, t,
                                sr.error.c_str());
                    ++st.mismatches;
                    file_ok = false;
                    break;
                }

                spine::Animation* ref_anim = ref_data->getAnimations()[ai];
                ref_skel.setToSetupPose();
                ref_anim->apply(ref_skel, 0.0f, t, false, nullptr, 1.0f, spine::MixBlend_First,
                                spine::MixDirection_In);
                ref_skel.updateWorldTransform();

                spine::Vector<spine::Bone*>& ref_bones = ref_skel.getBones();
                if (ref_bones.size() != ours.bones.size())
                {
                    std::printf("[对不上] %s: 骨骼数 参考 %zu vs 我们 %zu\n", skel_path.string().c_str(),
                                ref_bones.size(), ours.bones.size());
                    ++st.mismatches;
                    file_ok = false;
                    break;
                }

                std::vector<size_t> bad_bones;
                for (size_t bi = 0; bi < ours.bones.size(); ++bi)
                {
                    const SpineGpuBoneSample& o = ours.bones[bi];
                    const float rv[6] = {ref_bones[bi]->getA(),      ref_bones[bi]->getB(),
                                         ref_bones[bi]->getC(),      ref_bones[bi]->getD(),
                                         ref_bones[bi]->getWorldX(), ref_bones[bi]->getWorldY()};
                    const float ov[6] = {o.a, o.b, o.c, o.d, o.world_x, o.world_y};
                    bool ok = true;
                    float worst = 0.0f;
                    for (int k = 0; k < 6; ++k)
                    {
                        ++st.bone_checks;
                        if (!CloseEnough(ov[k], rv[k], worst))
                        {
                            ok = false;
                        }
                    }
                    if (worst > st.worst_rel)
                    {
                        st.worst_rel = worst;
                    }
                    if (!ok)
                    {
                        ++st.mismatches;
                        bad_bones.push_back(bi);
                        if (st.details.size() < max_details)
                        {
                            char buf[640];
                            std::snprintf(buf, sizeof(buf),
                                          "%s 动画 %zu t=%g 骨骼 %zu '%s': 我们 "
                                          "(a=%g b=%g c=%g d=%g wx=%g wy=%g) vs 参考 "
                                          "(a=%g b=%g c=%g d=%g wx=%g wy=%g)",
                                          skel_path.filename().string().c_str(), ai, t, bi,
                                          ref_bones[bi]->getData().getName().buffer(), ov[0], ov[1], ov[2], ov[3],
                                          ov[4], ov[5], rv[0], rv[1], rv[2], rv[3], rv[4], rv[5]);
                            st.details.push_back(buf);
                        }
                    }
                }
                if (!bad_bones.empty())
                {
                    file_ok = false;
                    // ⚠️ deform 判据**每个文件**都要扫一次，不能挂在"全局第一个坏样本"上：
                    //    挂在全局上时只有第一个坏文件会被判，后面的文件一律记成 deform=0
                    //    （曾经就这样把 75 个坏文件里真实占比量成了 1）。
                    if (!deform_scanned && n_pa != 0)
                    {
                        deform_scanned = true;
                        spine::Vector<spine::PathConstraint*>& pcs_all = ref_skel.getPathConstraints();
                        for (size_t pci = 0; pci < pcs_all.size(); ++pci)
                        {
                            spine::Slot* ts = pcs_all[pci]->getTarget();
                            if (ts != nullptr && ts->getAttachmentVertices().size() != 0)
                            {
                                deform_on_path = true;
                                break;
                            }
                        }
                    }
                    // 第一个坏样本就把"哪些骨骼坏"全打出来：
                    // 是**一棵子树**（⇒ 约束 / 父链）还是**散点**（⇒ 时间轴）是两种病。
                    if (st.first_bad_bones.empty())
                    {
                        std::string s;
                        for (size_t bi : bad_bones)
                        {
                            if (!s.empty())
                            {
                                s += ",";
                            }
                            s += std::to_string(bi) + "(" + ref_bones[bi]->getData().getName().buffer() + ")";
                        }
                        std::string extra;
                        if (n_pa != 0)
                        {
                            spine::Vector<spine::PathConstraint*>& pcs = ref_skel.getPathConstraints();
                            for (size_t pci = 0; pci < pcs.size() && pci < 12; ++pci)
                            {
                                spine::Slot* ts = pcs[pci]->getTarget();
                                spine::Attachment* ta = ts != nullptr ? ts->getAttachment() : nullptr;
                                // 判据：`computeWorldVertices` 在 **rigid** 分支里会用槽位的
                                // deform 数组**整体替换**顶点（VertexAttachment.cpp:68-70）
                                // ⇒ deform 改了 path 的形状 ⇒ 被约束骨骼的位置也跟着变。
                                // 我们目前**跳过** deform 时间轴（它只影响顶点、GPU 上做），
                                // 对 path 约束来说这个跳过是错的。
                                if (ts != nullptr && ts->getAttachmentVertices().size() != 0)
                                {
                                    deform_on_path = true;
                                }
                                char pb[256];
                                std::snprintf(pb, sizeof(pb),
                                              "\n    path[%zu] slot=%s att=%s refpos=%g refspace=%g refrot=%g reftr=%g "
                                              "refslotbone=(%g,%g a=%g b=%g c=%g d=%g)",
                                              pci, ts != nullptr ? ts->getData().getName().buffer() : "?",
                                              ta != nullptr ? ta->getName().buffer() : "<none>",
                                              static_cast<double>(pcs[pci]->getPosition()),
                                              static_cast<double>(pcs[pci]->getSpacing()),
                                              static_cast<double>(pcs[pci]->getRotateMix()),
                                              static_cast<double>(pcs[pci]->getTranslateMix()),
                                              static_cast<double>(ts != nullptr ? ts->getBone().getWorldX() : 0.0f),
                                              static_cast<double>(ts != nullptr ? ts->getBone().getWorldY() : 0.0f),
                                              static_cast<double>(ts != nullptr ? ts->getBone().getA() : 0.0f),
                                              static_cast<double>(ts != nullptr ? ts->getBone().getB() : 0.0f),
                                              static_cast<double>(ts != nullptr ? ts->getBone().getC() : 0.0f),
                                              static_cast<double>(ts != nullptr ? ts->getBone().getD() : 0.0f));
                                extra += pb;
                                if (ta != nullptr && ta->getRTTI().instanceOf(spine::PathAttachment::rtti))
                                {
                                    spine::PathAttachment* pa = static_cast<spine::PathAttachment*>(ta);
                                    spine::Vector<float>& rv = pa->getVertices();
                                    std::string lv = "\n         ref-localverts:";
                                    for (size_t q = 0; q < rv.size() && q < 8; ++q)
                                    {
                                        lv += " " + std::to_string(rv[q]);
                                    }
                                    lv += " closed=" + std::to_string(pa->isClosed()) + " cs=" +
                                          std::to_string(pa->isConstantSpeed()) + " vcount=" +
                                          std::to_string(static_cast<int>(rv.size()));
                                    // 参考侧的世界顶点（`world` 数组的输入）
                                    spine::Vector<float> wv;
                                    wv.setSize(pa->getWorldVerticesLength(), 0);
                                    pa->computeWorldVertices(*ts, wv);
                                    lv += "\n         ref-worldverts:";
                                    for (size_t q = 0; q < wv.size() && q < 8; ++q)
                                    {
                                        lv += " " + std::to_string(wv[q]);
                                    }
                                    lv += " wvlen=" + std::to_string(static_cast<int>(wv.size()));
                                    // 关键判据：rigid 分支里 `computeWorldVertices` 会**优先用槽位的
                                    // deform 数组**（VertexAttachment.cpp:69-70）⇒ deform 会改 path
                                    // 的形状，进而改变被约束骨骼的位置。
                                    spine::Vector<float>& dv = ts->getAttachmentVertices();
                                    lv += "\n         ref-deform(" + std::to_string(static_cast<int>(dv.size())) + "):";
                                    for (size_t q = 0; q < dv.size() && q < 8; ++q)
                                    {
                                        lv += " " + std::to_string(dv[q]);
                                    }
                                    extra += lv;
                                }
                            }
                        }
                        char buf[2048];
                        std::snprintf(buf, sizeof(buf),
                                      "%s 动画 %zu t=%g 共 %zu 根骨骼不一致: %s\n"
                                      "    （该资产约束构成: ik=%zu transform=%zu path=%zu；骨骼 %zu 根）%s",
                                      skel_path.filename().string().c_str(), ai, t, bad_bones.size(), s.c_str(), n_ik,
                                      n_tr, n_pa, ours.bones.size(), extra.c_str());
                        st.first_bad_bones = buf;
                    }
                    break;
                }
            }
        }

        if (file_ok)
        {
            ++st.files_compared;
        }
        else
        {
            ++st.files_bad;
            if (deform_on_path)
            {
                ++st.files_bad_deform_on_path;
            }
            if (st.bad_files.size() < 200)
            {
                // 带上父目录名：同一批资产有 hou/qian/zhong/Low 四个变体，
                // 文件名完全相同，只看文件名分不清是哪一个。
                const std::string short_path =
                    (skel_path.parent_path().filename().string() + "/" + skel_path.filename().string());
                char b2[256];
                std::snprintf(b2, sizeof(b2), "%-56s ik=%-3zu tr=%-3zu pa=%-3zu bones=%-4zu deform=%d",
                              short_path.c_str(), n_ik, n_tr, n_pa, ref_data->getBones().size(),
                              deform_on_path ? 1 : 0);
                st.bad_files.push_back(b2);
            }
        }
        if (verbose)
        {
            std::printf("[%s] %s\n", file_ok ? "OK " : "BAD", skel_path.string().c_str());
        }
    }

    if (uv_scan)
    {
        std::printf("[uv-scan] 绘制项 %zu，索引最小值 != 0 的 %zu，uv_offset + 段内索引 越过 UV 表的 %zu\n", uv_items,
                    uv_items_base_nonzero, uv_items_oob_abs);
        std::printf("[uv-scan] 混合模式 normal %zu / additive %zu / multiply %zu / screen %zu；atlas_page != 0 的 %zu；"
                    "CPU fallback 项 %zu\n",
                    blend_count[0], blend_count[1], blend_count[2], blend_count[3], page_nonzero, fallback_items);
        std::printf("[uv-scan] 第 0 个动画 t=0 时挂着附件的槽位：找到绘制项 %zu，找不到 %zu（其中槽位里有别的可画项的 %zu）\n",
                    vis_found, vis_missing, vis_missing_with_candidates);
        std::printf("[uv-scan] 动画时长 > 0 的 %zu 个，= 0 的 %zu 个\n", anim_dur_pos, anim_dur_zero);
        return 0;
    }
    std::printf("\n=== 参考实现对拍（spine-cpp 3.8）===\n");
    std::printf("扫描 .skel.bytes      : %zu\n", st.files_seen);
    std::printf("成功烘焙（含往返）    : %zu\n", st.files_baked);
    std::printf("烘焙失败（跳过）      : %zu\n", st.files_bake_failed);
    std::printf("参考解析失败（跳过）  : %zu\n", st.files_ref_failed);
    std::printf("逐骨骼比过的文件      : %zu\n", st.files_compared);
    std::printf("  └ 比对失败的文件    : %zu\n", st.files_bad);
    std::printf("  └ 其中 path 带 deform: %zu\n", st.files_bad_deform_on_path);
    std::printf("动画时长对拍         : %zu\n", st.anim_duration_checks);
    std::printf("动画 × 时间点采样     : %zu\n", st.anim_samples);
    std::printf("骨骼分量比较次数      : %zu\n", st.bone_checks);
    std::printf("最大相对误差          : %.3g\n", static_cast<double>(st.worst_rel));
    std::printf("不一致                : %zu\n", st.mismatches);
    if (!st.first_bad_bones.empty())
    {
        std::printf("\n第一个坏样本：\n  %s\n", st.first_bad_bones.c_str());
    }
    for (const std::string& d : st.details)
    {
        std::printf("  - %s\n", d.c_str());
    }
    if (!st.bad_files.empty())
    {
        std::printf("\n比对失败的文件（前 %zu 个）:\n", st.bad_files.size());
        for (const std::string& b : st.bad_files)
        {
            std::printf("  %s\n", b.c_str());
        }
    }
    std::printf("\n=== verdict: %s ===\n", st.mismatches == 0 ? "PASS" : "FAIL");

    return st.mismatches == 0 ? 0 : 1;
}
