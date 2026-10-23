#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rapidjson/document.h>

// =====================================================================
// 本文件里所有"照抄 spine 行为"的地方都带参考实现的出处：
//   G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp
// 不要凭印象改这里的公式。
// =====================================================================

namespace
{

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

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

// ---------------------------------------------------------------------
// .atlas 解析
// ---------------------------------------------------------------------
// 格式（参考实现 Atlas::load）：
//   页名 / "size: w,h" / "format: .." / "filter: ..,.." / "repeat: .."
//   空行 ⇒ 结束当前页
//   region 名 / "rotate: true|false" / "xy: x,y" / "size: w,h"
//   (可选) "orig: w,h" / "offset: x,y" / "index: n"
std::vector<std::string> SplitLines(const char* text, size_t len)
{
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < len; ++i)
    {
        const char c = text[i];
        if (c == '\n')
        {
            if (!cur.empty() && cur.back() == '\r')
            {
                cur.pop_back();
            }
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty())
    {
        lines.push_back(cur);
    }
    return lines;
}

void Trim(std::string& s)
{
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    size_t b = 0;
    while (b < s.size() && is_space(s[b]))
    {
        ++b;
    }
    size_t e = s.size();
    while (e > b && is_space(s[e - 1]))
    {
        --e;
    }
    s = s.substr(b, e - b);
}

int ColonPos(const std::string& s)
{
    return static_cast<int>(s.find(':'));
}

std::string KeyOf(const std::string& s)
{
    const int p = ColonPos(s);
    std::string k = (p < 0) ? s : s.substr(0, static_cast<size_t>(p));
    Trim(k);
    return k;
}

std::string ValueOf(const std::string& s)
{
    const int p = ColonPos(s);
    if (p < 0)
    {
        return std::string();
    }
    std::string v = s.substr(static_cast<size_t>(p) + 1);
    Trim(v);
    return v;
}

// "size: 100, 50" → {100, 50}
bool ParseTuple(const std::string& line, int* out, int max_count, int& count)
{
    const std::string v = ValueOf(line);
    count = 0;
    size_t i = 0;
    while (i < v.size() && count < max_count)
    {
        size_t comma = v.find(',', i);
        if (comma == std::string::npos)
        {
            comma = v.size();
        }
        std::string piece = v.substr(i, comma - i);
        Trim(piece);
        out[count++] = std::atoi(piece.c_str());
        i = comma + 1;
    }
    return count > 0;
}

// 骨骼名 → 下标（`skeletonData->findBone(name)` 的等价物）。
//
// ⚠️ 约束的 bones[] / target 在**两条**输入路径上的失败方式不一样：
//    json      → 参考实现显式报错（SkeletonJson.cpp:272-275 / :318-321 / :368-371）
//    二进制    → `skeletonData->_bones[readVarint()]` 直接下标
//                （SkeletonBinary.cpp:200、:223、:247），越界是 UB、不报错。
//    ⇒ 二进制侧也要显式校验，不能"反正参考实现那样写"。
int FindBoneIndex(const std::vector<SpineSourceBone>& bones, const std::string& name)
{
    for (size_t i = 0; i < bones.size(); ++i)
    {
        if (bones[i].name == name)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// PathAttachment 的扁平顶点流拆成「骨骼流 + 顶点流」两条。
//
// 两条输入路径拿到的都是**扁平**的一条（weighted 时每点 [n, (idx, x, y, w) × n]，
// rigid 时 [x, y] × points），而 chunk 20 要的是两条**分开**的流 —— 与 v7 把影响流
// 搬出顶点同一个理由：bone 那条要能整条 4 B 一次读 ⇒ 不能和 float 交错。
//
// 返回 false = 扁平流与 points 对不上（数据坏了）。参考实现在这种情况下会一路
// 越界读下去（SkeletonJson.cpp:1226 `for (i = 0, n = entrySize; i < n;)`），
// 我们报显式错。
bool SplitPathVertices(const std::vector<float>& flat, int32_t points, bool weighted,
                       std::vector<int>& bones_out, std::vector<float>& verts_out, std::string& err)
{
    bones_out.clear();
    verts_out.clear();
    if (points < 0)
    {
        err = "顶点数为负 " + std::to_string(points);
        return false;
    }
    if (!weighted)
    {
        if (flat.size() != static_cast<size_t>(points) * 2u)
        {
            err = "rigid path 需要 " + std::to_string(points * 2) + " 个 float，实际 " +
                  std::to_string(flat.size()) + " 个";
            return false;
        }
        verts_out = flat;
        return true;
    }

    size_t i = 0;
    for (int32_t p = 0; p < points; ++p)
    {
        if (i >= flat.size())
        {
            err = "weighted path 在第 " + std::to_string(p) + " 个点处扁平流就断了";
            return false;
        }
        const int32_t n = static_cast<int32_t>(flat[i++]);
        if (n <= 0)
        {
            err = "weighted path 第 " + std::to_string(p) + " 个点的影响数是 " + std::to_string(n);
            return false;
        }
        if (i + static_cast<size_t>(n) * 4u > flat.size())
        {
            err = "weighted path 第 " + std::to_string(p) + " 个点声明 " + std::to_string(n) +
                  " 个影响，但扁平流不够长";
            return false;
        }
        bones_out.push_back(n);
        for (int32_t k = 0; k < n; ++k)
        {
            const int32_t bone = static_cast<int32_t>(flat[i]);
            if (bone < 0 || bone > 65535)
            {
                err = "weighted path 的骨骼下标 " + std::to_string(bone) + " 超出 unsigned short";
                return false;
            }
            bones_out.push_back(bone);
            verts_out.push_back(flat[i + 1]);
            verts_out.push_back(flat[i + 2]);
            verts_out.push_back(flat[i + 3]);
            i += 4;
        }
    }
    if (i != flat.size())
    {
        err = "weighted path 的扁平流还剩 " + std::to_string(flat.size() - i) + " 个 float 没被消费";
        return false;
    }
    return true;
}

}   // namespace

SpineImportResult ParseSpineAtlas(const char* text, size_t len, SpineAtlas& out)
{
    out.pages.clear();
    out.regions.clear();

    std::vector<std::string> lines = SplitLines(text, len);
    size_t i = 0;
    int page = -1;

    while (i < lines.size())
    {
        std::string line = lines[i];
        Trim(line);

        if (line.empty())
        {
            // 空行结束当前页（参考实现：str.end - str.begin == 0 ⇒ page = 0）
            page = -1;
            ++i;
            continue;
        }

        if (page < 0)
        {
            SpineAtlasPage p;
            p.name = line;
            ++i;

            auto next = [&]() -> std::string { return i < lines.size() ? lines[i] : std::string(); };

            int t[4] = {0, 0, 0, 0};
            int n = 0;
            std::string l = next();
            Trim(l);
            if (KeyOf(l) == "size" && ParseTuple(l, t, 4, n) && n >= 2)
            {
                p.width = static_cast<uint32_t>(t[0]);
                p.height = static_cast<uint32_t>(t[1]);
                ++i;
            }
            else
            {
                return Fail("atlas: page '" + p.name + "' 缺少 size 行");
            }

            // format / filter / repeat —— 本导入器不用，按序吞掉
            for (int k = 0; k < 3 && i < lines.size(); ++k)
            {
                std::string s = lines[i];
                Trim(s);
                const std::string key = KeyOf(s);
                if (key == "format" || key == "filter" || key == "repeat" || key == "pma")
                {
                    ++i;
                }
                else
                {
                    break;
                }
            }

            if (p.width == 0 || p.height == 0)
            {
                return Fail("atlas: page '" + p.name + "' 的 size 非法");
            }

            out.pages.push_back(p);
            page = static_cast<int>(out.pages.size()) - 1;
            continue;
        }

        // ---- region ----
        SpineAtlasRegion r;
        r.name = line;
        r.page = static_cast<uint32_t>(page);
        ++i;

        auto peek_key = [&]() -> std::string {
            if (i >= lines.size())
            {
                return std::string();
            }
            std::string s = lines[i];
            Trim(s);
            return KeyOf(s);
        };

        if (peek_key() == "rotate")
        {
            std::string s = lines[i];
            Trim(s);
            r.rotate = (ValueOf(s) == "true");
            ++i;
        }

        int t[4] = {0, 0, 0, 0};
        int n = 0;
        if (peek_key() == "xy" && ParseTuple(lines[i], t, 4, n) && n >= 2)
        {
            const int x = t[0];
            const int y = t[1];
            ++i;

            n = 0;
            if (peek_key() == "size" && ParseTuple(lines[i], t, 4, n) && n >= 2)
            {
                const int w = t[0];
                const int h = t[1];
                ++i;

                // 可选：orig（原始尺寸）/ offset（裁剪偏移）/ index / split / pad
                r.width = 0;
                r.height = 0;
                for (;;)
                {
                    const std::string key = peek_key();
                    if (key == "orig")
                    {
                        n = 0;
                        ParseTuple(lines[i], t, 4, n);
                        r.orig_w = t[0];
                        r.orig_h = t[1];
                        ++i;
                    }
                    else if (key == "offset")
                    {
                        n = 0;
                        ParseTuple(lines[i], t, 4, n);
                        r.offset_x = t[0];
                        r.offset_y = t[1];
                        ++i;
                    }
                    else if (key == "index" || key == "split" || key == "pad")
                    {
                        ++i;
                    }
                    else
                    {
                        break;
                    }
                }

                // 没写 orig ⇒ 未裁剪：原始尺寸 = 打包尺寸，偏移 = 0
                if (r.orig_w <= 0 || r.orig_h <= 0)
                {
                    r.orig_w = w;
                    r.orig_h = h;
                }
                r.width = w;
                r.height = h;

                const SpineAtlasPage& pg = out.pages[static_cast<size_t>(page)];
                const float pw = static_cast<float>(pg.width);
                const float ph = static_cast<float>(pg.height);
                r.u = static_cast<float>(x) / pw;
                r.v = static_cast<float>(y) / ph;
                if (r.rotate)
                {
                    // 参考实现 Atlas.cpp:180-185：rotate 时 u 方向走的是 height
                    r.u2 = static_cast<float>(x + h) / pw;
                    r.v2 = static_cast<float>(y + w) / ph;
                }
                else
                {
                    r.u2 = static_cast<float>(x + w) / pw;
                    r.v2 = static_cast<float>(y + h) / ph;
                }

                out.regions.push_back(r);
                continue;
            }
            return Fail("atlas: region '" + r.name + "' 缺少 size 行");
        }

        return Fail("atlas: region '" + r.name + "' 缺少 xy 行");
    }

    return Ok();
}

const SpineAtlasRegion* FindAtlasRegion(const SpineAtlas& atlas, const std::string& name)
{
    for (const SpineAtlasRegion& r : atlas.regions)
    {
        if (r.name == name)
        {
            return &r;
        }
    }
    return nullptr;
}

// =====================================================================
// json 解析
// =====================================================================
namespace
{

using JsonValue = rapidjson::Value;

bool TryGetMember(const JsonValue& v, const char* key, JsonValue::ConstMemberIterator& out)
{
    if (!v.IsObject())
    {
        return false;
    }
    out = v.FindMember(key);
    return out != v.MemberEnd();
}

const JsonValue* GetMember(const JsonValue& v, const char* key)
{
    if (!v.IsObject())
    {
        return nullptr;
    }
    auto it = v.FindMember(key);
    return it != v.MemberEnd() ? &it->value : nullptr;
}

float GetFloat(const JsonValue& v, const char* key, float fallback)
{
    const JsonValue* m = GetMember(v, key);
    return (m && m->IsNumber()) ? m->GetFloat() : fallback;
}

std::string GetString(const JsonValue& v, const char* key, const char* fallback)
{
    const JsonValue* m = GetMember(v, key);
    return (m && m->IsString()) ? std::string(m->GetString()) : std::string(fallback);
}

// 与参考实现的 Json::getInt(..., fallback) 对齐（Json.cpp:89-91 读的是 `_valueInt`）。
//
// ⚠️ **布尔必须当 0/1**：spine 自己的 json 解析器把 true / false 写进同一个
//    `_valueInt`（Json.cpp:165 的 calloc ⇒ false = 0，:173 ⇒ true = 1），
//    而 Spine 编辑器导出的 json 里 `compress` / `stretch` / `uniform` /
//    `local` / `relative` / `bendPositive` / `constantSpeed` **全都是布尔**。
//    这里若只认 IsNumber()，布尔会被当成"非数字"回落到 fallback —— 不报错、
//    值悄悄反了。实测踩到过一次：夹具写 `"bendPositive": false` 而 IK 拿到
//    bend_dir = +1、`"constantSpeed": false` 而 path 附件拿到 constantSpeed = 1，
//    只有 skel↔json 逐字段对拍抓得到（二进制那边是显式布尔，不会错）。
//    用到它的地方（linked mesh 的 "deform"）在 spine 里也是 0/1 的开关。
int GetInt(const JsonValue& v, const char* key, int fallback)
{
    const JsonValue* m = GetMember(v, key);
    if (m == nullptr)
    {
        return fallback;
    }
    if (m->IsBool())
    {
        return m->GetBool() ? 1 : 0;
    }
    if (m->IsInt())
    {
        return m->GetInt();
    }
    return m->IsNumber() ? static_cast<int>(m->GetFloat()) : fallback;
}

// 槽位名 → 下标。参考实现里到处是 `skeletonData->findSlot(name)`，而它返回 NULL 后
// 调用方常常不检查（deform 的 :1051 空指针解引用、clip 的 :634 静默变 NULL）。
// 这里统一走这个函数，找不到就**报错**，不让作用域/目标悄悄变。
int FindSlotIndex(const std::vector<SpineSourceSlot>& slots, const std::string& name)
{
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].name == name)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}


// region 附件的 4 个顶点：照抄 RegionAttachment::updateOffset()
//   （RegionAttachment.cpp:77-103），顶点顺序 BL, UL, UR, BR（BLX=0,ULX=2,URX=4,BRX=6）。
// 参数里的 region_* 全部来自图集 region。
void ComputeRegionVertices(float x, float y, float width, float height, float scale_x, float scale_y,
                           float rotation_deg, float region_width, float region_height, float region_offset_x,
                           float region_offset_y, float region_orig_w, float region_orig_h, float* out_xy)
{
    if (region_orig_w <= 0.0f)
    {
        region_orig_w = region_width;
    }
    if (region_orig_h <= 0.0f)
    {
        region_orig_h = region_height;
    }

    const float region_scale_x = width / region_orig_w * scale_x;
    const float region_scale_y = height / region_orig_h * scale_y;
    const float local_x = -width / 2.0f * scale_x + region_offset_x * region_scale_x;
    const float local_y = -height / 2.0f * scale_y + region_offset_y * region_scale_y;
    const float local_x2 = local_x + region_width * region_scale_x;
    const float local_y2 = local_y + region_height * region_scale_y;

    const float c = std::cos(rotation_deg * kDegToRad);
    const float s = std::sin(rotation_deg * kDegToRad);

    const float lx_c = local_x * c + x;
    const float lx_s = local_x * s;
    const float ly_c = local_y * c + y;
    const float ly_s = local_y * s;
    const float lx2_c = local_x2 * c + x;
    const float lx2_s = local_x2 * s;
    const float ly2_c = local_y2 * c + y;
    const float ly2_s = local_y2 * s;

    out_xy[0] = lx_c - ly_s;   // BL.x
    out_xy[1] = ly_c + lx_s;   // BL.y
    out_xy[2] = lx_c - ly2_s;  // UL.x
    out_xy[3] = ly2_c + lx_s;  // UL.y
    out_xy[4] = lx2_c - ly2_s; // UR.x
    out_xy[5] = ly2_c + lx2_s; // UR.y
    out_xy[6] = lx2_c - ly_s;  // BR.x
    out_xy[7] = ly_c + lx2_s;  // BR.y
}

// region 的 UV：照抄 RegionAttachment::setUVs()（:105-128）。
// 顶点顺序 BL, UL, UR, BR。
void ComputeRegionUvs(const SpineAtlasRegion& reg, float* out_uv)
{
    const float u = reg.u, v = reg.v, u2 = reg.u2, v2 = reg.v2;
    // ⚠️ 参考实现的 `_uvs` 是按 **computeWorldVertices 的输出顺序** BR, BL, UL, UR 配对的
    //    （RegionAttachment.cpp:137-158 出 br/bl/ul/ur，`getUVs()` 同下标）—— 槽位名 BLX/ULX/…
    //    只是下标常量，**不是**那个角。我们的顶点按 `_vertexOffset` 的 BL, UL, UR, BR 排，
    //    所以第 j 个顶点要取参考 `_uvs` 的第 {1, 2, 3, 0}[j] 对：
    //      BL ← _uvs[ULX]，UL ← _uvs[URX]，UR ← _uvs[BRX]，BR ← _uvs[BLX]
    //    v14 及以前直接按槽位名抄（BL ← _uvs[BLX]），每个角错一位 ⇒ 贴图转了 90°。
    if (reg.rotate)
    {
        // setUVs(rotate)：_uvs[URX]=(u,v2) [BRX]=(u,v) [BLX]=(u2,v) [ULX]=(u2,v2)
        out_uv[0] = u2; out_uv[1] = v2;   // BL ← _uvs[ULX]
        out_uv[2] = u;  out_uv[3] = v2;   // UL ← _uvs[URX]
        out_uv[4] = u;  out_uv[5] = v;    // UR ← _uvs[BRX]
        out_uv[6] = u2; out_uv[7] = v;    // BR ← _uvs[BLX]
    }
    else
    {
        // setUVs：_uvs[ULX]=(u,v2) [URX]=(u,v) [BRX]=(u2,v) [BLX]=(u2,v2)
        out_uv[0] = u;  out_uv[1] = v2;   // BL ← _uvs[ULX]
        out_uv[2] = u;  out_uv[3] = v;    // UL ← _uvs[URX]
        out_uv[4] = u2; out_uv[5] = v;    // UR ← _uvs[BRX]
        out_uv[6] = u2; out_uv[7] = v2;   // BR ← _uvs[BLX]
    }
}

// mesh 的最终 UV：逐行照抄 MeshAttachment::updateUVs()（MeshAttachment.cpp:64-92）。
// region_uvs 是 json 里的 "uvs"（图集空间），输出是归一化后的最终 UV。
// 图集 region 的像素尺寸（width/height/orig/offset）全部参与计算 ——
// 图集一旦带裁剪（trim），忽略这些值会让 UV 整体偏移且**不报错**。
void ComputeMeshUvs(const SpineAtlasRegion& reg, const std::vector<float>& region_uvs, std::vector<float>& out)
{
    out.assign(region_uvs.size(), 0.0f);
    const size_t n = region_uvs.size();

    const float region_u = reg.u;
    const float region_v = reg.v;
    const float rw = static_cast<float>(reg.width);
    const float rh = static_cast<float>(reg.height);
    const float orig_w = static_cast<float>(reg.orig_w > 0 ? reg.orig_w : reg.width);
    const float orig_h = static_cast<float>(reg.orig_h > 0 ? reg.orig_h : reg.height);
    const float off_x = static_cast<float>(reg.offset_x);
    const float off_y = static_cast<float>(reg.offset_y);

    if (reg.rotate)
    {
        // textureHeight = regionWidth  / (v2 - v)   （MeshAttachment.cpp:70）
        // textureWidth  = regionHeight / (u2 - u)   （:71）
        // ⚠️ v14 及以前这里把 rw / rh 写反了（注释是对的、代码是反的）：旋转 region 的宽高相等时
        //    看不出来，不等时 UV 整体拉伸。参考侧以前没配图集、不比 UV，所以一直没被抓到
        //    （v15 的对拍台给参考加载器配了图集：OSGame 前 120 个资产里 40% 的 mesh 采样对不上）。
        const float texture_height = (reg.v2 - reg.v) != 0.0f ? rw / (reg.v2 - reg.v) : 0.0f;
        const float texture_width = (reg.u2 - reg.u) != 0.0f ? rh / (reg.u2 - reg.u) : 0.0f;
        const float u = region_u - (orig_h - off_y - rh) / (texture_width != 0.0f ? texture_width : 1.0f);
        const float v = region_v - (orig_w - off_x - rw) / (texture_height != 0.0f ? texture_height : 1.0f);
        const float width = orig_h / (texture_width != 0.0f ? texture_width : 1.0f);
        const float height = orig_w / (texture_height != 0.0f ? texture_height : 1.0f);
        for (size_t i = 0; i + 1 < n; i += 2)
        {
            out[i] = u + region_uvs[i + 1] * width;
            out[i + 1] = v + height - region_uvs[i] * height;
        }
    }
    else
    {
        const float texture_width = (reg.u2 - reg.u) != 0.0f ? rw / (reg.u2 - reg.u) : 0.0f;
        const float texture_height = (reg.v2 - reg.v) != 0.0f ? rh / (reg.v2 - reg.v) : 0.0f;
        const float u = region_u - off_x / (texture_width != 0.0f ? texture_width : 1.0f);
        const float v = region_v - (orig_h - off_y - rh) / (texture_height != 0.0f ? texture_height : 1.0f);
        const float width = orig_w / (texture_width != 0.0f ? texture_width : 1.0f);
        const float height = orig_h / (texture_height != 0.0f ? texture_height : 1.0f);
        for (size_t i = 0; i + 1 < n; i += 2)
        {
            out[i] = u + region_uvs[i] * width;
            out[i + 1] = v + region_uvs[i + 1] * height;
        }
    }
}

// ---------------------------------------------------------------------
// linked mesh（附属网格）
// ---------------------------------------------------------------------
// 参考实现的两处：SkeletonBinary.cpp:302-323、SkeletonJson.cpp:649-669，逻辑相同。
//
// linked mesh **不自带几何**：它只是"我复用那个 mesh 的顶点/三角形/regionUV，
// 但贴图区域用我自己的"。所以：
//   * `setParentMesh()` 拷父的 _vertices / _bones / _regionUVs / _triangles / _hullLength
//     （MeshAttachment.cpp:226-239）
//   * `updateUVs()` 用**自己的** region（_regionU/V/U2/V2、尺寸、offset、rotate）
//     去算最终 UV（:64-92）
//   ⇒ 最终 UV = 父的 regionUVs × 自己的图集 region。
//
// ⚠️ `configureAttachment()` 是**空函数**（AtlasAttachmentLoader.cpp:120-122），
//    region 在 `newMeshAttachment()` 里就设好了 —— 别以为它会重算一次 UV。
//
// ⚠️ 父附件在**哪个皮肤里找**，由 linked mesh 的 "skin" 字段决定 ——
//    **不是**"它所在的那个皮肤"：
//      skin 字段为空 ⇒ 在 **default 皮肤**里找
//      （SkeletonJson.cpp:653-654 / SkeletonBinary.cpp:302-303：
//       `linkedMesh->_skin.length() == 0 ? getDefaultSkin() : findSkin(...)`）
//    这条最容易照错：写一个"在 skin2 里放 linked mesh、省略 skin 字段"的资源，
//    照错了会静默拿到 default 的父附件（形状错但不报错）。
//
//    找到之后，linked mesh 自己是加在**它所在的那个皮肤**里的
//    （参考实现 :643 `skin->addAttachment(...)`，skin 是外层循环的那个）。
struct SpineLinkedMeshRef
{
    int slot_index = -1;
    int skin_index = -1;     // **它所在的**皮肤
    std::string name;
    std::string path;
    std::string parent;
    std::string skin;        // 去哪个皮肤里找父附件；空 ⇒ default
    // json 字段名是 **"deform"**（SkeletonJson.cpp:572），默认 1；
    // 二进制是紧跟 parent 的一个 bool（SkeletonBinary.cpp:594）。
    bool inherit_deform = true;
};

// ---------------------------------------------------------------------
// deform（FFD）时间轴的一项：**解析期**只记 (皮肤, 槽位, 附件名)
// ---------------------------------------------------------------------
// 帧数据本身是 per-frame 的，烘不进静态 VB —— 导入器只需要知道"哪些附件被 FFD 动到"
// （规格 §7.1）。真正的顶点在运行时由 spine 的 AnimationState 算。
//
// ⚠️ 必须等 linked mesh 解引用完之后再解析：deform 时间轴完全可以指向一个
// linked mesh，而它是**最后**才被插进 attachments 的。
struct SpineDeformRef
{
    int skin_index = -1;
    int slot_index = -1;
    std::string attachment;
    // 只为了报错时说清出处
    std::string skin_name;
    std::string slot_name;
    // 所有帧里最大的 (offset + 顶点数)。上限要等附件解出来才比得了 ⇒ 先记着。
    // 0 = 所有帧都没写 vertices（"回到 setup"），不需要比对。
    size_t required_length = 0;
};

bool ResolveLinkedMeshes(const SpineAtlas* atlas, const std::vector<std::string>& skins,
                         const std::vector<SpineSourceSlot>& slots,
                         std::vector<SpineSourceAttachment>& attachments,
                         const std::vector<SpineLinkedMeshRef>& linked, std::string& err)
{
    auto find_skin = [&skins](const std::string& name) -> int {
        for (size_t i = 0; i < skins.size(); ++i)
        {
            if (skins[static_cast<size_t>(i)] == name)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    for (const SpineLinkedMeshRef& lm : linked)
    {
        // 与常规附件同一条去重规则：先出现的胜出（同一皮肤内同名同槽位）。
        bool dup = false;
        for (const SpineSourceAttachment& a : attachments)
        {
            if (a.skin_index == lm.skin_index && a.slot_index == lm.slot_index && a.name == lm.name)
            {
                dup = true;
                break;
            }
        }
        if (dup)
        {
            continue;
        }

        const std::string slot_name =
            (lm.slot_index >= 0 && static_cast<size_t>(lm.slot_index) < slots.size())
                ? slots[static_cast<size_t>(lm.slot_index)].name
                : std::string("?");

        // 参考实现 :653-658 —— skin 找不到直接报错，不退回"当前皮肤"。
        int lookup_skin = lm.skin.empty() ? find_skin("default") : find_skin(lm.skin);
        if (lookup_skin < 0)
        {
            if (lm.skin.empty())
            {
                err = "槽位 '" + slot_name + "' 的 linked mesh '" + lm.name +
                      "' 没有 skin 字段，而文件里没有名为 \"default\" 的皮肤"
                      "（参考实现 getDefaultSkin() 在这里返回 NULL，报 \"Skin not found\"）";
            }
            else
            {
                err = "槽位 '" + slot_name + "' 的 linked mesh '" + lm.name + "' 引用了不存在的皮肤 '" +
                      lm.skin + "'（参考实现在这里报 \"Skin not found\"）";
            }
            return false;
        }

        // 记**下标**而不是指针：deform 传播要用它（下面 push_back 会让指针失效）。
        int parent_index = -1;
        for (size_t i = 0; i < attachments.size(); ++i)
        {
            const SpineSourceAttachment& a = attachments[i];
            if (a.skin_index == lookup_skin && a.slot_index == lm.slot_index && a.name == lm.parent)
            {
                parent_index = static_cast<int>(i);
                break;
            }
        }
        if (parent_index < 0)
        {
            err = "槽位 '" + slot_name + "' 的 linked mesh '" + lm.name + "' 在皮肤 '" +
                  skins[static_cast<size_t>(lookup_skin)] + "' 里找不到父附件 '" + lm.parent +
                  "'（参考实现在这里报 \"Parent mesh not found\"）";
            return false;
        }
        const SpineSourceAttachment& parent = attachments[static_cast<size_t>(parent_index)];
        if (parent.type != SpineSourceAttachment::Type::kMesh || parent.region_uvs.empty())
        {
            err = "槽位 '" + slot_name + "' 的 linked mesh '" + lm.name + "' 的父附件 '" + lm.parent +
                  "' 不是带 regionUV 的 mesh（参考实现在这里直接 static_cast 成 MeshAttachment，"
                  "父附件不是 mesh 就是未定义行为）";
            return false;
        }

        SpineSourceAttachment att;
        att.type = SpineSourceAttachment::Type::kMesh;
        att.name = lm.name;
        att.path = lm.path;
        att.slot_index = lm.slot_index;
        // 归**它所在的**皮肤，不是查父附件用的那个皮肤。
        att.skin_index = lm.skin_index;
        att.parent_index = parent_index;
        att.inherit_deform = lm.inherit_deform;

        // setParentMesh：顶点 / 骨骼 / regionUV / 三角形全部来自父附件
        att.vertices = parent.vertices;
        att.region_uvs = parent.region_uvs;
        att.triangles = parent.triangles;

        const std::string region_name = lm.path.empty() ? lm.name : lm.path;
        const SpineAtlasRegion* reg = (atlas != nullptr) ? FindAtlasRegion(*atlas, region_name) : nullptr;
        if (reg == nullptr)
        {
            err = "atlas: linked mesh '" + lm.name + "'（槽位 '" + slot_name + "'）找不到图集 region '" +
                  region_name + "'。它继承父附件的几何，但贴图区域是自己的 —— 缺了算不出 UV。";
            return false;
        }
        ComputeMeshUvs(*reg, parent.region_uvs, att.uvs);
        att.atlas_page = reg->page;   // 自己的 region、自己的页（不继承父附件的）
        attachments.push_back(std::move(att));
    }
    return true;
}

// ---------------------------------------------------------------------
// deform（FFD）—— 参考实现 SkeletonJson.cpp:1071 / SkeletonBinary.cpp:976
// ---------------------------------------------------------------------
//   deformLength = weighted ? _vertices.size() / 3 * 2 : _vertices.size()
//
// ⚠️ 两处容易照错的地方：
//   1. 参考实现的 _vertices 已经**剥掉**了每顶点那个 n（n 进了 _bones），
//      所以 size/3 是**总影响数** ⇒ weighted 时 deform 是**每影响 2 个 float**，
//      不是每顶点 2 个。运行时也印证：computeWorldVertices 里 f 随影响 +2
//      （VertexAttachment.cpp:115-127）。
//   2. rigid 与 weighted 存的东西**不一样**：
//        rigid    → **绝对**局部坐标（setup + 偏移），运行时整体**替换** _vertices（:72-73）
//        weighted → **偏移**，运行时逐影响**相加**（:122-123）
//      我们存的是带 n 的原始扁平数组，所以得先把 n 累加回总影响数。
size_t DeformLength(const SpineSourceAttachment& a)
{
    if (!a.IsWeighted())
    {
        return a.vertices.size();
    }
    size_t influences = 0;
    size_t p = 0;
    for (uint32_t v = 0; v < a.VertexCount(); ++v)
    {
        if (p >= a.vertices.size())
        {
            break;
        }
        const int n = static_cast<int>(a.vertices[p]);
        p += 1u + static_cast<size_t>(n) * 4u;   // [n, (boneIdx, x, y, w) × n]
        influences += static_cast<size_t>(n);
    }
    return influences * 2u;
}

// 稳定重排附件，并把 linked mesh 的 `parent_index` 跟着改写到新下标。
// ⚠️ 以前直接 `std::stable_sort(attachments)`：parent_index 是**下标**，重排后指向别人。
//    v13 及以前它只在重排**之前**用过（inheritDeform 传播），所以没出事；v14 把它落进
//    绘制项的 deform_parent，OSGame 73 个资产因此指到了别的槽位的附件（读端校验抓到的）。
template <typename Less>
void StableSortAttachmentsKeepParents(std::vector<SpineSourceAttachment>& attachments, Less less)
{
    std::vector<size_t> order(attachments.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return less(attachments[a], attachments[b]); });
    std::vector<int> new_index_of(attachments.size(), -1);
    for (size_t n = 0; n < order.size(); ++n)
    {
        new_index_of[order[n]] = static_cast<int>(n);
    }
    std::vector<SpineSourceAttachment> sorted;
    sorted.reserve(attachments.size());
    for (size_t n = 0; n < order.size(); ++n)
    {
        sorted.push_back(std::move(attachments[order[n]]));
        SpineSourceAttachment& a = sorted.back();
        if (a.parent_index >= 0)
        {
            a.parent_index = new_index_of[static_cast<size_t>(a.parent_index)];
        }
    }
    attachments = std::move(sorted);
}

// 把解析期收集的 deform 引用落到附件上，并按 inheritDeform 传播。
// ⚠️ 必须在 linked mesh 解引用**之后**、attachments 重排**之前**调用：
//     前者是因为 deform 可以指向 linked mesh（它是最后才插进来的）；
//     后者是因为传播用的是 parent_index（下标，重排后就失效）。
bool ApplyDeformRefs(std::vector<SpineSourceAttachment>& attachments,
                     const std::vector<SpineDeformRef>& refs,
                     const std::vector<SpineSourceNoDrawAttachment>& skipped, std::string& err)
{
    for (const SpineDeformRef& r : refs)
    {
        int hit = -1;
        for (size_t i = 0; i < attachments.size(); ++i)
        {
            const SpineSourceAttachment& a = attachments[i];
            if (a.skin_index == r.skin_index && a.slot_index == r.slot_index && a.name == r.attachment)
            {
                hit = static_cast<int>(i);
                break;
            }
        }
        if (hit < 0)
        {
            // 指向一个**我们不渲染**的附件（boundingbox / path / point）⇒ 无事发生。
            // 它们没有绘制项，deform 对它们无从生效 —— 不是错误，跳过。
            // 本工程实测 88 处（全是 path），把它们当错误会让 50 个资产导不出来，
            // 而导不出来的理由是"某个看不见的东西被动画了"。
            bool skipped_hit = false;
            for (const SpineSourceNoDrawAttachment& s : skipped)
            {
                if (s.skin_index == r.skin_index && s.slot_index == r.slot_index && s.name == r.attachment)
                {
                    skipped_hit = true;
                    break;
                }
            }
            if (skipped_hit)
            {
                continue;
            }
            err = "deform 时间轴指向的附件 '" + r.attachment + "'（皮肤 '" + r.skin_name + "'、槽位 '" +
                  r.slot_name + "'）在骨架里不存在（参考实现在这里报 \"Attachment not found\"）";
            return false;
        }

        SpineSourceAttachment& a = attachments[static_cast<size_t>(hit)];

        // 帧的 (offset + 顶点数) 不能超过这个附件的 deform 长度。
        // 参考实现是直接写 `deformed[v]`，越界了也不报错（SkeletonJson.cpp:1092-1099）。
        if (r.required_length > 0)
        {
            const size_t cap = DeformLength(a);
            if (r.required_length > cap)
            {
                err = "deform 时间轴对附件 '" + r.attachment + "'（皮肤 '" + r.skin_name + "'、槽位 '" +
                      r.slot_name + "'）需要 " + std::to_string(r.required_length) +
                      " 个 float，但该附件的 deform 长度只有 " + std::to_string(cap) +
                      "（weighted 是每**影响** 2 个、rigid 是每顶点 2 个 —— "
                      "SkeletonJson.cpp:1071 / SkeletonBinary.cpp:976）。"
                      "参考实现在这里是越界写";
                return false;
            }
        }

        // 参考实现是**无检查**的 static_cast<VertexAttachment*>（SkeletonJson.cpp:1067 /
        // SkeletonBinary.cpp:972）：而 3.8 的 RegionAttachment 继承自 Attachment、
        // **不是** VertexAttachment（RegionAttachment.h:46）⇒ 指到 region 就是未定义行为。
        // 运行时那边有 RTTI 检查，会**静默不生效**（DeformTimeline.cpp:72-74）。
        // 一边 UB、一边静默，都不能要 ⇒ 报错。
        if (a.type != SpineSourceAttachment::Type::kMesh)
        {
            err = "deform 时间轴指向的附件 '" + r.attachment + "'（皮肤 '" + r.skin_name + "'、槽位 '" +
                  r.slot_name + "'）不是 mesh —— 3.8 的 RegionAttachment 不是 VertexAttachment，"
                  "参考实现在这里是未定义的 static_cast，运行时则静默不生效";
            return false;
        }
        a.deformed = true;
    }

    // inheritDeform 传播。判据照抄 MeshAttachment::applyDeform（:94-95）：
    //   this == source || (inheritDeform && parentMesh == source)
    // 父被 FFD 动到 ⇒ 继承方的顶点同样每帧变 ⇒ 一起回退。
    // linked mesh 的父**也可以**是 linked mesh，所以循环到不动点（而不是只走一层）。
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (SpineSourceAttachment& a : attachments)
        {
            if (a.deformed || a.parent_index < 0 || !a.inherit_deform)
            {
                continue;
            }
            if (attachments[static_cast<size_t>(a.parent_index)].deformed)
            {
                a.deformed = true;
                changed = true;
            }
        }
    }

    return true;
}

}   // namespace

bool ClassifySpineVersion(const std::string& version_string, SpineSourceVersion& out_version,
                          uint32_t& out_runtime, std::string& out_error)
{
    out_version = SpineSourceVersion::kUnknown;
    out_runtime = 0;

    const auto fail = [&](const std::string& m) {
        out_error = m;
        return false;
    };

    if (version_string.empty())
    {
        return fail("spine 版本串为空。json 要 `skeleton.spine`、二进制要开头第二个字符串；"
                    "没有它就无从判断按 3.7 还是 3.8 解释槽位暗色。");
    }
    if (version_string.size() < 3)
    {
        return fail("spine 版本串 '" + version_string + "' 太短，认不出主次版本。");
    }

    const std::string major_minor = version_string.substr(0, 3);
    if (major_minor == "3.7")
    {
        out_version = SpineSourceVersion::k37;
        out_runtime = SpineGpuConstants::kSpineRuntime37;
        return true;
    }
    if (major_minor == "3.8")
    {
        out_version = SpineSourceVersion::k38;
        out_runtime = SpineGpuConstants::kSpineRuntime38;
        return true;
    }

    return fail("spine 版本是 '" + version_string + "'，本导入器只认 3.7.x 与 3.8.x。"
                "4.x 的 `_bones` 是 `Vector<int>`（不再是 unsigned short），顶点布局不同，"
                "需要另一条导入路径（规格 §1.1）。不做静默降级 —— 猜错版本只会让形状悄悄错。");
}

// =====================================================================
// 动画：json（SkeletonJson.cpp:758-1190）
// ---------------------------------------------------------------------
// 抄的时候最需要注意的不是字段，而是**三类"参考实现静默、我们报错"**：
//   1. 约束名找不到（`findIkConstraint()` 返回 NULL 后 `if (constraint == ...)` 永不成立
//      ⇒ 下标**留在 0**，SkeletonJson.cpp:933-940）⇒ 动到了**另一条**约束且不报错。
//   2. `readCurve()` 认不出的曲线字节（switch 没有 default，SkeletonBinary.cpp:1112）。
//   3. color 串长度不足 8（`toColor()` 返回 **-1**，:724-725）⇒ 颜色变成负数，
//      而"某个槽位颜色不对"不会有任何报错。
// =====================================================================

// 逐字符抄 SkeletonJson::toColor（:719-739）："rrggbbaa" 里第 index 个字节。
// ⚠️ 长度不足时返回 **-1** 而不是 0 —— 照抄（改了就与参考实现分叉）。
float ToColorComponent(const std::string& value, size_t index)
{
    if (index >= value.size() / 2)
    {
        return -1.0f;
    }
    const char hi = value[index * 2];
    const char lo = value[index * 2 + 1];
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    };
    const int h = hex(hi);
    const int l = hex(lo);
    if (h < 0 || l < 0)
    {
        return -1.0f;
    }
    return static_cast<float>(h * 16 + l) / 255.0f;
}

// 一个动画里的一个时间轴段：帧数组 + 每帧的曲线。
// `readCurve(valueMap, timeline, frameIndex)` 在**每一帧**都调用（含最后一帧），
// 而 CurveTimeline 只有 frameCount-1 段 —— 参考实现靠 `assert` 挡住越界（Release 下
// 就是越界写）。我们显式只收前 frameCount-1 段。
bool ReadCurveJson(const JsonValue& frame, SpineSourceCurve& out, std::string& err)
{
    out = SpineSourceCurve {};
    const JsonValue* curve = GetMember(frame, "curve");
    if (curve == nullptr)
    {
        return true;   // 没有 "curve" ⇒ linear（:741-742）
    }
    if (curve->IsString())
    {
        if (std::string(curve->GetString()) == "stepped")
        {
            out.type = SpineCurveType::kStepped;
            return true;
        }
        err = "曲线串 '" + std::string(curve->GetString()) + "' 不认识（只认 \"stepped\"）";
        return false;
    }
    if (curve->IsArray() && curve->Size() == 4)
    {
        out.type = SpineCurveType::kBezier;
        out.cx1 = static_cast<float>((*curve)[0].GetDouble());
        out.cy1 = static_cast<float>((*curve)[1].GetDouble());
        out.cx2 = static_cast<float>((*curve)[2].GetDouble());
        out.cy2 = static_cast<float>((*curve)[3].GetDouble());
        return true;
    }
    // ⚠️ 参考实现在这里**什么都不做**（4 个 if 之后没有 else）⇒ 静默变 linear。
    err = "curve 既不是 \"stepped\" 也不是 4 元数组（参考实现在这里静默退回 linear）";
    return false;
}

// 往 frames 里追加一帧，并把曲线收进去。`values` 的个数必须等于
// SpineTimelineEntries(type) - 1（time 另算）。
bool PushFrame(SpineSourceTimeline& tl, const JsonValue& frame, const float* values, uint32_t value_count,
               std::string& err)
{
    if (value_count + 1u != SpineTimelineEntries(tl.type))
    {
        err = "时间轴内部错误：entries 不匹配";
        return false;
    }
    tl.frames.push_back(GetFloat(frame, "time", 0.0f));
    for (uint32_t i = 0; i < value_count; ++i)
    {
        tl.frames.push_back(values[i]);
    }
    return true;
}

// 收尾：段数 = frame_count - 1。参考实现把每帧的 curve（含最后一帧）都写进去，
// Release 下是越界写 —— 我们只保留前 frame_count - 1 段。
void TrimCurves(SpineSourceTimeline& tl)
{
    if (!SpineTimelineHasCurve(tl.type))
    {
        tl.curves.clear();
        return;
    }
    const size_t want = tl.frames.empty() ? 0 : (tl.frames.size() / SpineTimelineEntries(tl.type)) - 1u;
    if (tl.curves.size() > want)
    {
        tl.curves.resize(want);
    }
    while (tl.curves.size() < want)
    {
        tl.curves.push_back(SpineSourceCurve {});
    }
}

SpineImportResult ParseSpineAnimationJson(const JsonValue& anim, const std::string& anim_name,
                                          const SpineSourceSkeleton& skel, SpineSourceAnimation& out)
{
    const auto fail = [](const std::string& m) { return Fail("json: animations." + m); };

    // ---- bones：rotate / translate / scale / shear ----
    if (const JsonValue* bones = GetMember(anim, "bones"))
    {
        if (!bones->IsObject())
        {
            return fail(anim_name + ".bones 不是对象");
        }
        for (auto bone_it = bones->MemberBegin(); bone_it != bones->MemberEnd(); ++bone_it)
        {
            const std::string bone_name(bone_it->name.GetString());
            int bone_index = -1;
            for (size_t i = 0; i < skel.bones.size(); ++i)
            {
                if (skel.bones[i].name == bone_name)
                {
                    bone_index = static_cast<int>(i);
                    break;
                }
            }
            // 参考实现 `findBoneIndex()` 返回 -1 后就拿 -1 当骨骼下标用（:875-885 之间），
            // 之后是越界读 ⇒ 报错。
            if (bone_index < 0)
            {
                return fail(anim_name + ".bones 引用了不存在的骨骼 '" + bone_name + "'");
            }
            if (!bone_it->value.IsObject())
            {
                return fail(anim_name + ".bones." + bone_name + " 不是对象");
            }
            for (auto tl_it = bone_it->value.MemberBegin(); tl_it != bone_it->value.MemberEnd(); ++tl_it)
            {
                const std::string kind(tl_it->name.GetString());
                SpineTimelineType type;
                if (kind == "rotate")
                {
                    type = SpineTimelineType::kRotate;
                }
                else if (kind == "translate")
                {
                    type = SpineTimelineType::kTranslate;
                }
                else if (kind == "scale")
                {
                    type = SpineTimelineType::kScale;
                }
                else if (kind == "shear")
                {
                    type = SpineTimelineType::kShear;
                }
                else
                {
                    // 参考实现在这里报 "Invalid timeline type for a bone"（:931）。
                    return fail(anim_name + ".bones." + bone_name + " 的时间轴类型 '" + kind +
                                "' 不认识（参考实现报 Invalid timeline type for a bone）");
                }
                if (!tl_it->value.IsArray())
                {
                    return fail(anim_name + ".bones." + bone_name + "." + kind + " 不是数组");
                }

                SpineSourceTimeline tl;
                tl.type = type;
                tl.target = bone_index;
                std::string err;
                for (const JsonValue& frame : tl_it->value.GetArray())
                {
                    if (!frame.IsObject())
                    {
                        return fail(anim_name + ".bones." + bone_name + "." + kind + " 的帧不是对象");
                    }
                    if (type == SpineTimelineType::kRotate)
                    {
                        // ⚠️ 字段名是 **"angle"**（SkeletonJson.cpp:895），不是 "rotation"/"value"
                        const float v[1] = { GetFloat(frame, "angle", 0.0f) };
                        if (!PushFrame(tl, frame, v, 1u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    else
                    {
                        // translate / scale / shear 都是 (x, y)；timelineScale = 1（_scale 恒为 1）
                        const float v[2] = { GetFloat(frame, "x", 0.0f), GetFloat(frame, "y", 0.0f) };
                        if (!PushFrame(tl, frame, v, 2u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    SpineSourceCurve c;
                    if (!ReadCurveJson(frame, c, err))
                    {
                        return fail(anim_name + ".bones." + bone_name + "." + kind + ": " + err);
                    }
                    tl.curves.push_back(c);
                }
                TrimCurves(tl);
                out.timelines.push_back(std::move(tl));
            }
        }
    }

    // ---- slots：attachment / color / twoColor ----
    if (const JsonValue* slots = GetMember(anim, "slots"))
    {
        if (!slots->IsObject())
        {
            return fail(anim_name + ".slots 不是对象");
        }
        for (auto slot_it = slots->MemberBegin(); slot_it != slots->MemberEnd(); ++slot_it)
        {
            const std::string slot_name(slot_it->name.GetString());
            const int slot_index = FindSlotIndex(skel.slots, slot_name);
            if (slot_index < 0)
            {
                return fail(anim_name + ".slots 引用了不存在的槽位 '" + slot_name +
                            "'（参考实现报 Slot not found）");
            }
            if (!slot_it->value.IsObject())
            {
                return fail(anim_name + ".slots." + slot_name + " 不是对象");
            }
            for (auto tl_it = slot_it->value.MemberBegin(); tl_it != slot_it->value.MemberEnd(); ++tl_it)
            {
                const std::string kind(tl_it->name.GetString());
                if (!tl_it->value.IsArray())
                {
                    return fail(anim_name + ".slots." + slot_name + "." + kind + " 不是数组");
                }
                SpineSourceTimeline tl;
                std::string err;
                if (kind == "attachment")
                {
                    tl.type = SpineTimelineType::kAttachment;
                    tl.target = slot_index;
                    for (const JsonValue& frame : tl_it->value.GetArray())
                    {
                        tl.frames.push_back(GetFloat(frame, "time", 0.0f));
                        // `"name": null` ⇒ 空串 = 该帧**隐藏**这个槽位（:821-823）。
                        const JsonValue* n = GetMember(frame, "name");
                        tl.attachment_names.push_back((n != nullptr && n->IsString())
                                                          ? std::string(n->GetString())
                                                          : std::string());
                    }
                    TrimCurves(tl);
                }
                else if (kind == "color")
                {
                    tl.type = SpineTimelineType::kColor;
                    tl.target = slot_index;
                    for (const JsonValue& frame : tl_it->value.GetArray())
                    {
                        const std::string col = GetString(frame, "color", "");
                        const float v[4] = {
                            ToColorComponent(col, 0), ToColorComponent(col, 1),
                            ToColorComponent(col, 2), ToColorComponent(col, 3),
                        };
                        if (!PushFrame(tl, frame, v, 4u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                        SpineSourceCurve c;
                        if (!ReadCurveJson(frame, c, err))
                        {
                            return fail(anim_name + ".slots." + slot_name + ".color: " + err);
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                }
                else if (kind == "twoColor")
                {
                    tl.type = SpineTimelineType::kTwoColor;
                    tl.target = slot_index;
                    for (const JsonValue& frame : tl_it->value.GetArray())
                    {
                        const std::string light = GetString(frame, "light", "");
                        const std::string dark = GetString(frame, "dark", "");
                        // ⚠️ dark 只有 **rgb** 三个分量（:863-866）⇒ 加起来是 1 + 4 + 3 = 8
                        const float v[7] = {
                            ToColorComponent(light, 0), ToColorComponent(light, 1),
                            ToColorComponent(light, 2), ToColorComponent(light, 3),
                            ToColorComponent(dark, 0),  ToColorComponent(dark, 1),
                            ToColorComponent(dark, 2),
                        };
                        if (!PushFrame(tl, frame, v, 7u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                        SpineSourceCurve c;
                        if (!ReadCurveJson(frame, c, err))
                        {
                            return fail(anim_name + ".slots." + slot_name + ".twoColor: " + err);
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                }
                else
                {
                    return fail(anim_name + ".slots." + slot_name + " 的时间轴类型 '" + kind +
                                "' 不认识（只认 attachment / color / twoColor）");
                }
                out.timelines.push_back(std::move(tl));
            }
        }
    }

    // ---- ik / transform：值是"约束名 → 帧数组"的**对象** ----
    const auto read_mix_timeline = [&](const JsonValue& holder, const char* section,
                                       const std::vector<std::string>& names, SpineTimelineType type,
                                       uint32_t value_count, const char* const* keys) -> SpineImportResult {
        if (!holder.IsObject())
        {
            return fail(anim_name + "." + section + " 不是对象");
        }
        for (auto it = holder.MemberBegin(); it != holder.MemberEnd(); ++it)
        {
            const std::string cname(it->name.GetString());
            int index = -1;
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (names[i] == cname)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            // ⚠️ 参考实现 `findIkConstraint()` 返回 NULL 后找下标的循环一次都不命中 ⇒
            //    `_ikConstraintIndex` **留在 0**（SkeletonJson.cpp:933-940）⇒ 动到第一条
            //    约束，而"动画动错了约束"不报错。path 那条却有检查（:990-994）。
            if (index < 0)
            {
                return fail(anim_name + "." + section + " 引用了不存在的约束 '" + cname +
                            "'（参考实现在这里把下标静默留在 0）");
            }
            if (!it->value.IsArray())
            {
                return fail(anim_name + "." + section + "." + cname + " 不是数组");
            }
            SpineSourceTimeline tl;
            tl.type = type;
            tl.target = index;
            std::string err;
            for (const JsonValue& frame : it->value.GetArray())
            {
                float v[5] = { 0, 0, 0, 0, 0 };
                for (uint32_t k = 0; k < value_count; ++k)
                {
                    v[k] = GetFloat(frame, keys[k], 1.0f);
                }
                if (!PushFrame(tl, frame, v, value_count, err))
                {
                    return fail(anim_name + ": " + err);
                }
                SpineSourceCurve c;
                if (!ReadCurveJson(frame, c, err))
                {
                    return fail(anim_name + "." + section + "." + cname + ": " + err);
                }
                tl.curves.push_back(c);
            }
            TrimCurves(tl);
            out.timelines.push_back(std::move(tl));
        }
        return Ok();
    };

    if (const JsonValue* ik = GetMember(anim, "ik"))
    {
        // ⚠️ IK 帧里 bendPositive / compress / stretch 在 json 里是 **0/1 数字**
        //    （`Json::getInt(valueMap, "bendPositive", 1) ? 1 : -1`，:945-947），
        //    而容器里存的是 float ⇒ 1 / -1 / 0。
        if (!ik->IsObject())
        {
            return fail(anim_name + ".ik 不是对象");
        }
        for (auto it = ik->MemberBegin(); it != ik->MemberEnd(); ++it)
        {
            const std::string cname(it->name.GetString());
            int index = -1;
            for (size_t i = 0; i < skel.ik_names.size(); ++i)
            {
                if (skel.ik_names[i] == cname)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0)
            {
                return fail(anim_name + ".ik 引用了不存在的约束 '" + cname +
                            "'（参考实现在这里把下标静默留在 0）");
            }
            if (!it->value.IsArray())
            {
                return fail(anim_name + ".ik." + cname + " 不是数组");
            }
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kIkConstraint;
            tl.target = index;
            std::string err;
            for (const JsonValue& frame : it->value.GetArray())
            {
                const float bend = GetInt(frame, "bendPositive", 1) ? 1.0f : -1.0f;
                const float v[4] = {
                    GetFloat(frame, "mix", 1.0f),
                    bend,
                    GetInt(frame, "compress", 0) ? 1.0f : 0.0f,
                    GetInt(frame, "stretch", 0) ? 1.0f : 0.0f,
                };
                if (!PushFrame(tl, frame, v, 4u, err))
                {
                    return fail(anim_name + ": " + err);
                }
                SpineSourceCurve c;
                if (!ReadCurveJson(frame, c, err))
                {
                    return fail(anim_name + ".ik." + cname + ": " + err);
                }
                tl.curves.push_back(c);
            }
            TrimCurves(tl);
            out.timelines.push_back(std::move(tl));
        }
    }

    if (const JsonValue* tc = GetMember(anim, "transform"))
    {
        static const char* kTransformKeys[4] = { "rotateMix", "translateMix", "scaleMix", "shearMix" };
        const SpineImportResult r =
            read_mix_timeline(*tc, "transform", skel.transform_names,
                              SpineTimelineType::kTransformConstraint, 4u, kTransformKeys);
        if (!r.ok)
        {
            return r;
        }
    }

    // ---- paths：**两**层（约束名 → 时间轴名 → 帧数组），与 ik/transform 不同 ----
    if (const JsonValue* paths = GetMember(anim, "paths"))
    {
        if (!paths->IsObject())
        {
            return fail(anim_name + ".paths 不是对象");
        }
        for (auto c_it = paths->MemberBegin(); c_it != paths->MemberEnd(); ++c_it)
        {
            const std::string cname(c_it->name.GetString());
            int index = -1;
            for (size_t i = 0; i < skel.path_names.size(); ++i)
            {
                if (skel.path_names[i] == cname)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0)
            {
                return fail(anim_name + ".paths 引用了不存在的约束 '" + cname +
                            "'（参考实现报 Path constraint not found）");
            }
            if (!c_it->value.IsObject())
            {
                return fail(anim_name + ".paths." + cname + " 不是对象");
            }
            for (auto tl_it = c_it->value.MemberBegin(); tl_it != c_it->value.MemberEnd(); ++tl_it)
            {
                const std::string kind(tl_it->name.GetString());
                SpineTimelineType type;
                const char* value_key = nullptr;
                if (kind == "position")
                {
                    type = SpineTimelineType::kPathConstraintPosition;
                    value_key = "position";
                }
                else if (kind == "spacing")
                {
                    type = SpineTimelineType::kPathConstraintSpacing;
                    value_key = "spacing";
                }
                else if (kind == "mix")
                {
                    type = SpineTimelineType::kPathConstraintMix;
                }
                else
                {
                    return fail(anim_name + ".paths." + cname + " 的时间轴类型 '" + kind +
                                "' 不认识（只认 position / spacing / mix）");
                }
                if (!tl_it->value.IsArray())
                {
                    return fail(anim_name + ".paths." + cname + "." + kind + " 不是数组");
                }
                SpineSourceTimeline tl;
                tl.type = type;
                tl.target = index;
                std::string err;
                for (const JsonValue& frame : tl_it->value.GetArray())
                {
                    if (type == SpineTimelineType::kPathConstraintMix)
                    {
                        const float v[2] = { GetFloat(frame, "rotateMix", 1.0f),
                                             GetFloat(frame, "translateMix", 1.0f) };
                        if (!PushFrame(tl, frame, v, 2u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    else
                    {
                        const float v[1] = { GetFloat(frame, value_key, 0.0f) };
                        if (!PushFrame(tl, frame, v, 1u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    SpineSourceCurve c;
                    if (!ReadCurveJson(frame, c, err))
                    {
                        return fail(anim_name + ".paths." + cname + "." + kind + ": " + err);
                    }
                    tl.curves.push_back(c);
                }
                TrimCurves(tl);
                out.timelines.push_back(std::move(tl));
            }
        }
    }

    // ---- deform：皮肤 → 槽位 → 附件 → 帧数组（**四**层）----
    if (const JsonValue* deform = GetMember(anim, "deform"))
    {
        if (!deform->IsObject())
        {
            return fail(anim_name + ".deform 不是对象");
        }
        for (auto sit = deform->MemberBegin(); sit != deform->MemberEnd(); ++sit)
        {
            const std::string skin_name(sit->name.GetString());
            if (!sit->value.IsObject())
            {
                return fail(anim_name + ".deform 里皮肤 '" + skin_name + "' 的值不是对象");
            }
            int skin_index = -1;
            for (size_t i = 0; i < skel.skins.size(); ++i)
            {
                if (skel.skins[i] == skin_name)
                {
                    skin_index = static_cast<int>(i);
                    break;
                }
            }
            // 参考实现 `findSkin(...)` 返回 NULL 后紧接着 `skin->getAttachment(...)`
            // ⇒ 皮肤名写错是**空指针**，不是报错。
            if (skin_index < 0)
            {
                return fail(anim_name + ".deform 引用了不存在的皮肤 '" + skin_name +
                            "'（参考实现在这里是空指针解引用）");
            }
            for (auto slot_it = sit->value.MemberBegin(); slot_it != sit->value.MemberEnd(); ++slot_it)
            {
                const std::string slot_name(slot_it->name.GetString());
                if (!slot_it->value.IsObject())
                {
                    return fail(anim_name + ".deform 里槽位 '" + slot_name + "' 的值不是对象");
                }
                const int slot_index = FindSlotIndex(skel.slots, slot_name);
                if (slot_index < 0)
                {
                    return fail(anim_name + ".deform 引用了不存在的槽位 '" + slot_name + "'");
                }
                for (auto att_it = slot_it->value.MemberBegin(); att_it != slot_it->value.MemberEnd();
                     ++att_it)
                {
                    const std::string att_name(att_it->name.GetString());
                    if (!att_it->value.IsArray())
                    {
                        return fail(anim_name + ".deform 里附件 '" + att_name + "' 的值不是帧数组");
                    }
                    SpineSourceTimeline tl;
                    tl.type = SpineTimelineType::kDeform;
                    tl.deform_skin = skin_index;
                    tl.deform_slot = slot_index;
                    tl.deform_attachment = att_name;
                    std::string err;
                    for (const JsonValue& frame : att_it->value.GetArray())
                    {
                        SpineSourceDeformFrame df;
                        const JsonValue* verts = GetMember(frame, "vertices");
                        if (verts != nullptr)
                        {
                            if (!verts->IsArray())
                            {
                                return fail(anim_name + ".deform." + att_name + " 的 vertices 不是数组");
                            }
                            const int offset = static_cast<int>(GetFloat(frame, "offset", 0.0f));
                            if (offset < 0)
                            {
                                return fail(anim_name + ".deform." + att_name + " 的帧 offset " +
                                            std::to_string(offset) + " 为负");
                            }
                            df.start = static_cast<uint32_t>(offset);
                            for (const JsonValue& v : verts->GetArray())
                            {
                                df.values.push_back(static_cast<float>(v.GetDouble()));
                            }
                        }
                        // verts == nullptr ⇒ 回退帧（df.values 空）
                        if (!PushFrame(tl, frame, nullptr, 0u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                        tl.deform_frames.push_back(std::move(df));
                        SpineSourceCurve c;
                        if (!ReadCurveJson(frame, c, err))
                        {
                            return fail(anim_name + ".deform." + att_name + ": " + err);
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                    out.timelines.push_back(std::move(tl));
                }
            }
        }
    }

    // ---- drawOrder / draworder：整个骨架的槽位顺序 ----
    const JsonValue* draw_order = GetMember(anim, "drawOrder");
    if (draw_order == nullptr)
    {
        draw_order = GetMember(anim, "draworder");   // 参考实现两个都认（:772-774）
    }
    if (draw_order != nullptr)
    {
        if (!draw_order->IsArray())
        {
            return fail(anim_name + ".drawOrder 不是数组");
        }
        SpineSourceTimeline tl;
        tl.type = SpineTimelineType::kDrawOrder;
        tl.target = 0;
        for (const JsonValue& frame : draw_order->GetArray())
        {
            SpineSourceDrawOrderFrame dof;
            if (const JsonValue* offsets = GetMember(frame, "offsets"))
            {
                if (!offsets->IsArray())
                {
                    return fail(anim_name + ".drawOrder 的 offsets 不是数组");
                }
                for (const JsonValue& o : offsets->GetArray())
                {
                    const std::string sname = GetString(o, "slot", "");
                    const int si = FindSlotIndex(skel.slots, sname);
                    if (si < 0)
                    {
                        return fail(anim_name + ".drawOrder 引用了不存在的槽位 '" + sname +
                                    "'（参考实现报 Slot not found）");
                    }
                    dof.slots.push_back(static_cast<uint32_t>(si));
                    dof.offsets.push_back(GetInt(o, "offset", 0));
                }
            }
            tl.frames.push_back(GetFloat(frame, "time", 0.0f));
            tl.draw_order_frames.push_back(std::move(dof));
        }
        TrimCurves(tl);
        out.timelines.push_back(std::move(tl));
    }

    // ---- events（v16，SkeletonJson.cpp:1172-1198）----
    if (const JsonValue* events = GetMember(anim, "events"))
    {
        if (!events->IsArray())
        {
            return fail(anim_name + ".events 不是数组");
        }
        if (events->Size() > 0)
        {
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kEvent;
            tl.target = 0;
            for (const JsonValue& frame : events->GetArray())
            {
                const std::string ename = GetString(frame, "name", "");
                int ei = -1;
                for (size_t k = 0; k < skel.events.size(); ++k)
                {
                    if (skel.events[k].name == ename)
                    {
                        ei = static_cast<int>(k);
                        break;
                    }
                }
                if (ei < 0)
                {
                    return fail(anim_name + ".events 引用了不存在的事件 '" + ename + "'（参考实现报 Event not found）");
                }
                const SpineSourceEventData& data = skel.events[static_cast<size_t>(ei)];
                SpineSourceEventKey key;
                key.event = static_cast<uint32_t>(ei);
                key.int_value = GetInt(frame, "int", data.int_value);
                key.float_value = GetFloat(frame, "float", data.float_value);
                key.string_value = GetString(frame, "string", data.string_value.c_str());
                // 只有定义带音频（非空）时逐帧读，否则是 Event 构造函数的 1 / 0（不继承定义的值）
                if (!data.audio_path.empty())
                {
                    key.volume = GetFloat(frame, "volume", 1.0f);
                    key.balance = GetFloat(frame, "balance", 0.0f);
                }
                tl.frames.push_back(GetFloat(frame, "time", 0.0f));
                tl.event_keys.push_back(std::move(key));
            }
            out.timelines.push_back(std::move(tl));
        }
    }

    return Ok();
}

SpineImportResult ParseSpineSkeletonJson(const char* json, size_t len, const SpineAtlas* atlas,
                                         SpineSourceSkeleton& out)
{
    out.bones.clear();
    out.slots.clear();
    out.attachments.clear();
    out.skins.clear();
    out.has_draw_order_timeline = false;
    out.uses_tint_black = false;
    out.has_clipping = false;
    out.has_event_timeline = false;
    out.animations.clear();
    out.ik_names.clear();
    out.transform_names.clear();
    out.path_names.clear();
    out.iks.clear();
    out.transforms.clear();
    out.paths.clear();
    out.path_attachments.clear();
    out.no_draw_attachments.clear();

    rapidjson::Document doc;
    const std::string text(json, len);
    if (doc.Parse(text.c_str()).HasParseError())
    {
        return Fail("json: 解析失败（offset " + std::to_string(doc.GetErrorOffset()) + "）");
    }
    if (!doc.IsObject())
    {
        return Fail("json: 根节点不是对象");
    }

    // ---- 版本：只认 3.7.x / 3.8.x ----
    // 放宽到 3.7 是因为本工程（OSGame）782 个 `.skel.bytes` **全是 3.7.83**；
    // 两者字节布局逐段比对一致，唯一差别是槽位暗色判据（见 `SpineSourceVersion` 的注释）。
    const JsonValue* skeleton = GetMember(doc, "skeleton");
    const std::string spine_ver = skeleton ? GetString(*skeleton, "spine", "") : std::string();
    {
        uint32_t runtime = 0;   // Bake 阶段由 `src.version` 重新算出，这里不用
        std::string verr;
        if (!ClassifySpineVersion(spine_ver, out.version, runtime, verr))
        {
            return Fail("json: " + verr);
        }
    }
    out.version_string = spine_ver;

    // 图集页名：容器 chunk 8（atlas_pages）要用（纹理引用）。没有图集时留空 —— 但那样需要 UV 的
    // 附件会在下面报错，不会静默产出半个资产。
    if (atlas)
    {
        out.atlas_pages.reserve(atlas->pages.size());
        for (const SpineAtlasPage& p : atlas->pages)
        {
            out.atlas_pages.push_back(p.name);
        }
    }

    // ---- bones ----
    const JsonValue* bones = GetMember(doc, "bones");
    if (bones && bones->IsArray())
    {
        for (const JsonValue& b : bones->GetArray())
        {
            SpineSourceBone bone;
            bone.name = GetString(b, "name", "");
            const std::string parent = GetString(b, "parent", "");
            bone.parent = -1;
            if (!parent.empty())
            {
                // 父骨骼必须在自己**之前** —— 参考实现也是只查已加进去的骨骼
                // （`skeletonData->findBone(parentName)`，SkeletonJson.cpp:155），
                // 所以这条不变量是格式自带的，不是我们加的约束。
                for (size_t k = 0; k < out.bones.size(); ++k)
                {
                    if (out.bones[k].name == parent)
                    {
                        bone.parent = static_cast<int>(k);
                        break;
                    }
                }
                // ⚠️ 参考实现在这里**报错**（SkeletonJson.cpp:156-160 "Parent bone not found"）。
                // 早先我们漏了这一步：查不到就把 parent 留在 -1，于是那根骨骼**静默变成
                // 根骨骼** —— 整棵子树的位移/旋转全错，而"看起来只是摆位不对"没有任何报错。
                if (bone.parent < 0)
                {
                    return Fail("json: 骨骼 '" + bone.name + "' 引用了不存在的父骨骼 '" + parent +
                                "'（参考实现在这里报 Parent bone not found）");
                }
            }

            // 默认值逐条抄 SkeletonJson.cpp:165-172。⚠️ scaleX/scaleY 的默认是 **1**。
            bone.length = GetFloat(b, "length", 0.0f);
            bone.x = GetFloat(b, "x", 0.0f);
            bone.y = GetFloat(b, "y", 0.0f);
            bone.rotation = GetFloat(b, "rotation", 0.0f);
            bone.scale_x = GetFloat(b, "scaleX", 1.0f);
            bone.scale_y = GetFloat(b, "scaleY", 1.0f);
            bone.shear_x = GetFloat(b, "shearX", 0.0f);
            bone.shear_y = GetFloat(b, "shearY", 0.0f);

            // 字段名是 **"transform"**（不是 "transformMode"）。5 个 if 都没有 else
            // （SkeletonJson.cpp:173-189）⇒ 认不出的串会被**静默留在 Normal**。
            // 那是"整棵子树的 inherit 行为变了且不报错"，所以我们显式报错。
            const std::string mode = GetString(b, "transform", "normal");
            if (mode == "normal")
            {
                bone.transform_mode = SpineTransformMode::kNormal;
            }
            else if (mode == "onlyTranslation")
            {
                bone.transform_mode = SpineTransformMode::kOnlyTranslation;
            }
            else if (mode == "noRotationOrReflection")
            {
                bone.transform_mode = SpineTransformMode::kNoRotationOrReflection;
            }
            else if (mode == "noScale")
            {
                bone.transform_mode = SpineTransformMode::kNoScale;
            }
            else if (mode == "noScaleOrReflection")
            {
                bone.transform_mode = SpineTransformMode::kNoScaleOrReflection;
            }
            else
            {
                return Fail("json: 骨骼 '" + bone.name + "' 的 transform 模式 '" + mode +
                            "' 不认识（只认 normal / onlyTranslation / noRotationOrReflection / "
                            "noScale / noScaleOrReflection —— 参考实现在这里静默退回 normal，"
                            "那会让整棵子树的继承行为悄悄变掉）");
            }

            out.bones.push_back(bone);
        }
    }

    // ---- slots ----
    const JsonValue* slots = GetMember(doc, "slots");
    if (slots && slots->IsArray())
    {
        for (const JsonValue& s : slots->GetArray())
        {
            SpineSourceSlot slot;
            slot.name = GetString(s, "name", "");
            const std::string bone_name = GetString(s, "bone", "");
            slot.bone_index = -1;
            for (size_t k = 0; k < out.bones.size(); ++k)
            {
                if (out.bones[k].name == bone_name)
                {
                    slot.bone_index = static_cast<int>(k);
                    break;
                }
            }
            if (slot.bone_index < 0)
            {
                return Fail("json: slot '" + slot.name + "' 引用了不存在的骨骼 '" + bone_name + "'");
            }
            // ---- 颜色（SkeletonJson.cpp:219-232）----
            // ⚠️ 默认值是 **(1,1,1,1)**（`SlotData.cpp:39`），不是 0。
            //    setup 色是 color / twoColor 时间轴的基线：
            //    时间轴写的是**绝对颜色**（ColorTimeline.cpp:110 `slot._color.set(r,g,b,a)`），
            //    而"没有时间轴命中"时槽位停在 setup 色 —— 容器里没有 setup 色，
            //    解算器只能默认成纯白，而**纯白与 setup 白在多数资产上完全看不出区别**
            //    （正是那种"错了但不报错"的坑）。
            if (GetMember(s, "color") != nullptr)
            {
                const std::string col = GetString(s, "color", "");
                std::string err;
                const float rgba[4] = {ToColorComponent(col, 0), ToColorComponent(col, 1),
                                       ToColorComponent(col, 2), ToColorComponent(col, 3)};
                for (int k = 0; k < 4; ++k)
                {
                    if (rgba[k] < 0.0f)
                    {
                        err = "颜色串 '" + col + "' 不是 8 位十六进制";
                        break;
                    }
                }
                if (!err.empty())
                {
                    return Fail("json: slot '" + slot.name + "'.color: " + err);
                }
                for (int k = 0; k < 4; ++k)
                {
                    slot.color[k] = rgba[k];
                }
            }
            if (GetMember(s, "dark") != nullptr)
            {
                const std::string dk = GetString(s, "dark", "");
                std::string err;
                // ⚠️ dark 只有 **rgb** 三个分量（:227-230），alpha 写死 **1**（:230）。
                const float rgb[3] = {ToColorComponent(dk, 0), ToColorComponent(dk, 1),
                                      ToColorComponent(dk, 2)};
                for (int k = 0; k < 3; ++k)
                {
                    if (rgb[k] < 0.0f)
                    {
                        err = "颜色串 '" + dk + "' 不是 6 位十六进制";
                        break;
                    }
                }
                if (!err.empty())
                {
                    return Fail("json: slot '" + slot.name + "'.dark: " + err);
                }
                for (int k = 0; k < 3; ++k)
                {
                    slot.dark[k] = rgb[k];
                }
                slot.dark[3] = 1.0f;
                slot.has_dark = true;
                out.uses_tint_black = true;
            }
            slot.attachment_name = GetString(s, "attachment", "");
            // SkeletonJson.cpp:241-250：认不出的字符串参考实现静默当 normal；这里报错。
            {
                const std::string blend = GetString(s, "blend", "normal");
                if (blend == "normal")
                {
                    slot.blend_mode = SpineBlendMode::kNormal;
                }
                else if (blend == "additive")
                {
                    slot.blend_mode = SpineBlendMode::kAdditive;
                }
                else if (blend == "multiply")
                {
                    slot.blend_mode = SpineBlendMode::kMultiply;
                }
                else if (blend == "screen")
                {
                    slot.blend_mode = SpineBlendMode::kScreen;
                }
                else
                {
                    return Fail("json: slot '" + slot.name + "' 的混合模式 '" + blend +
                                "' 不认识（normal / additive / multiply / screen）");
                }
            }
            out.slots.push_back(slot);
        }
    }

    // ---- skins ----
    // ⚠️ 3.8 的 json 是**对象**形式：skins → { 皮肤名 → { 槽位名 → { 附件名 → {...} } } }
    //    判据在参考实现自己身上：SkeletonJson.cpp:439 `new Skin(skinMap->_name)` ——
    //    皮肤名取自**成员名**；而 Json.cpp:402-450 的 parseArray 里数组成员是
    //    `new Json(NULL)`、之后**从不**赋 _name —— 数组形式会拿到空名。
    //    数组形式（[{ "name": ..., "attachments": {...} }]）是后面的版本才有的，
    //    3.8 的参考实现读不了；这里顺手接住，皮肤名改取 "name" 字段。
    struct SkinEntry
    {
        std::string name;
        const JsonValue* atts = nullptr;
    };
    std::vector<SkinEntry> skin_entries;
    const JsonValue* skins = GetMember(doc, "skins");
    if (skins && skins->IsArray())
    {
        for (const JsonValue& sk : skins->GetArray())
        {
            if (const JsonValue* atts = GetMember(sk, "attachments"))
            {
                skin_entries.push_back(SkinEntry {GetString(sk, "name", ""), atts});
            }
        }
    }
    else if (skins && skins->IsObject())
    {
        for (auto it = skins->MemberBegin(); it != skins->MemberEnd(); ++it)
        {
            skin_entries.push_back(SkinEntry {std::string(it->name.GetString()), &it->value});
        }
    }

    // 皮肤名必须唯一：重名会让"按名换装"落到哪一个变不确定
    // （参考实现 findSkin() 取第一个，是**静默**的）。
    for (const SkinEntry& se : skin_entries)
    {
        for (const std::string& s : out.skins)
        {
            if (s == se.name)
            {
                return Fail("json: 重复的皮肤名 '" + se.name + "'（换装会按名字查，重名取哪个是不确定的）");
            }
        }
        out.skins.push_back(se.name);
    }

    // 收集：**不再跨皮肤合并** —— 同一个 (槽位, 附件名) 在不同皮肤下是两份几何。
    struct PendingAttachment
    {
        int skin_index;
        int slot_index;
        std::string name;
        const JsonValue* json;
    };
    std::vector<PendingAttachment> pending;

    for (size_t si = 0; si < skin_entries.size(); ++si)
    {
        const JsonValue* atts = skin_entries[si].atts;
        if (!atts || !atts->IsObject())
        {
            continue;
        }
        for (auto slot_it = atts->MemberBegin(); slot_it != atts->MemberEnd(); ++slot_it)
        {
            const std::string slot_name(slot_it->name.GetString());
            int slot_index = -1;
            for (size_t k = 0; k < out.slots.size(); ++k)
            {
                if (out.slots[k].name == slot_name)
                {
                    slot_index = static_cast<int>(k);
                    break;
                }
            }
            if (slot_index < 0 || !slot_it->value.IsObject())
            {
                continue;
            }
            for (auto att_it = slot_it->value.MemberBegin(); att_it != slot_it->value.MemberEnd(); ++att_it)
            {
                const std::string att_name(att_it->name.GetString());
                bool dup = false;
                for (const PendingAttachment& p : pending)
                {
                    if (p.skin_index == static_cast<int>(si) && p.slot_index == slot_index && p.name == att_name)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                {
                    pending.push_back(PendingAttachment {static_cast<int>(si), slot_index, att_name,
                                                         &att_it->value});
                }
            }
        }
    }

    // 按 (皮肤, 绘制顺序) 排：先 default 皮肤的全部附件，再下一个皮肤。
    std::sort(pending.begin(), pending.end(),
              [](const PendingAttachment& a, const PendingAttachment& b)
              {
                  if (a.skin_index != b.skin_index)
                  {
                      return a.skin_index < b.skin_index;
                  }
                  return a.slot_index < b.slot_index;
              });

    // linked mesh 要等所有附件都解析完才能解引用父附件（父可能在**后面**的 skin 里）
    std::vector<SpineLinkedMeshRef> linked;

    for (const PendingAttachment& p : pending)
    {
        const JsonValue& a = *p.json;
        const std::string type = GetString(a, "type", "region");

        // json 里 linked mesh 有两种写法：type="linkedmesh"，或 type="mesh" 带 "parent"
        // （参考实现 SkeletonJson.cpp:462-465 判类型、:538 判 parent）。
        const JsonValue* jparent = GetMember(a, "parent");
        if (type == "linkedmesh" || (type == "mesh" && jparent != nullptr))
        {
            SpineLinkedMeshRef lm;
            lm.slot_index = p.slot_index;
            lm.skin_index = p.skin_index;
            lm.name = p.name;
            lm.path = GetString(a, "path", "");
            lm.parent = jparent != nullptr && jparent->IsString() ? jparent->GetString() : std::string();
            lm.skin = GetString(a, "skin", "");
            if (lm.parent.empty())
            {
                return Fail("json: 附件 '" + lm.name + "' 标成 linked mesh 但没有 parent");
            }
            // json 里这个字段就叫 **"deform"**（不是 "inheritDeform"），默认 1
            // （SkeletonJson.cpp:572 `Json::getInt(attachmentMap, "deform", 1)`）。
            // 它决定父附件的 deform 是否也作用于继承方
            // （MeshAttachment::applyDeform，:94-95）⇒ 导入期要用：继承方得一起回退。
            lm.inherit_deform = GetInt(a, "deform", 1) != 0;
            linked.push_back(std::move(lm));
            continue;
        }

        if (type == "clipping")
        {
            // 字段顺序照抄参考实现（SkeletonJson.cpp:625-640）："end" 槽位名 →
            // vertexCount → vertices。
            //
            // ⚠️ `Json::getString(map, "end", 0)` 拿到名字后 `findSlot()` 的返回值
            // **没有检查**（:631-635）⇒ 槽位名写错 / 槽位不存在时 `_endSlot` 静默变 NULL，
            // 作用域从"裁到某个槽"变成"裁到最后一个槽"。画面差别极大却没有任何报错
            // （二进制那边反而是显式报错的，见下面 ParseSkelAttachment）⇒ 这里必须报错。
            SpineSourceClip clip;
            clip.skin_index = p.skin_index;
            clip.slot_index = p.slot_index;
            clip.name = p.name;
            const std::string end_name = GetString(a, "end", "");
            if (!end_name.empty())
            {
                const int end_index = FindSlotIndex(out.slots, end_name);
                if (end_index < 0)
                {
                    return Fail("json: 裁剪附件 '" + p.name + "'（槽位 '" +
                                out.slots[static_cast<size_t>(p.slot_index)].name + "'）的 end 槽位 '" +
                                end_name + "' 不存在（参考实现在这里静默当成 NULL ⇒ 裁剪一路延伸到最后一个槽位）");
                }
                clip.end_slot_index = end_index;
            }
            // 没写 "end"（或写了空串）⇒ end_slot_index 保持 -1 ⇒ 裁到最后一个槽位。

            // ---- 裁剪多边形本身（SkeletonJson.cpp:636-637）----
            // `vertexCount = Json::getInt(map, "vertexCount", 0)` 是**顶点数**，
            // readVertices 里拿它当 worldVerticesLength（float 数 = 顶点数 × 2）。
            const int vertex_count = GetInt(a, "vertexCount", 0);
            const JsonValue* jverts = GetMember(a, "vertices");
            // ⚠️ 参考实现在这里有个坑：缺 "vertexCount" ⇒ verticesLength == 0 ≠ entrySize
            // ⇒ `readVertices` 会走 **weighted** 分支（:1218 的判据），把一串局部坐标
            // 当成 [n, (bone,x,y,w)×n] 去解析 ⇒ 多边形变成一堆垃圾骨骼索引，且不报错。
            // 这里必须显式挡住。
            if (vertex_count <= 0 || jverts == nullptr || !jverts->IsArray())
            {
                return Fail("json: 裁剪附件 '" + p.name + "'（槽位 '" +
                            out.slots[static_cast<size_t>(p.slot_index)].name +
                            "'）缺 vertexCount 或 vertices（参考实现在这里会把 rigid 多边形"
                            "误判成 weighted ⇒ 静默解析成垃圾骨骼索引）");
            }
            clip.vertex_count = vertex_count;
            clip.vertices.reserve(jverts->Size());
            for (const JsonValue& f : jverts->GetArray())
            {
                clip.vertices.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }
            // ⚠️ weighted 的判据照抄 `readVertices`（:1218）：**数组长度 == vertexCount*2
            // 才是 rigid**。裁剪附件不贴图 ⇒ 没有 uvs 可比对，只能比这个数。
            clip.weighted = clip.vertices.size() != static_cast<size_t>(vertex_count) * 2u;

            // deform 可以指向它（ClippingAttachment 也是 VertexAttachment）⇒ 与二进制侧
            // （RecordNoDrawAttachment）对齐进"不渲染附件表"，否则指向它的 deform 时间轴
            // 会被当成"附件不存在"报错（v14 及以前 json 路径就是这样，二进制路径没有）。
            SpineSourceNoDrawAttachment nd;
            nd.skin_index = p.skin_index;
            nd.slot_index = p.slot_index;
            nd.name = p.name;
            out.no_draw_attachments.push_back(std::move(nd));

            out.clips.push_back(clip);
            out.has_clipping = true;
            continue;
        }

        SpineSourceAttachment att;
        att.name = p.name;
        att.path = GetString(a, "path", "");
        att.slot_index = p.slot_index;
        att.skin_index = p.skin_index;

        const std::string region_name = att.path.empty() ? att.name : att.path;
        const SpineAtlasRegion* reg = atlas ? FindAtlasRegion(*atlas, region_name) : nullptr;
        if (reg != nullptr)
        {
            att.atlas_page = reg->page;
        }

        if (type == "region")
        {
            att.type = SpineSourceAttachment::Type::kRegion;

            const float x = GetFloat(a, "x", 0.0f);
            const float y = GetFloat(a, "y", 0.0f);
            const float w = GetFloat(a, "width", 0.0f);
            const float h = GetFloat(a, "height", 0.0f);
            const float sx = GetFloat(a, "scaleX", 1.0f);
            const float sy = GetFloat(a, "scaleY", 1.0f);
            const float rot = GetFloat(a, "rotation", 0.0f);

            if (!reg)
            {
                return Fail("atlas: 附件 '" + att.name + "'（槽位 '" + out.slots[static_cast<size_t>(p.slot_index)].name +
                            "'）找不到图集 region '" + region_name + "'。缺图集就算不出 UV —— 不拿零 UV 糊过去。");
            }

            // 未裁剪：region 尺寸即原始尺寸，偏移为 0
            float xy[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            ComputeRegionVertices(x, y, w, h, sx, sy, rot, static_cast<float>(reg->width),
                                  static_cast<float>(reg->height), static_cast<float>(reg->offset_x),
                                  static_cast<float>(reg->offset_y), static_cast<float>(reg->orig_w),
                                  static_cast<float>(reg->orig_h), xy);
            att.vertices.assign(xy, xy + 8);

            float uv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            ComputeRegionUvs(*reg, uv);
            att.uvs.assign(uv, uv + 8);

            // Spine 的 region 三角形固定是 BL,UL,UR / UR,BR,BL
            att.triangles = {0, 1, 2, 2, 3, 0};
        }
        else if (type == "mesh")
        {
            att.type = SpineSourceAttachment::Type::kMesh;

            const JsonValue* juvs = GetMember(a, "uvs");
            const JsonValue* jtri = GetMember(a, "triangles");
            const JsonValue* jverts = GetMember(a, "vertices");
            if (!juvs || !juvs->IsArray() || !jtri || !jtri->IsArray() || !jverts || !jverts->IsArray())
            {
                return Fail("json: mesh 附件 '" + att.name + "' 缺少 uvs / triangles / vertices");
            }

            std::vector<float> region_uvs;
            region_uvs.reserve(juvs->Size());
            for (const JsonValue& f : juvs->GetArray())
            {
                region_uvs.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }

            att.triangles.reserve(jtri->Size());
            for (const JsonValue& t : jtri->GetArray())
            {
                att.triangles.push_back(static_cast<uint16_t>(t.GetUint()));
            }

            att.vertices.reserve(jverts->Size());
            for (const JsonValue& f : jverts->GetArray())
            {
                att.vertices.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }

            if (!reg)
            {
                return Fail("atlas: mesh 附件 '" + att.name + "'（槽位 '" +
                            out.slots[static_cast<size_t>(p.slot_index)].name + "'）找不到图集 region '" + region_name +
                            "'。json 里的 uvs 是图集空间坐标，没有图集变不成最终 UV。");
            }

            std::vector<float> final_uvs;
            ComputeMeshUvs(*reg, region_uvs, final_uvs);
            att.uvs = std::move(final_uvs);
            // 留给将来引用它的 linked mesh（它要拿父的 regionUVs 配自己的 region 重算）
            att.region_uvs = std::move(region_uvs);
        }
        else if (type == "path")
        {
            // PathAttachment（chunk 20，v11）。**不产生绘制项**，但 path 约束要沿它
            // 摆骨骼 ⇒ 必须烘焙几何。字段抄 SkeletonJson.cpp:588-604。
            //
            // ⚠️ `vertexCount` 是**点数**不是 float 数：参考实现把它 `<< 1` 之后
            //    才交给 readVertices（:598），而 lengths 用的是**没移位**的那个
            //    （:600 `vertexCount / 3`）。与二进制侧一致（SkeletonBinary.cpp:548-550
            //    直接把 varint 当点数传给 readVertices，lengths = 点数 / 3）。
            SpineSourcePathAttachment pa;
            pa.skin_index = p.skin_index;
            pa.slot_index = p.slot_index;
            pa.name = p.name;
            pa.closed = GetInt(a, "closed", 0) != 0;
            // ⚠️ 默认 **1**（:596）—— 与 offset 类字段的默认 0 不一样，记反了
            //    表现是"沿路径匀速 vs 按曲线参数匀速"，肉眼看不出来。
            pa.constant_speed = GetInt(a, "constantSpeed", 1) != 0;
            pa.vertex_count = GetInt(a, "vertexCount", 0);
            if (pa.vertex_count < 0)
            {
                return Fail("json: path 附件 '" + pa.name + "' 的 vertexCount 是负的");
            }
            const JsonValue* jverts = GetMember(a, "vertices");
            if (jverts == nullptr || !jverts->IsArray())
            {
                return Fail("json: path 附件 '" + pa.name + "' 缺 vertices");
            }
            std::vector<float> flat;
            flat.reserve(jverts->Size());
            for (const JsonValue& f : jverts->GetArray())
            {
                flat.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }
            // weighted 的判据照抄 json `readVertices`（:1208 `verticesLength == entrySize`）：
            // 顶点数组长度 == 点数 × 2 才是 rigid。
            const bool weighted = flat.size() != static_cast<size_t>(pa.vertex_count) * 2u;
            pa.weighted = weighted;
            std::string verr;
            if (!SplitPathVertices(flat, pa.vertex_count, weighted, pa.bones, pa.vertices, verr))
            {
                return Fail("json: path 附件 '" + pa.name + "': " + verr);
            }
            if (const JsonValue* jlen = GetMember(a, "lengths"); jlen != nullptr && jlen->IsArray())
            {
                pa.lengths.reserve(jlen->Size());
                for (const JsonValue& f : jlen->GetArray())
                {
                    pa.lengths.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
                }
            }
            // 参考实现永远写 vertexCount/3 条（setSize，多的截断、少的补 0），
            // 我们按声明的值校验 —— 少了说明对不上，多了是资产自己带的脏数据。
            if (pa.lengths.size() < static_cast<size_t>(pa.vertex_count) / 3u)
            {
                return Fail("json: path 附件 '" + pa.name + "' 的 lengths 只有 " +
                            std::to_string(pa.lengths.size()) + " 条，声明需要 " +
                            std::to_string(pa.vertex_count / 3) + " 条");
            }
            pa.lengths.resize(static_cast<size_t>(pa.vertex_count) / 3u);

            // deform 可以指向它（PathAttachment 也是 VertexAttachment）⇒ 也要进
            // "不渲染附件表"，否则 Bake 会把指向它的 deform 当成找不到附件而报错。
            SpineSourceNoDrawAttachment nd;
            nd.skin_index = p.skin_index;
            nd.slot_index = p.slot_index;
            nd.name = p.name;
            out.no_draw_attachments.push_back(std::move(nd));

            out.path_attachments.push_back(std::move(pa));
            continue;   // 不进 out.attachments —— 它不产生绘制项
        }
        else if (type == "boundingbox" || type == "point")
        {
            // 与二进制侧对齐（RecordNoDrawAttachment）：不烘焙，但 deform 指向它们
            // 是"没有顶点可改"而不是错误 ⇒ 记进不渲染附件表。
            SpineSourceNoDrawAttachment nd;
            nd.skin_index = p.skin_index;
            nd.slot_index = p.slot_index;
            nd.name = p.name;
            out.no_draw_attachments.push_back(std::move(nd));
            continue;
        }
        else
        {
            return Fail("json: 附件 '" + att.name + "' 的类型 '" + type + "' 不支持");
        }

        out.attachments.push_back(std::move(att));
    }

    // ---- 约束 setup（chunk 18，v11）----
    // 字段逐条抄 3.7 `SkeletonJson.cpp:255-422`；名字表与 setup **必须同序**
    // （动画里的时间轴按名字查下标，下标 = 它在数组里的位置）。
    //
    // 三处默认值要盯住，它们都是"记错就静默错姿势"的：
    //   1. `bendPositive` 是**布尔**，取反成 -1（:291）—— 不是 `bendDirection`。
    //   2. `mix` / 四个 `*:Mix` 默认 **1**（:290、:344-347、:418-419）。
    //   3. path 的三个 mode 串在**缺 key** 与**认不出的串**两种情况下落到的值
    //      **不一样**：缺 key 时 getString 的 fallback 是 "percent"/"length"/"tangent"
    //      ⇒ Percent(1)/Length(0)/Tangent(0)；认不出的串则三个 if 都不进，
    //      留在 PathConstraintData 构造里的默认 Fixed(0)/Length(0)/Tangent(0)
    //      （PathConstraintData.cpp:48-50）⇒ position 变成 Fixed。
    //      这是参考实现自己的不一致 ⇒ 我们不猜，**显式报错**。
    //
    // ⚠️ `_scale`：参考实现把 offsetX/offsetY 和 spacing 乘 `_scale`
    //    （:332-333、:410-417）。本解析器全程按 scale == 1 工作（bone 的
    //    length / x / y 也没乘，见上面 bones 段），所以这里同样不乘。
    {
        // 骨骼名数组 → 下标数组。参考实现查不到时**报错**（IK bone not found 等）。
        std::string bone_err;
        const auto read_bones = [&](const JsonValue& c, const char* what, const std::string& cname,
                                    std::vector<int>& dst) -> bool {
            const JsonValue* jb = GetMember(c, "bones");
            if (jb == nullptr || !jb->IsArray())
            {
                bone_err = "json: " + std::string(what) + " 约束 '" + cname + "' 缺 bones 数组";
                return false;
            }
            for (const JsonValue& bn : jb->GetArray())
            {
                const std::string bn_name = bn.IsString() ? std::string(bn.GetString()) : std::string();
                const int bi = FindBoneIndex(out.bones, bn_name);
                if (bi < 0)
                {
                    bone_err = "json: " + std::string(what) + " 约束 '" + cname + "' 引用了不存在的骨骼 '" +
                               bn_name + "'（参考实现在这里报 " + what + " bone not found）";
                    return false;
                }
                dst.push_back(bi);
            }
            if (dst.empty())
            {
                // 参考实现允许 0 条骨骼（循环不进），但那样的约束什么也不做 ——
                // 而"什么也不做"和"漏解析"在结果上无法区分 ⇒ 报错。
                bone_err = "json: " + std::string(what) + " 约束 '" + cname + "' 的 bones 是空的";
                return false;
            }
            return true;
        };

        if (const JsonValue* arr = GetMember(doc, "ik"); arr != nullptr && arr->IsArray())
        {
            for (const JsonValue& c : arr->GetArray())
            {
                SpineSourceIkConstraint k;
                k.name = GetString(c, "name", "");
                k.order = GetInt(c, "order", 0);
                if (!read_bones(c, "IK", k.name, k.bones))
                {
                    return Fail(bone_err);
                }
                const std::string tname = GetString(c, "target", "");
                k.target = FindBoneIndex(out.bones, tname);
                if (k.target < 0)
                {
                    return Fail("json: IK 约束 '" + k.name + "' 的目标骨骼 '" + tname +
                                "' 不存在（参考实现在这里报 Target bone not found）");
                }
                k.mix = GetFloat(c, "mix", 1.0f);
                k.bend_dir = GetInt(c, "bendPositive", 1) ? 1 : -1;
                k.compress = GetInt(c, "compress", 0) != 0;
                k.stretch = GetInt(c, "stretch", 0) != 0;
                k.uniform = GetInt(c, "uniform", 0) != 0;
                out.ik_names.push_back(k.name);
                out.iks.push_back(std::move(k));
            }
        }

        if (const JsonValue* arr = GetMember(doc, "transform"); arr != nullptr && arr->IsArray())
        {
            for (const JsonValue& c : arr->GetArray())
            {
                SpineSourceTransformConstraint t;
                t.name = GetString(c, "name", "");
                t.order = GetInt(c, "order", 0);
                if (!read_bones(c, "transform", t.name, t.bones))
                {
                    return Fail(bone_err);
                }
                const std::string tname = GetString(c, "target", "");
                t.target = FindBoneIndex(out.bones, tname);
                if (t.target < 0)
                {
                    return Fail("json: transform 约束 '" + t.name + "' 的目标骨骼 '" + tname +
                                "' 不存在（参考实现在这里报 Target bone not found）");
                }
                t.local = GetInt(c, "local", 0) != 0;
                t.relative = GetInt(c, "relative", 0) != 0;
                t.offset_rotation = GetFloat(c, "rotation", 0.0f);
                t.offset_x = GetFloat(c, "x", 0.0f);
                t.offset_y = GetFloat(c, "y", 0.0f);
                t.offset_scale_x = GetFloat(c, "scaleX", 0.0f);
                t.offset_scale_y = GetFloat(c, "scaleY", 0.0f);
                t.offset_shear_y = GetFloat(c, "shearY", 0.0f);
                t.rotate_mix = GetFloat(c, "rotateMix", 1.0f);
                t.translate_mix = GetFloat(c, "translateMix", 1.0f);
                t.scale_mix = GetFloat(c, "scaleMix", 1.0f);
                t.shear_mix = GetFloat(c, "shearMix", 1.0f);
                out.transform_names.push_back(t.name);
                out.transforms.push_back(std::move(t));
            }
        }

        if (const JsonValue* arr = GetMember(doc, "path"); arr != nullptr && arr->IsArray())
        {
            for (const JsonValue& c : arr->GetArray())
            {
                SpineSourcePathConstraint p;
                p.name = GetString(c, "name", "");
                p.order = GetInt(c, "order", 0);
                if (!read_bones(c, "path", p.name, p.bones))
                {
                    return Fail(bone_err);
                }
                // ⚠️ path 的 target 是**槽位**（findSlot，SkeletonJson.cpp:376）
                const std::string tname = GetString(c, "target", "");
                p.target_slot = FindSlotIndex(out.slots, tname);
                if (p.target_slot < 0)
                {
                    return Fail("json: path 约束 '" + p.name + "' 的目标**槽位** '" + tname +
                                "' 不存在（参考实现在这里报 Target slot not found）");
                }
                const std::string pm = GetString(c, "positionMode", "percent");
                if (pm == "fixed")
                {
                    p.position_mode = 0;
                }
                else if (pm == "percent")
                {
                    p.position_mode = 1;
                }
                else
                {
                    return Fail("json: path 约束 '" + p.name + "' 的 positionMode '" + pm +
                                "' 认不出（只认 fixed / percent）");
                }
                const std::string sm = GetString(c, "spacingMode", "length");
                if (sm == "length")
                {
                    p.spacing_mode = 0;
                }
                else if (sm == "fixed")
                {
                    p.spacing_mode = 1;
                }
                else if (sm == "percent")
                {
                    p.spacing_mode = 2;
                }
                else
                {
                    return Fail("json: path 约束 '" + p.name + "' 的 spacingMode '" + sm +
                                "' 认不出（只认 length / fixed / percent）");
                }
                const std::string rm = GetString(c, "rotateMode", "tangent");
                if (rm == "tangent")
                {
                    p.rotate_mode = 0;
                }
                else if (rm == "chain")
                {
                    p.rotate_mode = 1;
                }
                else if (rm == "chainScale")
                {
                    p.rotate_mode = 2;
                }
                else
                {
                    return Fail("json: path 约束 '" + p.name + "' 的 rotateMode '" + rm +
                                "' 认不出（只认 tangent / chain / chainScale）");
                }
                p.offset_rotation = GetFloat(c, "rotation", 0.0f);
                p.position = GetFloat(c, "position", 0.0f);
                p.spacing = GetFloat(c, "spacing", 0.0f);
                p.rotate_mix = GetFloat(c, "rotateMix", 1.0f);
                p.translate_mix = GetFloat(c, "translateMix", 1.0f);
                out.path_names.push_back(p.name);
                out.paths.push_back(std::move(p));
            }
        }
    }

    // ---- events（v16，SkeletonJson.cpp:673-694）：必须在 animations 之前（时间轴按名字找定义）----
    out.events.clear();
    if (const JsonValue* events = GetMember(doc, "events"))
    {
        if (!events->IsObject())
        {
            return Fail("json: events 不是对象");
        }
        for (auto it = events->MemberBegin(); it != events->MemberEnd(); ++it)
        {
            SpineSourceEventData e;
            e.name = it->name.GetString();
            if (e.name.empty())
            {
                return Fail("json: events 里有空名字的事件");
            }
            const JsonValue& v = it->value;
            e.int_value = GetInt(v, "int", 0);
            e.float_value = GetFloat(v, "float", 0.0f);
            e.string_value = GetString(v, "string", "");
            e.audio_path = GetString(v, "audio", "");
            // ⚠️ 参考实现是 `if (audioPath)` —— 指针非空。json 里写了 `"audio": ""` 时指针非空、
            //    串是空的，于是 volume / balance 照读；但事件派发时 `getAudioPath().isEmpty()` 为真。
            //    这里按"键存在"读，与它一致。
            if (GetMember(v, "audio") != nullptr)
            {
                e.volume = GetFloat(v, "volume", 1.0f);
                e.balance = GetFloat(v, "balance", 0.0f);
            }
            out.events.push_back(std::move(e));
        }
    }

    // ---- animations：整段烘焙（v9 chunk 11-16）----
    // deform **仍然**要单独收一份引用：它要落到附件上把绘制项标成 kFallback，
    // 而这件事必须等 linked mesh 解引用完才能做（deform 可以指向 linked mesh）。
    std::vector<SpineDeformRef> deform_refs;
    const JsonValue* anims = GetMember(doc, "animations");
    if (anims && anims->IsObject())
    {
        for (auto it = anims->MemberBegin(); it != anims->MemberEnd(); ++it)
        {
            const JsonValue& anim = it->value;
            const std::string anim_name(it->name.GetString());
            if (!anim.IsObject())
            {
                continue;
            }
            if (GetMember(anim, "drawOrder") || GetMember(anim, "draworder"))
            {
                out.has_draw_order_timeline = true;
            }
            if (const JsonValue* ev = GetMember(anim, "events"))
            {
                if (ev->IsArray() && ev->Size() > 0)
                {
                    out.has_event_timeline = true;   // 时间轴本身在 ParseSpineAnimationJson 里解析（v16）
                }
            }

            SpineSourceAnimation parsed;
            parsed.name = anim_name;
            const SpineImportResult r = ParseSpineAnimationJson(anim, anim_name, out, parsed);
            if (!r.ok)
            {
                return r;
            }
            for (const SpineSourceTimeline& tl : parsed.timelines)
            {
                if (tl.type != SpineTimelineType::kDeform)
                {
                    continue;
                }
                SpineDeformRef ref;
                ref.skin_index = tl.deform_skin;
                ref.slot_index = tl.deform_slot;
                ref.attachment = tl.deform_attachment;
                ref.skin_name = out.skins[static_cast<size_t>(tl.deform_skin)];
                ref.slot_name = out.slots[static_cast<size_t>(tl.deform_slot)].name;
                for (const SpineSourceDeformFrame& df : tl.deform_frames)
                {
                    const size_t need = static_cast<size_t>(df.start) + df.values.size();
                    if (need > ref.required_length)
                    {
                        ref.required_length = need;
                    }
                }
                deform_refs.push_back(std::move(ref));
            }
            out.animations.push_back(std::move(parsed));
        }
    }

    // ---- linked mesh：所有皮肤读完后才解引用父附件 ----
    const bool need_sort = !linked.empty();
    if (need_sort)
    {
        std::string err;
        if (!ResolveLinkedMeshes(atlas, out.skins, out.slots, out.attachments, linked, err))
        {
            return Fail("json: " + err);
        }
    }

    // ---- deform：linked mesh 解完之后才落，且必须在重排之前（传播要用 parent_index）----
    if (!deform_refs.empty())
    {
        std::string err;
        if (!ApplyDeformRefs(out.attachments, deform_refs, out.no_draw_attachments, err))
        {
            return Fail("json: " + err);
        }
    }

    if (need_sort)
    {
        // 解出来的附件插在末尾 ⇒ 重排一次，保证 (皮肤, 槽位) 有序。
        StableSortAttachmentsKeepParents(out.attachments,
                         [](const SpineSourceAttachment& a, const SpineSourceAttachment& b)
                         {
                             if (a.skin_index != b.skin_index)
                             {
                                 return a.skin_index < b.skin_index;
                             }
                             return a.slot_index < b.slot_index;
                         });
    }

    return Ok();
}

// =====================================================================
// 二进制 .skel（Spine 3.8）
// ---------------------------------------------------------------------
// 布局**逐字段照抄** `SkeletonBinary.cpp`，没有"我觉得应该是这样"的部分：
//   readSkeletonData :132-386   readSkin       :492-513   readAttachment :515-656
//   readVertices     :658-691   readVarint     :467-489   readString     :413-424
//   readInt/Float    :426-458   readFloatArray :693-706   readShortArray :708-717
//   readAnimation    :719-1110  readCurve      :1112-1127
//
// ⚠️ 四个踩了就**静默产出垃圾**的点：
//   1. 文件**没有魔数**，开头就是 hash 字符串 —— 传错文件不会立刻失败，
//      会顺着 varint 一路读飞。所以这里额外挡一道"开头是 '{'"。
//   2. `nonessential` 这一个 bool 会改变后面几乎每个结构的字段数。
//   3. region 在二进制里的字段顺序是 rotation,x,y,scaleX,scaleY,width,height，
//      和 json 的 x,y,width,height,rotation **不一样**。抄 json 那边的顺序
//      能编译、能跑，region 会整体错位/转错角度。
//   4. 事件时间轴每帧是否多 2 个 float，取决于**对应事件有没有 audioPath**
//      ⇒ 必须先把 events 段解析出来，不能跳过。
// =====================================================================
namespace
{

// 对应参考实现的 DataInput。每个 Read 都做越界检查：
// 二进制格式 + 无越界检查 = 崩溃，而且是取决于文件尾部内容的那种偶发崩溃。
struct SkelReader
{
    const uint8_t* data = nullptr;
    size_t len = 0;
    size_t pos = 0;
    std::string err;

    bool Fail(const std::string& m)
    {
        err = m;
        return false;
    }

    bool Have(size_t n) const
    {
        return n <= len && pos <= len - n;
    }

    bool FailEof(size_t n)
    {
        return Fail("skel: 偏移 " + std::to_string(pos) + " 处要读 " + std::to_string(n) + " 字节，但只剩 " +
                    std::to_string(len - pos) + " 字节");
    }

    bool Byte(uint8_t& v)
    {
        if (!Have(1))
        {
            return FailEof(1);
        }
        v = data[pos++];
        return true;
    }

    bool Skip(size_t n)
    {
        if (!Have(n))
        {
            return FailEof(n);
        }
        pos += n;
        return true;
    }

    // 照抄 :467-489。optimizePositive=false 时是 zigzag（有符号）。
    bool Varint(bool optimize_positive, int32_t& v)
    {
        uint8_t b = 0;
        if (!Byte(b))
        {
            return false;
        }
        uint32_t value = b & 0x7Fu;
        if (b & 0x80u)
        {
            if (!Byte(b))
            {
                return false;
            }
            value |= static_cast<uint32_t>(b & 0x7Fu) << 7;
            if (b & 0x80u)
            {
                if (!Byte(b))
                {
                    return false;
                }
                value |= static_cast<uint32_t>(b & 0x7Fu) << 14;
                if (b & 0x80u)
                {
                    if (!Byte(b))
                    {
                        return false;
                    }
                    value |= static_cast<uint32_t>(b & 0x7Fu) << 21;
                    if (b & 0x80u)
                    {
                        if (!Byte(b))
                        {
                            return false;
                        }
                        value |= static_cast<uint32_t>(b & 0x7Fu) << 28;
                    }
                }
            }
        }
        if (!optimize_positive)
        {
            // zigzag: (u >> 1) ^ -(u & 1)
            value = (value >> 1) ^ (0u - (value & 1u));
        }
        v = static_cast<int32_t>(value);
        return true;
    }

    bool Int32Be(int32_t& v)
    {
        uint8_t b[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i)
        {
            if (!Byte(b[i]))
            {
                return false;
            }
        }
        v = (static_cast<int32_t>(b[0]) << 24) | (static_cast<int32_t>(b[1]) << 16) |
            (static_cast<int32_t>(b[2]) << 8) | static_cast<int32_t>(b[3]);
        return true;
    }

    // 照抄 :426-435：大端 int32 再按位重解释成 float。
    bool Float(float& v)
    {
        int32_t i = 0;
        if (!Int32Be(i))
        {
            return false;
        }
        std::memcpy(&v, &i, sizeof(v));
        return true;
    }

    bool Boolean(bool& v)
    {
        uint8_t b = 0;
        if (!Byte(b))
        {
            return false;
        }
        v = b != 0;
        return true;
    }

    // 照抄 :413-424：varint 长度含结尾 '\0'，但结尾不落在文件里。长度 0 ⇒ 空串。
    bool String(std::string& out)
    {
        int32_t n = 0;
        if (!Varint(true, n))
        {
            return false;
        }
        out.clear();
        if (n == 0)
        {
            return true;
        }
        if (!Have(static_cast<size_t>(n) - 1))
        {
            return FailEof(static_cast<size_t>(n) - 1);
        }
        out.assign(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(n) - 1);
        pos += static_cast<size_t>(n) - 1;
        return true;
    }

    // 照抄 :693-706（scale 恒为 1）
    bool FloatArray(size_t n, std::vector<float>& out)
    {
        if (n > len)
        {
            return Fail("skel: 浮点数组声明了 " + std::to_string(n) + " 个元素，比整个文件还长");
        }
        if (!Have(n * 4))
        {
            return FailEof(n * 4);
        }
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            float f = 0.0f;
            if (!Float(f))
            {
                return false;
            }
            out[i] = f;
        }
        return true;
    }

    // 照抄 :708-717：元素数是 varint，每个元素是**大端** u16。
    bool ShortArray(std::vector<uint16_t>& out)
    {
        int32_t n = 0;
        if (!Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("skel: 短整型数组元素数为负 " + std::to_string(n));
        }
        const size_t cnt = static_cast<size_t>(n);
        if (cnt > len)
        {
            return Fail("skel: 短整型数组声明了 " + std::to_string(cnt) + " 个元素，比整个文件还长");
        }
        if (!Have(cnt * 2))
        {
            return FailEof(cnt * 2);
        }
        out.clear();
        out.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i)
        {
            uint8_t hi = 0;
            uint8_t lo = 0;
            if (!Byte(hi) || !Byte(lo))
            {
                return false;
            }
            out.push_back(static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | static_cast<uint16_t>(lo)));
        }
        return true;
    }
};

// 附件类型：照抄 AttachmentType.h:34-42
enum SkelAttachmentType
{
    kAttRegion = 0,
    kAttBoundingBox = 1,
    kAttMesh = 2,
    kAttLinkedMesh = 3,
    kAttPath = 4,
    kAttPoint = 5,
    kAttClipping = 6,
};

// 时间轴类型：照抄 SkeletonBinary.cpp:80-95
enum SkelTimelineType
{
    kBoneRotate = 0,
    kBoneTranslate = 1,
    kBoneScale = 2,
    kBoneShear = 3,
    kSlotAttachment = 0,
    kSlotColor = 1,
    kSlotTwoColor = 2,
    kPathPosition = 0,
    kPathSpacing = 1,
    kPathMix = 2,
};

constexpr uint8_t kCurveLinear = 0;
constexpr uint8_t kCurveStepped = 1;
constexpr uint8_t kCurveBezier = 2;

class SkelParser
{
public:
    SkelParser(const uint8_t* data, size_t len, const SpineAtlas* atlas, SpineSourceSkeleton& out)
        : atlas_(atlas)
        , out_(out)
    {
        r_.data = data;
        r_.len = len;
    }

    bool Run()
    {
        if (!ParseTopLevel())
        {
            return false;
        }
        // 与 json 版一致：去重（同皮肤内先出现的胜出）后按 (皮肤, 槽位) 排。
        std::stable_sort(collected_.begin(), collected_.end(), CompareAttachmentOrder);
        out_.attachments = std::move(collected_);

        // linked mesh：所有 skin 读完后才解引用父附件（父可能在后面的 skin 里）
        const bool need_resort = !linked_.empty();
        if (!linked_.empty())
        {
            std::string err;
            if (!ResolveLinkedMeshes(atlas_, out_.skins, out_.slots, out_.attachments, linked_, err))
            {
                return r_.Fail("skel: " + err);
            }
        }

        // deform：必须在 linked mesh 解完之后（deform 可以指向它），
        // 且必须在重排之前（inheritDeform 传播用的是 parent_index 下标）。
        if (!deform_refs_.empty())
        {
            std::string err;
            if (!ApplyDeformRefs(out_.attachments, deform_refs_, out_.no_draw_attachments, err))
            {
                return r_.Fail("skel: " + err);
            }
        }

        if (need_resort)
        {
            StableSortAttachmentsKeepParents(out_.attachments, CompareAttachmentOrder);
        }
        return true;
    }

    const std::string& error() const { return r_.err; }

private:
    SkelReader r_;
    const SpineAtlas* atlas_;
    SpineSourceSkeleton& out_;
    bool nonessential_ = false;
    std::vector<SpineSourceAttachment> collected_;
    std::vector<SpineLinkedMeshRef> linked_;
    // deform 时间轴命中的 (皮肤, 槽位, 附件名)。**动画段在最后**，而 deform 可以指向
    // linked mesh ⇒ 只能先收着，等 Run() 里 linked mesh 解完再落。
    std::vector<SpineDeformRef> deform_refs_;
    int current_skin_ = -1;   // 正在读的皮肤下标（附件 / linked mesh 都记它）
    std::vector<float> clip_vertices_;   // ReadVertices 的临时落地（裁剪附件用）

    // 附件顺序：先皮肤、皮肤内按槽位。与 json 路径**同一个**次序，
    // 两条路径烘焙出来的容器才能逐字节相同。
    static bool CompareAttachmentOrder(const SpineSourceAttachment& a, const SpineSourceAttachment& b)
    {
        if (a.skin_index != b.skin_index)
        {
            return a.skin_index < b.skin_index;
        }
        return a.slot_index < b.slot_index;
    }

    bool Fail(const std::string& m) { return r_.Fail("skel: " + m); }

    // 记下"解析到了但不烘焙"的附件。存在的唯一理由：deform 时间轴可以指向它们
    // （三者都是 VertexAttachment），而指向它们只是"没有绘制项可影响"，不是错误。
    // 见 `SpineSourceNoDrawAttachment` 的注释（本工程实测 88 处指向 path）。
    void RecordNoDrawAttachment(const std::string& name, int slot_index)
    {
        SpineSourceNoDrawAttachment s;
        s.skin_index = current_skin_;
        s.slot_index = slot_index;
        s.name = name;
        out_.no_draw_attachments.push_back(std::move(s));
    }

    bool ParseTopLevel()
    {
        std::string hash;
        std::string version;
        if (!r_.String(hash) || !r_.String(version))
        {
            return false;
        }
        // 只认 3.7.x / 3.8.x。3.7 与 3.8 的字节布局逐段比对完全一致，
        // 只有槽位暗色判据不同 ⇒ 下面按版本分支，不合并。
        {
            uint32_t runtime = 0;   // Bake 阶段由 `src.version` 重新算出，这里不用
            std::string verr;
            if (!ClassifySpineVersion(version, out_.version, runtime, verr))
            {
                return Fail(verr);
            }
        }
        out_.version_string = version;

        float width = 0.0f;
        float height = 0.0f;
        if (!r_.Float(width) || !r_.Float(height))
        {
            return false;
        }
        if (!r_.Boolean(nonessential_))
        {
            return false;
        }
        if (nonessential_)
        {
            // fps + imagesPath + audioPath（参考实现 :143-148 全部跳过）
            float fps = 0.0f;
            std::string images;
            std::string audio;
            if (!r_.Float(fps) || !r_.String(images) || !r_.String(audio))
            {
                return false;
            }
        }

        // ⚠️ 三类约束必须夹在 slots **之后**、skins **之前**（参考实现 :190-257
        //    就是在这个位置）—— 它们要拿已解析好的骨骼 / 槽位下标表。
        if (!ParseBones() || !ParseSlots() || !ParseIkConstraints() || !ParseTransformConstraints() ||
            !ParsePathConstraints())
        {
            return false;
        }

        // default skin + 具名 skin。注意 varint 读出来的就是**具名** skin 的个数
        // （参考实现 :277-292：default skin 在读到 count 之后才 +1）。
        //
        // ⚠️ 两者还有一个不对称：default skin 在 slotCount == 0 时**不占位**
        // （:279-281 直接 return NULL，:278 的 skinsCount 也因此没 +1），
        // 而具名 skin 即使空也占一个下标（:293-295 只是把它写成 NULL）。
        // 少这一条，后面**每一个** skin 下标都会整体偏移一格 ——
        // 换装会拿到隔壁皮肤的附件，而且不报错。
        if (!ParseSkin("default", false))
        {
            return false;
        }
        int32_t named_skin_count = 0;
        if (!r_.Varint(true, named_skin_count))
        {
            return false;
        }
        if (named_skin_count < 0)
        {
            return Fail("具名皮肤数为负 " + std::to_string(named_skin_count));
        }
        for (int32_t i = 0; i < named_skin_count; ++i)
        {
            std::string skin_name;
            if (!r_.String(skin_name))
            {
                return false;
            }
            if (!ParseSkin(skin_name, true))
            {
                return false;
            }
        }

        // 事件：必须先解析 —— 事件时间轴每帧多不多 2 个 float 取决于它。
        if (!ParseEvents())
        {
            return false;
        }

        int32_t anim_count = 0;
        if (!r_.Varint(true, anim_count))
        {
            return false;
        }
        for (int32_t i = 0; i < anim_count; ++i)
        {
            std::string anim_name;
            if (!r_.String(anim_name))
            {
                return false;
            }
            SpineSourceAnimation anim;
            anim.name = anim_name;
            if (!ReadAnimation(anim))
            {
                return false;
            }
            out_.animations.push_back(std::move(anim));
        }
        return true;
    }

    // ---- bones（:150-171）----
    bool ParseBones()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("骨骼数为负 " + std::to_string(n));
        }
        out_.bones.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i)
        {
            SpineSourceBone bone;
            if (!r_.String(bone.name))
            {
                return false;
            }
            bone.parent = -1;
            if (i != 0)
            {
                int32_t parent = 0;
                if (!r_.Varint(true, parent))
                {
                    return false;
                }
                // 二进制里父骨骼下标指向**已读过的**骨骼，所以必须 < i。
                if (parent < 0 || parent >= i)
                {
                    return Fail("骨骼 '" + bone.name + "' 的父下标 " + std::to_string(parent) +
                                " 非法（当前已读 " + std::to_string(i) + " 根，父骨骼必须排在自己之前）");
                }
                bone.parent = parent;
            }
            // 顺序抄 SkeletonBinary.cpp:157-164 —— 注意 **rotation 在最前**，
            // 与 json 的书写顺序（length, x, y, rotation, …）不一样；
            // 顺序错了不报错，只是每根骨骼摆位全错。
            // （x / y / length 在参考实现里还要乘 `_scale`，我们恒为 1。）
            if (!r_.Float(bone.rotation) || !r_.Float(bone.x) || !r_.Float(bone.y) ||
                !r_.Float(bone.scale_x) || !r_.Float(bone.scale_y) || !r_.Float(bone.shear_x) ||
                !r_.Float(bone.shear_y) || !r_.Float(bone.length))
            {
                return false;
            }
            int32_t transform_mode = 0;
            if (!r_.Varint(true, transform_mode))
            {
                return false;
            }
            // 参考实现是 `static_cast<TransformMode>(...)`（SkeletonBinary.cpp:165），
            // 越界值会被**静默**当成枚举外的数，落到 `Bone::updateWorldTransform` 的
            // switch 里没有 default ⇒ 那根骨骼的世界变换根本不更新（保持单位阵），
            // 且无任何报错。所以这里显式挡。
            if (transform_mode < 0 || transform_mode > static_cast<int32_t>(SpineTransformMode::kNoScaleOrReflection))
            {
                return Fail("骨骼 '" + bone.name + "' 的 transform 模式 " +
                            std::to_string(transform_mode) + " 越界（TransformMode 只有 0..4，"
                            "见 TransformMode.h:34-40；越界值在参考实现的 switch 里没有分支，"
                            "那根骨骼的世界变换会静默保持单位阵）");
            }
            bone.transform_mode = static_cast<SpineTransformMode>(transform_mode);
            if (nonessential_ && !r_.Skip(4))
            {
                return false;   // bone color
            }
            out_.bones.push_back(std::move(bone));
        }
        return true;
    }

    // ---- slots（:173-193）----
    bool ParseSlots()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("槽位数为负 " + std::to_string(n));
        }
        out_.slots.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i)
        {
            SpineSourceSlot slot;
            if (!r_.String(slot.name))
            {
                return false;
            }
            int32_t bone_index = 0;
            if (!r_.Varint(true, bone_index))
            {
                return false;
            }
            if (bone_index < 0 || static_cast<size_t>(bone_index) >= out_.bones.size())
            {
                return Fail("槽位 '" + slot.name + "' 引用了不存在的骨骼下标 " + std::to_string(bone_index));
            }
            slot.bone_index = bone_index;

            // ---- color（`SkeletonBinary.cpp:182` 的 readColor）----
            // ⚠️ 二进制**没有默认值**：4 个字节全是显式写入的（`SlotData` 的默认
            //    (1,1,1,1) 只在 json 路径里靠"字段不存在"体现）。所以这里必须原样读，
            //    不能"跳过" —— 之前就是 Skip(4)，setup 色因此全丢（v9 及更早的既有缺口）。
            uint8_t color[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k)
            {
                if (!r_.Byte(color[k]))
                {
                    return false;
                }
            }
            for (int k = 0; k < 4; ++k)
            {
                slot.color[k] = static_cast<float>(color[k]) / 255.0f;
            }
            // ⚠️ 暗色判据**分版本**，两边都是 4 个字节，但语义不同：
            //   * 3.7（`spine-runtimes` 3.7 `spine-csharp/src/SkeletonBinary.cs:190-192`）：
            //         int darkColor = ReadInt(input);
            //         if (darkColor != -1) slotData.hasSecondColor = true;
            //     —— 一个 int32，只有 -1（0xFFFFFFFF）表示"没有"。
            //   * 3.8（`spine-cpp/src/spine/SkeletonBinary.cpp:186-189`）：
            //         读 4 个字节，四个分量**不全是** 0xff 才算有暗色。
            //     —— 0xFFFFFF00（a=0）在 3.8 是"有"，在 3.7 也是"有"（!= -1）；
            //        0xFFFFFFFF 两边都是"无"。所以不能取巧合并成一种读法。
            uint8_t dark[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k)
            {
                if (!r_.Byte(dark[k]))
                {
                    return false;
                }
            }
            bool has_dark = false;
            if (out_.version == SpineSourceVersion::k37)
            {
                const uint32_t packed = static_cast<uint32_t>(dark[0]) |
                                        (static_cast<uint32_t>(dark[1]) << 8) |
                                        (static_cast<uint32_t>(dark[2]) << 16) |
                                        (static_cast<uint32_t>(dark[3]) << 24);
                has_dark = (packed != 0xFFFFFFFFu);
            }
            else
            {
                has_dark = !(dark[0] == 0xff && dark[1] == 0xff && dark[2] == 0xff && dark[3] == 0xff);
            }
            if (has_dark)
            {
                for (int k = 0; k < 3; ++k)
                {
                    slot.dark[k] = static_cast<float>(dark[k]) / 255.0f;
                }
                // ⚠️ alpha 写死 **1**（:188 `set(r/255, g/255, b/255, 1)`）；
                //    读进来的第 4 个字节只用来判"有没有暗色"，**从不作为 alpha 落地**。
                slot.dark[3] = 1.0f;
                slot.has_dark = true;
                out_.uses_tint_black = true;
            }
            if (!r_.String(slot.attachment_name))
            {
                return false;
            }
            int32_t blend_mode = 0;
            if (!r_.Varint(true, blend_mode))
            {
                return false;
            }
            // SkeletonBinary.cpp:191 直接 static_cast<BlendMode> —— 越界在参考实现里是个
            // 不存在的枚举值、走哪个分支看运气。这里显式报错。
            if (blend_mode < 0 || blend_mode > static_cast<int32_t>(SpineBlendMode::kScreen))
            {
                return Fail("槽位 '" + slot.name + "' 的混合模式 " + std::to_string(blend_mode) +
                            " 不在 0..3（normal / additive / multiply / screen）");
            }
            slot.blend_mode = static_cast<SpineBlendMode>(blend_mode);
            out_.slots.push_back(std::move(slot));
        }
        return true;
    }

    // ---- 约束 setup（chunk 18，v11）----
    // 字段顺序逐条抄 3.7 `SkeletonBinary.cpp:190-257`。
    //
    // ⚠️ 二进制**没有**默认值：每个字段都是显式写进文件的（与 json 相反）。
    //    两条路径 Bake 出来的容器必须逐字节相同，靠这条对拍钉住。
    //
    // ⚠️ 参考实现在这里是 `skeletonData->_bones[readVarint()]` **直接下标**
    //    （:200、:223、:247）—— 越界是 UB 不是报错。我们显式校验。
    bool ReadBoneIndexArray(std::vector<int>& out)
    {
        out.clear();
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("约束的 bones 数组长度是负的 " + std::to_string(n));
        }
        for (int32_t i = 0; i < n; ++i)
        {
            int32_t bi = 0;
            if (!r_.Varint(true, bi))
            {
                return false;
            }
            if (bi < 0 || bi >= static_cast<int32_t>(out_.bones.size()))
            {
                return Fail("约束引用的骨骼下标 " + std::to_string(bi) + " 越界（骨骼共 " +
                            std::to_string(out_.bones.size()) + " 根）。参考实现在这里是 "
                            "`_bones[readVarint()]` 直接下标 ⇒ 越界不报错，只是蒙皮乱掉。");
            }
            out.push_back(bi);
        }
        return true;
    }

    bool ReadBoneTarget(int& target_out, const std::string& what)
    {
        int32_t t = 0;
        if (!r_.Varint(true, t))
        {
            return false;
        }
        if (t < 0 || t >= static_cast<int32_t>(out_.bones.size()))
        {
            return Fail(what + " 的目标骨骼下标 " + std::to_string(t) + " 越界（骨骼共 " +
                        std::to_string(out_.bones.size()) + " 根）");
        }
        target_out = t;
        return true;
    }

    bool ParseIkConstraints()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("IK 约束条数是负的 " + std::to_string(n));
        }
        for (int32_t i = 0; i < n; ++i)
        {
            SpineSourceIkConstraint k;
            if (!r_.String(k.name) || !r_.Varint(true, k.order))
            {
                return false;
            }
            if (!ReadBoneIndexArray(k.bones))
            {
                return false;
            }
            if (k.bones.empty())
            {
                return Fail("skel: IK 约束 '" + k.name + "' 的 bones 是空的");
            }
            if (!ReadBoneTarget(k.target, "IK 约束 '" + k.name + "'"))
            {
                return false;
            }
            if (!r_.Float(k.mix))
            {
                return false;
            }
            // bendDirection 是 **signed char**，不是 varint（:203 `readSByte`）。
            // 与 IK 时间轴那处的处理保持一致（按位型重解释，见 :4207）。
            uint8_t bend_byte = 0;
            if (!r_.Byte(bend_byte))
            {
                return false;
            }
            const int bd = static_cast<int>(static_cast<int8_t>(bend_byte));
            if (bd != 1 && bd != -1)
            {
                return Fail("skel: IK 约束 '" + k.name + "' 的 bendDirection 是 " + std::to_string(bd) +
                            "（只可能是 1 / -1）");
            }
            k.bend_dir = bd;
            if (!r_.Boolean(k.compress) || !r_.Boolean(k.stretch) || !r_.Boolean(k.uniform))
            {
                return false;
            }
            out_.ik_names.push_back(k.name);
            out_.iks.push_back(std::move(k));
        }
        return true;
    }

    bool ParseTransformConstraints()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("transform 约束条数是负的 " + std::to_string(n));
        }
        for (int32_t i = 0; i < n; ++i)
        {
            SpineSourceTransformConstraint t;
            if (!r_.String(t.name) || !r_.Varint(true, t.order))
            {
                return false;
            }
            if (!ReadBoneIndexArray(t.bones))
            {
                return false;
            }
            if (t.bones.empty())
            {
                return Fail("skel: transform 约束 '" + t.name + "' 的 bones 是空的");
            }
            if (!ReadBoneTarget(t.target, "transform 约束 '" + t.name + "'"))
            {
                return false;
            }
            if (!r_.Boolean(t.local) || !r_.Boolean(t.relative))
            {
                return false;
            }
            if (!r_.Float(t.offset_rotation) || !r_.Float(t.offset_x) || !r_.Float(t.offset_y) ||
                !r_.Float(t.offset_scale_x) || !r_.Float(t.offset_scale_y) || !r_.Float(t.offset_shear_y) ||
                !r_.Float(t.rotate_mix) || !r_.Float(t.translate_mix) || !r_.Float(t.scale_mix) ||
                !r_.Float(t.shear_mix))
            {
                return false;
            }
            out_.transform_names.push_back(t.name);
            out_.transforms.push_back(std::move(t));
        }
        return true;
    }

    bool ParsePathConstraints()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("path 约束条数是负的 " + std::to_string(n));
        }
        for (int32_t i = 0; i < n; ++i)
        {
            SpineSourcePathConstraint p;
            if (!r_.String(p.name) || !r_.Varint(true, p.order))
            {
                return false;
            }
            if (!ReadBoneIndexArray(p.bones))
            {
                return false;
            }
            if (p.bones.empty())
            {
                return Fail("skel: path 约束 '" + p.name + "' 的 bones 是空的");
            }
            // ⚠️ target 是**槽位**下标（:249 `skeletonData->_slots[...]`）
            int32_t ts = 0;
            if (!r_.Varint(true, ts))
            {
                return false;
            }
            if (ts < 0 || ts >= static_cast<int32_t>(out_.slots.size()))
            {
                return Fail("skel: path 约束 '" + p.name + "' 的目标槽位下标 " + std::to_string(ts) +
                            " 越界（槽位共 " + std::to_string(out_.slots.size()) + " 个）");
            }
            p.target_slot = ts;
            if (!r_.Varint(true, p.position_mode) || !r_.Varint(true, p.spacing_mode) ||
                !r_.Varint(true, p.rotate_mode))
            {
                return false;
            }
            // 参考实现是 `static_cast<PositionMode>(readVarint())` —— 不检查范围，
            // 越界后 switch 走不到 ⇒ 约束静默不生效。我们报错。
            if (p.position_mode < 0 || p.position_mode > 1)
            {
                return Fail("skel: path 约束 '" + p.name + "' 的 positionMode 是 " +
                            std::to_string(p.position_mode) + "（只可能是 0 fixed / 1 percent）");
            }
            if (p.spacing_mode < 0 || p.spacing_mode > 2)
            {
                return Fail("skel: path 约束 '" + p.name + "' 的 spacingMode 是 " +
                            std::to_string(p.spacing_mode) + "（只可能是 0 length / 1 fixed / 2 percent）");
            }
            if (p.rotate_mode < 0 || p.rotate_mode > 2)
            {
                return Fail("skel: path 约束 '" + p.name + "' 的 rotateMode 是 " +
                            std::to_string(p.rotate_mode) + "（只可能是 0 tangent / 1 chain / 2 chainScale）");
            }
            if (!r_.Float(p.offset_rotation) || !r_.Float(p.position) || !r_.Float(p.spacing) ||
                !r_.Float(p.rotate_mix) || !r_.Float(p.translate_mix))
            {
                return false;
            }
            out_.path_names.push_back(p.name);
            out_.paths.push_back(std::move(p));
        }
        return true;
    }


    bool SkipFloats(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            float f = 0.0f;
            if (!r_.Float(f))
            {
                return false;
            }
        }
        return true;
    }

    // ---- skin（:492-513）----
    // reserve_index_when_empty：空 skin 要不要占一个下标（见调用处的注释）。
    bool ParseSkin(const std::string& skin_name, bool reserve_index_when_empty)
    {
        int32_t slot_count = 0;
        if (!r_.Varint(true, slot_count))
        {
            return false;
        }
        if (slot_count < 0)
        {
            return Fail("skin 的槽位数为负 " + std::to_string(slot_count));
        }
        if (slot_count == 0)
        {
            if (!reserve_index_when_empty)
            {
                return true;   // 参考实现直接返回 NULL，且不占下标
            }
            return AddSkin(skin_name);
        }
        if (!AddSkin(skin_name))
        {
            return false;
        }
        for (int32_t i = 0; i < slot_count; ++i)
        {
            int32_t slot_index = 0;
            if (!r_.Varint(true, slot_index))
            {
                return false;
            }
            if (slot_index < 0 || static_cast<size_t>(slot_index) >= out_.slots.size())
            {
                return Fail("skin 引用了不存在的槽位下标 " + std::to_string(slot_index));
            }
            int32_t att_count = 0;
            if (!r_.Varint(true, att_count))
            {
                return false;
            }
            for (int32_t ii = 0; ii < att_count; ++ii)
            {
                std::string name;
                if (!r_.String(name))
                {
                    return false;
                }
                if (name.empty())
                {
                    break;   // 参考实现 :500-503
                }
                if (!ParseAttachment(slot_index, name))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool AddSkin(const std::string& skin_name)
    {
        for (const std::string& s : out_.skins)
        {
            if (s == skin_name)
            {
                return Fail("重复的皮肤名 '" + skin_name + "'（换装按名字查，重名取哪个是不确定的）");
            }
        }
        out_.skins.push_back(skin_name);
        current_skin_ = static_cast<int>(out_.skins.size()) - 1;
        return true;
    }

    // ---- attachment（:515-656）----
    bool ParseAttachment(int32_t slot_index, const std::string& skin_attachment_name)
    {
        std::string name;
        if (!r_.String(name))
        {
            return false;
        }
        if (name.empty())
        {
            name = skin_attachment_name;
        }

        uint8_t type_byte = 0;
        if (!r_.Byte(type_byte))
        {
            return false;
        }

        switch (type_byte)
        {
            case kAttRegion:
            {
                std::string path;
                if (!r_.String(path))
                {
                    return false;
                }
                if (path.empty())
                {
                    path = name;
                }
                // ⚠️ 顺序和 json 不一样：rotation,x,y,scaleX,scaleY,width,height
                float rotation = 0.0f;
                float x = 0.0f;
                float y = 0.0f;
                float sx = 0.0f;
                float sy = 0.0f;
                float w = 0.0f;
                float h = 0.0f;
                if (!r_.Float(rotation) || !r_.Float(x) || !r_.Float(y) || !r_.Float(sx) || !r_.Float(sy) ||
                    !r_.Float(w) || !r_.Float(h))
                {
                    return false;
                }
                if (!r_.Skip(4))
                {
                    return false;   // color
                }

                SpineSourceAttachment att;
                att.type = SpineSourceAttachment::Type::kRegion;
                att.name = name;
                att.path = path;
                att.slot_index = slot_index;

                const SpineAtlasRegion* reg = LookupRegion(att, path);
                if (reg == nullptr)
                {
                    return false;
                }
                att.atlas_page = reg->page;

                float xy[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                ComputeRegionVertices(x, y, w, h, sx, sy, rotation, static_cast<float>(reg->width),
                                      static_cast<float>(reg->height), static_cast<float>(reg->offset_x),
                                      static_cast<float>(reg->offset_y), static_cast<float>(reg->orig_w),
                                      static_cast<float>(reg->orig_h), xy);
                att.vertices.assign(xy, xy + 8);

                float uv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                ComputeRegionUvs(*reg, uv);
                att.uvs.assign(uv, uv + 8);

                // Spine 的 region 三角形固定 BL,UL,UR / UR,BR,BL
                att.triangles = {0, 1, 2, 2, 3, 0};
                Collect(std::move(att));
                return true;
            }
            case kAttMesh:
            {
                std::string path;
                if (!r_.String(path))
                {
                    return false;
                }
                if (path.empty())
                {
                    path = name;
                }
                if (!r_.Skip(4))
                {
                    return false;   // color
                }
                int32_t vertex_count = 0;
                if (!r_.Varint(true, vertex_count))
                {
                    return false;
                }
                if (vertex_count < 0)
                {
                    return Fail("mesh 附件 '" + name + "' 顶点数为负");
                }

                std::vector<float> region_uvs;
                if (!r_.FloatArray(static_cast<size_t>(vertex_count) * 2u, region_uvs))
                {
                    return false;
                }
                std::vector<uint16_t> triangles;
                if (!r_.ShortArray(triangles))
                {
                    return false;
                }
                std::vector<float> vertices;
                if (!ReadVertices(vertex_count, &vertices))
                {
                    return false;
                }
                int32_t hull = 0;
                if (!r_.Varint(true, hull))
                {
                    return false;
                }
                if (nonessential_)
                {
                    std::vector<uint16_t> edges;
                    if (!r_.ShortArray(edges) || !SkipFloats(2))
                    {
                        return false;   // edges, width, height
                    }
                }

                SpineSourceAttachment att;
                att.type = SpineSourceAttachment::Type::kMesh;
                att.name = name;
                att.path = path;
                att.slot_index = slot_index;
                att.triangles = std::move(triangles);
                att.vertices = std::move(vertices);

                const SpineAtlasRegion* reg = LookupRegion(att, path);
                if (reg == nullptr)
                {
                    return false;
                }
                att.atlas_page = reg->page;
                std::vector<float> final_uvs;
                ComputeMeshUvs(*reg, region_uvs, final_uvs);
                att.uvs = std::move(final_uvs);
                // 留给将来引用它的 linked mesh（它要拿父的 regionUVs 配自己的 region 重算）
                att.region_uvs = std::move(region_uvs);
                Collect(std::move(att));
                return true;
            }
            case kAttClipping:
            {
                // 顺序照抄参考实现（SkeletonBinary.cpp:636-646）：先 endSlotIndex
                // 再 vertexCount。⚠️ json 那边是"名字"，这边是"下标"。
                int32_t end_slot = 0;
                int32_t vertex_count = 0;
                if (!r_.Varint(true, end_slot) || !r_.Varint(true, vertex_count))
                {
                    return false;
                }
                // 与参考实现**同一条**边界检查（:641-645 "parse clip attachment failed!"）。
                // 它是唯一一处对裁剪作用域做校验的地方 —— 越界就意味着作用域算不出来，
                // 不是"可选信息"，必须报错而不是当成"裁到末尾"。
                if (end_slot < 0 || static_cast<size_t>(end_slot) >= out_.slots.size())
                {
                    return Fail("裁剪附件 '" + name + "' 的 endSlot 下标 " + std::to_string(end_slot) +
                                " 越界（槽位数 " + std::to_string(out_.slots.size()) +
                                "）；参考实现在这里报 parse clip attachment failed!");
                }
                // 裁剪多边形本身也要读 —— 它是**静态资产几何**（局部坐标 + 骨骼绑定），
                // 与 deform 那种逐帧动画数据不同，必须进容器（规格 §7.2）。
                // ⚠️ 二进制有**显式**的 weighted 标志，比 json 那边"靠数组长度猜"可靠。
                bool weighted = false;
                if (!ReadVertices(vertex_count, &clip_vertices_, &weighted))
                {
                    return false;
                }
                if (nonessential_ && !r_.Skip(4))
                {
                    return false;
                }
                // 裁剪附件本身不产生几何 ⇒ 不进 attachments（也就没有绘制项）。
                // 它留下的是**作用域** + **多边形**，Bake 拿它们把作用域内的绘制项
                // 标成 kFallback 并指到这条多边形上。
                SpineSourceClip clip;
                clip.skin_index = current_skin_;
                clip.slot_index = slot_index;
                clip.name = name;
                clip.end_slot_index = end_slot;
                clip.vertices = std::move(clip_vertices_);
                clip_vertices_.clear();
                clip.vertex_count = vertex_count;
                clip.weighted = weighted;
                out_.clips.push_back(clip);
                out_.has_clipping = true;
                // 裁剪附件**没有绘制项**（它只留下作用域 + 多边形），且作用域内的绘制项
                // 本来就走 CPU fallback ⇒ 指向它的 deform 由那条路径处理，不是错误。
                RecordNoDrawAttachment(name, slot_index);
                return true;
            }
            case kAttBoundingBox:
            {
                int32_t vertex_count = 0;
                if (!r_.Varint(true, vertex_count))
                {
                    return false;
                }
                if (!ReadVertices(vertex_count, nullptr))
                {
                    return false;
                }
                if (nonessential_ && !r_.Skip(4))
                {
                    return false;
                }
                RecordNoDrawAttachment(name, slot_index);
                return true;   // 包围盒不渲染
            }
            case kAttPath:
            {
                // PathAttachment（chunk 20，v11）。**不产生绘制项**，但 path 约束要
                // 沿它摆骨骼 ⇒ 必须烘焙几何。字段抄 SkeletonBinary.cpp:544-558。
                SpineSourcePathAttachment pa;
                pa.skin_index = current_skin_;
                pa.slot_index = slot_index;
                pa.name = name;
                if (!r_.Boolean(pa.closed) || !r_.Boolean(pa.constant_speed))
                {
                    return false;
                }
                int32_t vertex_count = 0;
                if (!r_.Varint(true, vertex_count))
                {
                    return false;
                }
                if (vertex_count < 0)
                {
                    return Fail("path 附件 '" + name + "' 顶点数为负");
                }
                pa.vertex_count = vertex_count;

                // ⚠️ 二进制的 weighted 是**显式**的 bool（json 那边只能靠数组长度猜），
                //    用这个更可靠的答案（ReadVertices 的 weighted_out）。
                std::vector<float> flat;
                if (!ReadVertices(vertex_count, &flat, &pa.weighted))
                {
                    return false;
                }
                std::string verr;
                if (!SplitPathVertices(flat, vertex_count, pa.weighted, pa.bones, pa.vertices, verr))
                {
                    return Fail("skel: path 附件 '" + name + "': " + verr);
                }

                // lengths：条数 = 点数 / 3（:550）—— 与 json 侧同一个式子。
                // ⚠️ 这里是 `vertex_count / 3`，**不是** `2 * vertex_count / 3`：
                //    json 那边 readVertices 收到的是 `vertexCount << 1`（float 数），
                //    而 lengths 用的是没移位的点数；二进制那边 readVertices 收的
                //    直接就是点数。两条路径落到的条数一致，靠对拍钉住。
                pa.lengths.resize(static_cast<size_t>(vertex_count) / 3u);
                for (size_t li = 0; li < pa.lengths.size(); ++li)
                {
                    if (!r_.Float(pa.lengths[li]))
                    {
                        return false;
                    }
                }
                if (nonessential_ && !r_.Skip(4))
                {
                    return false;   // color
                }
                // deform 可以指向它（PathAttachment 也是 VertexAttachment）
                RecordNoDrawAttachment(name, slot_index);
                out_.path_attachments.push_back(std::move(pa));
                return true;   // path 不渲染
            }
            case kAttPoint:
            {
                if (!SkipFloats(3))
                {
                    return false;   // rotation, x, y
                }
                if (nonessential_ && !r_.Skip(4))
                {
                    return false;
                }
                RecordNoDrawAttachment(name, slot_index);
                return true;   // point 不渲染
            }
            case kAttLinkedMesh:
            {
                // 参考实现 :584-604：path, color, skin, parent, inheritDeform,
                // [nonessential: width, height]
                std::string path;
                if (!r_.String(path))
                {
                    return false;
                }
                if (path.empty())
                {
                    path = name;
                }
                if (!r_.Skip(4))
                {
                    return false;   // color
                }
                SpineLinkedMeshRef lm;
                lm.slot_index = slot_index;
                lm.skin_index = current_skin_;
                lm.name = name;
                lm.path = path;
                if (!r_.String(lm.skin) || !r_.String(lm.parent))
                {
                    return false;
                }
                bool inherit_deform = false;
                if (!r_.Boolean(inherit_deform))
                {
                    return false;
                }
                // json 那边同一个开关写在字段 "deform" 里（SkeletonJson.cpp:572），
                // 二进制是一个 bool（:594）。父附件的 deform 是否作用于继承方由它决定
                // （MeshAttachment::applyDeform，:94-95）⇒ 导入期要用。
                lm.inherit_deform = inherit_deform;
                if (nonessential_ && !SkipFloats(2))
                {
                    return false;   // width, height
                }
                linked_.push_back(std::move(lm));
                return true;
            }
            default:
            {
                return Fail("附件 '" + name + "' 的附件类型是 " + std::to_string(static_cast<int>(type_byte)) +
                            "，3.8 只到 6（clipping）");
            }
        }
    }

    // readVertices（:658-691）。out 为 nullptr 时只推进游标（跳过）。
    // 扁平编码与 json 版一致：weighted 时每顶点 [n, (boneIdx, x, y, w) × n]。
    // weighted_out 非空时把那个 bool 传出去 —— json 那边只能靠"数组长度猜"，
    // 二进制这边是**显式**的，调用方要拿这个更可靠的答案。
    bool ReadVertices(int32_t vertex_count, std::vector<float>* out, bool* weighted_out = nullptr)
    {
        if (vertex_count < 0)
        {
            return Fail("顶点数为负 " + std::to_string(vertex_count));
        }
        bool weighted = false;
        if (!r_.Boolean(weighted))
        {
            return false;
        }
        if (weighted_out != nullptr)
        {
            *weighted_out = weighted;
        }
        if (!weighted)
        {
            std::vector<float> tmp;
            if (!r_.FloatArray(static_cast<size_t>(vertex_count) * 2u, tmp))
            {
                return false;
            }
            if (out != nullptr)
            {
                *out = std::move(tmp);
            }
            return true;
        }
        if (out != nullptr)
        {
            out->clear();
        }
        for (int32_t i = 0; i < vertex_count; ++i)
        {
            int32_t bone_count = 0;
            if (!r_.Varint(true, bone_count))
            {
                return false;
            }
            if (bone_count <= 0)
            {
                return Fail("第 " + std::to_string(i) + " 个顶点的影响数是 " + std::to_string(bone_count) +
                            "，但这一层已被标成 weighted");
            }
            if (out != nullptr)
            {
                out->push_back(static_cast<float>(bone_count));
            }
            for (int32_t ii = 0; ii < bone_count; ++ii)
            {
                int32_t bone_index = 0;
                if (!r_.Varint(true, bone_index))
                {
                    return false;
                }
                // 3.8 的 _bones 是 unsigned short（VertexAttachment.h:79-81）。
                // 参考实现在这里是 assert —— 我们报错，不静默截断。
                if (bone_index < 0 || bone_index > 65535)
                {
                    return Fail("骨骼下标 " + std::to_string(bone_index) +
                                " 超出 3.8 的 unsigned short 范围，截断会直接毁掉蒙皮");
                }
                float x = 0.0f;
                float y = 0.0f;
                float w = 0.0f;
                if (!r_.Float(x) || !r_.Float(y) || !r_.Float(w))
                {
                    return false;
                }
                if (out != nullptr)
                {
                    out->push_back(static_cast<float>(bone_index));
                    out->push_back(x);
                    out->push_back(y);
                    out->push_back(w);
                }
            }
        }
        return true;
    }

    const SpineAtlasRegion* LookupRegion(const SpineSourceAttachment& att, const std::string& path)
    {
        const std::string region_name = path.empty() ? att.name : path;
        const SpineAtlasRegion* reg = (atlas_ != nullptr) ? FindAtlasRegion(*atlas_, region_name) : nullptr;
        if (reg == nullptr)
        {
            r_.err = "skel: atlas: 附件 '" + att.name + "'（槽位 '" +
                     out_.slots[static_cast<size_t>(att.slot_index)].name + "'）找不到图集 region '" + region_name +
                     "'。缺图集就算不出 UV —— 不拿零 UV 糊过去。";
        }
        return reg;
    }

    void Collect(SpineSourceAttachment att)
    {
        att.skin_index = current_skin_;
        for (const SpineSourceAttachment& existing : collected_)
        {
            if (existing.skin_index == att.skin_index && existing.slot_index == att.slot_index &&
                existing.name == att.name)
            {
                return;   // 同一皮肤内先出现的胜出
            }
        }
        collected_.push_back(std::move(att));
    }

    // ---- events（:327-347）----
    bool ParseEvents()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        if (n < 0)
        {
            return Fail("事件数为负 " + std::to_string(n));
        }
        out_.events.clear();
        out_.events.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i)
        {
            SpineSourceEventData e;
            if (!r_.String(e.name) || !r_.Varint(false, e.int_value) || !r_.Float(e.float_value) ||
                !r_.String(e.string_value) || !r_.String(e.audio_path))
            {
                return false;
            }
            if (e.name.empty())
            {
                return Fail("事件 " + std::to_string(i) + " 的名字为空（参考实现报 Event name is null or empty）");
            }
            const bool has_audio = !e.audio_path.empty();
            if (has_audio && (!r_.Float(e.volume) || !r_.Float(e.balance)))
            {
                return false;
            }
            out_.events.push_back(std::move(e));
        }
        return true;
    }

    // ---- animation：整段烘焙（v9 chunk 11-16）----
    // 字段顺序照抄 SkeletonBinary::readAnimation（:719-1107）。与 json 那边产出
    // **同一个** SpineSourceTimeline，后面的 Bake 完全不知道数据来自哪条路径。
    bool ReadAnimation(SpineSourceAnimation& anim)
    {
        // 槽位时间轴（:725-793）
        int32_t slot_n = 0;
        if (!r_.Varint(true, slot_n))
        {
            return false;
        }
        for (int32_t i = 0; i < slot_n; ++i)
        {
            int32_t slot_index = 0;
            int32_t tl_n = 0;
            if (!r_.Varint(true, slot_index) || !r_.Varint(true, tl_n))
            {
                return false;
            }
            for (int32_t ii = 0; ii < tl_n; ++ii)
            {
                uint8_t t = 0;
                if (!r_.Byte(t))
                {
                    return false;
                }
                int32_t frames = 0;
                if (!r_.Varint(true, frames))
                {
                    return false;
                }
                if (frames < 0)
                {
                    return Fail("槽位时间轴帧数为负 " + std::to_string(frames));
                }
                if (t == kSlotAttachment)
                {
                    // 每帧 (time, attachmentName)，**没有** curve
                    SpineSourceTimeline tl;
                    tl.type = SpineTimelineType::kAttachment;
                    tl.target = static_cast<int>(slot_index);
                    for (int32_t f = 0; f < frames; ++f)
                    {
                        float time = 0.0f;
                        std::string s;
                        if (!r_.Float(time) || !r_.String(s))
                        {
                            return false;
                        }
                        tl.frames.push_back(time);
                        tl.attachment_names.push_back(s);
                    }
                    TrimCurves(tl);
                    anim.timelines.push_back(std::move(tl));
                }
                else if (t == kSlotColor || t == kSlotTwoColor)
                {
                    SpineSourceTimeline tl;
                    tl.type = (t == kSlotTwoColor) ? SpineTimelineType::kTwoColor : SpineTimelineType::kColor;
                    tl.target = static_cast<int>(slot_index);
                    for (int32_t f = 0; f < frames; ++f)
                    {
                        float time = 0.0f;
                        if (!r_.Float(time))
                        {
                            return false;
                        }
                        tl.frames.push_back(time);
                        // 二进制里颜色是 **int**（大端 rgba）：主色 4 字节 + 暗色 4 字节
                        // （暗色只有 rgb 有效，:772-775）。
                        int32_t c0_be = 0;
                        if (!r_.Int32Be(c0_be))
                        {
                            return false;
                        }
                        const uint32_t c0 = static_cast<uint32_t>(c0_be);
                        tl.frames.push_back(static_cast<float>((c0 >> 24) & 0xFFu) / 255.0f);
                        tl.frames.push_back(static_cast<float>((c0 >> 16) & 0xFFu) / 255.0f);
                        tl.frames.push_back(static_cast<float>((c0 >> 8) & 0xFFu) / 255.0f);
                        tl.frames.push_back(static_cast<float>(c0 & 0xFFu) / 255.0f);
                        if (t == kSlotTwoColor)
                        {
                            int32_t c1_be = 0;
                            if (!r_.Int32Be(c1_be))
                            {
                                return false;
                            }
                            const uint32_t c1 = static_cast<uint32_t>(c1_be);
                            // ⚠️ 暗色是 0x00rrggbb：只有 **3** 个分量（:772-775）
                            tl.frames.push_back(static_cast<float>((c1 >> 16) & 0xFFu) / 255.0f);
                            tl.frames.push_back(static_cast<float>((c1 >> 8) & 0xFFu) / 255.0f);
                            tl.frames.push_back(static_cast<float>(c1 & 0xFFu) / 255.0f);
                        }
                        SpineSourceCurve c;
                        if (f < frames - 1 && !Curve(&c))
                        {
                            return false;
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                    anim.timelines.push_back(std::move(tl));
                }
                else
                {
                    return Fail("槽位时间轴类型 " + std::to_string(static_cast<int>(t)) + " 未知（3.8 只到 2）");
                }
            }
        }

        // 骨骼时间轴（:795-852）
        int32_t bone_n = 0;
        if (!r_.Varint(true, bone_n))
        {
            return false;
        }
        for (int32_t i = 0; i < bone_n; ++i)
        {
            int32_t bone_index = 0;
            int32_t tl_n = 0;
            if (!r_.Varint(true, bone_index) || !r_.Varint(true, tl_n))
            {
                return false;
            }
            for (int32_t ii = 0; ii < tl_n; ++ii)
            {
                uint8_t t = 0;
                if (!r_.Byte(t))
                {
                    return false;
                }
                int32_t frames = 0;
                if (!r_.Varint(true, frames))
                {
                    return false;
                }
                if (frames < 0)
                {
                    return Fail("骨骼时间轴帧数为负 " + std::to_string(frames));
                }
                // rotate 是 (time, degrees)；translate / scale / shear 是 (time, x, y)
                const int values = (t == kBoneRotate) ? 1 : 2;
                if (t > kBoneShear)
                {
                    return Fail("骨骼时间轴类型 " + std::to_string(static_cast<int>(t)) + " 未知（3.8 只到 3）");
                }
                SpineSourceTimeline tl;
                tl.type = (t == kBoneRotate) ? SpineTimelineType::kRotate
                          : (t == kBoneTranslate) ? SpineTimelineType::kTranslate
                          : (t == kBoneScale) ? SpineTimelineType::kScale
                                              : SpineTimelineType::kShear;
                tl.target = static_cast<int>(bone_index);
                for (int32_t f = 0; f < frames; ++f)
                {
                    std::vector<float> v;
                    if (!r_.FloatArray(static_cast<size_t>(1 + values), v))
                    {
                        return false;
                    }
                    for (float x : v)
                    {
                        tl.frames.push_back(x);
                    }
                    SpineSourceCurve c;
                    if (f < frames - 1 && !Curve(&c))
                    {
                        return false;
                    }
                    tl.curves.push_back(c);
                }
                TrimCurves(tl);
                anim.timelines.push_back(std::move(tl));
            }
        }

        // IK 约束时间轴（:854-873）：每帧 time,mix,sbyte,bool,bool
        int32_t ik_n = 0;
        if (!r_.Varint(true, ik_n))
        {
            return false;
        }
        for (int32_t i = 0; i < ik_n; ++i)
        {
            int32_t index = 0;
            int32_t frames = 0;
            if (!r_.Varint(true, index) || !r_.Varint(true, frames))
            {
                return false;
            }
            if (frames < 0)
            {
                return Fail("IK 时间轴帧数为负 " + std::to_string(frames));
            }
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kIkConstraint;
            tl.target = static_cast<int>(index);
            for (int32_t f = 0; f < frames; ++f)
            {
                std::vector<float> v;
                if (!r_.FloatArray(2, v))
                {
                    return false;
                }
                uint8_t bend_byte = 0;
                bool compress = false;
                bool stretch = false;
                if (!r_.Byte(bend_byte) || !r_.Boolean(compress) || !r_.Boolean(stretch))
                {
                    return false;
                }
                // bendDirection 在二进制里是 **signed char**（:863）⇒ 按位型重解释。
                const int8_t bend = static_cast<int8_t>(bend_byte);
                tl.frames.push_back(v[0]);   // time
                tl.frames.push_back(v[1]);   // mix
                tl.frames.push_back(static_cast<float>(bend));
                tl.frames.push_back(compress ? 1.0f : 0.0f);
                tl.frames.push_back(stretch ? 1.0f : 0.0f);
                SpineSourceCurve c;
                if (f < frames - 1 && !Curve(&c))
                {
                    return false;
                }
                tl.curves.push_back(c);
            }
            TrimCurves(tl);
            anim.timelines.push_back(std::move(tl));
        }

        // transform 约束时间轴（:875-894）：每帧 5 个 float
        int32_t tc_n = 0;
        if (!r_.Varint(true, tc_n))
        {
            return false;
        }
        for (int32_t i = 0; i < tc_n; ++i)
        {
            int32_t index = 0;
            int32_t frames = 0;
            if (!r_.Varint(true, index) || !r_.Varint(true, frames))
            {
                return false;
            }
            if (frames < 0)
            {
                return Fail("transform 约束时间轴帧数为负 " + std::to_string(frames));
            }
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kTransformConstraint;
            tl.target = static_cast<int>(index);
            for (int32_t f = 0; f < frames; ++f)
            {
                std::vector<float> v;
                if (!r_.FloatArray(5, v))
                {
                    return false;
                }
                for (float x : v)
                {
                    tl.frames.push_back(x);
                }
                SpineSourceCurve c;
                if (f < frames - 1 && !Curve(&c))
                {
                    return false;
                }
                tl.curves.push_back(c);
            }
            TrimCurves(tl);
            anim.timelines.push_back(std::move(tl));
        }

        // path 约束时间轴（:896-955）：类型字段是 **sbyte**
        int32_t pc_n = 0;
        if (!r_.Varint(true, pc_n))
        {
            return false;
        }
        for (int32_t i = 0; i < pc_n; ++i)
        {
            int32_t index = 0;
            int32_t tl_n = 0;
            if (!r_.Varint(true, index) || !r_.Varint(true, tl_n))
            {
                return false;
            }
            for (int32_t ii = 0; ii < tl_n; ++ii)
            {
                uint8_t t = 0;
                if (!r_.Byte(t))
                {
                    return false;
                }
                int32_t frames = 0;
                if (!r_.Varint(true, frames))
                {
                    return false;
                }
                if (frames < 0)
                {
                    return Fail("path 约束时间轴帧数为负 " + std::to_string(frames));
                }
                int values = 0;
                SpineTimelineType type = SpineTimelineType::kPathConstraintPosition;
                if (t == kPathPosition || t == kPathSpacing)
                {
                    values = 1;
                    type = (t == kPathSpacing) ? SpineTimelineType::kPathConstraintSpacing
                                               : SpineTimelineType::kPathConstraintPosition;
                }
                else if (t == kPathMix)
                {
                    values = 2;
                    type = SpineTimelineType::kPathConstraintMix;
                }
                else
                {
                    return Fail("path 约束时间轴类型 " + std::to_string(static_cast<int>(t)) + " 未知（3.8 只到 2）");
                }
                SpineSourceTimeline tl;
                tl.type = type;
                tl.target = static_cast<int>(index);
                for (int32_t f = 0; f < frames; ++f)
                {
                    std::vector<float> v;
                    if (!r_.FloatArray(static_cast<size_t>(1 + values), v))
                    {
                        return false;
                    }
                    for (float x : v)
                    {
                        tl.frames.push_back(x);
                    }
                    SpineSourceCurve c;
                    if (f < frames - 1 && !Curve(&c))
                    {
                        return false;
                    }
                    tl.curves.push_back(c);
                }
                TrimCurves(tl);
                anim.timelines.push_back(std::move(tl));
            }
        }

        // ---- deform 时间轴（:957-1030）----
        // 以前这里是"精确跳过"。现在要收 (皮肤, 槽位, 附件名)：被 FFD 动到的附件
        // 烘不进静态 VB，得在容器里标成 CPU fallback（规格 §7.1）。
        int32_t def_n = 0;
        if (!r_.Varint(true, def_n))
        {
            return false;
        }
        if (def_n < 0)
        {
            return Fail("deform 时间轴数为负 " + std::to_string(def_n));
        }
        for (int32_t i = 0; i < def_n; ++i)
        {
            int32_t skin_index = 0;
            int32_t slot_n = 0;
            if (!r_.Varint(true, skin_index) || !r_.Varint(true, slot_n))
            {
                return false;
            }
            // 参考实现 `skeletonData->_skins[skinIndex]`（:959）**不检查**下标 ⇒
            // 越界读。我们要报错。
            if (skin_index < 0 || static_cast<size_t>(skin_index) >= out_.skins.size())
            {
                return Fail("deform 时间轴引用了不存在的皮肤下标 " + std::to_string(skin_index) + "（皮肤数是 " +
                            std::to_string(out_.skins.size()) + "）—— 参考实现在这里是越界读");
            }
            if (slot_n < 0)
            {
                return Fail("deform 时间轴的槽位数为负 " + std::to_string(slot_n));
            }
            for (int32_t ii = 0; ii < slot_n; ++ii)
            {
                int32_t slot_index = 0;
                int32_t att_n = 0;
                if (!r_.Varint(true, slot_index) || !r_.Varint(true, att_n))
                {
                    return false;
                }
                if (slot_index < 0 || static_cast<size_t>(slot_index) >= out_.slots.size())
                {
                    return Fail("deform 时间轴引用了不存在的槽位下标 " + std::to_string(slot_index) + "（槽位数是 " +
                                std::to_string(out_.slots.size()) + "）");
                }
                if (att_n < 0)
                {
                    return Fail("deform 时间轴的附件数为负 " + std::to_string(att_n));
                }
                for (int32_t iii = 0; iii < att_n; ++iii)
                {
                    std::string att_name;
                    if (!r_.String(att_name))
                    {
                        return false;
                    }
                    int32_t frames = 0;
                    if (!r_.Varint(true, frames))
                    {
                        return false;
                    }
                    if (frames < 0)
                    {
                        return Fail("deform 时间轴帧数为负 " + std::to_string(frames));
                    }

                    SpineDeformRef ref;
                    ref.skin_index = static_cast<int>(skin_index);
                    ref.slot_index = static_cast<int>(slot_index);
                    ref.attachment = att_name;
                    ref.skin_name = out_.skins[static_cast<size_t>(skin_index)];
                    ref.slot_name = out_.slots[static_cast<size_t>(slot_index)].name;

                    SpineSourceTimeline tl;
                    tl.type = SpineTimelineType::kDeform;
                    tl.deform_skin = static_cast<int>(skin_index);
                    tl.deform_slot = static_cast<int>(slot_index);
                    tl.deform_attachment = att_name;

                    for (int32_t f = 0; f < frames; ++f)
                    {
                        float time = 0.0f;
                        int32_t end = 0;
                        if (!r_.Float(time) || !r_.Varint(true, end))
                        {
                            return false;
                        }
                        tl.frames.push_back(time);
                        SpineSourceDeformFrame df;
                        if (end != 0)
                        {
                            int32_t start = 0;
                            if (!r_.Varint(true, start))
                            {
                                return false;
                            }
                            if (end < 0)
                            {
                                return Fail("deform 帧长度 " + std::to_string(end) + " 为负");
                            }
                            if (start < 0)
                            {
                                return Fail("deform 帧 offset " + std::to_string(start) + " 为负");
                            }
                            // 读端是 `end += start` 再读 end-start 个 float ⇒
                            // **落盘的第二个 varint 是"个数"不是"终点"**。
                            const size_t need = static_cast<size_t>(start) + static_cast<size_t>(end);
                            if (need > ref.required_length)
                            {
                                ref.required_length = need;
                            }
                            df.start = static_cast<uint32_t>(start);
                            if (!r_.FloatArray(static_cast<size_t>(end), df.values))
                            {
                                return false;
                            }
                        }
                        tl.deform_frames.push_back(std::move(df));
                        SpineSourceCurve c;
                        if (f < frames - 1 && !Curve(&c))
                        {
                            return false;
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                    deform_refs_.push_back(std::move(ref));
                    anim.timelines.push_back(std::move(tl));
                }
            }
        }

        // ---- 绘制顺序时间轴（:1032-1077）：这条决定资产要不要整体走 CPU ----
        int32_t draw_order_count = 0;
        if (!r_.Varint(true, draw_order_count))
        {
            return false;
        }
        if (draw_order_count < 0)
        {
            return Fail("绘制顺序时间轴帧数为负 " + std::to_string(draw_order_count));
        }
        if (draw_order_count > 0)
        {
            out_.has_draw_order_timeline = true;
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kDrawOrder;
            tl.target = 0;
            for (int32_t i = 0; i < draw_order_count; ++i)
            {
                float time = 0.0f;
                int32_t offset_count = 0;
                if (!r_.Float(time) || !r_.Varint(true, offset_count))
                {
                    return false;
                }
                if (offset_count < 0)
                {
                    return Fail("绘制顺序帧的 offset 数为负 " + std::to_string(offset_count));
                }
                tl.frames.push_back(time);
                SpineSourceDrawOrderFrame dof;
                for (int32_t ii = 0; ii < offset_count; ++ii)
                {
                    int32_t slot = 0;
                    int32_t off = 0;
                    if (!r_.Varint(true, slot) || !r_.Varint(true, off))
                    {
                        return false;
                    }
                    // 参考实现 `drawOrder[index + offset] = ...`（:1059）不检查下标 ⇒
                    // 越界写。我们要报错 —— 绘制顺序写飞了不报错，只是层级乱掉。
                    if (slot < 0 || static_cast<size_t>(slot) >= out_.slots.size())
                    {
                        return Fail("绘制顺序帧引用了不存在的槽位下标 " + std::to_string(slot) +
                                    "（槽位数是 " + std::to_string(out_.slots.size()) + "）");
                    }
                    dof.slots.push_back(static_cast<uint32_t>(slot));
                    dof.offsets.push_back(off);
                }
                tl.draw_order_frames.push_back(std::move(dof));
            }
            TrimCurves(tl);
            anim.timelines.push_back(std::move(tl));
        }

        // 事件时间轴（:1079-1107）。⚠️ 末尾那 2 个 float 取决于对应事件有没有 audioPath。
        // v16 起**烘焙**：time 进帧、其余进 SpineSourceEventKey（Bake 再落成 24 B 载荷）。
        int32_t ev_n = 0;
        if (!r_.Varint(true, ev_n))
        {
            return false;
        }
        if (ev_n < 0)
        {
            return Fail("事件时间轴帧数为负 " + std::to_string(ev_n));
        }
        if (ev_n > 0)
        {
            out_.has_event_timeline = true;
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kEvent;
            tl.target = 0;
            for (int32_t i = 0; i < ev_n; ++i)
            {
                float time = 0.0f;
                int32_t event_index = 0;
                SpineSourceEventKey key;
                bool free_string = false;
                if (!r_.Float(time) || !r_.Varint(true, event_index))
                {
                    return false;
                }
                if (event_index < 0 || static_cast<size_t>(event_index) >= out_.events.size())
                {
                    return Fail("事件时间轴引用了不存在的事件下标 " + std::to_string(event_index) + "（事件数是 " +
                                std::to_string(out_.events.size()) + "）");
                }
                const SpineSourceEventData& data = out_.events[static_cast<size_t>(event_index)];
                key.event = static_cast<uint32_t>(event_index);
                if (!r_.Varint(false, key.int_value) || !r_.Float(key.float_value) || !r_.Boolean(free_string))
                {
                    return false;
                }
                // freeString = 这一帧自带字符串；否则用定义的字符串（:1091-1093）
                if (free_string)
                {
                    if (!r_.String(key.string_value))
                    {
                        return false;
                    }
                }
                else
                {
                    key.string_value = data.string_value;
                }
                // 只有定义带音频时才有逐帧 volume / balance；否则保持 Event 构造函数的 1 / 0
                if (!data.audio_path.empty() && (!r_.Float(key.volume) || !r_.Float(key.balance)))
                {
                    return false;
                }
                tl.frames.push_back(time);
                tl.event_keys.push_back(std::move(key));
            }
            anim.timelines.push_back(std::move(tl));
        }
        return true;
    }

    // readCurve（:1112-1127）：只有 bezier 带 4 个 float。
    // `out` 非空时把曲线**收下来**（v9 起动画要烘焙）；否则只推进游标。
    //
    // ⚠️ 参考实现的 switch **没有 default** ⇒ 认不出的字节**静默当成 linear**
    //    （连 `CURVE_LINEAR` 都没写 case）。我们要报错：认不出的曲线字节意味着
    //    "这段插值方式与 spine 里不同"，静默当 linear 会让动作悄悄变样。
    bool Curve(SpineSourceCurve* out = nullptr)
    {
        uint8_t t = 0;
        if (!r_.Byte(t))
        {
            return false;
        }
        if (t == kCurveBezier)
        {
            if (out != nullptr)
            {
                out->type = SpineCurveType::kBezier;
                std::vector<float> v;
                if (!r_.FloatArray(4, v))
                {
                    return false;
                }
                out->cx1 = v[0];
                out->cy1 = v[1];
                out->cx2 = v[2];
                out->cy2 = v[3];
                return true;
            }
            return SkipFloats(4);
        }
        if (t == kCurveLinear || t == kCurveStepped)
        {
            if (out != nullptr)
            {
                out->type = (t == kCurveStepped) ? SpineCurveType::kStepped : SpineCurveType::kLinear;
            }
            return true;
        }
        return Fail("curve 类型 " + std::to_string(static_cast<int>(t)) +
                    " 未知（3.8 只到 2）—— 参考实现在这里静默当成 linear");
    }
};

}   // namespace

SpineImportResult ParseSpineSkeletonBinary(const uint8_t* data, size_t len, const SpineAtlas* atlas,
                                           SpineSourceSkeleton& out)
{
    out.bones.clear();
    out.slots.clear();
    out.attachments.clear();
    out.skins.clear();
    out.has_draw_order_timeline = false;
    out.uses_tint_black = false;
    out.has_clipping = false;
    // ⚠️ 约束与"不画的附件"也**必须**清：不清的话同一个 SpineSourceSkeleton
    //    被复用第二次会攒下重复条目，而重复条目不报错（只是约束多跑一遍、
    //    path 附件多一份几何）。json 那边一开始就清了，这里补齐。
    out.iks.clear();
    out.transforms.clear();
    out.paths.clear();
    out.path_attachments.clear();
    out.no_draw_attachments.clear();

    if (atlas != nullptr)
    {
        out.atlas_pages.clear();
        out.atlas_pages.reserve(atlas->pages.size());
        for (const SpineAtlasPage& p : atlas->pages)
        {
            out.atlas_pages.push_back(p.name);
        }
    }

    if (data == nullptr || len == 0)
    {
        return Fail("skel: 数据为空");
    }
    // .skel **没有魔数**，开头就是 hash 字符串。json 传进来不会立刻失败，
    // 而是顺着 varint 读飞 ⇒ 这里挡一道，给人能看懂的错误。
    if (data[0] == '{')
    {
        return Fail("skel: 开头是 '{'，看起来是 json —— 请改用 ParseSpineSkeletonJson()");
    }

    SkelParser parser(data, len, atlas, out);
    if (!parser.Run())
    {
        return Fail(parser.error());
    }
    return Ok();
}

// =====================================================================
// 烘焙：切成 GPU 布局
// =====================================================================
namespace
{

// 权重 → unorm8，且**和必须恰好 255**（规格 §3.1）。
// 余量补给权重最大的那个；补不进去就报错，绝不静默。
bool QuantizeWeights(const float* w, int n, uint8_t* out8, std::string& err)
{
    float sum = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        sum += w[i];
    }
    if (!(sum > 0.0f))
    {
        err = "权重和为 0";
        return false;
    }

    int acc = 0;
    int max_i = 0;
    for (int i = 0; i < n; ++i)
    {
        int q = static_cast<int>(std::lround(w[i] / sum * 255.0f));
        q = std::max(0, std::min(255, q));
        out8[i] = static_cast<uint8_t>(q);
        acc += q;
        if (q > out8[max_i])
        {
            max_i = i;
        }
    }

    const int diff = 255 - acc;
    const int fixed = static_cast<int>(out8[max_i]) + diff;
    if (fixed < 0 || fixed > 255)
    {
        err = "量化后权重和为 " + std::to_string(acc) + "，无法调整到 255";
        return false;
    }
    out8[max_i] = static_cast<uint8_t>(fixed);
    return true;
}

// 顶点块 / UV 块的去重池：内容**逐字节完全相同**才共用。
//
// 为什么敢共用顶点：共用的是"几何 + 蒙皮"这一段字节，它只决定形状；
// 而槽位骨骼（rigid 用）、UV、皮肤全在**绘制项**上，不在这段字节里
// ⇒ 两个绘制项共用同一段顶点，各自的骨骼 / UV / 皮肤仍然各算各的，不会互相污染。
//
// 为什么只做完全相同、绝不做"近似"：近似就是"我觉得这两个差不多"，
// 差一点点形状就静默错，而且没有任何报错。宁可少共享，不可错共享。
// （哈希只用来**缩小比较范围**；判定始终是逐字节比较 —— 哈希撞了也不会出错。）
//
// 起点单位是**条目**（顶点块 → 顶点；UV 块 → UV 条目），不是字节。
class ExactBlobPool
{
public:
    explicit ExactBlobPool(uint32_t unit_size) : unit_size_(unit_size) {}

    // 返回 {是否命中已有块, 起点}。没命中时块已被追加进池子。
    std::pair<bool, uint32_t> Intern(const uint8_t* bytes, size_t n)
    {
        const uint64_t h = Fnv1a(bytes, n);
        const auto it = by_hash_.find(h);
        if (it != by_hash_.end())
        {
            for (uint32_t base : it->second)
            {
                const size_t at = static_cast<size_t>(base) * unit_size_;
                if (at + n <= data_.size() && std::memcmp(data_.data() + at, bytes, n) == 0)
                {
                    return {true, base};
                }
            }
        }
        // 调用方只传"整条目"（vc × stride），所以这里一定是整数倍。
        const uint32_t base = unit_count_;
        data_.insert(data_.end(), bytes, bytes + n);
        unit_count_ += static_cast<uint32_t>(n) / unit_size_;
        by_hash_[h].push_back(base);
        return {false, base};
    }

    uint32_t unit_count() const { return unit_count_; }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    static uint64_t Fnv1a(const uint8_t* p, size_t n)
    {
        uint64_t h = 1469598103934665603ull;   // FNV-1a 64 位偏移基数
        for (size_t i = 0; i < n; ++i)
        {
            h ^= static_cast<uint64_t>(p[i]);
            h *= 1099511628211ull;
        }
        return h;
    }

    uint32_t unit_size_ = 1;
    uint32_t unit_count_ = 0;
    std::vector<uint8_t> data_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_hash_;
};

// 把 spine 的扁平顶点数组编码成容器顶点块（规格 §3.1）。
// **裁剪多边形与 mesh 顶点共用这一条路径** —— 裁剪附件是 VertexAttachment，
// 顶点跟 mesh 顶点是同一个东西（局部坐标 + 骨骼绑定），没有理由另写一份。
//   rigid    → SpineRigidVertex    8 B，vertices 就是 [x,y, ...]
//   weighted → SpineWeightedVertex 8 B（**只是引用**）+ 影响流，
//              vertices 是每顶点 [n, (boneIdx,x,y,w) × n]
//
// ⚠️ weighted 是**两段式**：
//     * `influence_out`（可选）收 (bindPos 8 B + bone 4 B)/影响，**本地**下标从 0 起；
//     * `blob` 里的 influence_offset 也先写**本地**下标 —— 调用方拿去重后的
//       块起点把它改写成全局下标（见 BakeSpineGpuAsset 里的 FixupInfluenceOffsets）。
//   分成两段是因为：不先对影响流去重，两个几何相同的附件就会拿到不同的
//   influence_offset ⇒ 顶点块的字节不再相同 ⇒ **顶点去重整个失效**（这才是
//   GPU-Spine 省内存的那一下）。所以影响流必须先按"逐字节相同"去重。
bool EncodeVertices(const std::string& what, const std::vector<float>& vertices, uint32_t vertex_count,
                    bool weighted, uint32_t bone_count, std::vector<uint8_t>& blob, std::string& err,
                    std::vector<uint8_t>* influence_out = nullptr)
{
    blob.clear();
    if (influence_out)
    {
        influence_out->clear();
    }
    if (weighted)
    {
        // 展开 spine 的扁平顶点数组：
        //   每顶点 [n, (boneIdx, x, y, w) × n]   —— SkeletonJson::readVertices
        size_t p = 0;
        uint32_t local_offset = 0;
        for (uint32_t v = 0; v < vertex_count; ++v)
        {
            if (p >= vertices.size())
            {
                err = what + " 的 vertices 在第 " + std::to_string(v) + " 个顶点处提前结束";
                return false;
            }

            const float nf = vertices[p++];
            const int n = static_cast<int>(nf);
            if (n <= 0 || n > static_cast<int>(SpineGpuConstants::kMaxInfluences))
            {
                err = what + " 第 " + std::to_string(v) + " 个顶点有 " + std::to_string(n) +
                      " 个影响，超过 MAX_INFLUENCES=" + std::to_string(SpineGpuConstants::kMaxInfluences) +
                      "。不静默截断 —— 截断会让形状悄悄错。";
                return false;
            }
            if (p + static_cast<size_t>(n) * 4u > vertices.size())
            {
                err = what + " 的 vertices 在第 " + std::to_string(v) + " 个顶点处越界";
                return false;
            }

            // 变长：按实际影响数开，不是 kMaxInfluences（那只是"数据坏了"的护栏）。
            std::vector<float> w(static_cast<size_t>(n), 0.0f);
            std::vector<uint8_t> w8(static_cast<size_t>(n), 0u);
            std::vector<uint16_t> idx(static_cast<size_t>(n), 0u);
            std::vector<float> bind(static_cast<size_t>(n) * 2u, 0.0f);

            for (int k = 0; k < n; ++k)
            {
                const float bone_f = vertices[p];
                const float vx = vertices[p + 1];
                const float vy = vertices[p + 2];
                const float ww = vertices[p + 3];
                p += 4;

                // 3.8 的 _bones 是 unsigned short，截断会直接毁掉蒙皮
                if (bone_f < 0.0f || bone_f > 65535.0f || static_cast<uint32_t>(bone_f) >= bone_count)
                {
                    err = what + " 第 " + std::to_string(v) + " 个顶点引用骨骼 " +
                          std::to_string(static_cast<int>(bone_f)) + "，超出范围（bone_count=" +
                          std::to_string(bone_count) + "）";
                    return false;
                }

                idx[static_cast<size_t>(k)] = static_cast<uint16_t>(bone_f);
                w[static_cast<size_t>(k)] = ww;
                bind[static_cast<size_t>(k) * 2u] = vx;
                bind[static_cast<size_t>(k) * 2u + 1u] = vy;
            }

            if (!QuantizeWeights(w.data(), n, w8.data(), err))
            {
                err = what + " 第 " + std::to_string(v) + " 个顶点：" + err;
                return false;
            }

            if (influence_out)
            {
                for (int k = 0; k < n; ++k)
                {
                    // 一条 = bindPos(2 float) + bone 记录(4 B)，共 kInfluenceEntrySize。
                    // 落盘时再拆成 chunk 21 / 22 —— 这里并在一起只是为了去重有个唯一的键。
                    const size_t at = influence_out->size();
                    influence_out->resize(at + SpineGpuConstants::kInfluenceEntrySize, 0u);
                    std::memcpy(influence_out->data() + at, &bind[static_cast<size_t>(k) * 2u], 2u * sizeof(float));
                    SpineInfluenceBone ib {};
                    ib.bone_idx = idx[static_cast<size_t>(k)];
                    ib.weight = w8[static_cast<size_t>(k)];
                    ib.pad = 0;
                    std::memcpy(influence_out->data() + at + SpineGpuConstants::kInfluenceBindStride, &ib,
                                sizeof(ib));
                }
            }

            SpineWeightedVertex out_v {};
            out_v.influence_offset = local_offset;   // 本地下标，调用方再叠加块起点
            out_v.influence_count = static_cast<uint32_t>(n);
            local_offset += static_cast<uint32_t>(n);
            blob.resize(blob.size() + sizeof(out_v));
            std::memcpy(blob.data() + blob.size() - sizeof(out_v), &out_v, sizeof(out_v));
        }
        return true;
    }

    // rigid：⚠️ 这里要**显式**查顶点数 × 2 是否超出 —— 原来直接 `vertices[v*2]`
    // 是越界读（附件判成 rigid 但 vertices 比 uvs 短时就会踩到）。
    if (static_cast<size_t>(vertex_count) * 2u > vertices.size())
    {
        err = what + " 需要 " + std::to_string(vertex_count * 2u) + " 个 float，但只有 " +
              std::to_string(vertices.size()) + " 个（rigid 是每顶点 2 个）";
        return false;
    }
    for (uint32_t v = 0; v < vertex_count; ++v)
    {
        SpineRigidVertex out_v {};
        out_v.bind_pos[0] = vertices[static_cast<size_t>(v) * 2u];
        out_v.bind_pos[1] = vertices[static_cast<size_t>(v) * 2u + 1u];
        blob.resize(blob.size() + sizeof(out_v));
        std::memcpy(blob.data() + blob.size() - sizeof(out_v), &out_v, sizeof(out_v));
    }
    return true;
}

// 把顶点块里的 influence_offset 从"附件内本地下标"改写成"影响流里的全局下标"。
//
// 调用顺序**必须**是：先 `influence_pool.Intern(影响流)` 拿到 base → 再本函数
// 改写顶点块 → 最后才 `weighted_pool.Intern(顶点块)`。
// 颠倒的后果不报错：先改写顶点块的话，两个相同附件的本地下标都是 0 ⇒ 顶点
// 去重看着"命中"了，但它们指的其实是**各自**那段影响流 ⇒ 其中一个会取到
// 另一份的 bindPos，形状静默错。
void FixupInfluenceOffsets(std::vector<uint8_t>& blob, uint32_t base)
{
    const size_t n = blob.size() / SpineGpuConstants::kWeightedVertexStride;
    for (size_t v = 0; v < n; ++v)
    {
        SpineWeightedVertex wv {};
        std::memcpy(&wv, blob.data() + v * SpineGpuConstants::kWeightedVertexStride, sizeof(wv));
        wv.influence_offset += base;
        std::memcpy(blob.data() + v * SpineGpuConstants::kWeightedVertexStride, &wv, sizeof(wv));
    }
}

// 影响流里的"本地下标 → 全局下标"改写，同时把一条 12 B 的影响拆成两条流。
// 拆开是必须的：`ByteAddressBuffer::Load2` 要求 8 B 对齐，12 B 步长会让
// 第 1、3、5…条的 bindPos 落在 12、36…（不是 8 的倍数）⇒ 只能退化成标量 Load。
void SplitInfluenceStream(const std::vector<uint8_t>& combined, std::vector<float>& bind_out,
                          std::vector<SpineInfluenceBone>& bone_out)
{
    const size_t count = combined.size() / SpineGpuConstants::kInfluenceEntrySize;
    bind_out.resize(count * 2u);
    bone_out.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t* p = combined.data() + i * SpineGpuConstants::kInfluenceEntrySize;
        std::memcpy(bind_out.data() + i * 2u, p, SpineGpuConstants::kInfluenceBindStride);
        std::memcpy(&bone_out[i], p + SpineGpuConstants::kInfluenceBindStride,
                    sizeof(SpineInfluenceBone));
    }
}

}   // namespace

// =====================================================================
// 动画烘焙（chunk 11-16）
// =====================================================================

void AppendU32To(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
}

void AppendF32To(std::vector<uint8_t>& v, float x)
{
    uint32_t u = 0;
    std::memcpy(&u, &x, sizeof(u));
    AppendU32To(v, u);
}

// 字符串表的落盘顺序与偏移计算。**只有这一处**实现 ——
//   * Bake      拿它算 chunk 11 的 name_offset 与 chunk 16 里的附件名偏移
//   * Write     拿它拼 chunk 1 的字节
// 两处共用一份实现，才不会出现"写端按 A 顺序、Bake 按 B 顺序算偏移"的静默错位。
//
// 顺序固定：骨骼 → 皮肤 → 槽位 → **槽位 setup 附件名** → 附件 → 图集页 →
//            **动画名** → **path 附件名**（v11）→ **裁剪附件名**（v15）→ **动画附件名**。
// 裁剪附件名按 chunk 9 的条目数切分（一条多边形一个名字，**不去重**）。
// 前六组按 header 里的计数切分（槽位 setup 附件名按 slot_count 切），
// 第七组按 chunk 11 的条目数切分，第八组按 chunk 20 的条目数切分
// （一条 path 附件一个名字，**不去重**）；
// 最后一组**只按偏移访问**（attachment 时间轴存的是偏移不是下标）⇒ 不需要计数，
// 也正因如此它**必须排在最后** —— 排在它后面的组读端切不出来。
// ⚠️ v10 插进来的"槽位 setup 附件名"每组都是 slot_count 条 —— 空名也要占一条
//    （写一个 '\0'）⇒ 条数恒定，读端才能靠 header.slot_count 切准。
void BuildSpineStringTable(const SpineGpuAsset& a, std::vector<uint8_t>& blob,
                           std::vector<uint32_t>& slot_setup_attachment_offsets,
                           std::vector<uint32_t>& anim_name_offsets,
                           std::vector<uint32_t>& anim_attachment_offsets,
                           std::vector<uint32_t>& path_attachment_name_offsets,
                           std::vector<uint32_t>& clip_name_offsets)
{
    blob.clear();
    clip_name_offsets.clear();
    slot_setup_attachment_offsets.clear();
    anim_name_offsets.clear();
    anim_attachment_offsets.clear();
    path_attachment_name_offsets.clear();

    const auto group = [&](const std::vector<std::string>& names, std::vector<uint32_t>* offsets) {
        for (const std::string& s : names)
        {
            if (offsets != nullptr)
            {
                offsets->push_back(static_cast<uint32_t>(blob.size()));
            }
            blob.insert(blob.end(), s.begin(), s.end());
            blob.push_back(0u);
        }
    };

    group(a.bone_names, nullptr);
    group(a.skin_names, nullptr);
    group(a.slot_names, nullptr);
    group(a.slot_setup_attachment_names, &slot_setup_attachment_offsets);  // v10：chunk 17 引用
    group(a.attachment_names, nullptr);
    group(a.atlas_page_names, nullptr);
    group(a.anim_names, &anim_name_offsets);
    // ⚠️ path 附件名必须排在"动画附件名"**前面**：后者是"剩下的全归它"，
    //    没有计数 ⇒ 排在它后面的组读端切不出来。
    group(a.path_attachment_names, &path_attachment_name_offsets);   // v11：chunk 20 引用
    group(a.clip_names, &clip_name_offsets);                         // v15：chunk 9 引用
    group(a.anim_attachment_names, &anim_attachment_offsets);
}

// 往 out 里追加一个字符串并返回它的**下标**（不去重时下标 = size()-1）。
uint32_t InternString(std::vector<std::string>& pool, const std::string& s)
{
    for (size_t i = 0; i < pool.size(); ++i)
    {
        if (pool[i] == s)
        {
            return static_cast<uint32_t>(i);
        }
    }
    pool.push_back(s);
    return static_cast<uint32_t>(pool.size() - 1u);
}

// ---------------------------------------------------------------------
// 约束 setup + 执行顺序（chunk 18 / 19 / 20，v11）
// ---------------------------------------------------------------------
// 这一段是 `Skeleton::updateCache()`（3.7 `Skeleton.cpp:134-178`）的**转录**，
// 连同它调用的 sort* 系列（:537-650）。逐字照搬的理由见 chunk 19 的注释：
// 运行时再实现一遍 = 给自己多一次写反的机会，而写反的表现是"姿势略偏"。
//
// ⚠️ 三处"参考实现静默、我们报错"：
//   1. `order` 不是 0..N-1 的排列 ⇒ 参考实现的 `goto continue_outer` 按 i 逐个找
//      order == i 的那条，找不到就**跳过**（约束静默不生效）。
//      order 为负时 `int == size_t` 的比较会把负数变成天文数字 ⇒ 同样静默丢。
//   2. `_updateCache.contains(child)` 依赖"这条骨骼此前被加过"，是**序列**的函数；
//      转录反了不报错，只是"要不要复位"的答案反了（表现是动作拖影）。
//   3. path 约束的曲线来自槽位**当前**挂的 PathAttachment
//      （`PathConstraint::update()`，PathConstraint.cpp:77），而"当前"取决于皮肤
//      ⇒ 只按槽位找会在换装后拿到别的皮肤的曲线。所以要落 skin_index。
namespace
{
constexpr uint32_t kMaxU16Index = 0xFFFFu;

// `Skeleton::sortBone()`（:642-648）+ `sortReset()`（:645-651）的状态机。
// `_sorted` 与 `_updateCache` 都是**这个对象**的，逐字段对上。
struct UpdateOrderBuilder
{
    const SpineSourceSkeleton& src;
    std::vector<std::vector<uint32_t>> children;   // 每根骨骼的子骨骼（由 parent 反推）
    std::vector<uint8_t> sorted;
    std::vector<SpineGpuUpdateStep> cache;         // = `_updateCache`
    std::vector<uint32_t> reset;                   // = `_updateCacheReset`

    explicit UpdateOrderBuilder(const SpineSourceSkeleton& s) : src(s)
    {
        children.resize(src.bones.size());
        for (size_t i = 0; i < src.bones.size(); ++i)
        {
            const int p = src.bones[i].parent;
            if (p >= 0 && static_cast<size_t>(p) < src.bones.size())
            {
                children[static_cast<size_t>(p)].push_back(static_cast<uint32_t>(i));
            }
        }
        sorted.assign(src.bones.size(), 0u);
    }

    bool Contains(uint32_t bone) const
    {
        for (const SpineGpuUpdateStep& st : cache)
        {
            if (st.kind == SpineGpuUpdateKind::kUpdateBone && st.index == bone)
            {
                return true;
            }
        }
        return false;
    }

    void AddStep(uint8_t kind, uint32_t index)
    {
        SpineGpuUpdateStep st;
        st.kind = kind;
        st.pad = 0;
        st.index = static_cast<uint16_t>(index);
        cache.push_back(st);
    }

    // `sortBone()`：先父后己，`_sorted` 挡重复。
    void SortBone(uint32_t bone)
    {
        if (sorted[bone] != 0)
        {
            return;
        }
        const int p = src.bones[bone].parent;
        if (p >= 0)
        {
            SortBone(static_cast<uint32_t>(p));
        }
        sorted[bone] = 1u;
        AddStep(SpineGpuUpdateKind::kUpdateBone, bone);
    }

    // `sortReset()`：已排序的子树递归复位，然后把自己标成未排序（⇒ 可以再被加一次）。
    void SortReset(const std::vector<uint32_t>& bones)
    {
        for (uint32_t bone : bones)
        {
            if (sorted[bone] != 0)
            {
                SortReset(children[bone]);
            }
            sorted[bone] = 0u;
        }
    }

    // `sortPathConstraintAttachment(Attachment*, Bone&)`（:618-630）
    void SortPathAttachment(const SpineSourcePathAttachment& pa, uint32_t slot_bone)
    {
        if (!pa.weighted)
        {
            SortBone(slot_bone);   // `pathBones.size() == 0` 那一路
            return;
        }
        // ⚠️ 骨骼流是**游程**编码 [n, idx×n] × 点数 ⇒ 跳过计数。
        //    参考实现 :625-629 的 `nn = pathBones[i++]; nn += i; while (i < nn)`
        //    就是同一个跳法。
        size_t i = 0;
        while (i < pa.bones.size())
        {
            const int n = pa.bones[i++];
            for (int k = 0; k < n && i < pa.bones.size(); ++k)
            {
                SortBone(static_cast<uint32_t>(pa.bones[i++]));
            }
        }
    }
};
}   // namespace

// 三类约束 + PathAttachment 落 chunk 18 / 20，执行顺序落 chunk 19。
SpineImportResult BakeSpineConstraints(const SpineSourceSkeleton& src, SpineGpuAsset& out)
{
    const size_t ik_n = src.iks.size();
    const size_t tf_n = src.transforms.size();
    const size_t path_n = src.paths.size();
    const size_t total_n = ik_n + tf_n + path_n;

    // ---- chunk 20：PathAttachment 几何 ----
    // 排序定死为 (skin, slot, name)，只为让 Bake 出来的字节可复现
    // （对拍要逐字节比；解析顺序在 json / 二进制两条路径上本来就可能不同）。
    std::vector<const SpineSourcePathAttachment*> pas;
    pas.reserve(src.path_attachments.size());
    for (const SpineSourcePathAttachment& pa : src.path_attachments)
    {
        pas.push_back(&pa);
    }
    std::sort(pas.begin(), pas.end(), [](const SpineSourcePathAttachment* a, const SpineSourcePathAttachment* b) {
        if (a->skin_index != b->skin_index)
        {
            return a->skin_index < b->skin_index;
        }
        if (a->slot_index != b->slot_index)
        {
            return a->slot_index < b->slot_index;
        }
        return a->name < b->name;
    });

    out.path_attachments.reserve(pas.size());
    out.path_bone_pool.clear();
    out.path_vertices.clear();
    out.path_lengths.clear();
    out.path_attachment_names.clear();
    for (const SpineSourcePathAttachment* pa : pas)
    {
        // ⚠️ 同一个 (皮肤, 槽位) 下挂两个 path 附件是**合法的**，不能报错 ——
        //    实测 782 个资产里有 6 个（`K_Lubandashi_Spine` 的槽位 `EFFTAIYILIZI`
        //    挂了 `EFFTAIYILIZI` 与 `EFFTAIYILIZI2`）。参考实现
        //    `PathConstraint::update()`（PathConstraint.cpp:76-80）取的是槽位
        //    **当前**挂的附件，不是"最前面那条" ⇒ 静态烘焙给出的 `path_index`
        //    只是 setup 状态下的答案，运行时在 attachment 时间轴命中后必须
        //    **按名字重新查**（这就是 chunk 20 要落附件名的原因）。
        SpineGpuPathAttachment rec;
        if (pa->slot_index < 0 || pa->slot_index > static_cast<int>(kMaxU16Index) || pa->skin_index < 0 ||
            pa->skin_index > static_cast<int>(kMaxU16Index))
        {
            return Fail("bake: path 附件 '" + pa->name + "' 的槽位/皮肤下标超出 uint16");
        }
        rec.slot_index = static_cast<uint16_t>(pa->slot_index);
        rec.skin_index = static_cast<uint16_t>(pa->skin_index);
        rec.flags = (pa->closed ? SpineGpuPathAttachmentFlags::kPathAttFlagClosed : 0) |
                    (pa->constant_speed ? SpineGpuPathAttachmentFlags::kPathAttFlagConstantSpeed : 0) |
                    (pa->weighted ? SpineGpuPathAttachmentFlags::kPathAttFlagWeighted : 0);
        rec.pad = 0;
        rec.vertex_count = static_cast<uint32_t>(pa->vertex_count);
        rec.bone_offset = static_cast<uint32_t>(out.path_bone_pool.size());
        for (int b : pa->bones)
        {
            if (b < 0 || b > static_cast<int>(kMaxU16Index))
            {
                return Fail("bake: path 附件 '" + pa->name + "' 的骨骼下标 " + std::to_string(b) + " 超出 uint16");
            }
            out.path_bone_pool.push_back(static_cast<uint16_t>(b));
        }
        rec.bone_count = static_cast<uint32_t>(pa->bones.size());
        rec.vertex_offset = static_cast<uint32_t>(out.path_vertices.size());
        out.path_vertices.insert(out.path_vertices.end(), pa->vertices.begin(), pa->vertices.end());
        rec.vertex_float_count = static_cast<uint32_t>(pa->vertices.size());
        rec.length_offset = static_cast<uint32_t>(out.path_lengths.size());
        out.path_lengths.insert(out.path_lengths.end(), pa->lengths.begin(), pa->lengths.end());
        rec.length_count = static_cast<uint32_t>(pa->lengths.size());
        // 名字进**独立的第 9 组**字符串表（不与附件名混），读端按组边界校验偏移。
        out.path_attachment_names.push_back(pa->name);
        out.path_attachments.push_back(rec);
    }

    // ---- chunk 18：约束 setup + 共用的 u16 骨骼索引池 ----
    out.constraint_bone_pool.clear();
    out.iks.clear();
    out.transforms.clear();
    out.paths.clear();

    // order 必须是 0..N-1 的一个排列 —— 否则参考实现会**静默丢约束**。
    // （见上面的注释；这是 v11 里最值得钉住的一条。）
    {
        std::vector<int> orders;
        orders.reserve(total_n);
        for (const SpineSourceIkConstraint& k : src.iks)
        {
            orders.push_back(k.order);
        }
        for (const SpineSourceTransformConstraint& t : src.transforms)
        {
            orders.push_back(t.order);
        }
        for (const SpineSourcePathConstraint& p : src.paths)
        {
            orders.push_back(p.order);
        }
        std::sort(orders.begin(), orders.end());
        for (size_t i = 0; i < orders.size(); ++i)
        {
            if (orders[i] < 0)
            {
                return Fail("bake: 约束的 order 是负的 " + std::to_string(orders[i]) +
                            "（参考实现里 `int == size_t` 的比较会把它变成天文数字 ⇒ 约束静默不生效）");
            }
            if (static_cast<size_t>(orders[i]) != i)
            {
                return Fail("bake: " + std::to_string(total_n) + " 条约束的 order 不是 0.." +
                            std::to_string(total_n - 1) + " 的一个排列（第 " + std::to_string(i) +
                            " 个是 " + std::to_string(orders[i]) +
                            "）。参考实现的 updateCache 按 i 逐个找 order == i 的那条，"
                            "找不到就跳过 ⇒ 约束会**静默不生效**。");
            }
        }
    }

    std::string push_err;
    const auto push_bones = [&](const std::vector<int>& bones, uint32_t& first_out, uint16_t& count_out,
                                const std::string& what) -> bool {
        first_out = static_cast<uint32_t>(out.constraint_bone_pool.size());
        for (int b : bones)
        {
            if (b < 0 || b > static_cast<int>(kMaxU16Index))
            {
                push_err = "bake: " + what + " 的骨骼下标 " + std::to_string(b) + " 超出 uint16";
                return false;
            }
            out.constraint_bone_pool.push_back(static_cast<uint16_t>(b));
        }
        count_out = static_cast<uint16_t>(bones.size());
        return true;
    };

    for (const SpineSourceIkConstraint& k : src.iks)
    {
        SpineGpuIkConstraint rec;
        if (k.order < 0 || k.order > static_cast<int>(kMaxU16Index))
        {
            return Fail("bake: IK 约束 '" + k.name + "' 的 order " + std::to_string(k.order) + " 超出 uint16");
        }
        rec.order = static_cast<uint16_t>(k.order);
        if (!push_bones(k.bones, rec.bone_first, rec.bone_count, "IK 约束 '" + k.name + "'"))
        {
            return Fail(push_err);
        }
        rec.target = static_cast<uint32_t>(k.target);
        rec.mix = k.mix;
        rec.bend_dir = static_cast<int16_t>(k.bend_dir);
        rec.flags = (k.compress ? SpineGpuIkFlags::kIkFlagCompress : 0) |
                    (k.stretch ? SpineGpuIkFlags::kIkFlagStretch : 0) |
                    (k.uniform ? SpineGpuIkFlags::kIkFlagUniform : 0);
        out.iks.push_back(rec);
    }

    for (const SpineSourceTransformConstraint& t : src.transforms)
    {
        SpineGpuTransformConstraint rec;
        if (t.order < 0 || t.order > static_cast<int>(kMaxU16Index))
        {
            return Fail("bake: transform 约束 '" + t.name + "' 的 order " + std::to_string(t.order) +
                        " 超出 uint16");
        }
        rec.order = static_cast<uint16_t>(t.order);
        if (!push_bones(t.bones, rec.bone_first, rec.bone_count, "transform 约束 '" + t.name + "'"))
        {
            return Fail(push_err);
        }
        rec.target = static_cast<uint32_t>(t.target);
        rec.offset_rotation = t.offset_rotation;
        rec.offset_x = t.offset_x;
        rec.offset_y = t.offset_y;
        rec.offset_scale_x = t.offset_scale_x;
        rec.offset_scale_y = t.offset_scale_y;
        rec.offset_shear_y = t.offset_shear_y;
        rec.rotate_mix = t.rotate_mix;
        rec.translate_mix = t.translate_mix;
        rec.scale_mix = t.scale_mix;
        rec.shear_mix = t.shear_mix;
        rec.flags = (t.local ? SpineGpuTransformFlags::kTfFlagLocal : 0) |
                    (t.relative ? SpineGpuTransformFlags::kTfFlagRelative : 0);
        rec.pad = 0;
        out.transforms.push_back(rec);
    }

    for (const SpineSourcePathConstraint& p : src.paths)
    {
        SpineGpuPathConstraint rec;
        if (p.order < 0 || p.order > static_cast<int>(kMaxU16Index))
        {
            return Fail("bake: path 约束 '" + p.name + "' 的 order " + std::to_string(p.order) + " 超出 uint16");
        }
        rec.order = static_cast<uint16_t>(p.order);
        if (!push_bones(p.bones, rec.bone_first, rec.bone_count, "path 约束 '" + p.name + "'"))
        {
            return Fail(push_err);
        }
        rec.target_slot = static_cast<uint32_t>(p.target_slot);
        rec.position_mode = static_cast<uint8_t>(p.position_mode);
        rec.spacing_mode = static_cast<uint8_t>(p.spacing_mode);
        rec.rotate_mode = static_cast<uint8_t>(p.rotate_mode);
        rec.pad = 0;
        rec.offset_rotation = p.offset_rotation;
        rec.position = p.position;
        rec.spacing = p.spacing;
        rec.rotate_mix = p.rotate_mix;
        rec.translate_mix = p.translate_mix;
        // setup 状态下该槽位挂的 path 附件下标 —— 解算器的**起始**答案。
        //
        // ⚠️ 判据是"名字 == 槽位的 setup 附件名"，不是"这个槽位的第一个 path 附件"：
        //    `PathConstraint::update()` 取的是 `_target->getAttachment()`
        //    （PathConstraint.cpp:76-80），setup 时它就是 slot 的 setup 附件。
        //    一个槽位挂两条 path 附件时（实测 6/782），挑错名字会让整条约束
        //    去摆一条别人的曲线 —— 不报错，只是动作飘。
        //    ⇒ 找不到（setup 附件不是 path，比如是个 region）就是 kNoPathAttachment，
        //      约束在 setup 状态下不生效，直到 attachment 时间轴把它换成 path。
        rec.path_index = SpineGpuConstants::kNoPathAttachment;
        {
            const std::string& setup_name =
                src.slots[static_cast<size_t>(p.target_slot)].attachment_name;
            for (size_t i = 0; i < out.path_attachments.size(); ++i)
            {
                if (out.path_attachments[i].skin_index == 0 &&
                    out.path_attachments[i].slot_index == rec.target_slot &&
                    out.path_attachment_names[i] == setup_name)
                {
                    rec.path_index = static_cast<uint32_t>(i);
                    break;
                }
            }
        }
        out.paths.push_back(rec);
    }

    // ---- chunk 19：执行顺序 ----
    // ⚠️ 这里**不**"让运行时重排"。重排规则是 sortBone 的递归 + 三类约束共用 order
    //    两层，转录反了不报错、只是姿势略偏（IK 22.5% / transform 48.3% / path 11.1%
    //    的资产会用上，见 v7 文档 §1.3）。落结果、运行时照着遍历。
    UpdateOrderBuilder ord(src);
    {
        const auto sort_ik = [&](size_t idx) {
            const SpineSourceIkConstraint& k = src.iks[idx];
            ord.SortBone(static_cast<uint32_t>(k.target));
            const uint32_t parent = static_cast<uint32_t>(k.bones.front());
            ord.SortBone(parent);
            if (k.bones.size() > 1)
            {
                const uint32_t child = static_cast<uint32_t>(k.bones.back());
                if (!ord.Contains(child))
                {
                    ord.reset.push_back(child);
                }
            }
            ord.AddStep(SpineGpuUpdateKind::kUpdateIk, static_cast<uint32_t>(idx));
            ord.SortReset(ord.children[parent]);
            ord.sorted[static_cast<uint32_t>(k.bones.back())] = 1u;
        };
        const auto sort_tf = [&](size_t idx) {
            const SpineSourceTransformConstraint& t = src.transforms[idx];
            ord.SortBone(static_cast<uint32_t>(t.target));
            if (t.local)
            {
                for (int b : t.bones)
                {
                    const int p = src.bones[static_cast<size_t>(b)].parent;
                    if (p >= 0)
                    {
                        ord.SortBone(static_cast<uint32_t>(p));
                    }
                    if (!ord.Contains(static_cast<uint32_t>(b)))
                    {
                        ord.reset.push_back(static_cast<uint32_t>(b));
                    }
                }
            }
            else
            {
                for (int b : t.bones)
                {
                    ord.SortBone(static_cast<uint32_t>(b));
                }
            }
            ord.AddStep(SpineGpuUpdateKind::kUpdateTransform, static_cast<uint32_t>(idx));
            for (int b : t.bones)
            {
                ord.SortReset(ord.children[static_cast<size_t>(b)]);
                ord.sorted[static_cast<size_t>(b)] = 1u;
            }
        };
        const auto sort_path = [&](size_t idx) {
            const SpineSourcePathConstraint& p = src.paths[idx];
            const uint32_t slot = static_cast<uint32_t>(p.target_slot);
            const uint32_t slot_bone = static_cast<uint32_t>(src.slots[p.target_slot].bone_index);
            // 参考实现遍历：当前皮肤 → default 皮肤 → **所有**皮肤 → 槽位当前附件。
            // 前三步合起来就是"所有皮肤"，最后一步的附件也必然来自某个皮肤
            // ⇒ 静态烘焙下"遍历 chunk 20 里这个槽位的全部 path 附件"是等价的。
            for (const SpineSourcePathAttachment* pa : pas)
            {
                if (static_cast<uint32_t>(pa->slot_index) == slot)
                {
                    ord.SortPathAttachment(*pa, slot_bone);
                }
            }
            for (int b : p.bones)
            {
                ord.SortBone(static_cast<uint32_t>(b));
            }
            ord.AddStep(SpineGpuUpdateKind::kUpdatePath, static_cast<uint32_t>(idx));
            for (int b : p.bones)
            {
                ord.SortReset(ord.children[static_cast<size_t>(b)]);
                ord.sorted[static_cast<size_t>(b)] = 1u;
            }
        };

        // `updateCache()` 的 `goto continue_outer` 循环：按 i = 0..N-1 逐个找
        // order == i 的那条（先 ik、再 transform、再 path）。order 已校验是排列
        // ⇒ 每个 i 都必然命中。
        for (size_t i = 0; i < total_n; ++i)
        {
            bool done = false;
            for (size_t j = 0; j < ik_n; ++j)
            {
                if (src.iks[j].order == static_cast<int>(i))
                {
                    sort_ik(j);
                    done = true;
                    break;
                }
            }
            if (!done)
            {
                for (size_t j = 0; j < tf_n; ++j)
                {
                    if (src.transforms[j].order == static_cast<int>(i))
                    {
                        sort_tf(j);
                        done = true;
                        break;
                    }
                }
            }
            if (!done)
            {
                for (size_t j = 0; j < path_n; ++j)
                {
                    if (src.paths[j].order == static_cast<int>(i))
                    {
                        sort_path(j);
                        done = true;
                        break;
                    }
                }
            }
        }

        // 最后：`for (i = 0; i < bones.size(); ++i) sortBone(bones[i]);`
        for (size_t i = 0; i < src.bones.size(); ++i)
        {
            ord.SortBone(static_cast<uint32_t>(i));
        }
    }

    // 复位表里可能有重复（同一根骨骼被多条约束加过）。复位是幂等的
    // （`updateWorldTransform` 只是把 applied 值抄回 setup 值）⇒ 去重不丢信息。
    {
        std::vector<uint32_t> uniq;
        for (uint32_t b : ord.reset)
        {
            if (std::find(uniq.begin(), uniq.end(), b) == uniq.end())
            {
                uniq.push_back(b);
            }
        }
        ord.reset = std::move(uniq);
    }

    // ⚠️ `update_steps` **恒**描述"完整遍历顺序"（无约束时就是 0..bone_count-1），
    //    但 chunk 19 只在**有约束**时才落盘 —— 无约束时这个顺序是可证的常数
    //    （parent < index ⇒ 正向遍历即拓扑序，reset 必为空），写下来纯属冗余
    //    （782 个资产实测 32036 × 4 B）。读端在无 chunk 19 时按同一条规则重建，
    //    于是 Bake 出来的内存态与 Read 出来的内存态**一致** ⇒ 读→写能逐字节相同。
    out.update_steps = ord.cache;
    out.update_reset.reserve(ord.reset.size());
    for (uint32_t b : ord.reset)
    {
        out.update_reset.push_back(static_cast<uint16_t>(b));
    }
    return Ok();
}

uint32_t SpineGpuClipDeformLength(const SpineGpuAsset& asset, const SpineGpuClipPolygon& clip)
{
    if (clip.vertex_format == SpineVertexFormat::kRigid)
    {
        return 2u * clip.vertex_count;
    }
    const size_t w0 = static_cast<size_t>(asset.weighted_segment_offset) +
                      static_cast<size_t>(clip.vertex_offset) * SpineGpuConstants::kRigidVertexStride;
    if (w0 + static_cast<size_t>(clip.vertex_count) * SpineGpuConstants::kRigidVertexStride > asset.vertex_buffer.size())
    {
        return 0;
    }
    uint32_t len = 0;
    for (uint32_t v = 0; v < clip.vertex_count; ++v)
    {
        uint32_t cnt = 0;
        std::memcpy(&cnt, asset.vertex_buffer.data() + w0 + v * SpineGpuConstants::kRigidVertexStride + 4u, 4);
        len += 2u * cnt;
    }
    return len;
}

namespace
{
    // deform 时间轴 → chunk 9 行号：按 (皮肤, 槽位, 名字) 精确命中；-1 = 不是裁剪附件。
    int FindClipRow(const SpineGpuAsset& out, int skin, int slot, const std::string& name)
    {
        for (size_t k = 0; k < out.clip_polygons.size(); ++k)
        {
            const SpineGpuClipPolygon& c = out.clip_polygons[k];
            if (static_cast<int>(c.skin_id) == skin && static_cast<int>(c.slot_index) == slot && k < out.clip_names.size() &&
                out.clip_names[k] == name)
            {
                return static_cast<int>(k);
            }
        }
        return -1;
    }
}   // namespace

SpineImportResult BakeSpineGpuAsset(const SpineSourceSkeleton& src, SpineGpuAsset& out)
{
    out = SpineGpuAsset {};

    const uint32_t bone_count = static_cast<uint32_t>(src.bones.size());
    const uint32_t slot_count = static_cast<uint32_t>(src.slots.size());

    // ---- 骨骼 setup（chunk 10）----
    // 骨骼下标是容器里**唯一**的骨骼编号空间：chunk 1 的骨骼名、影响流的 bone_idx、
    // 槽位的 bone_index、时间轴的 target 全指它。所以这里的校验都在保"只有一个空间"。
    if (bone_count > SpineGpuConstants::kMaxBones)
    {
        return Fail("bake: 骨骼数 " + std::to_string(bone_count) + " 超过上限 " +
                    std::to_string(SpineGpuConstants::kMaxBones) +
                    "（parent 是 int16，且影响流的 bone_idx 是 uint16）");
    }
    out.bones.reserve(bone_count);
    for (uint32_t i = 0; i < bone_count; ++i)
    {
        const SpineSourceBone& s = src.bones[i];
        // ⚠️ **parent < index** 是"正向遍历即拓扑序"的前提。重排一下不会报错，
        // 只会让"骨骼下标"在容器里同时表示两种顺序 ⇒ 蒙皮取到错的骨骼。
        if (s.parent >= static_cast<int>(i))
        {
            return Fail("bake: 骨骼 '" + s.name + "'（下标 " + std::to_string(i) + "）的父下标是 " +
                        std::to_string(s.parent) + "，但父骨骼必须排在自己之前"
                        "（容器依赖这条不变量做单趟正向求解，不能静默重排）");
        }
        if (s.parent < -1)
        {
            return Fail("bake: 骨骼 '" + s.name + "' 的父下标 " + std::to_string(s.parent) + " 非法（-1 = 根）");
        }

        SpineGpuBone b;
        b.x = s.x;
        b.y = s.y;
        b.rotation = s.rotation;
        b.scale_x = s.scale_x;
        b.scale_y = s.scale_y;
        b.shear_x = s.shear_x;
        b.shear_y = s.shear_y;
        b.length = s.length;
        b.parent = static_cast<int16_t>(s.parent);
        b.transform_mode = static_cast<uint8_t>(s.transform_mode);
        // pad 保持 0：它是落盘字节的一部分，不清零会让 C++ 与 C# 的逐字节对拍
        // 拿到栈上的脏值，而这种"两个字节不一样"查起来极烦。
        b.pad = 0;
        out.bones.push_back(b);
    }

    // ---- 皮肤的门：容器约定 skin 0 = default（SpineGpuFormat.h 的 kDefaultSkinId）----
    // 两个来源的导出顺序都满足它，但"通常满足"不等于"可以不查"：
    // 不满足时若照单全收，默认外观会悄悄变成另一个皮肤，而没有任何报错。
    for (size_t i = 0; i < src.skins.size(); ++i)
    {
        if (src.skins[i] == "default" && i != SpineGpuConstants::kDefaultSkinId)
        {
            return Fail("bake: 名为 \"default\" 的皮肤排在第 " + std::to_string(i) +
                        " 位，但容器约定 **skin 0 恒为 default 皮肤**。"
                        "不静默挪位 —— 挪了默认外观就变成另一个皮肤，而外观错没有任何报错。");
        }
    }
    if (src.skins.size() > 0xFFFFu)
    {
        return Fail("bake: 皮肤数 " + std::to_string(src.skins.size()) + " 超过 uint16 上限（skin_id 存不下）");
    }
    if (src.attachments.size() > 0xFFFFu)
    {
        return Fail("bake: 附件数 " + std::to_string(src.attachments.size()) +
                    " 超过 uint16 上限（attachment_id 存不下）");
    }

    // ---- pass 1：只统计索引数 ----
    // 顶点数**不能**在 pass 1 算：几何相同的附件会共用同一段顶点（见下面的池子），
    // 最终顶点数要等去重跑完才知道。
    uint32_t index_count = 0;
    for (const SpineSourceAttachment& a : src.attachments)
    {
        index_count += static_cast<uint32_t>(a.triangles.size());
    }
    out.index_buffer.reserve(index_count);

    // 顶点块分两个池子（rigid / weighted），因为**索引是段内局部下标** ——
    // 两个段的顶点起点都从 0 开始，不能共用一个计数。
    ExactBlobPool rigid_pool(SpineGpuConstants::kRigidVertexStride);
    ExactBlobPool weighted_pool(SpineGpuConstants::kWeightedVertexStride);
    ExactBlobPool uv_pool(SpineGpuConstants::kUvStride);
    // 影响流池（v7）。单位是一条影响 = bindPos(8) + bone(4) = 12 B。
    // ⚠️ 它**必须**在顶点块之前去重 —— 见 FixupInfluenceOffsets 上的顺序说明。
    ExactBlobPool influence_pool(SpineGpuConstants::kInfluenceEntrySize);

    // ---- 裁剪多边形：先烘进顶点池，再算作用域 ----
    // 多边形顶点与 mesh 顶点**同一个池子**（裁剪附件是 VertexAttachment，顶点同构）
    // ⇒ 几何相同的两条裁剪多边形也共用一段，且不占 header 里任何新字段。
    // 顺序上必须在 pass 2 之前：这样裁剪顶点占据段的前部，后面的附件照常往后排。
    if (src.clips.size() > SpineGpuConstants::kNoClipIndex)
    {
        return Fail("bake: 裁剪附件数 " + std::to_string(src.clips.size()) +
                    " 超过 uint16 上限（绘制项的 clip_index 存不下）");
    }
    std::vector<SpineSourceClip> clips_sorted = src.clips;
    // ⚠️ **稳定**排序：同一 (皮肤, 槽位) 挂两条裁剪附件是合法的（v15 起按名字区分），
    //    std::sort 会让它们的行号随实现变 ⇒ 同一份源烘两次字节不同。
    std::stable_sort(clips_sorted.begin(), clips_sorted.end(),
              [](const SpineSourceClip& a, const SpineSourceClip& b)
              {
                  if (a.skin_index != b.skin_index)
                  {
                      return a.skin_index < b.skin_index;
                  }
                  return a.slot_index < b.slot_index;
              });
    for (const SpineSourceClip& c : clips_sorted)
    {
        const uint32_t vc = static_cast<uint32_t>(c.vertex_count);
        if (vc == 0)
        {
            return Fail("bake: 槽位 '" + src.slots[static_cast<size_t>(c.slot_index)].name +
                        "' 的裁剪附件顶点数为 0（空多边形裁不出任何东西）");
        }
        std::vector<uint8_t> blob;
        std::vector<uint8_t> infl;
        std::string err;
        const std::string what = "bake: 槽位 '" + src.slots[static_cast<size_t>(c.slot_index)].name + "' 的裁剪附件";
        if (!EncodeVertices(what, c.vertices, vc, c.weighted, bone_count, blob, err, &infl))
        {
            return Fail(err);
        }
        if (c.weighted)
        {
            FixupInfluenceOffsets(blob, influence_pool.Intern(infl.data(), infl.size()).second);
        }
        ExactBlobPool& pool = c.weighted ? weighted_pool : rigid_pool;

        SpineGpuClipPolygon rec {};
        rec.slot_index = static_cast<uint16_t>(c.slot_index);
        rec.skin_id = static_cast<uint16_t>(c.skin_index);
        rec.vertex_format = c.weighted ? SpineVertexFormat::kWeighted : SpineVertexFormat::kRigid;
        rec.vertex_count = static_cast<uint16_t>(vc);
        rec.vertex_offset = pool.Intern(blob.data(), blob.size()).second;
        // v15：结束槽位落盘（运行时按**当前**绘制顺序跑状态机，见 kFormatVersion 的 v15 条）
        if (c.end_slot_index >= static_cast<int>(SpineGpuConstants::kNoClipEndSlot))
        {
            return Fail("bake: 裁剪附件 '" + c.name + "' 的结束槽位 " + std::to_string(c.end_slot_index) +
                        " 超出 uint16（0xFFFF 留给\"没有结束槽位\"）");
        }
        rec.end_slot = c.end_slot_index < 0 ? SpineGpuConstants::kNoClipEndSlot
                                            : static_cast<uint16_t>(c.end_slot_index);
        out.clip_polygons.push_back(rec);
        out.clip_names.push_back(c.name);   // name_offset 在字符串表建好后回填
    }

    // ---- 裁剪作用域：逐皮肤**模拟**参考实现的状态机 ----
    // 不能用"存在某条 clip 覆盖这个槽"的宽松判定，因为参考实现有三条不对称的规则，
    // 少一条就会把不该回退的项也回退（或者反过来）：
    //   · 走到裁剪附件所在槽位 ⇒ `clipStart()` 然后 `continue`（MeshGeneration/SpineMesh.h:153-159）
    //     ⇒ **该槽位自己不被裁**，而且它也不会调 `clipEnd(slot)`。
    //   · 已经有一个裁剪在跑时 `clipStart()` 直接返回 0（SkeletonClipping.cpp:49-51）
    //     ⇒ **嵌套裁剪被忽略**：后面那条 clip 永远等不到它"开始"的机会，
    //     哪怕它的 endSlot 更靠后，也不会在前者结束后补上。
    //   · 槽位收尾调 `clipEnd(slot)`，只在 `slot == endSlot` 时收尾（:72-76）。
    // ⇒ 只有把槽位序列从头走一遍，才能知道每个槽位当时"是不是正在被裁"。
    //
    // 作用域是**逐皮肤**的：裁剪附件是某个皮肤下某槽位的附件，3.8 同时只有一个皮肤
    // 生效，换到没有它的皮肤时作用域就不存在（容器里 skin_id 是绘制项的键之一）。
    const uint32_t skin_count = static_cast<uint32_t>(src.skins.size());
    // clip_end[skin][slot]：该皮肤下该槽位**挂着**裁剪附件时的 endSlot；kNoClip = 没有。
    // 值是 -1 时表示"没有结束槽位"（json 缺 "end"）⇒ 裁到最后一个槽位。
    // clip_rec[skin][slot]：那条裁剪附件在 `out.clip_polygons` 里的**行号**
    // （= 绘制项 clip_index 要填的值）。两个矩阵下标一一对应。
    constexpr int kNoClip = -2;
    std::vector<std::vector<int>> clip_end(static_cast<size_t>(skin_count),
                                           std::vector<int>(static_cast<size_t>(slot_count), kNoClip));
    std::vector<std::vector<int>> clip_rec(static_cast<size_t>(skin_count),
                                           std::vector<int>(static_cast<size_t>(slot_count), -1));
    for (size_t ci = 0; ci < clips_sorted.size(); ++ci)
    {
        const SpineSourceClip& c = clips_sorted[ci];
        if (c.skin_index < 0 || c.slot_index < 0 ||
            static_cast<size_t>(c.skin_index) >= skin_count ||
            static_cast<size_t>(c.slot_index) >= slot_count)
        {
            return Fail("bake: 裁剪附件的下标越界（skin " + std::to_string(c.skin_index) + " / slot " +
                        std::to_string(c.slot_index) + "）—— 解析路径没把它归到已有的皮肤 / 槽位");
        }
        clip_end[static_cast<size_t>(c.skin_index)][static_cast<size_t>(c.slot_index)] = c.end_slot_index;
        clip_rec[static_cast<size_t>(c.skin_index)][static_cast<size_t>(c.slot_index)] = static_cast<int>(ci);
    }

    // clipped[skin][slot] = 1 ⇒ 该皮肤下该槽位的几何要由 CPU 裁剪 ⇒ kind = kFallback。
    // clip_of[skin][slot] = 当时生效的那条裁剪在 clip_polygons 里的行号（-1 = 没被裁）。
    std::vector<std::vector<uint8_t>> clipped(static_cast<size_t>(skin_count),
                                              std::vector<uint8_t>(static_cast<size_t>(slot_count), 0u));
    std::vector<std::vector<int>> clip_of(static_cast<size_t>(skin_count),
                                          std::vector<int>(static_cast<size_t>(slot_count), -1));
    for (uint32_t sk = 0; sk < skin_count; ++sk)
    {
        int active_end = kNoClip;   // 当前生效裁剪的结束槽位
        int active_rec = -1;        // 当前生效裁剪在 clip_polygons 里的行号
        for (uint32_t s = 0; s < slot_count; ++s)
        {
            const int here = clip_end[sk][s];
            if (here != kNoClip)
            {
                // 该槽位挂的是裁剪附件 ⇒ 参考实现 `clipStart()` 之后**立刻 continue**
                // （SpineMesh.h:153-159），也就是**跳过了**槽位收尾那句 `clipEnd(*slot)`
                // （:257）。两个后果，都不能省：
                //   · 这个槽位自己不画、也**不结束**任何进行中的裁剪；
                //   · 嵌套时 `clipStart()` 直接返回 0（SkeletonClipping.cpp:49-51）
                //     ⇒ 后一条裁剪**永远开不起来**，哪怕它的 endSlot 更靠后。
                if (active_end == kNoClip)
                {
                    // ⚠️ end <= 自己（含"没有 end" ⇒ here < 0）时**不需要**特殊处理：
                    // 收尾检查在下面那个分支里，而本槽位被 continue 掉了 ⇒
                    // `active_end == s` 永远不成立 ⇒ 这次裁剪只能靠整条绘制指令末尾
                    // 那次无条件 `clipEnd()` 收尾（:259）⇒ 一路裁到最后一个槽位。
                    // 这**正是**参考实现的行为：end 写错成"自己或更前面"时，
                    // 画面会一路裁到末尾，且不报任何错。
                    active_end = here;
                    active_rec = clip_rec[sk][s];
                }
                continue;
            }
            if (active_end != kNoClip)
            {
                clipped[sk][s] = 1u;
                clip_of[sk][s] = active_rec;
                if (active_end == static_cast<int>(s))
                {
                    active_end = kNoClip;   // clipEnd(slot)
                    active_rec = -1;
                }
            }
        }
    }

    // ---- pass 2：写顶点 / UV / 索引 / 绘制项 ----
    for (size_t ai = 0; ai < src.attachments.size(); ++ai)
    {
        const SpineSourceAttachment& a = src.attachments[ai];
        const uint32_t vc = a.VertexCount();
        const bool weighted = a.IsWeighted();

        // 没有 skin_index 的附件进不了容器（skin_id 是换装的键）。
        // 解析路径漏设它是内部 bug，但不能让它静默变成 0 —— 那就是"悄悄披上 default 皮肤"。
        if (a.skin_index < 0 || static_cast<size_t>(a.skin_index) >= src.skins.size())
        {
            return Fail("bake: 附件 '" + a.name + "' 的 skin_index 是 " + std::to_string(a.skin_index) +
                        "（皮肤数 " + std::to_string(src.skins.size()) + "）—— 解析路径没给它归皮肤");
        }

        // 先把这一份附件的顶点整块拼出来，再去重池里问"有没有一模一样的"。
        // 编码与裁剪多边形共用 EncodeVertices() —— 两者都是 VertexAttachment 的顶点。
        std::vector<uint8_t> blob;
        std::vector<uint8_t> infl;
        blob.reserve(static_cast<size_t>(vc) * (weighted ? SpineGpuConstants::kWeightedVertexStride
                                                         : SpineGpuConstants::kRigidVertexStride));
        {
            std::string err;
            if (!EncodeVertices("bake: 附件 '" + a.name + "'", a.vertices, vc, weighted, bone_count, blob, err,
                                &infl))
            {
                return Fail(err);
            }
        }

        // ---- 影响流去重 → 改写顶点块里的本地下标（顺序不能颠倒）----
        if (weighted)
        {
            FixupInfluenceOffsets(blob, influence_pool.Intern(infl.data(), infl.size()).second);
        }

        // ---- 顶点块去重：几何 + 蒙皮完全相同才共用 ----
        ExactBlobPool& pool = weighted ? weighted_pool : rigid_pool;
        const uint32_t vertex_base = pool.Intern(blob.data(), blob.size()).second;

        // ---- UV：每个绘制项一份，**不参与顶点去重** ----
        // 这正是拆通道的目的：linked mesh 继承父附件的几何，却用自己的图集 region
        // 算 UV ⇒ 几何共用、UV 各一份。
        std::vector<uint8_t> uv_blob(static_cast<size_t>(vc) * SpineGpuConstants::kUvStride, 0u);
        for (uint32_t v = 0; v < vc; ++v)
        {
            const float u = a.uvs[static_cast<size_t>(v) * 2u];
            const float wv = a.uvs[static_cast<size_t>(v) * 2u + 1u];
            std::memcpy(uv_blob.data() + static_cast<size_t>(v) * SpineGpuConstants::kUvStride, &u, sizeof(u));
            std::memcpy(uv_blob.data() + static_cast<size_t>(v) * SpineGpuConstants::kUvStride + 4u, &wv,
                        sizeof(wv));
        }
        const uint32_t uv_offset = uv_pool.Intern(uv_blob.data(), uv_blob.size()).second;

        // ---- 索引：相对**所在顶点段**的局部下标 ----
        // 两种格式是两个独立的顶点段、两次独立的绘制，索引不能跨段。
        const uint32_t index_offset = static_cast<uint32_t>(out.index_buffer.size());
        // 索引是 u16：段内绝对下标超过 0xFFFF 时 `static_cast<uint16_t>` 会**静默截断**成
        // 别的顶点 ⇒ 显式报错（vertex_base 本身也要进 v14 绘制项的 u16 字段，同理）。
        if (vertex_base > 0xFFFFu || vc > 0xFFFFu || (vc > 0u && vertex_base + vc - 1u > 0xFFFFu))
        {
            return Fail("bake: 附件 '" + a.name + "' 的顶点落在顶点段第 " + std::to_string(vertex_base) + " 起的 " +
                        std::to_string(vc) + " 个，超出 u16 索引范围（" +
                        std::string(weighted ? "weighted" : "rigid") + " 段总顶点数过多）");
        }
        for (uint16_t t : a.triangles)
        {
            if (t >= vc)
            {
                return Fail("bake: 附件 '" + a.name + "' 的三角形引用了第 " + std::to_string(t) + " 个顶点，但它只有 " +
                            std::to_string(vc) + " 个");
            }
            out.index_buffer.push_back(static_cast<uint16_t>(t + vertex_base));
        }

        SpineDrawItem item {};
        item.index_offset = index_offset;
        item.index_count = static_cast<uint32_t>(a.triangles.size());
        // v13：规格 §3.2 的公式是 `uv_buffer[uv_offset + vertexIndex]`，`vertexIndex` **直接取自
        // 索引缓冲** —— 而索引是**段内绝对**下标（= 附件本地下标 + vertex_base）。UV 却按附件
        // 本地下标排（uv_pool 里第 uv_unit 条起）。所以必须落 `uv_unit - vertex_base`，
        // 按 **uint32 回绕**算：`uv_offset + index` 在 C++ / C# / GLSL 的 uint 加法下都正好
        // 回到 `uv_unit + 本地下标`。v12 及以前落的是 `uv_unit`，只有 vertex_base == 0 时对；
        // OSGame 实测 119 737 个绘制项里 112 562 个 vertex_base != 0（39 548 个还会读出 UV 表），
        // 而错 UV 只是"贴图采错块"，不报错。
        item.uv_offset = uv_offset - vertex_base;
        item.vertex_format = weighted ? SpineVertexFormat::kWeighted : SpineVertexFormat::kRigid;
        if (a.slot_index < 0 || static_cast<size_t>(a.slot_index) >= src.slots.size())
        {
            return Fail("bake: 附件 '" + a.name + "' 的槽位下标越界");
        }
        item.blend_mode = src.slots[static_cast<size_t>(a.slot_index)].blend_mode;
        if (a.atlas_page >= src.atlas_pages.size() && !src.atlas_pages.empty())
        {
            return Fail("bake: 附件 '" + a.name + "' 在图集第 " + std::to_string(a.atlas_page) + " 页，但图集只有 " +
                        std::to_string(src.atlas_pages.size()) + " 页");
        }
        if (a.atlas_page > 0xFFFFu)
        {
            return Fail("bake: 附件 '" + a.name + "' 的图集页号超出 uint16");
        }
        item.atlas_page = static_cast<uint16_t>(a.atlas_page);
        item.slot_index = static_cast<uint16_t>(a.slot_index);
        item.attachment_id = static_cast<uint16_t>(ai);
        item.skin_id = static_cast<uint16_t>(a.skin_index);
        // 两个"画不动静态 VB"的原因，合起来决定 kind，**都只回退自己这一项**：
        //   * deform（FFD）命中 ⇒ 顶点每帧都变，静态 VB 里那份绑定姿势不算数（规格 §7.1）
        //   * 落在裁剪作用域内 ⇒ `clipTriangles()` 会重写顶点 / 三角形 / UV
        //     （SkeletonClipping.h:47-54），三角形个数都变了（规格 §7.2）
        // 这与 kHasDrawOrderTimeline 那种"整个资产走 CPU"是两回事。
        const bool clipped_here = clipped[static_cast<size_t>(a.skin_index)][static_cast<size_t>(a.slot_index)] != 0u;
        item.kind = (a.deformed || clipped_here) ? SpineDrawItemKind::kFallback : SpineDrawItemKind::kStatic;
        item.item_flags = a.type == SpineSourceAttachment::Type::kRegion ? SpineDrawItemFlags::kRegion : 0u;
        // 被裁的项指到**当时生效**的那条裁剪多边形上（chunk 9 的行号）。
        // 没被裁时是 kNoClipIndex（0xFFFF）—— **不是 0**：0 是一条真实存在的多边形，
        // 填 0 会让运行时拿第一条多边形去裁根本不该裁的项，而且不报错。
        const int clip_of_here = clip_of[static_cast<size_t>(a.skin_index)][static_cast<size_t>(a.slot_index)];
        item.clip_index = clip_of_here >= 0 ? static_cast<uint16_t>(clip_of_here)
                                            : SpineGpuConstants::kNoClipIndex;
        item.vertex_base = static_cast<uint16_t>(vertex_base);
        // linked mesh 继承父附件的 deform（MeshAttachment.cpp:94-95）：只记**直接**父，
        // 参考实现的判据就是 `_parentMesh == sourceAttachment`，不沿链往上找。
        item.deform_parent = SpineGpuConstants::kNoDeformParent;
        if (a.parent_index >= 0 && a.inherit_deform)
        {
            if (static_cast<size_t>(a.parent_index) >= src.attachments.size() ||
                src.attachments[static_cast<size_t>(a.parent_index)].slot_index != a.slot_index)
            {
                return Fail("bake: linked mesh '" + a.name + "' 的父附件下标 " + std::to_string(a.parent_index) +
                            " 越界或不在同一槽位（参考实现按本槽位查父附件，SkeletonBinary.cpp 解析 linkedMeshes）");
            }
            item.deform_parent = static_cast<uint16_t>(a.parent_index);
        }
        item.vertex_count = static_cast<uint16_t>(vc);
        out.draw_items.push_back(item);

        SpineGpuAttachmentRecord rec {};
        rec.slot_index = static_cast<uint16_t>(a.slot_index);
        rec.attachment_id = static_cast<uint16_t>(ai);
        rec.skin_id = static_cast<uint16_t>(a.skin_index);
        rec.pad = 0;
        out.attachment_records.push_back(rec);
    }

    // ---- 拼顶点缓冲：rigid 段 → 8 B 对齐 → weighted 段 ----
    // （去重之后才知道各段多长，所以放到循环之后。）
    const uint32_t rigid_bytes = static_cast<uint32_t>(rigid_pool.data().size());
    const uint32_t weighted_offset = (rigid_bytes + SpineGpuConstants::kVertexSegmentAlignment - 1) /
                                     SpineGpuConstants::kVertexSegmentAlignment *
                                     SpineGpuConstants::kVertexSegmentAlignment;
    const uint32_t weighted_bytes = static_cast<uint32_t>(weighted_pool.data().size());

    out.rigid_segment_offset = 0;
    out.weighted_segment_offset = weighted_offset;
    out.rigid_vertex_count = rigid_pool.unit_count();
    out.weighted_vertex_count = weighted_pool.unit_count();
    out.vertex_buffer.assign(static_cast<size_t>(weighted_offset) + weighted_bytes, 0u);
    if (!rigid_pool.data().empty())
    {
        std::memcpy(out.vertex_buffer.data(), rigid_pool.data().data(), rigid_pool.data().size());
    }
    if (!weighted_pool.data().empty())
    {
        std::memcpy(out.vertex_buffer.data() + weighted_offset, weighted_pool.data().data(),
                    weighted_pool.data().size());
    }

    // ---- 影响流：把去重后那条 12 B/条的流拆成 chunk 21（bindPos）与 22（bone）----
    // 拆开而不是保留 12 B：Load2 要 8 B 对齐，12 B 步长会让第 1、3、5…条的
    // bindPos 落在 12、36…（不是 8 的倍数）⇒ 只能退化成标量 Load（规格 §3.1）。
    SplitInfluenceStream(influence_pool.data(), out.influence_bind, out.influence_bone);
    // chunk 的 offset / size 是 u32，越界了会在写入时静默回绕 ⇒ 这里先挡住。
    if (static_cast<uint64_t>(out.influence_bone.size()) * SpineGpuConstants::kInfluenceBindStride >
        0xFFFFFFFFu)
    {
        return Fail("bake: 影响流 " + std::to_string(out.influence_bone.size()) +
                    " 条，字节数超过 u32（chunk 21 的偏移存不下）");
    }

    // ---- UV 通道：独立数组，条目数 **≠** 顶点数 ----
    const std::vector<uint8_t>& uv_bytes = uv_pool.data();
    out.uv_buffer.resize(uv_bytes.size() / sizeof(float));
    if (!uv_bytes.empty())
    {
        std::memcpy(out.uv_buffer.data(), uv_bytes.data(), uv_bytes.size());
    }

    // ---- slot_bone：rigid 用（槽位骨骼下标） ----
    out.slot_bone.resize(slot_count);
    for (uint32_t s = 0; s < slot_count; ++s)
    {
        out.slot_bone[s] = static_cast<uint16_t>(src.slots[s].bone_index);
    }

    out.bone_names.reserve(src.bones.size());
    for (const SpineSourceBone& b : src.bones)
    {
        out.bone_names.push_back(b.name);
    }
    out.skin_names = src.skins;
    out.slot_names.reserve(src.slots.size());
    // ---- 槽位 setup（chunk 17）----
    // ⚠️ 颜色**必须**进容器：color / twoColor 时间轴写的是**绝对颜色**
    //    （`ColorTimeline.cpp:110` `slot._color.set(r,g,b,a)`），而"没有时间轴命中"
    //    的槽位停在 setup 色。容器里只有 `kUsesTintBlack` 一个布尔、没有色值 ⇒
    //    解算器只能默认成纯白，而**纯白与 setup 白在多数资产上完全看不出区别**。
    out.slots.reserve(src.slots.size());
    out.slot_setup_attachment_names.reserve(src.slots.size());
    for (const SpineSourceSlot& s : src.slots)
    {
        out.slot_names.push_back(s.name);

        SpineGpuSlot rec;
        rec.color_r = s.color[0];
        rec.color_g = s.color[1];
        rec.color_b = s.color[2];
        rec.color_a = s.color[3];
        rec.dark_r = s.dark[0];
        rec.dark_g = s.dark[1];
        rec.dark_b = s.dark[2];
        rec.dark_a = s.dark[3];
        rec.flags = s.has_dark ? SpineGpuSlotFlags::kSlotFlagHasDark : SpineGpuSlotFlags::kSlotFlagNone;
        out.slots.push_back(rec);

        // 空名也占一条（写一个 '\0'）⇒ 这一组恒为 slot_count 条，读端能切准。
        out.slot_setup_attachment_names.push_back(s.attachment_name);
    }
    out.attachment_names.reserve(src.attachments.size());
    for (const SpineSourceAttachment& a : src.attachments)
    {
        out.attachment_names.push_back(a.name);
    }
    out.atlas_page_names = src.atlas_pages;

    // ---- 约束 setup（chunk 18 / 19 / 20，v11）----
    // 排在骨骼 / 槽位 / 皮肤名之后：三者都要拿已建好的下标空间。
    {
        const SpineImportResult r = BakeSpineConstraints(src, out);
        if (!r.ok)
        {
            return r;
        }
    }

    // ---- 动画（chunk 11-16）----
    // 顺序上必须排在 attachment_names / atlas_page_names **之后**：动画名与"动画附件名"
    // 是字符串表的最后两组，偏移依赖前面五组的总长。
    for (const SpineSourceAnimation& sa : src.animations)
    {
        out.anim_names.push_back(sa.name);
    }
    // 第一趟：把 attachment 时间轴用到的附件名**去重**收进字符串池。
    // 不去重的话同一个附件名被 10 个动画引用就是 10 份 —— 而它本来就在 chunk 1 的
    // 附件名那组里有一份（只是偏移不同，不能复用，因为切分规则不一样）。
    for (const SpineSourceAnimation& sa : src.animations)
    {
        for (const SpineSourceTimeline& tl : sa.timelines)
        {
            if (tl.type != SpineTimelineType::kAttachment)
            {
                continue;
            }
            for (const std::string& n : tl.attachment_names)
            {
                if (!n.empty())
                {
                    InternString(out.anim_attachment_names, n);
                }
            }
        }
    }
    {
        std::vector<uint8_t> blob;
        std::vector<uint32_t> slot_setup_att_off;
        std::vector<uint32_t> anim_name_off;
        std::vector<uint32_t> anim_att_off;
        std::vector<uint32_t> path_att_off;
        std::vector<uint32_t> clip_name_off;
        BuildSpineStringTable(out, blob, slot_setup_att_off, anim_name_off, anim_att_off, path_att_off,
                              clip_name_off);
        if (clip_name_off.size() != out.clip_polygons.size())
        {
            return Fail("bake: 字符串表内部错误 —— 裁剪附件名偏移数与裁剪多边形数不符");
        }
        for (size_t i = 0; i < out.clip_polygons.size(); ++i)
        {
            out.clip_polygons[i].name_offset = clip_name_off[i];
        }
        if (anim_name_off.size() != out.anim_names.size())
        {
            return Fail("bake: 字符串表内部错误 —— 动画名偏移数与动画数不符");
        }
        if (slot_setup_att_off.size() != out.slots.size())
        {
            return Fail("bake: 字符串表内部错误 —— 槽位 setup 附件名偏移数与槽位数不符");
        }
        if (path_att_off.size() != out.path_attachments.size())
        {
            return Fail("bake: 字符串表内部错误 —— path 附件名偏移数与 path 附件数不符");
        }
        for (size_t i = 0; i < out.path_attachments.size(); ++i)
        {
            out.path_attachments[i].name_offset = path_att_off[i];
        }
        // 空名 ⇒ `kNoAttachmentName`（**不是 0**：0 是字符串表里第一个名字的合法偏移，
        // 填 0 会让"没挂附件"变成"挂了某个真实附件"且不报错）。
        for (size_t i = 0; i < out.slots.size(); ++i)
        {
            out.slots[i].setup_attachment_name_offset = out.slot_setup_attachment_names[i].empty()
                ? SpineGpuConstants::kNoAttachmentName
                : slot_setup_att_off[i];
        }

        // 用户事件定义（v16，chunk 23）。字符串池第 0 条恒为空串；名字 / 字符串 / 音频去重进池。
        out.events.clear();
        out.event_strings.clear();
        if (!src.events.empty())
        {
            out.event_strings.assign(1, std::string());   // 没有事件 ⇒ 池子也空（与"不写 chunk 23"读回来一致）
        }
        for (const SpineSourceEventData& e : src.events)
        {
            if (e.name.empty())
            {
                return Fail("bake: 有一个空名字的用户事件（参考实现拒绝）");
            }
            SpineGpuEventData d;
            d.name = InternString(out.event_strings, e.name);
            d.int_value = e.int_value;
            d.float_value = e.float_value;
            d.string_value = InternString(out.event_strings, e.string_value);
            d.audio_path = InternString(out.event_strings, e.audio_path);
            d.volume = e.volume;
            d.balance = e.balance;
            out.events.push_back(d);
        }

        // 第二趟：真正建表。
        for (size_t ai = 0; ai < src.animations.size(); ++ai)
        {
            const SpineSourceAnimation& sa = src.animations[ai];
            SpineGpuAnim rec;
            rec.name_offset = anim_name_off[ai];
            rec.timeline_first = static_cast<uint32_t>(out.timelines.size());
            rec.duration = 0.0f;
            rec.pad = 0;

            // ⚠️ 时间轴在容器里的顺序是**规范化**的：按 (type, target) 升序、稳定排序。
            // 理由不是好看，是两条输入路径的顺序**本来就不一样**：
            //   · 二进制：槽位 → 骨骼 → ik → transform → path → deform → drawOrder → event
            //     （SkeletonBinary.cpp:723 起 "// Slot timelines."，骨骼在 :795）
            //   · json：同一套顺序（SkeletonJson.cpp:806），但 bones / slots 各自内部的
            //     条目顺序取决于**文件里键的书写顺序**，与骨骼下标无关。
            // 不规范化 ⇒ 同一个动画从 json 和 .skel 进来会得到两份不同的字节，
            // 而"skel↔json 逐字节相同"是我们抓解读漂移的主要手段 ⇒ 必须规范。
            // 排序键 (type, target) 还顺手保住了语义上真有依赖的一对：
            // attachment(4) < deform(6) —— deform 只有在当前附件匹配时才生效。
            std::vector<size_t> order(sa.timelines.size());
            for (size_t i = 0; i < order.size(); ++i)
            {
                order[i] = i;
            }
            std::stable_sort(order.begin(), order.end(), [&sa](size_t a, size_t b) {
                const SpineSourceTimeline& ta = sa.timelines[a];
                const SpineSourceTimeline& tb = sa.timelines[b];
                if (ta.type != tb.type)
                {
                    return static_cast<uint16_t>(ta.type) < static_cast<uint16_t>(tb.type);
                }
                return ta.target < tb.target;
            });

            for (size_t oi = 0; oi < order.size(); ++oi)
            {
                const SpineSourceTimeline& tl = sa.timelines[order[oi]];
                const uint32_t entries = SpineTimelineEntries(tl.type);
                if (entries == 0)
                {
                    return Fail("bake: 动画 '" + sa.name + "' 里有一条未知类型的时间轴");
                }
                // 0 帧要单独判：0 是任何 entries 的整数倍，"不是整数倍"那条拦不住它，
                // 但参考实现 `frames[(frameCount-1)*ENTRIES]` 在 frameCount == 0 时就是
                // **负下标读**（CurveTimeline 的 assert 在 Release 下不存在）。
                if (tl.frames.empty())
                {
                    return Fail("bake: 动画 '" + sa.name + "' 有一条 0 帧的时间轴"
                                "（参考实现在这里是 frames[(frameCount-1)*ENTRIES] 的负下标读）");
                }
                if ((tl.frames.size() % entries) != 0u)
                {
                    return Fail("bake: 动画 '" + sa.name + "' 的一条时间轴有 " +
                                std::to_string(tl.frames.size()) + " 个 float，不是每帧 " +
                                std::to_string(entries) + " 个的整数倍");
                }
                const uint32_t frame_count = static_cast<uint32_t>(tl.frames.size()) / entries;
                if (frame_count == 0u)
                {
                    return Fail("bake: 动画 '" + sa.name + "' 有一条 0 帧的时间轴（内部错误）");
                }

                // ---- deform 的 target 要现场换算 ----
                int target = tl.target;
                if (tl.type == SpineTimelineType::kDeform)
                {
                    int hit = -1;
                    for (size_t k = 0; k < src.attachments.size(); ++k)
                    {
                        const SpineSourceAttachment& a = src.attachments[k];
                        if (a.skin_index == tl.deform_skin && a.slot_index == tl.deform_slot &&
                            a.name == tl.deform_attachment)
                        {
                            hit = static_cast<int>(k);
                            break;
                        }
                    }
                    if (hit < 0)
                    {
                        // ---- v12：指向 **path 附件** ⇒ 照样烘焙，target 置最高位 ----
                        // deform 会改 path 曲线 ⇒ 改 path 约束摆出来的骨骼
                        // （VertexAttachment.cpp:69-70 rigid 整体替换 / :117-118 weighted 相加）。
                        // chunk 20 在动画之前已经烘好，按 (皮肤, 槽位, 名字) 精确命中。
                        int path_hit = -1;
                        for (size_t k = 0; k < out.path_attachments.size(); ++k)
                        {
                            const SpineGpuPathAttachment& pa = out.path_attachments[k];
                            if (static_cast<int>(pa.skin_index) == tl.deform_skin &&
                                static_cast<int>(pa.slot_index) == tl.deform_slot &&
                                out.path_attachment_names[k] == tl.deform_attachment)
                            {
                                path_hit = static_cast<int>(k);
                                break;
                            }
                        }
                        if (path_hit >= 0)
                        {
                            if (path_hit > static_cast<int>(SpineGpuConstants::kDeformTargetIndexMask))
                            {
                                return Fail("bake: deform 时间轴指向第 " + std::to_string(path_hit) +
                                            " 条 path 附件，超出 target 低 14 位能表示的范围");
                            }
                            // 每帧 start + count 必须落在 deform 数组里：参考实现是
                            // `deform.setFixedSize(deformLength)` 后直接写 `deform[v]`
                            // （SkeletonBinary.cpp:992-1003），越界是 UB 不是截断。
                            const uint32_t deform_len =
                                SpineGpuPathDeformLength(out.path_attachments[static_cast<size_t>(path_hit)]);
                            for (const SpineSourceDeformFrame& df : tl.deform_frames)
                            {
                                if (static_cast<uint64_t>(df.start) + df.values.size() > deform_len)
                                {
                                    return Fail("bake: path 附件 '" + tl.deform_attachment + "' 的 deform 帧写到 [" +
                                                std::to_string(df.start) + ", " +
                                                std::to_string(df.start + df.values.size()) + ")，超出它的 deform 长度 " +
                                                std::to_string(deform_len));
                                }
                            }
                            target = static_cast<int>(SpineGpuConstants::kDeformTargetPathBit) | path_hit;
                        }
                        else if (const int clip_hit = FindClipRow(out, tl.deform_skin, tl.deform_slot,
                                                                  tl.deform_attachment);
                                 clip_hit >= 0)
                        {
                            // ---- v15：指向 **裁剪附件** ⇒ 照样烘焙，target 置 clip 位 ----
                            // deform 改的是裁剪多边形（clipStart 里 `computeWorldVertices(slot, ...)`
                            // 读槽位的 deform 数组）⇒ 不改骨骼，但改**画面**。
                            if (clip_hit > static_cast<int>(SpineGpuConstants::kDeformTargetIndexMask))
                            {
                                return Fail("bake: deform 时间轴指向第 " + std::to_string(clip_hit) +
                                            " 条裁剪多边形，超出 target 低 14 位能表示的范围");
                            }
                            const uint32_t deform_len =
                                SpineGpuClipDeformLength(out, out.clip_polygons[static_cast<size_t>(clip_hit)]);
                            for (const SpineSourceDeformFrame& df : tl.deform_frames)
                            {
                                if (static_cast<uint64_t>(df.start) + df.values.size() > deform_len)
                                {
                                    return Fail("bake: 裁剪附件 '" + tl.deform_attachment + "' 的 deform 帧写到 [" +
                                                std::to_string(df.start) + ", " +
                                                std::to_string(df.start + df.values.size()) +
                                                ")，超出它的 deform 长度 " + std::to_string(deform_len));
                                }
                            }
                            target = static_cast<int>(SpineGpuConstants::kDeformTargetClipBit) | clip_hit;
                        }
                        else
                        {
                            // 指向一个**不产生绘制项、也不是 path / 裁剪**的附件（boundingbox /
                            // point）⇒ deform 不改任何姿势、也不改画面 ⇒ 整条时间轴不烘焙。
                            // 这不是静默丢弃：渲染与骨骼结果都**完全相同**。
                            bool no_draw = false;
                            for (const SpineSourceNoDrawAttachment& s : src.no_draw_attachments)
                            {
                                if (s.skin_index == tl.deform_skin && s.slot_index == tl.deform_slot &&
                                    s.name == tl.deform_attachment)
                                {
                                    no_draw = true;
                                    break;
                                }
                            }
                            if (!no_draw)
                            {
                                return Fail("bake: deform 时间轴指向的附件 '" + tl.deform_attachment +
                                            "' 找不到对应的附件记录（它既不产生绘制项、也不在"
                                            "不渲染附件表里）");
                            }
                            continue;
                        }
                    }
                    else
                    {
                        if (hit > static_cast<int>(SpineGpuConstants::kDeformTargetIndexMask))
                        {
                            return Fail("bake: deform 时间轴指向第 " + std::to_string(hit) +
                                        " 条附件记录，超出 target 低 14 位（高两位留给 path / 裁剪附件）");
                        }
                        target = hit;
                    }
                }

                SpineGpuTimeline t;
                t.type = static_cast<uint16_t>(tl.type);
                if (target < 0 || target > 0xFFFF)
                {
                    return Fail("bake: 时间轴的 target " + std::to_string(target) + " 超出 uint16");
                }
                t.target = static_cast<uint16_t>(target);
                t.frame_count = frame_count;
                // ⚠️ `frame_first` 是 **float 下标**（字节 / 4），不是"第几帧"。
                //    每类时间轴的每帧 float 数（entries）**不一样**（rotate 2、color 5…），
                //    所以不存在一个全局统一的"帧"单位 —— 硬凑一个就会让非首条的
                //    时间轴读到别人的帧数据，而偏移 0 那条看不出问题。
                t.frame_first = static_cast<uint32_t>(out.frames.size());
                t.curve_first = static_cast<uint32_t>(out.curve_types.size());
                t.bezier_first = static_cast<uint32_t>(out.beziers.size());
                t.bezier_count = 0;
                t.payload_offset = static_cast<uint32_t>(out.timeline_payload.size());
                t.payload_count = 0;

                // ---- 帧（定长）----
                for (float f : tl.frames)
                {
                    out.frames.push_back(f);
                }

                // ---- 曲线段 ----
                // 段数 = frame_count - 1；attachment / drawOrder **没有**曲线。
                const size_t want_segments = SpineTimelineHasCurve(tl.type) ? (frame_count - 1u) : 0u;
                if (tl.curves.size() != want_segments)
                {
                    return Fail("bake: 动画 '" + sa.name + "' 的一条时间轴有 " +
                                std::to_string(tl.curves.size()) + " 段曲线，但 " +
                                std::to_string(frame_count) + " 帧应该有 " +
                                std::to_string(want_segments) + " 段");
                }
                for (const SpineSourceCurve& c : tl.curves)
                {
                    out.curve_types.push_back(static_cast<uint8_t>(c.type));
                    if (c.type == SpineCurveType::kBezier)
                    {
                        SpineGpuBezier b;
                        b.cx1 = c.cx1;
                        b.cy1 = c.cy1;
                        b.cx2 = c.cx2;
                        b.cy2 = c.cy2;
                        out.beziers.push_back(b);
                        ++t.bezier_count;
                    }
                }

                // ---- 变长载荷 ----
                if (tl.type == SpineTimelineType::kAttachment)
                {
                    if (tl.attachment_names.size() != frame_count)
                    {
                        return Fail("bake: 动画 '" + sa.name + "' 的 attachment 时间轴帧数对不上");
                    }
                    for (const std::string& n : tl.attachment_names)
                    {
                        if (n.empty())
                        {
                            // ⚠️ 空名 = 隐藏该槽位，哨兵是 kNoAttachmentName **不是 0**：
                            // 0 是字符串表里第一个名字的合法偏移。
                            AppendU32To(out.timeline_payload, SpineGpuConstants::kNoAttachmentName);
                        }
                        else
                        {
                            const uint32_t idx = InternString(out.anim_attachment_names, n);
                            AppendU32To(out.timeline_payload, anim_att_off[idx]);
                        }
                    }
                    t.payload_count = frame_count;
                }
                else if (tl.type == SpineTimelineType::kDeform)
                {
                    if (tl.deform_frames.size() != frame_count)
                    {
                        return Fail("bake: 动画 '" + sa.name + "' 的 deform 时间轴帧数对不上");
                    }
                    for (const SpineSourceDeformFrame& df : tl.deform_frames)
                    {
                        AppendU32To(out.timeline_payload, df.start);
                        AppendU32To(out.timeline_payload, static_cast<uint32_t>(df.values.size()));
                        for (float f : df.values)
                        {
                            AppendF32To(out.timeline_payload, f);
                        }
                    }
                    t.payload_count = frame_count;
                }
                else if (tl.type == SpineTimelineType::kDrawOrder)
                {
                    if (tl.draw_order_frames.size() != frame_count)
                    {
                        return Fail("bake: 动画 '" + sa.name + "' 的 drawOrder 时间轴帧数对不上");
                    }
                    for (const SpineSourceDrawOrderFrame& dof : tl.draw_order_frames)
                    {
                        // ⚠️ 槽位下标必须**严格升序**。参考实现的重建循环是
                        //    `while (originalIndex != slotIndex) unchanged[ui++] = originalIndex++;`
                        //    （`SkeletonBinary.cpp:1050-1053` / `SkeletonJson.cpp:1144-1147`）：
                        //    下一个 slotIndex 小于当前 originalIndex 时那个 while **永不退出**，
                        //    一路写穿 `unchanged` ⇒ 死循环 + 越界写，不是"结果略有不同的容错"。
                        //    ⇒ 我们在这里显式报错，绝不静默重排（重排会改变绘制顺序的语义）。
                        for (size_t k = 1; k < dof.slots.size(); ++k)
                        {
                            if (dof.slots[k] <= dof.slots[k - 1])
                            {
                                return Fail("bake: 动画 '" + sa.name + "' 的 drawOrder 时间轴槽位下标不是严格升序（" +
                                            std::to_string(dof.slots[k - 1]) + " → " + std::to_string(dof.slots[k]) +
                                            "）—— 参考实现在这里是死循环 + 越界写");
                            }
                        }
                        AppendU32To(out.timeline_payload, static_cast<uint32_t>(dof.slots.size()));
                        for (size_t k = 0; k < dof.slots.size(); ++k)
                        {
                            AppendU32To(out.timeline_payload, dof.slots[k]);
                            // ⚠️ offset 是**有符号**的（参考实现读的是 zigzag varint，
                            //    `drawOrder[index + readVarint()] = ...`，可以是负的）。
                            //    按 u32 的**位模式**存，读端照 int32 解释回来 ——
                            //    二进制补码下这是无损的，但"存成 u32"这件事必须写清楚，
                            //    否则后来者会以为是"忘了符号"而改成"取绝对值"。
                            AppendU32To(out.timeline_payload, static_cast<uint32_t>(dof.offsets[k]));
                        }
                    }
                    t.payload_count = frame_count;
                }
                else if (tl.type == SpineTimelineType::kEvent)
                {
                    if (tl.event_keys.size() != frame_count)
                    {
                        return Fail("bake: 动画 '" + sa.name + "' 的 event 时间轴帧数对不上");
                    }
                    for (const SpineSourceEventKey& k : tl.event_keys)
                    {
                        if (k.event >= out.events.size())
                        {
                            return Fail("bake: 动画 '" + sa.name + "' 的 event 时间轴引用了不存在的事件 " +
                                        std::to_string(k.event));
                        }
                        AppendU32To(out.timeline_payload, k.event);
                        AppendU32To(out.timeline_payload, static_cast<uint32_t>(k.int_value));
                        AppendF32To(out.timeline_payload, k.float_value);
                        AppendU32To(out.timeline_payload, InternString(out.event_strings, k.string_value));
                        AppendF32To(out.timeline_payload, k.volume);
                        AppendF32To(out.timeline_payload, k.balance);
                    }
                    t.payload_count = frame_count;
                }

                // duration = max(各时间轴最后一帧的 time)，抄参考实现（v16 起事件时间轴也算在内，
                // 与 SkeletonBinary.cpp:1106 / SkeletonJson.cpp:1197 一致）。
                const float last_time = tl.frames[(frame_count - 1u) * entries];
                if (last_time > rec.duration)
                {
                    rec.duration = last_time;
                }

                out.timelines.push_back(t);
            }

            rec.timeline_count = static_cast<uint32_t>(out.timelines.size()) - rec.timeline_first;
            out.anims.push_back(rec);
        }
    }

    // ---- header ----
    SpineGpuHeader& h = out.header;
    h = SpineGpuHeader {};
    h.magic = SpineGpuConstants::kMagic;
    h.version = SpineGpuConstants::kFormatVersion;
    // **写实**：按源版本写 30700 / 30800。把 3.7 的资产记成 30800，读端就会按 3.8 的
    // 判据解释槽位暗色，而那个差别不报错，只是少数槽位颜色不对。
    h.spine_runtime = (src.version == SpineSourceVersion::k37) ? SpineGpuConstants::kSpineRuntime37
                                                               : SpineGpuConstants::kSpineRuntime38;
    h.flags = SpineGpuFlags::kNone;
    if (src.has_draw_order_timeline)
    {
        h.flags |= SpineGpuFlags::kHasDrawOrderTimeline;
    }
    if (src.uses_tint_black)
    {
        h.flags |= SpineGpuFlags::kUsesTintBlack;
    }
    if (src.has_clipping)
    {
        h.flags |= SpineGpuFlags::kHasClipping;
    }
    // 有绘制项因 deform 走了 CPU fallback ⇒ 运行时得为它准备每帧动态 VB
    // （deform 是**逐帧**改顶点，所以动态 VB 每帧都要重算）。
    // ⚠️ 不能扫 draw_items 的 kind —— kFallback 还有**裁剪**这个来源，而裁剪的
    // 动态 VB 只在裁剪多边形动的时候才需要重算。把两者混成一个位，运行时就会
    // 为纯裁剪的资产每帧重算一份根本没变的顶点。
    for (const SpineSourceAttachment& a : src.attachments)
    {
        if (a.deformed)
        {
            h.flags |= SpineGpuFlags::kHasDeform;
            break;
        }
    }
    if (src.has_event_timeline)
    {
        // 事件**没进容器**（chunk 11-16 里没有 event 时间轴），但"这个资产有事件"
        // 这件事必须留在容器里 —— 否则读端无法区分"没有事件"与"被我们丢了"。
        h.flags |= SpineGpuFlags::kHasEventTimeline;
    }
    h.bone_count = bone_count;
    h.slot_count = slot_count;
    h.skin_count = static_cast<uint32_t>(src.skins.size());
    h.draw_item_count = static_cast<uint32_t>(out.draw_items.size());
    h.vertex_count = rigid_pool.unit_count() + weighted_pool.unit_count();
    h.index_count = static_cast<uint32_t>(out.index_buffer.size());
    h.atlas_page_count = static_cast<uint32_t>(out.atlas_page_names.size());
    h.bone_stride = SpineGpuConstants::kBoneStrideCompact;
    h.chunk_count = 0;   // WriteSpineGpuContainer() 填

    return Ok();
}

// =====================================================================
// 容器序列化（规格 §9）
// =====================================================================
namespace
{

struct ChunkBuilder
{
    uint32_t id = 0;
    std::vector<uint8_t> bytes;
    uint32_t alignment = SpineGpuConstants::kChunkAlignment;
};

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
}

void PutU32To(uint8_t* p, uint32_t x)
{
    p[0] = static_cast<uint8_t>(x & 0xFFu);
    p[1] = static_cast<uint8_t>((x >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((x >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((x >> 24) & 0xFFu);
}

uint32_t GetU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t GetU16(const uint8_t* p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint8_t GetU8(const uint8_t* p)
{
    return p[0];
}

float GetF32(const uint8_t* p)
{
    float x = 0.0f;
    std::memcpy(&x, p, sizeof(float));
    return x;
}

void PutU16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
}

void PutU8(std::vector<uint8_t>& v, uint8_t x)
{
    v.push_back(x);
}

// float 按**小端**逐字节写 —— 与别的 float 块（chunk 4 / 21 走 PutBytes）同一个约定。
// 不 memcpy 整个结构体：那会把"结构体有没有尾部填充"变成一个隐式前提
// （sizeof 断言能挡住，但逐字段写更不容易被以后加字段的人踩到）。
void PutF32(std::vector<uint8_t>& v, float x)
{
    uint8_t b[4];
    std::memcpy(b, &x, sizeof(float));
    v.insert(v.end(), b, b + 4);
}

void PutBytes(std::vector<uint8_t>& v, const void* src, size_t n)
{
    const uint8_t* p = static_cast<const uint8_t*>(src);
    v.insert(v.end(), p, p + n);
}

void PutCString(std::vector<uint8_t>& v, const std::string& s)
{
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
}

uint32_t AlignUp(uint32_t x, uint32_t a)
{
    return (x + a - 1) / a * a;
}

}   // namespace

SpineImportResult WriteSpineGpuContainer(const SpineGpuAsset& asset, std::vector<uint8_t>& out)
{
    out.clear();

    // ---- 组装各 chunk ----
    std::vector<ChunkBuilder> chunks;

    // id=1 strings：骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名 → 动画名 → 动画附件名
    //                        ↑ 皮肤名插在这里：读端靠 header.skin_count 切分
    // 最后两组是 v9 加的：动画名按 chunk 11 的条目数切分，**动画附件名只按偏移访问**
    // （attachment 时间轴的载荷里存的是偏移不是下标）。
    // ⚠️ 组装逻辑只有 `BuildSpineStringTable()` 一份 —— Bake 算偏移用的也是它，
    //    两边分叉会让 chunk 11 的 name_offset 指到别人的名字上，且不报错。
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkStrings;
        std::vector<uint32_t> slot_setup_att_off;
        std::vector<uint32_t> anim_name_off;
        std::vector<uint32_t> anim_att_off;
        std::vector<uint32_t> path_att_off;
        std::vector<uint32_t> clip_name_off;
        BuildSpineStringTable(asset, c.bytes, slot_setup_att_off, anim_name_off, anim_att_off,
                              path_att_off, clip_name_off);
        if (!c.bytes.empty())
        {
            chunks.push_back(std::move(c));
        }
    }

    // id=2 slot_bone
    if (!asset.slot_bone.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkSlotBone;
        for (uint16_t b : asset.slot_bone)
        {
            PutU16(c.bytes, b);
        }
        chunks.push_back(std::move(c));
    }

    // id=10 bone_setup（36 B/骨）。下标 = header.bone_count 的序。
    // 落盘位置紧挨 slot_bone：两者都是"骨架的静态描述"，读起来是一段。
    if (!asset.bones.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkBoneSetup;
        for (const SpineGpuBone& b : asset.bones)
        {
            PutF32(c.bytes, b.x);
            PutF32(c.bytes, b.y);
            PutF32(c.bytes, b.rotation);
            PutF32(c.bytes, b.scale_x);
            PutF32(c.bytes, b.scale_y);
            PutF32(c.bytes, b.shear_x);
            PutF32(c.bytes, b.shear_y);
            PutF32(c.bytes, b.length);
            PutU16(c.bytes, static_cast<uint16_t>(static_cast<int16_t>(b.parent)));
            PutU8(c.bytes, b.transform_mode);
            PutU8(c.bytes, b.pad);   // 显式写 0（跟裁剪多边形同一个规矩）
        }
        chunks.push_back(std::move(c));
    }

    // ---- v9：动画（id 11..16，紧跟骨骼 setup 后面）----
    // 五块之间全是**下标**引用，落盘顺序无所谓，但读端要按"先目录后表"的顺序校验。
    if (!asset.anims.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkAnimDir;
        for (const SpineGpuAnim& a : asset.anims)
        {
            PutU32(c.bytes, a.name_offset);
            PutU32(c.bytes, a.timeline_first);
            PutU32(c.bytes, a.timeline_count);
            PutF32(c.bytes, a.duration);
            PutU32(c.bytes, a.pad);
        }
        chunks.push_back(std::move(c));
    }
    if (!asset.timelines.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkTimelines;
        for (const SpineGpuTimeline& t : asset.timelines)
        {
            PutU16(c.bytes, t.type);
            PutU16(c.bytes, t.target);
            PutU32(c.bytes, t.frame_count);
            PutU32(c.bytes, t.frame_first);
            PutU32(c.bytes, t.curve_first);
            PutU32(c.bytes, t.bezier_first);
            PutU32(c.bytes, t.bezier_count);
            PutU32(c.bytes, t.payload_offset);
            PutU32(c.bytes, t.payload_count);
        }
        chunks.push_back(std::move(c));
    }
    if (!asset.frames.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkFrames;
        PutBytes(c.bytes, asset.frames.data(), asset.frames.size() * sizeof(float));
        chunks.push_back(std::move(c));
    }
    if (!asset.curve_types.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkCurveTypes;
        for (uint8_t t : asset.curve_types)
        {
            PutU8(c.bytes, t);
        }
        chunks.push_back(std::move(c));
    }
    if (!asset.beziers.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkBeziers;
        for (const SpineGpuBezier& b : asset.beziers)
        {
            PutF32(c.bytes, b.cx1);
            PutF32(c.bytes, b.cy1);
            PutF32(c.bytes, b.cx2);
            PutF32(c.bytes, b.cy2);
        }
        chunks.push_back(std::move(c));
    }
    if (!asset.timeline_payload.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkTimelinePayload;
        PutBytes(c.bytes, asset.timeline_payload.data(), asset.timeline_payload.size());
        chunks.push_back(std::move(c));
    }

    // id=17 slot_setup（40 B/槽，v10）。紧跟动画块后面：两者都是"解算需要的静态输入"。
    // 逐字段写（不 memcpy 整个结构体）—— 加字段的人因此必须同时改这里和 static_assert。
    if (!asset.slots.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkSlotSetup;
        for (const SpineGpuSlot& s : asset.slots)
        {
            PutF32(c.bytes, s.color_r);
            PutF32(c.bytes, s.color_g);
            PutF32(c.bytes, s.color_b);
            PutF32(c.bytes, s.color_a);
            PutF32(c.bytes, s.dark_r);
            PutF32(c.bytes, s.dark_g);
            PutF32(c.bytes, s.dark_b);
            PutF32(c.bytes, s.dark_a);
            PutU32(c.bytes, s.setup_attachment_name_offset);
            PutU32(c.bytes, s.flags);
        }
        chunks.push_back(std::move(c));
    }

    // id=18 约束 setup（v11）。三段定长数组 + 尾部共用的 u16 骨骼索引池。
    // ⚠️ 块头里的三个 count 是**必需的**：空 chunk 不落盘，"chunk 不存在"读端看不出
    //    是"真没有"还是"被截断了"。有了 count 就能校验（同 chunk 10 / 11 / 17 的坑）。
    const bool has_constraints =
        !asset.iks.empty() || !asset.transforms.empty() || !asset.paths.empty();
    if (has_constraints)
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkConstraintSetup;
        PutU32(c.bytes, static_cast<uint32_t>(asset.iks.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.transforms.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.paths.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.constraint_bone_pool.size()));
        for (const SpineGpuIkConstraint& k : asset.iks)
        {
            PutU16(c.bytes, k.order);
            PutU16(c.bytes, k.bone_count);
            PutU32(c.bytes, k.bone_first);
            PutU32(c.bytes, k.target);
            PutF32(c.bytes, k.mix);
            PutU16(c.bytes, static_cast<uint16_t>(static_cast<int16_t>(k.bend_dir)));
            PutU16(c.bytes, k.flags);
        }
        for (const SpineGpuTransformConstraint& t : asset.transforms)
        {
            PutU16(c.bytes, t.order);
            PutU16(c.bytes, t.bone_count);
            PutU32(c.bytes, t.bone_first);
            PutU32(c.bytes, t.target);
            PutF32(c.bytes, t.offset_rotation);
            PutF32(c.bytes, t.offset_x);
            PutF32(c.bytes, t.offset_y);
            PutF32(c.bytes, t.offset_scale_x);
            PutF32(c.bytes, t.offset_scale_y);
            PutF32(c.bytes, t.offset_shear_y);
            PutF32(c.bytes, t.rotate_mix);
            PutF32(c.bytes, t.translate_mix);
            PutF32(c.bytes, t.scale_mix);
            PutF32(c.bytes, t.shear_mix);
            PutU16(c.bytes, t.flags);
            PutU16(c.bytes, t.pad);
        }
        for (const SpineGpuPathConstraint& p : asset.paths)
        {
            PutU16(c.bytes, p.order);
            PutU16(c.bytes, p.bone_count);
            PutU32(c.bytes, p.bone_first);
            PutU32(c.bytes, p.target_slot);
            PutU8(c.bytes, p.position_mode);
            PutU8(c.bytes, p.spacing_mode);
            PutU8(c.bytes, p.rotate_mode);
            PutU8(c.bytes, p.pad);
            PutF32(c.bytes, p.offset_rotation);
            PutF32(c.bytes, p.position);
            PutF32(c.bytes, p.spacing);
            PutF32(c.bytes, p.rotate_mix);
            PutF32(c.bytes, p.translate_mix);
            PutU32(c.bytes, p.path_index);
        }
        for (uint16_t b : asset.constraint_bone_pool)
        {
            PutU16(c.bytes, b);
        }
        chunks.push_back(std::move(c));
    }

    // id=19 执行顺序（v11）。`Skeleton::updateCache()` 的结果**原样落盘**。
    // ⚠️ 只在**有约束**时落：无约束时 updateCache 的结果恒等于 0..bone_count-1
    //    （parent < index 的格式不变量 ⇒ 正向遍历即拓扑序），读端按这个重建。
    //    这不是"兜底"，是可证的等价 —— 少写 32036 × 4 B 的纯冗余数据。
    if (has_constraints)
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkUpdateOrder;
        PutU32(c.bytes, static_cast<uint32_t>(asset.update_steps.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.update_reset.size()));
        for (const SpineGpuUpdateStep& s : asset.update_steps)
        {
            PutU8(c.bytes, s.kind);
            PutU8(c.bytes, s.pad);
            PutU16(c.bytes, s.index);
        }
        for (uint16_t b : asset.update_reset)
        {
            PutU16(c.bytes, b);
        }
        chunks.push_back(std::move(c));
    }

    // id=20 PathAttachment 几何（v11）。
    if (!asset.path_attachments.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkPathAttachments;
        PutU32(c.bytes, static_cast<uint32_t>(asset.path_attachments.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.path_bone_pool.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.path_vertices.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.path_lengths.size()));
        for (const SpineGpuPathAttachment& p : asset.path_attachments)
        {
            PutU16(c.bytes, p.slot_index);
            PutU16(c.bytes, p.skin_index);
            PutU16(c.bytes, p.flags);
            PutU16(c.bytes, p.pad);
            PutU32(c.bytes, p.vertex_count);
            PutU32(c.bytes, p.bone_offset);
            PutU32(c.bytes, p.bone_count);
            PutU32(c.bytes, p.vertex_offset);
            PutU32(c.bytes, p.vertex_float_count);
            PutU32(c.bytes, p.length_offset);
            PutU32(c.bytes, p.length_count);
            PutU32(c.bytes, p.name_offset);
        }
        for (uint16_t b : asset.path_bone_pool)
        {
            PutU16(c.bytes, b);
        }
        PutBytes(c.bytes, asset.path_vertices.data(), asset.path_vertices.size() * sizeof(float));
        PutBytes(c.bytes, asset.path_lengths.data(), asset.path_lengths.size() * sizeof(float));
        chunks.push_back(std::move(c));
    }

    // id=3 vertex_buffer（段起点 8 B 对齐 ⇒ chunk 本身也要 8 对齐）
    if (!asset.vertex_buffer.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkVertexBuffer;
        c.alignment = SpineGpuConstants::kVertexSegmentAlignment;
        PutBytes(c.bytes, asset.vertex_buffer.data(), asset.vertex_buffer.size());
        chunks.push_back(std::move(c));
    }

    // id=21 influence_bind —— float2 × 影响条目数，**8 B 对齐**（Load2 读 bindPos）。
    // 与 id=22 **共用同一个条目下标**（就是权重顶点的 influence_offset）。
    if (!asset.influence_bind.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkInfluenceBind;
        c.alignment = SpineGpuConstants::kVertexSegmentAlignment;
        PutBytes(c.bytes, asset.influence_bind.data(), asset.influence_bind.size() * sizeof(float));
        chunks.push_back(std::move(c));
    }

    // id=22 influence_bone —— SpineInfluenceBone[]（4 B/条）。
    // 未用字节（pad）必须显式写 0 —— 跟裁剪多边形一样整块清零再拷，不靠 struct 布局。
    if (!asset.influence_bone.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkInfluenceBone;
        for (const SpineInfluenceBone& b : asset.influence_bone)
        {
            uint8_t raw[sizeof(SpineInfluenceBone)];
            std::memset(raw, 0, sizeof(raw));
            std::memcpy(raw, &b, sizeof(b));
            PutBytes(c.bytes, raw, sizeof(raw));
        }
        chunks.push_back(std::move(c));
    }

    // id=4 uv_buffer —— float2 × uv_count，**8 B 对齐**（与顶点一样靠 Load2 读取）
    if (!asset.uv_buffer.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkUvBuffer;
        c.alignment = SpineGpuConstants::kVertexSegmentAlignment;
        PutBytes(c.bytes, asset.uv_buffer.data(), asset.uv_buffer.size() * sizeof(float));
        chunks.push_back(std::move(c));
    }

    // id=5 index_buffer
    if (!asset.index_buffer.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkIndexBuffer;
        for (uint16_t i : asset.index_buffer)
        {
            PutU16(c.bytes, i);
        }
        chunks.push_back(std::move(c));
    }

    // id=9 clip_polygons —— 20 B/条（v15）。**顶点本身在 chunk 3 里**（裁剪附件是
    // VertexAttachment，顶点与 mesh 顶点同构，连去重都共用同一个池子）。
    if (!asset.clip_polygons.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkClipData;
        for (const SpineGpuClipPolygon& p : asset.clip_polygons)
        {
            uint8_t raw[sizeof(SpineGpuClipPolygon)];
            std::memset(raw, 0, sizeof(raw));
            std::memcpy(raw, &p, sizeof(p));
            PutBytes(c.bytes, raw, sizeof(raw));
        }
        chunks.push_back(std::move(c));
    }

    // id=6 draw_items —— **32 B/项**（v14），未用字节显式写 0（规格 §4）
    if (!asset.draw_items.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkDrawItems;
        for (const SpineDrawItem& d : asset.draw_items)
        {
            uint8_t raw[sizeof(SpineDrawItem)];
            std::memset(raw, 0, sizeof(raw));
            std::memcpy(raw, &d, sizeof(d));
            PutBytes(c.bytes, raw, sizeof(raw));
        }
        chunks.push_back(std::move(c));
    }

    // id=7 attachments —— 8 B/项：(槽位, 附件 id, 皮肤 id)
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkAttachments;
        for (const SpineGpuAttachmentRecord& r : asset.attachment_records)
        {
            uint8_t raw[sizeof(SpineGpuAttachmentRecord)];
            std::memset(raw, 0, sizeof(raw));
            std::memcpy(raw, &r, sizeof(r));
            PutBytes(c.bytes, raw, sizeof(raw));
        }
        if (!c.bytes.empty())
        {
            chunks.push_back(std::move(c));
        }
    }

    // id=8 atlas_pages
    if (!asset.atlas_page_names.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkAtlasPages;
        for (const std::string& s : asset.atlas_page_names)
        {
            PutCString(c.bytes, s);
        }
        chunks.push_back(std::move(c));
    }

    // id=23 events（v16）—— 没有事件就不写（读端"不存在" = 0 个事件）。
    // 布局：SpineGpuEventChunkHeader + 定义 × 28 B + 字符串偏移 × u32 + blob（补齐到 4 B）。
    if (!asset.events.empty())
    {
        if (asset.event_strings.empty() || !asset.event_strings[0].empty())
        {
            return Fail("container: 事件字符串池第 0 条必须是空串");
        }
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkEvents;
        std::vector<uint8_t> blob;
        std::vector<uint32_t> string_offsets;
        for (const std::string& s : asset.event_strings)
        {
            string_offsets.push_back(static_cast<uint32_t>(blob.size()));
            PutCString(blob, s);
        }
        PutU32(c.bytes, static_cast<uint32_t>(asset.events.size()));
        PutU32(c.bytes, static_cast<uint32_t>(asset.event_strings.size()));
        PutU32(c.bytes, static_cast<uint32_t>(blob.size()));
        PutU32(c.bytes, 0u);
        for (const SpineGpuEventData& e : asset.events)
        {
            if (e.name >= asset.event_strings.size() || e.string_value >= asset.event_strings.size() ||
                e.audio_path >= asset.event_strings.size())
            {
                return Fail("container: 事件定义的字符串下标越界");
            }
            PutU32(c.bytes, e.name);
            PutU32(c.bytes, static_cast<uint32_t>(e.int_value));
            PutF32(c.bytes, e.float_value);
            PutU32(c.bytes, e.string_value);
            PutU32(c.bytes, e.audio_path);
            PutF32(c.bytes, e.volume);
            PutF32(c.bytes, e.balance);
        }
        for (uint32_t off : string_offsets)
        {
            PutU32(c.bytes, off);
        }
        PutBytes(c.bytes, blob.data(), blob.size());
        while ((c.bytes.size() & 3u) != 0u)
        {
            c.bytes.push_back(0u);
        }
        chunks.push_back(std::move(c));
    }

    // ---- 布局：header(64) + chunk table(16×n) + payloads ----
    const uint32_t table_bytes = static_cast<uint32_t>(chunks.size()) * SpineGpuConstants::kChunkTableEntrySize;
    uint32_t cursor = SpineGpuConstants::kHeaderSize + table_bytes;

    std::vector<uint32_t> offsets(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        cursor = AlignUp(cursor, chunks[i].alignment);
        offsets[i] = cursor;
        cursor += static_cast<uint32_t>(chunks[i].bytes.size());
    }

    out.assign(cursor, 0u);

    SpineGpuHeader h = asset.header;
    h.magic = SpineGpuConstants::kMagic;
    h.version = SpineGpuConstants::kFormatVersion;
    h.chunk_count = static_cast<uint32_t>(chunks.size());
    h.rigid_vertex_count = asset.rigid_vertex_count;
    h.weighted_segment_offset = asset.weighted_segment_offset;
    h.weighted_vertex_count = asset.weighted_vertex_count;
    h.skin_count = static_cast<uint32_t>(asset.skin_names.size());
    std::memcpy(out.data(), &h, sizeof(h));

    for (size_t i = 0; i < chunks.size(); ++i)
    {
        uint8_t* entry = out.data() + SpineGpuConstants::kHeaderSize +
                         static_cast<size_t>(i) * SpineGpuConstants::kChunkTableEntrySize;
        PutU32To(entry, chunks[i].id);
        PutU32To(entry + 4, offsets[i]);
        PutU32To(entry + 8, static_cast<uint32_t>(chunks[i].bytes.size()));
        PutU32To(entry + 12, 0u);
        if (!chunks[i].bytes.empty())
        {
            std::memcpy(out.data() + offsets[i], chunks[i].bytes.data(), chunks[i].bytes.size());
        }
    }

    return Ok();
}

SpineImportResult ReadSpineGpuContainer(const uint8_t* data, size_t len, SpineGpuAsset& out)
{
    out = SpineGpuAsset {};

    if (len < SpineGpuConstants::kHeaderSize)
    {
        return Fail("container: 长度 " + std::to_string(len) + " < header 64 B");
    }

    std::memcpy(&out.header, data, sizeof(SpineGpuHeader));
    if (out.header.magic != SpineGpuConstants::kMagic)
    {
        return Fail("container: 魔数不是 'ZSPG'");
    }
    if (out.header.version != SpineGpuConstants::kFormatVersion)
    {
        return Fail("container: 版本 " + std::to_string(out.header.version) + " != " +
                    std::to_string(SpineGpuConstants::kFormatVersion) +
                    "。规格 §11：版本变了必须明确拒绝，不做静默兼容。");
    }

    const uint32_t n = out.header.chunk_count;
    const uint64_t need = static_cast<uint64_t>(SpineGpuConstants::kHeaderSize) +
                          static_cast<uint64_t>(n) * SpineGpuConstants::kChunkTableEntrySize;
    if (need > len)
    {
        return Fail("container: chunk 表越界");
    }

    struct Found
    {
        uint32_t id = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };
    std::vector<Found> found;
    found.reserve(n);

    for (uint32_t i = 0; i < n; ++i)
    {
        const uint8_t* e = data + SpineGpuConstants::kHeaderSize +
                           static_cast<size_t>(i) * SpineGpuConstants::kChunkTableEntrySize;
        Found f;
        f.id = GetU32(e);
        f.offset = GetU32(e + 4);
        f.size = GetU32(e + 8);
        if (static_cast<uint64_t>(f.offset) + f.size > len)
        {
            return Fail("container: chunk " + std::to_string(f.id) + " 越界");
        }
        found.push_back(f);
    }

    auto find_chunk = [&](uint32_t id) -> const Found* {
        for (const Found& f : found)
        {
            if (f.id == id)
            {
                return &f;
            }
        }
        return nullptr;
    };

    // ---- 顶点 ----
    out.rigid_vertex_count = out.header.rigid_vertex_count;
    out.weighted_segment_offset = out.header.weighted_segment_offset;
    out.weighted_vertex_count = out.header.weighted_vertex_count;

    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkVertexBuffer))
    {
        out.vertex_buffer.assign(data + f->offset, data + f->offset + f->size);
    }

    // ---- 骨骼 setup（chunk 10，36 B/骨）----
    // ⚠️ "chunk 不存在"本身不报错（空 chunk 不落盘），所以必须拿 header.bone_count 对：
    // 少了它，读端会得到 0 根骨骼，而后面所有 bone_idx 全部越界 —— 越界在 shader 里
    // 不报错，只是蒙皮取到 0 号骨骼。这是加 chunk 10 时最需要钉住的一条。
    {
        const Found* f = find_chunk(SpineGpuChunkId::kChunkBoneSetup);
        if (out.header.bone_count != 0 && f == nullptr)
        {
            return Fail("container: header 说有 " + std::to_string(out.header.bone_count) +
                        " 根骨骼，但没有 chunk 10（bone_setup）—— v8 起骨骼 setup 必须落盘，"
                        "缺了它动画就无从解算（时间轴是相对 setup 的增量）");
        }
        if (f != nullptr)
        {
            if (f->size % SpineGpuConstants::kBoneSetupSize != 0)
            {
                return Fail("container: bone_setup 大小不是 " +
                            std::to_string(SpineGpuConstants::kBoneSetupSize) +
                            " B 的整数倍（v8 起一条是 36 B）");
            }
            const uint32_t count = f->size / SpineGpuConstants::kBoneSetupSize;
            if (count != out.header.bone_count)
            {
                return Fail("container: bone_setup 有 " + std::to_string(count) + " 条，但 header.bone_count 是 " +
                            std::to_string(out.header.bone_count));
            }
            out.bones.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t* p = data + f->offset + static_cast<size_t>(i) * SpineGpuConstants::kBoneSetupSize;
                SpineGpuBone b;
                // 8 个 float 连续，且与顶点缓冲同一约定（IEEE754 binary32 小端）。
                std::memcpy(&b.x, p, 8u * sizeof(float));
                b.parent = static_cast<int16_t>(static_cast<uint16_t>(p[32] | (static_cast<uint16_t>(p[33]) << 8)));
                b.transform_mode = p[34];
                b.pad = p[35];

                if (b.pad != 0)
                {
                    return Fail("container: 第 " + std::to_string(i) +
                                " 根骨骼的 pad 不是 0（落盘必须清零 —— 脏字节会让跨语言逐字节对拍对不上）");
                }
                // **parent < i**：容器靠这条不变量做单趟正向求解。重排过的表不报错，
                // 只是"骨骼下标"同时表示两种顺序 ⇒ 蒙皮取到错的骨骼。
                if (b.parent >= static_cast<int16_t>(i) || b.parent < -1)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 根骨骼的父下标是 " +
                                std::to_string(b.parent) + "，但父骨骼必须排在自己之前（-1 = 根）");
                }
                if (b.transform_mode > static_cast<uint8_t>(SpineTransformMode::kNoScaleOrReflection))
                {
                    return Fail("container: 第 " + std::to_string(i) + " 根骨骼的 transform 模式 " +
                                std::to_string(b.transform_mode) + " 越界（TransformMode 只有 0..4）");
                }
                out.bones[i] = b;
            }
        }
    }

    // ---- 动画（chunk 11-16，v9）----
    // 校验的重点不是"能不能读出来"，而是**下标引用有没有越界**：
    // 每一块的条目数是定长的，越界读在 C++ 里不崩溃、在 C# 里也不抛，只会拿到别人的
    // 数据 —— 也就是"播 A 动画用了 B 动画的帧"。所以每个区间都显式查一遍。
    {
        const Found* f_dir = find_chunk(SpineGpuChunkId::kChunkAnimDir);
        const Found* f_tl = find_chunk(SpineGpuChunkId::kChunkTimelines);
        const Found* f_frames = find_chunk(SpineGpuChunkId::kChunkFrames);
        const Found* f_curve = find_chunk(SpineGpuChunkId::kChunkCurveTypes);
        const Found* f_bez = find_chunk(SpineGpuChunkId::kChunkBeziers);
        const Found* f_pay = find_chunk(SpineGpuChunkId::kChunkTimelinePayload);

        uint32_t anim_count = 0;
        if (f_dir != nullptr)
        {
            if (f_dir->size % SpineGpuConstants::kAnimDirSize != 0)
            {
                return Fail("container: 动画目录大小不是 " + std::to_string(SpineGpuConstants::kAnimDirSize) +
                            " B 的整数倍（一条是 20 B）");
            }
            anim_count = f_dir->size / SpineGpuConstants::kAnimDirSize;
            out.anims.resize(anim_count);
            for (uint32_t i = 0; i < anim_count; ++i)
            {
                const uint8_t* p = data + f_dir->offset + static_cast<size_t>(i) * SpineGpuConstants::kAnimDirSize;
                SpineGpuAnim a;
                a.name_offset = GetU32(p + 0);
                a.timeline_first = GetU32(p + 4);
                a.timeline_count = GetU32(p + 8);
                std::memcpy(&a.duration, p + 12, sizeof(float));
                a.pad = GetU32(p + 16);
                if (a.pad != 0)
                {
                    return Fail("container: 动画目录第 " + std::to_string(i) + " 条的 pad 不是 0");
                }
                out.anims[i] = a;
            }
        }

        uint32_t timeline_count = 0;
        if (f_tl != nullptr)
        {
            if (f_tl->size % SpineGpuConstants::kTimelineEntrySize != 0)
            {
                return Fail("container: 时间轴表大小不是 " +
                            std::to_string(SpineGpuConstants::kTimelineEntrySize) +
                            " B 的整数倍（一条是 32 B）");
            }
            timeline_count = f_tl->size / SpineGpuConstants::kTimelineEntrySize;
            out.timelines.resize(timeline_count);
            for (uint32_t i = 0; i < timeline_count; ++i)
            {
                const uint8_t* p =
                    data + f_tl->offset + static_cast<size_t>(i) * SpineGpuConstants::kTimelineEntrySize;
                SpineGpuTimeline t;
                t.type = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
                t.target = static_cast<uint16_t>(p[2] | (static_cast<uint16_t>(p[3]) << 8));
                t.frame_count = GetU32(p + 4);
                t.frame_first = GetU32(p + 8);
                t.curve_first = GetU32(p + 12);
                t.bezier_first = GetU32(p + 16);
                t.bezier_count = GetU32(p + 20);
                t.payload_offset = GetU32(p + 24);
                t.payload_count = GetU32(p + 28);
                out.timelines[i] = t;
            }
        }

        const uint32_t frame_float_count = (f_frames != nullptr) ? (f_frames->size / 4u) : 0u;
        if (f_frames != nullptr && (f_frames->size % 4u) != 0u)
        {
            return Fail("container: 帧数据大小不是 4 B 的整数倍（一条是 1 float）");
        }
        if (f_frames != nullptr)
        {
            out.frames.resize(frame_float_count);
            std::memcpy(out.frames.data(), data + f_frames->offset, f_frames->size);
        }

        const uint32_t curve_count = (f_curve != nullptr) ? f_curve->size : 0u;
        if (f_curve != nullptr)
        {
            out.curve_types.assign(data + f_curve->offset, data + f_curve->offset + f_curve->size);
        }

        uint32_t bezier_count = 0;
        if (f_bez != nullptr)
        {
            if (f_bez->size % SpineGpuConstants::kBezierSize != 0)
            {
                return Fail("container: bezier 块大小不是 " + std::to_string(SpineGpuConstants::kBezierSize) +
                            " B 的整数倍（一条是 4 float）");
            }
            bezier_count = f_bez->size / SpineGpuConstants::kBezierSize;
            out.beziers.resize(bezier_count);
            for (uint32_t i = 0; i < bezier_count; ++i)
            {
                const uint8_t* p = data + f_bez->offset + static_cast<size_t>(i) * SpineGpuConstants::kBezierSize;
                SpineGpuBezier b;
                std::memcpy(&b.cx1, p, 4u * sizeof(float));
                out.beziers[i] = b;
            }
        }

        const uint32_t payload_size = (f_pay != nullptr) ? f_pay->size : 0u;
        if (f_pay != nullptr)
        {
            out.timeline_payload.assign(data + f_pay->offset, data + f_pay->offset + f_pay->size);
        }

        // ---- 区间校验：每一条时间轴的四块引用都必须落在自己的块里 ----
        for (uint32_t i = 0; i < timeline_count; ++i)
        {
            const SpineGpuTimeline& t = out.timelines[i];
            const SpineTimelineType type = static_cast<SpineTimelineType>(t.type);
            const uint32_t entries = SpineTimelineEntries(type);
            if (entries == 0)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条时间轴的类型 " +
                            std::to_string(t.type) + " 不认识（TimelineType 只到 14）");
            }
            if (t.frame_count == 0)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条时间轴有 0 帧");
            }
            const uint64_t frame_end = static_cast<uint64_t>(t.frame_first) +
                                       static_cast<uint64_t>(t.frame_count) * entries;
            if (frame_end > frame_float_count)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条时间轴的帧区间 [ " +
                            std::to_string(t.frame_first) + ", " + std::to_string(frame_end) +
                            " ) 超出帧块（" + std::to_string(frame_float_count) + " 个 float）");
            }
            const uint32_t segments = SpineTimelineHasCurve(type) ? (t.frame_count - 1u) : 0u;
            const uint64_t curve_end = static_cast<uint64_t>(t.curve_first) + segments;
            if (curve_end > curve_count)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条时间轴的曲线区间超出曲线块（" +
                            std::to_string(curve_count) + " 段）");
            }
            const uint64_t bez_end = static_cast<uint64_t>(t.bezier_first) + t.bezier_count;
            if (bez_end > bezier_count)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条时间轴的 bezier 区间超出块（" +
                            std::to_string(bezier_count) + " 条）");
            }
            // 曲线块里 bezier 段的个数必须**等于** bezier_count ——
            // 不等意味着"某段标了 bezier 却没有控制点"，采样时会取到上一条的。
            uint32_t bez_in_segments = 0;
            for (uint32_t s = 0; s < segments; ++s)
            {
                if (out.curve_types[static_cast<size_t>(t.curve_first) + s] ==
                    static_cast<uint8_t>(SpineCurveType::kBezier))
                {
                    ++bez_in_segments;
                }
                if (out.curve_types[static_cast<size_t>(t.curve_first) + s] >
                    static_cast<uint8_t>(SpineCurveType::kBezier))
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条时间轴第 " + std::to_string(s) +
                                " 段的曲线类型越界（0 linear / 1 stepped / 2 bezier）");
                }
            }
            if (bez_in_segments != t.bezier_count)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条时间轴标了 " +
                            std::to_string(bez_in_segments) + " 段 bezier，但 bezier_count 是 " +
                            std::to_string(t.bezier_count));
            }
            // 变长载荷：逐帧走一遍，顺带校验帧数（走不完 = 载荷被截断）
            if (type == SpineTimelineType::kAttachment || type == SpineTimelineType::kDeform ||
                type == SpineTimelineType::kDrawOrder || type == SpineTimelineType::kEvent)
            {
                uint32_t walked = 0;
                size_t pos = t.payload_offset;
                const auto need_u32 = [&](uint32_t& v) {
                    if (pos + 4u > payload_size)
                    {
                        return false;
                    }
                    v = GetU32(out.timeline_payload.data() + pos);
                    pos += 4u;
                    return true;
                };
                for (uint32_t fr = 0; fr < t.payload_count; ++fr)
                {
                    if (type == SpineTimelineType::kAttachment)
                    {
                        uint32_t off = 0;
                        if (!need_u32(off))
                        {
                            return Fail("container: 第 " + std::to_string(i) + " 条时间轴的载荷被截断");
                        }
                    }
                    else if (type == SpineTimelineType::kEvent)
                    {
                        // v16：SpineGpuEventKey 定长 24 B；下标的合法性等 chunk 23 读完再查
                        if (pos + sizeof(SpineGpuEventKey) > payload_size)
                        {
                            return Fail("container: 第 " + std::to_string(i) + " 条时间轴（event）的载荷被截断");
                        }
                        pos += sizeof(SpineGpuEventKey);
                    }
                    else if (type == SpineTimelineType::kDeform)
                    {
                        uint32_t start = 0;
                        uint32_t count = 0;
                        if (!need_u32(start) || !need_u32(count))
                        {
                            return Fail("container: 第 " + std::to_string(i) + " 条时间轴的载荷被截断");
                        }
                        if (pos + static_cast<size_t>(count) * 4u > payload_size)
                        {
                            return Fail("container: 第 " + std::to_string(i) + " 条时间轴第 " +
                                        std::to_string(fr) + " 帧的 deform 值被截断");
                        }
                        pos += static_cast<size_t>(count) * 4u;
                    }
                    else
                    {
                        uint32_t n = 0;
                        if (!need_u32(n))
                        {
                            return Fail("container: 第 " + std::to_string(i) + " 条时间轴的载荷被截断");
                        }
                        if (pos + static_cast<size_t>(n) * 8u > payload_size)
                        {
                            return Fail("container: 第 " + std::to_string(i) + " 条时间轴第 " +
                                        std::to_string(fr) + " 帧的 drawOrder 偏移被截断");
                        }
                        pos += static_cast<size_t>(n) * 8u;
                    }
                    ++walked;
                }
                if (walked != t.frame_count)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条时间轴的载荷有 " +
                                std::to_string(walked) + " 帧，但 frame_count 是 " +
                                std::to_string(t.frame_count));
                }
            }
            else if (t.payload_count != 0)
            {
                return Fail("container: 第 " + std::to_string(i) +
                            " 条时间轴是定长类型，却带了一份变长载荷");
            }
        }

        // 动画的区间必须落在时间轴表里
        for (uint32_t i = 0; i < anim_count; ++i)
        {
            const SpineGpuAnim& a = out.anims[i];
            const uint64_t end = static_cast<uint64_t>(a.timeline_first) + a.timeline_count;
            if (end > timeline_count)
            {
                return Fail("container: 第 " + std::to_string(i) + " 个动画的时间轴区间 [ " +
                            std::to_string(a.timeline_first) + ", " + std::to_string(end) +
                            " ) 超出时间轴表（" + std::to_string(timeline_count) + " 条）");
            }
        }
    }

    // ---- 槽位 setup（chunk 17，v10）----
    // ⚠️ "chunk 不存在"本身**不**报错（v9 及更早的容器没有它），但"存在而条数不等于
    //    header.slot_count"必须报错：少一条 ⇒ 后面的槽位读到别人的颜色，
    //    而"颜色略偏"是那种跑起来才发现、且查不出来的错。
    {
        const Found* f_strings = find_chunk(SpineGpuChunkId::kChunkStrings);
        if (const Found* f = find_chunk(SpineGpuChunkId::kChunkSlotSetup))
        {
            if (f->size % SpineGpuConstants::kSlotSetupSize != 0)
            {
                return Fail("container: slot_setup 大小不是 " + std::to_string(SpineGpuConstants::kSlotSetupSize) +
                            " B 的整数倍（一条是 40 B）");
            }
            const uint32_t count = f->size / SpineGpuConstants::kSlotSetupSize;
            if (count != out.header.slot_count)
            {
                return Fail("container: slot_setup 有 " + std::to_string(count) + " 条，但 header.slot_count 是 " +
                            std::to_string(out.header.slot_count));
            }
            out.slots.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t* p = data + f->offset + static_cast<size_t>(i) * SpineGpuConstants::kSlotSetupSize;
                SpineGpuSlot s;
                std::memcpy(&s.color_r, p, 8u * sizeof(float));   // 8 个 float 连续
                s.setup_attachment_name_offset = GetU32(p + 32);
                s.flags = GetU32(p + 36);

                // setup 附件名偏移必须落在字符串块里；`kNoAttachmentName` = "没挂附件"。
                // 指到别的组（比如骨骼名）不报错，只是槽位挂上了错误名字的附件。
                if (s.setup_attachment_name_offset != SpineGpuConstants::kNoAttachmentName &&
                    (f_strings == nullptr || s.setup_attachment_name_offset >= f_strings->size))
                {
                    return Fail("container: 第 " + std::to_string(i) + " 个槽位的 setup 附件名偏移 " +
                                std::to_string(s.setup_attachment_name_offset) + " 超出字符串块");
                }
                // 已知位之外的 flag 必须清零 —— 留着脏字节会让跨语言逐字节对拍对不上。
                if ((s.flags & ~SpineGpuSlotFlags::kSlotFlagHasDark) != 0u)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 个槽位的 flags 有未知位（0x" +
                                std::to_string(s.flags) + "，目前只有 bit0 = has_dark）");
                }
                out.slots[i] = s;
            }
        }
    }

    // ---- 约束 setup（chunk 18，v11）----
    // 块头带 ik/tf/path/bone_pool 四个 count：**没有**它们的话，"chunk 不存在"
    // 和"chunk 被截断"在字节上分不开（v11 之前 chunk 10 / 11 / 17 都栽过）。
    {
        const Found* f = find_chunk(SpineGpuChunkId::kChunkConstraintSetup);
        if (f != nullptr)
        {
            if (f->size < SpineGpuConstants::kConstraintHeaderSize)
            {
                return Fail("container: constraint_setup 比块头（" +
                            std::to_string(SpineGpuConstants::kConstraintHeaderSize) + " B）还小");
            }
            const uint8_t* base = data + f->offset;
            const uint32_t ik_n = GetU32(base);
            const uint32_t tf_n = GetU32(base + 4);
            const uint32_t path_n = GetU32(base + 8);
            const uint32_t pool_n = GetU32(base + 12);

            // 逐段算出字节大小并校验总长 —— 少一个字节都得报错，否则后面的
            // 骨骼池会整体错位，而"约束作用在错的骨骼上"是查不出来的错。
            const uint64_t want =
                static_cast<uint64_t>(SpineGpuConstants::kConstraintHeaderSize) +
                static_cast<uint64_t>(ik_n) * SpineGpuConstants::kIkConstraintSize +
                static_cast<uint64_t>(tf_n) * SpineGpuConstants::kTransformConstraintSize +
                static_cast<uint64_t>(path_n) * SpineGpuConstants::kPathConstraintSize +
                static_cast<uint64_t>(pool_n) * 2u;
            if (want != f->size)
            {
                return Fail("container: constraint_setup 大小 " + std::to_string(f->size) +
                            " B 与块头算出来的 " + std::to_string(want) + " B 不一致（ik=" +
                            std::to_string(ik_n) + " tf=" + std::to_string(tf_n) +
                            " path=" + std::to_string(path_n) + " pool=" + std::to_string(pool_n) + "）");
            }

            size_t off = SpineGpuConstants::kConstraintHeaderSize;
            out.iks.resize(ik_n);
            for (uint32_t i = 0; i < ik_n; ++i)
            {
                const uint8_t* p = base + off;
                SpineGpuIkConstraint& k = out.iks[i];
                k.order = GetU16(p);
                k.bone_count = GetU16(p + 2);
                k.bone_first = GetU32(p + 4);
                k.target = GetU32(p + 8);
                k.mix = GetF32(p + 12);
                k.bend_dir = static_cast<int16_t>(GetU16(p + 16));
                k.flags = GetU16(p + 18);
                if ((k.flags & ~(SpineGpuIkFlags::kIkFlagCompress | SpineGpuIkFlags::kIkFlagStretch |
                                 SpineGpuIkFlags::kIkFlagUniform)) != 0u)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 IK 约束的 flags 有未知位（0x" +
                                std::to_string(k.flags) + "）");
                }
                if (k.bend_dir != 1 && k.bend_dir != -1)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 IK 约束的 bend_dir 是 " +
                                std::to_string(k.bend_dir) + "（只可能是 1 / -1）");
                }
                // ⚠️ target 是**骨骼**下标，越界不报错 —— 解算时读到别人的变换矩阵，
                //    表现是"某根骨骼姿势怪"。IK 的 target 与 path 的 target_slot
                //    是同名不同 index space 的字段，两边都要各查一次。
                if (k.target >= out.header.bone_count)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 IK 约束的目标骨骼 " +
                                std::to_string(k.target) + " 越界（只有 " +
                                std::to_string(out.header.bone_count) + " 根骨骼）");
                }
                off += SpineGpuConstants::kIkConstraintSize;
            }
            out.transforms.resize(tf_n);
            for (uint32_t i = 0; i < tf_n; ++i)
            {
                const uint8_t* p = base + off;
                SpineGpuTransformConstraint& t = out.transforms[i];
                t.order = GetU16(p);
                t.bone_count = GetU16(p + 2);
                t.bone_first = GetU32(p + 4);
                t.target = GetU32(p + 8);
                t.offset_rotation = GetF32(p + 12);
                t.offset_x = GetF32(p + 16);
                t.offset_y = GetF32(p + 20);
                t.offset_scale_x = GetF32(p + 24);
                t.offset_scale_y = GetF32(p + 28);
                t.offset_shear_y = GetF32(p + 32);
                t.rotate_mix = GetF32(p + 36);
                t.translate_mix = GetF32(p + 40);
                t.scale_mix = GetF32(p + 44);
                t.shear_mix = GetF32(p + 48);
                t.flags = GetU16(p + 52);
                t.pad = GetU16(p + 54);
                if ((t.flags & ~(SpineGpuTransformFlags::kTfFlagLocal |
                                 SpineGpuTransformFlags::kTfFlagRelative)) != 0u)
                {
                    return Fail("container: 第 " + std::to_string(i) +
                                " 条 transform 约束的 flags 有未知位（0x" + std::to_string(t.flags) + "）");
                }
                if (t.pad != 0)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 transform 约束的 pad 不是 0");
                }
                if (t.target >= out.header.bone_count)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 transform 约束的目标骨骼 " +
                                std::to_string(t.target) + " 越界（只有 " +
                                std::to_string(out.header.bone_count) + " 根骨骼）");
                }
                off += SpineGpuConstants::kTransformConstraintSize;
            }
            out.paths.resize(path_n);
            for (uint32_t i = 0; i < path_n; ++i)
            {
                const uint8_t* p = base + off;
                SpineGpuPathConstraint& c = out.paths[i];
                c.order = GetU16(p);
                c.bone_count = GetU16(p + 2);
                c.bone_first = GetU32(p + 4);
                c.target_slot = GetU32(p + 8);
                c.position_mode = GetU8(p + 12);
                c.spacing_mode = GetU8(p + 13);
                c.rotate_mode = GetU8(p + 14);
                c.pad = GetU8(p + 15);
                c.offset_rotation = GetF32(p + 16);
                c.position = GetF32(p + 20);
                c.spacing = GetF32(p + 24);
                c.rotate_mix = GetF32(p + 28);
                c.translate_mix = GetF32(p + 32);
                c.path_index = GetU32(p + 36);
                if (c.position_mode > SpineGpuPathPositionMode::kPathPosPercent)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 约束的 positionMode 是 " +
                                std::to_string(c.position_mode));
                }
                if (c.spacing_mode > SpineGpuPathSpacingMode::kPathSpacingPercent)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 约束的 spacingMode 是 " +
                                std::to_string(c.spacing_mode));
                }
                if (c.rotate_mode > SpineGpuPathRotateMode::kPathRotateChainScale)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 约束的 rotateMode 是 " +
                                std::to_string(c.rotate_mode));
                }
                if (c.pad != 0)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 约束的 pad 不是 0");
                }
                // ⚠️ 是**槽位**下标（`SkeletonBinary.cpp:249` 取 `_slots[...]`）——
                //    与 IK / transform 的 target 同名但 index space 不同，
                //    拿 bone_count 去查或拿 slot_count 去查都会漏掉另一半。
                if (c.target_slot >= out.header.slot_count)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 约束的目标槽位 " +
                                std::to_string(c.target_slot) + " 越界（只有 " +
                                std::to_string(out.header.slot_count) + " 个槽位）");
                }
                off += SpineGpuConstants::kPathConstraintSize;
            }
            out.constraint_bone_pool.resize(pool_n);
            for (uint32_t i = 0; i < pool_n; ++i)
            {
                const uint16_t b = GetU16(base + off);
                if (b >= out.header.bone_count)
                {
                    return Fail("container: 约束骨骼池第 " + std::to_string(i) + " 条是 " +
                                std::to_string(b) + "，越界（只有 " +
                                std::to_string(out.header.bone_count) + " 根骨骼）");
                }
                out.constraint_bone_pool[i] = b;
                off += 2;
            }
            // 每条约束的 [bone_first, bone_first + bone_count) 必须落在池里 ——
            // 越界不报错，只是约束拿到别人的骨骼列表。
            std::string range_err;
            const auto check_range = [&](uint32_t first, uint32_t count, const char* what, uint32_t i) -> bool {
                const uint64_t end = static_cast<uint64_t>(first) + count;
                if (end > pool_n)
                {
                    range_err = "container: 第 " + std::to_string(i) + " 条 " + what + " 约束的骨骼区间 [ " +
                                std::to_string(first) + ", " + std::to_string(end) + " ) 超出骨骼池（" +
                                std::to_string(pool_n) + " 条）";
                    return false;
                }
                return true;
            };
            for (uint32_t i = 0; i < ik_n; ++i)
            {
                if (!check_range(out.iks[i].bone_first, out.iks[i].bone_count, "IK", i))
                {
                    return Fail(range_err);
                }
            }
            for (uint32_t i = 0; i < tf_n; ++i)
            {
                if (!check_range(out.transforms[i].bone_first, out.transforms[i].bone_count, "transform", i))
                {
                    return Fail(range_err);
                }
            }
            for (uint32_t i = 0; i < path_n; ++i)
            {
                if (!check_range(out.paths[i].bone_first, out.paths[i].bone_count, "path", i))
                {
                    return Fail(range_err);
                }
            }
            // order 必须是 0..N-1 的排列（Bake 侧已校验；这里是"读别人的容器"也要挡住）
            {
                const size_t total = static_cast<size_t>(ik_n) + tf_n + path_n;
                std::vector<uint32_t> orders;
                orders.reserve(total);
                for (const SpineGpuIkConstraint& k : out.iks)
                {
                    orders.push_back(k.order);
                }
                for (const SpineGpuTransformConstraint& t : out.transforms)
                {
                    orders.push_back(t.order);
                }
                for (const SpineGpuPathConstraint& c : out.paths)
                {
                    orders.push_back(c.order);
                }
                std::sort(orders.begin(), orders.end());
                for (size_t i = 0; i < orders.size(); ++i)
                {
                    if (orders[i] != i)
                    {
                        return Fail("container: 约束的 order 不是 0.." + std::to_string(total - 1) +
                                    " 的一个排列（第 " + std::to_string(i) + " 个是 " +
                                    std::to_string(orders[i]) + "）");
                    }
                }
            }
        }
    }

    // ---- 执行顺序（chunk 19，v11）----
    // ⚠️ chunk 19 缺失 = 这个资产**没有约束**（写端只在有约束时落它），此时
    //    updateCache 的结果恒等于 0..bone_count-1。反过来：有约束却没有 chunk 19
    //    ⇒ 容器坏了，必须报错（静默退化会让约束全都不跑，姿势不动也不报错）。
    {
        const bool any_constraint =
            !out.iks.empty() || !out.transforms.empty() || !out.paths.empty();
        const Found* f = find_chunk(SpineGpuChunkId::kChunkUpdateOrder);
        if (f != nullptr)
        {
            if (f->size < SpineGpuConstants::kUpdateOrderHeaderSize)
            {
                return Fail("container: update_order 比块头（" +
                            std::to_string(SpineGpuConstants::kUpdateOrderHeaderSize) + " B）还小");
            }
            const uint8_t* base = data + f->offset;
            const uint32_t step_n = GetU32(base);
            const uint32_t reset_n = GetU32(base + 4);
            const uint64_t want = static_cast<uint64_t>(SpineGpuConstants::kUpdateOrderHeaderSize) +
                                  static_cast<uint64_t>(step_n) * SpineGpuConstants::kUpdateStepSize +
                                  static_cast<uint64_t>(reset_n) * 2u;
            if (want != f->size)
            {
                return Fail("container: update_order 大小 " + std::to_string(f->size) +
                            " B 与块头算出来的 " + std::to_string(want) + " B 不一致");
            }
            size_t off = SpineGpuConstants::kUpdateOrderHeaderSize;
            out.update_steps.resize(step_n);
            for (uint32_t i = 0; i < step_n; ++i)
            {
                const uint8_t* p = base + off;
                SpineGpuUpdateStep& s = out.update_steps[i];
                s.kind = GetU8(p);
                s.pad = GetU8(p + 1);
                s.index = GetU16(p + 2);
                if (s.kind > SpineGpuUpdateKind::kUpdatePath)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 步的 kind 是 " +
                                std::to_string(s.kind) + "（只到 3 = path）");
                }
                if (s.pad != 0)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 步的 pad 不是 0");
                }
                // 下标必须落在对应表里 —— 越界不报错，只是解算器跑到别人的对象上。
                const uint32_t limit = (s.kind == SpineGpuUpdateKind::kUpdateBone)
                                           ? out.header.bone_count
                                           : (s.kind == SpineGpuUpdateKind::kUpdateIk
                                                  ? static_cast<uint32_t>(out.iks.size())
                                                  : (s.kind == SpineGpuUpdateKind::kUpdateTransform
                                                         ? static_cast<uint32_t>(out.transforms.size())
                                                         : static_cast<uint32_t>(out.paths.size())));
                if (s.index >= limit)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 步的 index " +
                                std::to_string(s.index) + " 超出该类对象的个数 " + std::to_string(limit));
                }
                off += SpineGpuConstants::kUpdateStepSize;
            }
            out.update_reset.resize(reset_n);
            for (uint32_t i = 0; i < reset_n; ++i)
            {
                const uint16_t b = GetU16(base + off);
                if (b >= out.header.bone_count)
                {
                    return Fail("container: update_reset 第 " + std::to_string(i) + " 项的骨骼下标 " +
                                std::to_string(b) + " 超出骨骼数 " + std::to_string(out.header.bone_count));
                }
                out.update_reset[i] = b;
                off += 2;
            }
        }
        else if (any_constraint)
        {
            return Fail("container: 有约束（ik=" + std::to_string(out.iks.size()) +
                        " tf=" + std::to_string(out.transforms.size()) +
                        " path=" + std::to_string(out.paths.size()) +
                        "）却没有 chunk 19（update_order）—— 约束会全部不跑，"
                        "表现是「姿势不动」而不是报错");
        }
        else
        {
            // 可证等价：无约束时 updateCache = 正向遍历全部骨骼，而 parent < index
            // ⇒ 结果就是 0..bone_count-1，reset 为空。
            out.update_steps.clear();
            out.update_steps.reserve(out.header.bone_count);
            for (uint32_t i = 0; i < out.header.bone_count; ++i)
            {
                SpineGpuUpdateStep s;
                s.kind = SpineGpuUpdateKind::kUpdateBone;
                s.pad = 0;
                s.index = static_cast<uint16_t>(i);
                out.update_steps.push_back(s);
            }
            out.update_reset.clear();
        }
    }

    // ---- PathAttachment 几何（chunk 20，v11）----
    {
        const Found* f = find_chunk(SpineGpuChunkId::kChunkPathAttachments);
        if (f != nullptr)
        {
            if (f->size < SpineGpuConstants::kPathAttHeaderSize)
            {
                return Fail("container: path_attachments 比块头（" +
                            std::to_string(SpineGpuConstants::kPathAttHeaderSize) + " B）还小");
            }
            const uint8_t* base = data + f->offset;
            const uint32_t att_n = GetU32(base);
            const uint32_t bone_n = GetU32(base + 4);
            const uint32_t vert_n = GetU32(base + 8);
            const uint32_t len_n = GetU32(base + 12);
            const uint64_t want = static_cast<uint64_t>(SpineGpuConstants::kPathAttHeaderSize) +
                                  static_cast<uint64_t>(att_n) * SpineGpuConstants::kPathAttachmentSize +
                                  static_cast<uint64_t>(bone_n) * 2u +
                                  static_cast<uint64_t>(vert_n) * 4u + static_cast<uint64_t>(len_n) * 4u;
            if (want != f->size)
            {
                return Fail("container: path_attachments 大小 " + std::to_string(f->size) +
                            " B 与块头算出来的 " + std::to_string(want) + " B 不一致");
            }
            size_t off = SpineGpuConstants::kPathAttHeaderSize;
            out.path_attachments.resize(att_n);
            for (uint32_t i = 0; i < att_n; ++i)
            {
                const uint8_t* p = base + off;
                SpineGpuPathAttachment& a = out.path_attachments[i];
                a.slot_index = GetU16(p);
                a.skin_index = GetU16(p + 2);
                a.flags = GetU16(p + 4);
                a.pad = GetU16(p + 6);
                a.vertex_count = GetU32(p + 8);
                a.bone_offset = GetU32(p + 12);
                a.bone_count = GetU32(p + 16);
                a.vertex_offset = GetU32(p + 20);
                a.vertex_float_count = GetU32(p + 24);
                a.length_offset = GetU32(p + 28);
                a.length_count = GetU32(p + 32);
                a.name_offset = GetU32(p + 36);
                if ((a.flags & ~(SpineGpuPathAttachmentFlags::kPathAttFlagClosed |
                                 SpineGpuPathAttachmentFlags::kPathAttFlagConstantSpeed |
                                 SpineGpuPathAttachmentFlags::kPathAttFlagWeighted)) != 0u)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 附件的 flags 有未知位（0x" +
                                std::to_string(a.flags) + "）");
                }
                if (a.pad != 0)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 附件的 pad 不是 0");
                }
                if (a.slot_index >= out.header.slot_count)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 附件的槽位下标 " +
                                std::to_string(a.slot_index) + " 超出槽位数 " +
                                std::to_string(out.header.slot_count));
                }
                // 三个池的区间都必须落在池里（越界 = 取到别人的几何，不报错）
                std::string pool_err;
                const auto in_pool = [&](uint32_t first, uint32_t count, uint32_t pool,
                                         const char* what) -> bool {
                    if (static_cast<uint64_t>(first) + count > pool)
                    {
                        pool_err = "container: 第 " + std::to_string(i) + " 条 path 附件的 " + what + " 区间 [ " +
                                   std::to_string(first) + ", " +
                                   std::to_string(static_cast<uint64_t>(first) + count) +
                                   " ) 超出池（" + std::to_string(pool) + "）";
                        return false;
                    }
                    return true;
                };
                if (!in_pool(a.bone_offset, a.bone_count, bone_n, "骨骼") ||
                    !in_pool(a.vertex_offset, a.vertex_float_count, vert_n, "顶点") ||
                    !in_pool(a.length_offset, a.length_count, len_n, "lengths"))
                {
                    return Fail(pool_err);
                }
                // lengths 条数 = 点数 / 3（SkeletonBinary.cpp:550）—— Bake 侧也是这个式子。
                if (a.length_count != a.vertex_count / 3u)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 path 附件的 lengths 有 " +
                                std::to_string(a.length_count) + " 条，但点数 " +
                                std::to_string(a.vertex_count) + " 对应 " +
                                std::to_string(a.vertex_count / 3u) + " 条");
                }
                off += SpineGpuConstants::kPathAttachmentSize;
            }
            out.path_bone_pool.resize(bone_n);
            for (uint32_t i = 0; i < bone_n; ++i)
            {
                const uint16_t b = GetU16(base + off);
                if (b >= out.header.bone_count)
                {
                    return Fail("container: path 附件的骨骼池第 " + std::to_string(i) + " 项 " +
                                std::to_string(b) + " 超出骨骼数 " + std::to_string(out.header.bone_count));
                }
                out.path_bone_pool[i] = b;
                off += 2;
            }
            out.path_vertices.resize(vert_n);
            for (uint32_t i = 0; i < vert_n; ++i)
            {
                out.path_vertices[i] = GetF32(base + off);
                off += 4;
            }
            out.path_lengths.resize(len_n);
            for (uint32_t i = 0; i < len_n; ++i)
            {
                out.path_lengths[i] = GetF32(base + off);
                off += 4;
            }
        }
    }

    // ---- 交叉检查：path 约束的 path_index 必须落在 chunk 20 里 ----
    // 放在 chunk 20 之后是因为它要引用那边的条数（chunk 顺序 18 < 19 < 20）。
    // ⚠️ kNoPathAttachment 是合法的（"这个槽位上没有 path 附件"），
    //    而 0 **也**是合法的下标 ⇒ 不能直接用"非 0"判断，必须显式比 MAX。
    {
        for (uint32_t i = 0; i < out.paths.size(); ++i)
        {
            const SpineGpuPathConstraint& c = out.paths[i];
            if (c.path_index != SpineGpuConstants::kNoPathAttachment &&
                c.path_index >= out.path_attachments.size())
            {
                return Fail("container: 第 " + std::to_string(i) + " 条 path 约束的 path_index " +
                            std::to_string(c.path_index) + " 越界（chunk 20 只有 " +
                            std::to_string(out.path_attachments.size()) + " 条）");
            }
        }
    }

    // ---- 交叉检查（v12）：指向 path 附件的 deform 时间轴 ----
    // target 最高位 = 低 15 位是 chunk 20 下标。越界或帧写出 deform 数组都是坏容器：
    // 解算器会读到别的 path 的曲线 / 越界，不报错。
    {
        for (uint32_t i = 0; i < out.timelines.size(); ++i)
        {
            const SpineGpuTimeline& t = out.timelines[i];
            if (static_cast<SpineTimelineType>(t.type) != SpineTimelineType::kDeform ||
                (t.target & SpineGpuConstants::kDeformTargetPathBit) == 0)
            {
                continue;
            }
            const uint32_t k = t.target & SpineGpuConstants::kDeformTargetIndexMask;
            if (k >= out.path_attachments.size())
            {
                return Fail("container: 第 " + std::to_string(i) + " 条 deform 时间轴指向 path 附件 " +
                            std::to_string(k) + "，越界（chunk 20 只有 " +
                            std::to_string(out.path_attachments.size()) + " 条）");
            }
            const uint32_t deform_len = SpineGpuPathDeformLength(out.path_attachments[k]);
            size_t pos = t.payload_offset;   // 载荷已在时间轴校验里走过一遍，这里不会截断
            for (uint32_t fr = 0; fr < t.payload_count; ++fr)
            {
                const uint32_t start = GetU32(out.timeline_payload.data() + pos);
                const uint32_t count = GetU32(out.timeline_payload.data() + pos + 4u);
                pos += 8u + static_cast<size_t>(count) * 4u;
                if (static_cast<uint64_t>(start) + count > deform_len)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 条 deform 时间轴第 " + std::to_string(fr) +
                                " 帧写到 [" + std::to_string(start) + ", " + std::to_string(start + count) +
                                ")，超出 path 附件 " + std::to_string(k) + " 的 deform 长度 " +
                                std::to_string(deform_len));
                }
            }
        }
    }

    // ---- 影响流（chunk 21 / 22）。两条流**必须条目数相同**： ----
    // 它们共用同一个下标（权重顶点的 influence_offset）。对不上意味着两条流被
    // 配错了对 —— 不报错，只是每个影响取到别人的 bindPos / 骨骼 ⇒ 形状静默错。
    {
        uint32_t bind_count = 0;
        uint32_t bone_count = 0;
        if (const Found* f = find_chunk(SpineGpuChunkId::kChunkInfluenceBind))
        {
            if (f->size % SpineGpuConstants::kInfluenceBindStride != 0)
            {
                return Fail("container: influence_bind 大小不是 " +
                            std::to_string(SpineGpuConstants::kInfluenceBindStride) +
                            " B 的整数倍（一条是 2 float）");
            }
            bind_count = f->size / SpineGpuConstants::kInfluenceBindStride;
            out.influence_bind.resize(static_cast<size_t>(bind_count) * 2u);
            std::memcpy(out.influence_bind.data(), data + f->offset, f->size);
        }
        if (const Found* f = find_chunk(SpineGpuChunkId::kChunkInfluenceBone))
        {
            if (f->size % sizeof(SpineInfluenceBone) != 0)
            {
                return Fail("container: influence_bone 大小不是 " + std::to_string(sizeof(SpineInfluenceBone)) +
                            " B 的整数倍（一条是 4 B）");
            }
            bone_count = f->size / static_cast<uint32_t>(sizeof(SpineInfluenceBone));
            out.influence_bone.resize(bone_count);
            for (uint32_t i = 0; i < bone_count; ++i)
            {
                std::memcpy(&out.influence_bone[i],
                            data + f->offset + static_cast<size_t>(i) * sizeof(SpineInfluenceBone),
                            sizeof(SpineInfluenceBone));
            }
        }
        if (bind_count != bone_count)
        {
            return Fail("container: 影响流两条 chunk 条目数不同（bind " + std::to_string(bind_count) +
                        " vs bone " + std::to_string(bone_count) + "）—— 它们必须共用同一个下标");
        }

        // 每个权重顶点引用的那段影响必须**落在流里**。这是"读端拿到的容器是不是
        // 自洽"最便宜的一道检查：越界不报错，shader 只会读到别人的 bindPos。
        const uint64_t seg = static_cast<uint64_t>(out.weighted_segment_offset) +
                             static_cast<uint64_t>(out.weighted_vertex_count) *
                                 SpineGpuConstants::kWeightedVertexStride;
        if (seg > out.vertex_buffer.size())
        {
            return Fail("container: weighted 段越界（顶点缓冲 " + std::to_string(out.vertex_buffer.size()) +
                        " B，段尾 " + std::to_string(seg) + " B）");
        }
        for (uint32_t v = 0; v < out.weighted_vertex_count; ++v)
        {
            SpineWeightedVertex wv {};
            std::memcpy(&wv,
                        out.vertex_buffer.data() + out.weighted_segment_offset +
                            static_cast<size_t>(v) * SpineGpuConstants::kWeightedVertexStride,
                        sizeof(wv));
            if (wv.influence_count == 0)
            {
                return Fail("container: 第 " + std::to_string(v) + " 个权重顶点的影响数是 0");
            }
            const uint64_t end = static_cast<uint64_t>(wv.influence_offset) + wv.influence_count;
            if (end > bind_count)
            {
                return Fail("container: 第 " + std::to_string(v) + " 个权重顶点引用影响 [" +
                            std::to_string(wv.influence_offset) + ", " + std::to_string(end) +
                            ")，但影响流只有 " + std::to_string(bind_count) + " 条");
            }
            if (wv.influence_count > SpineGpuConstants::kMaxInfluences)
            {
                return Fail("container: 第 " + std::to_string(v) + " 个权重顶点有 " +
                            std::to_string(wv.influence_count) + " 个影响，超过硬上限 " +
                            std::to_string(SpineGpuConstants::kMaxInfluences));
            }
        }
    }

    // ---- UV 通道（独立 chunk）----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkUvBuffer))
    {
        if (f->size % SpineGpuConstants::kUvStride != 0)
        {
            return Fail("container: uv_buffer 大小不是 " + std::to_string(SpineGpuConstants::kUvStride) +
                        " B 的整数倍（一条 UV 是 2 float）");
        }
        const uint32_t count = f->size / sizeof(float);
        out.uv_buffer.resize(count);
        // float 与顶点缓冲一样按 IEEE754 binary32 小端存放（见规格 §9 的布局规则）
        std::memcpy(out.uv_buffer.data(), data + f->offset, f->size);
    }

    // ---- 索引 ----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkIndexBuffer))
    {
        const uint32_t count = f->size / 2u;
        out.index_buffer.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* p = data + f->offset + static_cast<size_t>(i) * 2u;
            out.index_buffer[i] = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
        }
    }

    // ---- 裁剪多边形（chunk 9，12 B/条）----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkClipData))
    {
        if (f->size % sizeof(SpineGpuClipPolygon) != 0)
        {
            return Fail("container: clip_data 大小不是 " + std::to_string(sizeof(SpineGpuClipPolygon)) +
                        " B 的整数倍（v6 起一条是 12 B，v15 起 20 B：多了 name_offset / end_slot）");
        }
        const uint32_t count = f->size / static_cast<uint32_t>(sizeof(SpineGpuClipPolygon));
        out.clip_polygons.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(&out.clip_polygons[i], data + f->offset + static_cast<size_t>(i) * sizeof(SpineGpuClipPolygon),
                        sizeof(SpineGpuClipPolygon));
        }
    }

    // ---- 绘制项（32 B/项，v14） ----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkDrawItems))
    {
        if (f->size % sizeof(SpineDrawItem) != 0)
        {
            return Fail("container: draw_items 大小不是 " + std::to_string(sizeof(SpineDrawItem)) +
                        " B 的整数倍（v3 起一项是 24 B：多了 uv_offset；v6 起是 28 B：多了 clip_index；"
                        "v14 起是 32 B：多了 vertex_base / deform_parent）");
        }
        const uint32_t count = f->size / static_cast<uint32_t>(sizeof(SpineDrawItem));
        out.draw_items.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(&out.draw_items[i], data + f->offset + static_cast<size_t>(i) * sizeof(SpineDrawItem),
                        sizeof(SpineDrawItem));
        }
        // v13：每个绘制项的每个索引都必须能按 `uv_offset + index`（uint32 回绕）取到 UV。
        // 这正是 shader 里的寻址式子（规格 §3.2）—— 坏了的话 GPU 上是"贴图采错块 / 越界读"，
        // 不报错，所以在读端挡住。
        const uint64_t uv_count = out.uv_buffer.size() / 2u;
        for (uint32_t i = 0; i < count; ++i)
        {
            const SpineDrawItem& di = out.draw_items[i];
            if (static_cast<uint64_t>(di.index_offset) + di.index_count > out.index_buffer.size())
            {
                return Fail("container: 第 " + std::to_string(i) + " 个绘制项的索引区间越过索引缓冲");
            }
            // v15：item_flags 只认已定义的位；region 必须是 rigid 4 顶点 6 索引（CPU 裁剪按
            // {3, 0, 1, 2} 重排顶点，形状不对就是越界读）。
            if ((di.item_flags & ~SpineDrawItemFlags::kKnownMask) != 0)
            {
                return Fail("container: 第 " + std::to_string(i) + " 个绘制项的 item_flags " +
                            std::to_string(di.item_flags) + " 有未定义的位");
            }
            if ((di.item_flags & SpineDrawItemFlags::kRegion) != 0 &&
                (di.vertex_format != SpineVertexFormat::kRigid || di.vertex_count != 4 || di.index_count != 6))
            {
                return Fail("container: 第 " + std::to_string(i) + " 个绘制项标成 region，却不是 rigid 4 顶点 6 索引");
            }
            // v14：[vertex_base, vertex_base + vertex_count) 必须落在所属顶点段里
            const uint32_t seg_count = di.vertex_format == SpineVertexFormat::kWeighted ? out.header.weighted_vertex_count
                                                                                        : out.header.rigid_vertex_count;
            if (static_cast<uint32_t>(di.vertex_base) + di.vertex_count > seg_count)
            {
                return Fail("container: 第 " + std::to_string(i) + " 个绘制项的顶点区间 [" +
                            std::to_string(di.vertex_base) + ", +" + std::to_string(di.vertex_count) +
                            ") 越过所属顶点段（" + std::to_string(seg_count) + " 个）");
            }
            for (uint32_t q = 0; q < di.index_count; ++q)
            {
                // v14：索引 = 附件本地下标 + vertex_base ⇒ 必须落在 [vertex_base, vertex_base + vertex_count)。
                // 落在外面说明 vertex_base / vertex_count 写错了，GPU deform 会取到别的附件的 deform。
                const uint32_t idx = out.index_buffer[static_cast<size_t>(di.index_offset) + q];
                if (idx < di.vertex_base || idx >= static_cast<uint32_t>(di.vertex_base) + di.vertex_count)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 个绘制项的索引 " + std::to_string(idx) +
                                " 不在它的顶点区间 [" + std::to_string(di.vertex_base) + ", +" +
                                std::to_string(di.vertex_count) + ") 里");
                }
                const uint32_t uv_index =
                    di.uv_offset + static_cast<uint32_t>(out.index_buffer[static_cast<size_t>(di.index_offset) + q]);
                if (uv_index >= uv_count)
                {
                    return Fail("container: 第 " + std::to_string(i) + " 个绘制项的 uv_offset + 索引 = " +
                                std::to_string(uv_index) + "，越过 UV 表（" + std::to_string(uv_count) +
                                " 条）—— v12 及以前的 uv_offset 没减 vertex_base");
                }
            }
        }
    }

    // ---- slot_bone ----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkSlotBone))
    {
        const uint32_t count = f->size / 2u;
        out.slot_bone.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* p = data + f->offset + static_cast<size_t>(i) * 2u;
            out.slot_bone[i] = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
        }
    }

    // ---- attachments：拿到附件数，才能切分字符串表 ----
    size_t attachment_count = 0;
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkAttachments))
    {
        if (f->size % sizeof(SpineGpuAttachmentRecord) != 0)
        {
            return Fail("container: attachments 大小不是 " + std::to_string(sizeof(SpineGpuAttachmentRecord)) +
                        " B 的整数倍（v2 起一条记录是 8 B：多了 skin 列）");
        }
        const uint32_t count = f->size / static_cast<uint32_t>(sizeof(SpineGpuAttachmentRecord));
        out.attachment_records.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(&out.attachment_records[i],
                        data + f->offset + static_cast<size_t>(i) * sizeof(SpineGpuAttachmentRecord),
                        sizeof(SpineGpuAttachmentRecord));
            // 规格 §9："attachment_id 是全局下标（= 本表的行号）"。deform 时间轴按**记录下标**
            // 寻址、绘制项按 attachment_id 寻址，两者能互换全靠这条恒等式 ⇒ 读端钉住。
            if (out.attachment_records[i].attachment_id != i)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条附件记录的 attachment_id 是 " +
                            std::to_string(out.attachment_records[i].attachment_id) + "（应等于行号）");
            }
        }
        attachment_count = count;
    }

    // v14：deform_parent 要么是"没有"，要么是**同一槽位**里**另一个**附件的 id。
    // 坏了的话 GPU deform 会把别的附件的 deform 数组套到自己身上（长度对得上就不报错）。
    for (size_t i = 0; i < out.draw_items.size(); ++i)
    {
        const SpineDrawItem& di = out.draw_items[i];
        if (di.deform_parent == SpineGpuConstants::kNoDeformParent)
        {
            continue;
        }
        if (di.deform_parent >= out.attachment_records.size() || di.deform_parent == di.attachment_id ||
            out.attachment_records[di.deform_parent].slot_index != di.slot_index)
        {
            return Fail("container: 第 " + std::to_string(i) + " 个绘制项的 deform_parent " +
                        std::to_string(di.deform_parent) + " 越界、指向自己或不在同一槽位");
        }
    }

    // ---- 字符串：骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名 → 动画名 → 动画附件名 ----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkStrings))
    {
        std::vector<std::string> all;
        const uint8_t* p = data + f->offset;
        const uint8_t* end = p + f->size;
        while (p < end)
        {
            const uint8_t* z = static_cast<const uint8_t*>(std::memchr(p, 0, static_cast<size_t>(end - p)));
            if (!z)
            {
                all.emplace_back(reinterpret_cast<const char*>(p), static_cast<size_t>(end - p));
                break;
            }
            all.emplace_back(reinterpret_cast<const char*>(p), static_cast<size_t>(z - p));
            p = z + 1;
        }

        size_t i = 0;
        auto take = [&](size_t count, std::vector<std::string>& dst) {
            for (size_t k = 0; k < count && i < all.size(); ++k)
            {
                dst.push_back(all[i++]);
            }
        };
        take(out.header.bone_count, out.bone_names);
        take(out.header.skin_count, out.skin_names);
        take(out.header.slot_count, out.slot_names);
        take(out.header.slot_count, out.slot_setup_attachment_names);   // v10：chunk 17 引用
        take(attachment_count, out.attachment_names);
        take(out.header.atlas_page_count, out.atlas_page_names);
        // 动画名：数量 = chunk 11 的条目数（header 里没有 anim_count 的位置 ——
        // 64 B 的 header 早就排满了，而定长块的大小本身就是计数）。
        take(out.anims.size(), out.anim_names);
        // v11 path 附件名：数量 = chunk 20 的条目数（一一对应，不去重）。
        take(out.path_attachments.size(), out.path_attachment_names);
        // v15 裁剪附件名：数量 = chunk 9 的条目数（一一对应，不去重）。
        take(out.clip_polygons.size(), out.clip_names);
        // 剩下的全是"动画附件名"。**只按偏移访问**，所以不需要知道有几条 ⇒
        // 不校验"是否刚好读完"（多出来的是合法数据，不是错误）。
        while (i < all.size())
        {
            out.anim_attachment_names.push_back(all[i++]);
        }
        if (out.clip_names.size() != out.clip_polygons.size())
        {
            return Fail("container: 裁剪附件名只有 " + std::to_string(out.clip_names.size()) +
                        " 条，但 chunk 9 有 " + std::to_string(out.clip_polygons.size()) + " 条");
        }
        if (out.path_attachment_names.size() != out.path_attachments.size())
        {
            return Fail("container: path 附件名只有 " +
                        std::to_string(out.path_attachment_names.size()) + " 条，但 chunk 20 有 " +
                        std::to_string(out.path_attachments.size()) + " 条");
        }
        // 槽位 setup 附件名是 chunk 17 的复位基线 ⇒ 少一条就会让后面的槽位
        // 挂上别人的附件名（不报错，只是"这个槽位画错了图"）。
        if (out.slot_setup_attachment_names.size() != out.header.slot_count)
        {
            return Fail("container: 槽位 setup 附件名只有 " +
                        std::to_string(out.slot_setup_attachment_names.size()) + " 条，但 header.slot_count 是 " +
                        std::to_string(out.header.slot_count));
        }
        // ⚠️ 但每个 name_offset 必须真的落在这一块里 —— 指到前面几组（比如骨骼名）
        // 不报错，只是动画挂载了错误名字的附件。
        for (size_t k = 0; k < out.anims.size(); ++k)
        {
            if (out.anims[k].name_offset >= f->size)
            {
                return Fail("container: 第 " + std::to_string(k) + " 个动画的 name_offset " +
                            std::to_string(out.anims[k].name_offset) + " 超出字符串块（大小 " +
                            std::to_string(f->size) + "）");
            }
        }
        // path 附件的名字同理：指到骨骼名那组不报错，只是"换装后摆错了曲线"。
        for (size_t k = 0; k < out.path_attachments.size(); ++k)
        {
            if (out.path_attachments[k].name_offset >= f->size)
            {
                return Fail("container: 第 " + std::to_string(k) + " 条 path 附件的 name_offset " +
                            std::to_string(out.path_attachments[k].name_offset) +
                            " 超出字符串块（大小 " + std::to_string(f->size) + "）");
            }
        }
    }

    // ---- v15：裁剪多边形的交叉校验（要等顶点缓冲 / 影响流 / 字符串都读完）----
    // 每一条坏了都是"能画、但裁错了"：区间越界 ⇒ 多边形取到别人的顶点；结束槽位越界 ⇒
    // 作用域一路延伸到末尾；名字偏移指歪 ⇒ 槽位挂着它时认不出来（不裁），或者认错。
    if (!out.clip_polygons.empty())
    {
        if (out.clip_names.size() != out.clip_polygons.size())
        {
            return Fail("container: 有 " + std::to_string(out.clip_polygons.size()) +
                        " 条裁剪多边形，却没有对应的裁剪附件名（缺字符串块？）");
        }
        std::vector<uint8_t> blob;
        std::vector<uint32_t> slot_off, anim_off, anim_att_off, path_att_off, clip_off;
        BuildSpineStringTable(out, blob, slot_off, anim_off, anim_att_off, path_att_off, clip_off);
        const uint32_t influence_count = static_cast<uint32_t>(out.influence_bone.size());
        for (size_t k = 0; k < out.clip_polygons.size(); ++k)
        {
            const SpineGpuClipPolygon& c = out.clip_polygons[k];
            const std::string what = "container: 第 " + std::to_string(k) + " 条裁剪多边形";
            if (c.slot_index >= out.header.slot_count)
            {
                return Fail(what + "的槽位 " + std::to_string(c.slot_index) + " 越界");
            }
            if (c.skin_id >= out.header.skin_count)
            {
                return Fail(what + "的皮肤 " + std::to_string(c.skin_id) + " 越界");
            }
            if (c.end_slot != SpineGpuConstants::kNoClipEndSlot && c.end_slot >= out.header.slot_count)
            {
                return Fail(what + "的结束槽位 " + std::to_string(c.end_slot) + " 越界（槽位数 " +
                            std::to_string(out.header.slot_count) + "）");
            }
            if (c.pad != 0 || c.pad2 != 0)
            {
                return Fail(what + "的填充字节不是 0");
            }
            if (c.vertex_count == 0)
            {
                return Fail(what + "的顶点数为 0");
            }
            if (k >= clip_off.size() || c.name_offset != clip_off[k])
            {
                return Fail(what + "的 name_offset " + std::to_string(c.name_offset) +
                            " 不是字符串表第 9 组第 " + std::to_string(k) + " 条的偏移");
            }
            const uint64_t end_vertex = static_cast<uint64_t>(c.vertex_offset) + c.vertex_count;
            if (c.vertex_format == SpineVertexFormat::kRigid)
            {
                if (end_vertex * SpineGpuConstants::kRigidVertexStride > out.weighted_segment_offset)
                {
                    return Fail(what + "的 rigid 顶点区间越过 rigid 段");
                }
            }
            else if (c.vertex_format == SpineVertexFormat::kWeighted)
            {
                const uint64_t w0 = out.weighted_segment_offset;
                if (w0 + end_vertex * SpineGpuConstants::kRigidVertexStride > out.vertex_buffer.size())
                {
                    return Fail(what + "的 weighted 顶点区间越过顶点缓冲");
                }
                for (uint32_t v = 0; v < c.vertex_count; ++v)
                {
                    const size_t at = static_cast<size_t>(w0) +
                                      (static_cast<size_t>(c.vertex_offset) + v) * SpineGpuConstants::kRigidVertexStride;
                    const uint32_t off = GetU32(out.vertex_buffer.data() + at);
                    const uint32_t cnt = GetU32(out.vertex_buffer.data() + at + 4u);
                    if (cnt == 0 || static_cast<uint64_t>(off) + cnt > influence_count)
                    {
                        return Fail(what + "的第 " + std::to_string(v) + " 个顶点的影响区间 [" + std::to_string(off) +
                                    ", +" + std::to_string(cnt) + ") 越过影响流（" +
                                    std::to_string(influence_count) + " 条）");
                    }
                }
            }
            else
            {
                return Fail(what + "的顶点格式 " + std::to_string(static_cast<int>(c.vertex_format)) + " 不认识");
            }
        }
    }

    // ---- v15：指向裁剪多边形的 deform 时间轴 ----
    // 低 14 位越界 ⇒ 改到别的裁剪多边形；帧写出 deform 数组 ⇒ 运行时越界写。都不报错，只能这里挡。
    for (uint32_t i = 0; i < out.timelines.size(); ++i)
    {
        const SpineGpuTimeline& t = out.timelines[i];
        if (static_cast<SpineTimelineType>(t.type) != SpineTimelineType::kDeform)
        {
            continue;
        }
        const bool path_bit = (t.target & SpineGpuConstants::kDeformTargetPathBit) != 0;
        const bool clip_bit = (t.target & SpineGpuConstants::kDeformTargetClipBit) != 0;
        if (path_bit && clip_bit)
        {
            return Fail("container: 第 " + std::to_string(i) + " 条 deform 时间轴的 target 同时置了 path 位与 clip 位");
        }
        if (!clip_bit)
        {
            continue;
        }
        const uint32_t k = t.target & SpineGpuConstants::kDeformTargetIndexMask;
        if (k >= out.clip_polygons.size())
        {
            return Fail("container: 第 " + std::to_string(i) + " 条 deform 时间轴指向裁剪多边形 " + std::to_string(k) +
                        "，越界（chunk 9 只有 " + std::to_string(out.clip_polygons.size()) + " 条）");
        }
        const uint32_t deform_len = SpineGpuClipDeformLength(out, out.clip_polygons[k]);
        size_t pos = t.payload_offset;   // 载荷已在时间轴校验里走过一遍，这里不会截断
        for (uint32_t fr = 0; fr < t.payload_count; ++fr)
        {
            const uint32_t start = GetU32(out.timeline_payload.data() + pos);
            const uint32_t count = GetU32(out.timeline_payload.data() + pos + 4u);
            pos += 8u + static_cast<size_t>(count) * 4u;
            if (static_cast<uint64_t>(start) + count > deform_len)
            {
                return Fail("container: 第 " + std::to_string(i) + " 条 deform 时间轴第 " + std::to_string(fr) +
                            " 帧写到 [" + std::to_string(start) + ", " + std::to_string(start + count) +
                            ")，超出裁剪多边形 " + std::to_string(k) + " 的 deform 长度 " + std::to_string(deform_len));
            }
        }
    }

    // ---- 用户事件（chunk 23，v16）----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkEvents))
    {
        constexpr uint32_t kHead = sizeof(SpineGpuEventChunkHeader);
        if (f->size < kHead)
        {
            return Fail("container: events 比块头（16 B）还小");
        }
        const uint8_t* base = data + f->offset;
        const uint32_t event_n = GetU32(base);
        const uint32_t string_n = GetU32(base + 4);
        const uint32_t blob_n = GetU32(base + 8);
        if (GetU32(base + 12) != 0u)
        {
            return Fail("container: events 块头的保留字段不是 0");
        }
        const uint64_t want = static_cast<uint64_t>(kHead) + static_cast<uint64_t>(event_n) * sizeof(SpineGpuEventData) +
                              static_cast<uint64_t>(string_n) * 4u + blob_n;
        const uint64_t want_padded = (want + 3u) & ~static_cast<uint64_t>(3u);
        if (want_padded != f->size || event_n == 0u || string_n == 0u)
        {
            return Fail("container: events 大小 " + std::to_string(f->size) + " B 与块头算出来的 " +
                        std::to_string(want_padded) + " B 不一致（或事件 / 字符串数为 0）");
        }
        const uint8_t* rec = base + kHead;
        const uint8_t* offs = rec + static_cast<size_t>(event_n) * sizeof(SpineGpuEventData);
        const uint8_t* blob = offs + static_cast<size_t>(string_n) * 4u;
        out.event_strings.resize(string_n);
        for (uint32_t i = 0; i < string_n; ++i)
        {
            const uint32_t o = GetU32(offs + static_cast<size_t>(i) * 4u);
            const void* z = o < blob_n ? std::memchr(blob + o, 0, blob_n - o) : nullptr;
            if (z == nullptr)
            {
                return Fail("container: events 第 " + std::to_string(i) + " 条字符串越界或没有结尾的 '\\0'");
            }
            out.event_strings[i].assign(reinterpret_cast<const char*>(blob + o),
                                        static_cast<const uint8_t*>(z) - (blob + o));
        }
        if (!out.event_strings[0].empty())
        {
            return Fail("container: events 字符串池第 0 条必须是空串");
        }
        out.events.resize(event_n);
        for (uint32_t i = 0; i < event_n; ++i)
        {
            const uint8_t* p = rec + static_cast<size_t>(i) * sizeof(SpineGpuEventData);
            SpineGpuEventData& e = out.events[i];
            e.name = GetU32(p);
            e.int_value = static_cast<int32_t>(GetU32(p + 4));
            e.float_value = GetF32(p + 8);
            e.string_value = GetU32(p + 12);
            e.audio_path = GetU32(p + 16);
            e.volume = GetF32(p + 20);
            e.balance = GetF32(p + 24);
            if (e.name >= string_n || e.string_value >= string_n || e.audio_path >= string_n)
            {
                return Fail("container: 第 " + std::to_string(i) + " 个事件定义的字符串下标越界");
            }
            if (out.event_strings[e.name].empty())
            {
                return Fail("container: 第 " + std::to_string(i) + " 个事件定义没有名字");
            }
        }
    }
    // 事件时间轴的逐帧下标（载荷长度在时间轴校验里已经走过一遍）
    for (size_t i = 0; i < out.timelines.size(); ++i)
    {
        const SpineGpuTimeline& t = out.timelines[i];
        if (static_cast<SpineTimelineType>(t.type) != SpineTimelineType::kEvent)
        {
            continue;
        }
        for (uint32_t fr = 0; fr < t.payload_count; ++fr)
        {
            const uint8_t* p = out.timeline_payload.data() + t.payload_offset + static_cast<size_t>(fr) * sizeof(SpineGpuEventKey);
            // 没有 chunk 23 ⇒ events 为空 ⇒ 第一个条件必中（有事件时间轴却没有事件定义 = 坏容器）
            if (GetU32(p) >= out.events.size() || GetU32(p + 12) >= out.event_strings.size())
            {
                return Fail("container: 第 " + std::to_string(i) + " 条 event 时间轴第 " + std::to_string(fr) +
                            " 帧的事件 / 字符串下标越界（chunk 23 有 " + std::to_string(out.events.size()) + " 个事件）");
            }
        }
    }

    return Ok();
}
