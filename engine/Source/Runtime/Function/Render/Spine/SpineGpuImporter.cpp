#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

}   // namespace

SpineImportResult ParseSpineSkeletonJson(const char* json, size_t len, const SpineAtlas* atlas,
                                         SpineSourceSkeleton& out)
{
    out.bones.clear();
    out.slots.clear();
    out.attachments.clear();
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

    // 图集页名：容器 chunk 7 要用（纹理引用）。没有图集时留空 —— 但那样需要 UV 的
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
    // 3.8 导出是数组（[{name, attachments}]），老版本是对象；两种都接。
    std::vector<const JsonValue*> skin_attachment_maps;
    const JsonValue* skins = GetMember(doc, "skins");
    if (skins && skins->IsArray())
    {
        for (const JsonValue& sk : skins->GetArray())
        {
            if (const JsonValue* atts = GetMember(sk, "attachments"))
            {
                skin_attachment_maps.push_back(atts);
            }
        }
    }
    else if (skins && skins->IsObject())
    {
        for (auto it = skins->MemberBegin(); it != skins->MemberEnd(); ++it)
        {
            skin_attachment_maps.push_back(&it->value);
        }
    }

    // 收集：按 (slot, 附件名) 去重，先出现的胜出（default skin 在前）
    struct PendingAttachment
    {
        int slot_index;
        std::string name;
        const JsonValue* json;
    };
    std::vector<PendingAttachment> pending;

    for (const JsonValue* atts : skin_attachment_maps)
    {
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
                    if (p.slot_index == slot_index && p.name == att_name)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                {
                    pending.push_back(PendingAttachment {slot_index, att_name, &att_it->value});
                }
            }
        }
    }

    // 按绘制顺序（槽位顺序）排
    std::sort(pending.begin(), pending.end(),
              [](const PendingAttachment& a, const PendingAttachment& b) { return a.slot_index < b.slot_index; });

    for (const PendingAttachment& p : pending)
    {
        const JsonValue& a = *p.json;
        const std::string type = GetString(a, "type", "region");

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

            if (GetMember(a, "parent") != nullptr)
            {
                return Fail("json: 附件 '" + att.name + "' 是 linked mesh（有 parent），本版本导入器不支持");
            }

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
        }
        else
        {
            return Fail("json: 附件 '" + att.name + "' 的类型 '" + type + "' 不支持");
        }

        out.attachments.push_back(std::move(att));
    }

    // ---- animations：只取 flags ----
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
            if (const JsonValue* deform = GetMember(anim, "deform"))
            {
                // deform: { skin: { slot: { attachment: [...] } } }
                if (deform->IsObject())
                {
                    for (auto sit = deform->MemberBegin(); sit != deform->MemberEnd(); ++sit)
                    {
                        if (!sit->value.IsObject())
                        {
                            continue;
                        }
                        for (auto slot_it = sit->value.MemberBegin(); slot_it != sit->value.MemberEnd(); ++slot_it)
                        {
                            const std::string slot_name(slot_it->name.GetString());
                            if (!slot_it->value.IsObject())
                            {
                                continue;
                            }
                            for (auto att_it = slot_it->value.MemberBegin(); att_it != slot_it->value.MemberEnd();
                                 ++att_it)
                            {
                                const std::string att_name(att_it->name.GetString());
                                for (size_t k = 0; k < out.attachments.size(); ++k)
                                {
                                    SpineSourceAttachment& cand = out.attachments[k];
                                    if (cand.name == att_name &&
                                        out.slots[static_cast<size_t>(cand.slot_index)].name == slot_name)
                                    {
                                        // deform 在动的槽位走 CPU fallback（规格 §7）
                                        cand.type = SpineSourceAttachment::Type::kMesh;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
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

}   // namespace

SpineImportResult BakeSpineGpuAsset(const SpineSourceSkeleton& src, SpineGpuAsset& out)
{
    out = SpineGpuAsset {};

    const uint32_t bone_count = static_cast<uint32_t>(src.bones.size());
    const uint32_t slot_count = static_cast<uint32_t>(src.slots.size());

    // ---- pass 1：分类与统计 ----
    uint32_t rigid_verts = 0;
    uint32_t weighted_verts = 0;
    uint32_t index_count = 0;

    for (const SpineSourceAttachment& a : src.attachments)
    {
        const uint32_t vc = a.VertexCount();
        if (a.IsWeighted())
        {
            weighted_verts += vc;
        }
        else
        {
            rigid_verts += vc;
        }
        index_count += static_cast<uint32_t>(a.triangles.size());
    }

    const uint32_t rigid_bytes = rigid_verts * SpineGpuConstants::kRigidVertexStride;
    const uint32_t weighted_offset = (rigid_bytes + SpineGpuConstants::kVertexSegmentAlignment - 1) /
                                     SpineGpuConstants::kVertexSegmentAlignment *
                                     SpineGpuConstants::kVertexSegmentAlignment;
    const uint32_t weighted_bytes = weighted_verts * SpineGpuConstants::kWeightedVertexStride;

    out.rigid_segment_offset = 0;
    out.weighted_segment_offset = weighted_offset;
    out.rigid_vertex_count = rigid_verts;
    out.weighted_vertex_count = weighted_verts;
    out.vertex_buffer.assign(static_cast<size_t>(weighted_offset) + weighted_bytes, 0u);
    out.index_buffer.reserve(index_count);

    uint32_t rigid_written = 0;
    uint32_t weighted_written = 0;

    // ---- pass 2：写顶点 / 索引 / 绘制项 ----
    for (size_t ai = 0; ai < src.attachments.size(); ++ai)
    {
        const SpineSourceAttachment& a = src.attachments[ai];
        const uint32_t vc = a.VertexCount();
        const bool weighted = a.IsWeighted();

        uint32_t vertex_base = 0;
        uint32_t write_offset = 0;

        if (weighted)
        {
            vertex_base = weighted_written;
            write_offset = weighted_offset + weighted_written * SpineGpuConstants::kWeightedVertexStride;

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
                out_v.uv[0] = a.uvs[static_cast<size_t>(v) * 2u];
                out_v.uv[1] = a.uvs[static_cast<size_t>(v) * 2u + 1u];

                std::memcpy(out.vertex_buffer.data() + write_offset +
                                static_cast<size_t>(v) * SpineGpuConstants::kWeightedVertexStride,
                            &out_v, sizeof(out_v));
            }
            weighted_written += vc;
        }
        else
        {
            vertex_base = rigid_written;
            write_offset = rigid_written * SpineGpuConstants::kRigidVertexStride;

            for (uint32_t v = 0; v < vc; ++v)
            {
                SpineRigidVertex out_v {};
                out_v.bind_pos[0] = a.vertices[static_cast<size_t>(v) * 2u];
                out_v.bind_pos[1] = a.vertices[static_cast<size_t>(v) * 2u + 1u];
                out_v.uv[0] = a.uvs[static_cast<size_t>(v) * 2u];
                out_v.uv[1] = a.uvs[static_cast<size_t>(v) * 2u + 1u];
                std::memcpy(out.vertex_buffer.data() + write_offset +
                                static_cast<size_t>(v) * SpineGpuConstants::kRigidVertexStride,
                            &out_v, sizeof(out_v));
            }
            rigid_written += vc;
        }

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
        item.vertex_format = weighted ? SpineVertexFormat::kWeighted4 : SpineVertexFormat::kRigid;
        item.blend_mode = SpineBlendMode::kNormal;
        item.atlas_page = 0;
        item.slot_index = static_cast<uint16_t>(a.slot_index);
        item.attachment_id = static_cast<uint16_t>(ai);
        item.kind = SpineDrawItemKind::kStatic;
        item.pad = 0;
        out.draw_items.push_back(item);
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
    h.bone_count = bone_count;
    h.slot_count = slot_count;
    h.draw_item_count = static_cast<uint32_t>(out.draw_items.size());
    h.vertex_count = rigid_verts + weighted_verts;
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

    // id=1 strings：骨骼名 → 槽位名 → 附件名 → 图集页名，每项 '\0' 结尾
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkStrings;
        for (const std::string& s : asset.bone_names)
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

    // id=4 index_buffer
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

    // id=5 draw_items —— **20 B/项**，尾部 2 B 填充显式写 0（规格 §4）
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

    // id=6 attachments
    {
        ChunkBuilder c;
        c.id = SpineGpuChunkId::kChunkAttachments;
        for (size_t i = 0; i < asset.attachment_names.size(); ++i)
        {
            const uint16_t slot_index = (i < asset.draw_items.size()) ? asset.draw_items[i].slot_index : 0;
            PutU16(c.bytes, slot_index);
            PutU16(c.bytes, static_cast<uint16_t>(i));
        }
        if (!c.bytes.empty())
        {
            chunks.push_back(std::move(c));
        }
    }

    // id=7 atlas_pages
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
            return Fail("container: draw_items 大小不是 20 B 的整数倍");
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
            return Fail("container: attachments 大小不是 4 B 的整数倍");
        }
        attachment_count = f->size / sizeof(SpineGpuAttachmentRecord);
    }

    // ---- 字符串：骨骼名 → 槽位名 → 附件名 → 图集页名 ----
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
        take(out.header.slot_count, out.slot_names);
        take(attachment_count, out.attachment_names);
        take(out.header.atlas_page_count, out.atlas_page_names);
    }

    return Ok();
}
