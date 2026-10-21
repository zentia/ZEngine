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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

int g_checks = 0;
int g_failed = 0;
// 同一批检查跑两遍时（如 .skel 的 nonessential 开/关）用来区分是哪一轮失败，
// 免得每条消息都要手写前缀。
std::string g_prefix;

void Check(bool cond, const char* what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failed;
        std::printf("  [FAIL] %s%s\n", g_prefix.c_str(), what);
    }
}

void CheckEq(long long got, long long want, const char* what)
{
    ++g_checks;
    if (got != want)
    {
        ++g_failed;
        std::printf("  [FAIL] %s%s (got %lld, want %lld)\n", g_prefix.c_str(), what, got, want);
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
//
// ⚠️ skins 写成**对象**（皮肤名 → 槽位 → 附件），不是数组。这是 3.8 唯一的写法：
//    SkeletonJson.cpp:439 `new Skin(skinMap->_name)` 取的是**成员名**，
//    而 Json.cpp:402-450 的 parseArray 里数组元素是 `new Json(NULL)` 且从不赋 _name ——
//    数组形式在参考实现里会拿到空皮肤名。
const char* kSkeleton = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "mesh" }
  ],
  "skins": {
    "default": {
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
    }
  }
})";

// ---------------------------------------------------------------------
// 手写 .skel 字节流
// ---------------------------------------------------------------------
// 按 SkeletonBinary.cpp 的字段顺序逐字节拼出来，内容与上面的 json 夹具**等价**
// ⇒ 两条完全独立的解析路径必须产出逐字节相同的烘焙结果。这是本文件最强的检查：
// 二进制那边的字段顺序、大端、varint、weighted 扁平编码只要错一处，两边就会分叉。
//
// 写 varint 照抄 spine 的 writeVarint（LEB128，末字节不带续位）：
//   v < 2^7 → 1 字节；< 2^14 → 2；< 2^21 → 3；< 2^28 → 4；否则 5
struct SkelWriter
{
    std::vector<uint8_t> b;

    void U8(uint8_t v) { b.push_back(v); }

    void Varint(int32_t v, bool optimize_positive = true)
    {
        uint32_t value = 0;
        if (optimize_positive)
        {
            value = static_cast<uint32_t>(v);
        }
        else
        {
            // zigzag：与读端的 (u >> 1) ^ -(u & 1) 互逆
            value = (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
        }
        if ((value >> 7) == 0)
        {
            U8(static_cast<uint8_t>(value));
            return;
        }
        if ((value >> 14) == 0)
        {
            U8(static_cast<uint8_t>((value & 0x7Fu) | 0x80u));
            U8(static_cast<uint8_t>(value >> 7));
            return;
        }
        if ((value >> 21) == 0)
        {
            U8(static_cast<uint8_t>((value & 0x7Fu) | 0x80u));
            U8(static_cast<uint8_t>(((value >> 7) & 0x7Fu) | 0x80u));
            U8(static_cast<uint8_t>(value >> 14));
            return;
        }
        if ((value >> 28) == 0)
        {
            U8(static_cast<uint8_t>((value & 0x7Fu) | 0x80u));
            U8(static_cast<uint8_t>(((value >> 7) & 0x7Fu) | 0x80u));
            U8(static_cast<uint8_t>(((value >> 14) & 0x7Fu) | 0x80u));
            U8(static_cast<uint8_t>(value >> 21));
            return;
        }
        U8(static_cast<uint8_t>((value & 0x7Fu) | 0x80u));
        U8(static_cast<uint8_t>(((value >> 7) & 0x7Fu) | 0x80u));
        U8(static_cast<uint8_t>(((value >> 14) & 0x7Fu) | 0x80u));
        U8(static_cast<uint8_t>(((value >> 21) & 0x7Fu) | 0x80u));
        U8(static_cast<uint8_t>(value >> 28));
    }

    // varint 长度含结尾 '\0'，但结尾不落盘；空串写 0
    void Str(const std::string& s)
    {
        if (s.empty())
        {
            Varint(0);
            return;
        }
        Varint(static_cast<int32_t>(s.size() + 1));
        for (char c : s)
        {
            U8(static_cast<uint8_t>(c));
        }
    }

    void F(float v)
    {
        uint32_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        U8(static_cast<uint8_t>((u >> 24) & 0xFFu));
        U8(static_cast<uint8_t>((u >> 16) & 0xFFu));
        U8(static_cast<uint8_t>((u >> 8) & 0xFFu));
        U8(static_cast<uint8_t>(u & 0xFFu));
    }

    void Bool(bool v) { U8(v ? 1u : 0u); }

    void ColorWhite()
    {
        U8(0xFF);
        U8(0xFF);
        U8(0xFF);
        U8(0xFF);
    }

    void ShortArray(const std::vector<uint16_t>& a)
    {
        Varint(static_cast<int32_t>(a.size()));
        for (uint16_t v : a)
        {
            U8(static_cast<uint8_t>(v >> 8));
            U8(static_cast<uint8_t>(v & 0xFFu));
        }
    }
};

// with_draw_order 时多写一个只含绘制顺序时间轴的动画（用于验 kHasDrawOrderTimeline）。
// linked_mesh 时把 mesh 的类型字节写成 3（应报错）。
// nonessential 必须两种都测 —— **Spine 编辑器默认就带 nonessential 数据**，
// 而它一开，后面几乎每个结构的字段数都不一样（骨骼多一个 color、mesh 多 edges+宽高、
// 头部多 fps/imagesPath/audioPath）。只测 false 等于把最常见的情况漏掉。
std::vector<uint8_t> BuildSkelFixture(bool with_draw_order, bool linked_mesh = false,
                                      const std::string& version = "3.8.99", bool nonessential = false,
                                      const std::string& linked_parent = "mesh", bool with_second_skin = false,
                                      bool with_deform = false, bool linked_inherit_deform = true,
                                      int deform_length = 10, bool with_clipping = false,
                                      int clip_end_slot = 2, int deform_slot = 1,
                                      bool nested_clip = false, int nested_clip_end_slot = 3,
                                      bool clip_weighted = false, bool with_path = false,
                                      const std::string& deform_name = "mesh")
{
    SkelWriter w;

    // 裁剪夹具的槽位布局。嵌套版多一个槽位，因为第二条裁剪附件要**占掉**一个槽位
    // （于是那个槽位没有几何）—— 这正是它此前一直没有夹具的原因。
    //   非裁剪    : s0=quad, s1=mesh
    //   裁剪      : s0=quad, s1=clip,  s2=mesh, s3=tail
    //   裁剪+嵌套 : s0=quad, s1=clipA, s2=clipB, s3=mesh, s4=tail
    const bool clip5 = with_clipping && nested_clip;
    const int slot_total = with_clipping ? (clip5 ? 5 : 4) : 2;
    const int mesh_slot = with_clipping ? (clip5 ? 3 : 2) : 1;
    const int tail_slot = with_clipping ? (clip5 ? 4 : 3) : -1;

    // ---- 头：hash + version（**没有魔数**）----
    w.Str("hash");
    w.Str(version);
    w.F(0.0f);           // width
    w.F(0.0f);           // height
    w.Bool(nonessential);
    if (nonessential)
    {
        w.F(30.0f);          // fps
        w.Str("images/");    // imagesPath
        w.Str("");           // audioPath
    }

    // ---- bones ----
    w.Varint(2);
    w.Str("root");
    w.F(0.0f); w.F(0.0f); w.F(0.0f);       // rotation, x, y
    w.F(1.0f); w.F(1.0f);                   // scaleX, scaleY
    w.F(0.0f); w.F(0.0f);                   // shearX, shearY
    w.F(0.0f);                              // length
    w.Varint(0);                            // transformMode
    if (nonessential)
    {
        w.ColorWhite();                     // bone color（int）
    }
    w.Str("hip");
    w.Varint(0);                            // parent = root（i != 0 才有）
    w.F(0.0f); w.F(0.0f); w.F(0.0f);
    w.F(1.0f); w.F(1.0f);
    w.F(0.0f); w.F(0.0f);
    w.F(0.0f);
    w.Varint(0);
    if (nonessential)
    {
        w.ColorWhite();
    }

    // ---- slots ----
    w.Varint(slot_total);
    w.Str("s0");
    w.Varint(0);            // bone = root
    w.ColorWhite();         // color
    w.ColorWhite();         // dark：全 0xff ⇒ **不**开 tint black
    w.Str("quad");
    w.Varint(0);            // blendMode = Normal
    if (with_clipping)
    {
        // 裁剪附件所在槽位。**它自己不被裁**：参考实现一命中 clipping 就
        // clipStart() + continue（SpineMesh.h:153-159）。
        w.Str("s1");
        w.Varint(1);
        w.ColorWhite();
        w.ColorWhite();
        w.Str("clip");
        w.Varint(0);
        if (clip5)
        {
            // 第二条裁剪，**嵌套**在 clipA 的作用域里 ⇒ 永远开不起来
            // （clipStart 在已有裁剪时返回 0，SkeletonClipping.cpp:49-51）。
            w.Str("s2");
            w.Varint(1);
            w.ColorWhite();
            w.ColorWhite();
            w.Str("clipb");
            w.Varint(0);
        }
    }
    w.Str("s" + std::to_string(mesh_slot));
    w.Varint(1);            // bone = hip
    w.ColorWhite();
    w.ColorWhite();
    w.Str("mesh");
    w.Varint(0);
    if (with_clipping)
    {
        // 作用域**边界**槽位：end 指向它时它被裁，end 指向它前面时它不被裁。
        w.Str("s" + std::to_string(tail_slot));
        w.Varint(1);
        w.ColorWhite();
        w.ColorWhite();
        w.Str("tail");
        w.Varint(0);
    }

    // ---- 约束：全 0 ----
    w.Varint(0);            // IK
    w.Varint(0);            // transform
    w.Varint(0);            // path

    // ---- default skin ----
    w.Varint(slot_total);   // skin 里涉及的槽位数（本夹具每个槽位都有附件）

    // 槽位 0：region "quad"（with_path 时再加一个 path 附件）
    w.Varint(0);            // slotIndex
    w.Varint(with_path ? 2 : 1);
    w.Str("quad");
    w.Str("");              // 附件名留空 ⇒ 取 skin 里的 key
    w.U8(0);                // AttachmentType_Region
    w.Str("");              // path 留空 ⇒ 取 name
    // ⚠️ 二进制顺序：rotation, x, y, scaleX, scaleY, width, height
    w.F(0.0f);              // rotation
    w.F(0.0f);              // x
    w.F(0.0f);              // y
    w.F(1.0f);              // scaleX
    w.F(1.0f);              // scaleY
    w.F(64.0f);             // width
    w.F(32.0f);             // height
    w.ColorWhite();

    if (with_path)
    {
        // 槽位 0 的第二个附件：**path**（type 4）。它不渲染、没有绘制项，
        // 但 `PathAttachment` 继承 `VertexAttachment` ⇒ deform 时间轴**可以**指向它。
        // 本工程实测 88 处（如 `K_Lvbu` 槽位 121）—— 指向它必须是"跳过"而不是"报错"。
        w.Str("path1");
        w.Str("");          // 附件名留空 ⇒ 取 skin 里的 key
        w.U8(4);            // AttachmentType_Path
        w.Bool(false);      // closed
        w.Bool(false);      // constantSpeed
        w.Varint(3);        // vertexCount
        w.Bool(false);      // readVertices：非 weighted
        w.F(0.0f); w.F(0.0f);
        w.F(10.0f); w.F(0.0f);
        w.F(20.0f); w.F(10.0f);
        w.F(1.0f);          // lengths：vertexCount / 3 = 1
        if (nonessential)
        {
            w.ColorWhite();
        }
    }

    // 裁剪附件（SkeletonBinary.cpp:636-646）
    // weighted 时 `verts` 是扁平编码：每顶点 [n, (boneIdx, x, y, w) × n]
    auto WriteClip = [&](int slot, const char* name, int end_slot, const float* verts, int vert_count,
                         bool weighted, size_t vert_floats)
    {
        w.Varint(slot);         // slotIndex
        w.Varint(1);            // 1 个附件
        w.Str(name);
        w.Str("");              // 附件名留空 ⇒ 取 skin 里的 key
        w.U8(6);                // AttachmentType_Clipping
        // ⚠️ 顺序是 **endSlotIndex 在前、vertexCount 在后**（:637-638），与 json 那边
        // 一个是下标、一个是名字，别互相抄。
        w.Varint(end_slot);
        w.Varint(vert_count);   // vertexCount（**顶点数**，不是 float 数）
        // ⚠️ 二进制有**显式**的 weighted 标志；json 那边只能靠数组长度猜（:1218）。
        w.Bool(weighted);
        if (!weighted)
        {
            for (size_t i = 0; i < vert_floats; ++i)
            {
                w.F(verts[i]);
            }
        }
        else
        {
            // ⚠️ weighted 的 `verts` 是**扁平编码**（每顶点 [n, (boneIdx,x,y,w) × n]），
            // 但二进制落盘时 n 与 boneIdx 是 **varint**，只有 x/y/w 是 float
            // （SkeletonBinary.cpp:658-691）—— 照抄 json 的"全是 float"会整段错位。
            size_t i = 0;
            for (int v = 0; v < vert_count; ++v)
            {
                const int n = static_cast<int>(verts[i++]);
                w.Varint(n);
                for (int k = 0; k < n; ++k)
                {
                    w.Varint(static_cast<int>(verts[i++]));   // boneIdx
                    w.F(verts[i++]);                          // x
                    w.F(verts[i++]);                          // y
                    w.F(verts[i++]);                          // w
                }
            }
        }
        if (nonessential)
        {
            w.ColorWhite();     // color
        }
    };

    if (with_clipping)
    {
        // 与 json 夹具**逐点相同**：v6 起裁剪多边形要落盘，两边不一样就对不上了。
        static const float kClipA[] = {0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f, 0.0f, 10.0f};
        static const float kClipB[] = {0.0f, 0.0f, 20.0f, 0.0f, 20.0f, 20.0f, 0.0f, 20.0f};
        // weighted 版：2 个顶点，各 1 个影响（bone 0 与 bone 1）
        static const float kClipW[] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                       1.0f, 1.0f, 10.0f, 10.0f, 1.0f};
        if (clip_weighted)
        {
            WriteClip(1, "clip", clip_end_slot, kClipW, 2, true, sizeof(kClipW) / sizeof(kClipW[0]));
        }
        else
        {
            WriteClip(1, "clip", clip_end_slot, kClipA, 4, false, 8);
        }
        if (clip5)
        {
            WriteClip(2, "clipb", nested_clip_end_slot, kClipB, 4, false, 8);
        }
    }

    // mesh "mesh"（4 顶点，第 2 个顶点 2 影响 0.6/0.4）
    w.Varint(mesh_slot);
    w.Varint(linked_mesh ? 2 : 1);
    w.Str("mesh");
    w.Str("");
    w.U8(2);                // AttachmentType_Mesh
    w.Str("");
    w.ColorWhite();
    w.Varint(4);            // vertexCount
    // regionUVs：与 json 夹具的 "uvs" 一致
    w.F(0.0f); w.F(0.0f); w.F(1.0f); w.F(0.0f); w.F(1.0f); w.F(1.0f); w.F(0.0f); w.F(1.0f);
    w.ShortArray({0, 1, 2, 0, 2, 3});
    w.Bool(true);           // weighted
    w.Varint(1); w.Varint(0); w.F(-10.0f); w.F(-5.0f); w.F(1.0f);
    w.Varint(2);
    w.Varint(1); w.F(20.0f); w.F(10.0f); w.F(0.6f);
    w.Varint(0); w.F(5.0f); w.F(7.0f); w.F(0.4f);
    w.Varint(1); w.Varint(1); w.F(3.0f); w.F(4.0f); w.F(1.0f);
    w.Varint(1); w.Varint(0); w.F(8.0f); w.F(2.0f); w.F(1.0f);
    w.Varint(0);            // hullLength
    if (nonessential)
    {
        w.ShortArray({});   // edges
        w.F(64.0f);         // width
        w.F(32.0f);         // height
    }

    // linked mesh：几何继承 "mesh"，贴图区域用自己的 "quad"
    if (linked_mesh)
    {
        w.Str("mesh-linked");
        w.Str("");              // 附件名留空 ⇒ 取 skin 里的 key
        w.U8(3);                // AttachmentType_Linkedmesh
        w.Str("quad");          // path（自己的图集 region）
        w.ColorWhite();         // color
        w.Str("");              // skin（空 ⇒ default skin）
        w.Str(linked_parent);   // parent
        // inheritDeform：父附件的 deform 是否也作用于继承方
        // （MeshAttachment::applyDeform，:94-95）。导入期要用 —— 继承方得一起回退。
        w.Bool(linked_inherit_deform);
        if (nonessential)
        {
            w.F(64.0f);         // width
            w.F(32.0f);         // height
        }
    }

    // region "tail"，path 指向图集里已有的 "quad"（图集只有 quad / mesh 两块）
    if (with_clipping)
    {
        w.Varint(tail_slot);    // slotIndex
        w.Varint(1);            // 1 个附件
        w.Str("tail");
        w.Str("");              // 附件名留空 ⇒ 取 skin 里的 key
        w.U8(0);                // AttachmentType_Region
        w.Str("quad");          // path ⇒ 图集 region 用 "quad"
        w.F(0.0f);              // rotation
        w.F(0.0f);              // x
        w.F(0.0f);              // y
        w.F(1.0f);              // scaleX
        w.F(1.0f);              // scaleY
        w.F(16.0f);             // width
        w.F(8.0f);              // height
        w.ColorWhite();
    }

    // ---- 具名 skin ----
    // with_second_skin 时写一个 skin2，它同样在槽位 s0 下放一个 "quad"，
    // 但尺寸 32×16 ≠ default 的 64×32 ⇒ 两份几何必须**都**留在容器里。
    w.Varint(with_second_skin ? 1 : 0);
    if (with_second_skin)
    {
        w.Str("skin2");
        w.Varint(1);            // 1 个槽位
        w.Varint(0);            // slotIndex = s0
        w.Varint(1);            // 1 个附件
        w.Str("quad");
        w.Str("");              // 附件名留空 ⇒ 取 skin 里的 key
        w.U8(0);                // AttachmentType_Region
        w.Str("");              // path 留空 ⇒ 取 name
        w.F(0.0f);              // rotation
        w.F(0.0f);              // x
        w.F(0.0f);              // y
        w.F(1.0f);              // scaleX
        w.F(1.0f);              // scaleY
        w.F(32.0f);             // width
        w.F(16.0f);             // height
        w.ColorWhite();
    }

    // ---- events：0 个 ----
    w.Varint(0);

    // ---- animations ----
    if (with_draw_order || with_deform)
    {
        w.Varint(1);
        w.Str("walk");
        w.Varint(0);        // slot 时间轴
        w.Varint(0);        // bone 时间轴
        w.Varint(0);        // IK
        w.Varint(0);        // transform 约束
        w.Varint(0);        // path 约束

        // ---- deform 时间轴（SkeletonBinary.cpp:957-1030）----
        w.Varint(with_deform ? 1 : 0);
        if (with_deform)
        {
            w.Varint(0);            // 皮肤下标 = default
            w.Varint(1);            // 1 个槽位
            w.Varint(deform_slot);  // slotIndex（裁剪夹具里 mesh 在 s2）
            w.Varint(1);            // 1 个附件
            w.Str(deform_name);     // 附件名（with_path 时指向 "path1"）
            w.Varint(1);            // 1 帧
            w.F(0.0f);              // time
            // ⚠️ 读端是 `end = readVarint; start = readVarint; end += start;` 再读
            //    end-start 个 float ⇒ **落盘第二个 varint 是"个数"，不是"终点"**。
            w.Varint(deform_length);
            w.Varint(0);            // start（= json 的 "offset"）
            for (int i = 0; i < deform_length; ++i)
            {
                w.F(0.0f);
            }
            // 最后一帧后面没有 curve
        }

        // ---- 绘制顺序 ----
        w.Varint(with_draw_order ? 1 : 0);
        if (with_draw_order)
        {
            w.F(0.0f);          // time
            w.Varint(0);        // offsetCount
        }
        w.Varint(0);        // 事件时间轴
    }
    else
    {
        w.Varint(0);
    }

    return w.b;
}

// 带 linked mesh 的夹具：槽位 s1 里除了真 mesh，还有一个 `mesh-linked`，
// parent = "mesh"，但**自己的** path 指向另一个图集 region（"quad"）。
// 选不同 region 是故意的：linked mesh 的 UV 必须用**自己的** region 重算，
// 若错用父附件的 region，UV 会和父附件一模一样 —— 这条断言才抓得住。
const char* kSkeletonLinked = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "mesh" }
  ],
  "skins": {
    "default": {
      "s0": { "quad": { "x": 0, "y": 0, "width": 64, "height": 32, "rotation": 0 } },
      "s1": {
        "mesh": {
          "type": "mesh",
          "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
          "triangles": [ 0,1,2, 0,2,3 ],
          "vertices": [ 1, 0,-10,-5,1,
                        2, 1, 20,10,0.6, 0,5,7,0.4,
                        1, 1, 3,4,1,
                        1, 0, 8,2,1 ]
        },
        "mesh-linked": { "type": "linkedmesh", "path": "quad", "parent": "mesh" }
      }
    }
  }
})";

// 跨皮肤的 linked mesh：**父附件在哪个皮肤里找，由 "skin" 字段决定**，
// **不是**"它所在的那个皮肤"（SkeletonJson.cpp:653 / SkeletonBinary.cpp:302）。
// 两个皮肤里都有名为 "eye" 的 mesh，但顶点完全不同（default 是 -10/20/3/8，
// skin2 是 99/98/97/96）；skin2 里的 "eye-linked" 省略 skin 字段 ⇒ 必须拿到
// **default** 的那一份顶点。照错了形状会静默变成另一个皮肤的样子。
// %SKINFIELD% 由 BuildCrossSkin() 替换。
const char* kSkeletonLinkedCrossSkin = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" } ],
  "slots": [ { "name": "s1", "bone": "root" } ],
  "skins": {
    "default": {
      "s1": {
        "eye": {
          "type": "mesh", "path": "mesh",
          "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
          "triangles": [ 0,1,2, 0,2,3 ],
          "vertices": [ 1, 0,-10,-5,1,  1, 0,20,10,1,  1, 0,3,4,1,  1, 0,8,2,1 ]
        }
      }
    },
    "skin2": {
      "s1": {
        "eye": {
          "type": "mesh", "path": "mesh",
          "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
          "triangles": [ 0,1,2, 0,2,3 ],
          "vertices": [ 1, 0,99,99,1,  1, 0,98,98,1,  1, 0,97,97,1,  1, 0,96,96,1 ]
        },
        "eye-linked": { "type": "linkedmesh", "path": "quad", "parent": "eye"%SKINFIELD% }
      }
    }
  }
})";

// 两个皮肤：default 与 skin2，**同一个槽位 s0 下都有名为 "quad" 的附件**，
// 但尺寸不同。这就是容器 v2 加 skin 维度要解决的案子：
// v1 会把两份合并成一份（先出现的胜出）⇒ skin2 的换装直接消失。
const char* kSkeletonTwoSkins = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "mesh" }
  ],
  "skins": {
    "default": {
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
    },
    "skin2": {
      "s0": { "quad": { "x": 0, "y": 0, "width": 32, "height": 16, "rotation": 0 } }
    }
  }
})";

// deform（FFD）夹具 = kSkeletonLinked 的骨架 + 一条 deform 时间轴，命中 default/s1/mesh。
//
// 关键长度：mesh 的顶点是 4 个，影响数分别 1/2/1/1 ⇒ **总影响数 5**
// ⇒ weighted 的 deform 长度 = 5 × 2 = **10** 个 float
//   （SkeletonJson.cpp:1071 `deformLength = verts.size() / 3 * 2`，注意是**每影响** 2 个，
//    不是每顶点 2 个 —— 运行时 computeWorldVertices 里 f 也是随影响 +2，
//    VertexAttachment.cpp:115-127）。
// 下面那帧正好写 10 个 float，是**贴着上限**的；越界那条用例改 offset 就能触发。
//
// %DEFORMFIELD% 由 BuildDeform() 替换成 linked mesh 的 inheritDeform 开关。
// ⚠️ json 里这个字段就叫 **"deform"**（不是 "inheritDeform"），默认 1
// （SkeletonJson.cpp:572）。
const char* kSkeletonDeform = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "mesh" }
  ],
  "skins": {
    "default": {
      "s0": { "quad": { "x": 0, "y": 0, "width": 64, "height": 32, "rotation": 0 } },
      "s1": {
        "mesh": {
          "type": "mesh",
          "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
          "triangles": [ 0,1,2, 0,2,3 ],
          "vertices": [ 1, 0,-10,-5,1,
                        2, 1, 20,10,0.6, 0,5,7,0.4,
                        1, 1, 3,4,1,
                        1, 0, 8,2,1 ]
        },
        "mesh-linked": { "type": "linkedmesh", "path": "quad", "parent": "mesh"%DEFORMFIELD% }
      }
    }
  },
  "animations": {
    "walk": {
      "deform": {
        "default": {
          "s1": {
            "mesh": [ { "time": 0, "offset": 0,
                        "vertices": [ 0,0, 0,0, 0,0, 0,0, 0,0 ] } ]
          }
        }
      }
    }
  }
})";

// 裁剪（clipping）夹具：4 个槽位
//   s0 = quad（裁剪**之前** ⇒ 不被裁）
//   s1 = clip（裁剪附件自己 ⇒ **不被裁**，参考实现一命中就 clipStart + continue）
//   s2 = mesh（作用域内）
//   s3 = tail（作用域**边界**槽位：end="s2" 时不该被裁，缺 end 时该被裁）
//
// %ENDFIELD% 由 BuildClip() 替换：
//   "\"end\": \"s2\", " → 作用域 (s1, s2]
//   ""                → 没有 end ⇒ 一路裁到最后一个槽位
// %CLIPVERTSFIELD% 由 BuildClip() 替换（默认 = 下面那份 rigid 多边形）。
//   weighted 那条用例换成 "vertexCount" 与扁平编码的 vertices —— 判据是
//   **数组长度 != vertexCount*2**（SkeletonJson.cpp:1218），没有 uvs 可比对。
// %ANIMFIELD% 由 BuildClipDeform() 替换成一条命中 default/s2/mesh 的 deform 时间轴
//   （用来验 kHasDeform 只由 deform 点亮，不由裁剪点亮）。
const char* kSkeletonClip = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "clip" },
    { "name": "s2", "bone": "hip", "attachment": "mesh" },
    { "name": "s3", "bone": "hip", "attachment": "tail" }
  ],
  "skins": {
    "default": {
      "s0": { "quad": { "x": 0, "y": 0, "width": 64, "height": 32, "rotation": 0 } },
      "s1": { "clip": { "type": "clipping", %ENDFIELD%%CLIPVERTSFIELD% } },
      "s2": { "mesh": {
        "type": "mesh",
        "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
        "triangles": [ 0,1,2, 0,2,3 ],
        "vertices": [ 1, 0,-10,-5,1,
                      2, 1, 20,10,0.6, 0,5,7,0.4,
                      1, 1, 3,4,1,
                      1, 0, 8,2,1 ]
      } },
      "s3": { "tail": { "x": 0, "y": 0, "width": 16, "height": 8, "rotation": 0, "path": "quad" } }
    }
  }%ANIMFIELD%
})";

// 嵌套裁剪夹具：**两条**裁剪叠在一起
//   s0 = quad  （裁剪之前）
//   s1 = clipA，end = "s4"（覆盖 s2..s4）
//   s2 = clipB，end = "s3"（**嵌套**在 clipA 的作用域里）
//   s3 = mesh
//   s4 = tail
// 判据是 **s4**：
//   正确行为（clipB 被忽略）⇒ s3、s4 都由 **clipA** 裁 ⇒ s4 是 kFallback
//   错误行为（clipB 生效并终结 clipA）⇒ clipB 在 s3 就收尾 ⇒ **s4 不被裁** ⇒ s4 是 kStatic
// ⇒ 少了"嵌套被忽略"这条规则，s4 的 kind 就会错，而且**没有任何报错**。
const char* kSkeletonClipNested = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" }, { "name": "hip", "parent": "root" } ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "clip" },
    { "name": "s2", "bone": "hip", "attachment": "clipb" },
    { "name": "s3", "bone": "hip", "attachment": "mesh" },
    { "name": "s4", "bone": "hip", "attachment": "tail" }
  ],
  "skins": {
    "default": {
      "s0": { "quad": { "x": 0, "y": 0, "width": 64, "height": 32, "rotation": 0 } },
      "s1": { "clip": { "type": "clipping", "end": "s4", "vertexCount": 4,
                        "vertices": [ 0,0, 10,0, 10,10, 0,10 ] } },
      "s2": { "clipb": { "type": "clipping", "end": "s3", "vertexCount": 4,
                         "vertices": [ 0,0, 20,0, 20,20, 0,20 ] } },
      "s3": { "mesh": {
        "type": "mesh",
        "uvs": [ 0,0, 1,0, 1,1, 0,1 ],
        "triangles": [ 0,1,2, 0,2,3 ],
        "vertices": [ 1, 0,-10,-5,1,
                      2, 1, 20,10,0.6, 0,5,7,0.4,
                      1, 1, 3,4,1,
                      1, 0, 8,2,1 ]
      } },
      "s4": { "tail": { "x": 0, "y": 0, "width": 16, "height": 8, "rotation": 0, "path": "quad" } }
    }
  }
})";

// 默认那份裁剪多边形：4 个顶点，rigid（数组长度 == vertexCount*2 ⇒ 判为 rigid）
const std::string kClipVertsRigid = "\"vertexCount\": 4, \"vertices\": [ 0,0, 10,0, 10,10, 0,10 ]";
// weighted 版：2 个顶点，各 1 个影响（bone 0 与 bone 1）。数组长度 10 != vertexCount*2 = 4
// ⇒ json 侧靠这个长度差判成 weighted（SkeletonJson.cpp:1218）。
const std::string kClipVertsWeighted =
    "\"vertexCount\": 2, \"vertices\": [ 1, 0, 0,0, 1,  1, 1, 10,10, 1 ]";

// 把 kSkeletonClip 的三个占位符填掉。end_field = "\"end\": \"s2\", " 或 ""（缺 end）；
// clip_verts 留空则用上面的 rigid 多边形（weighted 那条用例传自己的）。
std::string BuildClip(const std::string& end_field, bool with_deform,
                      const std::string& clip_verts = std::string())
{
    std::string s(kSkeletonClip);
    size_t at = s.find("%ENDFIELD%");
    if (at == std::string::npos)
    {
        return std::string();
    }
    s.replace(at, std::string("%ENDFIELD%").size(), end_field);
    at = s.find("%CLIPVERTSFIELD%");
    if (at == std::string::npos)
    {
        return std::string();
    }
    s.replace(at, std::string("%CLIPVERTSFIELD%").size(), clip_verts.empty() ? kClipVertsRigid : clip_verts);
    at = s.find("%ANIMFIELD%");
    if (at == std::string::npos)
    {
        return std::string();
    }
    // deform 时间轴命中 default/s2/mesh：与裁剪**同一个**绘制项，用来验
    // kHasDeform 只由 deform 点亮（裁剪不点亮它）。
    s.replace(at, std::string("%ANIMFIELD%").size(),
              with_deform ? R"(, "animations": { "walk": { "deform": { "default": { "s2": {
                "mesh": [ { "time": 0, "offset": 0,
                            "vertices": [ 0,0, 0,0, 0,0, 0,0, 0,0 ] } ] } } } } })" : "");
    return s;
}

// ---- 跨语言对拍的夹具落盘（SpineGpuImportTest --dump <dir>）----
// C# 读端（CSharp/）要对拍的是"同一副骨架经两种输入产出的**同一份字节**"，
// 光在 C++ 里自测不够 —— 两端各自对，等于没对。所以这里把夹具容器写成 .zspg 文件，
// 由 C# 端读回来再序列化一次，逐字节比对（规格 §12 第 2、7 项）。
//
// 为什么复用测试夹具而不是另写一份：夹具里那些"故意少一个字段 / 故意越界"的骨架
// 才是真正在用的边界，另写一份只会养出第二套、然后两边一起漂。
bool DumpContainers(const SpineAtlas& atlas, const std::string& dir)
{
    struct Case
    {
        const char* name;
        std::string json;                 // 空 ⇒ 只落 .skel
        std::vector<uint8_t> skel;
    };

    std::vector<Case> cases;
    cases.push_back({"baseline", kSkeleton, BuildSkelFixture(false)});
    cases.push_back({"linked_mesh", kSkeletonLinked, BuildSkelFixture(false, true)});
    cases.push_back({"two_skins", kSkeletonTwoSkins, BuildSkelFixture(false, false, "3.8.99", false, "mesh", true)});
    // kSkeletonDeform 带 %DEFORMFIELD% 占位符 ⇒ 只落 .skel（json 那条路径在套件里已经测过）
    cases.push_back({"deform", std::string(),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, true)});
    // 裁剪的三种 end：json 与 .skel 的表示**不对称**（名字 vs 下标），两种都要落。
    // ⚠️ "缺 end" 只有 json 能表达 —— .skel 的下标没有"空"这个值，越界是显式报错的。
    cases.push_back({"clip_end_s2", BuildClip("\"end\": \"s2\", ", false),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10, true, 2, 2)});
    cases.push_back({"clip_end_self", BuildClip("\"end\": \"s1\", ", false),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10, true, 1, 2)});
    cases.push_back({"clip_no_end", BuildClip("", false), std::vector<uint8_t>()});
    cases.push_back({"clip_nested", kSkeletonClipNested,
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10,
                                      true, 4, 3, true, 3)});
    cases.push_back({"clip_weighted", BuildClip("\"end\": \"s2\", ", false, kClipVertsWeighted),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10,
                                      true, 2, 2, false, 3, true)});
    // nonessential 开 / 关解析路径不同（字段数都不一样），C# 端也要各对一次
    cases.push_back({"baseline_nonessential", std::string(), BuildSkelFixture(false, false, "3.8.99", true)});
    cases.push_back({"clip_nested_nonessential", std::string(),
                     BuildSkelFixture(false, false, "3.8.99", true, "mesh", false, false, true, 10,
                                      true, 4, 3, true, 3)});

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        std::printf("  dump: 建目录 '%s' 失败：%s\n", dir.c_str(), ec.message().c_str());
        return false;
    }

    int written = 0;
    auto emit = [&](const std::string& file, const SpineGpuAsset& a) {
        std::vector<uint8_t> bytes;
        SpineImportResult rw = WriteSpineGpuContainer(a, bytes);
        if (!rw.ok)
        {
            std::printf("  dump: %s 序列化失败：%s\n", file.c_str(), rw.error.c_str());
            return false;
        }
        const std::filesystem::path p = std::filesystem::path(dir) / file;
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            std::printf("  dump: 写 %s 失败\n", p.string().c_str());
            return false;
        }
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ++written;
        return true;
    };

    bool ok = true;
    for (Case& c : cases)
    {
        if (!c.json.empty())
        {
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(c.json.data(), c.json.size(), &atlas, s);
            if (!r.ok)
            {
                std::printf("  dump: %s json 解析失败：%s\n", c.name, r.error.c_str());
                ok = false;
                continue;
            }
            SpineGpuAsset a;
            SpineImportResult rb = BakeSpineGpuAsset(s, a);
            if (!rb.ok)
            {
                std::printf("  dump: %s bake 失败：%s\n", c.name, rb.error.c_str());
                ok = false;
                continue;
            }
            ok = emit(std::string(c.name) + ".json.zspg", a) && ok;
        }
        if (!c.skel.empty())
        {
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(c.skel.data(), c.skel.size(), &atlas, s);
            if (!r.ok)
            {
                std::printf("  dump: %s skel 解析失败：%s\n", c.name, r.error.c_str());
                ok = false;
                continue;
            }
            SpineGpuAsset a;
            SpineImportResult rb = BakeSpineGpuAsset(s, a);
            if (!rb.ok)
            {
                std::printf("  dump: %s skel bake 失败：%s\n", c.name, rb.error.c_str());
                ok = false;
                continue;
            }
            ok = emit(std::string(c.name) + ".skel.zspg", a) && ok;
        }
    }

    std::printf("=== dump: %d 个容器 → %s ===\n", written, dir.c_str());
    return ok;
}

}   // namespace

int main(int argc, char** argv)
{
    const bool dump_mode = (argc >= 3 && std::strcmp(argv[1], "--dump") == 0);
    if (!dump_mode)
    {
        std::printf("=== GPU-Spine importer test ===\n");
    }

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

    // 只有图集就够落盘 ⇒ 夹具容器在这里就能出，不必先跑完整个套件
    if (dump_mode)
    {
        return DumpContainers(atlas, argv[2]) ? 0 : 1;
    }

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
    CheckEq(static_cast<long long>(src.skins.size()), 1, "json: 1 个皮肤");
    Check(src.skins.size() == 1 && src.skins[0] == "default",
          "json: 皮肤名取自**成员名**（3.8 的 skins 是对象，不是数组）");
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
    CheckEq(asset.header.skin_count, 1, "header.skin_count");
    CheckEq(asset.header.draw_item_count, 2, "header.draw_item_count");
    CheckEq(static_cast<long long>(asset.skin_names.size()), 1, "bake: 皮肤名表 1 项");
    Check(asset.skin_names.size() == 1 && asset.skin_names[0] == "default", "bake: 皮肤 0 名为 default");
    CheckEq(asset.draw_items.size() >= 1 ? asset.draw_items[0].skin_id : 999u, 0u, "bake: 绘制项的 skin_id = 0");
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
    // UV 是**独立通道**：条目数 = 各附件顶点数之和（这里 4 rigid + 4 weighted = 8 条 = 16 float）
    CheckEq(static_cast<long long>(asset.uv_buffer.size()), 16LL, "UV 通道 = 8 条 × 2 float = 16 个 float");
    Check(asset.draw_items.size() == 2 && asset.draw_items[0].uv_offset == 0u, "绘制项 0 的 uv_offset = 0");
    Check(asset.draw_items.size() == 2 && asset.draw_items[1].uv_offset == 4u, "绘制项 1 的 uv_offset = 4");

    // ---- 4. 影响流：每个顶点的 unorm8 权重和必须恰好 255 ----
    // v7 起蒙皮数据**不在顶点里**：顶点只有 (influence_offset, influence_count)，
    // 真正的 bindPos / 骨骼 / 权重在 influence_bind（float2）与 influence_bone 里。
    CheckEq(static_cast<long long>(asset.influence_bone.size()),
            static_cast<long long>(asset.influence_bind.size()) / 2LL, "影响流两条 chunk 条目数相同");
    // 夹具的 mesh 是 1,2,1,1 个影响 ⇒ 共 5 条
    CheckEq(static_cast<long long>(asset.influence_bone.size()), 5LL, "影响流 5 条（1+2+1+1）");
    {
        const uint8_t* wbase = asset.vertex_buffer.data() + asset.weighted_segment_offset;
        bool saw_nonzero_offset = false;
        for (uint32_t v = 0; v < asset.weighted_vertex_count; ++v)
        {
            SpineWeightedVertex wv {};
            std::memcpy(&wv, wbase + static_cast<size_t>(v) * SpineGpuConstants::kWeightedVertexStride,
                        sizeof(wv));
            if (wv.influence_offset != 0u)
            {
                saw_nonzero_offset = true;
            }
            Check(wv.influence_count >= 1u && wv.influence_offset + wv.influence_count <=
                                                  static_cast<uint32_t>(asset.influence_bone.size()),
                  "影响区间落在影响流里");
            int sum = 0;
            for (uint32_t k = 0; k < wv.influence_count; ++k)
            {
                sum += asset.influence_bone[wv.influence_offset + k].weight;
            }
            CheckEq(sum, 255, "权重和为 255");
        }
        // ⚠️ 必须有一条**偏移非 0**的影响：全是 0 时"写错成恒 0"测不出来
        // （与裁剪多边形 vertex_offset 那条断言同一个道理）。
        Check(saw_nonzero_offset, "有权重顶点的 influence_offset 非 0（否则测不出偏移写错）");
    }
    // 第 2 个顶点是 0.6 / 0.4 → 153 / 102，绑到 bone 1 与 bone 0
    {
        const uint8_t* wbase2 = asset.vertex_buffer.data() + asset.weighted_segment_offset;
        SpineWeightedVertex wv {};
        std::memcpy(&wv, wbase2 + SpineGpuConstants::kWeightedVertexStride, sizeof(wv));
        CheckEq(wv.influence_count, 2u, "第 2 个顶点有 2 个影响");
        CheckEq(wv.influence_offset, 1u, "第 2 个顶点的影响从第 1 条开始（前一个顶点占了 1 条）");
        CheckEq(asset.influence_bone[wv.influence_offset + 0].weight, 153, "0.6 → 153");
        CheckEq(asset.influence_bone[wv.influence_offset + 1].weight, 102, "0.4 → 102");
        CheckEq(asset.influence_bone[wv.influence_offset + 0].bone_idx, 1, "第 0 影响的骨骼下标");
        CheckEq(asset.influence_bone[wv.influence_offset + 1].bone_idx, 0, "第 1 影响的骨骼下标");
        // bindPos 也要对（v1 的两个影响分别是 (20,10) 与 (5,7)）
        CheckEq(static_cast<int>(asset.influence_bind[(wv.influence_offset + 0) * 2u + 0u]), 20,
                "影响 0 的 bindPos.x");
        CheckEq(static_cast<int>(asset.influence_bind[(wv.influence_offset + 0) * 2u + 1u]), 10,
                "影响 0 的 bindPos.y");
        CheckEq(static_cast<int>(asset.influence_bind[(wv.influence_offset + 1) * 2u + 0u]), 5,
                "影响 1 的 bindPos.x");
        CheckEq(static_cast<int>(asset.influence_bind[(wv.influence_offset + 1) * 2u + 1u]), 7,
                "影响 1 的 bindPos.y");
    }

    // ---- 5a. 影响数 5：v7 起**必须能烘焙**（定长 4 槽的时代它会被拒）----
    {
        const char* five = R"({
          "skeleton": { "spine": "3.8.99" },
          "bones": [ { "name": "root" }, { "name": "b1" }, { "name": "b2" },
                     { "name": "b3" }, { "name": "b4" } ],
          "slots": [ { "name": "s0", "bone": "root" } ],
          "skins": [ { "name": "default", "attachments": {
            "s0": { "mesh": {
              "type": "mesh",
              "uvs": [ 0,0 ],
              "triangles": [],
              "vertices": [ 5, 0,1,2,0.2, 1,3,4,0.2, 2,5,6,0.2, 3,7,8,0.2, 4,9,10,0.2 ]
            } }
          } } ]
        })";
        SpineSourceSkeleton f_src;
        SpineImportResult r1 = ParseSpineSkeletonJson(five, std::string(five).size(), &atlas, f_src);
        Check(r1.ok, "5 影响的 json 能解析");
        SpineGpuAsset f_asset;
        SpineImportResult r2 = BakeSpineGpuAsset(f_src, f_asset);
        Check(r2.ok, "影响数 5 ⇒ bake 成功（v7 起逐顶点变长，不再是定长 4 槽）");
        if (!r2.ok)
        {
            std::printf("  error: %s\n", r2.error.c_str());
        }
        CheckEq(static_cast<long long>(f_asset.influence_bone.size()), 5LL, "5 个影响 ⇒ 影响流 5 条");
        CheckEq(f_asset.weighted_vertex_count, 1u, "1 个顶点");
        if (f_asset.weighted_vertex_count == 1u)
        {
            SpineWeightedVertex wv {};
            std::memcpy(&wv, f_asset.vertex_buffer.data() + f_asset.weighted_segment_offset, sizeof(wv));
            CheckEq(wv.influence_count, 5u, "influence_count = 5（不是被截断成 4）");
        }
    }

    // ---- 5. 报错路径：影响数**坏掉**（300 > 硬上限 255）----
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
              "vertices": [ 300, 0,1,2,0.2 ]
            } }
          } } ]
        })";
        SpineSourceSkeleton bad_src;
        SpineImportResult r1 = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, bad_src);
        Check(r1.ok, "坏影响数的 json 能解析（烘焙时才报错）");
        SpineGpuAsset bad_asset;
        SpineImportResult r2 = BakeSpineGpuAsset(bad_src, bad_asset);
        Check(!r2.ok, "影响数 300 > MAX_INFLUENCES=255 ⇒ bake 必须失败");
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

    // 8 个 chunk：strings / slot_bone / vertex / uv / index / draw_items / attachments / atlas_pages
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
        CStrBytes(asset.bone_names) + CStrBytes(asset.skin_names) + CStrBytes(asset.slot_names) +
        CStrBytes(asset.attachment_names) + CStrBytes(asset.atlas_page_names) +      // id=1 strings
        static_cast<long long>(asset.slot_bone.size()) * 2LL +                       // id=2
        static_cast<long long>(asset.vertex_buffer.size()) +                         // id=3
        static_cast<long long>(asset.influence_bind.size()) * 4LL +                  // id=21
        static_cast<long long>(asset.influence_bone.size()) *
            static_cast<long long>(sizeof(SpineInfluenceBone)) +                    // id=22
        static_cast<long long>(asset.uv_buffer.size()) * 4LL +                       // id=4
        static_cast<long long>(asset.index_buffer.size()) * 2LL +                    // id=5
        static_cast<long long>(asset.draw_items.size()) *
            static_cast<long long>(sizeof(SpineDrawItem)) +                          // id=6（24 B/项）
        static_cast<long long>(asset.attachment_records.size()) *
            static_cast<long long>(sizeof(SpineGpuAttachmentRecord)) +               // id=7（8 B/项）
        CStrBytes(asset.atlas_page_names);                                           // id=8

    auto RdU32 = [&bytes](size_t off) -> uint32_t
    {
        return static_cast<uint32_t>(bytes[off]) | (static_cast<uint32_t>(bytes[off + 1]) << 8) |
               (static_cast<uint32_t>(bytes[off + 2]) << 16) | (static_cast<uint32_t>(bytes[off + 3]) << 24);
    };
    const uint32_t nchunk = RdU32(offsetof(SpineGpuHeader, chunk_count));
    CheckEq(static_cast<long long>(nchunk), 10,
            "字节流里 chunk_count = 10（夹具没有 clip_data；v7 多了影响流 21 / 22）");

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
        // ⚠️ 顺序**不是** id 升序：影响流（21 / 22）紧跟顶点块，9（裁剪）夹在 5 与 6 之间。
        // 这是历史顺序，不是设计 —— 但 C# 必须照抄，否则跨语言对拍会红。
        const uint32_t kExpectedIds[] = {1, 2, 3, 21, 22, 4, 5, 6, 7, 8};
        CheckEq(static_cast<long long>(id), static_cast<long long>(kExpectedIds[i]),
                "chunk 表: id 顺序 = 1,2,3,21,22,4,5,6,7,8");
        CheckEq(static_cast<long long>(off % SpineGpuConstants::kChunkAlignment), 0, "chunk 表: 起点 4 B 对齐");
        if (id == static_cast<uint32_t>(SpineGpuChunkId::kChunkVertexBuffer) ||
            id == static_cast<uint32_t>(SpineGpuChunkId::kChunkInfluenceBind) ||
            id == static_cast<uint32_t>(SpineGpuChunkId::kChunkUvBuffer))
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
    CheckEq(back.header.chunk_count, 10, "往返: chunk_count = 10（夹具没有 clip_data；v7 多了影响流 21 / 22）");
    CheckEq(back.header.bone_count, asset.header.bone_count, "往返: bone_count");
    CheckEq(back.header.slot_count, asset.header.slot_count, "往返: slot_count");
    CheckEq(back.header.draw_item_count, asset.header.draw_item_count, "往返: draw_item_count");
    CheckEq(back.header.skin_count, asset.header.skin_count, "往返: skin_count");
    CheckEq(back.header.vertex_count, asset.header.vertex_count, "往返: vertex_count");
    CheckEq(back.header.index_count, asset.header.index_count, "往返: index_count");
    CheckEq(back.header.atlas_page_count, asset.header.atlas_page_count, "往返: atlas_page_count");
    CheckEq(back.rigid_vertex_count, asset.rigid_vertex_count, "往返: rigid_vertex_count");
    CheckEq(back.weighted_vertex_count, asset.weighted_vertex_count, "往返: weighted_vertex_count");
    CheckEq(back.weighted_segment_offset, asset.weighted_segment_offset, "往返: weighted_segment_offset");

    Check(back.vertex_buffer == asset.vertex_buffer, "往返: 顶点缓冲逐字节一致");
    Check(back.influence_bind == asset.influence_bind, "往返: 影响流 bindPos 逐字节一致");
    Check(back.influence_bone == asset.influence_bone, "往返: 影响流骨骼/权重逐字节一致");
    Check(back.uv_buffer == asset.uv_buffer, "往返: UV 通道逐字节一致");
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
    Check(back.skin_names == asset.skin_names, "往返: 皮肤名");
    Check(back.slot_names == asset.slot_names, "往返: 槽位名");
    Check(back.attachment_names == asset.attachment_names, "往返: 附件名");
    Check(back.atlas_page_names == asset.atlas_page_names, "往返: 图集页名");
    CheckEq(static_cast<long long>(back.attachment_records.size()),
            static_cast<long long>(asset.attachment_records.size()), "往返: 附件记录数");
    bool records_same = back.attachment_records.size() == asset.attachment_records.size();
    for (size_t i = 0; records_same && i < asset.attachment_records.size(); ++i)
    {
        records_same = std::memcmp(&back.attachment_records[i], &asset.attachment_records[i],
                                   sizeof(SpineGpuAttachmentRecord)) == 0;
    }
    Check(records_same, "往返: 附件记录逐字节一致（含 skin 列）");
    // 记录里的 (skin, slot) 必须与绘制项对得上 —— 换装查的就是这张表
    if (back.attachment_records.size() == asset.draw_items.size())
    {
        for (size_t i = 0; i < asset.draw_items.size(); ++i)
        {
            CheckEq(back.attachment_records[i].skin_id, asset.draw_items[i].skin_id, "往返: 记录与绘制项 skin_id 一致");
            CheckEq(back.attachment_records[i].slot_index, asset.draw_items[i].slot_index,
                    "往返: 记录与绘制项 slot_index 一致");
        }
    }

    // 再写一遍：读回来的资产重新序列化，字节必须完全一致（否则 C# 端读不出同一个东西）
    {
        std::vector<uint8_t> again;
        SpineImportResult rw2 = WriteSpineGpuContainer(back, again);
        Check(rw2.ok, "再序列化成功");
        Check(rw2.ok && again == bytes, "读→写 逐字节一致（容器是稳定的，不含指针/地址）");
    }

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

    // ---- 9. 二进制 .skel ----
    // 夹具是手写字节流，内容与 json 夹具**等价** ⇒ 两条独立的解析路径必须产出
    // 逐字节相同的烘焙结果。二进制的字段顺序 / 大端 / varint / weighted 扁平编码
    // 只要错一处，这里就会分叉。
    // nonessential 开 / 关各跑一遍：Spine 编辑器**默认带** nonessential 数据，
    // 而它一开，头部 / 骨骼 / mesh 的字段数全变了。只跑一种等于漏掉最常见的情况。
    auto RunSkelAgainstJson = [&](bool nonessential)
    {
        g_prefix = std::string("[nonessential=") + (nonessential ? "true" : "false") + "] ";
        const std::vector<uint8_t> skel = BuildSkelFixture(false, false, "3.8.99", nonessential);
        SpineSourceSkeleton skel_src;
        SpineImportResult rsk = ParseSpineSkeletonBinary(skel.data(), skel.size(), &atlas, skel_src);
        Check(rsk.ok, "skel 解析成功");
        if (!rsk.ok)
        {
            std::printf("  error: %s\n", rsk.error.c_str());
            g_prefix.clear();
            return;
        }
        CheckEq(static_cast<long long>(skel_src.bones.size()), static_cast<long long>(src.bones.size()),
                "skel: 骨骼数与 json 一致");
        Check(skel_src.bones.size() == 2 && skel_src.bones[1].parent == 0, "skel: hip 的父是 root（父下标 < 自己）");
        CheckEq(static_cast<long long>(skel_src.slots.size()), static_cast<long long>(src.slots.size()),
                "skel: 槽位数与 json 一致");
        CheckEq(static_cast<long long>(skel_src.attachments.size()),
                static_cast<long long>(src.attachments.size()), "skel: 附件数与 json 一致");
        CheckEq(static_cast<long long>(skel_src.skins.size()), static_cast<long long>(src.skins.size()),
                "skel: 皮肤数与 json 一致");
        if (skel_src.skins.size() == src.skins.size())
        {
            for (size_t i = 0; i < src.skins.size(); ++i)
            {
                Check(skel_src.skins[i] == src.skins[i], "skel: 皮肤名与 json 一致");
            }
        }

        for (size_t i = 0; i < src.attachments.size() && i < skel_src.attachments.size(); ++i)
        {
            const SpineSourceAttachment& a = src.attachments[i];
            const SpineSourceAttachment& k = skel_src.attachments[i];
            Check(k.name == a.name, "skel: 附件名与 json 一致");
            Check(k.slot_index == a.slot_index, "skel: 附件槽位与 json 一致");
            Check(k.IsWeighted() == a.IsWeighted(), "skel: rigid/weighted 判定与 json 一致");
            Check(k.vertices == a.vertices, "skel: 顶点扁平数组与 json **逐值相同**");
            Check(k.uvs == a.uvs, "skel: UV 与 json 逐值相同");
            Check(k.triangles == a.triangles, "skel: 三角形与 json 一致");
        }

        Check(!skel_src.uses_tint_black, "skel: dark 分量全 0xff ⇒ **不**开 tint black");
        Check(!skel_src.has_clipping, "skel: 夹具没有裁剪");
        Check(!skel_src.has_draw_order_timeline, "skel: 没有动画 ⇒ 不置绘制顺序标志");

        SpineGpuAsset skel_asset;
        SpineImportResult rsk2 = BakeSpineGpuAsset(skel_src, skel_asset);
        Check(rsk2.ok, "skel: bake 成功");
        if (!rsk2.ok)
        {
            std::printf("  error: %s\n", rsk2.error.c_str());
        }
        else
        {
            Check(skel_asset.vertex_buffer == asset.vertex_buffer,
                  "skel↔json：烘焙后的顶点缓冲**逐字节相同**（两条路径的唯一交叉验证）");
            Check(skel_asset.rigid_vertex_count == asset.rigid_vertex_count, "skel↔json: rigid 顶点数");
            Check(skel_asset.weighted_vertex_count == asset.weighted_vertex_count, "skel↔json: weighted 顶点数");
            Check(skel_asset.index_buffer == asset.index_buffer, "skel↔json: 索引缓冲相同");
            Check(skel_asset.influence_bind == asset.influence_bind, "skel↔json: 影响流 bindPos 逐字节相同");
            Check(skel_asset.influence_bone == asset.influence_bone, "skel↔json: 影响流骨骼/权重逐字节相同");
            Check(skel_asset.uv_buffer == asset.uv_buffer, "skel↔json: UV 通道逐字节相同");
            Check(skel_asset.slot_bone == asset.slot_bone, "skel↔json: slot_bone 相同");
            Check(skel_asset.bone_names == asset.bone_names, "skel↔json: 骨骼名相同");
            Check(skel_asset.skin_names == asset.skin_names, "skel↔json: 皮肤名相同");
            Check(skel_asset.slot_names == asset.slot_names, "skel↔json: 槽位名相同");
            CheckEq(static_cast<long long>(skel_asset.draw_items.size()),
                    static_cast<long long>(asset.draw_items.size()), "skel↔json: 绘制项数相同");
            if (skel_asset.draw_items.size() == asset.draw_items.size())
            {
                for (size_t i = 0; i < asset.draw_items.size(); ++i)
                {
                    Check(std::memcmp(&skel_asset.draw_items[i], &asset.draw_items[i], sizeof(SpineDrawItem)) == 0,
                          "skel↔json: 绘制项逐字节相同");
                }
            }
        }
        g_prefix.clear();
    };
    RunSkelAgainstJson(false);
    RunSkelAgainstJson(true);

    // 9b. 绘制顺序时间轴 ⇒ 整个资产走 CPU（规格 §7）
    {
        const std::vector<uint8_t> skel_do = BuildSkelFixture(true);
        SpineSourceSkeleton s_do;
        SpineImportResult r_do = ParseSpineSkeletonBinary(skel_do.data(), skel_do.size(), &atlas, s_do);
        Check(r_do.ok, "skel: 带绘制顺序动画的夹具能解析");
        Check(r_do.ok && s_do.has_draw_order_timeline,
              "skel: 有绘制顺序时间轴 ⇒ 置 kHasDrawOrderTimeline（为拿到这个标志必须把前面 5 类时间轴逐个跳过）");
    }

    // 9c. 报错路径
    {
        const std::vector<uint8_t> bad = BuildSkelFixture(false, false, "4.2.11");
        SpineSourceSkeleton s;
        SpineImportResult r = ParseSpineSkeletonBinary(bad.data(), bad.size(), &atlas, s);
        Check(!r.ok, "skel: 版本 4.2 ⇒ 报错（与 json 路径同一道门）");
        std::printf("  (预期报错) %s\n", r.error.c_str());
    }
    {
        const std::string as_text(kSkeleton);
        SpineSourceSkeleton s;
        SpineImportResult r =
            ParseSpineSkeletonBinary(reinterpret_cast<const uint8_t*>(as_text.data()), as_text.size(), &atlas, s);
        Check(!r.ok, "skel: 拿 json 当二进制传 ⇒ 报错（.skel 没有魔数，不挡会顺着 varint 读飞）");
        std::printf("  (预期报错) %s\n", r.error.c_str());
    }
    {
        // parent 指向不存在的附件 ⇒ 必须报错（参考实现在这里报 "Parent mesh not found"）
        const std::vector<uint8_t> bad = BuildSkelFixture(false, true, "3.8.99", false, "no-such-attachment");
        SpineSourceSkeleton s;
        SpineImportResult r = ParseSpineSkeletonBinary(bad.data(), bad.size(), &atlas, s);
        Check(!r.ok, "skel: linked mesh 的 parent 不存在 ⇒ 报错");
        std::printf("  (预期报错) %s\n", r.error.c_str());
    }
    // 9d. 越界：任何真前缀都必须**失败**，绝不能越界读。
    //     .skel 没有魔数也没有长度字段，截断是最容易触发越界读的场景。
    for (bool nonessential : {false, true})
    {
        g_prefix = std::string("[nonessential=") + (nonessential ? "true" : "false") + "] ";
        const std::vector<uint8_t> full = BuildSkelFixture(false, false, "3.8.99", nonessential);
        int wrong_ok = 0;
        for (size_t n = 0; n < full.size(); ++n)
        {
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(full.data(), n, &atlas, s);
            if (r.ok)
            {
                ++wrong_ok;
            }
        }
        CheckEq(wrong_ok, 0, "skel: 任意真前缀都必须解析失败（截断不能假装成功，更不能越界读）");
        g_prefix.clear();
    }

    // ---- 10. linked mesh（附属网格）----
    // 几何继承父附件，贴图区域用自己的 ⇒ 顶点/三角形/regionUV 与父相同，**UV 不同**。
    // json 与 .skel 两条路径都要能解，且互相一致。
    auto FindAtt = [](const std::vector<SpineSourceAttachment>& v, const std::string& n) -> const SpineSourceAttachment*
    {
        for (const SpineSourceAttachment& a : v)
        {
            if (a.name == n)
            {
                return &a;
            }
        }
        return nullptr;
    };

    for (bool nonessential : {false, true})
    {
        g_prefix = std::string("[linked/nonessential=") + (nonessential ? "true" : "false") + "] ";

        SpineSourceSkeleton lj;
        SpineImportResult rlj = ParseSpineSkeletonJson(kSkeletonLinked, std::string(kSkeletonLinked).size(), &atlas, lj);
        Check(rlj.ok, "linked: json 解析成功");
        if (!rlj.ok)
        {
            std::printf("  error: %s\n", rlj.error.c_str());
            g_prefix.clear();
            continue;
        }

        const std::vector<uint8_t> lk = BuildSkelFixture(false, true, "3.8.99", nonessential);
        SpineSourceSkeleton lk_src;
        SpineImportResult rlk = ParseSpineSkeletonBinary(lk.data(), lk.size(), &atlas, lk_src);
        Check(rlk.ok, "linked: skel 解析成功");
        if (!rlk.ok)
        {
            std::printf("  error: %s\n", rlk.error.c_str());
            g_prefix.clear();
            continue;
        }

        CheckEq(static_cast<long long>(lj.attachments.size()), 3, "linked: 3 个附件（region + mesh + linked mesh）");
        CheckEq(static_cast<long long>(lk_src.attachments.size()),
                static_cast<long long>(lj.attachments.size()), "linked: skel 附件数与 json 一致");

        const SpineSourceAttachment* parent = FindAtt(lj.attachments, "mesh");
        const SpineSourceAttachment* child = FindAtt(lj.attachments, "mesh-linked");
        Check(parent != nullptr && child != nullptr, "linked: 父附件与 linked mesh 都在");
        if (parent != nullptr && child != nullptr)
        {
            // setParentMesh（MeshAttachment.cpp:226-239）
            Check(child->vertices == parent->vertices, "linked: 顶点（含骨骼权重）与父附件相同");
            Check(child->region_uvs == parent->region_uvs, "linked: regionUV 与父附件相同");
            Check(child->triangles == parent->triangles, "linked: 三角形与父附件相同");
            Check(child->slot_index == parent->slot_index, "linked: 与父附件同槽位");
            Check(child->IsWeighted() == parent->IsWeighted(), "linked: rigid/weighted 判定与父附件相同");
            // updateUVs（:64-92）用**自己的** region ⇒ UV 必须与父附件不同
            CheckEq(static_cast<long long>(child->uvs.size()), static_cast<long long>(parent->uvs.size()),
                    "linked: UV 数量与父附件相同");
            Check(child->uvs != parent->uvs,
                  "linked: UV 与父附件**不同** —— 用**自己的**图集 region 算（'quad' vs 'mesh'）");
        }

        // json ↔ skel 交叉验证
        for (size_t i = 0; i < lj.attachments.size() && i < lk_src.attachments.size(); ++i)
        {
            Check(lk_src.attachments[i].name == lj.attachments[i].name, "linked: skel/json 附件名一致");
            Check(lk_src.attachments[i].vertices == lj.attachments[i].vertices, "linked: skel/json 顶点一致");
            Check(lk_src.attachments[i].uvs == lj.attachments[i].uvs, "linked: skel/json UV 一致");
            Check(lk_src.attachments[i].region_uvs == lj.attachments[i].region_uvs, "linked: skel/json regionUV 一致");
        }

        SpineGpuAsset aj;
        SpineImportResult rbj = BakeSpineGpuAsset(lj, aj);
        SpineGpuAsset ak;
        SpineImportResult rbk = BakeSpineGpuAsset(lk_src, ak);
        Check(rbj.ok && rbk.ok, "linked: 两条路径都 bake 成功");
        if (rbj.ok && rbk.ok)
        {
            Check(aj.vertex_buffer == ak.vertex_buffer, "linked: skel↔json 顶点缓冲逐字节相同");
            Check(aj.index_buffer == ak.index_buffer, "linked: skel↔json 索引相同");
            Check(aj.uv_buffer == ak.uv_buffer, "linked: skel↔json UV 通道逐字节相同");
            // ✅ UV 拆成独立通道之后，linked mesh **共用父附件的顶点**：
            //    几何（位置 + 骨骼 + 权重）与父附件逐字节相同 ⇒ 顶点池里命中同一段。
            CheckEq(static_cast<long long>(aj.weighted_vertex_count), 4,
                    "linked: weighted 顶点仍 4 ⇒ linked mesh **共用**了父附件的顶点（拆通道的收益）");
            CheckEq(static_cast<long long>(aj.rigid_vertex_count), 4, "linked: rigid 顶点仍 4（region）");
            CheckEq(static_cast<long long>(aj.index_buffer.size()), 18, "linked: 索引 3 × 6");
            CheckEq(static_cast<long long>(aj.draw_items.size()), 3,
                    "linked: 3 个绘制项（换装时由 CPU 挑可见的那几个）");
            // UV **不**共用：各 4 条，共 12 条
            CheckEq(static_cast<long long>(aj.uv_buffer.size()), 24,
                    "linked: UV 条目 12 × 2 float = 24 个 float（region 4 + 父 mesh 4 + linked 4）");
            if (aj.draw_items.size() == 3)
            {
                // 父 mesh 与 linked mesh 的顶点段**相同** ⇒ 索引值完全相同
                bool same_indices = true;
                for (uint32_t k = 0; k < 6u && k + 6u < aj.index_buffer.size(); ++k)
                {
                    // 绘制项 1（父 mesh）与 2（linked mesh）的 6 个索引逐值比较
                    const size_t base1 = aj.draw_items[1].index_offset;
                    const size_t base2 = aj.draw_items[2].index_offset;
                    if (aj.index_buffer[base1 + k] != aj.index_buffer[base2 + k])
                    {
                        same_indices = false;
                    }
                }
                Check(same_indices, "linked: 父 mesh 与 linked mesh 的索引逐值相同 ⇒ 确实共用顶点");
                // 但 UV **不同**：各自的 uv_offset 指向不同的 4 条
                CheckEq(aj.draw_items[2].uv_offset - aj.draw_items[1].uv_offset, 4u,
                        "linked: 两个绘制项的 uv_offset 相差 4 ⇒ UV 各一份");
                // 且与父附件的 UV 逐值不同（用**自己的**图集 region 算的）
                bool uv_differs = false;
                for (uint32_t k = 0; k < 8u; ++k)
                {
                    if (aj.uv_buffer[aj.draw_items[1].uv_offset * 2u + k] !=
                        aj.uv_buffer[aj.draw_items[2].uv_offset * 2u + k])
                    {
                        uv_differs = true;
                    }
                }
                Check(uv_differs, "linked: 共用顶点但 UV 不同（UV 走独立通道）");
            }
        }
        g_prefix.clear();
    }

    // ---- 11. skin 维度（容器 v2）----
    // 容器 v1 把所有 skin 合并成一份 ⇒ 同一个 (槽位, 附件名) 在不同皮肤下只剩一份，
    // 换装直接失效。v2 给附件表加了 skin 列、给绘制项加了 skin_id，这里验的是：
    //   两份同名附件**都**留在容器里，且能按 (skin, slot) 查回去。
    auto FindAttSkin = [](const std::vector<SpineSourceAttachment>& v, int skin,
                          const std::string& n) -> const SpineSourceAttachment*
    {
        for (const SpineSourceAttachment& a : v)
        {
            if (a.skin_index == skin && a.name == n)
            {
                return &a;
            }
        }
        return nullptr;
    };

    for (bool nonessential : {false, true})
    {
        g_prefix = std::string("[skin/nonessential=") + (nonessential ? "true" : "false") + "] ";

        // ---- json：两个皮肤，s0 下都有 "quad"，尺寸 64×32 vs 32×16 ----
        SpineSourceSkeleton tj;
        SpineImportResult rtj = ParseSpineSkeletonJson(kSkeletonTwoSkins, std::string(kSkeletonTwoSkins).size(),
                                                       &atlas, tj);
        Check(rtj.ok, "skin: json 解析成功");
        if (!rtj.ok)
        {
            std::printf("  error: %s\n", rtj.error.c_str());
            g_prefix.clear();
            continue;
        }
        CheckEq(static_cast<long long>(tj.skins.size()), 2, "skin: 2 个皮肤");
        Check(tj.skins.size() == 2 && tj.skins[0] == "default" && tj.skins[1] == "skin2",
              "skin: 皮肤 0 是 default、皮肤 1 是 skin2");
        CheckEq(static_cast<long long>(tj.attachments.size()), 3,
                "skin: 3 个附件 ⇒ 同名跨皮肤的附件**没有**被合并掉");

        const SpineSourceAttachment* q0 = FindAttSkin(tj.attachments, 0, "quad");
        const SpineSourceAttachment* q1 = FindAttSkin(tj.attachments, 1, "quad");
        Check(q0 != nullptr && q1 != nullptr, "skin: 两个皮肤里的 'quad' 都在");
        if (q0 != nullptr && q1 != nullptr)
        {
            Check(q0->vertices != q1->vertices, "skin: 两份 'quad' 几何不同（64×32 vs 32×16）");
            Check(q0->slot_index == q1->slot_index, "skin: 两份 'quad' 同槽位 ⇒ 只有 skin_id 能区分");
        }

        // ---- .skel：同样的内容，手写字节流 ----
        const std::vector<uint8_t> tk = BuildSkelFixture(false, false, "3.8.99", nonessential, "mesh", true);
        SpineSourceSkeleton tk_src;
        SpineImportResult rtk = ParseSpineSkeletonBinary(tk.data(), tk.size(), &atlas, tk_src);
        Check(rtk.ok, "skin: skel 解析成功");
        if (!rtk.ok)
        {
            std::printf("  error: %s\n", rtk.error.c_str());
            g_prefix.clear();
            continue;
        }
        CheckEq(static_cast<long long>(tk_src.skins.size()), 2, "skin: skel 也是 2 个皮肤");
        Check(tk_src.skins.size() == 2 && tk_src.skins[0] == "default" && tk_src.skins[1] == "skin2",
              "skin: skel 的皮肤顺序与 json 一致（default 先，具名皮肤后）");
        CheckEq(static_cast<long long>(tk_src.attachments.size()),
                static_cast<long long>(tj.attachments.size()), "skin: skel 附件数与 json 一致");
        for (size_t i = 0; i < tj.attachments.size() && i < tk_src.attachments.size(); ++i)
        {
            Check(tk_src.attachments[i].name == tj.attachments[i].name, "skin: skel/json 附件名一致");
            Check(tk_src.attachments[i].skin_index == tj.attachments[i].skin_index,
                  "skin: skel/json 附件所属皮肤一致");
            Check(tk_src.attachments[i].vertices == tj.attachments[i].vertices, "skin: skel/json 顶点一致");
            Check(tk_src.attachments[i].uvs == tj.attachments[i].uvs, "skin: skel/json UV 一致");
        }

        // ---- bake 两条路径 ----
        SpineGpuAsset aj;
        SpineImportResult rbj = BakeSpineGpuAsset(tj, aj);
        SpineGpuAsset ak;
        SpineImportResult rbk = BakeSpineGpuAsset(tk_src, ak);
        Check(rbj.ok && rbk.ok, "skin: 两条路径都 bake 成功");
        if (!rbj.ok || !rbk.ok)
        {
            if (!rbj.ok)
            {
                std::printf("  error(json): %s\n", rbj.error.c_str());
            }
            if (!rbk.ok)
            {
                std::printf("  error(skel): %s\n", rbk.error.c_str());
            }
            g_prefix.clear();
            continue;
        }
        CheckEq(aj.header.skin_count, 2, "skin: header.skin_count = 2");
        CheckEq(static_cast<long long>(aj.draw_items.size()), 3, "skin: 3 个绘制项");
        if (aj.draw_items.size() == 3)
        {
            // 附件顺序是 (皮肤, 槽位)：default.s0 / default.s1 / skin2.s0
            CheckEq(aj.draw_items[0].skin_id, 0u, "skin: 绘制项 0 属 default");
            CheckEq(aj.draw_items[1].skin_id, 0u, "skin: 绘制项 1 属 default");
            CheckEq(aj.draw_items[2].skin_id, 1u, "skin: 绘制项 2 属 skin2");
            CheckEq(aj.draw_items[0].slot_index, aj.draw_items[2].slot_index,
                    "skin: 绘制项 0 与 2 同槽位 ⇒ 换装就是换这两个");
        }
        CheckEq(static_cast<long long>(aj.rigid_vertex_count), 8,
                "skin: rigid 顶点 = default 的 quad 4 + skin2 的 quad 4");
        CheckEq(static_cast<long long>(aj.weighted_vertex_count), 4, "skin: weighted 顶点仍 4（只有一个 mesh）");
        Check(aj.vertex_buffer == ak.vertex_buffer, "skin: skel↔json 顶点缓冲逐字节相同");
        Check(aj.index_buffer == ak.index_buffer, "skin: skel↔json 索引相同");

        // ---- 容器往返：皮肤名与 (skin, slot) 都得能读回去 ----
        CheckEq(static_cast<long long>(aj.attachment_records.size()), 3, "skin: 3 条附件记录");
        std::vector<uint8_t> sbytes;
        SpineImportResult rsw = WriteSpineGpuContainer(aj, sbytes);
        Check(rsw.ok, "skin: 序列化成功");
        SpineGpuAsset sback;
        SpineImportResult rsr = ReadSpineGpuContainer(sbytes.data(), sbytes.size(), sback);
        Check(rsr.ok, "skin: 反序列化成功");
        if (rsr.ok)
        {
            CheckEq(sback.header.skin_count, 2, "skin: 往返 skin_count = 2");
            Check(sback.skin_names == aj.skin_names, "skin: 往返 皮肤名一致");
            CheckEq(static_cast<long long>(sback.attachment_records.size()), 3, "skin: 往返 记录数 3");
            // 换装真正要做的查询：给定 (skin, slot)，查出该画哪个附件
            int found_skin2 = -1;
            for (size_t i = 0; i < sback.attachment_records.size(); ++i)
            {
                if (sback.attachment_records[i].skin_id == 1u &&
                    sback.attachment_records[i].slot_index == 0u)
                {
                    found_skin2 = static_cast<int>(sback.attachment_records[i].attachment_id);
                }
            }
            CheckEq(found_skin2, 2, "skin: 按 (skin=1, slot=0) 查到附件 id 2");
            Check(sback.attachment_names.size() == 3 && sback.attachment_names[2] == "quad",
                  "skin: 附件 id 2 的名字是 quad（与 attachment_id 同序）");
        }
        g_prefix.clear();
    }

    // 11b. 跨皮肤 linked mesh：父附件由 "skin" 字段决定，不是"所在的皮肤"
    {
        auto BuildCrossSkin = [](const std::string& skin_field) -> std::string
        {
            std::string s(kSkeletonLinkedCrossSkin);
            const std::string key = "%SKINFIELD%";
            const size_t at = s.find(key);
            if (at == std::string::npos)
            {
                return std::string();   // 夹具被改坏了 ⇒ 下面所有解析都会失败
            }
            s.replace(at, key.size(), skin_field);
            return s;
        };

        struct Case
        {
            const char* label;
            std::string skin_field;
            int want_parent_skin;   // 期望父附件取自哪个皮肤
        };
        // 省略 skin 字段 ⇒ **default**（这正是最容易照错的一条）
        const Case cases[] = {
            {"省略 skin 字段", "", 0},
            {"skin=default", ", \"skin\": \"default\"", 0},
            {"skin=skin2", ", \"skin\": \"skin2\"", 1},
        };

        for (const Case& c : cases)
        {
            g_prefix = std::string("[cross-skin/") + c.label + "] ";
            const std::string json = BuildCrossSkin(c.skin_field);
            Check(!json.empty(), "cross-skin: 夹具替换成功");
            if (json.empty())
            {
                g_prefix.clear();
                continue;
            }

            SpineSourceSkeleton cs;
            SpineImportResult r = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, cs);
            Check(r.ok, "cross-skin: json 解析成功");
            if (!r.ok)
            {
                std::printf("  error: %s\n", r.error.c_str());
                g_prefix.clear();
                continue;
            }
            CheckEq(static_cast<long long>(cs.attachments.size()), 3,
                    "cross-skin: 3 个附件（default.eye / skin2.eye / skin2.eye-linked）");

            const SpineSourceAttachment* eye0 = FindAttSkin(cs.attachments, 0, "eye");
            const SpineSourceAttachment* eye1 = FindAttSkin(cs.attachments, 1, "eye");
            const SpineSourceAttachment* linked = FindAttSkin(cs.attachments, 1, "eye-linked");
            Check(eye0 != nullptr && eye1 != nullptr && linked != nullptr, "cross-skin: 三个附件都在");
            if (eye0 == nullptr || eye1 == nullptr || linked == nullptr)
            {
                g_prefix.clear();
                continue;
            }
            Check(eye0->vertices != eye1->vertices, "cross-skin: 两个皮肤的 'eye' 几何不同（判据本身成立）");
            CheckEq(linked->skin_index, 1, "cross-skin: eye-linked 归**它所在的** skin2");
            const SpineSourceAttachment& want = (c.want_parent_skin == 0) ? *eye0 : *eye1;
            const SpineSourceAttachment& other = (c.want_parent_skin == 0) ? *eye1 : *eye0;
            Check(linked->vertices == want.vertices,
                  "cross-skin: 顶点取自 skin 字段指定的那个皮肤的 'eye'");
            Check(linked->vertices != other.vertices,
                  "cross-skin: 顶点**不是**另一个皮肤的 'eye'（照错会静默变成那份）");
            Check(linked->region_uvs == want.region_uvs, "cross-skin: regionUV 也取自同一个父附件");

            SpineGpuAsset ca;
            SpineImportResult rcb = BakeSpineGpuAsset(cs, ca);
            Check(rcb.ok, "cross-skin: bake 成功");
            if (rcb.ok)
            {
                // 两个皮肤的 'eye' 几何不同 ⇒ 各 4 个顶点；linked mesh 与**它引用的那个**
                // 'eye' 共用顶点 ⇒ 总共 **8** 个而不是 12 个（拆通道的收益）。
                CheckEq(static_cast<long long>(ca.weighted_vertex_count), 8,
                        "cross-skin: weighted 顶点 = 8 ⇒ linked mesh 共用了父附件的顶点");
            }
            g_prefix.clear();
        }
    }

    // 11c. 报错路径：皮肤相关
    {
        // linked mesh 的 "skin" 指向不存在的皮肤 ⇒ 必须报错（参考实现报 "Skin not found"）
        const char* bad = R"({
          "skeleton": { "spine": "3.8.99" },
          "bones": [ { "name": "root" } ],
          "slots": [ { "name": "s1", "bone": "root" } ],
          "skins": { "default": {
            "s1": {
              "mesh": { "type": "mesh", "uvs": [0,0, 1,0, 1,1], "triangles": [0,1,2],
                        "vertices": [ 1, 0,0,0,1, 1,0,1,0,1, 1,0,2,0,1 ] },
              "linked": { "type": "linkedmesh", "path": "mesh", "parent": "mesh", "skin": "no-such-skin" }
            }
          } }
        })";
        SpineSourceSkeleton s;
        SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
        Check(!r.ok, "skin: linked mesh 引用不存在的皮肤 ⇒ 报错");
        std::printf("  (预期报错) %s\n", r.error.c_str());
    }
    {
        // default 皮肤不在首位 ⇒ bake 必须报错（悄悄挪位会让默认外观变成另一个皮肤）
        const char* bad = R"({
          "skeleton": { "spine": "3.8.99" },
          "bones": [ { "name": "root" } ],
          "slots": [ { "name": "s0", "bone": "root" } ],
          "skins": {
            "skin2": { "s0": { "quad": { "width": 32, "height": 16 } } },
            "default": { "s0": { "quad": { "width": 64, "height": 32 } } }
          }
        })";
        SpineSourceSkeleton s;
        SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
        Check(r.ok, "skin: default 不在首位时**解析**仍成功");
        if (r.ok)
        {
            SpineGpuAsset a;
            SpineImportResult rb_skin = BakeSpineGpuAsset(s, a);
            Check(!rb_skin.ok, "skin: default 不在首位 ⇒ bake 必须报错（不静默挪位）");
            std::printf("  (预期报错) %s\n", rb_skin.error.c_str());
        }
    }
    {
        // 数组形式的 skins（后面版本的写法）皮肤名取 "name" 字段 —— 参考实现读不了，
        // 我们顺手接住，但至少要保证名字拿对了。
        const char* arr = R"({
          "skeleton": { "spine": "3.8.99" },
          "bones": [ { "name": "root" } ],
          "slots": [ { "name": "s0", "bone": "root" } ],
          "skins": [ { "name": "default", "attachments": {
            "s0": { "quad": { "width": 64, "height": 32 } }
          } } ]
        })";
        SpineSourceSkeleton s;
        SpineImportResult r = ParseSpineSkeletonJson(arr, std::string(arr).size(), &atlas, s);
        Check(r.ok, "skin: 数组形式的 skins 也能解析（非 3.8 写法，兜住）");
        Check(r.ok && s.skins.size() == 1 && s.skins[0] == "default", "skin: 数组形式取 \"name\" 字段作皮肤名");
    }

    // ---- 12. deform（FFD）----
    // 顶点每帧都变 ⇒ 烘不进静态 VB（里面存的是**绑定姿势**）⇒ 被命中的绘制项走
    // CPU fallback（规格 §7.1）。**不是整个资产**回退 —— 只有被命中的那几项。
    {
        // 按名字找附件（deform 夹具里每个名字只出现一次）
        auto FindByName = [](const std::vector<SpineSourceAttachment>& v,
                             const std::string& n) -> const SpineSourceAttachment*
        {
            for (const SpineSourceAttachment& a : v)
            {
                if (a.name == n)
                {
                    return &a;
                }
            }
            return nullptr;
        };

        auto BuildDeform = [](bool inherit) -> std::string
        {
            std::string s(kSkeletonDeform);
            const std::string key = "%DEFORMFIELD%";
            const size_t at = s.find(key);
            if (at == std::string::npos)
            {
                return std::string();   // 夹具被改坏了 ⇒ 下面所有解析都会失败
            }
            s.replace(at, key.size(), inherit ? ", \"deform\": 1" : ", \"deform\": 0");
            return s;
        };

        // 基线：没有动画的同一副骨架 ⇒ 一个 fallback 都没有
        {
            SpineSourceSkeleton base;
            SpineImportResult rb = ParseSpineSkeletonJson(kSkeletonLinked, std::string(kSkeletonLinked).size(),
                                                          &atlas, base);
            Check(rb.ok, "deform: 基线骨架解析成功");
            if (rb.ok)
            {
                SpineGpuAsset ba;
                Check(BakeSpineGpuAsset(base, ba).ok, "deform: 基线 bake 成功");
                bool any_fallback = false;
                for (const SpineDrawItem& it : ba.draw_items)
                {
                    any_fallback = any_fallback || it.kind == SpineDrawItemKind::kFallback;
                }
                Check(!any_fallback, "deform: 没有 deform 时间轴 ⇒ 一个 fallback 都没有");
                Check((ba.header.flags & SpineGpuFlags::kHasDeform) == 0, "deform: 基线不置 kHasDeform");
            }
        }

        for (int inherit_i = 1; inherit_i >= 0; --inherit_i)
        {
            const bool inherit = inherit_i != 0;
            for (int ne = 0; ne < 2; ++ne)
            {
                const bool nonessential = ne != 0;
                g_prefix = std::string("[deform/继承=") + (inherit ? "1" : "0") +
                           (nonessential ? "/nonessential" : "") + "] ";

                const std::string json = BuildDeform(inherit);
                Check(!json.empty(), "deform: 夹具替换成功");
                if (json.empty())
                {
                    g_prefix.clear();
                    continue;
                }

                SpineSourceSkeleton dj;
                SpineImportResult rj = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, dj);
                Check(rj.ok, "deform: json 解析成功");
                if (!rj.ok)
                {
                    std::printf("  error: %s\n", rj.error.c_str());
                    g_prefix.clear();
                    continue;
                }

                const SpineSourceAttachment* mesh = FindByName(dj.attachments, "mesh");
                const SpineSourceAttachment* linked = FindByName(dj.attachments, "mesh-linked");
                const SpineSourceAttachment* quad = FindByName(dj.attachments, "quad");
                Check(mesh != nullptr && linked != nullptr && quad != nullptr, "deform: 三个附件都在");
                if (mesh == nullptr || linked == nullptr || quad == nullptr)
                {
                    g_prefix.clear();
                    continue;
                }
                Check(mesh->deformed, "deform: 被时间轴命中的 'mesh' 标上了 deformed");
                Check(!quad->deformed, "deform: 同一个骨架里的 region 'quad' **没有**被标");
                Check(linked->deformed == inherit,
                      inherit ? "deform: inheritDeform=1 ⇒ linked mesh 跟着回退"
                              : "deform: inheritDeform=0 ⇒ linked mesh 不跟着回退");

                SpineGpuAsset da;
                SpineImportResult rdb = BakeSpineGpuAsset(dj, da);
                Check(rdb.ok, "deform: bake 成功");
                if (!rdb.ok)
                {
                    std::printf("  error: %s\n", rdb.error.c_str());
                    g_prefix.clear();
                    continue;
                }

                Check(da.draw_items.size() == 3, "deform: 3 个绘制项");
                if (da.draw_items.size() == 3)
                {
                    Check(da.draw_items[0].kind == SpineDrawItemKind::kStatic,
                          "deform: 绘制项 0（quad）仍是 kStatic");
                    Check(da.draw_items[1].kind == SpineDrawItemKind::kFallback,
                          "deform: 绘制项 1（mesh）是 kFallback");
                    Check(da.draw_items[2].kind ==
                              (inherit ? SpineDrawItemKind::kFallback : SpineDrawItemKind::kStatic),
                          inherit ? "deform: 绘制项 2（mesh-linked）也是 kFallback"
                                  : "deform: 绘制项 2（mesh-linked）仍是 kStatic");
                }
                Check((da.header.flags & SpineGpuFlags::kHasDeform) != 0, "deform: 置上了 kHasDeform");

                // ---- 同一副骨架走 .skel：必须产出**逐字节相同**的容器 ----
                const std::vector<uint8_t> skel = BuildSkelFixture(false, true, "3.8.99", nonessential, "mesh",
                                                                   false, true, inherit);
                SpineSourceSkeleton ds;
                SpineImportResult rs = ParseSpineSkeletonBinary(skel.data(), skel.size(), &atlas, ds);
                Check(rs.ok, "deform: skel 解析成功");
                if (!rs.ok)
                {
                    std::printf("  error: %s\n", rs.error.c_str());
                    g_prefix.clear();
                    continue;
                }
                const SpineSourceAttachment* smesh = FindByName(ds.attachments, "mesh");
                const SpineSourceAttachment* slinked = FindByName(ds.attachments, "mesh-linked");
                Check(smesh != nullptr && slinked != nullptr, "deform: skel 里两个 mesh 都在");
                if (smesh != nullptr && slinked != nullptr)
                {
                    Check(smesh->deformed, "deform: skel 的 'mesh' 也被标上 deformed");
                    Check(slinked->deformed == inherit, "deform: skel 的 inheritDeform 与 json 一致");
                }

                SpineGpuAsset sa;
                Check(BakeSpineGpuAsset(ds, sa).ok, "deform: skel bake 成功");
                // SpineDrawItem 没有 operator==（也不该有：逐字节比较才是要验的）
                Check(sa.draw_items.size() == da.draw_items.size() &&
                          std::memcmp(sa.draw_items.data(), da.draw_items.data(),
                                      sa.draw_items.size() * sizeof(SpineDrawItem)) == 0,
                      "deform: skel↔json 绘制项逐字节相同（含 kind）");
                Check(sa.header.flags == da.header.flags, "deform: skel↔json flags 相同");

                std::vector<uint8_t> bytes_j;
                std::vector<uint8_t> bytes_s;
                Check(WriteSpineGpuContainer(da, bytes_j).ok && WriteSpineGpuContainer(sa, bytes_s).ok,
                      "deform: 两个容器都序列化成功");
                Check(bytes_j == bytes_s, "deform: skel↔json 容器**逐字节相同**");

                g_prefix.clear();
            }
        }
    }

    // ---- 12b. deform 的报错路径 ----
    {
        // ① 皮肤名写错：参考实现 findSkin 返回 NULL 后紧接着 skin->getAttachment ⇒ 空指针
        {
            const char* bad = R"({
              "skeleton": { "spine": "3.8.99" },
              "bones": [ { "name": "root" } ],
              "slots": [ { "name": "s1", "bone": "root" } ],
              "skins": { "default": {
                "s1": { "mesh": { "type": "mesh", "uvs": [0,0, 1,0, 1,1], "triangles": [0,1,2],
                                  "vertices": [ 1, 0,0,0,1, 1,0,1,0,1, 1,0,2,0,1 ] } }
              } },
              "animations": { "walk": { "deform": { "no-such-skin": { "s1": {
                "mesh": [ { "time": 0 } ] } } } } }
            })";
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
            Check(!r.ok, "deform: 引用不存在的皮肤 ⇒ 报错");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
        // ①b **反面**：deform 指向一个**没有绘制项**的附件（path）⇒ **不是错误**，跳过。
        //     本工程实测 88 处指向 path（`K_Lvbu` 槽位 121 等）；当错误会让 50 个资产
        //     整个导不出来，而理由是"某个看不见的东西被动画了"。
        //     ⚠️ 关键副作用：它**不能点亮 kHasDeform** —— 没有绘制项要回退，
        //        点亮它等于让运行时为一个根本不存在的东西每帧准备动态 VB。
        {
            const std::vector<uint8_t> skel =
                BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, true, true, 10,
                                 false, 2, 0, false, 3, false, true, "path1");
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(skel.data(), skel.size(), &atlas, s);
            Check(r.ok, "deform→path: 解析**成功**（不报错）");
            if (!r.ok)
            {
                std::printf("  error: %s\n", r.error.c_str());
            }
            else
            {
                Check(!s.no_draw_attachments.empty(), "deform→path: path 被记进 no_draw_attachments");
                bool any_deformed = false;
                for (const SpineSourceAttachment& a : s.attachments)
                {
                    if (a.deformed)
                    {
                        any_deformed = true;
                    }
                }
                Check(!any_deformed, "deform→path: 没有任何附件被标 deformed");
                SpineGpuAsset asset;
                Check(BakeSpineGpuAsset(s, asset).ok, "deform→path: bake 成功");
                Check((asset.header.flags & SpineGpuFlags::kHasDeform) == 0,
                      "deform→path: **不点亮** kHasDeform（没有绘制项可回退）");
            }
        }
        // ①c **3.7 与 3.8**：字节布局逐段比对一致，只有槽位暗色判据不同 ⇒
        //     同一个夹具只改版本串就必须照样能导，且 `spine_runtime` **写实** 30700。
        //     本工程 782 个资产全是 3.7.83 —— 不写实（伪装成 30800）会让读端
        //     按 3.8 的判据解释暗色，而那个差别**不报错**。
        {
            SpineSourceVersion v = SpineSourceVersion::kUnknown;
            uint32_t runtime = 0;
            std::string err;
            Check(ClassifySpineVersion("3.7.83", v, runtime, err), "版本: 3.7.83 被接受");
            Check(v == SpineSourceVersion::k37, "版本: 判成 k37");
            Check(runtime == SpineGpuConstants::kSpineRuntime37, "版本: runtime = 30700");
            Check(ClassifySpineVersion("3.8.99", v, runtime, err), "版本: 3.8.99 被接受");
            Check(v == SpineSourceVersion::k38 && runtime == SpineGpuConstants::kSpineRuntime38,
                  "版本: 3.8.99 判成 k38 / 30800");
            Check(!ClassifySpineVersion("4.0.0", v, runtime, err), "版本: 4.x 被拒绝");
            Check(!ClassifySpineVersion("", v, runtime, err), "版本: 空串被拒绝");

            const std::vector<uint8_t> skel37 = BuildSkelFixture(false, false, "3.7.83");
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(skel37.data(), skel37.size(), &atlas, s);
            Check(r.ok, "3.7: 解析成功");
            if (!r.ok)
            {
                std::printf("  error: %s\n", r.error.c_str());
            }
            else
            {
                SpineGpuAsset a;
                Check(BakeSpineGpuAsset(s, a).ok, "3.7: bake 成功");
                Check(a.header.spine_runtime == SpineGpuConstants::kSpineRuntime37,
                      "3.7: header.spine_runtime **写实** 30700（不伪装成 30800）");
            }
        }
        // ② 附件名写错：参考实现在这里**倒是**报错（"Attachment not found"）
        {
            const char* bad = R"({
              "skeleton": { "spine": "3.8.99" },
              "bones": [ { "name": "root" } ],
              "slots": [ { "name": "s1", "bone": "root" } ],
              "skins": { "default": {
                "s1": { "mesh": { "type": "mesh", "uvs": [0,0, 1,0, 1,1], "triangles": [0,1,2],
                                  "vertices": [ 1, 0,0,0,1, 1,0,1,0,1, 1,0,2,0,1 ] } }
              } },
              "animations": { "walk": { "deform": { "default": { "s1": {
                "no-such-mesh": [ { "time": 0 } ] } } } } }
            })";
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
            Check(!r.ok, "deform: 引用不存在的附件 ⇒ 报错");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
        // ③ deform 指向 **region**：3.8 的 RegionAttachment 不是 VertexAttachment
        //    ⇒ 参考实现是未定义的 static_cast，运行时是静默不生效。两边都不能要。
        {
            const char* bad = R"({
              "skeleton": { "spine": "3.8.99" },
              "bones": [ { "name": "root" } ],
              "slots": [ { "name": "s0", "bone": "root" } ],
              "skins": { "default": {
                "s0": { "quad": { "width": 64, "height": 32 } }
              } },
              "animations": { "walk": { "deform": { "default": { "s0": {
                "quad": [ { "time": 0 } ] } } } } }
            })";
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
            Check(!r.ok, "deform: 指向 region ⇒ 报错（参考实现是 UB / 运行时静默不生效）");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
        // ④ 帧的 offset + 顶点数 超过该附件的 deform 长度
        //    （weighted 是每**影响** 2 个 ⇒ 3 个影响只有 6 个 float，写 offset 4 + 4 个就爆了）
        {
            const char* bad = R"({
              "skeleton": { "spine": "3.8.99" },
              "bones": [ { "name": "root" } ],
              "slots": [ { "name": "s1", "bone": "root" } ],
              "skins": { "default": {
                "s1": { "mesh": { "type": "mesh", "uvs": [0,0, 1,0, 1,1], "triangles": [0,1,2],
                                  "vertices": [ 1, 0,0,0,1, 1,0,1,0,1, 1,0,2,0,1 ] } }
              } },
              "animations": { "walk": { "deform": { "default": { "s1": {
                "mesh": [ { "time": 0, "offset": 4, "vertices": [ 0,0,0,0 ] } ] } } } } }
            })";
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
            Check(!r.ok, "deform: 帧 offset+数量 越界 ⇒ 报错（参考实现是越界写）");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
        // ⑤ .skel 同一条：deform 帧长度写过头 ⇒ 也要报错（两条路径都不能越界）
        {
            const std::vector<uint8_t> bad = BuildSkelFixture(false, false, "3.8.99", false, "mesh", false,
                                                              true, true, 11);
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(bad.data(), bad.size(), &atlas, s);
            Check(!r.ok, "deform: skel 帧长度超过 deform 长度 ⇒ 报错");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
        // ⑥ .skel 的 deform 皮肤下标越界（参考实现是不检查的数组访问 ⇒ 越界读）
        {
            std::vector<uint8_t> bad = BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, true);
            // 把 deform 段里的皮肤下标 0 改成 9：定位方式与夹具写入顺序一致
            // （最后一个 "walk" 动画段里，deform 的第一个 varint 就是它）。
            bool patched = false;
            for (size_t i = 0; i + 5 < bad.size(); ++i)
            {
                if (bad[i] == 'w' && bad[i + 1] == 'a' && bad[i + 2] == 'l' && bad[i + 3] == 'k')
                {
                    // "walk" 后面紧跟 5 个 varint(0)（slot/bone/ik/transform/path），
                    // 再后面就是 deform 数（1）和皮肤下标（0）。
                    const size_t at = i + 4 + 5 + 1;
                    if (at < bad.size() && bad[at] == 0)
                    {
                        bad[at] = 9;
                        patched = true;
                    }
                    break;
                }
            }
            Check(patched, "deform: 找到要改的皮肤下标字节");
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(bad.data(), bad.size(), &atlas, s);
            Check(!r.ok, "deform: skel 皮肤下标越界 ⇒ 报错（参考实现是越界读）");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
    }

    // ---- 13. 裁剪作用域（规格 §7.2）----
    // 裁剪要在运行时用 CPU 重写顶点 / 三角形 / UV（SkeletonClipping.h:47-54），
    // 三角形个数都会变 ⇒ 静态 VB 里那份画不出来 ⇒ 作用域内的绘制项走 kFallback。
    // 关键是**作用域的边界**，参考实现有三条不对称规则，各由一条用例钉住：
    //   s0 = 裁剪之前      ⇒ 永不裁
    //   s1 = 裁剪附件自己  ⇒ 永不裁（clipStart 后立刻 continue）
    //   s2 = 作用域内
    //   s3 = 边界槽位      ⇒ end 指向它才裁
    {
        // 按槽位取绘制项的处置；-1 = 该槽位没有绘制项（裁剪附件就是这种情况）
        auto KindAtSlot = [](const SpineGpuAsset& a, uint16_t slot) -> int
        {
            for (const SpineDrawItem& it : a.draw_items)
            {
                if (it.slot_index == slot)
                {
                    return it.kind == SpineDrawItemKind::kFallback ? 1 : 0;
                }
            }
            return -1;
        };

        // 基线：没有裁剪附件的骨架
        {
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(kSkeleton, std::string(kSkeleton).size(), &atlas, s);
            Check(r.ok, "clip: 基线骨架解析成功");
            if (r.ok)
            {
                SpineGpuAsset a;
                Check(BakeSpineGpuAsset(s, a).ok, "clip: 基线 bake 成功");
                Check((a.header.flags & SpineGpuFlags::kHasClipping) == 0, "clip: 基线不置 kHasClipping");
                for (const SpineDrawItem& it : a.draw_items)
                {
                    Check(it.kind == SpineDrawItemKind::kStatic, "clip: 基线每个绘制项都是 kStatic");
                }
            }
        }

        // ---- 作用域矩阵：json × .skel × nonessential × 四种 end ----
        //   end="s2"     ⇒ (s1, s2]        ⇒ s2 裁、s3 不裁
        //   end="s1"     ⇒ 结束槽位 = 自己 ⇒ clipEnd(slot) 永不成立 ⇒ 裁到最后一个槽位
        //   end="s0"     ⇒ 结束槽位在前面 ⇒ 同上
        //   （缺 end      ⇒ json 专属：_endSlot 是 NULL ⇒ 裁到最后一个槽位）
        struct ClipCase
        {
            const char* name;
            const char* end_field;
            int clip_end_slot;   // .skel 用的下标；< 0 表示该用例没有 .skel 对应物
            int want_s2;
            int want_s3;
        };
        const ClipCase kCases[] = {
            {"end=s2", "\"end\": \"s2\", ", 2, 1, 0},
            {"end=s1（自己）", "\"end\": \"s1\", ", 1, 1, 1},
            {"end=s0（在前面）", "\"end\": \"s0\", ", 0, 1, 1},
        };

        for (const ClipCase& c : kCases)
        {
            for (int deform_i = 0; deform_i < 2; ++deform_i)
            {
                const bool with_deform = deform_i != 0;
                for (int ne = 0; ne < 2; ++ne)
                {
                    const bool nonessential = ne != 0;
                    g_prefix = std::string("[clip/") + c.name + (with_deform ? "/deform" : "") +
                               (nonessential ? "/nonessential" : "") + "] ";

                    const std::string json = BuildClip(c.end_field, with_deform);
                    Check(!json.empty(), "clip: 夹具替换成功");
                    if (json.empty())
                    {
                        g_prefix.clear();
                        continue;
                    }

                    SpineSourceSkeleton cj;
                    SpineImportResult rj = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, cj);
                    Check(rj.ok, "clip: json 解析成功");
                    if (!rj.ok)
                    {
                        std::printf("  error: %s\n", rj.error.c_str());
                        g_prefix.clear();
                        continue;
                    }
                    Check(cj.has_clipping, "clip: json 认出了裁剪附件");
                    CheckEq(static_cast<long long>(cj.clips.size()), 1, "clip: json 只解析出一条裁剪作用域");
                    Check(cj.clips.size() == 1 && cj.clips[0].slot_index == 1, "clip: 作用域起点槽位是 s1");

                    SpineGpuAsset ja;
                    SpineImportResult rjb = BakeSpineGpuAsset(cj, ja);
                    Check(rjb.ok, "clip: json bake 成功");
                    if (!rjb.ok)
                    {
                        std::printf("  error: %s\n", rjb.error.c_str());
                        g_prefix.clear();
                        continue;
                    }

                    // s1 是裁剪附件 ⇒ **没有**绘制项
                    CheckEq(static_cast<long long>(ja.draw_items.size()), 3, "clip: 3 个绘制项（裁剪附件不画）");
                    CheckEq(static_cast<long long>(KindAtSlot(ja, 1)), -1, "clip: s1（裁剪附件自己）没有绘制项");
                    CheckEq(static_cast<long long>(KindAtSlot(ja, 0)), 0, "clip: s0（裁剪之前）仍是 kStatic");
                    CheckEq(static_cast<long long>(KindAtSlot(ja, 2)), c.want_s2,
                            c.want_s2 ? "clip: s2 在作用域内 ⇒ kFallback" : "clip: s2 不在作用域内 ⇒ kStatic");
                    CheckEq(static_cast<long long>(KindAtSlot(ja, 3)), c.want_s3,
                            c.want_s3 ? "clip: s3 在作用域内 ⇒ kFallback" : "clip: s3 不在作用域内 ⇒ kStatic");
                    Check((ja.header.flags & SpineGpuFlags::kHasClipping) != 0, "clip: 置上了 kHasClipping");
                    // 裁剪**不**点亮 kHasDeform —— 那个位的语义是"有逐帧变动的顶点"，
                    // 运行时据此决定要不要每帧重算动态 VB，与裁剪不是一回事。
                    Check((ja.header.flags & SpineGpuFlags::kHasDeform) == (with_deform ? SpineGpuFlags::kHasDeform : 0u),
                          with_deform ? "clip: 有 deform ⇒ kHasDeform 置上"
                                      : "clip: 只有裁剪 ⇒ **不**置 kHasDeform");

                    // ---- 同一副骨架走 .skel：必须产出**逐字节相同**的容器 ----
                    const std::vector<uint8_t> skel =
                        BuildSkelFixture(false, false, "3.8.99", nonessential, "mesh", false, with_deform, true,
                                         10, true, c.clip_end_slot, 2);
                    SpineSourceSkeleton cs;
                    SpineImportResult rs = ParseSpineSkeletonBinary(skel.data(), skel.size(), &atlas, cs);
                    Check(rs.ok, "clip: skel 解析成功");
                    if (!rs.ok)
                    {
                        std::printf("  error: %s\n", rs.error.c_str());
                        g_prefix.clear();
                        continue;
                    }
                    Check(cs.clips.size() == 1 && cs.clips[0].slot_index == 1 &&
                              cs.clips[0].end_slot_index == c.clip_end_slot,
                          "clip: skel 解析出的 endSlot 与夹具一致");

                    SpineGpuAsset sa;
                    Check(BakeSpineGpuAsset(cs, sa).ok, "clip: skel bake 成功");
                    Check(sa.draw_items.size() == ja.draw_items.size() &&
                              std::memcmp(sa.draw_items.data(), ja.draw_items.data(),
                                          sa.draw_items.size() * sizeof(SpineDrawItem)) == 0,
                          "clip: skel↔json 绘制项逐字节相同（含 kind）");
                    Check(sa.header.flags == ja.header.flags, "clip: skel↔json flags 相同");

                    std::vector<uint8_t> bytes_j;
                    std::vector<uint8_t> bytes_s;
                    Check(WriteSpineGpuContainer(ja, bytes_j).ok && WriteSpineGpuContainer(sa, bytes_s).ok,
                          "clip: 两个容器都序列化成功");
                    Check(bytes_j == bytes_s, "clip: skel↔json 容器**逐字节相同**");

                    g_prefix.clear();
                }
            }
        }

        // ---- json 专属：缺 "end" ⇒ _endSlot 是 NULL ⇒ 裁到最后一个槽位 ----
        // 参考实现拿到 NULL 也不会报错（SkeletonJson.cpp:631-635 不检查 findSlot 的
        // 返回值），只是作用域悄悄变成"到末尾"。这里照样不报错，但**必须**把它和
        // "end 指向 s3" 区分开 —— 所以 s3 要真的被裁。
        {
            g_prefix = "[clip/缺 end] ";
            const std::string json = BuildClip("", false);
            Check(!json.empty(), "clip: 缺 end 的夹具替换成功");
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, s);
            Check(r.ok, "clip: 缺 end 也能解析（参考实现也是 NULL，不是错误）");
            if (r.ok)
            {
                Check(s.clips.size() == 1 && s.clips[0].end_slot_index < 0,
                      "clip: 缺 end ⇒ end_slot_index 记为负数（表示裁到最后一个槽位）");
                SpineGpuAsset a;
                Check(BakeSpineGpuAsset(s, a).ok, "clip: 缺 end 时 bake 成功");
                int k2 = -1;
                int k3 = -1;
                for (const SpineDrawItem& it : a.draw_items)
                {
                    if (it.slot_index == 2)
                    {
                        k2 = it.kind == SpineDrawItemKind::kFallback ? 1 : 0;
                    }
                    if (it.slot_index == 3)
                    {
                        k3 = it.kind == SpineDrawItemKind::kFallback ? 1 : 0;
                    }
                }
                CheckEq(static_cast<long long>(k2), 1, "clip: 缺 end ⇒ s2 被裁");
                CheckEq(static_cast<long long>(k3), 1, "clip: 缺 end ⇒ 一路裁到最后一个槽位 s3");
            }
            g_prefix.clear();
        }
    }

    // ---- 13b. 裁剪的报错路径 ----
    {
        // ① json 的 "end" 槽位名不存在：参考实现 findSlot 返回 NULL 后
        //   **不检查**（SkeletonJson.cpp:631-635）⇒ 作用域从"到某槽"变成"到末尾"，
        //   画面差异极大且没有报错。这里必须报错。
        {
            const char* bad = R"({
              "skeleton": { "spine": "3.8.99" },
              "bones": [ { "name": "root" } ],
              "slots": [ { "name": "s0", "bone": "root" }, { "name": "s1", "bone": "root" } ],
              "skins": { "default": {
                "s0": { "quad": { "width": 64, "height": 32 } },
                "s1": { "clip": { "type": "clipping", "end": "no-such-slot", "vertexCount": 4,
                                  "vertices": [ 0,0, 10,0, 10,10, 0,10 ] } }
              } }
            })";
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
            Check(!r.ok, "clip: json 的 end 槽位名不存在 ⇒ 报错（参考实现在这里静默当成 NULL）");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
        // ② .skel 的 endSlot 下标越界：与参考实现同一条检查
        //   （SkeletonBinary.cpp:641-645 "parse clip attachment failed!"）
        {
            const std::vector<uint8_t> bad = BuildSkelFixture(false, false, "3.8.99", false, "mesh", false,
                                                              false, true, 10, true, 9, 2);
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(bad.data(), bad.size(), &atlas, s);
            Check(!r.ok, "clip: skel 的 endSlot 下标越界 ⇒ 报错");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
        // ③ .skel 的 endSlot 是负数
        //   （参考实现是 `int >= size_t` 的比较，负数会被转成巨大的无符号数 ⇒ 也报错，
        //    但那是**碰巧**对的；这里显式判 < 0，不依赖整型提升的方向。）
        {
            const std::vector<uint8_t> bad = BuildSkelFixture(false, false, "3.8.99", false, "mesh", false,
                                                              false, true, 10, true, -1, 2);
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(bad.data(), bad.size(), &atlas, s);
            Check(!r.ok, "clip: skel 的 endSlot 为负 ⇒ 报错");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
    }

    // ---- 13c. 嵌套裁剪：后一条裁剪**永远开不起来** ----
    // clipA(s1 → s4) 里套着 clipB(s2 → s3)。`clipStart()` 在已有裁剪时直接返回 0
    // （SkeletonClipping.cpp:49-51）⇒ clipB 开不起来，s3 / s4 都由 **clipA** 裁。
    // 少了这条规则会怎样：clipB 生效并在 s3 收尾 ⇒ **s4 不被裁** ⇒ s4 从 kFallback
    // 变成 kStatic，而且**没有任何报错**。所以 s4 的 kind 就是这条规则的判据 ——
    // 只断言"s3 被裁"是不够的，两种实现都会让它成立。
    {
        auto KindAt = [](const SpineGpuAsset& a, uint16_t slot) -> int
        {
            for (const SpineDrawItem& it : a.draw_items)
            {
                if (it.slot_index == slot)
                {
                    return it.kind == SpineDrawItemKind::kFallback ? 1 : 0;
                }
            }
            return -1;
        };

        for (int ne = 0; ne < 2; ++ne)
        {
            const bool nonessential = ne != 0;
            g_prefix = std::string("[clip/嵌套") + (nonessential ? "/nonessential" : "") + "] ";

            SpineSourceSkeleton cj;
            SpineImportResult rj = ParseSpineSkeletonJson(kSkeletonClipNested,
                                                          std::string(kSkeletonClipNested).size(), &atlas, cj);
            Check(rj.ok, "nested: json 解析成功");
            if (!rj.ok)
            {
                std::printf("  error: %s\n", rj.error.c_str());
                g_prefix.clear();
                continue;
            }
            CheckEq(static_cast<long long>(cj.clips.size()), 2, "nested: json 解析出两条裁剪作用域");
            // 两条都要照单记下来 —— clipB 的 end 更靠前，正是"一旦让它生效就会提前收尾"的那条
            Check(cj.clips.size() == 2 && cj.clips[0].slot_index == 1 && cj.clips[0].end_slot_index == 4,
                  "nested: clipA 记为 s1 → s4");
            Check(cj.clips.size() == 2 && cj.clips[1].slot_index == 2 && cj.clips[1].end_slot_index == 3,
                  "nested: clipB 记为 s2 → s3");

            SpineGpuAsset ja;
            SpineImportResult rjb = BakeSpineGpuAsset(cj, ja);
            Check(rjb.ok, "nested: json bake 成功");
            if (!rjb.ok)
            {
                std::printf("  error: %s\n", rjb.error.c_str());
                g_prefix.clear();
                continue;
            }
            // 两个裁剪附件各占掉一个槽位 ⇒ 只有 s0 / s3 / s4 有绘制项
            CheckEq(static_cast<long long>(ja.draw_items.size()), 3, "nested: 3 个绘制项");
            CheckEq(static_cast<long long>(KindAt(ja, 1)), -1, "nested: s1（clipA 自己）没有绘制项");
            CheckEq(static_cast<long long>(KindAt(ja, 2)), -1, "nested: s2（clipB 自己）没有绘制项");
            CheckEq(static_cast<long long>(KindAt(ja, 0)), 0, "nested: s0（裁剪之前）仍是 kStatic");
            CheckEq(static_cast<long long>(KindAt(ja, 3)), 1, "nested: s3 被裁 ⇒ kFallback");
            CheckEq(static_cast<long long>(KindAt(ja, 4)), 1,
                    "nested: s4 由 **clipA** 裁 ⇒ kFallback（clipB 被忽略的判据）");
            Check((ja.header.flags & SpineGpuFlags::kHasClipping) != 0, "nested: 置上了 kHasClipping");
            Check((ja.header.flags & SpineGpuFlags::kHasDeform) == 0, "nested: 不置 kHasDeform");

            // 两条多边形都要落盘，而且**第二条**的偏移非 0 —— 这是唯一能钉住
            // "vertex_offset 的单位是**顶点下标**而不是字节偏移"的地方：只有一条多边形时
            // 它落在段首，0 × stride 还是 0，两种解释逐字节相同，写错也测不出来。
            CheckEq(static_cast<long long>(ja.clip_polygons.size()), 2, "nested: 两条多边形都落盘");
            if (ja.clip_polygons.size() == 2)
            {
                const SpineGpuClipPolygon& pa = ja.clip_polygons[0];
                const SpineGpuClipPolygon& pb = ja.clip_polygons[1];
                CheckEq(static_cast<long long>(pa.slot_index), 1, "nested: 第 0 条是 s1 的 clipA");
                CheckEq(static_cast<long long>(pb.slot_index), 2, "nested: 第 1 条是 s2 的 clipB");
                CheckEq(static_cast<long long>(pa.vertex_offset), 0, "nested: clipA 落在 rigid 段开头");
                // 4 个顶点 ⇒ 第 1 条从**顶点下标 4** 开始（字节偏移会算成 32）
                CheckEq(static_cast<long long>(pb.vertex_offset), 4,
                        "nested: clipB 从顶点下标 4 开始（**不是**字节偏移 32）");
                const float kWantB[8] = {0.0f, 0.0f, 20.0f, 0.0f, 20.0f, 20.0f, 0.0f, 20.0f};
                bool same_b = true;
                for (int v = 0; v < 4; ++v)
                {
                    const uint8_t* pv = ja.vertex_buffer.data() +
                                        static_cast<size_t>(pb.vertex_offset + static_cast<uint32_t>(v)) *
                                            SpineGpuConstants::kRigidVertexStride;
                    float xy[2] = {0.0f, 0.0f};
                    std::memcpy(xy, pv, sizeof(xy));
                    same_b = same_b && xy[0] == kWantB[v * 2] && xy[1] == kWantB[v * 2 + 1];
                }
                Check(same_b, "nested: clipB 的 4 个点在顶点缓冲里逐点相同");
            }

            // ---- 同一副骨架走 .skel：必须产出**逐字节相同**的容器 ----
            const std::vector<uint8_t> skel =
                BuildSkelFixture(false, false, "3.8.99", nonessential, "mesh", false, false, true, 10,
                                 true, 4, 3, true, 3);
            SpineSourceSkeleton cs;
            SpineImportResult rs = ParseSpineSkeletonBinary(skel.data(), skel.size(), &atlas, cs);
            Check(rs.ok, "nested: skel 解析成功");
            if (!rs.ok)
            {
                std::printf("  error: %s\n", rs.error.c_str());
                g_prefix.clear();
                continue;
            }
            CheckEq(static_cast<long long>(cs.clips.size()), 2, "nested: skel 也解析出两条裁剪作用域");
            Check(cs.clips.size() == 2 && cs.clips[0].end_slot_index == 4 && cs.clips[1].end_slot_index == 3,
                  "nested: skel 的两条 endSlot 与 json 一致");

            SpineGpuAsset sa;
            Check(BakeSpineGpuAsset(cs, sa).ok, "nested: skel bake 成功");
            Check(sa.draw_items.size() == ja.draw_items.size() &&
                      std::memcmp(sa.draw_items.data(), ja.draw_items.data(),
                                  sa.draw_items.size() * sizeof(SpineDrawItem)) == 0,
                  "nested: skel↔json 绘制项逐字节相同（含 kind）");
            Check(sa.header.flags == ja.header.flags, "nested: skel↔json flags 相同");

            std::vector<uint8_t> bytes_j;
            std::vector<uint8_t> bytes_s;
            Check(WriteSpineGpuContainer(ja, bytes_j).ok && WriteSpineGpuContainer(sa, bytes_s).ok,
                  "nested: 两个容器都序列化成功");
            Check(bytes_j == bytes_s, "nested: skel↔json 容器**逐字节相同**");

            g_prefix.clear();
        }
    }

    // ---- 13d. 裁剪多边形本身要落盘（v6）----
    // 它与 deform 不同：deform 是**逐帧动画数据**（来自时间轴，不该烘焙），
    // 裁剪多边形是**静态资产几何**（局部坐标 + 骨骼绑定），和顶点同类 ——
    // 顶点烘焙了它不烘焙，容器就还是不自包含。
    {
        auto FindChunk = [](const std::vector<uint8_t>& bytes, uint32_t id) -> uint32_t
        {
            uint32_t chunk_count = 0;
            std::memcpy(&chunk_count, bytes.data() + 0x2C, sizeof(chunk_count));
            for (uint32_t i = 0; i < chunk_count; ++i)
            {
                const uint8_t* e = bytes.data() + SpineGpuConstants::kHeaderSize +
                                   static_cast<size_t>(i) * SpineGpuConstants::kChunkTableEntrySize;
                uint32_t cid = 0;
                uint32_t csize = 0;
                std::memcpy(&cid, e, sizeof(cid));
                std::memcpy(&csize, e + 8, sizeof(csize));
                if (cid == id)
                {
                    return csize;
                }
            }
            return 0xFFFFFFFFu;   // 没有这个 chunk
        };

        // 基线：没有裁剪 ⇒ chunk 9 **根本不写**，且每个绘制项都是 kNoClipIndex
        {
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(kSkeleton, std::string(kSkeleton).size(), &atlas, s);
            SpineGpuAsset a;
            Check(r.ok && BakeSpineGpuAsset(s, a).ok, "clip-data: 基线解析 + bake 成功");
            Check(a.clip_polygons.empty(), "clip-data: 基线没有裁剪多边形");
            std::vector<uint8_t> bytes;
            Check(WriteSpineGpuContainer(a, bytes).ok, "clip-data: 基线序列化成功");
            CheckEq(static_cast<long long>(FindChunk(bytes, SpineGpuChunkId::kChunkClipData)), 0xFFFFFFFF,
                    "clip-data: 没有裁剪时 chunk 9 **不存在**（不是写个空块）");
            for (const SpineDrawItem& it : a.draw_items)
            {
                CheckEq(static_cast<long long>(it.clip_index),
                        static_cast<long long>(SpineGpuConstants::kNoClipIndex),
                        "clip-data: 基线每个绘制项的 clip_index 都是 kNoClipIndex");
            }
        }

        for (int ne = 0; ne < 2; ++ne)
        {
            const bool nonessential = ne != 0;
            g_prefix = std::string("[clip-data") + (nonessential ? "/nonessential" : "") + "] ";

            const std::string json = BuildClip("\"end\": \"s2\", ", false);
            Check(!json.empty(), "clip-data: 夹具替换成功");
            if (json.empty())
            {
                g_prefix.clear();
                continue;
            }
            SpineSourceSkeleton cj;
            SpineImportResult rj = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, cj);
            Check(rj.ok, "clip-data: json 解析成功");
            if (!rj.ok)
            {
                std::printf("  error: %s\n", rj.error.c_str());
                g_prefix.clear();
                continue;
            }
            SpineGpuAsset ja;
            Check(BakeSpineGpuAsset(cj, ja).ok, "clip-data: json bake 成功");

            CheckEq(static_cast<long long>(ja.clip_polygons.size()), 1, "clip-data: 一条裁剪多边形");
            if (ja.clip_polygons.size() == 1)
            {
                const SpineGpuClipPolygon& p = ja.clip_polygons[0];
                CheckEq(static_cast<long long>(p.slot_index), 1, "clip-data: 多边形记的是 s1");
                CheckEq(static_cast<long long>(p.skin_id), 0, "clip-data: 多边形记的是 default 皮肤");
                Check(p.vertex_format == SpineVertexFormat::kRigid,
                      "clip-data: 这份多边形的顶点是 rigid（数组长度 == vertexCount*2）");
                CheckEq(static_cast<long long>(p.vertex_count), 4, "clip-data: 4 个顶点");
                // 顶点**复用主顶点缓冲**（chunk 3）⇒ 偏移必须落在 rigid 段内
                Check(p.vertex_offset < ja.rigid_vertex_count, "clip-data: 顶点落在 rigid 段内（复用主顶点缓冲）");

                // 逐点比对：多边形那 4 个点必须真的在顶点缓冲里
                const float kWant[8] = {0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f, 0.0f, 10.0f};
                bool same = true;
                for (int v = 0; v < 4; ++v)
                {
                    const uint8_t* pv = ja.vertex_buffer.data() +
                                        static_cast<size_t>(p.vertex_offset + static_cast<uint32_t>(v)) *
                                            SpineGpuConstants::kRigidVertexStride;
                    float xy[2] = {0.0f, 0.0f};
                    std::memcpy(xy, pv, sizeof(xy));
                    same = same && xy[0] == kWant[v * 2] && xy[1] == kWant[v * 2 + 1];
                }
                Check(same, "clip-data: 顶点缓冲里那 4 个点与 json 里的多边形逐点相同");
            }

            // 被裁的项指到那条多边形上；**没被裁的必须是 kNoClipIndex，不能是 0** ——
            // 0 是一条真实存在的多边形，填 0 会让运行时拿它去裁根本不该裁的项，且不报错。
            for (const SpineDrawItem& it : ja.draw_items)
            {
                if (it.kind == SpineDrawItemKind::kFallback && it.slot_index == 2)
                {
                    CheckEq(static_cast<long long>(it.clip_index), 0, "clip-data: 被裁的项 clip_index = 0");
                }
                else
                {
                    CheckEq(static_cast<long long>(it.clip_index),
                            static_cast<long long>(SpineGpuConstants::kNoClipIndex),
                            "clip-data: **没**被裁的项 clip_index 是 kNoClipIndex（不是 0）");
                }
            }

            std::vector<uint8_t> bytes;
            Check(WriteSpineGpuContainer(ja, bytes).ok, "clip-data: 序列化成功");
            CheckEq(static_cast<long long>(FindChunk(bytes, SpineGpuChunkId::kChunkClipData)), 12,
                    "clip-data: chunk 9 = 12 B × 1 条");

            // 容器必须可重入：读回来再写一遍要逐字节相同（多边形与 clip_index 都得保住）
            SpineGpuAsset back;
            Check(ReadSpineGpuContainer(bytes.data(), bytes.size(), back).ok, "clip-data: 读回成功");
            CheckEq(static_cast<long long>(back.clip_polygons.size()), 1, "clip-data: 读回后多边形还在");
            std::vector<uint8_t> again;
            Check(WriteSpineGpuContainer(back, again).ok, "clip-data: 再序列化成功");
            Check(bytes == again, "clip-data: round-trip 逐字节相同（含裁剪多边形与 clip_index）");

            g_prefix.clear();
        }
    }

    // ---- 13e. weighted 裁剪多边形：两种输入的判据**不一样** ----
    // 二进制有**显式**的 weighted 标志（readVertices 先读一个 bool）；
    // json 只能靠 `vertices.size() != vertexCount*2` 猜（SkeletonJson.cpp:1218）。
    // 裁剪附件不贴图 ⇒ 没有 uvs 可比对 ⇒ 这个"猜"是唯一手段，两条路径必须殊途同归。
    {
        const std::string kClipVertsWeighted =
            "\"vertexCount\": 2, \"vertices\": [ 1, 0, 0,0, 1,  1, 1, 10,10, 1 ]";

        for (int ne = 0; ne < 2; ++ne)
        {
            const bool nonessential = ne != 0;
            g_prefix = std::string("[clip/weighted") + (nonessential ? "/nonessential" : "") + "] ";

            const std::string json = BuildClip("\"end\": \"s2\", ", false, kClipVertsWeighted);
            Check(!json.empty(), "clip-weighted: 夹具替换成功");
            if (json.empty())
            {
                g_prefix.clear();
                continue;
            }
            SpineSourceSkeleton cj;
            SpineImportResult rj = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, cj);
            Check(rj.ok, "clip-weighted: json 解析成功");
            if (!rj.ok)
            {
                std::printf("  error: %s\n", rj.error.c_str());
                g_prefix.clear();
                continue;
            }
            Check(cj.clips.size() == 1 && cj.clips[0].weighted,
                  "clip-weighted: json 判成 weighted（数组长度 10 != vertexCount*2 = 4）");
            Check(cj.clips.size() == 1 && cj.clips[0].vertex_count == 2,
                  "clip-weighted: json 记下 2 个顶点（**不是** 5 —— 扁平编码里 n 不算顶点）");

            SpineGpuAsset ja;
            Check(BakeSpineGpuAsset(cj, ja).ok, "clip-weighted: json bake 成功");
            CheckEq(static_cast<long long>(ja.clip_polygons.size()), 1, "clip-weighted: 一条多边形");
            if (ja.clip_polygons.size() == 1)
            {
                const SpineGpuClipPolygon& p = ja.clip_polygons[0];
                Check(p.vertex_format == SpineVertexFormat::kWeighted, "clip-weighted: 顶点格式是 weighted");
                CheckEq(static_cast<long long>(p.vertex_count), 2, "clip-weighted: 2 个顶点");
                Check(p.vertex_offset < ja.weighted_vertex_count,
                      "clip-weighted: 顶点落在 weighted 段内");
                // 两个顶点分别绑到 bone 0 与 bone 1 ⇒ 必须真的进 weighted 顶点块。
                // v7 起顶点只是引用，蒙皮数据要去影响流里取。
                const SpineWeightedVertex* wv = reinterpret_cast<const SpineWeightedVertex*>(
                    ja.vertex_buffer.data() + ja.weighted_segment_offset +
                    static_cast<size_t>(p.vertex_offset) * SpineGpuConstants::kWeightedVertexStride);
                Check(wv[0].influence_count == 1u && wv[1].influence_count == 1u,
                      "clip-weighted: 两个顶点各 1 个影响");
                Check(ja.influence_bone.size() >= 2 &&
                          ja.influence_bone[wv[0].influence_offset].bone_idx == 0 &&
                          ja.influence_bone[wv[1].influence_offset].bone_idx == 1,
                      "clip-weighted: 两个顶点分别绑到 bone 0 / bone 1");
                Check(ja.influence_bone.size() >= 2 &&
                          ja.influence_bone[wv[0].influence_offset].weight == 255 &&
                          ja.influence_bone[wv[1].influence_offset].weight == 255,
                      "clip-weighted: 权重 1.0 量化成 255");
            }

            // ---- .skel：二进制是**显式**标志，但结果必须一样 ----
            const std::vector<uint8_t> skel =
                BuildSkelFixture(false, false, "3.8.99", nonessential, "mesh", false, false, true, 10,
                                 true, 2, 2, false, 3, true);
            SpineSourceSkeleton cs;
            SpineImportResult rs = ParseSpineSkeletonBinary(skel.data(), skel.size(), &atlas, cs);
            Check(rs.ok, "clip-weighted: skel 解析成功");
            if (!rs.ok)
            {
                std::printf("  error: %s\n", rs.error.c_str());
                g_prefix.clear();
                continue;
            }
            Check(cs.clips.size() == 1 && cs.clips[0].weighted && cs.clips[0].vertex_count == 2,
                  "clip-weighted: skel 也判成 weighted（**显式**的 bool，不用猜）");

            SpineGpuAsset sa;
            Check(BakeSpineGpuAsset(cs, sa).ok, "clip-weighted: skel bake 成功");
            Check(sa.clip_polygons.size() == ja.clip_polygons.size() &&
                      std::memcmp(sa.clip_polygons.data(), ja.clip_polygons.data(),
                                  sa.clip_polygons.size() * sizeof(SpineGpuClipPolygon)) == 0,
                  "clip-weighted: skel↔json 多边形记录逐字节相同");

            std::vector<uint8_t> bytes_j;
            std::vector<uint8_t> bytes_s;
            Check(WriteSpineGpuContainer(ja, bytes_j).ok && WriteSpineGpuContainer(sa, bytes_s).ok,
                  "clip-weighted: 两个容器都序列化成功");
            Check(bytes_j == bytes_s, "clip-weighted: skel↔json 容器**逐字节相同**");

            g_prefix.clear();
        }

        // 报错：json 缺 "vertexCount" ⇒ 参考实现会把 rigid 多边形**误判成 weighted**
        // （verticesLength=0 != entrySize ⇒ 走扁平编码）⇒ 静默解析成垃圾骨骼索引
        {
            const char* bad = R"({
              "skeleton": { "spine": "3.8.99" },
              "bones": [ { "name": "root" } ],
              "slots": [ { "name": "s0", "bone": "root" }, { "name": "s1", "bone": "root" } ],
              "skins": { "default": {
                "s0": { "quad": { "width": 64, "height": 32 } },
                "s1": { "clip": { "type": "clipping", "end": "s1",
                                  "vertices": [ 0,0, 10,0, 10,10, 0,10 ] } }
              } }
            })";
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(bad, std::string(bad).size(), &atlas, s);
            Check(!r.ok, "clip-weighted: json 缺 vertexCount ⇒ 报错（参考实现会误判成 weighted）");
            std::printf("  (预期报错) %s\n", r.error.c_str());
        }
    }

    std::printf("=== verdict: %s (%d/%d checks) ===\n", g_failed == 0 ? "PASS" : "FAIL",
                g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
