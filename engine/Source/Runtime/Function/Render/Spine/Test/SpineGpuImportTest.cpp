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
                                      int deform_length = 10)
{
    SkelWriter w;

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
    w.Varint(2);
    w.Str("s0");
    w.Varint(0);            // bone = root
    w.ColorWhite();         // color
    w.ColorWhite();         // dark：全 0xff ⇒ **不**开 tint black
    w.Str("quad");
    w.Varint(0);            // blendMode = Normal
    w.Str("s1");
    w.Varint(1);            // bone = hip
    w.ColorWhite();
    w.ColorWhite();
    w.Str("mesh");
    w.Varint(0);

    // ---- 约束：全 0 ----
    w.Varint(0);            // IK
    w.Varint(0);            // transform
    w.Varint(0);            // path

    // ---- default skin ----
    w.Varint(2);            // skin 里涉及 2 个槽位

    // 槽位 0：region "quad"
    w.Varint(0);            // slotIndex
    w.Varint(1);            // 1 个附件
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

    // 槽位 1：mesh "mesh"（4 顶点，第 2 个顶点 2 影响 0.6/0.4）
    w.Varint(1);
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
            w.Varint(1);            // slotIndex = s1
            w.Varint(1);            // 1 个附件
            w.Str("mesh");
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
    CheckEq(static_cast<long long>(nchunk), 8, "字节流里 chunk_count = 8（夹具没有 clip_data）");

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
        CheckEq(static_cast<long long>(id), static_cast<long long>(i) + 1, "chunk 表: id 按 1..8 升序");
        CheckEq(static_cast<long long>(off % SpineGpuConstants::kChunkAlignment), 0, "chunk 表: 起点 4 B 对齐");
        if (id == static_cast<uint32_t>(SpineGpuChunkId::kChunkVertexBuffer) ||
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
    CheckEq(back.header.chunk_count, 8, "往返: chunk_count = 8（夹具没有 clip_data）");
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

    std::printf("=== verdict: %s (%d/%d checks) ===\n", g_failed == 0 ? "PASS" : "FAIL",
                g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
