#include "Runtime/Function/Render/Spine/SpineGpuPoseSolver.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// =====================================================================
// 参考实现：G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp（**3.8**）
//   * Bone::updateWorldTransform()          Bone.cpp:91-206
//   * Bone::updateAppliedTransform()        Bone.cpp（_a* ← 局部值）
//   * Skeleton::updateWorldTransform()      Skeleton.cpp:199-216（复位表 + 遍历）
//   * TransformConstraint::apply*()         TransformConstraint.cpp:138-382
//   * IkConstraint::apply()                 IkConstraint.cpp:46-208
//   * PathConstraint::update()              PathConstraint.cpp:75-213
//   * PathConstraint::computeWorldPositions PathConstraint.cpp:267-523
//   * PathConstraint::addBefore/After/CurvePosition  PathConstraint.cpp:525-570
//   * VertexAttachment::computeWorldVertices VertexAttachment.cpp:64-...
//   * RotateTimeline::apply()               RotateTimeline.cpp:49-106
//   * TranslateTimeline::apply()            TranslateTimeline.cpp:57-109
//   * ScaleTimeline::apply()                ScaleTimeline.cpp:55-142
//   * ShearTimeline::apply()                ShearTimeline.cpp:52-104
//   * PathConstraintPositionTimeline::apply PathConstraintPositionTimeline.cpp:59-100
//   * PathConstraintMixTimeline::apply      PathConstraintMixTimeline.cpp:59-116
//
// ⚠️ 3.7 与 3.8 的**数学**有 2 处差异（§4.1），两处都是 3.8 修掉的 3.7 bug，
//    本文件按 **3.8** 实现（逐文件 diff 已剥掉 license 头核对过）：
//      · Bone.cpp 根骨骼分支的 `_b/_c` 用了 sx/sy（3.7 用反）—— 对我们休眠，
//        因为 `Skeleton::_scaleX/_scaleY` 不从文件读、恒为 1。
//      · IkConstraint.cpp 单骨骼分支的 `&&`/`||` 优先级（3.7 少了括号）
//        ⇒ 3.7 在 `b≈0` 且开启 compress 时会算出 NaN。
// =====================================================================

namespace
{

constexpr float kPi = 3.1415926535897932385f;
constexpr float kPi2 = kPi * 2.0f;
constexpr float kDegRad = kPi / 180.0f;
constexpr float kRadDeg = 180.0f / kPi;

// `PathConstraint::EPSILON`（PathConstraint.cpp:49 附近）
constexpr float kPathEpsilon = 0.00001f;

inline float SinDeg(float d) { return std::sin(d * kDegRad); }
inline float CosDeg(float d) { return std::cos(d * kDegRad); }
inline float Atan2(float y, float x) { return std::atan2(y, x); }
inline float Sqrt(float v) { return std::sqrt(v); }
inline float Acos(float v) { return std::acos(v); }
inline float Fmod(float a, float b) { return std::fmod(a, b); }
inline float Abs(float v) { return v < 0.0f ? -v : v; }
inline bool IsNan(float v) { return std::isnan(v); }

// `r -= (16384 - (int)(16384.499999999996 - r / 360)) * 360` —— 把角度收进 (-180, 180]。
//
// ⚠️ 这个式子出现在 6 个地方（RotateTimeline ×2、transform 约束的 local 变体 ×2、
//    IK 的收尾没有用这个写法）。它**不是** `fmod`：
//      * `(16384 - (int)(...)) * 360` 全程是 **int** 运算，最后才转 float；
//      * `(int)` 是**向零截断**，不是 floor。
//    改写成 fmod 会在负数侧差 360°，且不报错。
inline float WrapDegrees(float r)
{
    const int k = static_cast<int>(16384.499999999996 - static_cast<double>(r / 360.0f));
    return r - static_cast<float>((16384 - k) * 360);
}

SpineImportResult Fail(std::string message)
{
    SpineImportResult r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

SpineImportResult Ok()
{
    SpineImportResult r;
    r.ok = true;
    return r;
}

// spine-cpp 的 `Bone` 在解算期间的全部可变状态。
//
// 分三层，与参考实现**逐字段对应**，不要合并：
//   * `x..shear_y`  = 局部值（时间轴写它）
//   * `ax..ashear_y` = "applied" 值（约束读/写它；`updateWorldTransform` 也会同步它）
//   * `a..world_y`  = 世界变换（最终要上传的东西）
struct BoneState
{
    // 局部
    float x = 0.0f;
    float y = 0.0f;
    float rotation = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float shear_x = 0.0f;
    float shear_y = 0.0f;
    // applied
    float ax = 0.0f;
    float ay = 0.0f;
    float arotation = 0.0f;
    float ascale_x = 1.0f;
    float ascale_y = 1.0f;
    float ashear_x = 0.0f;
    float ashear_y = 0.0f;
    // 世界
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float world_x = 0.0f;
    float world_y = 0.0f;

    bool applied_valid = false;

    // setup 常量
    float length = 0.0f;
    int32_t parent = -1;
    uint32_t mode = 0;
};

struct PoseContext
{
    const SpineGpuAsset& asset;
    std::vector<BoneState> bones;

    // path 约束的运行时参数（可被 pathConstraintPosition / Mix 时间轴改写）
    std::vector<float> path_position;
    std::vector<float> path_spacing;
    std::vector<float> path_rotate_mix;
    std::vector<float> path_translate_mix;
    // 每条 path 约束**当前**看到的那条 chunk 20 记录；`kNoPathAttachment` = 不生效。
    std::vector<uint32_t> path_att;
    // 每条 chunk 20 记录的 deform 数组（= 参考实现里该槽位的 `_attachmentVertices`）。
    // 空 = 没有 deform（用 setup 顶点）。rigid 是**绝对坐标**、weighted 是**偏移**，
    // 两者长度都是 `SpineGpuPathDeformLength()`。
    std::vector<std::vector<float>> path_deform;

    // PathConstraint::computeWorldPositions 的暂存（`_positions` / `_world` / `_curves` / `_segments`）
    std::vector<float> positions;
    std::vector<float> world;
    std::vector<float> curves;
    std::vector<float> lengths;   // TransformConstraint(scale) 与 PathConstraint(scale) 各用一份
    std::vector<float> spaces;
    float segments[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    explicit PoseContext(const SpineGpuAsset& a) : asset(a) {}
};

// ---------------------------------------------------------------------
// Bone
// ---------------------------------------------------------------------

// `Bone::updateWorldTransform(x, y, rotation, scaleX, scaleY, shearX, shearY)`（Bone.cpp:91-206）。
//
// ⚠️ 它会**顺带把 applied 值同步成入参**并置 `applied_valid = true` —— 这是参考实现
//    里"约束改了世界矩阵之后，下一个约束读到的是改后的值"这条链的载体。
//    少了这三行，约束之间的传递会静默断掉。
void UpdateWorldTransform(PoseContext& ctx, uint32_t i, float x, float y, float rotation, float scale_x, float scale_y,
                          float shear_x, float shear_y)
{
    BoneState& bo = ctx.bones[i];
    bo.ax = x;
    bo.ay = y;
    bo.arotation = rotation;
    bo.ascale_x = scale_x;
    bo.ascale_y = scale_y;
    bo.ashear_x = shear_x;
    bo.ashear_y = shear_y;
    bo.applied_valid = true;

    // `Skeleton::_scaleX/_scaleY/_x/_y` **不从文件读**（SkeletonJson/Binary 里没有
    // 给骨架 scale 的字段，构造时就是 1 / 0），且 `Bone::yDown` 默认 false
    // ⇒ getScaleX()/getScaleY() 恒为 1、getX()/getY() 恒为 0。
    // 之所以还写成常量而不是直接删掉：3.7 与 3.8 的差异**就在**这两个值上
    // （Bone.cpp:110-113），留着它们才能看出"我们选的是 3.8 那一支"。
    const float sx = 1.0f;
    const float sy = 1.0f;

    if (bo.parent < 0)
    {
        const float rotation_y = rotation + 90.0f + shear_y;
        // ⚠️ 3.8：`_b` 用 sx、`_c` 用 sy（3.7 用反 —— 差异 1，见文件头）
        bo.a = CosDeg(rotation + shear_x) * scale_x * sx;
        bo.b = CosDeg(rotation_y) * scale_y * sx;
        bo.c = SinDeg(rotation + shear_x) * scale_x * sy;
        bo.d = SinDeg(rotation_y) * scale_y * sy;
        bo.world_x = x * sx;
        bo.world_y = y * sy;
        return;
    }

    const BoneState& p = ctx.bones[static_cast<size_t>(bo.parent)];
    const float pa = p.a;
    const float pb = p.b;
    const float pc = p.c;
    const float pd = p.d;

    bo.world_x = pa * x + pb * y + p.world_x;
    bo.world_y = pc * x + pd * y + p.world_y;

    switch (bo.mode)
    {
        case static_cast<uint32_t>(SpineTransformMode::kNormal):
        {
            const float rotation_y = rotation + 90.0f + shear_y;
            const float la = CosDeg(rotation + shear_x) * scale_x;
            const float lb = CosDeg(rotation_y) * scale_y;
            const float lc = SinDeg(rotation + shear_x) * scale_x;
            const float ld = SinDeg(rotation_y) * scale_y;
            bo.a = pa * la + pb * lc;
            bo.b = pa * lb + pb * ld;
            bo.c = pc * la + pd * lc;
            bo.d = pc * lb + pd * ld;
            return;
        }
        case static_cast<uint32_t>(SpineTransformMode::kOnlyTranslation):
        {
            const float rotation_y = rotation + 90.0f + shear_y;
            bo.a = CosDeg(rotation + shear_x) * scale_x;
            bo.b = CosDeg(rotation_y) * scale_y;
            bo.c = SinDeg(rotation + shear_x) * scale_x;
            bo.d = SinDeg(rotation_y) * scale_y;
            break;
        }
        case static_cast<uint32_t>(SpineTransformMode::kNoRotationOrReflection):
        {
            float s = pa * pa + pc * pc;
            float prx;
            float rx;
            float ry;
            float la;
            float lb;
            float lc;
            float ld;
            float ppa = pa;
            float ppb = pb;
            float ppc = pc;
            float ppd = pd;
            if (s > 0.0001f)
            {
                s = Abs(ppa * ppd - ppb * ppc) / s;
                ppb = ppc * s;
                ppd = ppa * s;
                prx = Atan2(ppc, ppa) * kRadDeg;
            }
            else
            {
                ppa = 0.0f;
                ppc = 0.0f;
                prx = 90.0f - Atan2(ppd, ppb) * kRadDeg;
            }
            rx = rotation + shear_x - prx;
            ry = rotation + shear_y - prx + 90.0f;
            la = CosDeg(rx) * scale_x;
            lb = CosDeg(ry) * scale_y;
            lc = SinDeg(rx) * scale_x;
            ld = SinDeg(ry) * scale_y;
            bo.a = ppa * la - ppb * lc;
            bo.b = ppa * lb - ppb * ld;
            bo.c = ppc * la + ppd * lc;
            bo.d = ppc * lb + ppd * ld;
            break;
        }
        case static_cast<uint32_t>(SpineTransformMode::kNoScale):
        case static_cast<uint32_t>(SpineTransformMode::kNoScaleOrReflection):
        {
            float za;
            float zc;
            float s;
            float r;
            float zb;
            float zd;
            float la;
            float lb;
            float lc;
            float ld;
            const float cosine = CosDeg(rotation);
            const float sine = SinDeg(rotation);
            za = (pa * cosine + pb * sine) / sx;
            zc = (pc * cosine + pd * sine) / sy;
            s = Sqrt(za * za + zc * zc);
            if (s > 0.00001f)
            {
                s = 1.0f / s;
            }
            za *= s;
            zc *= s;
            s = Sqrt(za * za + zc * zc);
            if (bo.mode == static_cast<uint32_t>(SpineTransformMode::kNoScale) &&
                (pa * pd - pb * pc < 0) != (sx < 0 != sy < 0))
            {
                s = -s;
            }
            r = kPi / 2.0f + Atan2(zc, za);
            zb = std::cos(r) * s;
            zd = std::sin(r) * s;
            la = CosDeg(shear_x) * scale_x;
            lb = CosDeg(90.0f + shear_y) * scale_y;
            lc = SinDeg(shear_x) * scale_x;
            ld = SinDeg(90.0f + shear_y) * scale_y;
            bo.a = za * la + zb * lc;
            bo.b = za * lb + zb * ld;
            bo.c = zc * la + zd * lc;
            bo.d = zc * lb + zd * ld;
            break;
        }
        default:
            break;
    }
    bo.a *= sx;
    bo.b *= sx;
    bo.c *= sy;
    bo.d *= sy;
}

// `Bone::update()`（Bone.cpp:82-84）：用**局部**值重算世界变换。
inline void BoneUpdate(PoseContext& ctx, uint32_t i)
{
    const BoneState& bo = ctx.bones[i];
    UpdateWorldTransform(ctx, i, bo.x, bo.y, bo.rotation, bo.scale_x, bo.scale_y, bo.shear_x, bo.shear_y);
}

// `Skeleton::updateWorldTransform()` 第一段（Skeleton.cpp:200-211）：复位表里的骨骼
// applied = **局部**值。这是"每帧开头抹掉上一帧约束残留"，与下面的
// `UpdateAppliedTransform`（从世界矩阵**反推**）不是同一个操作。
inline void ResetAppliedToLocal(PoseContext& ctx, uint32_t i)
{
    BoneState& bo = ctx.bones[i];
    bo.ax = bo.x;
    bo.ay = bo.y;
    bo.arotation = bo.rotation;
    bo.ascale_x = bo.scale_x;
    bo.ascale_y = bo.scale_y;
    bo.ashear_x = bo.shear_x;
    bo.ashear_y = bo.shear_y;
    bo.applied_valid = true;
}

// `Bone::updateAppliedTransform()`（Bone.cpp:501-540）：从**当前世界矩阵**相对父骨骼
// **反推** applied 值。
//
// 参考实现在"约束要读 applied 值但 `!_appliedValid`"时调它；`_appliedValid` 被清掉
// 的唯一原因是 world 模式的 transform / path 约束**直接改了世界矩阵**。
// ⚠️ 早先这里写成"applied = 局部值"——那会把约束改过的世界姿势整个丢掉：
//    下一个 IK 从**约束前**的姿势出发去解。OSGame 实测 13 个资产（全是
//    "transform 约束 → IK"串联）逐骨骼对不上，就是这一个原因。
inline void UpdateAppliedTransform(PoseContext& ctx, uint32_t i)
{
    BoneState& bo = ctx.bones[i];
    bo.applied_valid = true;
    if (bo.parent < 0)
    {
        bo.ax = bo.world_x;
        bo.ay = bo.world_y;
        bo.arotation = Atan2(bo.c, bo.a) * kRadDeg;
        bo.ascale_x = Sqrt(bo.a * bo.a + bo.c * bo.c);
        bo.ascale_y = Sqrt(bo.b * bo.b + bo.d * bo.d);
        bo.ashear_x = 0.0f;
        bo.ashear_y = Atan2(bo.a * bo.b + bo.c * bo.d, bo.a * bo.d - bo.b * bo.c) * kRadDeg;
        return;
    }
    const BoneState& p = ctx.bones[static_cast<size_t>(bo.parent)];
    const float pa = p.a;
    const float pb = p.b;
    const float pc = p.c;
    const float pd = p.d;
    const float pid = 1.0f / (pa * pd - pb * pc);
    const float dx = bo.world_x - p.world_x;
    const float dy = bo.world_y - p.world_y;
    const float ia = pid * pd;
    const float id = pid * pa;
    const float ib = pid * pb;
    const float ic = pid * pc;
    const float ra = ia * bo.a - ib * bo.c;
    const float rb = ia * bo.b - ib * bo.d;
    const float rc = id * bo.c - ic * bo.a;
    const float rd = id * bo.d - ic * bo.b;
    // ⚠️ 逐字照抄运算顺序（`dx * pd * pid`，不是 `dx * ia`）：两种写法数学等价，
    //    浮点结果差最后几位，而对拍的容差就是按"逐字照抄"定的。
    bo.ax = (dx * pd * pid - dy * pb * pid);
    bo.ay = (dy * pa * pid - dx * pc * pid);
    bo.ashear_x = 0.0f;
    bo.ascale_x = Sqrt(ra * ra + rc * rc);
    if (bo.ascale_x > 0.0001f)
    {
        const float det = ra * rd - rb * rc;
        bo.ascale_y = det / bo.ascale_x;
        bo.ashear_y = Atan2(ra * rb + rc * rd, det) * kRadDeg;
        bo.arotation = Atan2(rc, ra) * kRadDeg;
    }
    else
    {
        bo.ascale_x = 0.0f;
        bo.ascale_y = Sqrt(rb * rb + rd * rd);
        bo.ashear_y = 0.0f;
        bo.arotation = 90.0f - Atan2(rd, rb) * kRadDeg;
    }
}

// ---------------------------------------------------------------------
// TransformConstraint
// ---------------------------------------------------------------------

// 四个变体共用的"取目标骨骼 applied 值"前奏（`applyAbsoluteLocal` / `applyRelativeLocal`）。
inline void EnsureApplied(PoseContext& ctx, uint32_t i)
{
    if (!ctx.bones[i].applied_valid)
    {
        UpdateAppliedTransform(ctx, i);
    }
}

// TransformConstraint.cpp:138-216
void TransformApplyAbsoluteWorld(PoseContext& ctx, const SpineGpuTransformConstraint& c, uint32_t ci)
{
    (void)ci;
    if (c.target >= ctx.bones.size())
    {
        return;
    }
    const BoneState& t = ctx.bones[c.target];
    const float ta = t.a;
    const float tb = t.b;
    const float tc = t.c;
    const float td = t.d;
    const float deg_rad_reflect = (ta * td - tb * tc > 0) ? kDegRad : -kDegRad;
    const float offset_rotation = c.offset_rotation * deg_rad_reflect;
    const float offset_shear_y = c.offset_shear_y * deg_rad_reflect;

    for (uint32_t k = 0; k < c.bone_count; ++k)
    {
        const uint32_t bi = ctx.asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + k];
        BoneState& bone = ctx.bones[bi];
        bool modified = false;

        if (c.rotate_mix != 0.0f)
        {
            const float a = bone.a;
            const float b = bone.b;
            const float cc = bone.c;
            const float dd = bone.d;
            float r = Atan2(tc, ta) - Atan2(cc, a) + offset_rotation;
            if (r > kPi)
            {
                r -= kPi2;
            }
            else if (r < -kPi)
            {
                r += kPi2;
            }
            r *= c.rotate_mix;
            const float cs = std::cos(r);
            const float sn = std::sin(r);
            bone.a = cs * a - sn * cc;
            bone.b = cs * b - sn * dd;
            bone.c = sn * a + cs * cc;
            bone.d = sn * b + cs * dd;
            modified = true;
        }

        if (c.translate_mix != 0.0f)
        {
            // `target.localToWorld(offsetX, offsetY)`（Bone.cpp:227-230）
            const float tx = c.offset_x * ta + c.offset_y * tb + t.world_x;
            const float ty = c.offset_x * tc + c.offset_y * td + t.world_y;
            bone.world_x += (tx - bone.world_x) * c.translate_mix;
            bone.world_y += (ty - bone.world_y) * c.translate_mix;
            modified = true;
        }

        if (c.scale_mix > 0.0f)
        {
            float s = Sqrt(bone.a * bone.a + bone.c * bone.c);
            if (s > 0.00001f)
            {
                s = (s + (Sqrt(ta * ta + tc * tc) - s + c.offset_scale_x) * c.scale_mix) / s;
            }
            bone.a *= s;
            bone.c *= s;
            s = Sqrt(bone.b * bone.b + bone.d * bone.d);
            if (s > 0.00001f)
            {
                s = (s + (Sqrt(tb * tb + td * td) - s + c.offset_scale_y) * c.scale_mix) / s;
            }
            bone.b *= s;
            bone.d *= s;
            modified = true;
        }

        if (c.shear_mix > 0.0f)
        {
            const float b = bone.b;
            const float dd = bone.d;
            const float by = Atan2(dd, b);
            float r = Atan2(td, tb) - Atan2(tc, ta) - (by - Atan2(bone.c, bone.a));
            if (r > kPi)
            {
                r -= kPi2;
            }
            else if (r < -kPi)
            {
                r += kPi2;
            }
            r = by + (r + offset_shear_y) * c.shear_mix;
            const float s = Sqrt(b * b + dd * dd);
            bone.b = std::cos(r) * s;
            bone.d = std::sin(r) * s;
            modified = true;
        }

        if (modified)
        {
            bone.applied_valid = false;
        }
    }
}

// TransformConstraint.cpp:218-286
void TransformApplyRelativeWorld(PoseContext& ctx, const SpineGpuTransformConstraint& c)
{
    if (c.target >= ctx.bones.size())
    {
        return;
    }
    const BoneState& t = ctx.bones[c.target];
    const float ta = t.a;
    const float tb = t.b;
    const float tc = t.c;
    const float td = t.d;
    const float deg_rad_reflect = (ta * td - tb * tc > 0) ? kDegRad : -kDegRad;
    const float offset_rotation = c.offset_rotation * deg_rad_reflect;
    const float offset_shear_y = c.offset_shear_y * deg_rad_reflect;

    for (uint32_t k = 0; k < c.bone_count; ++k)
    {
        const uint32_t bi = ctx.asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + k];
        BoneState& bone = ctx.bones[bi];
        bool modified = false;

        if (c.rotate_mix != 0.0f)
        {
            const float a = bone.a;
            const float b = bone.b;
            const float cc = bone.c;
            const float dd = bone.d;
            float r = Atan2(tc, ta) + offset_rotation;
            if (r > kPi)
            {
                r -= kPi2;
            }
            else if (r < -kPi)
            {
                r += kPi2;
            }
            r *= c.rotate_mix;
            const float cs = std::cos(r);
            const float sn = std::sin(r);
            bone.a = cs * a - sn * cc;
            bone.b = cs * b - sn * dd;
            bone.c = sn * a + cs * cc;
            bone.d = sn * b + cs * dd;
            modified = true;
        }

        if (c.translate_mix != 0.0f)
        {
            const float tx = c.offset_x * ta + c.offset_y * tb + t.world_x;
            const float ty = c.offset_x * tc + c.offset_y * td + t.world_y;
            bone.world_x += tx * c.translate_mix;
            bone.world_y += ty * c.translate_mix;
            modified = true;
        }

        if (c.scale_mix > 0.0f)
        {
            float s = (Sqrt(ta * ta + tc * tc) - 1.0f + c.offset_scale_x) * c.scale_mix + 1.0f;
            bone.a *= s;
            bone.c *= s;
            s = (Sqrt(tb * tb + td * td) - 1.0f + c.offset_scale_y) * c.scale_mix + 1.0f;
            bone.b *= s;
            bone.d *= s;
            modified = true;
        }

        if (c.shear_mix > 0.0f)
        {
            float r = Atan2(td, tb) - Atan2(tc, ta);
            if (r > kPi)
            {
                r -= kPi2;
            }
            else if (r < -kPi)
            {
                r += kPi2;
            }
            const float b = bone.b;
            const float dd = bone.d;
            r = Atan2(dd, b) + (r - kPi / 2.0f + offset_shear_y) * c.shear_mix;
            const float s = Sqrt(b * b + dd * dd);
            bone.b = std::cos(r) * s;
            bone.d = std::sin(r) * s;
            modified = true;
        }

        if (modified)
        {
            bone.applied_valid = false;
        }
    }
}

// TransformConstraint.cpp:288-336
//
// ⚠️ 参考实现的 shear 分支写的是 **`bone._shearY`（局部量）** 而不是局部变量
//    `shearY`，所以当帧世界变换拿到的仍是未改的 `bone._ashearY` ⇒
//    **absolute+local 模式下 shearMix 对世界变换没有作用**。这是参考实现的行为，
//    **照抄**（改成 `shearY +=` 会与参考实现对不上，且不报错）。
//    副作用：被改的 `shear_y` 会在**后续** `Bone::update()` 里生效
//    （`update()` 读的是局部值）⇒ 必须真的写进 `shear_y`，不能省。
void TransformApplyAbsoluteLocal(PoseContext& ctx, const SpineGpuTransformConstraint& c)
{
    EnsureApplied(ctx, c.target);
    const BoneState& t = ctx.bones[c.target];

    for (uint32_t k = 0; k < c.bone_count; ++k)
    {
        const uint32_t bi = ctx.asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + k];
        EnsureApplied(ctx, bi);
        BoneState& bone = ctx.bones[bi];

        float rotation = bone.arotation;
        if (c.rotate_mix != 0.0f)
        {
            float r = t.arotation - rotation + c.offset_rotation;
            r = WrapDegrees(r);
            rotation += r * c.rotate_mix;
        }

        float x = bone.ax;
        float y = bone.ay;
        if (c.translate_mix != 0.0f)
        {
            x += (t.ax - x + c.offset_x) * c.translate_mix;
            y += (t.ay - y + c.offset_y) * c.translate_mix;
        }

        float scale_x = bone.ascale_x;
        float scale_y = bone.ascale_y;
        if (c.scale_mix != 0.0f)
        {
            if (scale_x > 0.00001f)
            {
                scale_x = (scale_x + (t.ascale_x - scale_x + c.offset_scale_x) * c.scale_mix) / scale_x;
            }
            if (scale_y > 0.00001f)
            {
                scale_y = (scale_y + (t.ascale_y - scale_y + c.offset_scale_y) * c.scale_mix) / scale_y;
            }
        }

        const float shear_y = bone.ashear_y;
        if (c.shear_mix != 0.0f)
        {
            float r = t.ashear_y - shear_y + c.offset_shear_y;
            r = WrapDegrees(r);
            bone.shear_y += r * c.shear_mix;   // ⚠️ 写的是**局部**量（参考实现就是这样）
        }

        UpdateWorldTransform(ctx, bi, x, y, rotation, scale_x, scale_y, bone.ashear_x, shear_y);
    }
}

// TransformConstraint.cpp:338-382
void TransformApplyRelativeLocal(PoseContext& ctx, const SpineGpuTransformConstraint& c)
{
    EnsureApplied(ctx, c.target);
    const BoneState& t = ctx.bones[c.target];

    for (uint32_t k = 0; k < c.bone_count; ++k)
    {
        const uint32_t bi = ctx.asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + k];
        EnsureApplied(ctx, bi);
        BoneState& bone = ctx.bones[bi];

        float rotation = bone.arotation;
        if (c.rotate_mix != 0.0f)
        {
            rotation += (t.arotation + c.offset_rotation) * c.rotate_mix;
        }

        float x = bone.ax;
        float y = bone.ay;
        if (c.translate_mix != 0.0f)
        {
            x += (t.ax + c.offset_x) * c.translate_mix;
            y += (t.ay + c.offset_y) * c.translate_mix;
        }

        float scale_x = bone.ascale_x;
        float scale_y = bone.ascale_y;
        if (c.scale_mix != 0.0f)
        {
            if (scale_x > 0.00001f)
            {
                scale_x *= ((t.ascale_x - 1.0f + c.offset_scale_x) * c.scale_mix) + 1.0f;
            }
            if (scale_y > 0.00001f)
            {
                scale_y *= ((t.ascale_y - 1.0f + c.offset_scale_y) * c.scale_mix) + 1.0f;
            }
        }

        float shear_y = bone.ashear_y;
        if (c.shear_mix != 0.0f)
        {
            shear_y += (t.ashear_y + c.offset_shear_y) * c.shear_mix;
        }

        UpdateWorldTransform(ctx, bi, x, y, rotation, scale_x, scale_y, bone.ashear_x, shear_y);
    }
}

// ---------------------------------------------------------------------
// IkConstraint
// ---------------------------------------------------------------------

// IkConstraint.cpp:46-70（单骨骼）
//
// ⚠️ 3.8 的括号：`((compress && dd < b) || (stretch && dd > b)) && (b > 0.0001f)`。
//    3.7 少一层括号 ⇒ `b≈0` 且开 compress 时除零得 NaN（差异 2，见文件头）。
void IkApplyOneBone(PoseContext& ctx, uint32_t bi, float target_x, float target_y, bool compress, bool stretch,
                    bool uniform, float alpha)
{
    BoneState& bone = ctx.bones[bi];
    if (!bone.applied_valid)
    {
        UpdateAppliedTransform(ctx, bi);
    }
    if (bone.parent < 0)
    {
        return;   // 参考实现在这里是空指针解引用；调用方已提前报错
    }
    const BoneState& p = ctx.bones[static_cast<size_t>(bone.parent)];
    const float id = 1.0f / (p.a * p.d - p.b * p.c);
    const float x = target_x - p.world_x;
    const float y = target_y - p.world_y;
    const float tx = (x * p.d - y * p.b) * id - bone.ax;
    const float ty = (y * p.a - x * p.c) * id - bone.ay;
    float rotation_ik = Atan2(ty, tx) * kRadDeg - bone.ashear_x - bone.arotation;
    if (bone.ascale_x < 0)
    {
        rotation_ik += 180.0f;
    }
    if (rotation_ik > 180.0f)
    {
        rotation_ik -= 360.0f;
    }
    else if (rotation_ik < -180.0f)
    {
        rotation_ik += 360.0f;
    }
    float sx = bone.ascale_x;
    float sy = bone.ascale_y;
    if (compress || stretch)
    {
        const float bb = bone.length * sx;
        const float dd = Sqrt(tx * tx + ty * ty);
        if (((compress && dd < bb) || (stretch && dd > bb)) && (bb > 0.0001f))
        {
            const float s = (dd / bb - 1.0f) * alpha + 1.0f;
            sx *= s;
            if (uniform)
            {
                sy *= s;
            }
        }
    }
    UpdateWorldTransform(ctx, bi, bone.ax, bone.ay, bone.arotation + rotation_ik * alpha, sx, sy, bone.ashear_x,
                         bone.ashear_y);
}

// IkConstraint.cpp:72-208（双骨骼）
void IkApplyTwoBones(PoseContext& ctx, uint32_t parent_i, uint32_t child_i, float target_x, float target_y,
                     int bend_dir, bool stretch, float alpha)
{
    BoneState& parent = ctx.bones[parent_i];
    BoneState& child = ctx.bones[child_i];
    if (!parent.applied_valid)
    {
        UpdateAppliedTransform(ctx, parent_i);
    }
    if (!child.applied_valid)
    {
        UpdateAppliedTransform(ctx, child_i);
    }
    if (parent.parent < 0)
    {
        return;   // 参考实现在这里是空指针解引用；调用方已提前报错
    }
    const BoneState& pp = ctx.bones[static_cast<size_t>(parent.parent)];

    const float px = parent.ax;
    const float py = parent.ay;
    float psx = parent.ascale_x;
    float sx = psx;
    float psy = parent.ascale_y;
    float csx = child.ascale_x;
    int o1;
    int s2;
    int o2;
    if (psx < 0)
    {
        psx = -psx;
        o1 = 180;
        s2 = -1;
    }
    else
    {
        o1 = 0;
        s2 = 1;
    }
    if (psy < 0)
    {
        psy = -psy;
        s2 = -s2;
    }
    if (csx < 0)
    {
        csx = -csx;
        o2 = 180;
    }
    else
    {
        o2 = 0;
    }
    float r = psx - psy;
    const float cx = child.ax;
    const bool u = Abs(r) <= 0.0001f;
    float cy;
    float cwx;
    float cwy;
    if (!u)
    {
        cy = 0.0f;
        cwx = parent.a * cx + parent.world_x;
        cwy = parent.c * cx + parent.world_y;
    }
    else
    {
        cy = child.ay;
        cwx = parent.a * cx + parent.b * cy + parent.world_x;
        cwy = parent.c * cx + parent.d * cy + parent.world_y;
    }
    const float id = 1.0f / (pp.a * pp.d - pp.b * pp.c);
    float x = target_x - pp.world_x;
    float y = target_y - pp.world_y;
    const float tx = (x * pp.d - y * pp.b) * id - px;
    const float ty = (y * pp.a - x * pp.c) * id - py;
    const float dd = tx * tx + ty * ty;
    x = cwx - pp.world_x;
    y = cwy - pp.world_y;
    const float dx = (x * pp.d - y * pp.b) * id - px;
    const float dy = (y * pp.a - x * pp.c) * id - py;
    const float l1 = Sqrt(dx * dx + dy * dy);
    float l2 = child.length * csx;

    float a1 = 0.0f;
    float a2 = 0.0f;
    bool solved = false;

    if (u)
    {
        l2 *= psx;
        float cosine = (dd - l1 * l1 - l2 * l2) / (2.0f * l1 * l2);
        if (cosine < -1.0f)
        {
            cosine = -1.0f;
        }
        else if (cosine > 1.0f)
        {
            cosine = 1.0f;
            if (stretch && l1 + l2 > 0.0001f)
            {
                sx *= (Sqrt(dd) / (l1 + l2) - 1.0f) * alpha + 1.0f;
            }
        }
        a2 = Acos(cosine) * static_cast<float>(bend_dir);
        const float a = l1 + l2 * cosine;
        const float bb = l2 * std::sin(a2);
        a1 = Atan2(ty * a - tx * bb, tx * a + ty * bb);
        solved = true;
    }
    else
    {
        const float a = psx * l2;
        const float bb = psy * l2;
        const float aa = a * a;
        const float bbb = bb * bb;
        const float ll = l1 * l1;
        const float ta = Atan2(ty, tx);
        float c0 = bbb * ll + aa * dd - aa * bbb;
        const float c1 = -2.0f * bbb * l1;
        const float c2 = bbb - aa;
        float d = c1 * c1 - 4.0f * c2 * c0;
        if (d >= 0.0f)
        {
            float q = Sqrt(d);
            if (c1 < 0.0f)
            {
                q = -q;
            }
            q = -(c1 + q) / 2.0f;
            const float r0 = q / c2;
            const float r1 = c0 / q;
            r = Abs(r0) < Abs(r1) ? r0 : r1;
            if (r * r <= dd)
            {
                y = Sqrt(dd - r * r) * static_cast<float>(bend_dir);
                a1 = ta - Atan2(y, r);
                a2 = Atan2(y / psy, (r - l1) / psx);
                solved = true;
            }
        }
        if (!solved)
        {
            float min_angle = kPi;
            float min_x = l1 - a;
            float min_dist = min_x * min_x;
            float min_y = 0.0f;
            float max_angle = 0.0f;
            float max_x = l1 + a;
            float max_dist = max_x * max_x;
            float max_y = 0.0f;
            c0 = -a * l1 / (aa - bbb);
            if (c0 >= -1.0f && c0 <= 1.0f)
            {
                c0 = Acos(c0);
                x = a * std::cos(c0) + l1;
                y = bb * std::sin(c0);
                d = x * x + y * y;
                if (d < min_dist)
                {
                    min_angle = c0;
                    min_dist = d;
                    min_x = x;
                    min_y = y;
                }
                if (d > max_dist)
                {
                    max_angle = c0;
                    max_dist = d;
                    max_x = x;
                    max_y = y;
                }
            }
            if (dd <= (min_dist + max_dist) / 2.0f)
            {
                a1 = ta - Atan2(min_y * static_cast<float>(bend_dir), min_x);
                a2 = min_angle * static_cast<float>(bend_dir);
            }
            else
            {
                a1 = ta - Atan2(max_y * static_cast<float>(bend_dir), max_x);
                a2 = max_angle * static_cast<float>(bend_dir);
            }
        }
    }

    const float os = Atan2(cy, cx) * static_cast<float>(s2);
    a1 = (a1 - os) * kRadDeg + static_cast<float>(o1) - parent.arotation;
    if (a1 > 180.0f)
    {
        a1 -= 360.0f;
    }
    else if (a1 < -180.0f)
    {
        a1 += 360.0f;
    }
    // ⚠️ 用的是**局部** `rotation`（不是 `arotation`）与 `ascale_y`，shear 传 0,0
    //    （IkConstraint.cpp:201）。
    UpdateWorldTransform(ctx, parent_i, px, py, parent.rotation + a1 * alpha, sx, parent.ascale_y, 0.0f, 0.0f);

    a2 = ((a2 + os) * kRadDeg - child.ashear_x) * static_cast<float>(s2) + static_cast<float>(o2) -
         child.arotation;
    if (a2 > 180.0f)
    {
        a2 -= 360.0f;
    }
    else if (a2 < -180.0f)
    {
        a2 += 360.0f;
    }
    UpdateWorldTransform(ctx, child_i, cx, cy, child.arotation + a2 * alpha, child.ascale_x, child.ascale_y,
                         child.ashear_x, child.ashear_y);
}

// ---------------------------------------------------------------------
// PathAttachment → 世界顶点
// ---------------------------------------------------------------------

// `VertexAttachment::computeWorldVertices()`（VertexAttachment.cpp:64-...）。
//
// `start` / `count` 的单位是 **float**（参考实现的口径：`count = offset + (count >> 1) * stride`，
// stride 默认 2）。抄错单位时偏移为 0 的那次调用逐字节相同，只有**非 0** 的调用能测出来。
//
// ⚠️ 槽位的 deform 数组（`slot.getAttachmentVertices()`）**会**作用于 path：
//    rigid 分支**整体替换**顶点（VertexAttachment.cpp:69-70），weighted 分支**逐影响相加**
//    偏移（:117-118）。早先这里写的是"DeformTimeline 只挂在 mesh 上，path 恒为空"——
//    那是错的：OSGame 75 个逐骨骼对不上的资产里 62 个就是 path 的 deform 被跳过。
//    deform 数组由 `SampleTimelines` 的 5b 步算好放在 `ctx.path_deform`。
void PathComputeWorldVertices(PoseContext& ctx, const SpineGpuPathAttachment& att, uint32_t slot_bone, uint32_t start,
                              uint32_t count, std::vector<float>& out, uint32_t offset)
{
    const uint32_t limit = offset + (count >> 1) * 2u;
    if (limit > out.size())
    {
        out.resize(limit, 0.0f);
    }
    // `att` 是 chunk 20 里的元素 ⇒ 下标就是它在数组里的位置（deform 按这个下标存）。
    const std::vector<float>* deform = nullptr;
    {
        const SpineGpuPathAttachment* base = ctx.asset.path_attachments.data();
        const size_t att_index = static_cast<size_t>(&att - base);
        if (att_index < ctx.path_deform.size() && !ctx.path_deform[att_index].empty())
        {
            deform = &ctx.path_deform[att_index];
        }
    }
    const std::vector<float>& verts = ctx.asset.path_vertices;
    const std::vector<uint16_t>& pool = ctx.asset.path_bone_pool;
    // ⚠️ `vertex_offset` 的单位是 **f32 下标**（不是字节），且是**池内**偏移：
    //    chunk 20 的顶点池是所有 path 附件**共用一个**数组（`SpineGpuImporter.cpp:5244`）。
    //    漏掉它 ⇒ 每条 path 都读到第 0 条曲线的控制点 ⇒ 所有 path 约束把骨骼摆到
    //    **同一个点**，而这在"只有一条 path"的资产上完全测不出来（偏移为 0）。
    const size_t vbase = static_cast<size_t>(att.vertex_offset);
    const size_t vend = vbase + static_cast<size_t>(att.vertex_float_count);

    if (att.bone_count == 0)
    {
        // rigid：整块由**槽位的骨骼**变换（参考实现用 `slot._bone`）
        if (slot_bone >= ctx.bones.size())
        {
            return;
        }
        const BoneState& bone = ctx.bones[slot_bone];
        const float x = bone.world_x;
        const float y = bone.world_y;
        const float a = bone.a;
        const float b = bone.b;
        const float c = bone.c;
        const float d = bone.d;
        // 有 deform 时**整体换成** deform 数组（它存的是绝对坐标，下标从 0 起，不加 vbase）
        const std::vector<float>& src = deform != nullptr ? *deform : verts;
        const size_t sbase = deform != nullptr ? 0u : vbase;
        const size_t send = deform != nullptr ? deform->size() : vend;
        for (uint32_t vv = start, w = offset; w < limit; vv += 2, w += 2)
        {
            const size_t vi = sbase + vv;
            if (vi + 1 >= send || vi + 1 >= src.size())
            {
                break;
            }
            const float vx = src[vi];
            const float vy = src[vi + 1];
            out[w] = vx * a + vy * b + x;
            out[w + 1] = vx * c + vy * d + y;
        }
        return;
    }

    // weighted：`_bones` 是 `[n, idx×n]` 的扁平序列（与 mesh 顶点同编码）
    uint32_t v = 0;
    uint32_t skip = 0;
    for (uint32_t i = 0; i < start; i += 2)
    {
        if (static_cast<size_t>(att.bone_offset) + v >= pool.size())
        {
            return;
        }
        const uint32_t n = pool[static_cast<size_t>(att.bone_offset) + v];
        v += n + 1u;
        skip += n;
    }
    const uint32_t bone_count = ctx.bones.size() != 0 ? static_cast<uint32_t>(ctx.bones.size()) : 0u;
    // deform 下标 `f` 从 `skip << 1` 起、每个影响 +2（VertexAttachment.cpp:110）——
    // 与顶点下标 `bb`（每影响 +3）不是同一个步长。
    for (uint32_t w = offset, bb = skip * 3u, f = skip << 1; w < limit; w += 2)
    {
        if (static_cast<size_t>(att.bone_offset) + v >= pool.size())
        {
            break;
        }
        uint32_t n = pool[static_cast<size_t>(att.bone_offset) + v];
        ++v;
        n += v;
        float wx = 0.0f;
        float wy = 0.0f;
        for (; v < n; ++v, bb += 3u, f += 2u)
        {
            const size_t vi = vbase + bb;
            if (static_cast<size_t>(att.bone_offset) + v >= pool.size() || vi + 2u >= vend ||
                vi + 2u >= verts.size())
            {
                break;
            }
            const uint32_t bone_index = pool[static_cast<size_t>(att.bone_offset) + v];
            if (bone_index >= bone_count)
            {
                continue;
            }
            const BoneState& bone = ctx.bones[bone_index];
            float vx = verts[vi];
            float vy = verts[vi + 1];
            if (deform != nullptr && f + 1u < deform->size())
            {
                vx += (*deform)[f];
                vy += (*deform)[f + 1u];
            }
            const float weight = verts[vi + 2];
            wx += (vx * bone.a + vy * bone.b + bone.world_x) * weight;
            wy += (vx * bone.c + vy * bone.d + bone.world_y) * weight;
        }
        out[w] = wx;
        out[w + 1] = wy;
    }
}

// addBeforePosition / addAfterPosition（PathConstraint.cpp:525-546）
void AddBeforePosition(float p, const std::vector<float>& temp, uint32_t i, std::vector<float>& out, uint32_t o)
{
    if (i + 3 >= temp.size() || o + 2 >= out.size())
    {
        return;
    }
    const float x1 = temp[i];
    const float y1 = temp[i + 1];
    const float dx = temp[i + 2] - x1;
    const float dy = temp[i + 3] - y1;
    const float r = Atan2(dy, dx);
    out[o] = x1 + p * std::cos(r);
    out[o + 1] = y1 + p * std::sin(r);
    out[o + 2] = r;
}

void AddAfterPosition(float p, const std::vector<float>& temp, uint32_t i, std::vector<float>& out, uint32_t o)
{
    if (i + 3 >= temp.size() || o + 2 >= out.size())
    {
        return;
    }
    const float x1 = temp[i + 2];
    const float y1 = temp[i + 3];
    const float dx = x1 - temp[i];
    const float dy = y1 - temp[i + 1];
    const float r = Atan2(dy, dx);
    out[o] = x1 + p * std::cos(r);
    out[o + 1] = y1 + p * std::sin(r);
    out[o + 2] = r;
}

// addCurvePosition（PathConstraint.cpp:548-570）
void AddCurvePosition(float p, float x1, float y1, float cx1, float cy1, float cx2, float cy2, float x2, float y2,
                      std::vector<float>& out, uint32_t o, bool tangents)
{
    if (o + 2 >= out.size())
    {
        return;
    }
    if (p < kPathEpsilon || IsNan(p))
    {
        out[o] = x1;
        out[o + 1] = y1;
        out[o + 2] = Atan2(cy1 - y1, cx1 - x1);
        return;
    }
    const float tt = p * p;
    const float ttt = tt * p;
    const float u = 1.0f - p;
    const float uu = u * u;
    const float uuu = uu * u;
    const float ut = u * p;
    const float ut3 = ut * 3.0f;
    const float uut3 = u * ut3;
    const float utt3 = ut3 * p;
    const float x = x1 * uuu + cx1 * uut3 + cx2 * utt3 + x2 * ttt;
    const float y = y1 * uuu + cy1 * uut3 + cy2 * utt3 + y2 * ttt;
    out[o] = x;
    out[o + 1] = y;
    if (tangents)
    {
        if (p < 0.001f)
        {
            out[o + 2] = Atan2(cy1 - y1, cx1 - x1);
        }
        else
        {
            out[o + 2] = Atan2(y - (y1 * uu + cy1 * ut * 2.0f + cy2 * tt),
                               x - (x1 * uu + cx1 * ut * 2.0f + cx2 * tt));
        }
    }
}

// `PathConstraint::computeWorldPositions()`（PathConstraint.cpp:267-523）。
//
// ⚠️ 这个函数里的 `curve * 6 + 2`、`verticesLength - 4`、`curveCount--` 之类的算术
//    **全部照抄**，不做"看懂了再改写"。它依赖 spine 对 path 顶点数组的内部约定
//    （`vertexCount` 个控制点、每 6 个 float 一段曲线、`lengths` 条数 = vertexCount/3），
//    任何"化简"都会得到一条不同的曲线，且不报错。
bool PathComputeWorldPositions(PoseContext& ctx, const SpineGpuPathAttachment& att, uint32_t slot_bone,
                               uint32_t constraint_index, uint32_t spaces_count, bool tangents, bool percent_position,
                               bool percent_spacing, std::string& err)
{
    const std::vector<float>& lengths_src = ctx.asset.path_lengths;
    // ⚠️ 和 `vertex_offset` 同理：lengths 也是**所有 path 附件共用一个池**
    //    （`SpineGpuImporter.cpp:5247`），`length_offset` 是 f32 下标。
    //    参考实现里 `path.getLengths()` 拿的是**这条附件自己的**数组
    //    （PathConstraint.cpp:283 / :412）⇒ 必须加上偏移。
    const size_t lbase = static_cast<size_t>(att.length_offset);
    const size_t lend = lbase + static_cast<size_t>(att.length_count);
    const bool closed = (att.flags & SpineGpuPathAttachmentFlags::kPathAttFlagClosed) != 0;
    const bool constant_speed = (att.flags & SpineGpuPathAttachmentFlags::kPathAttFlagConstantSpeed) != 0;

    ctx.positions.assign(static_cast<size_t>(spaces_count) * 3u + 2u, 0.0f);
    std::vector<float>& out = ctx.positions;

    // `getWorldVerticesLength()` = `vertexCount << 1`（SkeletonBinary.cpp:661）
    int vertices_length = static_cast<int>(att.vertex_count) * 2;
    int curve_count = vertices_length / 6;

    float position = ctx.path_position[constraint_index];

    if (!constant_speed)
    {
        int tmp_count = curve_count - (closed ? 1 : 2);
        if (tmp_count < 0 || lbase + static_cast<size_t>(tmp_count) >= lend ||
            lbase + static_cast<size_t>(tmp_count) >= lengths_src.size())
        {
            err = "path 约束的 lengths 条数不够（curveCount=" + std::to_string(tmp_count) +
                  "，该附件 length_count=" + std::to_string(att.length_count) + "）";
            return false;
        }
        float path_length = lengths_src[lbase + static_cast<size_t>(tmp_count)];
        if (percent_position)
        {
            position *= path_length;
        }
        if (percent_spacing)
        {
            for (uint32_t i = 1; i < spaces_count; ++i)
            {
                ctx.spaces[i] *= path_length;
            }
        }

        ctx.world.assign(8, 0.0f);
        int prev_curve = -1;
        for (uint32_t i = 0, o = 0, curve = 0; i < spaces_count; ++i, o += 3)
        {
            const float space = ctx.spaces[i];
            position += space;
            float p = position;

            if (closed)
            {
                p = Fmod(p, path_length);
                if (p < 0)
                {
                    p += path_length;
                }
                curve = 0;
            }
            else if (p < 0)
            {
                if (prev_curve != -2)
                {
                    prev_curve = -2;
                    ctx.world.assign(8, 0.0f);
                    PathComputeWorldVertices(ctx, att, slot_bone, 2, 4, ctx.world, 0);
                }
                AddBeforePosition(p, ctx.world, 0, out, o);
                continue;
            }
            else if (p > path_length)
            {
                if (prev_curve != -3)
                {
                    prev_curve = -3;
                    ctx.world.assign(8, 0.0f);
                    PathComputeWorldVertices(ctx, att, slot_bone,
                                             static_cast<uint32_t>(vertices_length) >= 6u ? vertices_length - 6 : 0, 4,
                                             ctx.world, 0);
                }
                AddAfterPosition(p - path_length, ctx.world, 0, out, o);
                continue;
            }

            // Determine curve containing position.
            for (;; ++curve)
            {
                if (lbase + static_cast<size_t>(curve) >= lend || lbase + static_cast<size_t>(curve) >= lengths_src.size())
                {
                    err = "path 约束定位曲线时下标越界";
                    return false;
                }
                const float length = lengths_src[lbase + static_cast<size_t>(curve)];
                if (p > length)
                {
                    continue;
                }
                if (curve == 0)
                {
                    p /= length;
                }
                else
                {
                    const float prev = lengths_src[lbase + static_cast<size_t>(curve) - 1];
                    p = (p - prev) / (length - prev);
                }
                break;
            }

            if (static_cast<int>(curve) != prev_curve)
            {
                prev_curve = static_cast<int>(curve);
                if (closed && static_cast<int>(curve) == tmp_count)
                {
                    ctx.world.assign(8, 0.0f);
                    PathComputeWorldVertices(ctx, att, slot_bone, static_cast<uint32_t>(vertices_length - 4), 4,
                                             ctx.world, 0);
                    PathComputeWorldVertices(ctx, att, slot_bone, 0, 4, ctx.world, 4);
                }
                else
                {
                    ctx.world.assign(8, 0.0f);
                    PathComputeWorldVertices(ctx, att, slot_bone, static_cast<uint32_t>(curve) * 6u + 2u, 8, ctx.world,
                                             0);
                }
            }

            AddCurvePosition(p, ctx.world[0], ctx.world[1], ctx.world[2], ctx.world[3], ctx.world[4], ctx.world[5],
                             ctx.world[6], ctx.world[7], out, o, tangents || (i > 0 && space < kPathEpsilon));
        }
        return true;
    }

    // ---- constantSpeed ----
    if (closed)
    {
        vertices_length += 2;
        ctx.world.assign(static_cast<size_t>(vertices_length), 0.0f);
        PathComputeWorldVertices(ctx, att, slot_bone, 2, static_cast<uint32_t>(vertices_length - 4), ctx.world, 0);
        PathComputeWorldVertices(ctx, att, slot_bone, 0, 2, ctx.world, static_cast<uint32_t>(vertices_length - 4));
        ctx.world[static_cast<size_t>(vertices_length) - 2] = ctx.world[0];
        ctx.world[static_cast<size_t>(vertices_length) - 1] = ctx.world[1];
    }
    else
    {
        --curve_count;
        vertices_length -= 4;
        if (vertices_length < 0)
        {
            err = "path 附件的顶点数太少";
            return false;
        }
        ctx.world.assign(static_cast<size_t>(vertices_length), 0.0f);
        PathComputeWorldVertices(ctx, att, slot_bone, 2, static_cast<uint32_t>(vertices_length), ctx.world, 0);
    }

    // Curve lengths.
    ctx.curves.assign(static_cast<size_t>(curve_count), 0.0f);
    float path_length = 0.0f;
    float x1 = ctx.world.empty() ? 0.0f : ctx.world[0];
    float y1 = ctx.world.size() > 1 ? ctx.world[1] : 0.0f;
    float cx1 = 0.0f;
    float cy1 = 0.0f;
    float cx2 = 0.0f;
    float cy2 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    for (int i = 0, w = 2; i < curve_count; ++i, w += 6)
    {
        if (static_cast<size_t>(w) + 5 >= ctx.world.size())
        {
            break;
        }
        cx1 = ctx.world[static_cast<size_t>(w)];
        cy1 = ctx.world[static_cast<size_t>(w) + 1];
        cx2 = ctx.world[static_cast<size_t>(w) + 2];
        cy2 = ctx.world[static_cast<size_t>(w) + 3];
        x2 = ctx.world[static_cast<size_t>(w) + 4];
        y2 = ctx.world[static_cast<size_t>(w) + 5];
        const float tmpx = (x1 - cx1 * 2.0f + cx2) * 0.1875f;
        const float tmpy = (y1 - cy1 * 2.0f + cy2) * 0.1875f;
        const float dddfx = ((cx1 - cx2) * 3.0f - x1 + x2) * 0.09375f;
        const float dddfy = ((cy1 - cy2) * 3.0f - y1 + y2) * 0.09375f;
        float ddfx = tmpx * 2.0f + dddfx;
        float ddfy = tmpy * 2.0f + dddfy;
        float dfx = (cx1 - x1) * 0.75f + tmpx + dddfx * 0.16666667f;
        float dfy = (cy1 - y1) * 0.75f + tmpy + dddfy * 0.16666667f;
        path_length += Sqrt(dfx * dfx + dfy * dfy);
        dfx += ddfx;
        dfy += ddfy;
        ddfx += dddfx;
        ddfy += dddfy;
        path_length += Sqrt(dfx * dfx + dfy * dfy);
        dfx += ddfx;
        dfy += ddfy;
        path_length += Sqrt(dfx * dfx + dfy * dfy);
        dfx += ddfx + dddfx;
        dfy += ddfy + dddfy;
        path_length += Sqrt(dfx * dfx + dfy * dfy);
        ctx.curves[static_cast<size_t>(i)] = path_length;
        x1 = x2;
        y1 = y2;
    }

    if (percent_position)
    {
        position *= path_length;
    }
    else
    {
        const int idx = curve_count - 1;
        const size_t abs_idx = lbase + static_cast<size_t>(idx);
        if (idx < 0 || abs_idx >= lend || abs_idx >= lengths_src.size() || lengths_src[abs_idx] == 0.0f)
        {
            err = "path 约束的 lengths 下标越界（curveCount-1=" + std::to_string(idx) + "）";
            return false;
        }
        position *= path_length / lengths_src[abs_idx];
    }

    if (percent_spacing)
    {
        for (uint32_t i = 1; i < spaces_count; ++i)
        {
            ctx.spaces[i] *= path_length;
        }
    }

    float curve_length = 0.0f;
    int prev_curve = -1;
    for (uint32_t i = 0, o = 0, curve = 0, segment = 0; i < spaces_count; ++i, o += 3)
    {
        const float space = ctx.spaces[i];
        position += space;
        float p = position;

        if (closed)
        {
            p = Fmod(p, path_length);
            if (p < 0)
            {
                p += path_length;
            }
            curve = 0;
        }
        else if (p < 0)
        {
            AddBeforePosition(p, ctx.world, 0, out, o);
            continue;
        }
        else if (p > path_length)
        {
            AddAfterPosition(p - path_length, ctx.world, static_cast<uint32_t>(vertices_length) >= 4u
                                                             ? static_cast<uint32_t>(vertices_length) - 4u
                                                             : 0u,
                             out, o);
            continue;
        }

        for (;; ++curve)
        {
            if (static_cast<size_t>(curve) >= ctx.curves.size())
            {
                err = "path 约束定位曲线时下标越界（constantSpeed）";
                return false;
            }
            const float length = ctx.curves[curve];
            if (p > length)
            {
                continue;
            }
            if (curve == 0)
            {
                p /= length;
            }
            else
            {
                const float prev = ctx.curves[curve - 1];
                p = (p - prev) / (length - prev);
            }
            break;
        }

        if (static_cast<int>(curve) != prev_curve)
        {
            prev_curve = static_cast<int>(curve);
            const uint32_t ii = curve * 6u;
            if (ii + 7 >= ctx.world.size())
            {
                err = "path 附件的世界顶点不够";
                return false;
            }
            x1 = ctx.world[ii];
            y1 = ctx.world[ii + 1];
            cx1 = ctx.world[ii + 2];
            cy1 = ctx.world[ii + 3];
            cx2 = ctx.world[ii + 4];
            cy2 = ctx.world[ii + 5];
            x2 = ctx.world[ii + 6];
            y2 = ctx.world[ii + 7];
            const float tmpx = (x1 - cx1 * 2.0f + cx2) * 0.03f;
            const float tmpy = (y1 - cy1 * 2.0f + cy2) * 0.03f;
            const float dddfx = ((cx1 - cx2) * 3.0f - x1 + x2) * 0.006f;
            const float dddfy = ((cy1 - cy2) * 3.0f - y1 + y2) * 0.006f;
            float ddfx = tmpx * 2.0f + dddfx;
            float ddfy = tmpy * 2.0f + dddfy;
            float dfx = (cx1 - x1) * 0.3f + tmpx + dddfx * 0.16666667f;
            float dfy = (cy1 - y1) * 0.3f + tmpy + dddfy * 0.16666667f;
            curve_length = Sqrt(dfx * dfx + dfy * dfy);
            ctx.segments[0] = curve_length;
            for (uint32_t j = 1; j < 8; ++j)
            {
                dfx += ddfx;
                dfy += ddfy;
                ddfx += dddfx;
                ddfy += dddfy;
                curve_length += Sqrt(dfx * dfx + dfy * dfy);
                ctx.segments[j] = curve_length;
            }
            dfx += ddfx;
            dfy += ddfy;
            curve_length += Sqrt(dfx * dfx + dfy * dfy);
            ctx.segments[8] = curve_length;
            dfx += ddfx + dddfx;
            dfy += ddfy + dddfy;
            curve_length += Sqrt(dfx * dfx + dfy * dfy);
            ctx.segments[9] = curve_length;
            segment = 0;
        }

        p *= curve_length;
        for (;; ++segment)
        {
            if (segment >= 10)
            {
                err = "path 约束定位段时下标越界";
                return false;
            }
            const float length = ctx.segments[segment];
            if (p > length)
            {
                continue;
            }
            if (segment == 0)
            {
                p /= length;
            }
            else
            {
                const float prev = ctx.segments[segment - 1];
                p = static_cast<float>(segment) + (p - prev) / (length - prev);
            }
            break;
        }
        AddCurvePosition(p * 0.1f, x1, y1, cx1, cy1, cx2, cy2, x2, y2, out, o,
                         tangents || (i > 0 && space < kPathEpsilon));
    }
    return true;
}

// `PathConstraint::update()`（PathConstraint.cpp:75-213）
bool PathApply(PoseContext& ctx, uint32_t ci, std::string& err)
{
    const SpineGpuPathConstraint& c = ctx.asset.paths[ci];
    const uint32_t att_index = ctx.path_att[ci];
    if (att_index == SpineGpuConstants::kNoPathAttachment ||
        att_index >= ctx.asset.path_attachments.size())
    {
        return true;   // `update()` 直接 return（PathConstraint.cpp:77-79）
    }
    const SpineGpuPathAttachment& att = ctx.asset.path_attachments[att_index];

    const float rotate_mix = ctx.path_rotate_mix[ci];
    const float translate_mix = ctx.path_translate_mix[ci];
    const bool translate = translate_mix > 0.0f;
    const bool rotate = rotate_mix > 0.0f;
    if (!translate && !rotate)
    {
        return true;
    }

    const bool percent_spacing = c.spacing_mode == static_cast<uint8_t>(2);   // SpacingMode_Percent
    const uint8_t rotate_mode = c.rotate_mode;
    const bool tangents = rotate_mode == 0;    // RotateMode_Tangent
    const bool scale = rotate_mode == 2;       // RotateMode_ChainScale
    const uint32_t bone_count = c.bone_count;
    const uint32_t spaces_count = tangents ? bone_count : bone_count + 1u;
    ctx.spaces.assign(static_cast<size_t>(spaces_count), 0.0f);
    const float spacing = ctx.path_spacing[ci];

    if (scale || !percent_spacing)
    {
        if (scale)
        {
            ctx.lengths.assign(static_cast<size_t>(bone_count), 0.0f);
        }
        const bool length_spacing = c.spacing_mode == 0;   // SpacingMode_Length
        for (uint32_t i = 0, n = spaces_count - 1u; i < n;)
        {
            const uint32_t bi = ctx.asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + i];
            if (bi >= ctx.bones.size())
            {
                err = "path 约束的骨骼下标越界";
                return false;
            }
            const BoneState& bone = ctx.bones[bi];
            const float setup_length = bone.length;
            if (setup_length < kPathEpsilon)
            {
                if (scale)
                {
                    ctx.lengths[i] = 0.0f;
                }
                ctx.spaces[++i] = 0.0f;
            }
            else if (percent_spacing)
            {
                if (scale)
                {
                    const float x = setup_length * bone.a;
                    const float y = setup_length * bone.c;
                    ctx.lengths[i] = Sqrt(x * x + y * y);
                }
                ctx.spaces[++i] = spacing;
            }
            else
            {
                const float x = setup_length * bone.a;
                const float y = setup_length * bone.c;
                const float length = Sqrt(x * x + y * y);
                if (scale)
                {
                    ctx.lengths[i] = length;
                }
                ctx.spaces[++i] = (length_spacing ? setup_length + spacing : spacing) * length / setup_length;
            }
        }
    }
    else
    {
        for (uint32_t i = 1; i < spaces_count; ++i)
        {
            ctx.spaces[i] = spacing;
        }
    }

    if (c.target_slot >= ctx.asset.slot_bone.size())
    {
        err = "path 约束的槽位下标越界";
        return false;
    }
    const uint32_t slot_bone = ctx.asset.slot_bone[static_cast<size_t>(c.target_slot)];

    if (!PathComputeWorldPositions(ctx, att, slot_bone, ci, spaces_count, tangents,
                                   c.position_mode == 1, percent_spacing, err))
    {
        return false;
    }
    const std::vector<float>& positions = ctx.positions;

    if (std::getenv("ZENGINE_SPINE_POSE_DEBUG") != nullptr)
    {
        std::printf("[path %u] att=%u vc=%u bc=%u pos=%g space=%g rot=%g/%g rm=%u sm=%u pm=%u slot=%u sb=%u(sbw=%g,%g)\n",
                    ci, att_index, att.vertex_count, bone_count, static_cast<double>(ctx.path_position[ci]),
                    static_cast<double>(spacing), static_cast<double>(rotate_mix),
                    static_cast<double>(translate_mix), rotate_mode, c.spacing_mode, c.position_mode, c.target_slot,
                    slot_bone,
                    slot_bone < ctx.bones.size() ? static_cast<double>(ctx.bones[slot_bone].world_x) : 0.0,
                    slot_bone < ctx.bones.size() ? static_cast<double>(ctx.bones[slot_bone].world_y) : 0.0);
        const size_t dbg_vbase = static_cast<size_t>(att.vertex_offset);
        const size_t dbg_vend = dbg_vbase + static_cast<size_t>(att.vertex_float_count);
        std::printf("         localverts(skin=%u bones=%u):", att.skin_index, att.bone_count);
        for (size_t q = dbg_vbase; q < dbg_vend && q < dbg_vbase + 8u && q < ctx.asset.path_vertices.size(); ++q)
        {
            std::printf(" %g", static_cast<double>(ctx.asset.path_vertices[q]));
        }
        std::printf("\n         slotbone a=%g b=%g c=%g d=%g\n",
                    slot_bone < ctx.bones.size() ? static_cast<double>(ctx.bones[slot_bone].a) : 0.0,
                    slot_bone < ctx.bones.size() ? static_cast<double>(ctx.bones[slot_bone].b) : 0.0,
                    slot_bone < ctx.bones.size() ? static_cast<double>(ctx.bones[slot_bone].c) : 0.0,
                    slot_bone < ctx.bones.size() ? static_cast<double>(ctx.bones[slot_bone].d) : 0.0);
        std::printf("         spaces:");
        for (float s : ctx.spaces)
        {
            std::printf(" %g", static_cast<double>(s));
        }
        std::printf("\n         positions:");
        for (float s : positions)
        {
            std::printf(" %g", static_cast<double>(s));
        }
        std::printf("\n         bones:");
        for (uint32_t i = 0; i < bone_count; ++i)
        {
            std::printf(" %u(w=%g,%g)", ctx.asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + i],
                        static_cast<double>(ctx.bones[ctx.asset.constraint_bone_pool[c.bone_first + i]].world_x),
                        static_cast<double>(ctx.bones[ctx.asset.constraint_bone_pool[c.bone_first + i]].world_y));
        }
        std::printf("\n");
    }

    float bone_x = positions[0];
    float bone_y = positions[1];
    float offset_rotation = c.offset_rotation;
    bool tip;
    if (offset_rotation == 0.0f)
    {
        tip = rotate_mode == 1;   // RotateMode_Chain
    }
    else
    {
        tip = false;
        if (slot_bone < ctx.bones.size())
        {
            const BoneState& p = ctx.bones[slot_bone];
            offset_rotation *= (p.a * p.d - p.b * p.c > 0) ? kDegRad : -kDegRad;
        }
    }

    for (uint32_t i = 0, p = 3; i < bone_count; ++i, p += 3)
    {
        const uint32_t bi = ctx.asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + i];
        if (bi >= ctx.bones.size())
        {
            err = "path 约束的骨骼下标越界";
            return false;
        }
        BoneState& bone = ctx.bones[bi];
        bone.world_x += (bone_x - bone.world_x) * translate_mix;
        bone.world_y += (bone_y - bone.world_y) * translate_mix;
        const float x = positions[p];
        const float y = positions[p + 1];
        const float dx = x - bone_x;
        const float dy = y - bone_y;
        if (scale)
        {
            const float length = ctx.lengths[i];
            if (length >= kPathEpsilon)
            {
                const float s = (Sqrt(dx * dx + dy * dy) / length - 1.0f) * rotate_mix + 1.0f;
                bone.a *= s;
                bone.c *= s;
            }
        }
        bone_x = x;
        bone_y = y;

        if (rotate)
        {
            const float a = bone.a;
            const float b = bone.b;
            const float cc = bone.c;
            const float dd = bone.d;
            float r;
            if (tangents)
            {
                r = positions[p - 1];
            }
            else if (ctx.spaces[i + 1] < kPathEpsilon)
            {
                r = positions[p + 2];
            }
            else
            {
                r = Atan2(dy, dx);
            }
            r -= Atan2(cc, a);
            if (tip)
            {
                const float cs = std::cos(r);
                const float sn = std::sin(r);
                const float length = bone.length;
                bone_x += (length * (cs * a - sn * cc) - dx) * rotate_mix;
                bone_y += (length * (sn * a + cs * cc) - dy) * rotate_mix;
            }
            else
            {
                r += offset_rotation;
            }
            if (r > kPi)
            {
                r -= kPi2;
            }
            else if (r < -kPi)
            {
                r += kPi2;
            }
            r *= rotate_mix;
            const float cs = std::cos(r);
            const float sn = std::sin(r);
            bone.a = cs * a - sn * cc;
            bone.b = cs * b - sn * dd;
            bone.c = sn * a + cs * cc;
            bone.d = sn * b + cs * dd;
        }

        bone.applied_valid = false;
    }
    return true;
}

}   // namespace

// ---------------------------------------------------------------------
// 对外入口
// ---------------------------------------------------------------------

namespace
{
    void TraceWorld(const PoseContext& ctx, SpineGpuPoseTrace* trace, uint8_t kind, uint32_t index)
    {
        if (trace == nullptr)
        {
            return;
        }
        trace->step_kind.push_back(kind);
        trace->step_index.push_back(index);
        std::vector<SpineGpuBoneSample> snap(ctx.bones.size());
        for (size_t i = 0; i < ctx.bones.size(); ++i)
        {
            const BoneState& b = ctx.bones[i];
            snap[i].a = b.a;
            snap[i].b = b.b;
            snap[i].c = b.c;
            snap[i].d = b.d;
            snap[i].world_x = b.world_x;
            snap[i].world_y = b.world_y;
        }
        trace->after_step.push_back(std::move(snap));
        std::vector<SpineGpuPoseTrace::Local> ap(ctx.bones.size());
        std::vector<uint8_t> av(ctx.bones.size());
        for (size_t i = 0; i < ctx.bones.size(); ++i)
        {
            const BoneState& b = ctx.bones[i];
            ap[i] = {b.ax, b.ay, b.arotation, b.ascale_x, b.ascale_y, b.ashear_x, b.ashear_y};
            av[i] = b.applied_valid ? 1u : 0u;
        }
        trace->applied_after_step.push_back(std::move(ap));
        trace->applied_valid_after_step.push_back(std::move(av));
    }

    // 一条 deform 时间轴在 `time` 处的整份数组（`DeformTimeline::apply`，DeformTimeline.cpp:62-...）。
    // path（5b 步）与 mesh（8 步，S6）共用 —— 两者的时间轴是同一种东西，只是"作用于谁"不同。
    //
    // 口径：参考对拍是 `setToSetupPose()` 之后 `apply(alpha=1, MixBlend_First)`。
    // `Slot::setToSetupPose()` 先把 `_attachment` 置空再挂回去（Slot.cpp:59-60）⇒
    // deform 数组**必然被清空** ⇒ apply 里 `verticesArray.size()==0` 把 blend 改成
    // MixBlend_Setup（DeformTimeline.cpp:82-84）。alpha==1 时各分支收敛成：
    //   time < 第 0 帧  → 清空（:91-93）               ⇒ `cleared = true`
    //   time >= 末帧    → 末帧整份（:139 memcpy）
    //   其余            → prev + (next - prev) * percent（:200-204）
    // "整份"是参考实现**读文件时**拼出来的（SkeletonBinary.cpp:986-1016）：
    // 稀疏区间外补 0，rigid 再逐个加上 setup 顶点（rigid 存绝对坐标、weighted 存偏移）。
    // `rigid_setup` 非空 ⇒ rigid（长度 = deform_len）。
    SpineImportResult SampleDeformTimeline(const SpineGpuAsset& asset, uint32_t tl_index, float time,
                                           uint32_t deform_len, const float* rigid_setup, bool& cleared,
                                           std::vector<float>& dst)
    {
        const SpineGpuTimeline& tl = asset.timelines[tl_index];
        cleared = false;
        if (tl.frame_count == 0 || static_cast<size_t>(tl.frame_first) + tl.frame_count > asset.frames.size())
        {
            return Fail("sample: deform 时间轴 " + std::to_string(tl_index) + " 的帧区间越界或为空");
        }
        const float* frames = asset.frames.data() + tl.frame_first;
        if (time < frames[0])
        {
            cleared = true;   // 清空 ⇒ 用 setup 顶点
            dst.clear();
            return Ok();
        }

        // 逐帧解出稀疏载荷 (start, count, values*)
        struct SparseFrame
        {
            uint32_t start;
            uint32_t count;
            const uint8_t* values;
        };
        std::vector<SparseFrame> sparse(tl.frame_count);
        size_t pos = tl.payload_offset;
        for (uint32_t fr = 0; fr < tl.frame_count; ++fr)
        {
            if (pos + 8u > asset.timeline_payload.size())
            {
                return Fail("sample: deform 时间轴 " + std::to_string(tl_index) + " 的载荷被截断");
            }
            SparseFrame& sf = sparse[fr];
            std::memcpy(&sf.start, asset.timeline_payload.data() + pos, 4);
            std::memcpy(&sf.count, asset.timeline_payload.data() + pos + 4u, 4);
            sf.values = asset.timeline_payload.data() + pos + 8u;
            pos += 8u + static_cast<size_t>(sf.count) * 4u;
            if (pos > asset.timeline_payload.size())
            {
                return Fail("sample: deform 时间轴 " + std::to_string(tl_index) + " 的载荷被截断");
            }
        }

        // 拼出"整份"帧（SkeletonBinary.cpp:999-1016）
        const auto expand = [&](uint32_t fr, std::vector<float>& full) -> bool {
            full.assign(deform_len, 0.0f);
            const SparseFrame& sf = sparse[fr];
            if (static_cast<uint64_t>(sf.start) + sf.count > deform_len)
            {
                return false;
            }
            for (uint32_t q = 0; q < sf.count; ++q)
            {
                std::memcpy(&full[sf.start + q], sf.values + static_cast<size_t>(q) * 4u, 4);
            }
            if (rigid_setup != nullptr)
            {
                for (uint32_t q = 0; q < deform_len; ++q)
                {
                    full[q] += rigid_setup[q];
                }
            }
            return true;
        };

        const uint32_t last = tl.frame_count - 1u;
        if (time >= frames[last])
        {
            if (!expand(last, dst))
            {
                return Fail("sample: deform 时间轴 " + std::to_string(tl_index) + " 的帧超出 deform 长度");
            }
            return Ok();
        }
        const uint32_t cur = SpineGpuBinarySearchFrames(frames, tl.frame_count, 1u, time);
        float percent = 0.0f;
        const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
        if (!cr.ok)
        {
            return cr;
        }
        std::vector<float> prev;
        std::vector<float> next;
        if (!expand(cur - 1u, prev) || !expand(cur, next))
        {
            return Fail("sample: deform 时间轴 " + std::to_string(tl_index) + " 的帧超出 deform 长度");
        }
        dst.resize(deform_len);
        for (uint32_t q = 0; q < deform_len; ++q)
        {
            dst[q] = prev[q] + (next[q] - prev[q]) * percent;
        }
        return Ok();
    }
}  // namespace

SpineImportResult SpineGpuSamplePose(const SpineGpuAsset& asset, uint32_t anim_index, float time,
                                     SpineGpuPoseResult& out, SpineGpuPoseTrace* trace)
{
    const uint32_t bone_count = asset.header.bone_count;
    if (asset.bones.size() != bone_count)
    {
        return Fail("sample: header.bone_count 是 " + std::to_string(bone_count) + "，但 chunk 10 有 " +
                    std::to_string(asset.bones.size()) + " 条（读端应该在读容器时就报这个）");
    }

    PoseContext ctx(asset);

    // ---- 1) 骨骼复位到 setup（`Bone::setToSetupPose()`）----
    ctx.bones.resize(bone_count);
    for (uint32_t i = 0; i < bone_count; ++i)
    {
        const SpineGpuBone& b = asset.bones[i];
        BoneState& s = ctx.bones[i];
        s.x = b.x;
        s.y = b.y;
        s.rotation = b.rotation;
        s.scale_x = b.scale_x;
        s.scale_y = b.scale_y;
        s.shear_x = b.shear_x;
        s.shear_y = b.shear_y;
        s.length = b.length;
        s.parent = b.parent;
        s.mode = b.transform_mode;
        s.applied_valid = false;
        if (s.parent >= 0 && static_cast<uint32_t>(s.parent) >= i)
        {
            return Fail("sample: 第 " + std::to_string(i) + " 根骨骼的 parent 是 " + std::to_string(s.parent) +
                        "，不是 < 自己的下标（正向遍历就不是拓扑序了）");
        }
    }

    // ---- 2) 槽位侧（`SpineGpuSampleSlots`，S3）----
    // ⚠️ 必须在骨骼**之前**：path 约束看的是槽位**当前**挂的附件
    //    （`PathConstraint.cpp:76` `_target->getAttachment()`），而"当前"会被
    //    attachment 时间轴改掉。先算骨骼的话 path 会用错曲线，且不报错。
    {
        const SpineImportResult r = SpineGpuSampleSlots(asset, anim_index, time, out);
        if (!r.ok)
        {
            return r;
        }
    }

    if (anim_index >= asset.anims.size())
    {
        return Fail("sample: 动画下标 " + std::to_string(anim_index) + " 越界");
    }
    const SpineGpuAnim& anim = asset.anims[anim_index];

    // ---- 3) 时间轴 ----
    //
    // ⚠️ 容器里的时间轴按 **(type, target) 升序稳定排序**（为了让 .skel 与 json
    //    两条解析路径产出同一个容器，见 §12 第 6 项），而参考实现用的是文件里的
    //    书写顺序。对骨骼侧**没有影响**：alpha==1 的 MixBlend_First 对每一类都是
    //    "写死局部值"（`bone._rotation = data._rotation + normalize(...)` 之类），
    //    不同类写的是不同字段 ⇒ 顺序无关。
    for (uint32_t ti = 0; ti < anim.timeline_count; ++ti)
    {
        const uint32_t tl_index = anim.timeline_first + ti;
        if (tl_index >= asset.timelines.size())
        {
            return Fail("sample: 动画 " + std::to_string(anim_index) + " 引用了越界的时间轴 " +
                        std::to_string(tl_index));
        }
        const SpineGpuTimeline& tl = asset.timelines[tl_index];
        const SpineTimelineType type = static_cast<SpineTimelineType>(tl.type);
        const uint32_t entries = SpineTimelineEntries(type);
        if (entries == 0 || tl.frame_count == 0)
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴类型/帧数不自洽");
        }
        const uint64_t frame_end =
            static_cast<uint64_t>(tl.frame_first) + static_cast<uint64_t>(tl.frame_count) * entries;
        if (frame_end > asset.frames.size())
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴的帧区间越界");
        }
        const float* frames = asset.frames.data() + tl.frame_first;
        const bool before_first = time < frames[0];

        auto locate = [&](uint32_t& cur) -> uint32_t {
            cur = SpineGpuBinarySearchFrames(frames, tl.frame_count, entries, time);
            return cur;
        };

        if (type == SpineTimelineType::kRotate || type == SpineTimelineType::kTranslate ||
            type == SpineTimelineType::kScale || type == SpineTimelineType::kShear)
        {
            if (tl.target >= bone_count)
            {
                return Fail("sample: 第 " + std::to_string(tl_index) + " 条骨骼时间轴的骨骼下标 " +
                            std::to_string(tl.target) + " 越界（只有 " + std::to_string(bone_count) + " 根骨骼）");
            }
            if (before_first)
            {
                continue;   // Mixture: alpha==1 下 MixBlend_First 在这里是恒等变形
            }
            BoneState& bone = ctx.bones[tl.target];
            const SpineGpuBone& setup = asset.bones[tl.target];
            const uint32_t last = tl.frame_count - 1u;
            const float last_time = frames[static_cast<size_t>(last) * entries];

            if (type == SpineTimelineType::kRotate)
            {
                float r;
                if (tl.frame_count == 1u || time >= last_time)
                {
                    r = frames[static_cast<size_t>(last) * entries + 1u];
                }
                else
                {
                    uint32_t cur = 0;
                    locate(cur);
                    float percent = 0.0f;
                    const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
                    if (!cr.ok)
                    {
                        return cr;
                    }
                    const float prev_rotation = frames[static_cast<size_t>(cur) - entries + 1u];
                    float delta = frames[cur + 1u] - prev_rotation;
                    // ⚠️ 归一化作用在**增量**上（最短路径），再作用一次在**绝对值**上
                    //    （RotateTimeline.cpp:93-104）。
                    delta = WrapDegrees(delta);
                    r = prev_rotation + delta * percent;
                }
                r = WrapDegrees(r);
                bone.rotation = setup.rotation + r;
                continue;
            }

            float xv;
            float yv;
            if (tl.frame_count == 1u || time >= last_time)
            {
                const size_t base = static_cast<size_t>(last) * entries;
                xv = frames[base + 1u];
                yv = frames[base + 2u];
            }
            else
            {
                uint32_t cur = 0;
                locate(cur);
                float percent = 0.0f;
                const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
                if (!cr.ok)
                {
                    return cr;
                }
                const size_t prev = static_cast<size_t>(cur) - entries;
                xv = frames[prev + 1u] + (frames[cur + 1u] - frames[prev + 1u]) * percent;
                yv = frames[prev + 2u] + (frames[cur + 2u] - frames[prev + 2u]) * percent;
            }

            if (type == SpineTimelineType::kTranslate)
            {
                // `bone._x = data._x + x`（TranslateTimeline.cpp:104-105）
                bone.x = setup.x + xv;
                bone.y = setup.y + yv;
            }
            else if (type == SpineTimelineType::kScale)
            {
                // ⚠️ 帧里存的是**倍率**，要乘 setup 的 scale（ScaleTimeline.cpp:96,109）
                bone.scale_x = xv * setup.scale_x;
                bone.scale_y = yv * setup.scale_y;
            }
            else
            {
                // `bone._shearX = data._shearX + x`（ShearTimeline.cpp:99-100）
                bone.shear_x = setup.shear_x + xv;
                bone.shear_y = setup.shear_y + yv;
            }
            continue;
        }

        if (type == SpineTimelineType::kPathConstraintPosition || type == SpineTimelineType::kPathConstraintMix)
        {
            if (ctx.path_position.empty())
            {
                // 上面还没初始化（第一次遇到 path 时间轴时初始化）
                ctx.path_position.assign(asset.paths.size(), 0.0f);
                ctx.path_spacing.assign(asset.paths.size(), 0.0f);
                ctx.path_rotate_mix.assign(asset.paths.size(), 0.0f);
                ctx.path_translate_mix.assign(asset.paths.size(), 0.0f);
                for (size_t i = 0; i < asset.paths.size(); ++i)
                {
                    ctx.path_position[i] = asset.paths[i].position;
                    ctx.path_spacing[i] = asset.paths[i].spacing;
                    ctx.path_rotate_mix[i] = asset.paths[i].rotate_mix;
                    ctx.path_translate_mix[i] = asset.paths[i].translate_mix;
                }
            }
            if (tl.target >= asset.paths.size())
            {
                return Fail("sample: 第 " + std::to_string(tl_index) + " 条 path 约束时间轴的下标 " +
                            std::to_string(tl.target) + " 越界（只有 " + std::to_string(asset.paths.size()) +
                            " 条 path 约束）");
            }
            if (before_first)
            {
                continue;
            }
            if (type == SpineTimelineType::kPathConstraintPosition)
            {
                float position;
                if (tl.frame_count == 1u || time >= frames[static_cast<size_t>(tl.frame_count - 1u) * entries])
                {
                    position = frames[static_cast<size_t>(tl.frame_count - 1u) * entries + 1u];
                }
                else
                {
                    uint32_t cur = 0;
                    locate(cur);
                    float percent = 0.0f;
                    const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
                    if (!cr.ok)
                    {
                        return cr;
                    }
                    const size_t prev = static_cast<size_t>(cur) - entries;
                    position = frames[prev + 1u] + (frames[cur + 1u] - frames[prev + 1u]) * percent;
                }
                ctx.path_position[tl.target] = position;
            }
            else
            {
                float rot;
                float tr;
                if (tl.frame_count == 1u || time >= frames[static_cast<size_t>(tl.frame_count - 1u) * entries])
                {
                    const size_t base = static_cast<size_t>(tl.frame_count - 1u) * entries;
                    rot = frames[base + 1u];
                    tr = frames[base + 2u];
                }
                else
                {
                    uint32_t cur = 0;
                    locate(cur);
                    float percent = 0.0f;
                    const SpineImportResult cr = SpineGpuCurvePercentAt(asset, tl, tl_index, cur, time, percent);
                    if (!cr.ok)
                    {
                        return cr;
                    }
                    const size_t prev = static_cast<size_t>(cur) - entries;
                    rot = frames[prev + 1u] + (frames[cur + 1u] - frames[prev + 1u]) * percent;
                    tr = frames[prev + 2u] + (frames[cur + 2u] - frames[prev + 2u]) * percent;
                }
                ctx.path_rotate_mix[tl.target] = rot;
                ctx.path_translate_mix[tl.target] = tr;
            }
            continue;
        }

        if (type == SpineTimelineType::kDeform || type == SpineTimelineType::kEvent)
        {
            // mesh 的 deform 是 S6（共享 VB 不动、只给命中项写增量），不影响骨骼；
            // **path 的 deform 会影响骨骼**，在第 5b 步单独算（需要先知道槽位当前附件）。
            // event 不改渲染状态。
            continue;
        }

        if (type == SpineTimelineType::kIkConstraint || type == SpineTimelineType::kTransformConstraint ||
            type == SpineTimelineType::kPathConstraintSpacing)
        {
            // 这三类**会改姿势**，本阶段不做 ⇒ 显式报错，不能静默跳过
            // （实测 0 / 782 个资产用到，所以不会真的挡住任何资产）。
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条时间轴类型 " + std::to_string(tl.type) +
                        " 本阶段不做（会改姿势，跳过会得到静默错的姿势）");
        }

        // 其余（attachment / color / twoColor / drawOrder）是槽位侧，已在第 2 步处理。
    }

    // ---- 4) path 约束的运行时参数默认值（没有时间轴时 = setup）----
    if (ctx.path_position.empty())
    {
        ctx.path_position.assign(asset.paths.size(), 0.0f);
        ctx.path_spacing.assign(asset.paths.size(), 0.0f);
        ctx.path_rotate_mix.assign(asset.paths.size(), 0.0f);
        ctx.path_translate_mix.assign(asset.paths.size(), 0.0f);
        for (size_t i = 0; i < asset.paths.size(); ++i)
        {
            ctx.path_position[i] = asset.paths[i].position;
            ctx.path_spacing[i] = asset.paths[i].spacing;
            ctx.path_rotate_mix[i] = asset.paths[i].rotate_mix;
            ctx.path_translate_mix[i] = asset.paths[i].translate_mix;
        }
    }

    // ---- 5) 解析每条 path 约束**当前**用的那条附件 ----
    //
    // `PathConstraint::update()` 取 `_target->getAttachment()` —— 槽位当前挂的附件。
    // 判据是**名字**（不是"这个槽位的第一条 path"）：一个槽位挂两条 path 是合法的
    // （实测 6 / 782），取第一条会去摆一条别人的曲线，不报错。
    const std::vector<uint8_t> blob = BuildSpineStringBlob(asset);
    // 槽位 → 它此刻挂着的 chunk 20 记录（没有 / 不是 path ⇒ kNoPathAttachment）。
    // 约束（第 5 步）与 deform（第 5b 步）**必须用同一个答案**：deform 只在
    // "槽位当前附件 == 时间轴的附件"时生效（`DeformTimeline::apply` 的 `applyDeform`，
    // DeformTimeline.cpp:77），两处各查一遍就可能一边认这条、一边认那条。
    std::string slot_err;
    const auto current_path_of_slot = [&](uint32_t slot) -> uint32_t {
        if (slot >= out.slots.size())
        {
            slot_err = "槽位下标 " + std::to_string(slot) + " 越界";
            return SpineGpuConstants::kNoPathAttachment;
        }
        const uint32_t name_off = out.slots[slot].attachment_name_offset;
        if (name_off == SpineGpuConstants::kNoAttachmentName)
        {
            return SpineGpuConstants::kNoPathAttachment;   // 这一帧不挂附件
        }
        const char* name = SpineGpuStringAt(blob, name_off);
        if (name == nullptr)
        {
            slot_err = "槽位 " + std::to_string(slot) + " 的附件名偏移越界";
            return SpineGpuConstants::kNoPathAttachment;
        }
        for (size_t k = 0; k < asset.path_attachments.size(); ++k)
        {
            if (asset.path_attachments[k].slot_index == slot && asset.path_attachment_names[k] == name)
            {
                return static_cast<uint32_t>(k);
            }
        }
        return SpineGpuConstants::kNoPathAttachment;
    };
    {
        ctx.path_att.assign(asset.paths.size(), SpineGpuConstants::kNoPathAttachment);
        for (size_t i = 0; i < asset.paths.size(); ++i)
        {
            const SpineGpuPathConstraint& c = asset.paths[i];
            ctx.path_att[i] = current_path_of_slot(c.target_slot);
            if (!slot_err.empty())
            {
                return Fail("sample: path 约束 " + std::to_string(i) + ": " + slot_err);
            }
        }
    }

    // ---- 5b) path 附件的 deform（`DeformTimeline::apply`，DeformTimeline.cpp:62-...）----
    //
    // 只算 path 的：mesh 的 deform 只改顶点、由 GPU 那一路（S6）做，不影响骨骼。
    // path 的 deform 改的是 path 约束的**输入曲线** ⇒ 骨骼位置跟着变，必须在 CPU 上做。
    //
    // 口径：参考对拍是 `setToSetupPose()` 之后 `apply(alpha=1, MixBlend_First)`。
    // `Slot::setToSetupPose()` 先把 `_attachment` 置空再挂回去（Slot.cpp:59-60）⇒
    // deform 数组**必然被清空** ⇒ apply 里 `verticesArray.size()==0` 把 blend 改成
    // MixBlend_Setup（DeformTimeline.cpp:82-84）。alpha==1 时各分支收敛成：
    //   time < 第 0 帧  → 清空（:91-93）
    //   time >= 末帧    → 末帧整份（:139 memcpy）
    //   其余            → prev + (next - prev) * percent（:200-204）
    // "整份"是参考实现**读文件时**拼出来的（SkeletonBinary.cpp:986-1016）：
    // 稀疏区间外补 0，rigid 再逐个加上 setup 顶点（rigid 存绝对坐标、weighted 存偏移）。
    ctx.path_deform.assign(asset.path_attachments.size(), std::vector<float>());
    for (uint32_t ti = 0; ti < anim.timeline_count; ++ti)
    {
        const uint32_t tl_index = anim.timeline_first + ti;
        const SpineGpuTimeline& tl = asset.timelines[tl_index];
        if (static_cast<SpineTimelineType>(tl.type) != SpineTimelineType::kDeform ||
            (tl.target & SpineGpuConstants::kDeformTargetPathBit) == 0)
        {
            continue;
        }
        const uint32_t k = tl.target & SpineGpuConstants::kDeformTargetIndexMask;
        if (k >= asset.path_attachments.size())
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条 deform 时间轴指向越界的 path 附件 " +
                        std::to_string(k));
        }
        const SpineGpuPathAttachment& att = asset.path_attachments[k];
        // `applyDeform`：槽位此刻挂的不是这条 path ⇒ 整条时间轴不生效（:77-79）
        if (current_path_of_slot(att.slot_index) != k)
        {
            if (!slot_err.empty())
            {
                return Fail("sample: deform 时间轴 " + std::to_string(tl_index) + ": " + slot_err);
            }
            continue;
        }
        const uint32_t deform_len = SpineGpuPathDeformLength(att);
        const bool rigid = att.bone_count == 0;
        if (rigid && static_cast<size_t>(att.vertex_offset) + deform_len > asset.path_vertices.size())
        {
            return Fail("sample: path 附件 " + std::to_string(k) + " 的顶点区间越界");
        }
        bool cleared = false;
        const SpineImportResult dr =
            SampleDeformTimeline(asset, tl_index, time, deform_len,
                                 rigid ? asset.path_vertices.data() + att.vertex_offset : nullptr, cleared,
                                 ctx.path_deform[k]);
        if (!dr.ok)
        {
            return dr;
        }
    }

    if (trace != nullptr)
    {
        trace->local.resize(bone_count);
        for (uint32_t i = 0; i < bone_count; ++i)
        {
            const BoneState& b = ctx.bones[i];
            trace->local[i] = {b.x, b.y, b.rotation, b.scale_x, b.scale_y, b.shear_x, b.shear_y};
        }
    }

    // ---- 6) 复位表（`Skeleton::updateWorldTransform()` 的第一段，:200-211）----
    // ⚠️ 少复位会让**上一帧**被约束改过的 applied 值残留到这一帧，表现是**动作拖影**。
    for (uint16_t reset_bone : asset.update_reset)
    {
        if (reset_bone >= bone_count)
        {
            return Fail("sample: 复位表里有越界的骨骼下标 " + std::to_string(reset_bone));
        }
        ResetAppliedToLocal(ctx, reset_bone);
    }

    // ---- 7) 按 chunk 19 遍历 ----
    //
    // ⚠️ chunk 19 **只在有约束时落盘**（无约束时结果可证恒等于 0..bone_count-1）。
    //    读端按同一规则重建，所以没有 chunk 19 时这里退化成"正向遍历全部骨骼"
    //    —— 因为 `parent < index` ⇒ 正向遍历就是拓扑序。
    if (asset.update_steps.empty())
    {
        if (!asset.iks.empty() || !asset.transforms.empty() || !asset.paths.empty())
        {
            return Fail("sample: 有约束却没有 chunk 19（执行顺序）—— 容器坏了，读端应该已经报过");
        }
        for (uint32_t i = 0; i < bone_count; ++i)
        {
            BoneUpdate(ctx, i);
            TraceWorld(ctx, trace, SpineGpuUpdateKind::kUpdateBone, i);
        }
    }
    else
    {
        for (const SpineGpuUpdateStep& step : asset.update_steps)
        {
            switch (step.kind)
            {
                case SpineGpuUpdateKind::kUpdateBone:
                {
                    if (step.index >= bone_count)
                    {
                        return Fail("sample: 执行步骤里的骨骼下标 " + std::to_string(step.index) + " 越界");
                    }
                    BoneUpdate(ctx, step.index);
                    break;
                }
                case SpineGpuUpdateKind::kUpdateIk:
                {
                    if (step.index >= asset.iks.size())
                    {
                        return Fail("sample: 执行步骤里的 IK 下标 " + std::to_string(step.index) + " 越界");
                    }
                    const SpineGpuIkConstraint& c = asset.iks[step.index];
                    const bool compress = (c.flags & SpineGpuIkFlags::kIkFlagCompress) != 0;
                    const bool stretch = (c.flags & SpineGpuIkFlags::kIkFlagStretch) != 0;
                    const bool uniform = (c.flags & SpineGpuIkFlags::kIkFlagUniform) != 0;
                    if (c.bone_count == 0)
                    {
                        break;   // 参考实现的 `switch (_bones.size())` 没有 case 0
                    }
                    if (c.target >= bone_count)
                    {
                        return Fail("sample: IK 约束 " + std::to_string(step.index) + " 的目标骨骼越界");
                    }
                    const BoneState& target = ctx.bones[c.target];
                    if (c.bone_count == 1)
                    {
                        const uint32_t bi =
                            asset.constraint_bone_pool[static_cast<size_t>(c.bone_first)];
                        if (bi >= bone_count || ctx.bones[bi].parent < 0)
                        {
                            return Fail("sample: IK 约束 " + std::to_string(step.index) +
                                        " 的骨骼没有父骨骼（参考实现在这里是空指针解引用）");
                        }
                        IkApplyOneBone(ctx, bi, target.world_x, target.world_y, compress, stretch, uniform, c.mix);
                    }
                    else
                    {
                        const uint32_t parent_i =
                            asset.constraint_bone_pool[static_cast<size_t>(c.bone_first)];
                        const uint32_t child_i =
                            asset.constraint_bone_pool[static_cast<size_t>(c.bone_first) + c.bone_count - 1u];
                        if (parent_i >= bone_count || child_i >= bone_count || ctx.bones[parent_i].parent < 0)
                        {
                            return Fail("sample: IK 约束 " + std::to_string(step.index) +
                                        " 的父骨骼没有父骨骼（参考实现在这里是空指针解引用）");
                        }
                        IkApplyTwoBones(ctx, parent_i, child_i, target.world_x, target.world_y, c.bend_dir, stretch,
                                        c.mix);
                    }
                    break;
                }
                case SpineGpuUpdateKind::kUpdateTransform:
                {
                    if (step.index >= asset.transforms.size())
                    {
                        return Fail("sample: 执行步骤里的 transform 下标 " + std::to_string(step.index) + " 越界");
                    }
                    const SpineGpuTransformConstraint& c = asset.transforms[step.index];
                    if (c.target >= bone_count)
                    {
                        return Fail("sample: transform 约束 " + std::to_string(step.index) + " 的目标骨骼越界");
                    }
                    const bool local = (c.flags & SpineGpuTransformFlags::kTfFlagLocal) != 0;
                    const bool relative = (c.flags & SpineGpuTransformFlags::kTfFlagRelative) != 0;
                    if (local)
                    {
                        if (relative)
                        {
                            TransformApplyRelativeLocal(ctx, c);
                        }
                        else
                        {
                            TransformApplyAbsoluteLocal(ctx, c);
                        }
                    }
                    else
                    {
                        if (relative)
                        {
                            TransformApplyRelativeWorld(ctx, c);
                        }
                        else
                        {
                            TransformApplyAbsoluteWorld(ctx, c, step.index);
                        }
                    }
                    break;
                }
                case SpineGpuUpdateKind::kUpdatePath:
                {
                    if (step.index >= asset.paths.size())
                    {
                        return Fail("sample: 执行步骤里的 path 下标 " + std::to_string(step.index) + " 越界");
                    }
                    std::string err;
                    if (!PathApply(ctx, step.index, err))
                    {
                        return Fail("sample: path 约束 " + std::to_string(step.index) + ": " + err);
                    }
                    break;
                }
                default:
                    return Fail("sample: 执行步骤的 kind " + std::to_string(step.kind) + " 不认识");
            }
            TraceWorld(ctx, trace, step.kind, step.index);
        }
    }

    out.bones.resize(bone_count);
    for (uint32_t i = 0; i < bone_count; ++i)
    {
        const BoneState& s = ctx.bones[i];
        out.bones[i].a = s.a;
        out.bones[i].b = s.b;
        out.bones[i].c = s.c;
        out.bones[i].d = s.d;
        out.bones[i].world_x = s.world_x;
        out.bones[i].world_y = s.world_y;
    }

    // ---- 8) 槽位当前附件 → 绘制项；mesh deform（S6）----
    //
    // 骨骼与此无关（mesh 的 deform 只改顶点），所以放在最后。
    //
    // 当前附件按**名字**在 default 皮肤（skin 0）里找：`Skeleton::getAttachment()` 先查当前皮肤、
    // 再查 default 皮肤；我们不支持换肤 ⇒ 只剩 default。名字找不到可画项 = 挂的是裁剪 / point /
    // bbox / path（它们不产生绘制项），不是错。
    const uint32_t slot_count = static_cast<uint32_t>(out.slots.size());
    out.slot_draw_item.assign(slot_count, SpineGpuPoseResult::kNoDrawItem);
    out.mesh_deforms.clear();
    {
        std::vector<std::vector<uint32_t>> skin0_items(slot_count);
        for (uint32_t i = 0; i < asset.draw_items.size(); ++i)
        {
            const SpineDrawItem& di = asset.draw_items[i];
            if (di.skin_id == SpineGpuConstants::kDefaultSkinId && di.slot_index < slot_count)
            {
                skin0_items[di.slot_index].push_back(i);
            }
        }
        for (uint32_t s = 0; s < slot_count; ++s)
        {
            const uint32_t name_off = out.slots[s].attachment_name_offset;
            if (name_off == SpineGpuConstants::kNoAttachmentName || skin0_items[s].empty())
            {
                continue;
            }
            const char* name = SpineGpuStringAt(blob, name_off);
            if (name == nullptr)
            {
                return Fail("sample: 槽位 " + std::to_string(s) + " 的附件名偏移越界");
            }
            for (uint32_t i : skin0_items[s])
            {
                const uint16_t att = asset.draw_items[i].attachment_id;
                if (att < asset.attachment_names.size() && asset.attachment_names[att] == name)
                {
                    out.slot_draw_item[s] = i;
                    break;
                }
            }
        }
    }

    // mesh 的 deform 时间轴：作用于"该槽位此刻挂着的附件"，判据照抄
    // `MeshAttachment::applyDeform()`（MeshAttachment.cpp:94-95）：
    //   this == source || (inheritDeform && parentMesh == source)
    // 同一槽位被多条时间轴命中时按时间轴顺序**后者覆盖前者**（alpha = 1 的 replace；
    // 落在第 0 帧之前的那条则把数组清空，DeformTimeline.cpp:91-93）。
    // 顺序问题：参考实现里 attachment 时间轴排在 deform 之前（SkeletonBinary 先读槽位时间轴），
    // 换附件会清空 deform 数组（Slot::setAttachment）⇒ "先定当前附件、再套 deform"与之等价。
    std::vector<float> setup;   // rigid 的 setup 顶点（从静态 VB 取出来，参考实现的 `getVertices()`）
    for (uint32_t ti = 0; ti < anim.timeline_count; ++ti)
    {
        const uint32_t tl_index = anim.timeline_first + ti;
        const SpineGpuTimeline& tl = asset.timelines[tl_index];
        if (static_cast<SpineTimelineType>(tl.type) != SpineTimelineType::kDeform ||
            (tl.target & SpineGpuConstants::kDeformTargetPathBit) != 0)
        {
            continue;
        }
        if (tl.target >= asset.attachment_records.size())
        {
            return Fail("sample: 第 " + std::to_string(tl_index) + " 条 deform 时间轴指向越界的附件记录 " +
                        std::to_string(tl.target));
        }
        const SpineGpuAttachmentRecord& rec = asset.attachment_records[tl.target];
        if (rec.slot_index >= slot_count)
        {
            return Fail("sample: deform 时间轴 " + std::to_string(tl_index) + " 的槽位越界");
        }
        const uint32_t item = out.slot_draw_item[rec.slot_index];
        if (item == SpineGpuPoseResult::kNoDrawItem)
        {
            continue;
        }
        const SpineDrawItem& di = asset.draw_items[item];
        if (di.attachment_id != rec.attachment_id && di.deform_parent != rec.attachment_id)
        {
            continue;   // `applyDeform` 为假：槽位挂的不是它（也不是继承它的 linked mesh）
        }

        // deform 长度（DeformTimeline::_deformLength）：rigid 每顶点 2 个，weighted 每影响 2 个
        uint32_t deform_len = 0;
        const float* rigid_setup = nullptr;
        if (di.vertex_format == SpineVertexFormat::kRigid)
        {
            deform_len = 2u * di.vertex_count;
            const size_t byte0 = static_cast<size_t>(di.vertex_base) * SpineGpuConstants::kRigidVertexStride;
            if (byte0 + static_cast<size_t>(di.vertex_count) * SpineGpuConstants::kRigidVertexStride >
                asset.weighted_segment_offset)
            {
                return Fail("sample: 绘制项 " + std::to_string(item) + " 的 rigid 顶点区间越过 rigid 段");
            }
            setup.resize(deform_len);
            std::memcpy(setup.data(), asset.vertex_buffer.data() + byte0, static_cast<size_t>(deform_len) * 4u);
            rigid_setup = setup.data();
        }
        else
        {
            const size_t w0 = asset.weighted_segment_offset / 4u + static_cast<size_t>(di.vertex_base) * 2u;
            if ((w0 + static_cast<size_t>(di.vertex_count) * 2u) * 4u > asset.vertex_buffer.size())
            {
                return Fail("sample: 绘制项 " + std::to_string(item) + " 的 weighted 顶点区间越过顶点缓冲");
            }
            for (uint32_t v = 0; v < di.vertex_count; ++v)
            {
                uint32_t cnt = 0;
                std::memcpy(&cnt, asset.vertex_buffer.data() + (w0 + v * 2u + 1u) * 4u, 4);
                deform_len += 2u * cnt;
            }
        }

        // 找 / 建这一项的条目（同一项可能被多条时间轴命中：后者覆盖）
        SpineGpuMeshDeform* entry = nullptr;
        for (SpineGpuMeshDeform& e : out.mesh_deforms)
        {
            if (e.draw_item == item)
            {
                entry = &e;
                break;
            }
        }
        if (entry == nullptr)
        {
            out.mesh_deforms.push_back(SpineGpuMeshDeform {});
            entry = &out.mesh_deforms.back();
            entry->draw_item = item;
        }
        bool cleared = false;
        const SpineImportResult dr =
            SampleDeformTimeline(asset, tl_index, time, deform_len, rigid_setup, cleared, entry->values);
        if (!dr.ok)
        {
            return dr;
        }
    }
    // 被清空的（第 0 帧之前）= 没有 deform ⇒ 不出现在结果里
    out.mesh_deforms.erase(std::remove_if(out.mesh_deforms.begin(), out.mesh_deforms.end(),
                                          [](const SpineGpuMeshDeform& e) { return e.values.empty(); }),
                           out.mesh_deforms.end());
    std::sort(out.mesh_deforms.begin(), out.mesh_deforms.end(),
              [](const SpineGpuMeshDeform& a, const SpineGpuMeshDeform& b) { return a.draw_item < b.draw_item; });

    SpineImportResult r;
    r.ok = true;
    return r;
}
