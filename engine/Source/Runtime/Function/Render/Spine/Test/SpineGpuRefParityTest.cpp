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
#include <spine/SkeletonClipping.h>
#include <spine/IkConstraint.h>
#include <spine/TransformConstraint.h>
#include <spine/PathConstraint.h>
#include <spine/Updatable.h>
#include <spine/IkConstraintData.h>
#include <spine/TransformConstraintData.h>
#include <spine/PathConstraintData.h>
#include <spine/Animation.h>
#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/DeformTimeline.h>
#include <spine/Timeline.h>
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
#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"
#include "Runtime/Function/Render/Spine/SpineGpuClipper.h"
#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
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
//
// v15：给了图集就按 `AtlasAttachmentLoader`（AtlasAttachmentLoader.cpp:51-96）配 region / mesh 的
// 图集参数 —— 端到端比 UV / 裁剪要用参考侧的 UV。找不到 region 时**照样建附件**（参考加载器
// 会返回 NULL ⇒ 附件集合变了，骨骼对拍就没法比），只是不配 UV。
class RefAttachmentLoader : public spine::AttachmentLoader
{
public:
    explicit RefAttachmentLoader(const SpineAtlas* atlas = nullptr) : m_Atlas(atlas) {}

    spine::RegionAttachment* newRegionAttachment(spine::Skin& skin, const spine::String& name,
                                                 const spine::String& path) override
    {
        (void)skin;
        spine::RegionAttachment* a = new (__FILE__, __LINE__) spine::RegionAttachment(name);
        a->setPath(path);
        if (const SpineAtlasRegion* r = Find(path))
        {
            a->setUVs(r->u, r->v, r->u2, r->v2, r->rotate);
            a->setRegionOffsetX(static_cast<float>(r->offset_x));
            a->setRegionOffsetY(static_cast<float>(r->offset_y));
            a->setRegionWidth(static_cast<float>(r->width));
            a->setRegionHeight(static_cast<float>(r->height));
            a->setRegionOriginalWidth(static_cast<float>(r->orig_w));
            a->setRegionOriginalHeight(static_cast<float>(r->orig_h));
        }
        return a;
    }
    spine::MeshAttachment* newMeshAttachment(spine::Skin& skin, const spine::String& name,
                                             const spine::String& path) override
    {
        (void)skin;
        spine::MeshAttachment* a = new (__FILE__, __LINE__) spine::MeshAttachment(name);
        a->setPath(path);
        if (const SpineAtlasRegion* r = Find(path))
        {
            a->setRegionU(r->u);
            a->setRegionV(r->v);
            a->setRegionU2(r->u2);
            a->setRegionV2(r->v2);
            a->setRegionRotate(r->rotate);
            a->setRegionOffsetX(static_cast<float>(r->offset_x));
            a->setRegionOffsetY(static_cast<float>(r->offset_y));
            a->setRegionWidth(static_cast<float>(r->width));
            a->setRegionHeight(static_cast<float>(r->height));
            a->setRegionOriginalWidth(static_cast<float>(r->orig_w));
            a->setRegionOriginalHeight(static_cast<float>(r->orig_h));
        }
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

private:
    const SpineAtlasRegion* Find(const spine::String& path) const
    {
        // spine::String 空串时 buffer() 是 NULL
        return m_Atlas != nullptr && path.buffer() != nullptr ? FindAtlasRegion(*m_Atlas, path.buffer()) : nullptr;
    }
    const SpineAtlas* m_Atlas;
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

// S6 端到端蒙皮：rigid 的世界坐标绝对误差门槛（骨架单位）。
constexpr float kRigidSkinTol = 1.0e-3f;

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
    size_t slot_attachment_checks = 0; // S6：槽位当前附件（→ 绘制项）与参考 Slot::getAttachment() 一致
    size_t deform_checks = 0;          // S6：槽位挂 mesh 时，deform 数组与参考 getAttachmentVertices() 比过
    size_t region_uv_checks = 0;       // v15：region 每个角的 (位置, UV) 配对与参考一致
    size_t region_uv_bad = 0;
    size_t mesh_uv_checks = 0;
    size_t mesh_uv_bad = 0;
    size_t deform_nonempty = 0;        //     其中参考数组非空的
    size_t deform_values = 0;          //     逐 float 比较次数
    float worst_deform_rel = 0.0f;
    size_t gpu_addr_checks = 0;        // S6：weighted 影响流在附件内连续（GPU deform 寻址的前提）
    // S6 端到端：按 spine_gpu.vert **同一套寻址 / 公式**在 CPU 上蒙皮，与参考
    // `VertexAttachment::computeWorldVertices()` 比世界坐标。按 (rigid/weighted) × (有/无 deform) 分四格：
    // weighted 的权重是 u8 量化的（容器格式的既定取舍），所以它的误差底噪由"无 deform"那格给出，
    // "有 deform"那格只要与底噪同量级就说明 deform 寻址 / 加法没错（错了是成百上千单位）。
    size_t skin_verts[2][2] = {{0, 0}, {0, 0}};
    float skin_worst[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
    size_t bone_checks = 0;
    size_t mismatches = 0;
    float worst_rel = 0.0f;
    std::string first_bad_bones;
    std::vector<std::string> details;
    std::vector<std::string> bad_files;
};

}   // namespace

// =====================================================================
// S5：多轨混合对拍（SpineGpuAnimationState ↔ spine::AnimationState）
// ---------------------------------------------------------------------
// 每个资产跑若干段**随机脚本**：两边各持有一份**跨帧存活**的骨架（我们的 SpineGpuSkeletonState
// / 参考的 spine::Skeleton）与一份 AnimationState，喂同一串命令（setAnimation / addAnimation /
// setEmptyAnimation(s) / addEmptyAnimation / clearTrack / 改 alpha、timeScale、holdPrevious、
// 阈值、mixBlend、mixDuration）与同一串 dt，每帧 update → apply → updateWorldTransform 后逐项比：
//   * 每根骨骼 (a, b, c, d, worldX, worldY)
//   * 每个槽位：当前附件名、颜色 rgba、暗色 rgba、deform 数组（逐 float，含 path 附件的）
//   * 绘制顺序
//   * 每条 path 约束的 position / spacing / rotateMix / translateMix
// 第一处分叉就停下这段脚本，打印帧号与"到这一帧为止的命令"，可以直接照着复现。
//
// 参考侧的 propertyId 撞号（见 SpineGpuSkeletonState.h `property_key` 的注释）单独统计：
// 撞号是参考实现随加载历史变的偶发行为，不是语义 ⇒ 落在撞号资产上的分叉另记一栏，不混进"不一致"。
// =====================================================================

namespace
{

struct S5Stats
{
    size_t assets = 0;            // 参与的资产
    size_t assets_bad = 0;        // 出现分叉的资产（不含撞号资产）
    size_t assets_collision = 0;  // 参考侧 propertyId 撞号的资产
    size_t assets_collision_bad = 0;
    size_t scripts = 0;
    size_t frames = 0;
    size_t commands = 0;
    size_t bone_checks = 0;
    size_t slot_checks = 0;
    size_t deform_values = 0;
    size_t draw_order_checks = 0;
    size_t deform_skipped_unbaked = 0;  // 参考侧非空、但附件是 bbox / point（烘焙期就不收 deform）
    size_t clip_attach_checks = 0;      // 槽位挂着裁剪附件的比较次数（v15）
    // ---- v15 裁剪（每帧跑一遍 SpineMesh.h 的绘制循环）----
    size_t clip_frames = 0;             // 至少开始过一次裁剪的帧
    size_t clip_best_frames = 0;        // 裁剪帧最多的那个资产（编辑器实跑挑它）
    std::string clip_best_asset;
    // tint black：槽位挂着可画附件（region / mesh）且暗色 rgb 非 0 的次数 = 打开 tintBlack 后
    // 画面**真的会变**的次数（暗色已与参考逐槽位比过，这里只量覆盖面）。
    size_t dark_drawn = 0;
    size_t dark_negative = 0;           // 其中有分量 < 0（TwoColor 首帧前的符号 bug 路径）
    size_t dark_best = 0;
    std::string dark_best_asset;
    size_t clip_scope_checks = 0;       // 可画槽位"在不在作用域里"的比较
    size_t clip_core_calls = 0;         // 同输入 clipTriangles 的逐位比较次数
    size_t clip_core_out_vertices = 0;  // 其中输出的顶点数
    size_t clip_core_pieces = 0;        // clipStart 返回的凸块数
    size_t clip_e2e_items = 0;          // 端到端：两边都裁了的项
    size_t clip_e2e_same_topo = 0;      // 其中三角形 / 顶点数相同的
    float clip_e2e_worst = 0.0f;        // 拓扑相同的项里世界坐标最大绝对误差
    float clip_e2e_worst_uv = 0.0f;
    float clip_e2e_worst_rigid = 0.0f;  // 被裁项与裁剪多边形都是 rigid（没有 u8 权重量化）的那部分
    float clip_e2e_worst_rigid_uv = 0.0f;
    size_t clip_e2e_rigid_items = 0;
    size_t mixing_frames = 0;       // 至少一条轨道处于 mixingFrom 中的帧（确认真的在测混合）
    size_t multi_track_frames = 0;  // 两条及以上轨道同时生效的帧
    // 覆盖面：确认各分支真的被走到（脚本是随机的，光看"全过"不知道测没测到）
    size_t hold_frames = 0;       // 被混出的那条里有 Hold 模式的时间轴
    size_t hold_mix_frames = 0;   //                   有 HoldMix 模式的时间轴
    size_t add_frames = 0;        // 有 MixBlend_Add 的上层轨道在生效
    size_t chain2_frames = 0;     // mixingFrom 链长 >= 2（混合途中又被打断）
    float worst_rel = 0.0f;
    std::vector<std::string> details;
};

// xorshift64*：两边吃的是**同一串**命令，所以只要可复现，不需要高质量。
struct S5Rng
{
    uint64_t s;
    explicit S5Rng(uint64_t seed) : s(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}
    uint32_t Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<uint32_t>((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    float Uniform(float a, float b) { return a + (b - a) * (static_cast<float>(Next() >> 8) / 16777216.0f); }
    uint32_t Below(uint32_t n) { return n == 0 ? 0u : Next() % n; }
    bool Chance(float p) { return Uniform(0.0f, 1.0f) < p; }
};

uint64_t HashPath(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (char c : s)
    {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

// 参考侧 propertyId 是否撞号：非 deform 的 id 是 (type << 24) + 下标，天然单射；
// 只有 deform 的 `(Deform << 24) + attachment->_id + slotIndex` 会越界撞进别人的号段。
bool RefHasPropertyIdCollision(spine::SkeletonData* data)
{
    std::map<int, std::string> seen;
    spine::Vector<spine::Animation*>& anims = data->getAnimations();
    for (size_t a = 0; a < anims.size(); ++a)
    {
        spine::Vector<spine::Timeline*>& tls = anims[a]->getTimelines();
        for (size_t t = 0; t < tls.size(); ++t)
        {
            spine::Timeline* tl = tls[t];
            const int id = tl->getPropertyId();
            std::string key;
            if (tl->getRTTI().isExactly(spine::DeformTimeline::rtti))
            {
                spine::DeformTimeline* dt = static_cast<spine::DeformTimeline*>(tl);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "D:%d:%p", dt->getSlotIndex(), static_cast<void*>(dt->getAttachment()));
                key = buf;
            }
            else
            {
                key = "N:" + std::to_string(id);
            }
            auto [it, inserted] = seen.emplace(id, key);
            if (!inserted && it->second != key)
            {
                return true;
            }
        }
    }
    return false;
}

const char* S5BlendName(bool add)
{
    return add ? "Add" : "Replace";
}

// 比一帧。返回空串 = 全部一致；否则是第一处分叉的描述。
std::string S5CompareFrame(const SpineGpuAsset& asset, const SpineGpuRuntimeData& data, const SpineGpuSkeletonState& st,
                           const SpineGpuPoseResult& pose, spine::Skeleton& ref, spine::SkeletonData* ref_data,
                           S5Stats& s5)
{
    char buf[768];
    // ---- 骨骼 ----
    spine::Vector<spine::Bone*>& rb = ref.getBones();
    if (rb.size() != pose.bones.size())
    {
        return "骨骼数不同";
    }
    for (size_t i = 0; i < rb.size(); ++i)
    {
        const SpineGpuBoneSample& o = pose.bones[i];
        const float rv[6] = {rb[i]->getA(), rb[i]->getB(), rb[i]->getC(), rb[i]->getD(), rb[i]->getWorldX(),
                             rb[i]->getWorldY()};
        const float ov[6] = {o.a, o.b, o.c, o.d, o.world_x, o.world_y};
        // 世界平移按"参与求和的量级"定尺度：`wx = pa*ax + pb*ay + p.wx`。三项各几百、和却接近 0 时，
        // 1 ulp 的旋转差就能在 0 附近放大成 1e-4 的绝对差——按 max(1, |值|) 算就是"超差"（实测：
        // Ui_Yao bone9 局部 x=653.8、世界 x=0.066，差 1.2e-4 = 1.6 ulp × 653）。S4 的单动画对拍每个动画
        // 只有 5 个采样点碰不到，S5 每轮 23 万帧就碰得到。
        float xy_scale = std::fmax(std::fabs(rv[4]), std::fabs(rv[5]));
        {
            spine::Bone* p = rb[i]->getParent();
            const float ax = std::fabs(rb[i]->getAX()) + std::fabs(rb[i]->getAY());
            if (p != nullptr)
            {
                const float pm = std::fmax(std::fmax(std::fabs(p->getA()), std::fabs(p->getB())),
                                           std::fmax(std::fabs(p->getC()), std::fabs(p->getD())));
                xy_scale = std::fmax(xy_scale, std::fmax(std::fabs(p->getWorldX()), std::fabs(p->getWorldY())));
                xy_scale = std::fmax(xy_scale, ax * pm);
            }
            else
            {
                xy_scale = std::fmax(xy_scale, ax);
            }
        }
        float worst = 0.0f;
        bool ok = true;
        for (int k = 0; k < 6; ++k)
        {
            ++s5.bone_checks;
            if (k < 4)
            {
                ok = CloseEnough(ov[k], rv[k], worst) && ok;
            }
            else
            {
                // 把两侧都缩到 xy_scale 的尺度上再比（与 CloseEnough 同一套相对容差）
                const float sc = std::fmax(1.0f, xy_scale);
                ok = CloseEnough(ov[k] / sc, rv[k] / sc, worst) && ok;
            }
        }
        s5.worst_rel = std::max(s5.worst_rel, worst);
        if (!ok)
        {
            const SpineGpuBoneState& b = st.bones[i];
            spine::Bone* r = rb[i];
            std::snprintf(buf, sizeof(buf),
                          "骨骼 %zu '%s'：我们 (a=%g b=%g c=%g d=%g wx=%g wy=%g | 局部 x=%g y=%g r=%g sx=%g sy=%g shx=%g "
                          "shy=%g) vs 参考 (a=%g b=%g c=%g d=%g wx=%g wy=%g | 局部 x=%g y=%g r=%g sx=%g sy=%g shx=%g shy=%g)",
                          i, r->getData().getName().buffer(), ov[0], ov[1], ov[2], ov[3], ov[4], ov[5], b.x, b.y,
                          b.rotation, b.scale_x, b.scale_y, b.shear_x, b.shear_y, rv[0], rv[1], rv[2], rv[3], rv[4],
                          rv[5], r->getX(), r->getY(), r->getRotation(), r->getScaleX(), r->getScaleY(), r->getShearX(),
                          r->getShearY());
            return buf;
        }
    }
    // ---- 槽位 ----
    spine::Vector<spine::Slot*>& rs = ref.getSlots();
    if (rs.size() != st.slots.size())
    {
        return "槽位数不同";
    }
    for (size_t si = 0; si < rs.size(); ++si)
    {
        ++s5.slot_checks;
        spine::Slot* r = rs[si];
        const SpineGpuSlotState& o = st.slots[si];
        spine::Attachment* ra = r->getAttachment();
        const std::string our_name =
            o.attachment.name_id == SpineGpuAttachRef::kNone ? std::string() : data.names[o.attachment.name_id];
        bool att_ok;
        if (ra != nullptr)
        {
            att_ok = our_name == ra->getName().buffer();
        }
        else
        {
            // 参考是 NULL：我们要么也不挂，要么挂的名字在参考的皮肤里同样找不到（见 SpineGpuAttachRef 的注释）
            att_ok = our_name.empty() || ref_data->getDefaultSkin() == nullptr ||
                     ref_data->getDefaultSkin()->getAttachment(si, spine::String(our_name.c_str())) == nullptr;
        }
        if (!att_ok)
        {
            return "槽位 " + std::to_string(si) + " '" + r->getData().getName().buffer() + "' 当前附件：我们 '" +
                   our_name + "' vs 参考 '" + (ra != nullptr ? ra->getName().buffer() : "<无>") + "'";
        }
        const spine::Color& rc = r->getColor();
        const spine::Color& rd = r->getDarkColor();
        const float rcol[8] = {rc.r, rc.g, rc.b, rc.a, rd.r, rd.g, rd.b, rd.a};
        const float ocol[8] = {o.color[0], o.color[1], o.color[2], o.color[3],
                               o.dark[0],  o.dark[1],  o.dark[2],  o.dark[3]};
        for (int k = 0; k < 8; ++k)
        {
            float w = 0.0f;
            if (!CloseEnough(ocol[k], rcol[k], w))
            {
                const int b0 = k < 4 ? 0 : 4;
                std::snprintf(buf, sizeof(buf), "槽位 %zu '%s' %s：我们 (%g %g %g %g) vs 参考 (%g %g %g %g)", si,
                              r->getData().getName().buffer(), k < 4 ? "颜色" : "暗色", ocol[b0], ocol[b0 + 1],
                              ocol[b0 + 2], ocol[b0 + 3], rcol[b0], rcol[b0 + 1], rcol[b0 + 2], rcol[b0 + 3]);
                return buf;
            }
        }
        if (ra != nullptr && (ra->getRTTI().isExactly(spine::RegionAttachment::rtti) ||
                              ra->getRTTI().isExactly(spine::MeshAttachment::rtti)) &&
            (rd.r != 0.0f || rd.g != 0.0f || rd.b != 0.0f))
        {
            ++s5.dark_drawn;
            s5.dark_negative += (rd.r < 0.0f || rd.g < 0.0f || rd.b < 0.0f) ? 1u : 0u;
        }
        spine::Vector<float>& rv = r->getAttachmentVertices();
        // 只比 mesh / path / clipping（v15）的 deform：bbox / point 的 deform 时间轴在**烘焙期就跳过**
        // （不改姿势、也不改画面）。
        const bool is_clip = ra != nullptr && ra->getRTTI().isExactly(spine::ClippingAttachment::rtti);
        if (is_clip)
        {
            // v15：槽位挂着裁剪附件 ⇒ 我们的 AttachRef 必须认出它是哪条裁剪多边形
            ++s5.clip_attach_checks;
            if (o.attachment.clip == SpineGpuAttachRef::kNone)
            {
                return "槽位 " + std::to_string(si) + " '" + r->getData().getName().buffer() + "' 挂着裁剪附件 '" +
                       our_name + "'，我们没认出来（AttachRef.clip = kNone）";
            }
        }
        const bool baked_kind = ra != nullptr && (ra->getRTTI().isExactly(spine::MeshAttachment::rtti) ||
                                                  ra->getRTTI().isExactly(spine::PathAttachment::rtti) || is_clip);
        if (ra != nullptr && !baked_kind)
        {
            if (!o.deform.empty())
            {
                return "槽位 " + std::to_string(si) + " '" + r->getData().getName().buffer() +
                       "' 挂的是不烘焙 deform 的附件类型，我们却有 deform 数组";
            }
            s5.deform_skipped_unbaked += rv.size() != 0 ? 1u : 0u;
            continue;
        }
        if (rv.size() != o.deform.size())
        {
            return "槽位 " + std::to_string(si) + " '" + r->getData().getName().buffer() + "' deform 长度：我们 " +
                   std::to_string(o.deform.size()) + " vs 参考 " + std::to_string(rv.size()) + "（附件 '" + our_name +
                   "'）";
        }
        for (size_t q = 0; q < rv.size(); ++q)
        {
            ++s5.deform_values;
            float w = 0.0f;
            if (!CloseEnough(o.deform[q], rv[q], w))
            {
                std::snprintf(buf, sizeof(buf), "槽位 %zu '%s' deform[%zu]：我们 %g vs 参考 %g（附件 '%s'）", si,
                              r->getData().getName().buffer(), q, o.deform[q], rv[q], our_name.c_str());
                return buf;
            }
        }
    }
    // ---- 绘制顺序 ----
    spine::Vector<spine::Slot*>& rdo = ref.getDrawOrder();
    if (rdo.size() != st.draw_order.size())
    {
        return "绘制顺序长度不同";
    }
    for (size_t i = 0; i < rdo.size(); ++i)
    {
        ++s5.draw_order_checks;
        if (static_cast<uint32_t>(rdo[i]->getData().getIndex()) != st.draw_order[i])
        {
            return "绘制顺序第 " + std::to_string(i) + " 位：我们槽位 " + std::to_string(st.draw_order[i]) + " vs 参考 " +
                   std::to_string(rdo[i]->getData().getIndex());
        }
    }
    // ---- path 约束运行时参数 ----
    spine::Vector<spine::PathConstraint*>& rp = ref.getPathConstraints();
    if (rp.size() != asset.paths.size())
    {
        return "path 约束数不同";
    }
    for (size_t i = 0; i < rp.size(); ++i)
    {
        const float rv4[4] = {rp[i]->getPosition(), rp[i]->getSpacing(), rp[i]->getRotateMix(),
                              rp[i]->getTranslateMix()};
        const float ov4[4] = {st.path_position[i], st.path_spacing[i], st.path_rotate_mix[i], st.path_translate_mix[i]};
        for (int k = 0; k < 4; ++k)
        {
            float w = 0.0f;
            if (!CloseEnough(ov4[k], rv4[k], w))
            {
                std::snprintf(buf, sizeof(buf), "path 约束 %zu 参数 #%d：我们 %g vs 参考 %g", i, k, ov4[k], rv4[k]);
                return buf;
            }
        }
    }
    return std::string();
}

std::string S5DescribeRefTracks(spine::AnimationState& ref)
{
    std::string tracks;
    for (size_t ti = 0; ti < ref.getTracks().size(); ++ti)
    {
        spine::TrackEntry* e = ref.getTracks()[ti];
        if (e == nullptr)
        {
            continue;
        }
        std::string from;
        for (spine::TrackEntry* m = e->getMixingFrom(); m != nullptr; m = m->getMixingFrom())
        {
            from += std::string(" <- '") + m->getAnimation()->getName().buffer() + "'";
        }
        char tb[256];
        std::snprintf(tb, sizeof(tb), "\n        轨道 %zu：'%s' t=%g mix %g/%g alpha %g delay %g", ti,
                      e->getAnimation()->getName().buffer(), static_cast<double>(e->getTrackTime()),
                      static_cast<double>(e->getMixTime()), static_cast<double>(e->getMixDuration()),
                      static_cast<double>(e->getAlpha()), static_cast<double>(e->getDelay()));
        tracks += tb + from;
    }
    return tracks;
}

// 一个资产跑 `scripts` 段脚本。返回 false = 出现分叉（细节进 s5.details）。
// ---------------------------------------------------------------------
// v15：裁剪对拍。参考侧照 MeshGeneration/SpineMesh.h:107-259 的绘制循环跑 spine::SkeletonClipping；
// 同一个循环里把**参考的**世界坐标 / UV / 三角形 / 裁剪多边形同时喂给我们的转录版
// （SpineGpuSkeletonClipping），逐位比；再拿我们完整的一帧（SpineGpuClipFrame：状态机 + u8 权重
// 的 CPU 蒙皮）比作用域（硬判据）与端到端几何（只统计）。
// ---------------------------------------------------------------------
std::string S5CompareClip(const SpineGpuAsset& asset, const SpineGpuSkeletonState& st, const SpineGpuPoseResult& pose,
                          spine::Skeleton& ref, S5Stats& s5)
{
    if (asset.clip_polygons.empty())
    {
        return std::string();
    }
    static spine::SkeletonClipping ref_clip;
    static SpineGpuSkeletonClipping core;   // 喂参考输入
    static SpineGpuSkeletonClipping ours;   // 我们完整的一帧
    static SpineGpuClipFrameResult frame;
    ref_clip.clipEnd();
    core.ClipEnd();

    const SpineImportResult fr = SpineGpuClipFrame(asset, st, pose, ours, frame);
    if (!fr.ok)
    {
        return "裁剪帧失败：" + fr.error;
    }

    static const unsigned short kRegionTris[6] = {0, 1, 2, 2, 3, 0};
    std::vector<float> world;
    std::vector<int> ref_clipped(st.slots.size(), -1);   // [slot] = 生效裁剪附件所在槽位，-1 = 不在作用域
    bool any_start = false;
    char buf[256];
    spine::Vector<spine::Slot*>& order = ref.getDrawOrder();
    for (size_t oi = 0; oi < order.size(); ++oi)
    {
        spine::Slot* slot = order[oi];
        spine::Attachment* att = slot->getAttachment();
        const uint32_t si = static_cast<uint32_t>(slot->getData().getIndex());
        if (att == nullptr)
        {
            ref_clip.clipEnd(*slot);
            core.ClipEnd(si);
            continue;
        }
        const float* verts = nullptr;
        const float* uvs = nullptr;
        const unsigned short* tris = nullptr;
        size_t tri_len = 0;
        if (att->getRTTI().isExactly(spine::RegionAttachment::rtti))
        {
            spine::RegionAttachment* ra = static_cast<spine::RegionAttachment*>(att);
            world.assign(8, 0.0f);
            ra->computeWorldVertices(slot->getBone(), world.data(), 0, 2);
            verts = world.data();
            uvs = ra->getUVs().buffer();
            tris = kRegionTris;
            tri_len = 6;
        }
        else if (att->getRTTI().isExactly(spine::MeshAttachment::rtti))
        {
            spine::MeshAttachment* ma = static_cast<spine::MeshAttachment*>(att);
            const size_t wl = ma->getWorldVerticesLength();
            world.assign(wl, 0.0f);
            ma->computeWorldVertices(*slot, 0, wl, world.data(), 0, 2);
            verts = world.data();
            uvs = ma->getUVs().buffer();
            tris = ma->getTriangles().buffer();
            tri_len = ma->getTriangles().size();
        }
        else if (att->getRTTI().isExactly(spine::ClippingAttachment::rtti))
        {
            spine::ClippingAttachment* ca = static_cast<spine::ClippingAttachment*>(att);
            const bool was = ref_clip.isClipping();
            const size_t rn = ref_clip.clipStart(*slot, ca);
            const size_t n = ca->getWorldVerticesLength();
            world.assign(n, 0.0f);
            ca->computeWorldVertices(*slot, 0, n, world.data(), 0, 2);
            spine::SlotData* end = ca->getEndSlot();
            const size_t cn = core.ClipStart(si, end != nullptr ? static_cast<uint16_t>(end->getIndex())
                                                                : SpineGpuConstants::kNoClipEndSlot,
                                             world);
            (void)was;
            if (rn != cn)
            {
                std::snprintf(buf, sizeof(buf), "裁剪：槽位 %u clipStart 凸块数 我们 %zu vs 参考 %zu", si, cn, rn);
                return buf;
            }
            s5.clip_core_pieces += rn;
            any_start = true;
            continue;
        }
        else
        {
            ref_clip.clipEnd(*slot);
            core.ClipEnd(si);
            continue;
        }

        if (ref_clip.isClipping() != core.IsClipping())
        {
            std::snprintf(buf, sizeof(buf), "裁剪：槽位 %u 处 isClipping 参考 %d vs 转录 %d", si, ref_clip.isClipping() ? 1 : 0,
                          core.IsClipping() ? 1 : 0);
            return buf;
        }
        if (ref_clip.isClipping())
        {
            ref_clipped[si] = static_cast<int>(core.ActiveClipRow());
            // ---- 同输入逐位 ----
            std::vector<uint16_t> tris16(tris, tris + tri_len);
            ref_clip.clipTriangles(const_cast<float*>(verts), const_cast<unsigned short*>(tris), tri_len,
                                   const_cast<float*>(uvs), 2);
            core.ClipTriangles(verts, tris16.data(), tri_len, uvs, 2);
            ++s5.clip_core_calls;
            spine::Vector<float>& rv = ref_clip.getClippedVertices();
            spine::Vector<float>& ruv = ref_clip.getClippedUVs();
            spine::Vector<unsigned short>& rt = ref_clip.getClippedTriangles();
            const bool same =
                rv.size() == core.ClippedVertices().size() && ruv.size() == core.ClippedUVs().size() &&
                rt.size() == core.ClippedTriangles().size() &&
                (rv.size() == 0 || std::memcmp(rv.buffer(), core.ClippedVertices().data(), rv.size() * 4u) == 0) &&
                (ruv.size() == 0 || std::memcmp(ruv.buffer(), core.ClippedUVs().data(), ruv.size() * 4u) == 0) &&
                (rt.size() == 0 || std::memcmp(rt.buffer(), core.ClippedTriangles().data(), rt.size() * 2u) == 0);
            if (!same)
            {
                std::snprintf(buf, sizeof(buf),
                              "裁剪：槽位 %u '%s' 同输入 clipTriangles 不逐位一致（顶点 %zu vs %zu，三角形索引 %zu vs %zu）", si,
                              att->getName().buffer(), core.ClippedVertices().size(), rv.size(),
                              core.ClippedTriangles().size(), rt.size());
                return buf;
            }
            s5.clip_core_out_vertices += rv.size() / 2u;

            // ---- 端到端（只统计）----
            const uint32_t k = si < frame.slot_clipped.size() ? frame.slot_clipped[si] : SpineGpuClipFrameResult::kNotClipped;
            if (k != SpineGpuClipFrameResult::kNotClipped)
            {
                const SpineGpuClippedDraw& d = frame.draws[k];
                ++s5.clip_e2e_items;
                if (d.vertex_count * 2u == rv.size() && d.index_count == rt.size() &&
                    std::memcmp(frame.indices.data() + d.index_first, rt.buffer(), rt.size() * 2u) == 0)
                {
                    ++s5.clip_e2e_same_topo;
                    const bool all_rigid =
                        asset.draw_items[d.draw_item].vertex_format == SpineVertexFormat::kRigid &&
                        asset.clip_polygons[d.clip_row].vertex_format == SpineVertexFormat::kRigid;
                    s5.clip_e2e_rigid_items += all_rigid ? 1u : 0u;
                    for (size_t q = 0; q < rv.size(); ++q)
                    {
                        if (all_rigid)
                        {
                            s5.clip_e2e_worst_rigid = std::max(
                                s5.clip_e2e_worst_rigid,
                                std::fabs(frame.xy[static_cast<size_t>(d.vertex_first) * 2u + q] - rv[q]));
                            s5.clip_e2e_worst_rigid_uv = std::max(
                                s5.clip_e2e_worst_rigid_uv,
                                std::fabs(frame.uv[static_cast<size_t>(d.vertex_first) * 2u + q] - ruv[q]));
                        }
                        s5.clip_e2e_worst = std::max(s5.clip_e2e_worst,
                                                     std::fabs(frame.xy[static_cast<size_t>(d.vertex_first) * 2u + q] - rv[q]));
                        s5.clip_e2e_worst_uv = std::max(
                            s5.clip_e2e_worst_uv, std::fabs(frame.uv[static_cast<size_t>(d.vertex_first) * 2u + q] - ruv[q]));
                    }
                }
            }
        }
        ref_clip.clipEnd(*slot);
        core.ClipEnd(si);
    }
    ref_clip.clipEnd();
    core.ClipEnd();
    if (any_start)
    {
        ++s5.clip_frames;
    }

    // ---- 作用域（硬判据）：每个可画槽位"在不在作用域里、被哪条裁" ----
    for (uint32_t si = 0; si < st.slots.size(); ++si)
    {
        if (pose.slot_draw_item[si] == SpineGpuPoseResult::kNoDrawItem)
        {
            continue;
        }
        ++s5.clip_scope_checks;
        const uint32_t k = frame.slot_clipped[si];
        const int ours_clip_slot = k == SpineGpuClipFrameResult::kNotClipped
                                       ? -1
                                       : static_cast<int>(asset.clip_polygons[frame.draws[k].clip_row].slot_index);
        if (ours_clip_slot != ref_clipped[si])
        {
            std::snprintf(buf, sizeof(buf), "裁剪作用域：槽位 %u 我们 %s%d vs 参考 %s%d", si,
                          ours_clip_slot < 0 ? "不裁" : "被槽位 ", ours_clip_slot, ref_clipped[si] < 0 ? "不裁" : "被槽位 ",
                          ref_clipped[si]);
            return buf;
        }
    }
    return std::string();
}

bool RunS5(const fs::path& skel_path, const SpineGpuAsset& asset, spine::SkeletonData* ref_data, size_t scripts,
           size_t frames, size_t max_details, S5Stats& s5)
{
    const std::string short_path = skel_path.parent_path().filename().string() + "/" + skel_path.filename().string();
    const size_t anim_count = asset.anims.size();
    if (anim_count == 0 || ref_data->getAnimations().size() != anim_count)
    {
        return true;
    }
    for (size_t a = 0; a < anim_count; ++a)
    {
        if (a >= asset.anim_names.size() || asset.anim_names[a] != ref_data->getAnimations()[a]->getName().buffer())
        {
            ++s5.assets_bad;
            if (s5.details.size() < max_details)
            {
                s5.details.push_back(short_path + "：动画 " + std::to_string(a) + " 的名字与参考对不上");
            }
            return false;
        }
    }
    ++s5.assets;
    const size_t clip_frames0 = s5.clip_frames;
    struct BestClip
    {
        S5Stats& s;
        size_t f0;
        std::string path;
        size_t d0;
        ~BestClip()
        {
            if (s.clip_frames - f0 > s.clip_best_frames)
            {
                s.clip_best_frames = s.clip_frames - f0;
                s.clip_best_asset = path;
            }
            if (s.dark_drawn - d0 > s.dark_best)
            {
                s.dark_best = s.dark_drawn - d0;
                s.dark_best_asset = path;
            }
        }
    } best_clip {s5, clip_frames0, skel_path.string(), s5.dark_drawn};
    const bool collision = RefHasPropertyIdCollision(ref_data);
    if (collision)
    {
        ++s5.assets_collision;
    }

    SpineGpuRuntimeData data;
    SpineImportResult r = SpineGpuBuildRuntimeData(asset, data);
    SpineGpuSkeletonState st;
    if (r.ok)
    {
        r = SpineGpuInitSkeletonState(asset, data, st);
    }
    if (!r.ok)
    {
        ++s5.assets_bad;
        if (s5.details.size() < max_details)
        {
            s5.details.push_back(short_path + "：建运行时状态失败：" + r.error);
        }
        return false;
    }
    spine::Skeleton ref_skel(ref_data);   // 两边都是"刚构造"的骨架，跨脚本一直活着

    S5Rng rng(HashPath(skel_path.string()));
    spine::Vector<spine::Animation*>& ref_anims = ref_data->getAnimations();

    for (size_t sc = 0; sc < scripts; ++sc)
    {
        ++s5.scripts;
        // ---- 混合时长表（两边同值）----
        SpineGpuAnimationStateData our_asd;
        spine::AnimationStateData ref_asd(ref_data);
        const float default_mixes[4] = {0.0f, 0.1f, 0.2f, 0.35f};
        our_asd.default_mix = default_mixes[rng.Below(4)];
        ref_asd.setDefaultMix(our_asd.default_mix);
        for (int k = 0; k < 4; ++k)
        {
            const uint32_t fa = rng.Below(static_cast<uint32_t>(anim_count));
            const uint32_t ta = rng.Below(static_cast<uint32_t>(anim_count));
            const float d = rng.Uniform(0.0f, 0.6f);
            our_asd.SetMix(fa, ta, d);
            ref_asd.setMix(ref_anims[fa], ref_anims[ta], d);
        }

        SpineGpuAnimationState ours(asset, data, &our_asd);
        spine::AnimationState ref(&ref_asd);
        std::vector<std::string> log;

        if (rng.Chance(0.5f))
        {
            SpineGpuSetToSetupPose(asset, data, st);
            ref_skel.setToSetupPose();
            log.push_back("setToSetupPose");
        }
        {
            const uint32_t a0 = rng.Below(static_cast<uint32_t>(anim_count));
            ours.SetAnimation(0, a0, true);
            ref.setAnimation(0, ref_anims[a0], true);
            log.push_back("f0 set(0, #" + std::to_string(a0) + ", loop)");
        }

        for (size_t f = 0; f < frames; ++f)
        {
            // ---- 命令 ----
            if (f > 0 && rng.Chance(0.07f))
            {
                ++s5.commands;
                const uint32_t op = rng.Below(100);
                const size_t track = rng.Chance(0.5f) ? 0u : (rng.Chance(0.6f) ? 1u : 2u);
                const uint32_t an = rng.Below(static_cast<uint32_t>(anim_count));
                const bool loop = rng.Chance(0.6f);
                char cbuf[160];
                SpineGpuTrackEntry* oe = nullptr;
                spine::TrackEntry* re = nullptr;
                if (op < 32)
                {
                    oe = ours.SetAnimation(track, an, loop);
                    re = ref.setAnimation(track, ref_anims[an], loop);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu set(%zu, #%u, %d)", f, track, an, loop ? 1 : 0);
                }
                else if (op < 55)
                {
                    const float delay = rng.Chance(0.5f) ? rng.Uniform(-0.3f, 0.0f) : rng.Uniform(0.0f, 1.0f);
                    oe = ours.AddAnimation(track, an, loop, delay);
                    re = ref.addAnimation(track, ref_anims[an], loop, delay);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu add(%zu, #%u, %d, delay %g)", f, track, an, loop ? 1 : 0,
                                  static_cast<double>(delay));
                }
                else if (op < 65)
                {
                    const float mix = rng.Uniform(0.0f, 0.5f);
                    oe = ours.SetEmptyAnimation(track, mix);
                    re = ref.setEmptyAnimation(track, mix);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu setEmpty(%zu, %g)", f, track, static_cast<double>(mix));
                }
                else if (op < 73)
                {
                    const float mix = rng.Uniform(0.0f, 0.5f);
                    const float delay = rng.Chance(0.5f) ? 0.0f : rng.Uniform(0.0f, 0.8f);
                    oe = ours.AddEmptyAnimation(track, mix, delay);
                    re = ref.addEmptyAnimation(track, mix, delay);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu addEmpty(%zu, %g, delay %g)", f, track,
                                  static_cast<double>(mix), static_cast<double>(delay));
                }
                else if (op < 77)
                {
                    const size_t ct = track == 0 ? 1u : track;   // 轨道 0 很少清（清了就只剩 setup）
                    ours.ClearTrack(ct);
                    ref.clearTrack(ct);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu clear(%zu)", f, ct);
                }
                else if (op < 80)
                {
                    const float mix = rng.Uniform(0.0f, 0.4f);
                    ours.SetEmptyAnimations(mix);
                    ref.setEmptyAnimations(mix);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu setEmpties(%g)", f, static_cast<double>(mix));
                }
                else if (op < 84)
                {
                    const float ts = rng.Uniform(0.25f, 2.0f);
                    ours.time_scale = ts;
                    ref.setTimeScale(ts);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu stateTimeScale(%g)", f, static_cast<double>(ts));
                }
                else
                {
                    // 改轨道当前那条
                    oe = ours.GetCurrent(track);
                    re = ref.getCurrent(track);
                    std::snprintf(cbuf, sizeof(cbuf), "f%zu tweak(%zu)", f, track);
                }
                log.push_back(cbuf);
                if ((oe == nullptr) != (re == nullptr))
                {
                    ++(collision ? s5.assets_collision_bad : s5.assets_bad);
                    if (s5.details.size() < max_details)
                    {
                        s5.details.push_back(short_path + " 脚本 " + std::to_string(sc) + "：命令 '" + cbuf +
                                             "' 后一边有 TrackEntry、一边没有");
                    }
                    return false;
                }
                // 新建 / 取到的那条随机改几个属性（两边同值）
                if (oe != nullptr && rng.Chance(0.6f))
                {
                    std::string tw;
                    if (track != 0 && rng.Chance(0.5f))
                    {
                        const float al = rng.Uniform(0.0f, 1.0f);
                        oe->alpha = al;
                        re->setAlpha(al);
                        tw += " alpha=" + std::to_string(al);
                    }
                    if (rng.Chance(0.25f))
                    {
                        const float ts = rng.Uniform(0.3f, 2.0f);
                        oe->time_scale = ts;
                        re->setTimeScale(ts);
                        tw += " timeScale=" + std::to_string(ts);
                    }
                    if (rng.Chance(0.25f))
                    {
                        oe->hold_previous = true;
                        re->setHoldPrevious(true);
                        tw += " holdPrevious";
                    }
                    if (rng.Chance(0.2f))
                    {
                        const float th = rng.Uniform(0.0f, 1.0f);
                        oe->attachment_threshold = th;
                        re->setAttachmentThreshold(th);
                        tw += " attTh=" + std::to_string(th);
                    }
                    if (rng.Chance(0.2f))
                    {
                        const float th = rng.Uniform(0.0f, 1.0f);
                        oe->draw_order_threshold = th;
                        re->setDrawOrderThreshold(th);
                        tw += " doTh=" + std::to_string(th);
                    }
                    if (track != 0 && rng.Chance(0.3f))
                    {
                        const bool add = rng.Chance(0.5f);
                        oe->mix_blend = add ? SpineGpuMixBlend::kAdd : SpineGpuMixBlend::kReplace;
                        re->setMixBlend(add ? spine::MixBlend_Add : spine::MixBlend_Replace);
                        tw += std::string(" blend=") + S5BlendName(add);
                    }
                    if (rng.Chance(0.2f))
                    {
                        const float md = rng.Uniform(0.0f, 0.6f);
                        oe->mix_duration = md;
                        re->setMixDuration(md);
                        tw += " mixDuration=" + std::to_string(md);
                    }
                    if (!tw.empty())
                    {
                        log.back() += " {" + tw.substr(1) + "}";
                    }
                }
            }

            // ---- 一帧 ----
            float dt = rng.Uniform(1.0f / 120.0f, 1.0f / 20.0f);
            if (rng.Chance(0.03f))
            {
                dt = 0.0f;
            }
            else if (rng.Chance(0.03f))
            {
                dt = rng.Uniform(0.1f, 0.4f);
            }
            ++s5.frames;
            ours.Update(dt);
            ref.update(dt);
            r = ours.Apply(st);
            ref.apply(ref_skel);
            SpineGpuPoseResult pose;
            if (r.ok)
            {
                r = SpineGpuUpdateWorld(asset, data, st, pose);
            }
            ref_skel.updateWorldTransform();

            size_t live_tracks = 0;
            bool mixing = false;
            bool hold = false;
            bool hold_mix = false;
            bool add_blend = false;
            bool chain2 = false;
            for (size_t ti = 0; ti < ours.TrackCount(); ++ti)
            {
                const SpineGpuTrackEntry* e = ours.GetCurrent(ti);
                if (e != nullptr && e->delay <= 0.0f)
                {
                    ++live_tracks;
                    mixing = mixing || e->mixing_from != nullptr;
                    add_blend = add_blend || (ti != 0 && e->mix_blend == SpineGpuMixBlend::kAdd);
                    chain2 = chain2 || (e->mixing_from != nullptr && e->mixing_from->mixing_from != nullptr);
                    for (const SpineGpuTrackEntry* m = e->mixing_from; m != nullptr; m = m->mixing_from)
                    {
                        for (int32_t mode : m->timeline_mode)
                        {
                            hold = hold || mode == 2;
                            hold_mix = hold_mix || mode == 3;
                        }
                    }
                }
            }
            s5.multi_track_frames += live_tracks >= 2 ? 1u : 0u;
            s5.mixing_frames += mixing ? 1u : 0u;
            s5.hold_frames += hold ? 1u : 0u;
            s5.hold_mix_frames += hold_mix ? 1u : 0u;
            s5.add_frames += add_blend ? 1u : 0u;
            s5.chain2_frames += chain2 ? 1u : 0u;

            std::string diff = r.ok ? S5CompareFrame(asset, data, st, pose, ref_skel, ref_data, s5)
                                    : ("我们的解算失败：" + r.error);
            if (diff.empty())
            {
                diff = S5CompareClip(asset, st, pose, ref_skel, s5);
            }
            if (!diff.empty())
            {
                ++(collision ? s5.assets_collision_bad : s5.assets_bad);
                if (s5.details.size() < max_details)
                {
                    std::string cmds;
                    const size_t from = log.size() > 12 ? log.size() - 12 : 0;
                    for (size_t k = from; k < log.size(); ++k)
                    {
                        cmds += "\n        " + log[k];
                    }
                    s5.details.push_back(short_path + (collision ? "（参考侧 propertyId 撞号）" : "") + " 脚本 " +
                                         std::to_string(sc) + " 第 " + std::to_string(f) + " 帧：" + diff +
                                         "\n      最近的命令：" + cmds + "\n      参考侧轨道：" +
                                         S5DescribeRefTracks(ref));
                }
                return false;
            }
        }
    }
    return true;
}

}   // namespace

// 烘焙 / 读回失败的原因按"错误串去掉引号里的名字"归类计数，最后汇总打印。
// 以前这里只有一个计数 —— 某次改动让失败数从 29 跳到 102 时，看不出是哪条规则在报。
std::map<std::string, size_t> g_bake_fail_kinds;
std::map<std::string, std::string> g_bake_fail_example;
void ReportBakeFailure(const char* stage, const std::filesystem::path& file, const std::string& error)
{
    std::string kind = std::string(stage) + ": ";
    bool in_quote = false;
    for (size_t i = 0; i < error.size(); ++i)
    {
        const char c = error[i];
        if (c == '\'')
        {
            in_quote = !in_quote;
            kind += '\'';
            continue;
        }
        if (!in_quote && (c < '0' || c > '9'))
        {
            kind += c;
        }
    }
    if (++g_bake_fail_kinds[kind] == 1)
    {
        g_bake_fail_example[kind] = file.string() + " :: " + error;
    }
}

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
    size_t vis_items = 0, vis_fb_clip = 0, vis_fb_deform_only = 0;
    size_t vis_found = 0, vis_missing = 0, vis_examples = 0, vis_missing_with_candidates = 0;
    size_t anim_dur_pos = 0, anim_dur_zero = 0, anim_dur_examples = 0;
    bool run_s5 = true;      // S5 多轨混合对拍（--no-s5 关掉）
    bool s5_only = false;    // 只跑 S5（跳过单动画逐时间点对拍）
    size_t s5_scripts = 2;   // 每个资产几段随机脚本
    size_t s5_frames = 150;  // 每段脚本几帧
    S5Stats s5;
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
        else if (a == "--no-s5")
        {
            run_s5 = false;
        }
        else if (a == "--s5-only")
        {
            s5_only = true;
        }
        else if (a == "--s5-scripts" && i + 1 < argc)
        {
            s5_scripts = static_cast<size_t>(std::atoi(argv[++i]));
        }
        else if (a == "--s5-frames" && i + 1 < argc)
        {
            s5_frames = static_cast<size_t>(std::atoi(argv[++i]));
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
                    "      [--flip-book <out.html> [--asset <名字子串>] [--anim N] [--frames N]]\n"
                    "      [--no-s5 | --s5-only] [--s5-scripts N] [--s5-frames N]\n");
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
            ReportBakeFailure("bake", skel_path, r.error);
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
            ReportBakeFailure("read", skel_path, r.error);
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
                                // 屏幕上真画的项里有多少走 fallback、因为什么（deform / 裁剪 / 两者）
                                ++vis_items;
                                if (di.kind == SpineDrawItemKind::kFallback)
                                {
                                    const bool clip = di.clip_index != SpineGpuConstants::kNoClipIndex;
                                    ++(clip ? vis_fb_clip : vis_fb_deform_only);
                                }
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
        RefAttachmentLoader ref_loader(&atlas);
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
        // ---- S6：GPU deform 寻址的前提 ----
        // shader 取 weighted deform 用 `deform[base + 2 × (influence_offset + k - infl_base)]`，
        // 其中 infl_base = 该项第 0 个顶点的 influence_offset。成立的前提：一个附件的影响流在
        // chunk 21/22 里**整块连续、保持原顺序**（参考实现 deform 下标 f 按"顶点序 × 影响序"
        // 递增，VertexAttachment.cpp:110）⇒ 第 v 个顶点的 offset - infl_base = 前 v 个顶点的影响数之和。
        // 影响流去重是按整块做的，所以这条应当恒成立；坏了 GPU 上是"取到别的顶点的偏移"，不报错。
        {
            const auto word = [&](size_t i) {
                uint32_t v = 0;
                std::memcpy(&v, loaded.vertex_buffer.data() + i * 4u, 4);
                return v;
            };
            for (size_t ii = 0; ii < loaded.draw_items.size() && file_ok; ++ii)
            {
                const SpineDrawItem& di = loaded.draw_items[ii];
                if (di.vertex_format != SpineVertexFormat::kWeighted || di.vertex_count == 0)
                {
                    continue;
                }
                const size_t w0 = loaded.weighted_segment_offset / 4u + static_cast<size_t>(di.vertex_base) * 2u;
                const uint32_t infl_base = word(w0);
                uint32_t prefix = 0;
                for (uint32_t v = 0; v < di.vertex_count; ++v)
                {
                    ++st.gpu_addr_checks;
                    const uint32_t off = word(w0 + v * 2u);
                    if (off - infl_base != prefix)
                    {
                        std::printf("[对不上] %s 绘制项 %zu 顶点 %u：influence_offset - infl_base = %u，"
                                    "但前面顶点的影响数之和是 %u（GPU deform 会取错偏移）\n",
                                    skel_path.string().c_str(), ii, v, off - infl_base, prefix);
                        ++st.mismatches;
                        file_ok = false;
                        break;
                    }
                    prefix += word(w0 + v * 2u + 1u);
                }
            }
        }
        bool deform_on_path = false;   // 首个坏样本里，path 约束的目标槽位是否带 deform
        bool deform_scanned = false;
        for (size_t ai = 0; ai < loaded.anims.size() && file_ok && !s5_only; ++ai)
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
                // ---- S6：槽位当前附件 + mesh deform 数组，与参考 `Slot` 逐个对 ----
                // 当前附件决定"画哪一项"，deform 数组决定"顶点怎么动"；两者都是 GPU 那一路的输入，
                // 错了都不报错（画错附件 / 形变错），所以每个采样点都比。
                {
                    spine::Vector<spine::Slot*>& ref_slots = ref_skel.getSlots();
                    if (ref_slots.size() != ours.slot_draw_item.size())
                    {
                        std::printf("[对不上] %s 槽位数：我们 %zu vs 参考 %zu\n", skel_path.string().c_str(),
                                    ours.slot_draw_item.size(), ref_slots.size());
                        ++st.mismatches;
                        file_ok = false;
                    }
                    const size_t ns = std::min(ref_slots.size(), ours.slot_draw_item.size());
                    for (size_t si = 0; si < ns; ++si)
                    {
                        spine::Slot* rs = ref_slots[si];
                        spine::Attachment* ra = rs->getAttachment();
                        const bool ref_mesh = ra != nullptr && ra->getRTTI().isExactly(spine::MeshAttachment::rtti);
                        const bool ref_drawable =
                            ref_mesh || (ra != nullptr && ra->getRTTI().isExactly(spine::RegionAttachment::rtti));
                        const uint32_t item = ours.slot_draw_item[si];
                        ++st.slot_attachment_checks;
                        bool ok = ref_drawable == (item != SpineGpuPoseResult::kNoDrawItem);
                        if (ok && ref_drawable)
                        {
                            const uint16_t aid = loaded.draw_items[item].attachment_id;
                            ok = aid < loaded.attachment_names.size() &&
                                 loaded.attachment_names[aid] == ra->getName().buffer();
                        }
                        if (!ok)
                        {
                            ++st.mismatches;
                            file_ok = false;
                            if (st.details.size() < max_details)
                            {
                                st.details.push_back(skel_path.filename().string() + " 动画 " + std::to_string(ai) +
                                                     " t=" + std::to_string(t) + " 槽位 " + std::to_string(si) +
                                                     " 当前附件：参考 '" +
                                                     (ra != nullptr ? ra->getName().buffer() : "<无>") +
                                                     "'，我们的绘制项 " +
                                                     (item == SpineGpuPoseResult::kNoDrawItem ? std::string("<无>")
                                                                                              : std::to_string(item)));
                            }
                            continue;
                        }
                        if (!ref_drawable)
                        {
                            continue;   // 两边都不画（NULL / bbox / clipping / path / point）
                        }
                        if (!ref_mesh)
                        {
                            // ---- region：每个角的 (位置, UV) 配对要和参考一致 ----
                            // 在**局部空间**比（世界空间里骨骼缩放为 0 时 4 个角重合，配对无从判断）：
                            // 我们第 j 个顶点 = 参考 `_vertexOffset` 第 j 对（BL, UL, UR, BR），
                            // 它的 UV = 参考 `_uvs` 第 {1, 2, 3, 0}[j] 对（`_uvs` 按 BR, BL, UL, UR 配对）。
                            spine::RegionAttachment* rr = static_cast<spine::RegionAttachment*>(ra);
                            spine::Vector<float>& roff = rr->getOffset();
                            spine::Vector<float>& ruv = rr->getUVs();
                            const SpineDrawItem& rdi = loaded.draw_items[item];
                            bool pair_ok = rdi.vertex_count == 4 && ruv.size() == 8 && roff.size() == 8;
                            static const uint32_t kUvOfCorner[4] = {1, 2, 3, 0};
                            for (uint32_t j = 0; j < 4 && pair_ok; ++j)
                            {
                                float bx = 0.0f, by = 0.0f;
                                const size_t at = (static_cast<size_t>(rdi.vertex_base) + j) * 8u;
                                std::memcpy(&bx, loaded.vertex_buffer.data() + at, 4);
                                std::memcpy(&by, loaded.vertex_buffer.data() + at + 4u, 4);
                                const size_t uv =
                                    static_cast<size_t>(static_cast<uint32_t>(rdi.uv_offset + rdi.vertex_base + j)) * 2u;
                                const uint32_t k = kUvOfCorner[j];
                                pair_ok = std::fabs(bx - roff[j * 2u]) <= 1e-3f && std::fabs(by - roff[j * 2u + 1u]) <= 1e-3f &&
                                          uv + 1u < loaded.uv_buffer.size() &&
                                          std::fabs(loaded.uv_buffer[uv] - ruv[k * 2u]) <= 1e-6f &&
                                          std::fabs(loaded.uv_buffer[uv + 1u] - ruv[k * 2u + 1u]) <= 1e-6f;
                                if (!pair_ok && st.region_uv_bad < 3)
                                {
                                    std::printf("    角 %u：我们 pos (%g %g) uv (%g %g) | 参考 off (%g %g) uv[%u] (%g %g)\n", j, bx, by,
                                                uv + 1u < loaded.uv_buffer.size() ? loaded.uv_buffer[uv] : -1.0f,
                                                uv + 1u < loaded.uv_buffer.size() ? loaded.uv_buffer[uv + 1u] : -1.0f,
                                                roff[j * 2u], roff[j * 2u + 1u], k, ruv[k * 2u], ruv[k * 2u + 1u]);
                                }
                            }
                            ++st.region_uv_checks;
                            if (!pair_ok)
                            {
                                ++st.region_uv_bad;
                                ++st.mismatches;   // v15：UV 配对是硬判据（错了就是贴图转 90° / 拉伸）
                                file_ok = false;
                                if (st.region_uv_bad <= 3)
                                {
                                    std::printf("[region UV] %s 槽位 %zu '%s'：角与 UV 的配对和参考不一致\n",
                                                skel_path.filename().string().c_str(), si, ra->getName().buffer());
                                }
                            }
                            continue;
                        }
                        {
                            // ---- mesh：逐顶点 UV 与参考 getUVs()（updateUVs 的结果）----
                            spine::Vector<float>& muv = static_cast<spine::MeshAttachment*>(ra)->getUVs();
                            const SpineDrawItem& mdi = loaded.draw_items[item];
                            bool uv_ok = muv.size() == static_cast<size_t>(mdi.vertex_count) * 2u;
                            for (uint32_t v = 0; v < mdi.vertex_count && uv_ok; ++v)
                            {
                                const size_t uv =
                                    static_cast<size_t>(static_cast<uint32_t>(mdi.uv_offset + mdi.vertex_base + v)) * 2u;
                                uv_ok = uv + 1u < loaded.uv_buffer.size() &&
                                        std::fabs(loaded.uv_buffer[uv] - muv[v * 2u]) <= 1e-5f &&
                                        std::fabs(loaded.uv_buffer[uv + 1u] - muv[v * 2u + 1u]) <= 1e-5f;
                            }
                            ++st.mesh_uv_checks;
                            if (!uv_ok)
                            {
                                ++st.mesh_uv_bad;
                                ++st.mismatches;
                                file_ok = false;
                                if (st.mesh_uv_bad <= 3)
                                {
                                    std::printf("[mesh UV] %s 槽位 %zu '%s'：UV 与参考不一致\n",
                                                skel_path.filename().string().c_str(), si, ra->getName().buffer());
                                    for (uint32_t v = 0; v < 3 && v < mdi.vertex_count && muv.size() >= 6; ++v)
                                    {
                                        const size_t uv = static_cast<size_t>(
                                            static_cast<uint32_t>(mdi.uv_offset + mdi.vertex_base + v)) * 2u;
                                        std::printf("    v%u 我们 (%.6f %.6f) 参考 (%.6f %.6f)\n", v, loaded.uv_buffer[uv],
                                                    loaded.uv_buffer[uv + 1u], muv[v * 2u], muv[v * 2u + 1u]);
                                    }
                                }
                            }
                        }
                        spine::Vector<float>& rv = rs->getAttachmentVertices();
                        const SpineGpuMeshDeform* od = nullptr;
                        for (const SpineGpuMeshDeform& e : ours.mesh_deforms)
                        {
                            if (e.draw_item == item)
                            {
                                od = &e;
                                break;
                            }
                        }
                        ++st.deform_checks;

                        // ---- 端到端：照 spine_gpu.vert 蒙皮 vs 参考 computeWorldVertices ----
                        {
                            spine::MeshAttachment* rm = static_cast<spine::MeshAttachment*>(ra);
                            const size_t wl = rm->getWorldVerticesLength();
                            std::vector<float> ref_world(wl);
                            rm->computeWorldVertices(*rs, 0, wl, ref_world.data(), 0, 2);
                            const SpineDrawItem& di = loaded.draw_items[item];
                            const bool weighted = di.vertex_format == SpineVertexFormat::kWeighted;
                            const auto word = [&](size_t i) {
                                uint32_t v = 0;
                                std::memcpy(&v, loaded.vertex_buffer.data() + i * 4u, 4);
                                return v;
                            };
                            const size_t wbase = loaded.weighted_segment_offset / 4u;
                            // 渲染器的偏置：本测试里 deform 段起点 = 0
                            const uint32_t deform_base =
                                od == nullptr ? 0u
                                              : (weighted ? 0u - 2u * word(wbase + static_cast<size_t>(di.vertex_base) * 2u)
                                                          : 0u - 2u * static_cast<uint32_t>(di.vertex_base));
                            const auto dget = [&](uint32_t f) { return od->values[static_cast<size_t>(f)]; };
                            float worst = 0.0f;
                            bool shape_ok = wl == static_cast<size_t>(di.vertex_count) * 2u;
                            for (uint32_t v = 0; v < di.vertex_count && shape_ok; ++v)
                            {
                                const uint32_t vi = di.vertex_base + v;
                                float x = 0.0f, y = 0.0f;
                                if (!weighted)
                                {
                                    float bx = 0.0f, by = 0.0f;
                                    std::memcpy(&bx, loaded.vertex_buffer.data() + static_cast<size_t>(vi) * 8u, 4);
                                    std::memcpy(&by, loaded.vertex_buffer.data() + static_cast<size_t>(vi) * 8u + 4u, 4);
                                    if (od != nullptr)
                                    {
                                        const uint32_t f = deform_base + vi * 2u;
                                        bx = dget(f);
                                        by = dget(f + 1u);
                                    }
                                    const SpineGpuBoneSample& b = ours.bones[loaded.slot_bone[si]];
                                    x = bx * b.a + by * b.b + b.world_x;
                                    y = bx * b.c + by * b.d + b.world_y;
                                }
                                else
                                {
                                    const uint32_t off = word(wbase + static_cast<size_t>(vi) * 2u);
                                    const uint32_t cnt = word(wbase + static_cast<size_t>(vi) * 2u + 1u);
                                    for (uint32_t k = 0; k < cnt; ++k)
                                    {
                                        float bx = loaded.influence_bind[(static_cast<size_t>(off) + k) * 2u];
                                        float by = loaded.influence_bind[(static_cast<size_t>(off) + k) * 2u + 1u];
                                        if (od != nullptr)
                                        {
                                            const uint32_t f = deform_base + (off + k) * 2u;
                                            bx += dget(f);
                                            by += dget(f + 1u);
                                        }
                                        const SpineInfluenceBone& ib = loaded.influence_bone[off + k];
                                        const SpineGpuBoneSample& b = ours.bones[ib.bone_idx];
                                        const float w = static_cast<float>(ib.weight) / 255.0f;
                                        x += (bx * b.a + by * b.b + b.world_x) * w;
                                        y += (bx * b.c + by * b.d + b.world_y) * w;
                                    }
                                }
                                worst = std::max(worst, std::max(std::fabs(x - ref_world[v * 2u]),
                                                                 std::fabs(y - ref_world[v * 2u + 1u])));
                            }
                            float& cell = st.skin_worst[weighted ? 1 : 0][od != nullptr ? 1 : 0];
                            st.skin_verts[weighted ? 1 : 0][od != nullptr ? 1 : 0] += di.vertex_count;
                            if (!shape_ok || std::isnan(worst))
                            {
                                worst = 1.0e30f;
                            }
                            if (worst > cell)
                            {
                                cell = worst;
                                // weighted 的误差被 u8 权重量化主导（见 skin_worst 注释），只有 rigid 超阈值才值得逐条列出
                                if (!weighted && worst > kRigidSkinTol && st.details.size() < max_details)
                                {
                                    st.details.push_back(skel_path.filename().string() + " 动画 " + std::to_string(ai) +
                                                         " t=" + std::to_string(t) + " 槽位 " + std::to_string(si) +
                                                         " mesh '" + ra->getName().buffer() + "' 蒙皮最大误差 " +
                                                         std::to_string(worst) + (od != nullptr ? "（有 deform）" : ""));
                                }
                            }
                        }

                        if (rv.size() == 0 && od == nullptr)
                        {
                            continue;
                        }
                        ++st.deform_nonempty;
                        bool dok = od != nullptr && od->values.size() == rv.size();
                        float dworst = 0.0f;
                        if (dok)
                        {
                            for (size_t q = 0; q < rv.size(); ++q)
                            {
                                ++st.deform_values;
                                if (!CloseEnough(od->values[q], rv[q], dworst))
                                {
                                    dok = false;
                                }
                            }
                        }
                        if (dworst > st.worst_deform_rel)
                        {
                            st.worst_deform_rel = dworst;
                        }
                        if (!dok)
                        {
                            ++st.mismatches;
                            file_ok = false;
                            if (st.details.size() < max_details)
                            {
                                st.details.push_back(skel_path.filename().string() + " 动画 " + std::to_string(ai) +
                                                     " t=" + std::to_string(t) + " 槽位 " + std::to_string(si) +
                                                     " mesh '" + ra->getName().buffer() + "' 的 deform：参考 " +
                                                     std::to_string(rv.size()) + " 个 float，我们 " +
                                                     (od != nullptr ? std::to_string(od->values.size()) : "<无>") +
                                                     " 个，最大相对误差 " + std::to_string(dworst));
                            }
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

        // ---- S5：多轨混合（单动画对上了才跑，否则分叉全是单动画的问题）----
        if (file_ok && run_s5 && (flip_asset.empty() || skel_path.string().find(flip_asset) != std::string::npos))
        {
            RunS5(skel_path, loaded, ref_data, s5_scripts, s5_frames, max_details, s5);
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

    for (const auto& kv : g_bake_fail_kinds)
    {
        std::printf("[烘焙失败] %zu 个：%s\n    例：%s\n", kv.second, kv.first.c_str(),
                    g_bake_fail_example[kv.first].c_str());
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
        std::printf("[uv-scan] t=0 可见绘制项 %zu：fallback 中带裁剪的 %zu，只因 deform 的 %zu\n", vis_items, vis_fb_clip,
                    vis_fb_deform_only);
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
    std::printf("槽位当前附件对拍     : %zu\n", st.slot_attachment_checks);
    std::printf("region 角/UV 配对    : %zu 次，对不上 %zu 次\n", st.region_uv_checks, st.region_uv_bad);
    std::printf("mesh 逐顶点 UV       : %zu 次，对不上 %zu 次\n", st.mesh_uv_checks, st.mesh_uv_bad);
    std::printf("mesh deform 对拍     : %zu 个槽位采样（参考非空 %zu），逐 float %zu，最大相对误差 %.3g\n",
                st.deform_checks, st.deform_nonempty, st.deform_values, static_cast<double>(st.worst_deform_rel));
    std::printf("GPU deform 寻址核对  : %zu 个 weighted 顶点\n", st.gpu_addr_checks);
    std::printf("蒙皮端到端（世界坐标最大绝对误差，单位 = 骨架单位）:\n");
    std::printf("    rigid    无 deform %zu 顶点 %.4g | 有 deform %zu 顶点 %.4g\n", st.skin_verts[0][0],
                static_cast<double>(st.skin_worst[0][0]), st.skin_verts[0][1], static_cast<double>(st.skin_worst[0][1]));
    std::printf("    weighted 无 deform %zu 顶点 %.4g | 有 deform %zu 顶点 %.4g（权重 u8 量化的底噪看左边）\n",
                st.skin_verts[1][0], static_cast<double>(st.skin_worst[1][0]), st.skin_verts[1][1],
                static_cast<double>(st.skin_worst[1][1]));
    // 门槛：rigid 两格都是"同一套 float 运算、只差求和顺序"⇒ 1e-3 骨架单位足够宽；
    // weighted 有 deform 的那格不许明显超过无 deform 的底噪（寻址 / 加法错了是成百上千单位）。
    if (st.skin_worst[0][0] > kRigidSkinTol || st.skin_worst[0][1] > kRigidSkinTol)
    {
        std::printf("[对不上] rigid 蒙皮误差超过 %g\n", static_cast<double>(kRigidSkinTol));
        ++st.mismatches;
    }
    if (st.skin_verts[1][1] > 0 && st.skin_worst[1][1] > 1.5f * st.skin_worst[1][0] + kRigidSkinTol)
    {
        std::printf("[对不上] weighted 有 deform 的蒙皮误差 %g 明显超过无 deform 的底噪 %g\n",
                    static_cast<double>(st.skin_worst[1][1]), static_cast<double>(st.skin_worst[1][0]));
        ++st.mismatches;
    }
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
    if (run_s5)
    {
        std::printf("\n=== S5 多轨混合对拍（spine::AnimationState）===\n");
        std::printf("参与资产             : %zu（参考侧 propertyId 撞号 %zu）\n", s5.assets, s5.assets_collision);
        std::printf("脚本 / 帧 / 命令      : %zu / %zu / %zu\n", s5.scripts, s5.frames, s5.commands);
        std::printf("  └ 处于混合中的帧    : %zu；两条及以上轨道同时生效的帧 %zu\n", s5.mixing_frames,
                    s5.multi_track_frames);
        std::printf("  └ 覆盖：Hold %zu 帧 / HoldMix %zu 帧 / Add 轨道 %zu 帧 / 混合被打断（链长>=2）%zu 帧\n",
                    s5.hold_frames, s5.hold_mix_frames, s5.add_frames, s5.chain2_frames);
        std::printf("骨骼分量比较         : %zu，最大相对误差 %.3g\n", s5.bone_checks, static_cast<double>(s5.worst_rel));
        std::printf("槽位比较             : %zu（附件 + 颜色 + 暗色 + deform），deform 逐 float %zu\n", s5.slot_checks,
                    s5.deform_values);
        std::printf("  └ bbox / point 的 deform（烘焙期不收，跳过）: %zu 次；槽位挂裁剪附件 %zu 次\n",
                    s5.deform_skipped_unbaked, s5.clip_attach_checks);
        std::printf("绘制顺序比较         : %zu\n", s5.draw_order_checks);
        std::printf("裁剪（v15）          : %zu 帧开始过裁剪，可画槽位作用域比较 %zu 次\n", s5.clip_frames,
                    s5.clip_scope_checks);
        std::printf("  └ 同输入 clipTriangles 逐位: %zu 次（输出 %zu 顶点，clipStart 凸块 %zu）\n", s5.clip_core_calls,
                    s5.clip_core_out_vertices, s5.clip_core_pieces);
        std::printf("  └ 端到端（u8 权重蒙皮）: %zu 项，拓扑相同 %zu 项，世界坐标最大误差 %.3g，UV 最大误差 %.3g\n",
                    s5.clip_e2e_items, s5.clip_e2e_same_topo, static_cast<double>(s5.clip_e2e_worst),
                    static_cast<double>(s5.clip_e2e_worst_uv));
        std::printf("     其中全 rigid（无权重量化）%zu 项：世界坐标最大误差 %.3g，UV 最大误差 %.3g\n", s5.clip_e2e_rigid_items,
                    static_cast<double>(s5.clip_e2e_worst_rigid), static_cast<double>(s5.clip_e2e_worst_rigid_uv));
        std::printf("  └ 裁剪帧最多的资产: %s（%zu 帧）\n", s5.clip_best_asset.c_str(), s5.clip_best_frames);
        std::printf("tint black 覆盖面     : 可画槽位暗色非 0 共 %zu 次（其中有负分量 %zu 次）\n", s5.dark_drawn,
                    s5.dark_negative);
        std::printf("  └ 暗色最多的资产: %s（%zu 次）\n", s5.dark_best_asset.c_str(), s5.dark_best);
        std::printf("分叉的资产           : %zu（撞号资产上的另计 %zu）\n", s5.assets_bad, s5.assets_collision_bad);
        for (const std::string& d : s5.details)
        {
            std::printf("  - %s\n", d.c_str());
        }
    }
    const bool pass = st.mismatches == 0 && s5.assets_bad == 0;
    std::printf("\n=== verdict: %s ===\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}
