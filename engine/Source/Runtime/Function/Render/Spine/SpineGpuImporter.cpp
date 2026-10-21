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

// 与参考实现的 Json::getInt(..., fallback) 对齐：非数字取 fallback。
// 用到它的地方（linked mesh 的 "deform"）在 spine 里就是 0/1 的开关。
int GetInt(const JsonValue& v, const char* key, int fallback)
{
    const JsonValue* m = GetMember(v, key);
    if (m == nullptr)
    {
        return fallback;
    }
    if (m->IsInt())
    {
        return m->GetInt();
    }
    return m->IsNumber() ? static_cast<int>(m->GetFloat()) : fallback;
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
    if (reg.rotate)
    {
        // UR=(u,v2) BR=(u,v) BL=(u2,v) UL=(u2,v2)
        out_uv[0] = u2; out_uv[1] = v;    // BL
        out_uv[2] = u2; out_uv[3] = v2;   // UL
        out_uv[4] = u;  out_uv[5] = v2;   // UR
        out_uv[6] = u;  out_uv[7] = v;    // BR
    }
    else
    {
        // UL=(u,v2) UR=(u,v) BR=(u2,v) BL=(u2,v2)
        out_uv[0] = u2; out_uv[1] = v2;   // BL
        out_uv[2] = u;  out_uv[3] = v2;   // UL
        out_uv[4] = u;  out_uv[5] = v;    // UR
        out_uv[6] = u2; out_uv[7] = v;    // BR
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
        // textureHeight = regionWidth  / (v2 - v)
        // textureWidth  = regionHeight / (u2 - u)
        const float texture_height = (reg.v2 - reg.v) != 0.0f ? rh / (reg.v2 - reg.v) : 0.0f;
        const float texture_width = (reg.u2 - reg.u) != 0.0f ? rw / (reg.u2 - reg.u) : 0.0f;
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

// 把解析期收集的 deform 引用落到附件上，并按 inheritDeform 传播。
// ⚠️ 必须在 linked mesh 解引用**之后**、attachments 重排**之前**调用：
//     前者是因为 deform 可以指向 linked mesh（它是最后才插进来的）；
//     后者是因为传播用的是 parent_index（下标，重排后就失效）。
bool ApplyDeformRefs(std::vector<SpineSourceAttachment>& attachments,
                     const std::vector<SpineDeformRef>& refs, std::string& err)
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

    // ---- 版本：本导入器只认 3.8 ----
    const JsonValue* skeleton = GetMember(doc, "skeleton");
    const std::string spine_ver = skeleton ? GetString(*skeleton, "spine", "") : std::string();
    if (spine_ver.size() < 3 || spine_ver.compare(0, 3, "3.8") != 0)
    {
        return Fail("json: spine 版本是 '" + (spine_ver.empty() ? std::string("<未标注>") : spine_ver) +
                    "'，本导入器锁定 3.8（规格 §1.1）。4.x 的 _bones 是 int，布局不同，需要另一条导入路径。");
    }

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
                for (size_t k = 0; k < out.bones.size(); ++k)
                {
                    if (out.bones[k].name == parent)
                    {
                        bone.parent = static_cast<int>(k);
                        break;
                    }
                }
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
            if (GetMember(s, "dark") != nullptr)
            {
                slot.has_dark = true;
                out.uses_tint_black = true;
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
            out.has_clipping = true;
            // 裁剪附件本身不产生几何；受它影响的槽位由**运行时**按 clip 作用域决定，
            // 导入期只能置 kHasClipping（作用域是运行时概念，这里猜不得）。
            continue;
        }

        SpineSourceAttachment att;
        att.name = p.name;
        att.path = GetString(a, "path", "");
        att.slot_index = p.slot_index;
        att.skin_index = p.skin_index;

        const std::string region_name = att.path.empty() ? att.name : att.path;
        const SpineAtlasRegion* reg = atlas ? FindAtlasRegion(*atlas, region_name) : nullptr;

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
        else
        {
            return Fail("json: 附件 '" + att.name + "' 的类型 '" + type + "' 不支持");
        }

        out.attachments.push_back(std::move(att));
    }

    // ---- animations：只取 flags + deform 引用 ----
    // deform 引用**不能**在这里就落到附件上：deform 可以指向 linked mesh，
    // 而它是最后才被 ResolveLinkedMeshes 插进来的。
    std::vector<SpineDeformRef> deform_refs;
    const JsonValue* anims = GetMember(doc, "animations");
    if (anims && anims->IsObject())
    {
        for (auto it = anims->MemberBegin(); it != anims->MemberEnd(); ++it)
        {
            const JsonValue& anim = it->value;
            if (!anim.IsObject())
            {
                continue;
            }
            if (GetMember(anim, "drawOrder") || GetMember(anim, "draworder"))
            {
                out.has_draw_order_timeline = true;
            }
            // deform: { skin: { slot: { attachment: [ {time, offset, vertices}, ... ] } } }
            // 参考实现 SkeletonJson.cpp:1049-1115。帧数据本身是 per-frame 的，
            // 烘不进静态 VB ⇒ 这里只收 (皮肤, 槽位, 附件名)，等 linked mesh 解引用完
            // 再统一落到附件上（deform 可以指向 linked mesh）。
            if (const JsonValue* deform = GetMember(anim, "deform"))
            {
                if (!deform->IsObject())
                {
                    return Fail("json: animations." + std::string(it->name.GetString()) + ".deform 不是对象");
                }
                for (auto sit = deform->MemberBegin(); sit != deform->MemberEnd(); ++sit)
                {
                    const std::string skin_name(sit->name.GetString());
                    if (!sit->value.IsObject())
                    {
                        return Fail("json: deform 里皮肤 '" + skin_name + "' 的值不是对象");
                    }
                    // 参考实现 `findSkin(...)` 返回 NULL 后紧接着 `skin->getAttachment(...)`
                    // ⇒ 皮肤名写错是**空指针**，不是报错。我们要报错。
                    int skin_index = -1;
                    for (size_t i = 0; i < out.skins.size(); ++i)
                    {
                        if (out.skins[i] == skin_name)
                        {
                            skin_index = static_cast<int>(i);
                            break;
                        }
                    }
                    if (skin_index < 0)
                    {
                        return Fail("json: deform 时间轴引用了不存在的皮肤 '" + skin_name +
                                    "'（参考实现在这里是空指针解引用）");
                    }

                    for (auto slot_it = sit->value.MemberBegin(); slot_it != sit->value.MemberEnd(); ++slot_it)
                    {
                        const std::string slot_name(slot_it->name.GetString());
                        if (!slot_it->value.IsObject())
                        {
                            return Fail("json: deform 里槽位 '" + slot_name + "' 的值不是对象");
                        }
                        // 同理：findSlotIndex 返回 -1 ⇒ getAttachment(-1, ...) 也是 UB。
                        int slot_index = -1;
                        for (size_t i = 0; i < out.slots.size(); ++i)
                        {
                            if (out.slots[i].name == slot_name)
                            {
                                slot_index = static_cast<int>(i);
                                break;
                            }
                        }
                        if (slot_index < 0)
                        {
                            return Fail("json: deform 时间轴引用了不存在的槽位 '" + slot_name +
                                        "'（参考实现在这里是 -1 下标）");
                        }

                        for (auto att_it = slot_it->value.MemberBegin(); att_it != slot_it->value.MemberEnd();
                             ++att_it)
                        {
                            const std::string att_name(att_it->name.GetString());
                            if (!att_it->value.IsArray())
                            {
                                return Fail("json: deform 里附件 '" + att_name + "' 的值不是帧数组");
                            }

                            SpineDeformRef ref;
                            ref.skin_index = skin_index;
                            ref.slot_index = slot_index;
                            ref.attachment = att_name;
                            ref.skin_name = skin_name;
                            ref.slot_name = slot_name;

                            // ---- 帧校验：offset + 顶点数不能越界 ----
                            // 参考实现 `deformed[v] = ...`（:1092-1099）**没有**边界检查：
                            // offset 写大了就是越界写。上限要等附件解出来才知道
                            // （deform 可以指向 linked mesh）⇒ 这里只记"需要多长"，
                            // 到 ApplyDeformRefs() 里比对。
                            for (const JsonValue& frame : att_it->value.GetArray())
                            {
                                const JsonValue* verts = GetMember(frame, "vertices");
                                if (verts == nullptr || !verts->IsArray())
                                {
                                    // 没有 vertices ⇒ 该帧是"回到 setup"（:1081-1086）
                                    continue;
                                }
                                const int offset = static_cast<int>(GetFloat(frame, "offset", 0.0f));
                                if (offset < 0)
                                {
                                    return Fail("json: deform 附件 '" + att_name + "' 的帧 offset " +
                                                std::to_string(offset) + " 为负");
                                }
                                const size_t need = static_cast<size_t>(offset) + verts->Size();
                                if (need > ref.required_length)
                                {
                                    ref.required_length = need;
                                }
                            }
                            deform_refs.push_back(std::move(ref));
                        }
                    }
                }
            }
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
        if (!ApplyDeformRefs(out.attachments, deform_refs, err))
        {
            return Fail("json: " + err);
        }
    }

    if (need_sort)
    {
        // 解出来的附件插在末尾 ⇒ 重排一次，保证 (皮肤, 槽位) 有序。
        std::stable_sort(out.attachments.begin(), out.attachments.end(),
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
            if (!ApplyDeformRefs(out_.attachments, deform_refs_, err))
            {
                return r_.Fail("skel: " + err);
            }
        }

        if (need_resort)
        {
            std::stable_sort(out_.attachments.begin(), out_.attachments.end(), CompareAttachmentOrder);
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
    std::vector<bool> event_has_audio_;
    std::vector<SpineLinkedMeshRef> linked_;
    // deform 时间轴命中的 (皮肤, 槽位, 附件名)。**动画段在最后**，而 deform 可以指向
    // linked mesh ⇒ 只能先收着，等 Run() 里 linked mesh 解完再落。
    std::vector<SpineDeformRef> deform_refs_;
    int current_skin_ = -1;   // 正在读的皮肤下标（附件 / linked mesh 都记它）

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

    bool ParseTopLevel()
    {
        std::string hash;
        std::string version;
        if (!r_.String(hash) || !r_.String(version))
        {
            return false;
        }
        if (version.size() < 3 || version.compare(0, 3, "3.8") != 0)
        {
            return Fail("spine 版本是 '" + (version.empty() ? std::string("<空>") : version) +
                        "'，本导入器锁定 3.8（规格 §1.1）。4.x 的 _bones 是 int，布局不同，需要另一条导入路径。");
        }

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

        if (!ParseBones() || !ParseSlots() || !SkipIk() || !SkipTransformConstraints() ||
            !SkipPathConstraints())
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
            if (!SkipAnimation())
            {
                return false;
            }
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
            // rotation,x,y,scaleX,scaleY,shearX,shearY,length
            if (!SkipFloats(8))
            {
                return false;
            }
            int32_t transform_mode = 0;
            if (!r_.Varint(true, transform_mode))
            {
                return false;
            }
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

            if (!r_.Skip(4))
            {
                return false;   // color
            }
            uint8_t dark[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k)
            {
                if (!r_.Byte(dark[k]))
                {
                    return false;
                }
            }
            // 参考实现 :186-189：四个分量**不全是** 0xff 才算有 dark color。
            if (!(dark[0] == 0xff && dark[1] == 0xff && dark[2] == 0xff && dark[3] == 0xff))
            {
                slot.has_dark = true;
                out_.uses_tint_black = true;
            }
            std::string attachment_name;
            if (!r_.String(attachment_name))
            {
                return false;
            }
            int32_t blend_mode = 0;
            if (!r_.Varint(true, blend_mode))
            {
                return false;
            }
            out_.slots.push_back(std::move(slot));
        }
        return true;
    }

    // ---- 约束：全部跳过，但必须按字段数精确跳过 ----
    bool SkipIk()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        for (int32_t i = 0; i < n; ++i)
        {
            std::string name;
            int32_t order = 0;
            if (!r_.String(name) || !r_.Varint(true, order))
            {
                return false;
            }
            if (!SkipVarintArray())
            {
                return false;   // bones
            }
            int32_t target = 0;
            if (!r_.Varint(true, target))
            {
                return false;
            }
            if (!SkipFloats(1))
            {
                return false;   // mix
            }
            // bendDirection(sbyte) + compress + stretch + uniform
            if (!r_.Skip(4))
            {
                return false;
            }
        }
        return true;
    }

    bool SkipTransformConstraints()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        for (int32_t i = 0; i < n; ++i)
        {
            std::string name;
            int32_t order = 0;
            if (!r_.String(name) || !r_.Varint(true, order))
            {
                return false;
            }
            if (!SkipVarintArray())
            {
                return false;
            }
            int32_t target = 0;
            if (!r_.Varint(true, target))
            {
                return false;
            }
            if (!r_.Skip(2))
            {
                return false;   // local + relative
            }
            // offsetRotation, offsetX/Y, offsetScaleX/Y, offsetShearY, rotateMix, translateMix, scaleMix, shearMix
            if (!SkipFloats(10))
            {
                return false;
            }
        }
        return true;
    }

    bool SkipPathConstraints()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        for (int32_t i = 0; i < n; ++i)
        {
            std::string name;
            int32_t order = 0;
            if (!r_.String(name) || !r_.Varint(true, order))
            {
                return false;
            }
            if (!SkipVarintArray())
            {
                return false;
            }
            int32_t target = 0;
            int32_t position_mode = 0;
            int32_t spacing_mode = 0;
            int32_t rotate_mode = 0;
            if (!r_.Varint(true, target) || !r_.Varint(true, position_mode) || !r_.Varint(true, spacing_mode) ||
                !r_.Varint(true, rotate_mode))
            {
                return false;
            }
            // offsetRotation, position, spacing, rotateMix, translateMix
            if (!SkipFloats(5))
            {
                return false;
            }
        }
        return true;
    }

    bool SkipVarintArray()
    {
        int32_t n = 0;
        if (!r_.Varint(true, n))
        {
            return false;
        }
        for (int32_t i = 0; i < n; ++i)
        {
            int32_t dummy = 0;
            if (!r_.Varint(true, dummy))
            {
                return false;
            }
            if (dummy < 0)
            {
                return Fail("约束引用的骨骼下标为负 " + std::to_string(dummy));
            }
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
                int32_t end_slot = 0;
                int32_t vertex_count = 0;
                if (!r_.Varint(true, end_slot) || !r_.Varint(true, vertex_count))
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
                // 与 json 版一致：裁剪附件本身不产生几何，只置 kHasClipping。
                out_.has_clipping = true;
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
                return true;   // 包围盒不渲染
            }
            case kAttPath:
            {
                if (!r_.Skip(2))
                {
                    return false;   // closed + constantSpeed
                }
                int32_t vertex_count = 0;
                if (!r_.Varint(true, vertex_count))
                {
                    return false;
                }
                if (!ReadVertices(vertex_count, nullptr))
                {
                    return false;
                }
                if (vertex_count < 0)
                {
                    return Fail("path 附件 '" + name + "' 顶点数为负");
                }
                if (!SkipFloats(vertex_count / 3))
                {
                    return false;   // lengths
                }
                if (nonessential_ && !r_.Skip(4))
                {
                    return false;
                }
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
    bool ReadVertices(int32_t vertex_count, std::vector<float>* out)
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
        event_has_audio_.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i)
        {
            std::string name;
            int32_t int_value = 0;
            float float_value = 0.0f;
            std::string string_value;
            std::string audio_path;
            if (!r_.String(name) || !r_.Varint(false, int_value) || !r_.Float(float_value) ||
                !r_.String(string_value) || !r_.String(audio_path))
            {
                return false;
            }
            bool has_audio = !audio_path.empty();
            if (has_audio && !SkipFloats(2))
            {
                return false;   // volume, balance
            }
            event_has_audio_.push_back(has_audio);
        }
        return true;
    }

    // ---- animation：只为取 has_draw_order_timeline，其余精确跳过 ----
    bool SkipAnimation()
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
                    for (int32_t f = 0; f < frames; ++f)
                    {
                        std::string s;
                        if (!SkipFloats(1) || !r_.String(s))
                        {
                            return false;
                        }
                    }
                }
                else if (t == kSlotColor || t == kSlotTwoColor)
                {
                    const int colors = (t == kSlotTwoColor) ? 2 : 1;
                    for (int32_t f = 0; f < frames; ++f)
                    {
                        if (!SkipFloats(1) || !r_.Skip(static_cast<size_t>(4) * colors))
                        {
                            return false;
                        }
                        if (f < frames - 1 && !Curve())
                        {
                            return false;
                        }
                    }
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
                for (int32_t f = 0; f < frames; ++f)
                {
                    if (!SkipFloats(1 + values))
                    {
                        return false;
                    }
                    if (f < frames - 1 && !Curve())
                    {
                        return false;
                    }
                }
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
            for (int32_t f = 0; f < frames; ++f)
            {
                if (!SkipFloats(2) || !r_.Skip(3))
                {
                    return false;
                }
                if (f < frames - 1 && !Curve())
                {
                    return false;
                }
            }
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
            for (int32_t f = 0; f < frames; ++f)
            {
                if (!SkipFloats(5))
                {
                    return false;
                }
                if (f < frames - 1 && !Curve())
                {
                    return false;
                }
            }
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
                if (t == kPathPosition || t == kPathSpacing)
                {
                    values = 1;
                }
                else if (t == kPathMix)
                {
                    values = 2;
                }
                else
                {
                    return Fail("path 约束时间轴类型 " + std::to_string(static_cast<int>(t)) + " 未知（3.8 只到 2）");
                }
                for (int32_t f = 0; f < frames; ++f)
                {
                    if (!SkipFloats(1 + values))
                    {
                        return false;
                    }
                    if (f < frames - 1 && !Curve())
                    {
                        return false;
                    }
                }
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

                    for (int32_t f = 0; f < frames; ++f)
                    {
                        // time（用不上，但要跳过）
                        int32_t end = 0;
                        if (!SkipFloats(1) || !r_.Varint(true, end))
                        {
                            return false;
                        }
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
                            if (!SkipFloats(end))
                            {
                                return false;
                            }
                        }
                        if (f < frames - 1 && !Curve())
                        {
                            return false;
                        }
                    }
                    deform_refs_.push_back(std::move(ref));
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
            for (int32_t i = 0; i < draw_order_count; ++i)
            {
                int32_t offset_count = 0;
                if (!SkipFloats(1) || !r_.Varint(true, offset_count))
                {
                    return false;
                }
                if (offset_count < 0)
                {
                    return Fail("绘制顺序帧的 offset 数为负 " + std::to_string(offset_count));
                }
                // 每个 offset 两个 varint（slotIndex, 位移量）
                for (int32_t ii = 0; ii < offset_count; ++ii)
                {
                    int32_t a = 0;
                    int32_t b = 0;
                    if (!r_.Varint(true, a) || !r_.Varint(true, b))
                    {
                        return false;
                    }
                }
            }
        }

        // 事件时间轴（:1079-1107）。⚠️ 末尾那 2 个 float 取决于对应事件有没有 audioPath。
        int32_t ev_n = 0;
        if (!r_.Varint(true, ev_n))
        {
            return false;
        }
        if (ev_n < 0)
        {
            return Fail("事件时间轴帧数为负 " + std::to_string(ev_n));
        }
        for (int32_t i = 0; i < ev_n; ++i)
        {
            int32_t event_index = 0;
            int32_t int_value = 0;
            bool free_string = false;
            if (!SkipFloats(1) || !r_.Varint(true, event_index) || !r_.Varint(false, int_value) ||
                !SkipFloats(1) || !r_.Boolean(free_string))
            {
                return false;
            }
            if (event_index < 0 || static_cast<size_t>(event_index) >= event_has_audio_.size())
            {
                return Fail("事件时间轴引用了不存在的事件下标 " + std::to_string(event_index) + "（事件数是 " +
                            std::to_string(event_has_audio_.size()) + "）");
            }
            if (free_string)
            {
                std::string s;
                if (!r_.String(s))
                {
                    return false;
                }
            }
            if (event_has_audio_[static_cast<size_t>(event_index)] && !SkipFloats(2))
            {
                return false;
            }
        }
        return true;
    }

    // readCurve（:1112-1127）：只有 bezier 带 4 个 float。
    bool Curve()
    {
        uint8_t t = 0;
        if (!r_.Byte(t))
        {
            return false;
        }
        if (t == kCurveBezier)
        {
            return SkipFloats(4);
        }
        if (t == kCurveLinear || t == kCurveStepped)
        {
            return true;
        }
        return Fail("curve 类型 " + std::to_string(static_cast<int>(t)) + " 未知（3.8 只到 2）");
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

}   // namespace

SpineImportResult BakeSpineGpuAsset(const SpineSourceSkeleton& src, SpineGpuAsset& out)
{
    out = SpineGpuAsset {};

    const uint32_t bone_count = static_cast<uint32_t>(src.bones.size());
    const uint32_t slot_count = static_cast<uint32_t>(src.slots.size());

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
        std::vector<uint8_t> blob;
        blob.reserve(static_cast<size_t>(vc) * (weighted ? SpineGpuConstants::kWeightedVertexStride
                                                         : SpineGpuConstants::kRigidVertexStride));
        uint32_t vertex_base = 0;

        if (weighted)
        {
            // 展开 spine 的扁平顶点数组：
            //   每顶点 [n, (boneIdx, x, y, w) × n]   —— SkeletonJson::readVertices
            size_t p = 0;
            for (uint32_t v = 0; v < vc; ++v)
            {
                if (p >= a.vertices.size())
                {
                    return Fail("bake: 附件 '" + a.name + "' 的 vertices 在第 " + std::to_string(v) +
                                " 个顶点处提前结束");
                }

                const float nf = a.vertices[p++];
                const int n = static_cast<int>(nf);
                if (n <= 0 || n > static_cast<int>(SpineGpuConstants::kMaxInfluences))
                {
                    return Fail("bake: 附件 '" + a.name + "' 第 " + std::to_string(v) + " 个顶点有 " +
                                std::to_string(n) + " 个影响，超过 MAX_INFLUENCES=" +
                                std::to_string(SpineGpuConstants::kMaxInfluences) +
                                "。不静默截断 —— 截断会让形状悄悄错。");
                }
                if (p + static_cast<size_t>(n) * 4u > a.vertices.size())
                {
                    return Fail("bake: 附件 '" + a.name + "' 的 vertices 在第 " + std::to_string(v) +
                                " 个顶点处越界");
                }

                float w[SpineGpuConstants::kMaxInfluences] = {0, 0, 0, 0};
                uint8_t w8[SpineGpuConstants::kMaxInfluences] = {0, 0, 0, 0};
                uint16_t idx[SpineGpuConstants::kMaxInfluences] = {0, 0, 0, 0};
                float bind[SpineGpuConstants::kMaxInfluences][2] = {};

                for (int k = 0; k < n; ++k)
                {
                    const float bone_f = a.vertices[p];
                    const float vx = a.vertices[p + 1];
                    const float vy = a.vertices[p + 2];
                    const float ww = a.vertices[p + 3];
                    p += 4;

                    // 3.8 的 _bones 是 unsigned short，截断会直接毁掉蒙皮
                    if (bone_f < 0.0f || bone_f > 65535.0f || static_cast<uint32_t>(bone_f) >= bone_count)
                    {
                        return Fail("bake: 附件 '" + a.name + "' 第 " + std::to_string(v) + " 个顶点引用骨骼 " +
                                    std::to_string(static_cast<int>(bone_f)) + "，超出范围（bone_count=" +
                                    std::to_string(bone_count) + "）");
                    }

                    idx[k] = static_cast<uint16_t>(bone_f);
                    w[k] = ww;
                    bind[k][0] = vx;
                    bind[k][1] = vy;
                }

                std::string err;
                if (!QuantizeWeights(w, n, w8, err))
                {
                    return Fail("bake: 附件 '" + a.name + "' 第 " + std::to_string(v) + " 个顶点：" + err);
                }

                SpineWeightedVertex out_v {};
                for (int k = 0; k < static_cast<int>(SpineGpuConstants::kMaxInfluences); ++k)
                {
                    out_v.bind_pos[k][0] = bind[k][0];
                    out_v.bind_pos[k][1] = bind[k][1];
                    out_v.bone_idx[k] = idx[k];
                    out_v.weight[k] = w8[k];
                }
                blob.resize(blob.size() + sizeof(out_v));
                std::memcpy(blob.data() + blob.size() - sizeof(out_v), &out_v, sizeof(out_v));
            }
        }
        else
        {
            for (uint32_t v = 0; v < vc; ++v)
            {
                SpineRigidVertex out_v {};
                out_v.bind_pos[0] = a.vertices[static_cast<size_t>(v) * 2u];
                out_v.bind_pos[1] = a.vertices[static_cast<size_t>(v) * 2u + 1u];
                blob.resize(blob.size() + sizeof(out_v));
                std::memcpy(blob.data() + blob.size() - sizeof(out_v), &out_v, sizeof(out_v));
            }
        }

        // ---- 顶点块去重：几何 + 蒙皮完全相同才共用 ----
        ExactBlobPool& pool = weighted ? weighted_pool : rigid_pool;
        vertex_base = pool.Intern(blob.data(), blob.size()).second;

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
        for (uint16_t t : a.triangles)
        {
            out.index_buffer.push_back(static_cast<uint16_t>(t + vertex_base));
        }

        SpineDrawItem item {};
        item.index_offset = index_offset;
        item.index_count = static_cast<uint32_t>(a.triangles.size());
        item.uv_offset = uv_offset;
        item.vertex_format = weighted ? SpineVertexFormat::kWeighted4 : SpineVertexFormat::kRigid;
        item.blend_mode = SpineBlendMode::kNormal;
        item.atlas_page = 0;
        item.slot_index = static_cast<uint16_t>(a.slot_index);
        item.attachment_id = static_cast<uint16_t>(ai);
        item.skin_id = static_cast<uint16_t>(a.skin_index);
        // deform（FFD）命中的附件顶点每帧都变 ⇒ 静态 VB 里那份绑定姿势不算数
        // ⇒ 走 CPU fallback（规格 §7.1）。**只有被命中的绘制项**回退，
        // 不是整个资产 —— 这和 kHasDrawOrderTimeline 那种"整体走 CPU"是两回事。
        item.kind = a.deformed ? SpineDrawItemKind::kFallback : SpineDrawItemKind::kStatic;
        item.pad = 0;
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
    for (const SpineSourceSlot& s : src.slots)
    {
        out.slot_names.push_back(s.name);
    }
    out.attachment_names.reserve(src.attachments.size());
    for (const SpineSourceAttachment& a : src.attachments)
    {
        out.attachment_names.push_back(a.name);
    }
    out.atlas_page_names = src.atlas_pages;

    // ---- header ----
    SpineGpuHeader& h = out.header;
    h = SpineGpuHeader {};
    h.magic = SpineGpuConstants::kMagic;
    h.version = SpineGpuConstants::kFormatVersion;
    h.spine_runtime = SpineGpuConstants::kSpineRuntime38;
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
    // 有绘制项因 deform 走了 CPU fallback ⇒ 运行时得为它准备每帧动态 VB。
    // 逐个绘制项看 kind 也能知道，但那是 O(绘制项) 的扫描 —— 标志位让运行时
    // 一眼决定"要不要建动态 VB"。
    for (const SpineDrawItem& item : out.draw_items)
    {
        if (item.kind == SpineDrawItemKind::kFallback)
        {
            h.flags |= SpineGpuFlags::kHasDeform;
            break;
        }
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

void PutU16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
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

    // id=1 strings：骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名，每项 '\0' 结尾
    //                        ↑ 皮肤名插在这里：读端靠 header.skin_count 切分
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkStrings;
        for (const std::string& s : asset.bone_names)
        {
            PutCString(c.bytes, s);
        }
        for (const std::string& s : asset.skin_names)
        {
            PutCString(c.bytes, s);
        }
        for (const std::string& s : asset.slot_names)
        {
            PutCString(c.bytes, s);
        }
        for (const std::string& s : asset.attachment_names)
        {
            PutCString(c.bytes, s);
        }
        for (const std::string& s : asset.atlas_page_names)
        {
            PutCString(c.bytes, s);
        }
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

    // id=3 vertex_buffer（段起点 8 B 对齐 ⇒ chunk 本身也要 8 对齐）
    if (!asset.vertex_buffer.empty())
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkVertexBuffer;
        c.alignment = SpineGpuConstants::kVertexSegmentAlignment;
        PutBytes(c.bytes, asset.vertex_buffer.data(), asset.vertex_buffer.size());
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

    // id=6 draw_items —— **24 B/项**，未用字节显式写 0（规格 §4）
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

    // ---- 绘制项（20 B/项） ----
    if (const Found* f = find_chunk(SpineGpuChunkId::kChunkDrawItems))
    {
        if (f->size % sizeof(SpineDrawItem) != 0)
        {
            return Fail("container: draw_items 大小不是 " + std::to_string(sizeof(SpineDrawItem)) +
                        " B 的整数倍（v3 起一项是 24 B：多了 uv_offset）");
        }
        const uint32_t count = f->size / static_cast<uint32_t>(sizeof(SpineDrawItem));
        out.draw_items.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(&out.draw_items[i], data + f->offset + static_cast<size_t>(i) * sizeof(SpineDrawItem),
                        sizeof(SpineDrawItem));
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
        }
        attachment_count = count;
    }

    // ---- 字符串：骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名 ----
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
        take(attachment_count, out.attachment_names);
        take(out.header.atlas_page_count, out.atlas_page_names);
    }

    return Ok();
}
