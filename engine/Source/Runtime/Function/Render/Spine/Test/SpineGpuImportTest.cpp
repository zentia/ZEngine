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

#include "Runtime/Function/Render/Spine/SpineGpuAnimSolver.h"
#include "Runtime/Function/Render/Spine/SpineGpuBatch.h"
#include "SpineGpuEventFixture.h"
#include "SpineGpuSkinFixture.h"
#include "Runtime/Function/Render/Spine/SpineGpuAnimationState.h"
#include "Runtime/Function/Render/Spine/SpineGpuTintBlack.h"
#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{

int g_checks = 0;
int g_failed = 0;
// 同一批检查跑两遍时（如 .skel 的 nonessential 开/关）用来区分是哪一轮失败，
// 免得每条消息都要手写前缀。
std::string g_prefix;
bool g_verbose_v11 = (std::getenv("ZENGINE_SPINE_V11_DUMP") != nullptr);

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
// ---- 动画（v9 chunk 11-16）：与 BuildSkelFixture(with_animation=true) 逐字段等价 ----
// 覆盖 S2 的每一条边界：单帧 / 多帧 / linear / stepped / bezier / 负缩放 /
// 超 180° 旋转 / 空附件名（隐藏）/ 非 0 的 frame_first（靠第二个动画）。
//
// ⚠️ rotate 的字段名是 **"angle"**（SkeletonJson.cpp:895），写 "value"/"rotation"
//    会被 GetFloat 取默认 0 ⇒ 动不出来且不报错。
// ⚠️ curve **只能**写在非最后一帧上：参考实现把每帧的 curve 都写进只含
//    frameCount-1 段的数组里（Release 下是越界写）⇒ 夹具里最后一帧不写 curve。
// ⚠️ 夹具是**真 JSON**，里面不能写 `//` 注释（解析器不开 comment 开关）。
// ⚠️ 槽位 s1 带**非默认**的 color 与 dark —— chunk 17 的往返断言必须有一条非默认：
//    "全是默认色"的往返什么也挡不住（纯白读回来还是纯白，纯白与 setup 白看不出区别）。
//    dark 只写 6 位十六进制：json 里的 dark 只有 **rgb** 三个分量
//    （SkeletonJson.cpp:227-230），alpha 参考实现写死 1（:230）。
//
// ---- 约束（v11 chunk 18-20）：与 BuildSkelFixture(with_constraints=true) 逐字段等价 ----
// 三条约束的字段**全部**取非默认值，且 order 交错（transform=0, path=1, ik=2）：
//   * 默认值全对也能让 round-trip 通过 ⇒ 只有非默认才能抓到"字段错位"；
//   * order 不等于数组下标 ⇒ 能抓到"把 order 当下标"的写法（参考实现是按
//     i = 0..N-1 逐个找 order == i 的那条，找不到就**静默跳过**）；
//   * IK 挂 **2** 根骨骼 ⇒ 走到 `sortIkConstraint` 里 `constrained.size() > 1`
//     那条复位分支（单骨骼时 `_updateCacheReset` 恒空，测不出来）；
//   * path 的三个 mode 全取**非默认**（fixed / percent / chainScale）⇒
//     能抓到"mode 写反"或"缺 key 时默认值猜错"（json 缺 key 是 percent/length/
//     tangent，认不出的串却落在构造函数的 Fixed/Length/Tangent —— 两者不一致）。
const char* kSkeleton = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [
    { "name": "root", "x": 3, "y": -4, "rotation": 15, "scaleX": 2, "scaleY": 0.5,
      "shearX": 7, "shearY": -9, "length": 42 },
    { "name": "hip", "parent": "root", "x": 1.5, "y": 2.5, "rotation": -30,
      "scaleX": 1.25, "scaleY": 3, "shearX": -1.5, "shearY": 0.25, "length": 17,
      "transform": "noScaleOrReflection" }
  ],
  "slots": [
    { "name": "s0", "bone": "root", "attachment": "quad" },
    { "name": "s1", "bone": "hip", "attachment": "mesh", "color": "80c0ff40", "dark": "204060" }
  ],
  "ik": [
    { "name": "ik1", "order": 2, "bones": [ "root", "hip" ], "target": "hip",
      "mix": 0.75, "bendPositive": false, "compress": true, "stretch": false, "uniform": true }
  ],
  "transform": [
    { "name": "tf1", "order": 0, "bones": [ "hip" ], "target": "root",
      "local": false, "relative": true, "rotation": 11, "x": 2, "y": -3,
      "scaleX": 0.5, "scaleY": 1.5, "shearY": -7,
      "rotateMix": 0.25, "translateMix": 0.5, "scaleMix": 0.75, "shearMix": 0.125 }
  ],
  "path": [
    { "name": "path1", "order": 1, "bones": [ "hip" ], "target": "s0",
      "positionMode": "fixed", "spacingMode": "percent", "rotateMode": "chainScale",
      "rotation": 30, "position": 0.4, "spacing": 0.6,
      "rotateMix": 0.3, "translateMix": 0.7 }
  ],
  "skins": {
    "default": {
      "s0": { "quad": { "x": 0, "y": 0, "width": 64, "height": 32, "rotation": 0 },
              "path1": { "type": "path", "closed": false, "constantSpeed": false,
                         "vertexCount": 3, "vertices": [ 0,0, 10,0, 20,10 ], "lengths": [ 1 ] } },
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
  },
  "animations": {
    "walk": {
      "bones": {
        "root": {
          "rotate": [
            { "time": 0, "angle": 10, "curve": "stepped" },
            { "time": 0.5, "angle": 190, "curve": [ 0.25, 0.5, 0.75, 0.8 ] },
            { "time": 1, "angle": -20 }
          ],
          "translate": [
            { "time": 0, "x": 1, "y": 2 },
            { "time": 1, "x": -3, "y": 4 }
          ],
          "scale": [ { "time": 0, "x": -2, "y": 0.5 } ],
          "shear": [ { "time": 0.25, "x": 5, "y": -6 } ]
        },
        "hip": { "rotate": [ { "time": 0, "angle": 30 } ] }
      },
      "slots": {
        "s0": {
          "attachment": [ { "time": 0, "name": "quad" }, { "time": 0.5, "name": null } ],
          "color": [
            { "time": 0, "color": "ff0000ff" },
            { "time": 1, "color": "00ff0080" }
          ]
        },
        "s1": {
          "twoColor": [
            { "time": 0, "light": "ffffffff", "dark": "000000" },
            { "time": 0.5, "light": "808080ff", "dark": "102030" }
          ]
        }
      }
    },
    "idle": {
      "bones": { "root": { "rotate": [ { "time": 0, "angle": 7 } ] } }
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

    // 颜色在二进制里是一个 **int**（大端 rgba）：文件顺序就是 r,g,b,a 四个字节
    // （SkeletonBinary.cpp:748-752 按 `(color & 0xff000000) >> 24` 取 r ⇒ 第一个字节是 r）。
    void Color(uint32_t rgba)
    {
        U8(static_cast<uint8_t>((rgba >> 24) & 0xFFu));
        U8(static_cast<uint8_t>((rgba >> 16) & 0xFFu));
        U8(static_cast<uint8_t>((rgba >> 8) & 0xFFu));
        U8(static_cast<uint8_t>(rgba & 0xFFu));
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
                      const std::string& deform_name = "mesh", bool with_animation = false,
                      // v10：槽位 s1 带**非默认**的 color 与 dark（与 json 夹具 kSkeleton 对齐）。
                      // 默认 false —— 夹具的颜色会进 header.flags（kUsesTintBlack），
                      // 而裁剪 / deform 那几组用例要比"skel↔json flags 相同"，不能随手开。
                      bool slot_colors = false, bool with_constraints = false)
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
    // ⚠️ 数值必须与 json 夹具**逐字段相同**：两份字节流是两条独立解析路径的输入，
    // 最后要比"烘焙结果逐字节相同"。全取 2 的幂次小数 ⇒ f32 精确可表示，比较不受舍入影响。
    // 顺序是 **rotation, x, y, scaleX, scaleY, shearX, shearY, length**
    // （SkeletonBinary.cpp:157-164）—— 与 json 的书写顺序不同，写反了不报错。
    w.Str("root");
    w.F(15.0f); w.F(3.0f); w.F(-4.0f);      // rotation, x, y
    w.F(2.0f); w.F(0.5f);                   // scaleX, scaleY
    w.F(7.0f); w.F(-9.0f);                  // shearX, shearY
    w.F(42.0f);                             // length
    w.Varint(0);                            // transformMode = Normal
    if (nonessential)
    {
        w.ColorWhite();                     // bone color（int）
    }
    w.Str("hip");
    w.Varint(0);                            // parent = root（i != 0 才有）
    w.F(-30.0f); w.F(1.5f); w.F(2.5f);
    w.F(1.25f); w.F(3.0f);
    w.F(-1.5f); w.F(0.25f);
    w.F(17.0f);
    w.Varint(4);                            // transformMode = NoScaleOrReflection
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
    if (slot_colors)
    {
        // ⚠️ 与 json 夹具的 `"color": "80c0ff40"` 对齐（同样的 4 个字节 / 255.0f）。
        w.Color(0x80C0FF40u);
        // ⚠️ dark 的**第 4 个字节只用来判"有没有暗色"**（3.8：四分量不全为 0xff），
        //    从不作为 alpha 落地（参考实现写死 1，`SkeletonBinary.cpp:188`）。
        //    ⇒ 这里写 0xFF 是为了让 has_dark 成立；它的值不进容器。
        w.Color(0x204060FFu);
    }
    else
    {
        w.ColorWhite();
        w.ColorWhite();
    }
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

    // ---- 约束 setup（chunk 18，v11）----
    // with_constraints 时与 json 夹具 kSkeleton 的 ik / transform / path 三段
    // **逐字段等价**（含 order 交错 tf=0, path=1, ik=2），两边烘焙出的容器必须
    // 逐字节相同 —— 这是二进制侧字段顺序 / sbyte / varint 的最强检查。
    if (!with_constraints)
    {
        w.Varint(0);            // IK
        w.Varint(0);            // transform
        w.Varint(0);            // path
    }
    else
    {
        // ---- IK：name, order, bones[], target, mix, bendDirection(sbyte),
        //          compress, stretch, uniform（SkeletonBinary.cpp:192-208）----
        w.Varint(1);
        w.Str("ik1");
        w.Varint(2);            // order
        w.Varint(2);            // bones 条数
        w.Varint(0);            // root
        w.Varint(1);            // hip
        w.Varint(1);            // target = hip
        w.F(0.75f);             // mix
        w.U8(static_cast<uint8_t>(static_cast<int8_t>(-1)));   // bendDirection（**sbyte**）
        w.Bool(true);           // compress
        w.Bool(false);          // stretch
        w.Bool(true);           // uniform

        // ---- transform：name, order, bones[], target, local, relative,
        //          rotation, x, y, scaleX, scaleY, shearY,
        //          rotateMix, translateMix, scaleMix, shearMix（:217-237）----
        w.Varint(1);
        w.Str("tf1");
        w.Varint(0);            // order
        w.Varint(1);            // bones 条数
        w.Varint(1);            // hip
        w.Varint(0);            // target = root
        w.Bool(false);          // local
        w.Bool(true);           // relative
        w.F(11.0f);             // offsetRotation
        w.F(2.0f);              // offsetX
        w.F(-3.0f);             // offsetY
        w.F(0.5f);              // offsetScaleX
        w.F(1.5f);              // offsetScaleY
        w.F(-7.0f);             // offsetShearY
        w.F(0.25f);             // rotateMix
        w.F(0.5f);              // translateMix
        w.F(0.75f);             // scaleMix
        w.F(0.125f);            // shearMix

        // ---- path：name, order, bones[], target(**槽位**), positionMode,
        //          spacingMode, rotateMode, rotation, position, spacing,
        //          rotateMix, translateMix（:239-257）----
        w.Varint(1);
        w.Str("path1");
        w.Varint(1);            // order
        w.Varint(1);            // bones 条数
        w.Varint(1);            // hip
        w.Varint(0);            // target = **槽位** s0
        w.Varint(0);            // positionMode  = fixed
        w.Varint(2);            // spacingMode   = percent
        w.Varint(2);            // rotateMode    = chainScale
        w.F(30.0f);             // offsetRotation
        w.F(0.4f);              // position
        w.F(0.6f);              // spacing
        w.F(0.3f);              // rotateMix
        w.F(0.7f);              // translateMix
    }

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
        // 两帧：第一帧 offsetCount=0（⇒ 复位成 setup 序），第二帧把 s0 往后挪一位
        // （⇒ 两个槽位**对调**）。第二帧是 S3 的 drawOrder 重建算法唯一有区分力的输入：
        // 重建不是"把槽位挪到 index+offset"，未列出的槽位收进 `unchanged` 后要
        // **从数组末尾往前**填（SkeletonBinary.cpp:1063-1070）—— 只有 2 个槽位时
        // 恰好能把这个"从后往前"暴露出来。
        w.Varint(with_draw_order ? 2 : 0);
        if (with_draw_order)
        {
            w.F(0.0f);          // time
            w.Varint(0);        // offsetCount = 0 ⇒ 这一帧复位成 setup 序
            w.F(0.5f);          // time
            w.Varint(1);        // offsetCount = 1
            w.Varint(0);        // slotIndex = s0
            w.Varint(1);        // offset = +1（**有符号** varint，这里取正）
        }
        w.Varint(0);        // 事件时间轴
    }
    else if (with_animation)
    {
        // ---- 完整动画：与 json 夹具 kSkeleton 的 "animations" 段**逐字段等价** ----
        // 覆盖 S2 要求的每一条边界：单帧 / 多帧 / linear / stepped / bezier /
        // 负缩放 / 超 180° 旋转 / 空附件名（隐藏）/ 非 0 的 frame_first（第二个动画）。
        //
        // ⚠️ 顺序必须与 json 那边一致：json 是按**插入顺序**遍历对象成员的，
        //    二进制这里是手写顺序 ⇒ 两边错开一条，烘焙出的时间轴表就分叉。
        w.Varint(2);        // 2 个动画

        // ---- "walk" ----
        w.Str("walk");

        // 槽位时间轴（:725-793）
        w.Varint(2);        // 2 个槽位
        w.Varint(0);        // slot 0
        w.Varint(2);        // 2 条时间轴
        {
            w.U8(0);        // SLOT_ATTACHMENT
            w.Varint(2);    // 2 帧
            w.F(0.0f);
            w.Str("quad");
            w.F(0.5f);
            w.Str("");      // 空名 = 该帧隐藏这个槽位（json 那边是 "name": null）
        }
        {
            w.U8(1);        // SLOT_COLOR
            w.Varint(2);
            w.F(0.0f);
            w.Color(0xFF0000FFu);   // 不透明红
            w.U8(0);                // curve: linear
            w.F(1.0f);
            w.Color(0x00FF0080u);   // 半透明绿
            // 最后一帧**没有** curve
        }
        w.Varint(1);        // slot 1
        w.Varint(1);        // 1 条
        {
            w.U8(2);        // SLOT_TWO_COLOR
            w.Varint(2);
            w.F(0.0f);
            w.Color(0xFFFFFFFFu);   // light
            w.Color(0x00000000u);   // dark（只有 rgb 有效，最高字节不用）
            w.U8(0);                // linear
            w.F(0.5f);
            w.Color(0x808080FFu);
            w.Color(0x00102030u);
        }

        // 骨骼时间轴（:795-852）
        w.Varint(2);        // 2 根骨骼
        w.Varint(0);        // bone 0 = root
        w.Varint(4);        // 4 条
        {
            w.U8(0);        // BONE_ROTATE
            w.Varint(3);    // 3 帧 ⇒ 2 段
            w.F(0.0f);
            w.F(10.0f);
            w.U8(1);        // 段 0 = stepped
            w.F(0.5f);
            w.F(190.0f);    // > 180°：最短弧那条语义的边界
            w.U8(2);        // 段 1 = bezier
            w.F(0.25f);
            w.F(0.5f);
            w.F(0.75f);
            w.F(0.8f);
            w.F(1.0f);
            w.F(-20.0f);
        }
        {
            w.U8(1);        // BONE_TRANSLATE
            w.Varint(2);
            w.F(0.0f);
            w.F(1.0f);
            w.F(2.0f);
            w.U8(0);        // 段 0 = linear
            w.F(1.0f);
            w.F(-3.0f);
            w.F(4.0f);
        }
        {
            w.U8(2);        // BONE_SCALE：单帧（0 段曲线）
            w.Varint(1);
            w.F(0.0f);
            w.F(-2.0f);     // 负缩放
            w.F(0.5f);
        }
        {
            w.U8(3);        // BONE_SHEAR：单帧，且 time 非 0
            w.Varint(1);
            w.F(0.25f);
            w.F(5.0f);
            w.F(-6.0f);
        }
        w.Varint(1);        // bone 1 = hip
        w.Varint(1);
        {
            w.U8(0);
            w.Varint(1);
            w.F(0.0f);
            w.F(30.0f);
        }

        w.Varint(0);        // IK 约束时间轴
        w.Varint(0);        // transform 约束时间轴
        w.Varint(0);        // path 约束时间轴
        w.Varint(0);        // deform 时间轴
        w.Varint(0);        // 绘制顺序时间轴
        w.Varint(0);        // 事件时间轴

        // ---- "idle"：只有一条单帧 rotate ----
        // 存在的意义是让第二个动画的 timeline_first / frame_first / curve_first
        // 全部**非 0** —— 偏移 0 时"帧 vs float""条 vs 字节"两种解释逐字节相同，
        // 写错了也测不出来。
        w.Str("idle");
        w.Varint(0);        // 槽位时间轴
        w.Varint(1);        // 骨骼时间轴
        w.Varint(0);        // bone 0
        w.Varint(1);
        {
            w.U8(0);
            w.Varint(1);
            w.F(0.0f);
            w.F(7.0f);
        }
        w.Varint(0);
        w.Varint(0);
        w.Varint(0);
        w.Varint(0);
        w.Varint(0);
        w.Varint(0);
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
  "bones": [
    { "name": "root", "x": 3, "y": -4, "rotation": 15, "scaleX": 2, "scaleY": 0.5,
      "shearX": 7, "shearY": -9, "length": 42 },
    { "name": "hip", "parent": "root", "x": 1.5, "y": 2.5, "rotation": -30,
      "scaleX": 1.25, "scaleY": 3, "shearX": -1.5, "shearY": 0.25, "length": 17,
      "transform": "noScaleOrReflection" }
  ],
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
  "bones": [
    { "name": "root", "x": 3, "y": -4, "rotation": 15, "scaleX": 2, "scaleY": 0.5,
      "shearX": 7, "shearY": -9, "length": 42 },
    { "name": "hip", "parent": "root", "x": 1.5, "y": 2.5, "rotation": -30,
      "scaleX": 1.25, "scaleY": 3, "shearX": -1.5, "shearY": 0.25, "length": 17,
      "transform": "noScaleOrReflection" }
  ],
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
  "bones": [
    { "name": "root", "x": 3, "y": -4, "rotation": 15, "scaleX": 2, "scaleY": 0.5,
      "shearX": 7, "shearY": -9, "length": 42 },
    { "name": "hip", "parent": "root", "x": 1.5, "y": 2.5, "rotation": -30,
      "scaleX": 1.25, "scaleY": 3, "shearX": -1.5, "shearY": 0.25, "length": 17,
      "transform": "noScaleOrReflection" }
  ],
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

// v15：deform 指向**裁剪附件** s1/clip（rigid 4 顶点，offset 2 起 4 个 +1）。
std::string BuildClipDeformOnClip()
{
    std::string json = BuildClip("\"end\": \"s2\", ", false);
    const size_t last = json.rfind('}');
    if (json.empty() || last == std::string::npos)
    {
        return std::string();
    }
    json.insert(last, R"(, "animations": { "walk": { "deform": { "default": { "s1": {
                    "clip": [ { "time": 0, "offset": 2, "vertices": [ 1,1, 1,1 ] } ] } } } } })");
    return json;
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
    // baseline：json 带完整动画 ⇒ .skel 那一份也必须带（with_animation=true），
    // 否则两边烘焙出的容器会在 chunk 11-16 上分叉，而"少几个 chunk"不报错。
    // baseline：json 带完整动画 ⇒ .skel 那一份也必须带（with_animation=true），
    // 否则两边烘焙出的容器会在 chunk 11-16 上分叉，而"少几个 chunk"不报错。
    // v11 起同样要求 with_constraints / with_path 两边都开 —— chunk 18/19/20 也一样。
    cases.push_back({"baseline", kSkeleton,
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10, false,
                                      2, 1, false, 3, false, true, "mesh", true, true, true)});
    cases.push_back({"linked_mesh", kSkeletonLinked, BuildSkelFixture(false, true)});
    cases.push_back({"two_skins", kSkeletonTwoSkins, BuildSkelFixture(false, false, "3.8.99", false, "mesh", true)});
    // kSkeletonDeform 带 %DEFORMFIELD% 占位符 ⇒ 只落 .skel（json 那条路径在套件里已经测过）
    cases.push_back({"deform", std::string(),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, true)});
    // v12：deform 指向 **path 附件**（target 最高位）⇒ C# 读端必须认得最高位、按 chunk 20 校验，
    // 并原样写回同一份字节。path1 是 rigid 3 顶点 ⇒ deform 长度 6。
    cases.push_back({"deform_on_path", std::string(),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, true, true, 6,
                                      false, 2, 0, false, 3, false, true, "path1")});
    // 裁剪的三种 end：json 与 .skel 的表示**不对称**（名字 vs 下标），两种都要落。
    // ⚠️ "缺 end" 只有 json 能表达 —— .skel 的下标没有"空"这个值，越界是显式报错的。
    cases.push_back({"clip_end_s2", BuildClip("\"end\": \"s2\", ", false),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10, true, 2, 2)});
    cases.push_back({"clip_end_self", BuildClip("\"end\": \"s1\", ", false),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10, true, 1, 2)});
    cases.push_back({"clip_no_end", BuildClip("", false), std::vector<uint8_t>()});
    // v15：deform 指向裁剪附件（target 次高位）⇒ C# 读端要认得 clip 位并原样写回
    cases.push_back({"clip_deform", BuildClipDeformOnClip(), std::vector<uint8_t>()});
    cases.push_back({"clip_nested", kSkeletonClipNested,
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10,
                                      true, 4, 3, true, 3)});
    cases.push_back({"clip_weighted", BuildClip("\"end\": \"s2\", ", false, kClipVertsWeighted),
                     BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, false, true, 10,
                                      true, 2, 2, false, 3, true)});
    // v16：用户事件（chunk 23 + type 7 时间轴载荷）⇒ C# 读端要认得 chunk 23 并原样写回同一份字节
    cases.push_back({"events", SpineGpuEventFixture::kSkeleton, std::vector<uint8_t>()});
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

    // ---- 骨骼 setup（S1 / chunk 10 的源）----
    // 逐字段比，而不是只比"骨数对不对"：round-trip 只能抓"大小"漂，抓不到"解读"漂 ——
    // 把 x 与 y 的偏移对调，字节照样自洽，只有逐字段断言能抓到。
    // 参考默认值 SkeletonJson.cpp:165-172，⚠️ scaleX/scaleY 默认是 **1** 不是 0。
    if (src.bones.size() == 2)
    {
        const SpineSourceBone& r = src.bones[0];
        const SpineSourceBone& h = src.bones[1];
        Check(r.parent == -1, "json: root 是根（parent = -1）");
        Check(h.parent == 0, "json: hip 的父是 root（父下标 < 自己）");
        Check(r.x == 3.0f && r.y == -4.0f, "json: root 的 x/y");
        Check(r.rotation == 15.0f, "json: root 的 rotation（度，不是弧度）");
        Check(r.scale_x == 2.0f && r.scale_y == 0.5f, "json: root 的 scaleX/scaleY");
        Check(r.shear_x == 7.0f && r.shear_y == -9.0f, "json: root 的 shearX/shearY");
        Check(r.length == 42.0f, "json: root 的 length");
        Check(r.transform_mode == SpineTransformMode::kNormal, "json: root 的 transform 缺省 = normal");
        Check(h.x == 1.5f && h.y == 2.5f, "json: hip 的 x/y");
        Check(h.rotation == -30.0f, "json: hip 的 rotation");
        Check(h.scale_x == 1.25f && h.scale_y == 3.0f, "json: hip 的 scaleX/scaleY");
        Check(h.shear_x == -1.5f && h.shear_y == 0.25f, "json: hip 的 shearX/shearY");
        Check(h.length == 17.0f, "json: hip 的 length");
        Check(h.transform_mode == SpineTransformMode::kNoScaleOrReflection,
              "json: hip 的 transform = noScaleOrReflection（字段名是 \"transform\"，不是 transformMode）");
    }

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

    // ---- chunk 10：骨骼 setup 进了容器（36 B/骨）----
    CheckEq(static_cast<long long>(asset.bones.size()), 2, "bake: 骨骼 setup 2 条");
    CheckEq(static_cast<long long>(asset.bones.size()), static_cast<long long>(asset.header.bone_count),
            "bake: 骨骼 setup 条数 = header.bone_count（读端靠这个等式挡『缺 chunk 10』）");
    CheckEq(static_cast<long long>(asset.bones.size()), static_cast<long long>(asset.bone_names.size()),
            "bake: 骨骼 setup 条数 = 骨骼名数（同一个下标空间）");
    if (asset.bones.size() == 2)
    {
        const SpineGpuBone& r = asset.bones[0];
        const SpineGpuBone& h = asset.bones[1];
        Check(r.x == 3.0f && r.y == -4.0f && r.rotation == 15.0f, "chunk10: root 的 x/y/rotation");
        Check(r.scale_x == 2.0f && r.scale_y == 0.5f, "chunk10: root 的 scaleX/scaleY");
        Check(r.shear_x == 7.0f && r.shear_y == -9.0f, "chunk10: root 的 shearX/shearY");
        Check(r.length == 42.0f, "chunk10: root 的 length");
        Check(r.parent == -1, "chunk10: root 的 parent = -1");
        Check(r.transform_mode == static_cast<uint8_t>(SpineTransformMode::kNormal), "chunk10: root 的 transform_mode = 0");
        Check(h.parent == 0, "chunk10: hip 的 parent = 0（必须 **< 自己的下标**）");
        Check(h.transform_mode == static_cast<uint8_t>(SpineTransformMode::kNoScaleOrReflection),
              "chunk10: hip 的 transform_mode = 4");
        Check(h.x == 1.5f && h.y == 2.5f && h.rotation == -30.0f, "chunk10: hip 的 x/y/rotation");
        Check(h.scale_x == 1.25f && h.scale_y == 3.0f, "chunk10: hip 的 scaleX/scaleY");
        Check(h.shear_x == -1.5f && h.shear_y == 0.25f, "chunk10: hip 的 shearX/shearY");
        Check(h.length == 17.0f, "chunk10: hip 的 length");
        // ⚠️ 必须有**非 0** 的父下标样本：只有 -1 时"parent 写成了 0"之类也成立。
        Check(h.parent != -1, "chunk10: 夹具至少有一根非根骨骼（否则 parent 字段写错测不出来）");
        Check(r.pad == 0 && h.pad == 0, "chunk10: pad 落盘为 0（跨语言逐字节对拍的前提）");
    }

    // ---- 3c. 动画（chunk 11-16，v9）----
    // 夹具的 animations 段是**刻意**按"每条边界一条时间轴"排的：
    //   单帧（scale / shear / hip.rotate）· 多帧（rotate 3 帧）· linear（translate）·
    //   stepped（rotate 段 0）· bezier（rotate 段 1）· 负缩放（-2）· 超 180°（190）·
    //   空附件名（隐藏）· 非 0 的 frame_first（第二个动画 idle）
    {
        g_prefix = "[anim] ";
        CheckEq(static_cast<long long>(asset.anims.size()), 2LL, "anim: 2 个动画（walk / idle）");
        if (asset.anims.size() == 2)
        {
            CheckEq(asset.anims[0].timeline_first, 0u, "anim: walk 从时间轴表第 0 条开始");
            CheckEq(asset.anims[0].timeline_count, 8u, "anim: walk 有 8 条时间轴（5 骨骼 + 3 槽位）");
            // ⚠️ 第二个动画的三个偏移**全非 0**：偏移 0 时"帧 vs float"两种解释逐字节
            //    相同，写错单位测不出来。这条断言是全文件唯一能抓到它的地方。
            CheckEq(asset.anims[1].timeline_first, 8u, "anim: idle 的时间轴区间非 0（否则测不出下标写错）");
            CheckEq(asset.anims[1].timeline_count, 1u, "anim: idle 有 1 条时间轴");
            CheckEq(asset.anims[1].pad, 0u, "anim: pad 落盘为 0");
            Check(asset.anims[0].duration == 1.0f,
                  "anim: walk 的 duration = 1（参考实现取各时间轴最后一帧 time 的最大值）");
            Check(asset.anims[1].duration == 0.0f, "anim: idle 的 duration = 0（只有一帧 time=0）");
        }

        CheckEq(static_cast<long long>(asset.timelines.size()), 9LL, "anim: 时间轴表 9 条");
        if (asset.timelines.size() == 9)
        {
            // (type, target, frame_count) 序列 = 烘焙时的**规范化顺序**：
            // (type, target) 升序、稳定排序 —— 不跟着输入文件的书写顺序走。
            // walk 里 hip(1) 的 rotate 因此紧跟 root(0) 的 rotate，而不是落在 shear 后面。
            const uint16_t kWantType[] = {0, 0, 1, 2, 3, 4, 5, 14, 0};
            const uint16_t kWantTarget[] = {0, 1, 0, 0, 0, 0, 0, 1, 0};
            const uint32_t kWantFrames[] = {3, 1, 2, 1, 1, 2, 2, 2, 1};
            // frame_first 是 **float 下标**：各类的每帧 float 数不同，
            // 累计出来就是 rotate 6 + rotate 2 + translate 6 + scale 3 + shear 3 +
            // attachment 2 + color 10 + twoColor 16 …，绝不是"帧号 × 某常数"。
            const uint32_t kWantFrameFirst[] = {0, 6, 8, 14, 17, 20, 22, 32, 48};
            for (int i = 0; i < 9; ++i)
            {
                CheckEq(asset.timelines[i].type, kWantType[i], "anim: 时间轴类型序列");
                CheckEq(asset.timelines[i].target, kWantTarget[i], "anim: 时间轴 target 序列");
                CheckEq(asset.timelines[i].frame_count, kWantFrames[i], "anim: 时间轴帧数序列");
                CheckEq(asset.timelines[i].frame_first, kWantFrameFirst[i], "anim: frame_first 是 float 下标");
            }
            CheckEq(asset.timelines[8].frame_first != 0u && asset.timelines[8].curve_first != 0u, true,
                    "anim: 最后一条时间轴的 frame_first / curve_first 都非 0");
        }
        CheckEq(static_cast<long long>(asset.frames.size()), 50LL, "anim: 帧数据 50 个 float");
        // 曲线：rotate 2 段（stepped + bezier）+ translate 1 + color 1 + twoColor 1 = 5 段
        CheckEq(static_cast<long long>(asset.curve_types.size()), 5LL, "anim: 5 段曲线");
        if (asset.curve_types.size() == 5)
        {
            CheckEq(asset.curve_types[0], static_cast<uint8_t>(SpineCurveType::kStepped), "anim: 段 0 = stepped");
            CheckEq(asset.curve_types[1], static_cast<uint8_t>(SpineCurveType::kBezier), "anim: 段 1 = bezier");
            CheckEq(asset.curve_types[2], static_cast<uint8_t>(SpineCurveType::kLinear), "anim: 段 2 = linear");
            CheckEq(asset.curve_types[3], static_cast<uint8_t>(SpineCurveType::kLinear), "anim: 段 3 = linear");
            CheckEq(asset.curve_types[4], static_cast<uint8_t>(SpineCurveType::kLinear), "anim: 段 4 = linear");
        }
        CheckEq(static_cast<long long>(asset.beziers.size()), 1LL, "anim: 1 条 bezier");
        if (asset.beziers.size() == 1)
        {
            Check(asset.beziers[0].cx1 == 0.25f && asset.beziers[0].cy1 == 0.5f &&
                      asset.beziers[0].cx2 == 0.75f && asset.beziers[0].cy2 == 0.8f,
                  "anim: bezier 控制点按 (cx1, cy1, cx2, cy2) 落盘（顺序错了不报错）");
        }
        // 变长载荷：只有 attachment 时间轴有（2 帧）
        CheckEq(static_cast<long long>(asset.anim_attachment_names.size()), 1LL, "anim: 1 个动画附件名");
        Check(asset.anim_attachment_names.size() == 1 && asset.anim_attachment_names[0] == "quad",
              "anim: 动画附件名去重后只剩 quad");
        if (asset.timelines.size() == 9 && asset.timeline_payload.size() >= 8)
        {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(asset.timeline_payload.data());
            Check(p[0] != SpineGpuConstants::kNoAttachmentName,
                  "anim: attachment 第 0 帧指向真实附件名");
            CheckEq(p[1], SpineGpuConstants::kNoAttachmentName,
                    "anim: attachment 第 1 帧是空名 ⇒ kNoAttachmentName（**不是 0**：0 是合法偏移）");
        }
        // deform / drawOrder 在**别的**夹具里（deform / clip 段），这里不该出现
        for (const SpineGpuTimeline& t : asset.timelines)
        {
            Check(t.type != static_cast<uint16_t>(SpineTimelineType::kDeform) &&
                      t.type != static_cast<uint16_t>(SpineTimelineType::kDrawOrder),
                  "anim: 基线夹具没有 deform / drawOrder 时间轴");
        }
        g_prefix.clear();
    }

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

    // ---- 5b. 报错路径：骨骼的两条"静默就会错"的规则 ----
    // 两条都是参考实现**不报错**而我们报错的地方 —— 所以要把报错本身钉住。
    {
        // (1) transform 串认不出。参考实现 5 个 if 都没有 else（SkeletonJson.cpp:173-189）
        //     ⇒ 静默留在 Normal，整棵子树的继承行为悄悄变掉。
        const char* bad_mode = R"({
          "skeleton": { "spine": "3.8.99" },
          "bones": [ { "name": "root", "transform": "noSuchMode" } ],
          "slots": [], "skins": {}
        })";
        SpineSourceSkeleton bm;
        SpineImportResult r = ParseSpineSkeletonJson(bad_mode, std::string(bad_mode).size(), &atlas, bm);
        Check(!r.ok, "认不出的 transform 串 ⇒ 必须报错（参考实现静默退回 normal）");
        Check(r.error.find("noSuchMode") != std::string::npos, "错误信息里带那个串");
        std::printf("  (预期报错) %s\n", r.error.c_str());
    }
    {
        // (2) 父骨骼不存在。我们早先漏了这一步 ⇒ 那根骨骼**静默变成根骨骼**，
        //     整棵子树的位移/旋转全错，而"看起来只是摆位不对"没有任何报错。
        const char* bad_parent = R"({
          "skeleton": { "spine": "3.8.99" },
          "bones": [ { "name": "root", "parent": "nope" } ],
          "slots": [], "skins": {}
        })";
        SpineSourceSkeleton bp;
        SpineImportResult r = ParseSpineSkeletonJson(bad_parent, std::string(bad_parent).size(), &atlas, bp);
        Check(!r.ok, "父骨骼不存在 ⇒ 必须报错（不能静默当成根骨骼）");
        Check(r.error.find("nope") != std::string::npos, "错误信息里带父骨骼名");
        std::printf("  (预期报错) %s\n", r.error.c_str());
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

    // 21 个 chunk：strings / slot_bone / bone_setup / 动画 11-16 / 槽位 setup 17 /
    //              约束 setup 18 / 执行顺序 19 / path 几何 20 /
    //              vertex / 影响流 21,22 / uv / index / draw_items / attachments / atlas_pages
    //              （夹具没有 clip_data = 9）
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
    // v11：夹具带约束 ⇒ chunk 18 / 19 都落盘；带 path 附件 ⇒ chunk 20 落盘。
    // 落盘条件与写入端一致（`has_constraints` / `!path_attachments.empty()`），
    // 但这里**不**用条件表达式去算 —— 而是先断言夹具确实带约束，再按"全落"求和：
    // 万一哪天夹具被改成不带约束，这里会先红，而不是悄悄少算三项。
    const bool has_constraints =
        !asset.iks.empty() || !asset.transforms.empty() || !asset.paths.empty();
    const bool has_path_att = !asset.path_attachments.empty();
    Check(has_constraints, "夹具带约束（否则 v11 的 chunk 18 / 19 不落盘，本段断言失去意义）");
    Check(has_path_att, "夹具带 path 附件（否则 v11 的 chunk 20 不落盘，本段断言失去意义）");

    const long long want_payload =
        CStrBytes(asset.bone_names) + CStrBytes(asset.skin_names) + CStrBytes(asset.slot_names) +
        CStrBytes(asset.attachment_names) + CStrBytes(asset.atlas_page_names) +
        CStrBytes(asset.slot_setup_attachment_names) +                                // id=1（v10 组）
        CStrBytes(asset.anim_names) + CStrBytes(asset.anim_attachment_names) +        // id=1 strings
        CStrBytes(asset.path_attachment_names) +                                      // id=1（v11 组）
        static_cast<long long>(asset.slot_bone.size()) * 2LL +                       // id=2
        static_cast<long long>(asset.bones.size()) *
            static_cast<long long>(sizeof(SpineGpuBone)) +                          // id=10（36 B/骨）
        static_cast<long long>(asset.anims.size()) *
            static_cast<long long>(sizeof(SpineGpuAnim)) +                          // id=11（20 B/条）
        static_cast<long long>(asset.timelines.size()) *
            static_cast<long long>(sizeof(SpineGpuTimeline)) +                       // id=12（32 B/条）
        static_cast<long long>(asset.frames.size()) * 4LL +                           // id=13（float）
        static_cast<long long>(asset.curve_types.size()) +                            // id=14（u8/段）
        static_cast<long long>(asset.beziers.size()) *
            static_cast<long long>(sizeof(SpineGpuBezier)) +                          // id=15（16 B/段）
        static_cast<long long>(asset.timeline_payload.size()) +                       // id=16（变长净载荷）
        static_cast<long long>(asset.slots.size()) *
            static_cast<long long>(sizeof(SpineGpuSlot)) +                             // id=17（40 B/槽）
        (SpineGpuConstants::kConstraintHeaderSize +                                   // id=18（v11）
         static_cast<long long>(asset.iks.size()) * sizeof(SpineGpuIkConstraint) +
         static_cast<long long>(asset.transforms.size()) * sizeof(SpineGpuTransformConstraint) +
         static_cast<long long>(asset.paths.size()) * sizeof(SpineGpuPathConstraint) +
         static_cast<long long>(asset.constraint_bone_pool.size()) * 2LL) +
        (SpineGpuConstants::kUpdateOrderHeaderSize +                                  // id=19（v11）
         static_cast<long long>(asset.update_steps.size()) * sizeof(SpineGpuUpdateStep) +
         static_cast<long long>(asset.update_reset.size()) * 2LL) +
        (SpineGpuConstants::kPathAttHeaderSize +                                      // id=20（v11）
         static_cast<long long>(asset.path_attachments.size()) * sizeof(SpineGpuPathAttachment) +
         static_cast<long long>(asset.path_bone_pool.size()) * 2LL +
         static_cast<long long>(asset.path_vertices.size()) * 4LL +
         static_cast<long long>(asset.path_lengths.size()) * 4LL) +
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
    CheckEq(static_cast<long long>(nchunk), 21,
            "字节流里 chunk_count = 21（夹具没有 clip_data；v7 多了影响流 21 / 22，v8 多了骨骼 setup 10，"
            "v9 多了动画 11-16，v10 多了槽位 setup 17，v11 多了约束 setup 18 / 执行顺序 19 / path 几何 20）");

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
        const uint32_t kExpectedIds[] = {1,  2,  10, 11, 12, 13, 14, 15, 16, 17, 18,
                                         19, 20, 3,  21, 22, 4,  5,  6,  7,  8};
        CheckEq(static_cast<long long>(id), static_cast<long long>(kExpectedIds[i]),
                "chunk 表: id 顺序 = 1,2,10,11,12,13,14,15,16,17,18,19,20,3,21,22,4,5,6,7,8");
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
    CheckEq(back.header.chunk_count, 21, "往返: chunk_count = 21（夹具没有 clip_data；v7 多了影响流 21 / 22，v8 多了骨骼 setup 10，v9 多了动画 11-16，v10 多了槽位 setup 17，v11 多了约束 setup 18 / 执行顺序 19 / path 几何 20）");
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
    CheckEq(static_cast<long long>(back.bones.size()), static_cast<long long>(asset.bones.size()),
            "往返: 骨骼 setup 条数");
    Check(back.bones == asset.bones, "往返: 骨骼 setup 逐字节一致（含 pad）");
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

    // ---- 动画往返（chunk 11-16）----
    // 六块全部逐字节/逐值比一遍。它们之间只有**下标**引用，读端把下标换一个单位
    // （帧 vs float、条 vs 字节）不会报错，只会播错 —— 所以每块都要单独对。
    Check(back.anims == asset.anims, "往返: 动画目录逐字段一致（含 name_offset / duration）");
    Check(back.timelines == asset.timelines, "往返: 时间轴表逐字段一致");
    Check(back.frames == asset.frames, "往返: 帧数据逐值一致");
    Check(back.curve_types == asset.curve_types, "往返: 曲线类型逐段一致");
    Check(back.beziers == asset.beziers, "往返: bezier 控制点逐值一致");
    Check(back.timeline_payload == asset.timeline_payload, "往返: 变长载荷逐字节一致");
    Check(back.anim_names == asset.anim_names, "往返: 动画名（字符串表第 7 组）");
    Check(back.anim_attachment_names == asset.anim_attachment_names, "往返: 动画附件名（第 9 组）");

    // ---- 约束往返（chunk 18-20，v11）----
    Check(back.iks == asset.iks, "往返: IK 约束逐字段一致");
    Check(back.transforms == asset.transforms, "往返: transform 约束逐字段一致");
    Check(back.paths == asset.paths, "往返: path 约束逐字段一致（含 path_index）");
    Check(back.constraint_bone_pool == asset.constraint_bone_pool, "往返: 约束骨骼池一致");
    Check(back.update_steps == asset.update_steps, "往返: 执行顺序逐字段一致");
    Check(back.update_reset == asset.update_reset, "往返: 复位骨骼列表一致");
    Check(back.path_attachments == asset.path_attachments, "往返: PathAttachment 记录逐字段一致");
    Check(back.path_bone_pool == asset.path_bone_pool, "往返: path 骨骼池一致");
    Check(back.path_vertices == asset.path_vertices, "往返: path 顶点逐值一致");
    Check(back.path_lengths == asset.path_lengths, "往返: path lengths 逐值一致");
    Check(back.path_attachment_names == asset.path_attachment_names,
          "往返: path 附件名（字符串表第 8 组）");
    // ⚠️ 光比"名字符合预期"不够 —— 必须验证 name_offset 真的**指到了那个名字**。
    //    偏移算错（比如把 path 名那组排到动画附件名之后）字节照样自洽，
    //    只有把偏移翻译回字符串才能抓到。
    {
        std::vector<uint8_t> blob;
        std::vector<uint32_t> slot_off;
        std::vector<uint32_t> anim_off;
        std::vector<uint32_t> anim_att_off;
        std::vector<uint32_t> path_att_off;
        std::vector<uint32_t> clip_off;
        BuildSpineStringTable(back, blob, slot_off, anim_off, anim_att_off, path_att_off, clip_off);
        bool name_ok = asset.path_attachment_names.size() == 1 &&
                       back.path_attachments.size() == 1 &&
                       back.path_attachments[0].name_offset < blob.size() &&
                       std::string(reinterpret_cast<const char*>(blob.data() +
                                       back.path_attachments[0].name_offset)) == "path1";
        Check(name_ok, "往返: path 附件的 name_offset 翻译回字符串 = 'path1'");
        // 顺带钉住"槽位 setup 附件名"的组序没被 v11 插进来的新组挤歪
        bool slot_name_ok = back.slot_setup_attachment_names.size() == 2 && slot_off.size() == 2 &&
                            std::string(reinterpret_cast<const char*>(blob.data() + slot_off[1])) == "mesh";
        Check(slot_name_ok, "往返: 槽位 setup 附件名偏移仍指向 'mesh'（v11 新组没挤歪旧组）");
    }

    // ---- 槽位 setup 往返（chunk 17）----
    // ⚠️ 夹具里槽位 0 是**默认色**、槽位 1 起才带显式色与暗色 ——
    //    "全是默认值"的往返什么也挡不住（纯白读回来还是纯白），必须有一条非默认。
    CheckEq(static_cast<long long>(back.slots.size()), static_cast<long long>(asset.slots.size()),
            "往返: 槽位 setup 条数");
    Check(back.slots == asset.slots, "往返: 槽位 setup 逐字节一致（含 flags）");
    Check(back.slot_setup_attachment_names == asset.slot_setup_attachment_names,
          "往返: 槽位 setup 附件名（字符串表第 4 组）");
    // ⚠️ 夹具的 s1 有显式颜色 + 暗色 ⇒ 断言必须**非零**：
    //    "读回来还是纯白"这种断言在"写端根本没写颜色"时也会通过。
    CheckEq(static_cast<long long>(asset.slots.size()), 2LL, "往返: 槽位数 = 2");
    if (asset.slots.size() == 2)
    {
        Check(asset.slots[0].color_r == 1.0f && asset.slots[0].color_a == 1.0f &&
                  asset.slots[0].dark_a == 0.0f && asset.slots[0].flags == 0u,
              "chunk 17: s0 是默认（纯白 / 无暗色 / flags=0）");
        Check(asset.slots[1].flags == SpineGpuSlotFlags::kSlotFlagHasDark, "chunk 17: s1 的 flags 带 has_dark");
        Check(std::fabs(asset.slots[1].color_r - 0x80 / 255.0f) < 1e-6f &&
                  std::fabs(asset.slots[1].color_g - 0xC0 / 255.0f) < 1e-6f &&
                  std::fabs(asset.slots[1].color_b - 1.0f) < 1e-6f &&
                  std::fabs(asset.slots[1].color_a - 0x40 / 255.0f) < 1e-6f,
              "chunk 17: s1 的 setup 色 = 80c0ff40（**不是**默认纯白）");
        Check(std::fabs(asset.slots[1].dark_r - 0x20 / 255.0f) < 1e-6f &&
                  std::fabs(asset.slots[1].dark_g - 0x40 / 255.0f) < 1e-6f &&
                  std::fabs(asset.slots[1].dark_b - 0x60 / 255.0f) < 1e-6f &&
                  std::fabs(asset.slots[1].dark_a - 1.0f) < 1e-6f,
              "chunk 17: s1 的 setup 暗色 = 204060 + alpha **恒 1**（参考实现写死）");
        // setup 附件名偏移必须指向真名字（不是 0xFFFFFFFF、也不是错位到别人身上）
        const std::vector<uint8_t> blob = BuildSpineStringBlob(asset);
        const char* s0_att = SpineGpuStringAt(blob, asset.slots[0].setup_attachment_name_offset);
        const char* s1_att = SpineGpuStringAt(blob, asset.slots[1].setup_attachment_name_offset);
        Check(s0_att != nullptr && std::string(s0_att) == "quad",
              "chunk 17: s0 的 setup 附件名偏移解析出来是 'quad'");
        Check(s1_att != nullptr && std::string(s1_att) == "mesh",
              "chunk 17: s1 的 setup 附件名偏移解析出来是 'mesh'");
    }
    CheckEq(static_cast<long long>(back.anims.size()), 2LL, "往返: 动画数 = 2");

    // =================================================================
    // v11 专项：path 约束的 `path_index` 到底指向哪条 PathAttachment
    // -----------------------------------------------------------------
    // 主夹具（kSkeleton）里槽位 s0 的 setup 附件是**区域** quad ⇒ 答案是
    // kNoPathAttachment（约束在 setup 状态下不生效）。那只是半个用例 ——
    // 另一半"setup 附件**就是** path"必须由下面这个最小夹具补上，
    // 否则"恒等于 kNoPathAttachment"的实现也能通过全部断言。
    // =================================================================
    {
        g_prefix = "[path_index] ";
        const char* kPathSetup = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [ { "name": "root" } ],
  "slots": [ { "name": "s0", "bone": "root", "attachment": "p1" } ],
  "path": [
    { "name": "pc", "order": 0, "bones": [ "root" ], "target": "s0",
      "positionMode": "fixed", "spacingMode": "length", "rotateMode": "tangent",
      "rotation": 0, "position": 0, "spacing": 0, "rotateMix": 1, "translateMix": 1 }
  ],
  "skins": {
    "default": {
      "s0": {
        "p1": { "type": "path", "closed": false, "constantSpeed": false,
                "vertexCount": 3, "vertices": [ 0,0, 10,0, 20,10 ], "lengths": [ 1 ] },
        "p2": { "type": "path", "closed": true, "constantSpeed": true,
                "vertexCount": 3, "vertices": [ 0,0, 5,0, 7,7 ], "lengths": [ 1 ] }
      }
    }
  }
})";
        SpineSourceSkeleton ps;
        const SpineImportResult rps =
            ParseSpineSkeletonJson(kPathSetup, std::string(kPathSetup).size(), &atlas, ps);
        Check(rps.ok, "最小夹具（槽位 setup 附件是 path）解析成功");
        if (!rps.ok)
        {
            std::printf("  error: %s\n", rps.error.c_str());
        }
        else
        {
            CheckEq(static_cast<long long>(ps.path_attachments.size()), 2LL,
                    "同一个槽位挂两条 path 附件是**合法的**（不报错）");
            SpineGpuAsset pa;
            const SpineImportResult rpb = BakeSpineGpuAsset(ps, pa);
            Check(rpb.ok, "bake 成功（不再因「两条 path」报错）");
            if (!rpb.ok)
            {
                std::printf("  error: %s\n", rpb.error.c_str());
            }
            else
            {
                CheckEq(static_cast<long long>(pa.paths.size()), 1LL, "一条 path 约束");
                if (pa.paths.size() == 1)
                {
                    // ⚠️ 判据是"名字 == 槽位 setup 附件名（p1）"，不是"第一条"。
                    //    排序后 p1 在前，所以这条还**不能**区分两种实现 ——
                    //    下面再验一次名字本身，两条合起来才钉得住。
                    CheckEq(static_cast<long long>(pa.paths[0].path_index), 0LL,
                            "path_index = 0（setup 附件 p1 是 path ⇒ 约束生效）");
                }
                CheckEq(static_cast<long long>(pa.path_attachment_names.size()), 2LL,
                        "path 附件名 2 条（与记录一一对应）");
                if (pa.path_attachment_names.size() == 2)
                {
                    Check(pa.path_attachment_names[0] == "p1" && pa.path_attachment_names[1] == "p2",
                          "path 附件名按 (skin, slot, name) 排序 ⇒ p1 / p2");
                }
                if (pa.path_attachments.size() == 2)
                {
                    // p2 是 closed + constantSpeed ⇒ flags = 3。它**不是**约束用的那条，
                    //    但必须完整落盘（换装 / attachment 时间轴可能切到它）。
                    CheckEq(static_cast<long long>(pa.path_attachments[1].flags), 3LL,
                            "p2 的 flags = closed|constantSpeed = 3（不是约束用的那条也要落盘）");
                    Check(pa.path_vertices[6] == 0.0f && pa.path_vertices[8] == 5.0f &&
                              pa.path_vertices[10] == 7.0f,
                          "p2 的顶点独立成段（(0,0) (5,0) (7,7)）");
                }
                // 反过来：把槽位的 setup 附件换成不是 path 的名字 ⇒ kNoPathAttachment。
                //   这条才是真正区分"按名字匹配"与"取第一条"的断言。
                SpineSourceSkeleton ps2 = ps;
                ps2.slots[0].attachment_name = "p2";
                SpineGpuAsset pa2;
                const SpineImportResult rpb2 = BakeSpineGpuAsset(ps2, pa2);
                Check(rpb2.ok, "setup 附件改成 p2 后 bake 成功");
                if (rpb2.ok && pa2.paths.size() == 1 && pa2.path_attachments.size() == 2)
                {
                    // ⚠️ 关键：setup = p2 时匹配到的是**第 1 条**而不是第 0 条。
                    //    "取槽位上第一条 path"的实现在这里会给 0 ⇒ 抓得到。
                    CheckEq(static_cast<long long>(pa2.paths[0].path_index), 1LL,
                            "setup 附件 = p2 ⇒ path_index = 1（按名字匹配，不是取第一条）");
                    // ⚠️ 关键：把 setup 附件改成一个**不存在**的名字，
                    //    按名字匹配 ⇒ kNoPathAttachment；"取第一条"会给出 0。
                    SpineSourceSkeleton ps3 = ps;
                    ps3.slots[0].attachment_name = "not-a-path";
                    SpineGpuAsset pa3;
                    const SpineImportResult rpb3 = BakeSpineGpuAsset(ps3, pa3);
                    Check(rpb3.ok, "setup 附件改成不存在的名字后 bake 成功");
                    if (rpb3.ok && pa3.paths.size() == 1)
                    {
                        CheckEq(static_cast<long long>(pa3.paths[0].path_index),
                                static_cast<long long>(SpineGpuConstants::kNoPathAttachment),
                                "setup 附件不是任何 path ⇒ path_index = kNoPathAttachment（不是 0）");
                    }
                }
            }
        }
        g_prefix.clear();
    }

    // =================================================================
    // S3：槽位解算（color / twoColor / attachment / drawOrder）
    // -----------------------------------------------------------------
    // 期望值是**手算**的，不是从实现里抄回来的 —— 只有这样才能抓到"两边一起错"。
    // 每条都带参考实现的公式出处（doc/GPU_SPINE_V7_ANIMATION.md §7.0.4）：
    //   * 采样口径 = `Animation::apply(..., alpha=1, MixBlend_First, In)`
    //     （AnimationState.cpp:388/402），alpha==1 时它就是"先复位 setup 再覆盖"。
    //   * 帧定位 = `Animation::binarySearch(...)`（Animation.cpp:84-106），
    //     返回的是**后**一帧 ⇒ 段号 = frame/ENTRIES - 1。
    //   * 插值参数 = (time - prev) / (cur - prev)，再过 `getCurvePercent()`。
    // =================================================================
    {
        g_prefix = "[S3/解算] ";
        const std::vector<uint8_t> blob = BuildSpineStringBlob(asset);

        // 夹具里的颜色常量（/255.0f，与十六进制串逐分量对应）
        const float k80 = 0x80 / 255.0f;   // 0.50196…
        const float kC0 = 0xC0 / 255.0f;
        const float k40 = 0x40 / 255.0f;
        const float k20 = 0x20 / 255.0f;
        const float k10 = 0x10 / 255.0f;
        const float k30 = 0x30 / 255.0f;
        const float k60 = 0x60 / 255.0f;
        const auto Lerp = [](float a, float b, float t) { return a + (b - a) * t; };

        auto AttName = [&blob](const SpineGpuAsset&, uint32_t off) -> std::string {
            const char* s = SpineGpuStringAt(blob, off);
            return (s == nullptr) ? std::string("<none>") : std::string(s);
        };
        auto Sample = [&](const SpineGpuAsset& a, uint32_t ai, float t, SpineGpuSlotSampleResult& out) -> bool {
            const SpineImportResult r = SpineGpuSampleSlots(a, ai, t, out);
            if (!r.ok)
            {
                std::printf("  error: %s\n", r.error.c_str());
            }
            Check(r.ok, (std::string("采样 anim ") + std::to_string(ai) + " @ t=" + std::to_string(t) + " 成功").c_str());
            return r.ok;
        };
        auto Near = [](float got, float want) { return std::fabs(got - want) < 1e-5f; };
        auto Color4 = [&](const float* c, float r, float g, float b, float a) {
            return Near(c[0], r) && Near(c[1], g) && Near(c[2], b) && Near(c[3], a);
        };

        SpineGpuSlotSampleResult s;
        const uint32_t kWalk = 0;
        const uint32_t kIdle = 1;

        // ---- s0：attachment 时间轴（2 帧：t0 "quad" / t0.5 空名）+ color 时间轴（2 帧 linear）----
        Check(Sample(asset, kWalk, -1.0f, s), "S3: t=-1 采样成功");
        if (Sample(asset, kWalk, 0.0f, s))
        {
            Check(Color4(s.slots[0].color, 1.0f, 0.0f, 0.0f, 1.0f), "S3: t=0 s0 颜色 = ff0000ff（取帧 0）");
            Check(AttName(asset, s.slots[0].attachment_name_offset) == "quad", "S3: t=0 s0 挂 'quad'");
        }
        if (Sample(asset, kWalk, -1.0f, s))
        {
            // `time < frames[0]` ⇒ **保持 setup**（ColorTimeline.cpp:72-81 / AttachmentTimeline.cpp:64-70）
            Check(Color4(s.slots[0].color, 1.0f, 1.0f, 1.0f, 1.0f), "S3: t<首帧 ⇒ s0 停在 setup 色（纯白）");
            Check(AttName(asset, s.slots[0].attachment_name_offset) == "quad",
                  "S3: t<首帧 ⇒ s0 复位到 **setup 附件** 'quad'（不是「隐藏」）");
        }
        if (Sample(asset, kWalk, 0.25f, s))
        {
            Check(Color4(s.slots[0].color, Lerp(1.0f, 0.0f, 0.25f), Lerp(0.0f, 1.0f, 0.25f), 0.0f,
                         Lerp(1.0f, k80, 0.25f)),
                  "S3: t=0.25 s0 颜色 = 线性插值 25%（linear 段，无 curve）");
            Check(AttName(asset, s.slots[0].attachment_name_offset) == "quad",
                  "S3: t=0.25 s0 仍是 'quad'（binarySearch 返回后一帧 ⇒ 帧号 = 1-1 = 0）");
        }
        if (Sample(asset, kWalk, 0.5f, s))
        {
            Check(Color4(s.slots[0].color, 0.5f, 0.5f, 0.0f, Lerp(1.0f, k80, 0.5f)),
                  "S3: t=0.5 s0 颜色 = 插值 50%");
            Check(s.slots[0].attachment_name_offset == SpineGpuConstants::kNoAttachmentName,
                  "S3: t=0.5 s0 **空名** ⇒ 这一帧隐藏（kNoAttachmentName，不是偏移 0）");
        }
        if (Sample(asset, kWalk, 1.0f, s))
        {
            Check(Color4(s.slots[0].color, 0.0f, 1.0f, 0.0f, k80), "S3: t=1 s0 颜色 = 00ff0080（末帧）");
            Check(s.slots[0].attachment_name_offset == SpineGpuConstants::kNoAttachmentName,
                  "S3: t=1 s0 仍隐藏（time >= 末帧 ⇒ 取末帧）");
        }
        if (Sample(asset, kWalk, 2.0f, s))
        {
            Check(Color4(s.slots[0].color, 0.0f, 1.0f, 0.0f, k80), "S3: t=2（越过末帧）s0 颜色不变");
        }

        // ---- s1：twoColor 时间轴（2 帧 linear：light ffffffff→808080ff，dark 000000→102030）----
        if (Sample(asset, kWalk, -1.0f, s))
        {
            Check(Color4(s.slots[1].color, k80, kC0, 1.0f, k40), "S3: t<首帧 ⇒ s1 停在 **setup 色** 80c0ff40");
            // S5 起按参考实现的真实行为（逐条出处，手算）：
            //   * 暗色初值 = `Slot::Slot()` 的 (0,0,0,0)，`Slot::setToSetupPose()` **不复位**它（Slot.cpp:42-64）；
            //   * `TwoColorTimeline` 的 `time < 第 0 帧` + MixBlend_First 分支写的是
            //     `dark += (dark - setupDark) * alpha`（符号反了，TwoColorTimeline.cpp:84-93），alpha = 1
            //     ⇒ dark.rgb = 0 + (0 - setup) = **-setup**，且这一路**不夹** [0,1]；dark.a 不动（= 0）。
            //   S3 时这里写的是"停在 setup 暗色"——那是意图，不是参考实现（S5 逐行转录 apply 时发现；
            //   OSGame 资产没有走到这一支，782 资产对拍抓不到它，只能靠这条手算断言钉住）。
            Check(Color4(s.slots[1].dark, -k20, -k40, -k60, 0.0f),
                  "S3: t<首帧 ⇒ s1 暗色 = -setup（参考实现 TwoColor 的符号笔误 + setToSetupPose 不复位暗色）");
        }
        if (Sample(asset, kWalk, 0.0f, s))
        {
            Check(Color4(s.slots[1].color, 1.0f, 1.0f, 1.0f, 1.0f), "S3: t=0 s1 light = ffffffff");
            Check(Color4(s.slots[1].dark, 0.0f, 0.0f, 0.0f, 1.0f),
                  "S3: t=0 s1 dark = 000000 且 **alpha 恒 1**（TwoColorTimeline.cpp:139）");
        }
        if (Sample(asset, kWalk, 0.25f, s))
        {
            Check(Color4(s.slots[1].color, Lerp(1.0f, k80, 0.5f), Lerp(1.0f, k80, 0.5f),
                         Lerp(1.0f, k80, 0.5f), 1.0f),
                  "S3: t=0.25 s1 light 插值 50%（帧间距 0.5）");
            Check(Color4(s.slots[1].dark, Lerp(0.0f, k10, 0.5f), Lerp(0.0f, k20, 0.5f),
                         Lerp(0.0f, k30, 0.5f), 1.0f),
                  "S3: t=0.25 s1 dark 插值 50%，alpha 仍恒 1（dark 的 alpha 不参与插值）");
        }
        if (Sample(asset, kWalk, 0.5f, s))
        {
            Check(Color4(s.slots[1].color, k80, k80, k80, 1.0f), "S3: t=0.5 s1 light = 808080ff");
            Check(Color4(s.slots[1].dark, k10, k20, k30, 1.0f), "S3: t=0.5 s1 dark = 102030");
        }
        if (Sample(asset, kWalk, 1.0f, s))
        {
            Check(Color4(s.slots[1].color, k80, k80, k80, 1.0f), "S3: t=1 s1 停在末帧（time >= 末帧）");
        }

        // ---- drawOrder：夹具没有这条时间轴 ⇒ 恒为 setup 序 ----
        if (Sample(asset, kWalk, 0.25f, s))
        {
            Check(s.draw_order.size() == 2 && s.draw_order[0] == 0 && s.draw_order[1] == 1,
                  "S3: 没有 drawOrder 时间轴 ⇒ 绘制顺序 = setup 序 0,1");
        }

        // ---- idle（只有一条骨骼 rotate 时间轴）⇒ 槽位全是 setup ----
        if (Sample(asset, kIdle, 0.0f, s))
        {
            Check(Color4(s.slots[0].color, 1.0f, 1.0f, 1.0f, 1.0f), "S3: idle ⇒ s0 停在 setup 色");
            Check(Color4(s.slots[1].color, k80, kC0, 1.0f, k40), "S3: idle ⇒ s1 停在 setup 色");
            Check(AttName(asset, s.slots[0].attachment_name_offset) == "quad" &&
                      AttName(asset, s.slots[1].attachment_name_offset) == "mesh",
                  "S3: idle ⇒ 两个槽位都停在 setup 附件");
        }

        // ---- 读回来的容器必须解算出**同一个结果** ----
        // 写端对了、读端错了，往返的逐字节断言全都看不出来 —— 只有解算能抓到。
        SpineGpuSlotSampleResult s_back;
        bool same = true;
        for (float t : {-1.0f, 0.0f, 0.25f, 0.5f, 1.0f, 2.0f})
        {
            if (!Sample(asset, kWalk, t, s) || !Sample(back, kWalk, t, s_back))
            {
                same = false;
                continue;
            }
            if (s.slots.size() != s_back.slots.size())
            {
                same = false;
                continue;
            }
            for (size_t i = 0; i < s.slots.size(); ++i)
            {
                if (!Color4(s_back.slots[i].color, s.slots[i].color[0], s.slots[i].color[1],
                            s.slots[i].color[2], s.slots[i].color[3]) ||
                    !Color4(s_back.slots[i].dark, s.slots[i].dark[0], s.slots[i].dark[1],
                            s.slots[i].dark[2], s.slots[i].dark[3]) ||
                    s_back.slots[i].attachment_name_offset != s.slots[i].attachment_name_offset)
                {
                    same = false;
                }
            }
            if (s.draw_order != s_back.draw_order)
            {
                same = false;
            }
        }
        Check(same, "S3: 原始资产与**读回来**的容器解算出同一个结果（读端漂移的唯一拦网）");
        g_prefix.clear();
    }

    // =================================================================
    // S5：多轨混合（SpineGpuAnimationState）—— 期望值**手算**
    // -----------------------------------------------------------------
    // 真正钉住转录的是参考对拍（SpineGpuRefParityTest 的 S5 段，782 资产 × 随机脚本），但它依赖
    // G 盘的参考源码、不进 ctest。这里用夹具手算几条最有代表性的公式，让 CI 也有一道拦网。
    // 只比**局部**值：夹具的 IK 故意配成"父骨骼没有父骨骼"（测读端），跑不了世界变换。
    //
    // 夹具：root setup rotation 15、x 3、y -4、scale (2, 0.5)、shear (7, -9)；hip setup rotation -30。
    //   idle：root rotate 单帧 7（时长 0）
    //   walk：root rotate [0: 10 stepped] [0.5: 190 bezier] [1: -20]；translate [0:(1,2)] [1:(-3,4)]；
    //         scale [0:(-2,0.5)]；shear [0.25:(5,-6)]；hip rotate [0: 30]；s0 color ff0000ff → 00ff0080
    // =================================================================
    {
        g_prefix = "[S5/多轨] ";
        const uint32_t kWalk = 0;
        const uint32_t kIdle = 1;
        auto Near = [](float got, float want) { return std::fabs(got - want) < 1e-5f; };

        SpineGpuRuntimeData data;
        SpineGpuSkeletonState st;
        Check(SpineGpuBuildRuntimeData(asset, data).ok, "建查找表");
        Check(SpineGpuInitSkeletonState(asset, data, st).ok, "初始化骨架状态（= 构造 + setToSetupPose）");
        SpineGpuAnimationStateData asd;
        asd.default_mix = 0.5f;
        SpineGpuAnimationState as(asset, data, &asd);
        const auto Step = [&](float dt) {
            as.Update(dt);
            const SpineImportResult r = as.Apply(st);
            if (!r.ok)
            {
                std::printf("  error: %s\n", r.error.c_str());
            }
            Check(r.ok, "Apply 成功");
        };
        SpineGpuBoneState& root = st.bones[0];
        SpineGpuBoneState& hip = st.bones[1];

        // ---- 1) 轨道 0 单条：idle（alpha 1 + MixBlend_First）----
        // RotateTimeline 末帧分支：r = 7；r += setup - bone = 7 + (15 - 15)；bone += wrap(7) = 22
        as.SetAnimation(0, kIdle, true);
        Step(0.0f);
        Check(Near(root.rotation, 22.0f), "idle：root.rotation = 15 + 7");

        // ---- 2) 切 walk，混合时长 = default_mix 0.5；走 0.25 ⇒ mix = 0.5 ----
        // timelineMode：idle 的 root rotate = Hold（walk 也有），walk 的 root rotate = Subsequent，
        //               walk 其余 = First（⇒ blend 用 Setup）。
        //   idle（Hold，alpha = alphaHold = 1，Setup）：rotation = 15 + 7 = 22
        //   walk root rotate（Subsequent ⇒ First，alpha = mix 0.5，走 applyRotateTimeline）：
        //     r1 = 当前 22；t=0.25 落在 stepped 段 ⇒ r2 = wrap(10 + wrap(180)·0 + 15) = 25；
        //     diff = 3，首帧 ⇒ total = 3；rotation = 22 + 3·0.5 = 23.5
        //   translate（Setup，alpha 0.5）：帧值 (1 + (-4)·0.25, 2 + 2·0.25) = (0, 2.5)
        //     ⇒ x = 3 + 0·0.5 = 3，y = -4 + 2.5·0.5 = -2.75
        //   scale（Setup，alpha 0.5，方向 In）：帧值 = 倍率 × setup = (-2·2, 0.5·0.5) = (-4, 0.25)
        //     bx = |2|·sign(-4) = -2 ⇒ sx = -2 + (-4 + 2)·0.5 = -3；by = |0.5|·sign(0.25) = 0.5 ⇒ sy = 0.375
        //     （"混入取关键帧的符号"：从 setup 的 +2 直接翻到 -2 再插值，不经过 0）
        //   shear（Setup，alpha 0.5）：t=0.25 恰在唯一一帧 ⇒ (5, -6) ⇒ (7 + 2.5, -9 - 3) = (9.5, -12)
        //   hip rotate（First ⇒ Setup，alpha 0.5，applyRotateTimeline）：r1 = setup -30，r2 = -30 + 30 = 0，
        //     diff = 30 ⇒ rotation = -30 + 30·0.5 = -15
        //   s0 color（Setup，alpha 0.5）：帧值 (0.75, 0.25, 0, 1 + (128/255 - 1)·0.25)，
        //     先复位白色再 add((v - c)·0.5) ⇒ (0.875, 0.625, 0.5, 1 + (a_v - 1)·0.5)
        as.SetAnimation(0, kWalk, true);
        Check(as.GetCurrent(0) != nullptr && Near(as.GetCurrent(0)->mix_duration, 0.5f),
              "walk 的 mixDuration = AnimationStateData 的 default_mix");
        Step(0.25f);
        Check(as.GetCurrent(0)->mixing_from != nullptr, "混合中：walk 的 mixingFrom = idle");
        Check(Near(root.rotation, 23.5f), "混合 50%：root.rotation = 22 + 3·0.5（最短弧）");
        Check(Near(root.x, 3.0f) && Near(root.y, -2.75f), "混合 50%：translate 以 setup 为基");
        Check(Near(root.scale_x, -3.0f) && Near(root.scale_y, 0.375f), "混合 50%：scale 混入取关键帧符号");
        Check(Near(root.shear_x, 9.5f) && Near(root.shear_y, -12.0f), "混合 50%：shear");
        Check(Near(hip.rotation, -15.0f), "混合 50%：hip.rotation = -30 + 30·0.5");
        {
            const float av = 1.0f + (0x80 / 255.0f - 1.0f) * 0.25f;
            const float* c = st.slots[0].color;
            Check(Near(c[0], 0.875f) && Near(c[1], 0.625f) && Near(c[2], 0.5f) && Near(c[3], 1.0f + (av - 1.0f) * 0.5f),
                  "混合 50%：s0 颜色从 setup 白色混过去");
        }

        // ---- 3) 再走 0.25 ⇒ mix = 1 ⇒ walk 走"轨道 0 且 mix == 1"的直通分支 ----
        // t=0.5：binarySearch 返回 t=1 那一帧，percent = 1 - (0.5-1)/(0.5-1) = 0 ⇒ r = 190；
        // First：r += 15 - 22 ⇒ 183 ⇒ wrap = -177 ⇒ rotation = 22 - 177 = -155
        Step(0.25f);
        Check(Near(root.rotation, -155.0f), "mix = 1：root.rotation = 22 + wrap(190 + 15 - 22)");

        // ---- 4) 再走 0.5：walk trackTime = 1.0 ⇒ loop 取模 = 0（stepped 段，值 10）----
        // updateMixingFrom 返回 finished ⇒ mixingFrom 被摘掉。
        // ⚠️ 局部角度**不归一化**：r = 10 + (15 - (-155)) = 180，wrap(180) = **-180**（截断取整），
        //    rotation = -155 - 180 = -335（≡ 25）。写成 setup + 10 = 25 会与参考实现的局部值对不上。
        Step(0.5f);
        Check(as.GetCurrent(0)->mixing_from == nullptr, "混合结束：mixingFrom 被摘掉");
        Check(Near(root.rotation, -335.0f), "loop 回到 0：root.rotation = -155 + wrap(180) = -335（≡ 25，不归一化）");

        // ---- 5) 轨道 1 叠一层 MixBlend_Add（alpha 0.5）----
        // 轨道 0：r = 10 + (15 - (-335)) = 360 ⇒ wrap = 0 ⇒ rotation 不变（-335）
        // 轨道 1（Add 走直通分支）：末帧 r = 7 ⇒ rotation += 7·0.5
        SpineGpuTrackEntry* e1 = as.SetAnimation(1, kIdle, true);
        Check(e1 != nullptr, "轨道 1 setAnimation 成功");
        if (e1 != nullptr)
        {
            e1->mix_blend = SpineGpuMixBlend::kAdd;
            e1->alpha = 0.5f;
        }
        Step(0.0f);
        Check(Near(root.rotation, -331.5f), "轨道 1 Add：root.rotation = -335 + 7·0.5");

        // ---- 6) addAnimation 的 delay 推算（AnimationState.cpp:549-561）----
        // 上一条 walk：loop、时长 1、trackTime 1.0 ⇒ delay = 1·(1 + (int)(1.0/1)) - mix(walk→idle) 0.5 = 1.5
        SpineGpuTrackEntry* q = as.AddAnimation(0, kIdle, false, 0.0f);
        Check(q != nullptr && Near(q->delay, 1.5f), "addAnimation(delay<=0)：delay = 时长·(1 + 已播圈数) - 混合时长");

        // ---- 7) setEmptyAnimation：mixDuration 与 trackEnd 都 = 给定值 ----
        SpineGpuTrackEntry* em = as.SetEmptyAnimation(2, 0.2f);
        Check(em != nullptr && em->animation == kSpineGpuEmptyAnimation && Near(em->mix_duration, 0.2f) &&
                  Near(em->track_end, 0.2f),
              "setEmptyAnimation：空动画、mixDuration = trackEnd = 0.2");
        Check(as.SetAnimation(0, 999u, true) == nullptr, "动画下标越界 ⇒ 返回 nullptr（不改状态）");

        // ---- 8) 事件（EventQueue）：入队点与派发顺序（AnimationState.cpp:203-271 / :842-902）----
        // 新建一份状态机，事件写进 sink。walk 时长 1、idle 时长 0。
        {
            std::vector<SpineGpuAnimationEvent> ev;
            SpineGpuSkeletonState st2;
            Check(SpineGpuInitSkeletonState(asset, data, st2).ok, "事件：初始化骨架状态");
            SpineGpuAnimationState es(asset, data, &asd);
            es.event_sink = &ev;
            const auto Is = [&](size_t i, SpineGpuEventType t, uint32_t anim) {
                return i < ev.size() && ev[i].type == t && ev[i].animation == anim && ev[i].track == 0u;
            };
            const auto EStep = [&](float dt) {
                es.Update(dt);
                Check(es.Apply(st2).ok, "事件：Apply 成功");
            };
            es.SetAnimation(0, kWalk, true);
            Check(ev.size() == 1 && Is(0, SpineGpuEventType::kStart, kWalk),
                  "setAnimation 立刻派发 start（setCurrent → queue.start → setAnimation 末尾 drain）");
            ev.clear();
            EStep(0.4f);
            Check(ev.empty(), "循环 walk 走到 0.4：没有 complete（fmod(0, 1) = 0 不大于 fmod(0.4, 1)）");
            EStep(0.7f);
            // update：trackLast = 0.4、trackTime = 1.1 ⇒ fmod(0.4, 1) = 0.4 > fmod(1.1, 1) = 0.1 ⇒ complete
            Check(ev.size() == 1 && Is(0, SpineGpuEventType::kComplete, kWalk), "循环跨过一圈 ⇒ 一条 complete");
            ev.clear();
            es.SetAnimation(0, kIdle, false);
            // walk 已应用过（nextTrackLast != -1）⇒ setCurrent(interrupt = true)：先 interrupt(from) 再 start(新)
            Check(ev.size() == 2 && Is(0, SpineGpuEventType::kInterrupt, kWalk) && Is(1, SpineGpuEventType::kStart, kIdle),
                  "打断：interrupt(walk) 在 start(idle) 之前");
            ev.clear();
            es.ClearTrack(0);
            // clearTrack：end(current) → 顺着 mixingFrom 链 end(from)；drain 时每条 end 贯穿成 end + dispose
            Check(ev.size() == 4 && Is(0, SpineGpuEventType::kEnd, kIdle) && Is(1, SpineGpuEventType::kDispose, kIdle) &&
                      Is(2, SpineGpuEventType::kEnd, kWalk) && Is(3, SpineGpuEventType::kDispose, kWalk),
                  "clearTrack：end(idle), dispose(idle), end(walk), dispose(walk)");
            ev.clear();
            es.SetAnimation(0, kIdle, false);
            ev.clear();
            EStep(0.1f);
            // 非循环、时长 0：animationTime 0 >= animationEnd 0 且 animationLast -1 < 0 ⇒ 第一次 apply 即 complete
            Check(ev.size() == 1 && Is(0, SpineGpuEventType::kComplete, kIdle), "非循环：到头时 complete 一次");
            ev.clear();
            EStep(0.1f);
            Check(ev.empty(), "非循环：之后不再重复 complete（animationLast 已 >= animationEnd）");
        }
        g_prefix.clear();
    }

    // =================================================================
    // 用户事件（v16：导入 → 烘焙 → 容器 → AnimationState 派发）。夹具见 SpineGpuEventFixture.h；
    // 期望值按参考实现手推（SkeletonJson.cpp:673-694 / :1172-1198、EventTimeline.cpp:63-103、
    // AnimationState.cpp:765 / :842-878）。与 spine-cpp 的随机脚本逐帧对拍在 SpineGpuRefParityTest。
    // =================================================================
    {
        g_prefix = "[events] ";
        SpineAtlas eatlas;
        SpineSourceSkeleton esrc;
        SpineGpuAsset baked;
        SpineGpuAsset ea;
        std::vector<uint8_t> ebytes;
        SpineImportResult er = ParseSpineAtlas(SpineGpuEventFixture::kAtlas, std::strlen(SpineGpuEventFixture::kAtlas), eatlas);
        if (er.ok)
        {
            er = ParseSpineSkeletonJson(SpineGpuEventFixture::kSkeleton, std::strlen(SpineGpuEventFixture::kSkeleton),
                                        &eatlas, esrc);
        }
        if (er.ok)
        {
            er = BakeSpineGpuAsset(esrc, baked);
        }
        if (er.ok)
        {
            er = WriteSpineGpuContainer(baked, ebytes);
        }
        if (er.ok)
        {
            er = ReadSpineGpuContainer(ebytes.data(), ebytes.size(), ea);
        }
        if (!er.ok)
        {
            std::printf("  error: %s\n", er.error.c_str());
        }
        Check(er.ok, "夹具解析 → 烘焙 → 写 → 读");
        if (er.ok)
        {
            const auto Str = [&](uint32_t i) { return i < ea.event_strings.size() ? ea.event_strings[i] : std::string("<oob>"); };
            Check((ea.header.flags & SpineGpuFlags::kHasEventTimeline) != 0u, "flags 带 kHasEventTimeline");
            Check(ea.events.size() == 4 && !ea.event_strings.empty() && ea.event_strings[0].empty(),
                  "4 个事件定义；字符串池第 0 条是空串");
            Check(ea.events == baked.events && ea.event_strings == baked.event_strings,
                  "容器往返：事件定义与字符串池逐条相同");
            if (ea.events.size() == 4)
            {
                const SpineGpuEventData& step = ea.events[0];
                const SpineGpuEventData& sfx = ea.events[1];
                const SpineGpuEventData& mute = ea.events[3];
                Check(Str(step.name) == "step" && step.int_value == 7 && step.float_value == 0.5f &&
                          Str(step.string_value) == "left" && Str(step.audio_path).empty() && step.volume == 1.0f,
                      "step：int 7 / float 0.5 / string 'left' / 无音频");
                Check(Str(sfx.name) == "sfx" && Str(sfx.audio_path) == "boom.wav" && sfx.volume == 0.8f &&
                          sfx.balance == -0.25f,
                      "sfx：audio 'boom.wav'、定义层 volume 0.8 / balance -0.25");
                Check(Str(ea.events[2].name) == "blank" && ea.events[2].int_value == 0 && ea.events[2].float_value == 0.0f,
                      "blank：全缺省");
                Check(Str(mute.name) == "mute" && Str(mute.audio_path).empty() && mute.volume == 0.3f && mute.balance == 0.6f,
                      "mute：`\"audio\": \"\"` ⇒ 定义层照读 volume 0.3 / balance 0.6（参考实现判的是指针非空）");
            }
            const auto Dur = [&](size_t a) { return a < ea.anims.size() ? ea.anims[a].duration : -1.0f; };
            // json 里动画的顺序：walk, late, solo, burst
            Check(ea.anims.size() == 4 && Dur(0) == 1.0f && Dur(1) == 0.8f && Dur(2) == 0.25f && Dur(3) == 0.6f,
                  "duration 把末个事件算进去：walk 1.0 / late 0.8（骨骼只到 0.5）/ solo 0.25（只有事件）/ burst 0.6");
            // 时间轴载荷：late 的两帧（sfx 带音频 ⇒ 帧层缺省 1 / 0，不继承 0.8 / -0.25；mute 无音频 ⇒ 不读帧上的值）
            const auto KeyOf = [&](uint32_t anim, uint32_t frame, SpineGpuEventKey& k) {
                const SpineGpuAnim& a = ea.anims[anim];
                for (uint32_t t = 0; t < a.timeline_count; ++t)
                {
                    const SpineGpuTimeline& tl = ea.timelines[a.timeline_first + t];
                    if (static_cast<SpineTimelineType>(tl.type) == SpineTimelineType::kEvent && frame < tl.payload_count)
                    {
                        std::memcpy(&k, ea.timeline_payload.data() + tl.payload_offset + frame * sizeof(SpineGpuEventKey),
                                    sizeof(k));
                        return true;
                    }
                }
                return false;
            };
            SpineGpuEventKey k0;
            SpineGpuEventKey k1;
            SpineGpuEventKey w1;
            if (ea.anims.size() == 4 && KeyOf(1, 0, k0) && KeyOf(1, 1, k1) && KeyOf(0, 1, w1))
            {
                Check(k0.event == 1 && k0.volume == 1.0f && k0.balance == 0.0f,
                      "late 帧 0（sfx，帧上没写）：volume / balance = Event 构造函数的 1 / 0，不继承定义的 0.8 / -0.25");
                Check(k1.event == 3 && k1.volume == 1.0f && k1.balance == 0.0f,
                      "late 帧 1（mute，定义的 audio 为空）：帧上写了 0.1 / 0.2 也不读 ⇒ 1 / 0");
                Check(w1.event == 0 && w1.int_value == -3 && w1.float_value == 0.5f && Str(w1.string_value) == "right",
                      "walk 帧 1（step）：int 覆盖成 -3、float 继承定义 0.5、string 覆盖成 'right'");
            }
            else
            {
                Check(false, "找得到 late / walk 的事件时间轴");
            }

            // ---- 运行时派发 ----
            SpineGpuRuntimeData edata;
            Check(SpineGpuBuildRuntimeData(ea, edata).ok, "建查找表");
            SpineGpuAnimationStateData easd;
            easd.default_mix = 0.5f;
            const uint32_t kWalkE = 0, kSoloE = 2, kBurstE = 3;
            struct Run
            {
                SpineGpuSkeletonState st;
                std::vector<SpineGpuAnimationEvent> ev;
                std::unique_ptr<SpineGpuAnimationState> as;
            };
            const auto NewRun = [&](Run& r) {
                Check(SpineGpuInitSkeletonState(ea, edata, r.st).ok, "初始化骨架状态");
                r.as = std::make_unique<SpineGpuAnimationState>(ea, edata, &easd);
                r.as->event_sink = &r.ev;
            };
            const auto Step = [&](Run& r, float dt) {
                r.ev.clear();
                r.as->Update(dt);
                Check(r.as->Apply(r.st).ok, "Apply 成功");
            };
            // 第 i 条是名为 name 的用户事件（时刻 t），或是 type 类型的生命周期事件
            const auto IsUser = [&](const Run& r, size_t i, const char* name, float t) {
                return i < r.ev.size() && r.ev[i].type == SpineGpuEventType::kEvent && r.ev[i].user.event < ea.events.size() &&
                       Str(ea.events[r.ev[i].user.event].name) == name && r.ev[i].user.time == t;
            };
            const auto IsType = [&](const Run& r, size_t i, SpineGpuEventType t, uint32_t anim) {
                return i < r.ev.size() && r.ev[i].type == t && r.ev[i].animation == anim;
            };

            // 1) 循环 walk：首帧 lastTime = -1 ⇒ t=0 的事件触发；回绕时 (0.75, +∞) 的 t=1.0 排在 complete 之前、
            //    回绕后 [0, 0.25] 的 t=0 排在 complete 之后（queueEvents 的两段循环）
            {
                Run r;
                NewRun(r);
                r.as->SetAnimation(0, kWalkE, true);
                Step(r, 0.25f);
                Check(r.ev.size() == 1 && IsUser(r, 0, "step", 0.0f) && r.ev[0].user.int_value == 7 &&
                          Str(r.ev[0].user.string_value) == "left",
                      "walk 首帧：step@0（继承定义的 int 7 / 'left'）");
                Step(r, 0.5f);
                Check(r.ev.size() == 2 && IsUser(r, 0, "step", 0.5f) && r.ev[0].user.int_value == -3 &&
                          IsUser(r, 1, "sfx", 0.5f) && r.ev[1].user.volume == 0.4f && r.ev[1].user.balance == 0.75f,
                      "walk 0.25→0.75：同时刻两个事件按文件顺序（step -3，sfx vol 0.4 / bal 0.75）");
                Step(r, 0.5f);
                Check(r.ev.size() == 3 && IsUser(r, 0, "blank", 1.0f) && r.ev[0].user.float_value == 2.25f &&
                          IsType(r, 1, SpineGpuEventType::kComplete, kWalkE) && IsUser(r, 2, "step", 0.0f),
                      "walk 回绕 0.75→1.25：blank@1.0、complete、step@0（complete 前后两段）");
            }
            // 2) 非循环 burst：首帧同时刻两个、中段同时刻三个、到头只有 complete
            {
                Run r;
                NewRun(r);
                r.as->SetAnimation(0, kBurstE, false);
                Step(r, 0.1f);
                Check(r.ev.size() == 2 && IsUser(r, 0, "blank", 0.0f) && IsUser(r, 1, "step", 0.0f) &&
                          r.ev[1].user.int_value == 1,
                      "burst 首帧：t=0 的两个（blank、step int 1）");
                Step(r, 0.25f);
                Check(r.ev.size() == 3 && IsUser(r, 0, "step", 0.3f) && r.ev[0].user.int_value == 2 && IsUser(r, 1, "sfx", 0.3f) &&
                          IsUser(r, 2, "blank", 0.3f) && Str(r.ev[2].user.string_value) == "third",
                      "burst 0.1→0.35：t=0.3 的三个按文件顺序");
                Step(r, 0.5f);
                Check(r.ev.size() == 1 && IsType(r, 0, SpineGpuEventType::kComplete, kBurstE),
                      "burst 到头：只有 complete（lastTime 0.35 ≥ 末帧 0.3 ⇒ 不再收）");
            }
            // 3) 1 帧的 solo（循环，时长 0.25）：t=0.25 = animationEnd ⇒ 回绕那一帧收到，排在 complete 之前
            {
                Run r;
                NewRun(r);
                r.as->SetAnimation(0, kSoloE, true);
                Step(r, 0.1f);
                Step(r, 0.1f);
                Check(r.ev.empty(), "solo 0→0.2：还没到 0.25");
                Step(r, 0.1f);
                Check(r.ev.size() == 2 && IsUser(r, 0, "step", 0.25f) && r.ev[0].user.float_value == -1.5f &&
                          IsType(r, 1, SpineGpuEventType::kComplete, kSoloE),
                      "solo 0.2→0.3（回绕）：step@0.25（float -1.5）在 complete 之前");
            }
            // 4) eventThreshold（applyMixingFrom :765）：walk 被 solo 打断、混合 0.5 s；混到 mix = 0.6 那一帧，
            //    walk 回绕 0.75→1.05。threshold 0 ⇒ 不收 walk 的用户事件（complete 照样入队，因为 mixDuration > 0）；
            //    threshold 1 ⇒ 收，且全部挂在 walk 那条 entry 上、排在 solo 自己的事件之前
            for (int with_threshold = 0; with_threshold < 2; ++with_threshold)
            {
                Run r;
                NewRun(r);
                SpineGpuTrackEntry* walk = r.as->SetAnimation(0, kWalkE, true);
                if (walk != nullptr && with_threshold != 0)
                {
                    walk->event_threshold = 1.0f;
                }
                Step(r, 0.25f);
                Step(r, 0.5f);
                r.ev.clear();
                r.as->SetAnimation(0, kSoloE, true);
                Check(r.ev.size() == 2 && IsType(r, 0, SpineGpuEventType::kInterrupt, kWalkE) &&
                          IsType(r, 1, SpineGpuEventType::kStart, kSoloE),
                      "打断：interrupt(walk) → start(solo)");
                Step(r, 0.3f);
                if (with_threshold == 0)
                {
                    Check(r.ev.size() == 1 && IsType(r, 0, SpineGpuEventType::kComplete, kWalkE),
                          "threshold 0：混出的 walk 只有 complete，没有用户事件");
                }
                else
                {
                    Check(r.ev.size() == 3 && IsUser(r, 0, "blank", 1.0f) && r.ev[0].animation == kWalkE &&
                              IsType(r, 1, SpineGpuEventType::kComplete, kWalkE) && IsUser(r, 2, "step", 0.0f) &&
                              r.ev[2].animation == kWalkE,
                          "threshold 1（mix 0.6 < 1）：blank@1.0 / complete / step@0 都挂在 walk 上");
                }
            }
        }
        g_prefix.clear();
    }

    // =================================================================
    // 换肤（Skeleton::setSkin / getAttachment / Skin::attachAll，OSGame 3.8 分支）。
    // 夹具见 SpineGpuSkinFixture.h；期望值按参考实现的三条规则手推，不调被测函数自己算。
    // 与 spine-cpp 的随机脚本逐帧对拍在 SpineGpuRefParityTest（同一个夹具）。
    // =================================================================
    {
        g_prefix = "[skin] ";
        SpineAtlas katlas;
        SpineSourceSkeleton ksrc;
        SpineGpuAsset ka;
        SpineImportResult kr = ParseSpineAtlas(SpineGpuSkinFixture::kAtlas, std::strlen(SpineGpuSkinFixture::kAtlas), katlas);
        if (kr.ok)
        {
            kr = ParseSpineSkeletonJson(SpineGpuSkinFixture::kSkeleton, std::strlen(SpineGpuSkinFixture::kSkeleton), &katlas,
                                        ksrc);
        }
        if (kr.ok)
        {
            kr = BakeSpineGpuAsset(ksrc, ka);
        }
        SpineGpuRuntimeData kd;
        SpineGpuSkeletonState ks;
        if (kr.ok)
        {
            kr = SpineGpuBuildRuntimeData(ka, kd);
        }
        if (kr.ok)
        {
            kr = SpineGpuInitSkeletonState(ka, kd, ks);
        }
        Check(kr.ok, "夹具烘焙 + 查找表 + 初始状态");
        if (!kr.ok)
        {
            std::printf("  error: %s\n", kr.error.c_str());
        }
        uint32_t red = 0, blue = 0, nope = 0;
        const bool found = SpineGpuFindSkin(ka, "red", red) && SpineGpuFindSkin(ka, "blue", blue);
        Check(found && red == 1u && blue == 2u && !SpineGpuFindSkin(ka, "nope", nope), "findSkin：default=0 red=1 blue=2");
        if (kr.ok && found && ks.slots.size() == 6)
        {
            const auto item_skin = [&](uint32_t sl) -> long long {
                const uint32_t it = ks.slots[sl].attachment.draw_item;
                return it == SpineGpuAttachRef::kNone ? -1 : static_cast<long long>(ka.draw_items[it].skin_id);
            };
            Check(kd.has_default_skin, "有 default 皮肤（皮肤 0 名为 default）");
            // ---- 构造后：没有当前皮肤 ⇒ 只看 default ----
            Check(ks.skin == kSpineGpuNoSkin, "构造后当前皮肤为空");
            Check(ks.slots[0].attachment.skin == 0u && item_skin(0) == 0, "s0 body ← default 的 region");
            Check(ks.slots[2].attachment.name_id != SpineGpuAttachRef::kNone &&
                      ks.slots[2].attachment.skin == SpineGpuAttachRef::kNone &&
                      ks.slots[2].attachment.draw_item == SpineGpuAttachRef::kNone,
                  "s2 hat 只在 red / blue 里 ⇒ 无皮肤时解析不到（参考 = NULL），不画");
            // ---- setSkin(red)，旧皮肤为空 ⇒ 按 setup 名在 red 里找，找到才挂 ----
            Check(SpineGpuSetSkin(ka, kd, ks, red).ok && ks.skin == red, "setSkin(red)");
            Check(ks.slots[0].attachment.skin == red && item_skin(0) == static_cast<long long>(red),
                  "s0 body → red 的 mesh（同名不同类型）");
            Check(ks.slots[2].attachment.skin == red && item_skin(2) == static_cast<long long>(red), "s2 hat → red（从 NULL 挂上）");
            Check(ks.slots[1].attachment.skin == 0u, "s1 eye：red 没有 ⇒ 不动，仍是 default 的");
            Check(ks.slots[3].attachment.clip != SpineGpuAttachRef::kNone &&
                      ka.clip_polygons[ks.slots[3].attachment.clip].skin_id == red,
                  "s3 clip → red 的裁剪附件");
            // ---- setSkin(blue)，旧皮肤 red ⇒ attachAll：只换"当前挂的正是 red 那条"、且 blue 有同名的槽位 ----
            ks.slots[0].deform.assign(8, 1.0f);   // 换肤不动的槽位 deform 要留着
            ks.slots[1].deform.assign(8, 2.0f);
            Check(SpineGpuSetSkin(ka, kd, ks, blue).ok && ks.skin == blue, "setSkin(blue)");
            Check(ks.slots[0].attachment.skin == red && ks.slots[0].deform.size() == 8u,
                  "s0 body：blue 没有 ⇒ 仍挂 red 的（**不**回落 default），deform 保留");
            Check(ks.slots[2].attachment.skin == blue && item_skin(2) == static_cast<long long>(blue), "s2 hat：red → blue");
            Check(ks.slots[3].attachment.skin == red, "s3 clip：blue 没有 ⇒ 仍是 red 的");
            Check(ks.slots[1].attachment.skin == 0u && ks.slots[1].deform.size() == 8u,
                  "s1 eye：当前挂的是 default 的（不在 red 的条目里）⇒ blue 有也不换");
            // ---- setSlotsToSetupPose（当前 blue）：getAttachment 先 blue 再 default ----
            SpineGpuSetSlotsToSetupPose(ka, kd, ks);
            Check(ks.slots[1].attachment.skin == blue && item_skin(1) == static_cast<long long>(blue) &&
                      ks.slots[1].deform.empty(),
                  "s1 eye → blue 的 linked mesh，deform 清");
            Check(ks.slots[0].attachment.skin == 0u && item_skin(0) == 0 && ks.slots[0].deform.empty(),
                  "s0 body：blue 没有 ⇒ 回落 default（换了对象 ⇒ deform 清）");
            Check(ks.slots[3].attachment.skin == 0u, "s3 clip → default 的");
            // ---- setSkin(NULL)：附件一个都不动 ----
            Check(SpineGpuSetSkin(ka, kd, ks, kSpineGpuNoSkin).ok && ks.skin == kSpineGpuNoSkin &&
                      ks.slots[1].attachment.skin == blue && ks.slots[2].attachment.skin == blue,
                  "setSkin(NULL)：当前皮肤置空，槽位附件不动");
            Check(!SpineGpuSetSkin(ka, kd, ks, 99u).ok && ks.skin == kSpineGpuNoSkin, "越界皮肤 ⇒ 报错，状态不动");
            // ---- attachment 时间轴按当前皮肤解析：blink t=0.35 ⇒ s1 = eye_closed ----
            uint32_t blink = 0;
            for (uint32_t i = 0; i < ka.anim_names.size(); ++i)
            {
                blink = ka.anim_names[i] == "blink" ? i : blink;
            }
            SpineGpuSkeletonState k2;
            bool tl_ok = SpineGpuInitSkeletonState(ka, kd, k2).ok && SpineGpuSetSkin(ka, kd, k2, red).ok &&
                         SpineGpuApplyAnimation(ka, kd, k2, blink, 0.0f, 0.35f, false, 1.0f, SpineGpuMixBlend::kFirst,
                                                SpineGpuMixDirection::kIn)
                             .ok;
            Check(tl_ok && k2.slots[1].attachment.skin == red && item_skin(1) != -1,
                  "red 下 attachment 时间轴的 eye_closed → red 的那条");
            SpineGpuSkeletonState k3;
            tl_ok = SpineGpuInitSkeletonState(ka, kd, k3).ok &&
                    SpineGpuApplyAnimation(ka, kd, k3, blink, 0.0f, 0.35f, false, 1.0f, SpineGpuMixBlend::kFirst,
                                           SpineGpuMixDirection::kIn)
                        .ok;
            Check(tl_ok && k3.slots[1].attachment.skin == 0u, "无皮肤时同一帧 → default 的 eye_closed");
            // ---- 姿势：blue 的 path 曲线不同 ⇒ p1 摆出来的 b3 不同（换肤改骨骼）----
            SpineGpuPoseResult pn, pb;
            const bool pose_ok = SpineGpuSamplePose(ka, blink, 0.0f, pn).ok &&
                                 SpineGpuSamplePoseSkinned(ka, blue, blink, 0.0f, pb).ok;
            Check(pose_ok && pn.bones.size() == 4 && pb.bones.size() == 4 &&
                      (pn.bones[3].world_x != pb.bones[3].world_x || pn.bones[3].world_y != pb.bones[3].world_y),
                  "blue 的 path 附件 ⇒ b3 的世界位置变了");
            Check(pose_ok && pn.slot_draw_item[2] == SpineGpuPoseResult::kNoDrawItem &&
                      pb.slot_draw_item[2] != SpineGpuPoseResult::kNoDrawItem,
                  "s2 hat：无皮肤不画、blue 下画");
            // ---- deform 上界：每个槽位取所有皮肤里最长的 kFallback 项 ----
            SpineGpuBatchAssetInfo kinfo;
            SpineGpuBuildBatchAssetInfo(ka, kinfo);
            Check(kinfo.deform_capacity == 26u, "deform 上界 = s0 8 + s1 8 + s4 10（weighted arm 5 个影响）");
            if (kinfo.deform_capacity != 26u)
            {
                std::printf("  deform_capacity = %zu\n", kinfo.deform_capacity);
            }
        }
        g_prefix.clear();
    }

    // =================================================================
    // 多实例拼包（SpineGpuBatch）+ 摆放。布局数值手算；拼包按"排在别的实例后面 == 单独成包"逐位比
    // （SpineGpuEmulateVertex = spine_gpu.vert 的 CPU 版）。全量 781 资产版本在 SpineGpuRefParityTest。
    // =================================================================
    {
        g_prefix = "[multi-instance] ";
        auto Near = [](float got, float want) { return std::fabs(got - want) < 1e-6f; };
        // ---- 网格：列数 = round(sqrt(n · 宽高比))，行从上往下 ----
        SpineGpuRect c = SpineGpuGridCell(1, 0, 800.0f, 600.0f);
        Check(c.x == 0.0f && c.y == 0.0f && c.w == 800.0f && c.h == 600.0f, "1 个实例 ⇒ 整个视口");
        c = SpineGpuGridCell(2, 1, 800.0f, 600.0f);   // sqrt(2.67) = 1.63 → 2 列 1 行
        Check(c.x == 400.0f && c.y == 0.0f && c.w == 400.0f && c.h == 600.0f, "2 个 / 4:3 ⇒ 左右两格，第 1 个在右");
        c = SpineGpuGridCell(4, 3, 800.0f, 600.0f);   // sqrt(5.33) = 2.31 → 2 列 2 行
        Check(c.x == 400.0f && c.y == 300.0f && c.w == 400.0f && c.h == 300.0f, "4 个 ⇒ 2×2，第 3 个在右下");
        // ---- 自动摆放：bounds (-10,-20)-(30,20)，视口 800×600，Vulkan y 取负 ----
        //   fit = 0.8·min(800/40, 600/40) = 12；sx = 24/800 = 0.03；sy = -24/600 = -0.04；
        //   中心 (10, 0) ⇒ tx = -0.3、ty = 0
        const float bounds[4] = {-10.0f, -20.0f, 30.0f, 20.0f};
        SpineGpuGpuInst gi = SpineGpuFitInstance(bounds, SpineGpuGridCell(1, 0, 800.0f, 600.0f), 800.0f, 600.0f, -1.0f);
        Check(Near(gi.clip_from_world[0], 0.03f) && Near(gi.clip_from_world[5], -0.04f) &&
                  Near(gi.clip_from_world[12], -0.3f) && gi.clip_from_world[13] == 0.0f &&
                  gi.clip_from_world[15] == 1.0f && gi.root_abcd[0] == 1.0f && gi.root_abcd[3] == 1.0f,
              "单实例自动摆放 = 多实例之前的居中 80%");
        //   右半格（中心 x = 600 ⇒ 偏 +200 像素）：fit = 0.8·min(400/40, 600/40) = 8；sx = 0.02；
        //   tx = -10·0.02 + 2·200/800 = 0.3；y 不偏
        gi = SpineGpuFitInstance(bounds, SpineGpuGridCell(2, 1, 800.0f, 600.0f), 800.0f, 600.0f, -1.0f);
        Check(Near(gi.clip_from_world[0], 0.02f) && Near(gi.clip_from_world[12], 0.3f) &&
                  Near(gi.clip_from_world[13], 0.0f),
              "第 2 格：缩放按格子、平移到格子中心");
        // ---- 手动摆放：(100, 50) 像素、2 像素 / 单位，DX12（y 向上）----
        gi = SpineGpuPlaceInstance(100.0f, 50.0f, 2.0f, 800.0f, 600.0f, 1.0f);
        Check(Near(gi.clip_from_world[0], 0.005f) && Near(gi.clip_from_world[5], 4.0f / 600.0f) &&
                  Near(gi.clip_from_world[12], 0.25f) && Near(gi.clip_from_world[13], 100.0f / 600.0f),
              "手动摆放：原点在视口中心偏 (100, 50) 像素");

        // ---- 拼包：裁剪夹具（rigid region + weighted mesh，有动画）的一个姿势；第二个实例的骨骼整体平移，
        //      保证两个实例的骨骼表不同（bone_base 错了才看得出来）。裁剪交给全量对拍（这里 clip = 空）。
        SpineSourceSkeleton msrc;
        SpineGpuAsset ma;
        const std::string mjson = BuildClipDeformOnClip();
        SpineImportResult mr = ParseSpineSkeletonJson(mjson.data(), mjson.size(), &atlas, msrc);
        if (mr.ok)
        {
            mr = BakeSpineGpuAsset(msrc, ma);
        }
        SpineGpuPoseResult p1, p2;
        if (mr.ok)
        {
            mr = SpineGpuSamplePose(ma, 0, 0.1f, p1);
        }
        if (mr.ok)
        {
            p2 = p1;
            for (SpineGpuBoneSample& b : p2.bones)
            {
                b.world_x += 7.0f;
                b.world_y -= 3.0f;
            }
        }
        Check(mr.ok, "夹具烘焙 + 两个姿势采样成功");
        if (!mr.ok)
        {
            std::printf("  error: %s\n", mr.error.c_str());
        }
        if (mr.ok)
        {
            SpineGpuBatchAssetInfo info;
            SpineGpuBuildBatchAssetInfo(ma, info);
            const SpineGpuGpuInst ia = SpineGpuPlaceInstance(10.0f, -20.0f, 0.5f, 800.0f, 600.0f, -1.0f);
            const SpineGpuGpuInst ib = SpineGpuPlaceInstance(-100.0f, 40.0f, 1.25f, 800.0f, 600.0f, -1.0f);
            SpineGpuBatch single, combined;
            SpineGpuBatchRange ra, rb, ra2;
            const bool ok1 = SpineGpuAppendBatchInstance(ma, info, p1, nullptr, ia, false, single, ra).ok;
            const bool ok2 = SpineGpuAppendBatchInstance(ma, info, p2, nullptr, ib, false, combined, rb).ok &&
                             SpineGpuAppendBatchInstance(ma, info, p1, nullptr, ia, false, combined, ra2).ok;
            Check(ok1 && ok2 && ra.draw_count > 0, "两个实例都追加成功、有可画项");
            Check(ra2.draw_first == rb.draw_count && ra2.draw_count == ra.draw_count, "第 2 个实例的区间紧接第 1 个");
            Check(combined.bones.size() == 2u * ma.header.bone_count && combined.insts.size() == 2u,
                  "骨骼表 / 实例变换首尾相接");
            bool offs = ra2.draw_count > 0;
            for (uint32_t k = 0; k < ra2.draw_count && offs; ++k)
            {
                const SpineGpuGpuDraw& d = combined.draws[ra2.draw_first + k];
                offs = d.bone_base == ma.header.bone_count && d.inst_index == 1u;
            }
            Check(offs, "第 2 个实例的 bone_base = 骨骼数、inst_index = 1");
            bool same = ok1 && ok2;
            size_t verts = 0;
            for (uint32_t k = 0; k < ra.draw_count && same; ++k)
            {
                const SpineGpuBatchCmd& cmd = single.cmds[ra.draw_first + k];
                for (uint32_t q = 0; q < cmd.index_count && same; ++q)
                {
                    const uint32_t vi = ma.index_buffer[cmd.index_first + q];
                    SpineGpuEmulatedVertex e1, e2;
                    same = SpineGpuEmulateVertex(ma, single, ra.draw_first + k, vi, e1) &&
                           SpineGpuEmulateVertex(ma, combined, ra2.draw_first + k, vi, e2) &&
                           std::memcmp(&e1, &e2, sizeof(e1)) == 0;
                    ++verts;
                }
            }
            Check(same && verts > 0, "排在别的实例后面的每个顶点与单独成包逐位相同");
            // 换掉第 2 个实例的骨骼（= 着色器忘了加 bone_base 时读到的东西）⇒ 必须不同：检查本身有判别力
            SpineGpuBatch wrong = combined;
            for (SpineGpuGpuDraw& d : wrong.draws)
            {
                d.bone_base = 0;
            }
            bool differs = false;
            for (uint32_t k = 0; k < ra2.draw_count && !differs; ++k)
            {
                const SpineGpuBatchCmd& cmd = combined.cmds[ra2.draw_first + k];
                for (uint32_t q = 0; q < cmd.index_count && !differs; ++q)
                {
                    const uint32_t vi = ma.index_buffer[cmd.index_first + q];
                    SpineGpuEmulatedVertex e1, e2;
                    differs = SpineGpuEmulateVertex(ma, combined, ra2.draw_first + k, vi, e1) &&
                              SpineGpuEmulateVertex(ma, wrong, ra2.draw_first + k, vi, e2) &&
                              std::memcmp(&e1, &e2, sizeof(e1)) != 0;
                }
            }
            Check(differs, "反例：bone_base 置 0 ⇒ 顶点变了（两个姿势不同，检查有判别力）");
            // ---- 失败回滚：骨骼数不对的姿势 ⇒ 报错且合包原样 ----
            SpineGpuPoseResult bad = p1;
            bad.bones.pop_back();
            const size_t nb = combined.bones.size(), nd = combined.draws.size(), ni = combined.insts.size();
            SpineGpuBatchRange rbad;
            const SpineImportResult br = SpineGpuAppendBatchInstance(ma, info, bad, nullptr, ia, false, combined, rbad);
            Check(!br.ok && combined.bones.size() == nb && combined.draws.size() == nd && combined.insts.size() == ni &&
                      rbad.draw_count == 0,
                  "坏姿势 ⇒ 报错、合包不变（别的实例照画）");
            // ---- 越界寻址 ⇒ 仿真报 false（GPU 上是静默读错）----
            SpineGpuBatch oob = single;
            oob.draws[ra.draw_first].inst_index = 7;
            SpineGpuEmulatedVertex eo;
            const SpineGpuBatchCmd& c0 = single.cmds[ra.draw_first];
            Check(!SpineGpuEmulateVertex(ma, oob, ra.draw_first, ma.index_buffer[c0.index_first], eo),
                  "inst_index 越界 ⇒ 仿真返回 false");
        }
        g_prefix.clear();
    }

    // =================================================================
    // tint black（双色）的绘制参数 + 片元公式（SpineGpuTintBlack.h ↔ spine_gpu.frag）
    // 参考：Spine-SkeletonGraphic-TintBlack.shader（_Color = 1、_Black = 0）
    //   out = tex·vc + ((1 - tex.rgb)·dark·tex.a·vc.a, 0)，tex 预乘、vc = (rgb·a, a)
    // 数值手算，不调被测公式自己。
    // =================================================================
    {
        g_prefix = "[tint black] ";
        auto Near = [](float got, float want) { return std::fabs(got - want) < 1e-6f; };
        const float dark[4] = {0.25f, -0.5f, 1.5f, 0.0f};
        const float zero[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        SpineGpuTintBlackParams tb = SpineGpuMakeTintBlackParams(dark, false, false);
        Check(!tb.enabled, "开关关 ⇒ 不加暗色项（OSGame 默认）");
        tb = SpineGpuMakeTintBlackParams(dark, true, true);
        Check(!tb.enabled, "加色槽位 ⇒ 不加暗色项（PMA 顶点色 alpha = 0 ⇒ 项恒为 0）");
        tb = SpineGpuMakeTintBlackParams(zero, true, false);
        Check(!tb.enabled, "暗色 rgb 全 0 ⇒ 不加（dark.a 不参与）");
        tb = SpineGpuMakeTintBlackParams(dark, true, false);
        Check(tb.enabled && tb.dark[0] == 0.25f && tb.dark[1] == -0.5f && tb.dark[2] == 1.5f,
              "开 + 普通槽位 ⇒ 暗色 rgb **原样**（负数 / 大于 1 都不截断，参考走 float UV 通道）");

        // tex 直 alpha (0.2, 0.6, 1.0, 0.5) ⇒ 预乘 (0.1, 0.3, 0.5, 0.5)；tint (1, 0.5, 0.25, 0.8)
        //   基础项 rgb = tex_pm · tint.rgb · tint.a = (0.08, 0.12, 0.1)，a = 0.5·0.8 = 0.4
        //   暗色项 = (1 - tex_pm.rgb) · dark · tex.a · tint.a = (0.9·0.25, 0.7·(-0.5), 0.5·1.5) · 0.4
        //          = (0.09, -0.14, 0.3)
        const float tex[4] = {0.2f, 0.6f, 1.0f, 0.5f};
        const float tint[4] = {1.0f, 0.5f, 0.25f, 0.8f};
        float out[4];
        SpineGpuShadeReference(tex, tint, false, tb, out);
        Check(Near(out[0], 0.08f + 0.09f) && Near(out[1], 0.12f - 0.14f) && Near(out[2], 0.1f + 0.3f) &&
                  Near(out[3], 0.4f),
              "公式：rgb = (0.17, -0.02, 0.4)、a = 0.4（负暗色把 g 压到 0 以下，与参考一致不夹）");
        SpineGpuShadeReference(tex, tint, false, SpineGpuTintBlackParams {}, out);
        Check(Near(out[0], 0.08f) && Near(out[1], 0.12f) && Near(out[2], 0.1f) && Near(out[3], 0.4f),
              "公式：关掉暗色 ⇒ 退化为原来的 tex × 槽位色");
        // 白像素（tex.rgb = 1）上暗色项为 0：tint black 只染"黑"不染"白"
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        SpineGpuShadeReference(white, tint, false, tb, out);
        Check(Near(out[0], 0.8f) && Near(out[1], 0.4f) && Near(out[2], 0.2f), "公式：白像素不受暗色影响");
        g_prefix.clear();
    }

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
        const std::vector<uint8_t> skel =
            // ⚠️ with_path / with_constraints **必须都开**：json 夹具 kSkeleton 带 path1 附件与
            //    ik/tf/path 三条约束，.skel 少一样就会在 chunk 18/19/20 上分叉，
            //    而"少几个 chunk"不报错 —— 只有这里的逐字段对拍能抓到。
            BuildSkelFixture(false, false, "3.8.99", nonessential, "mesh", false, false, true, 10, false,
                             2, 1, false, 3, false, true, "mesh", true, true, true);
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

        // ⚠️ 夹具的 s1 带 dark（0x204060FF：四分量不全为 0xff）⇒ 3.8 判据成立。
        //    这条断言的意义不只是"标志置上了"，更是**两条路径的判据一致**：
        //    json 那边的判据是"有没有 'dark' 这个键"，二进制这边是"四个字节不全为 ff"。
        Check(skel_src.uses_tint_black, "skel: s1 的 dark 不全 0xff ⇒ 开 tint black（与 json 路径一致）");
        Check(src.uses_tint_black, "json: s1 写了 'dark' ⇒ 开 tint black");
        if (skel_src.slots.size() == 2 && src.slots.size() == 2)
        {
            // dark 的 alpha **恒为 1**（参考实现写死），而读进来的第 4 个字节不落地 ⇒
            // 两条路径必须都拿到 (0x20,0x40,0x60)/255 与 alpha 1，逐值相同。
            for (int k = 0; k < 4; ++k)
            {
                CheckEq(static_cast<long long>(static_cast<int>(
                            skel_src.slots[1].color[k] * 255.0f + 0.5f)),
                        static_cast<long long>(static_cast<int>(src.slots[1].color[k] * 255.0f + 0.5f)),
                        "skel↔json: s1 的 setup 颜色逐分量相同");
            }
            CheckEq(static_cast<long long>(static_cast<int>(skel_src.slots[1].dark[0] * 255.0f + 0.5f)), 0x20LL,
                    "skel: s1 的 dark.r = 0x20/255");
            CheckEq(static_cast<long long>(static_cast<int>(skel_src.slots[1].dark[3] * 255.0f + 0.5f)), 0xFFLL,
                    "skel: s1 的 dark.a 恒为 1（参考实现写死，不是读进来的第 4 个字节）");
            Check(skel_src.slots[1].has_dark && src.slots[1].has_dark, "skel↔json: s1 都判成有暗色");
            Check(!skel_src.slots[0].has_dark && !src.slots[0].has_dark, "skel↔json: s0 **没有**暗色");
            Check(skel_src.slots[0].attachment_name == src.slots[0].attachment_name &&
                      skel_src.slots[0].attachment_name == "quad",
                  "skel↔json: s0 的 setup 附件名是 'quad'（attachment 时间轴的复位基线）");
        }
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
            // 二进制的骨骼字段顺序是 rotation,x,y,scaleX,scaleY,shearX,shearY,length，
            // json 的书写顺序是 length,x,y,rotation,… ⇒ 顺序写反了这里会红。
            Check(skel_asset.bones == asset.bones,
                  "skel↔json：骨骼 setup **逐字节相同**（二进制字段顺序 vs json 默认值）");
            Check(skel_asset.rigid_vertex_count == asset.rigid_vertex_count, "skel↔json: rigid 顶点数");
            Check(skel_asset.weighted_vertex_count == asset.weighted_vertex_count, "skel↔json: weighted 顶点数");
            Check(skel_asset.index_buffer == asset.index_buffer, "skel↔json: 索引缓冲相同");
            Check(skel_asset.influence_bind == asset.influence_bind, "skel↔json: 影响流 bindPos 逐字节相同");
            Check(skel_asset.influence_bone == asset.influence_bone, "skel↔json: 影响流骨骼/权重逐字节相同");
            Check(skel_asset.uv_buffer == asset.uv_buffer, "skel↔json: UV 通道逐字节相同");
            Check(skel_asset.slot_bone == asset.slot_bone, "skel↔json: slot_bone 相同");
            // ---- 动画（chunk 11-16）：两条路径各解析一遍，必须**逐字段相同** ----
            // 这是本文件里唯一能同时验证"json 字段顺序"与"二进制字段顺序"的交叉点：
            // rotate 在 json 里叫 "angle"、在二进制里是第 2 个 float；color 在 json 里
            // 是 hex 串、在二进制里是大端 int —— 任何一边读错，这里就会红。
            Check(skel_asset.anims == asset.anims, "skel↔json: 动画目录逐字段相同");
            Check(skel_asset.timelines == asset.timelines, "skel↔json: 时间轴表逐字段相同");
            Check(skel_asset.frames == asset.frames, "skel↔json: 帧数据逐值相同");
            Check(skel_asset.curve_types == asset.curve_types, "skel↔json: 曲线类型逐段相同");
            Check(skel_asset.beziers == asset.beziers, "skel↔json: bezier 控制点逐值相同");
            Check(skel_asset.timeline_payload == asset.timeline_payload,
                  "skel↔json: 变长载荷逐字节相同");
            Check(skel_asset.anim_names == asset.anim_names, "skel↔json: 动画名相同");
            Check(skel_asset.anim_attachment_names == asset.anim_attachment_names,
                  "skel↔json: 动画附件名相同");
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

            // ---- 约束（chunk 18-20，v11）：两条路径各解析一遍，必须**逐字段相同** ----
            // ⚠️ 这里的价值不只是"值对"，而是**两条路径的编解码口径一致**：
            //   · IK 的 bendDirection 在二进制里是 **sbyte**，json 里是 ±1；
            //   · transform 的字段顺序两边不同（json 是 rotate/translate/scale/shear 四段嵌套，
            //     二进制是 rotation,x,y,scaleX,scaleY,shearY 一串 float）；
            //   · path 的三个 mode 在 json 里是**字符串**（缺键回落默认值），
            //     二进制里是 varint 枚举 —— 任何一边口径错，这里就会红。
#ifndef NDEBUG
            if (g_verbose_v11)
            {
                for (int side = 0; side < 2; ++side)
                {
                    const SpineGpuAsset& a = side ? skel_asset : asset;
                    std::printf("  [%s] iks=%zu tf=%zu path=%zu pool=%zu paa=%zu pv=%zu pl=%zu\n",
                                side ? "skel" : "json", a.iks.size(), a.transforms.size(), a.paths.size(),
                                a.constraint_bone_pool.size(), a.path_attachments.size(),
                                a.path_vertices.size(), a.path_lengths.size());
                    for (const SpineGpuIkConstraint& k : a.iks)
                    {
                        std::printf("    ik order=%u n=%u first=%u tgt=%u mix=%g bend=%d flags=%u\n",
                                    k.order, k.bone_count, k.bone_first, k.target,
                                    static_cast<double>(k.mix), k.bend_dir, k.flags);
                    }
                    for (const SpineGpuTransformConstraint& t : a.transforms)
                    {
                        std::printf("    tf order=%u n=%u first=%u tgt=%u rot=%g x=%g y=%g sx=%g sy=%g shy=%g"
                                    " rm=%g tm=%g sm=%g shm=%g flags=%u pad=%u\n",
                                    t.order, t.bone_count, t.bone_first, t.target,
                                    static_cast<double>(t.offset_rotation), static_cast<double>(t.offset_x),
                                    static_cast<double>(t.offset_y), static_cast<double>(t.offset_scale_x),
                                    static_cast<double>(t.offset_scale_y), static_cast<double>(t.offset_shear_y),
                                    static_cast<double>(t.rotate_mix), static_cast<double>(t.translate_mix),
                                    static_cast<double>(t.scale_mix), static_cast<double>(t.shear_mix),
                                    t.flags, t.pad);
                    }
                    for (const SpineGpuPathConstraint& p : a.paths)
                    {
                        std::printf("    path order=%u n=%u first=%u slot=%u pm=%u sm=%u rm=%u"
                                    " rot=%g pos=%g sp=%g rmix=%g tmix=%g pidx=%u\n",
                                    p.order, p.bone_count, p.bone_first, p.target_slot,
                                    p.position_mode, p.spacing_mode, p.rotate_mode,
                                    static_cast<double>(p.offset_rotation), static_cast<double>(p.position),
                                    static_cast<double>(p.spacing), static_cast<double>(p.rotate_mix),
                                    static_cast<double>(p.translate_mix), p.path_index);
                    }
                    for (const SpineGpuPathAttachment& p : a.path_attachments)
                    {
                        std::printf("    pAtt slot=%u skin=%u flags=%u pad=%u vc=%u bo=%u bc=%u vo=%u vfc=%u"
                                    " lo=%u lc=%u\n",
                                    p.slot_index, p.skin_index, p.flags, p.pad, p.vertex_count,
                                    p.bone_offset, p.bone_count, p.vertex_offset, p.vertex_float_count,
                                    p.length_offset, p.length_count);
                    }
                    std::printf("    steps=");
                    for (const SpineGpuUpdateStep& s : a.update_steps)
                    {
                        std::printf("(%u,%u) ", s.kind, s.index);
                    }
                    std::printf(" reset=");
                    for (uint16_t b : a.update_reset)
                    {
                        std::printf("%u ", b);
                    }
                    std::printf("\n");
                }
            }
#endif
            Check(skel_asset.iks == asset.iks, "skel↔json: IK 约束逐字段相同（含 sbyte bendDirection）");
            Check(skel_asset.transforms == asset.transforms,
                  "skel↔json: transform 约束逐字段相同（二进制 float 顺序 vs json 嵌套键）");
            Check(skel_asset.paths == asset.paths,
                  "skel↔json: path 约束逐字段相同（mode 枚举 vs json 字符串）");
            Check(skel_asset.constraint_bone_pool == asset.constraint_bone_pool,
                  "skel↔json: 约束骨骼池相同（IK 两条 / tf 一条 / path 一条，共 4）");
            Check(skel_asset.update_steps == asset.update_steps,
                  "skel↔json: 执行顺序逐字段相同（updateCache 的结果，跨三类约束的共享 order 排序）");
            Check(skel_asset.update_reset == asset.update_reset, "skel↔json: 复位骨骼列表相同");
            Check(skel_asset.path_attachments == asset.path_attachments,
                  "skel↔json: PathAttachment 记录逐字段相同（含 skin_index）");
            Check(skel_asset.path_bone_pool == asset.path_bone_pool, "skel↔json: path 骨骼池相同");
            Check(skel_asset.path_vertices == asset.path_vertices,
                  "skel↔json: path 顶点逐值相同（二进制 weighted 标记 vs json 的 verticesLength 推断）");
            Check(skel_asset.path_lengths == asset.path_lengths,
                  "skel↔json: path lengths 逐值相同（条数 = vertex_count / 3）");

            // ⚠️ 三条"错了不报错"的坑，各钉一颗钉子（值本身由上面的逐字段比较保证，
            //    这里钉的是**非空**：空数组时上面的比较会"双空相同"，什么都证明不了）。
            Check(asset.iks.size() == 1 && asset.transforms.size() == 1 && asset.paths.size() == 1,
                  "json: 三条约束各 1 条（否则上面的逐字段比较是「双空相同」，证明不了任何事）");
            Check(asset.update_steps.size() > static_cast<size_t>(asset.header.bone_count),
                  "json: 执行顺序步数 > 骨骼数（否则等于没把约束排进去，updateCache 是空的）");
            Check(asset.path_attachments.size() == 1 && asset.path_vertices.size() == 6 &&
                      asset.path_lengths.size() == 1,
                  "json: path 几何非空（3 顶点 × 2 float = 6，lengths = 3/3 = 1）");
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

        // ---- S3: drawOrder 解算 ----
        // 夹具的两帧：t=0 的 offsetCount=0（⇒ setup 序），t=0.5 把 s0 挪到 +1（⇒ 对调）。
        // 期望值手算（照 SkeletonBinary.cpp:1040-1072 的重建算法推一遍）：
        //   slotCount=2, offsetCount=1, (slot 0, offset +1)
        //   drawOrder=[-1,-1] → drawOrder[0+1]=0, originalIndex=1
        //   → unchanged=[1] → 从后往前填空洞：drawOrder[0]=1
        //   ⇒ [1, 0]（两个槽位**对调**，不是"s0 挪到下标 1、s1 补到下标 0"的巧合版）
        if (r_do.ok)
        {
            SpineGpuAsset a_do;
            const SpineImportResult rb = BakeSpineGpuAsset(s_do, a_do);
            Check(rb.ok, "drawOrder: bake 成功");
            if (rb.ok)
            {
                std::vector<uint8_t> bytes;
                Check(WriteSpineGpuContainer(a_do, bytes).ok, "drawOrder: 序列化成功");
                SpineGpuAsset back_do;
                const SpineImportResult rr = ReadSpineGpuContainer(bytes.data(), bytes.size(), back_do);
                Check(rr.ok, "drawOrder: 读回成功");

                SpineGpuSlotSampleResult sd;
                const auto sample_do = [&](float t) {
                    const SpineImportResult r = SpineGpuSampleSlots(back_do, 0, t, sd);
                    if (!r.ok)
                    {
                        std::printf("  error: %s\n", r.error.c_str());
                    }
                    Check(r.ok, (std::string("drawOrder: 采样 t=") + std::to_string(t) + " 成功").c_str());
                    return r.ok;
                };
                if (sample_do(-1.0f))
                {
                    Check(sd.draw_order.size() == 2 && sd.draw_order[0] == 0 && sd.draw_order[1] == 1,
                          "drawOrder: t<首帧 ⇒ 复位成 setup 序 0,1（DrawOrderTimeline.cpp:75-83）");
                }
                if (sample_do(0.0f))
                {
                    Check(sd.draw_order.size() == 2 && sd.draw_order[0] == 0 && sd.draw_order[1] == 1,
                          "drawOrder: t=0 的 offsetCount=0 ⇒ 仍是 setup 序（:87-92）");
                }
                if (sample_do(0.5f))
                {
                    Check(sd.draw_order.size() == 2 && sd.draw_order[0] == 1 && sd.draw_order[1] == 0,
                          "drawOrder: t=0.5 ⇒ [1,0]（s0 挪 +1，未列出的 s1 **从后往前**补进空洞）");
                }
                if (sample_do(1.0f))
                {
                    Check(sd.draw_order.size() == 2 && sd.draw_order[0] == 1 && sd.draw_order[1] == 0,
                          "drawOrder: t>=末帧 ⇒ 停在末帧 [1,0]");
                }
            }
        }
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

    // ---- 9e. 动画的报错路径 ----
    // 每一条都是"参考实现静默、我们报错"：静默的后果不是崩溃，而是**动错了还不报错**
    // （动到另一条约束、曲线变成 linear、下标留在 0）—— 全都是查不出来的那类。
    {
        auto AnimJson = [](const std::string& anims) {
            return std::string(R"({"skeleton":{"spine":"3.8.99"},"bones":[{"name":"root"}],)"
                               R"("slots":[{"name":"s0","bone":"root"}],"skins":{"default":{}},)"
                               // ⚠️ 末尾是 `)")`：第一个 `)` 与前引号组成原始串结束符 `)"`，
                               // 第二个 `)` 才是 `std::string(` 的收口。而下面 kBads 里的 `const char*`
                               // 条目**没有**这层括号 ⇒ 只能写成 `)"`，多写一个 `"` 就会被吞进内容。
                               R"("animations":)") + anims + "}";
        };
        struct AnimBad
        {
            const char* name;
            const char* anims;
            bool parse_fails;   // false ⇒ 解析过、但 **bake** 必须失败
        };
        const AnimBad kBads[] = {
            {"骨骼时间轴类型不认识", R"({"walk":{"bones":{"root":{"nope":[{"time":0}]}}}})", true},
            {"槽位时间轴类型不认识", R"({"walk":{"slots":{"s0":{"nope":[{"time":0}]}}}})", true},
            {"骨骼名不存在", R"({"walk":{"bones":{"nope":{"rotate":[{"time":0,"angle":1}]}}}})", true},
            {"槽位名不存在", R"({"walk":{"slots":{"nope":{"color":[{"time":0,"color":"ffffffff"}]}}}})", true},
            // ⚠️ 参考实现 `findIkConstraint()` 返回 NULL 后把下标**留在 0** ⇒ 动到第一条约束
            {"ik 约束名不存在", R"({"walk":{"ik":{"noSuch":[{"time":0,"mix":1}]}}})", true},
            {"path 约束名不存在", R"({"walk":{"paths":{"noSuch":{"position":[{"time":0,"position":0}]}}}})", true},
            {"curve 串不认识", R"({"walk":{"bones":{"root":{"rotate":[{"time":0,"angle":1,"curve":"bogus"},)"
                               R"({"time":1,"angle":2}]}}}})", true},
            {"curve 数组不是 4 元", R"({"walk":{"bones":{"root":{"rotate":[{"time":0,"angle":1,"curve":[1,2,3]}]}}}})",
             true},
            {"drawOrder 的槽位不存在", R"({"walk":{"drawOrder":[{"time":0,"offsets":[{"slot":"nope","offset":0}]}]}})",
             true},
            // 0 帧：参考实现 `frames[(frameCount-1)*ENTRIES]` 是**负下标**读
            {"0 帧时间轴", R"({"walk":{"bones":{"root":{"rotate":[]}}}})", false},
        };
        for (const AnimBad& b : kBads)
        {
            g_prefix = std::string("[anim/") + b.name + "] ";
            const std::string json = AnimJson(b.anims);
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, s);
            if (b.parse_fails)
            {
                Check(!r.ok, "anim: 解析必须失败");
                if (!r.ok)
                {
                    std::printf("  (预期报错) [anim/%s] %s\n", b.name, r.error.c_str());
                }
            }
            else
            {
                Check(r.ok, "anim: 这一步**能**解析（错在后面一层）");
                if (r.ok)
                {
                    SpineGpuAsset a;
                    SpineImportResult rb = BakeSpineGpuAsset(s, a);
                    Check(!rb.ok, "anim: bake 必须失败");
                    if (!rb.ok)
                    {
                        std::printf("  (预期报错) %s\n", rb.error.c_str());
                    }
                }
            }
        }
        g_prefix.clear();
    }

    // ---- 9f. 容器侧：动画块的越界引用必须被**读端**挡住 ----
    // 写端永远不会产出这些字节，但容器是跨语言、跨版本的产物 —— 读端不能假设
    // "写端一定对"。而且下标越界在 C++ 与 C# 里都不崩，只会播到别人的数据。
    {
        auto find_chunk_in = [](const std::vector<uint8_t>& b, uint32_t id, uint32_t& off, uint32_t& sz) {
            uint32_t n = 0;
            std::memcpy(&n, b.data() + offsetof(SpineGpuHeader, chunk_count), sizeof(n));
            for (uint32_t i = 0; i < n; ++i)
            {
                const size_t at = static_cast<size_t>(SpineGpuConstants::kHeaderSize) +
                                  static_cast<size_t>(i) * SpineGpuConstants::kChunkTableEntrySize;
                uint32_t cid = 0;
                std::memcpy(&cid, b.data() + at, sizeof(cid));
                if (cid == id)
                {
                    std::memcpy(&off, b.data() + at + 4, sizeof(off));
                    std::memcpy(&sz, b.data() + at + 8, sizeof(sz));
                    return true;
                }
            }
            return false;
        };

        uint32_t tl_off = 0;
        uint32_t tl_sz = 0;
        Check(find_chunk_in(bytes, SpineGpuChunkId::kChunkTimelines, tl_off, tl_sz),
              "anim: 容器里有 chunk 12（时间轴表）");
        uint32_t dir_off = 0;
        uint32_t dir_sz = 0;
        Check(find_chunk_in(bytes, SpineGpuChunkId::kChunkAnimDir, dir_off, dir_sz),
              "anim: 容器里有 chunk 11（动画目录）");
        if (tl_sz != 0)
        {
            {
                // 第 0 条时间轴的 frame_first 指到帧块外
                std::vector<uint8_t> bad = bytes;
                const uint32_t v = 0xFFFFFF00u;
                std::memcpy(bad.data() + tl_off + 8, &v, sizeof(v));
                SpineGpuAsset dummy;
                SpineImportResult r = ReadSpineGpuContainer(bad.data(), bad.size(), dummy);
                Check(!r.ok, "anim: frame_first 越界 ⇒ 读回失败（越界读不报错，只会播到别人的帧）");
                if (!r.ok)
                {
                    std::printf("  (预期报错) %s\n", r.error.c_str());
                }
            }
            {
                // 时间轴类型改成 99（TimelineType 只到 14）
                std::vector<uint8_t> bad = bytes;
                const uint16_t t = 99;
                std::memcpy(bad.data() + tl_off + 0, &t, sizeof(t));
                SpineGpuAsset dummy;
                SpineImportResult r = ReadSpineGpuContainer(bad.data(), bad.size(), dummy);
                Check(!r.ok, "anim: 时间轴类型 99 ⇒ 读回失败（entries 无从确定）");
                if (!r.ok)
                {
                    std::printf("  (预期报错) %s\n", r.error.c_str());
                }
            }
        }
        if (dir_sz != 0)
        {
            // 动画目录大小不是 20 的整数倍 ⇒ 条目数无从确定
            std::vector<uint8_t> bad = bytes;
            uint32_t n = 0;
            std::memcpy(&n, bad.data() + offsetof(SpineGpuHeader, chunk_count), sizeof(n));
            for (uint32_t i = 0; i < n; ++i)
            {
                const size_t at = static_cast<size_t>(SpineGpuConstants::kHeaderSize) +
                                  static_cast<size_t>(i) * SpineGpuConstants::kChunkTableEntrySize;
                uint32_t cid = 0;
                std::memcpy(&cid, bad.data() + at, sizeof(cid));
                if (cid == static_cast<uint32_t>(SpineGpuChunkId::kChunkAnimDir))
                {
                    const uint32_t sz = dir_sz + 1u;
                    std::memcpy(bad.data() + at + 8, &sz, sizeof(sz));
                    break;
                }
            }
            SpineGpuAsset dummy;
            SpineImportResult r = ReadSpineGpuContainer(bad.data(), bad.size(), dummy);
            Check(!r.ok, "anim: 动画目录大小不是 20 的整数倍 ⇒ 读回失败");
            if (!r.ok)
            {
                std::printf("  (预期报错) %s\n", r.error.c_str());
            }
        }
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
                // 且与父附件的 UV 逐值不同（用**自己的**图集 region 算的）。
                // v13：UV 按 `uv_offset + 索引` 取（索引是段内绝对下标）—— 与 shader 同一个式子。
                bool uv_differs = false;
                for (uint32_t k = 0; k < aj.draw_items[1].index_count; ++k)
                {
                    const uint32_t i1 = aj.index_buffer[aj.draw_items[1].index_offset + k];
                    const uint32_t i2 = aj.index_buffer[aj.draw_items[2].index_offset + k];
                    const uint32_t u1 = aj.draw_items[1].uv_offset + i1;
                    const uint32_t u2 = aj.draw_items[2].uv_offset + i2;
                    if (aj.uv_buffer[u1 * 2u] != aj.uv_buffer[u2 * 2u] ||
                        aj.uv_buffer[u1 * 2u + 1u] != aj.uv_buffer[u2 * 2u + 1u])
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
                    // v14：deform_parent 只在 linked mesh 且 inheritDeform 时指向父附件
                    // （MeshAttachment.cpp:94-95）。父附件的 id 就是它的绘制项的 attachment_id。
                    CheckEq(static_cast<long long>(da.draw_items[0].deform_parent),
                            static_cast<long long>(SpineGpuConstants::kNoDeformParent),
                            "deform: region 'quad' 没有 deform_parent");
                    CheckEq(static_cast<long long>(da.draw_items[1].deform_parent),
                            static_cast<long long>(SpineGpuConstants::kNoDeformParent),
                            "deform: 父 mesh 自己没有 deform_parent");
                    CheckEq(static_cast<long long>(da.draw_items[2].deform_parent),
                            inherit ? static_cast<long long>(da.draw_items[1].attachment_id)
                                    : static_cast<long long>(SpineGpuConstants::kNoDeformParent),
                            inherit ? "deform: inheritDeform=1 ⇒ linked mesh 的 deform_parent = 父 mesh"
                                    : "deform: inheritDeform=0 ⇒ linked mesh 没有 deform_parent");
                    // v14：vertex_base。linked mesh 与父 mesh 几何相同 ⇒ 共用同一段 ⇒ 起点相同；
                    // 且起点就是该项最小的索引（这两个夹具的三角形覆盖了全部顶点）。
                    CheckEq(static_cast<long long>(da.draw_items[2].vertex_base),
                            static_cast<long long>(da.draw_items[1].vertex_base),
                            "deform: linked mesh 与父 mesh 共用顶点段 ⇒ vertex_base 相同");
                    uint32_t min_index = 0xFFFFFFFFu;
                    for (uint32_t q = 0; q < da.draw_items[1].index_count; ++q)
                    {
                        min_index = std::min<uint32_t>(min_index, da.index_buffer[da.draw_items[1].index_offset + q]);
                    }
                    CheckEq(static_cast<long long>(da.draw_items[1].vertex_base), static_cast<long long>(min_index),
                            "deform: vertex_base = 该项最小的段内索引");
                    CheckEq(static_cast<long long>(da.draw_items[1].vertex_count),
                            static_cast<long long>(mesh->VertexCount()), "deform: vertex_count = 附件顶点数");
                    CheckEq(static_cast<long long>(da.draw_items[2].vertex_count),
                            static_cast<long long>(da.draw_items[1].vertex_count),
                            "deform: linked mesh 的 vertex_count 与父 mesh 相同（几何继承）");
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
        // ①b deform 指向一个**没有绘制项**的附件（path）⇒ **不是错误**。
        //     本工程实测 88 处指向 path（`K_Lvbu` 槽位 121 等）；当错误会让 50 个资产
        //     整个导不出来，而理由是"某个看不见的东西被动画了"。
        //     ⚠️ 它**不能点亮 kHasDeform** —— 没有绘制项要回退，点亮它等于让运行时
        //        为一个根本不存在的东西每帧准备动态 VB。
        //     v12：它**不再被跳过**。path 的 deform 会改 path 约束摆出来的骨骼
        //     （VertexAttachment.cpp:69-70），跳过它曾让 62 个资产逐骨骼对不上。
        //     ⇒ 烘成一条 target = kDeformTargetPathBit | path 下标 的 deform 时间轴。
        //     （解算语义由 SpineGpuRefParityTest 对着参考实现逐骨骼钉住；这里钉的是编码。）
        {
            // path1 是 rigid、3 个顶点 ⇒ deform 长度 6（SkeletonBinary.cpp:976-977）
            const std::vector<uint8_t> skel =
                BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, true, true, 6,
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
                const SpineImportResult br = BakeSpineGpuAsset(s, asset);
                Check(br.ok, "deform→path: bake 成功");
                if (!br.ok)
                {
                    std::printf("  error: %s\n", br.error.c_str());
                }
                Check((asset.header.flags & SpineGpuFlags::kHasDeform) == 0,
                      "deform→path: **不点亮** kHasDeform（没有绘制项可回退）");
                CheckEq(asset.header.version, SpineGpuConstants::kFormatVersion, "deform→path: 容器版本是 v12");
                size_t n_deform = 0;
                for (const SpineGpuTimeline& t : asset.timelines)
                {
                    if (t.type == static_cast<uint16_t>(SpineTimelineType::kDeform))
                    {
                        ++n_deform;
                        CheckEq(static_cast<long long>(t.target),
                                static_cast<long long>(SpineGpuConstants::kDeformTargetPathBit | 0u),
                                "deform→path: target = 最高位 | path 下标 0（**不是**附件记录 0）");
                    }
                }
                CheckEq(static_cast<long long>(n_deform), 1LL, "deform→path: 烘出 1 条 deform 时间轴（v11 是 0 条）");
                // 往返：读端必须认得最高位，并按 path 附件校验
                std::vector<uint8_t> bytes;
                Check(WriteSpineGpuContainer(asset, bytes).ok, "deform→path: 写容器");
                SpineGpuAsset back;
                const SpineImportResult rr = ReadSpineGpuContainer(bytes.data(), bytes.size(), back);
                Check(rr.ok, "deform→path: 读回来");
                Check(back.timelines == asset.timelines, "deform→path: 时间轴往返一致");
                // 读端校验：把 target 改成越界的 path 下标 ⇒ 必须报错（不是读到别的 path）
                if (rr.ok)
                {
                    SpineGpuAsset bad = asset;
                    for (SpineGpuTimeline& t : bad.timelines)
                    {
                        if (t.type == static_cast<uint16_t>(SpineTimelineType::kDeform))
                        {
                            t.target = static_cast<uint16_t>(SpineGpuConstants::kDeformTargetPathBit | 5u);
                        }
                    }
                    std::vector<uint8_t> bad_bytes;
                    Check(WriteSpineGpuContainer(bad, bad_bytes).ok, "deform→path: 写坏容器");
                    SpineGpuAsset bad_back;
                    const SpineImportResult br2 = ReadSpineGpuContainer(bad_bytes.data(), bad_bytes.size(), bad_back);
                    Check(!br2.ok, "deform→path: 越界的 path 下标 ⇒ 读端报错");
                    std::printf("  (预期报错) %s\n", br2.error.c_str());
                }
            }
        }
        // ①b' 反面：deform 帧写出 path 的 deform 长度（10 > 6）⇒ **烘焙报错**。
        //     参考实现是 `setFixedSize(deformLength)` 后直接写 `deform[v]`（越界 UB），
        //     我们不截断、不静默。
        {
            const std::vector<uint8_t> skel =
                BuildSkelFixture(false, false, "3.8.99", false, "mesh", false, true, true, 10,
                                 false, 2, 0, false, 3, false, true, "path1");
            SpineSourceSkeleton s;
            SpineImportResult r = ParseSpineSkeletonBinary(skel.data(), skel.size(), &atlas, s);
            Check(r.ok, "deform→path 过长: 解析成功（越界在烘焙期判）");
            if (r.ok)
            {
                SpineGpuAsset asset;
                const SpineImportResult br = BakeSpineGpuAsset(s, asset);
                Check(!br.ok, "deform→path 过长: bake 报错");
                std::printf("  (预期报错) %s\n", br.error.c_str());
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
                // v15：缺 end 落盘成 kNoClipEndSlot（**不是 0** —— 0 是 s0，一个真实存在的槽位）
                Check(a.clip_polygons.size() == 1 && a.clip_polygons[0].end_slot == SpineGpuConstants::kNoClipEndSlot,
                      "clip v15: 缺 end ⇒ end_slot = kNoClipEndSlot（不是 0）");
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
            CheckEq(static_cast<long long>(FindChunk(bytes, SpineGpuChunkId::kChunkClipData)), 20,
                    "clip-data: chunk 9 = 20 B × 1 条（v15）");

            // ---- v15：名字与结束槽位落盘 ----
            // 名字要**翻译回字符串**才算数：偏移算错（组序排歪）字节照样自洽。
            if (ja.clip_polygons.size() == 1)
            {
                const SpineGpuClipPolygon& p = ja.clip_polygons[0];
                CheckEq(static_cast<long long>(p.end_slot), 2, "clip-data v15: end_slot = s2（含）");
                CheckEq(static_cast<long long>(p.pad2), 0, "clip-data v15: pad2 写 0");
                const std::vector<uint8_t> blob = BuildSpineStringBlob(ja);
                Check(ja.clip_names.size() == 1 && ja.clip_names[0] == "clip",
                      "clip-data v15: clip_names = { 'clip' }（皮肤里的键）");
                Check(p.name_offset < blob.size() &&
                          std::string(reinterpret_cast<const char*>(blob.data() + p.name_offset)) == "clip",
                      "clip-data v15: name_offset 翻译回字符串 = 'clip'");
                // 运行时：s1 的 setup 附件就是这条裁剪附件 ⇒ AttachRef.clip = 0，且不产生绘制项
                SpineGpuRuntimeData rd;
                Check(SpineGpuBuildRuntimeData(ja, rd).ok, "clip-data v15: 运行时查找表建成");
                Check(rd.slot_setup.size() == 4 && rd.slot_setup[1].clip == 0u &&
                          rd.slot_setup[1].draw_item == SpineGpuAttachRef::kNone,
                      "clip-data v15: s1 的 setup 附件解析成第 0 条裁剪多边形、没有绘制项");
                Check(rd.slot_setup.size() == 4 && rd.slot_setup[2].clip == SpineGpuAttachRef::kNone,
                      "clip-data v15: 可画槽位的 clip 是 kNone");
            }

            // 容器必须可重入：读回来再写一遍要逐字节相同（多边形与 clip_index 都得保住）
            SpineGpuAsset back;
            Check(ReadSpineGpuContainer(bytes.data(), bytes.size(), back).ok, "clip-data: 读回成功");
            CheckEq(static_cast<long long>(back.clip_polygons.size()), 1, "clip-data: 读回后多边形还在");
            std::vector<uint8_t> again;
            Check(WriteSpineGpuContainer(back, again).ok, "clip-data: 再序列化成功");
            Check(bytes == again, "clip-data: round-trip 逐字节相同（含裁剪多边形与 clip_index）");

            g_prefix.clear();
        }

        // ---- 13d'. v15：deform 指向**裁剪附件** ⇒ 照样烘焙（target 置 clip 位）----
        // v14 在烘焙期把它当"不影响姿势"丢掉 —— 对骨骼成立，对画面不成立（clipStart 读槽位的
        // deform 数组算多边形）。OSGame 实测 7 条（K_Niumo `shandian1` 等）。
        // 夹具：rigid 4 顶点的 clip，offset 2 起 4 个 +1 ⇒ deform = setup + {0,0, 1,1, 1,1, 0,0}。
        {
            g_prefix = "[clip-deform] ";
            const std::string json = BuildClipDeformOnClip();
            Check(!json.empty(), "clip-deform: 夹具替换成功");
            if (!json.empty())
            {
                SpineSourceSkeleton cj;
                SpineImportResult rj = ParseSpineSkeletonJson(json.data(), json.size(), &atlas, cj);
                Check(rj.ok, "clip-deform: json 解析成功");
                SpineGpuAsset ja;
                const SpineImportResult bj = rj.ok ? BakeSpineGpuAsset(cj, ja) : rj;
                Check(bj.ok, "clip-deform: bake 成功");
                if (!bj.ok)
                {
                    std::printf("  error: %s\n", bj.error.c_str());
                }
                size_t n_deform = 0;
                for (const SpineGpuTimeline& t : ja.timelines)
                {
                    if (t.type == static_cast<uint16_t>(SpineTimelineType::kDeform))
                    {
                        ++n_deform;
                        CheckEq(static_cast<long long>(t.target),
                                static_cast<long long>(SpineGpuConstants::kDeformTargetClipBit | 0u),
                                "clip-deform: target = clip 位 | 裁剪多边形 0（不是附件记录 / path）");
                    }
                }
                CheckEq(static_cast<long long>(n_deform), 1LL, "clip-deform: 烘出 1 条 deform 时间轴（v14 是 0 条）");
                Check((ja.header.flags & SpineGpuFlags::kHasDeform) == 0,
                      "clip-deform: **不点亮** kHasDeform（没有绘制项因它回退）");

                // 运行时：apply 之后 s1 的 deform 数组 = setup + 偏移（rigid 是**绝对**坐标）
                if (bj.ok && ja.anims.size() == 1)
                {
                    SpineGpuRuntimeData rd;
                    SpineGpuSkeletonState st;
                    const bool init_ok = SpineGpuBuildRuntimeData(ja, rd).ok && SpineGpuInitSkeletonState(ja, rd, st).ok;
                    Check(init_ok, "clip-deform: 运行时初始化");
                    if (init_ok)
                    {
                        const SpineImportResult ar = SpineGpuApplyAnimation(ja, rd, st, 0, 0.0f, 0.5f, false, 1.0f,
                                                                            SpineGpuMixBlend::kFirst,
                                                                            SpineGpuMixDirection::kIn);
                        Check(ar.ok, "clip-deform: apply 成功（v14 的解算器会把 clip 位当附件记录下标 ⇒ 越界）");
                        const std::vector<float> want = {0, 0, 11, 1, 11, 11, 0, 10};
                        Check(st.slots.size() == 4 && st.slots[1].deform == want,
                              "clip-deform: s1 的 deform = {0,0, 11,1, 11,11, 0,10}");
                        Check(st.slots.size() == 4 && st.slots[2].deform.empty(), "clip-deform: s2（mesh）不受影响");
                    }
                }

                // 往返 + 读端校验：clip 下标越界 / 两位同时置位 ⇒ 必须报错
                std::vector<uint8_t> bytes;
                Check(bj.ok && WriteSpineGpuContainer(ja, bytes).ok, "clip-deform: 写容器");
                SpineGpuAsset back;
                Check(!bytes.empty() && ReadSpineGpuContainer(bytes.data(), bytes.size(), back).ok &&
                          back.timelines == ja.timelines,
                      "clip-deform: 读回、时间轴往返一致");
                for (int variant = 0; variant < 2 && bj.ok; ++variant)
                {
                    SpineGpuAsset bad = ja;
                    for (SpineGpuTimeline& t : bad.timelines)
                    {
                        if (t.type == static_cast<uint16_t>(SpineTimelineType::kDeform))
                        {
                            t.target = variant == 0 ? static_cast<uint16_t>(SpineGpuConstants::kDeformTargetClipBit | 3u)
                                                    : static_cast<uint16_t>(SpineGpuConstants::kDeformTargetClipBit |
                                                                            SpineGpuConstants::kDeformTargetPathBit);
                        }
                    }
                    std::vector<uint8_t> bad_bytes;
                    Check(WriteSpineGpuContainer(bad, bad_bytes).ok, "clip-deform: 写坏容器");
                    SpineGpuAsset bad_back;
                    const SpineImportResult br2 = ReadSpineGpuContainer(bad_bytes.data(), bad_bytes.size(), bad_back);
                    Check(!br2.ok, variant == 0 ? "clip-deform: 越界的裁剪多边形下标 ⇒ 读端报错"
                                                : "clip-deform: path 位与 clip 位同时置位 ⇒ 读端报错");
                    std::printf("  (预期报错) %s\n", br2.error.c_str());
                }
                // 结束槽位 / 名字偏移指歪 ⇒ 读端报错
                for (int variant = 0; variant < 2 && bj.ok && !ja.clip_polygons.empty(); ++variant)
                {
                    SpineGpuAsset bad = ja;
                    if (variant == 0)
                    {
                        bad.clip_polygons[0].end_slot = 4;   // 只有 4 个槽位
                    }
                    else
                    {
                        bad.clip_polygons[0].name_offset = 0;   // 指到第一个骨骼名上
                    }
                    std::vector<uint8_t> bad_bytes;
                    Check(WriteSpineGpuContainer(bad, bad_bytes).ok, "clip v15: 写坏容器");
                    SpineGpuAsset bad_back;
                    const SpineImportResult br2 = ReadSpineGpuContainer(bad_bytes.data(), bad_bytes.size(), bad_back);
                    Check(!br2.ok, variant == 0 ? "clip v15: end_slot 越界 ⇒ 读端报错"
                                                : "clip v15: name_offset 不是第 9 组那条 ⇒ 读端报错");
                    std::printf("  (预期报错) %s\n", br2.error.c_str());
                }
            }
            g_prefix.clear();
        }

        // ---- 13d''. v15：region 的 (角, UV) 配对 + item_flags ----
        // 参考 `_uvs` 按 computeWorldVertices 的 BR, BL, UL, UR 配对（槽位名 BLX/ULX… 只是下标），
        // v14 按槽位名抄 ⇒ 每个角错一位、贴图转 90°。手算：quad 64×32 居中，图集 region (0,0) 64×32 /
        // 256×256 ⇒ u=0 v=0 u2=0.25 v2=0.125；贴图 v 向下 ⇒ BL 角取 (u, v2)。
        {
            g_prefix = "[region-uv] ";
            SpineSourceSkeleton s;
            const SpineImportResult r = ParseSpineSkeletonJson(kSkeleton, std::strlen(kSkeleton), &atlas, s);
            SpineGpuAsset a;
            const bool ok = r.ok && BakeSpineGpuAsset(s, a).ok;
            Check(ok, "region-uv: 基线夹具解析 + 烘焙");
            const SpineDrawItem* quad = nullptr;
            for (const SpineDrawItem& it : a.draw_items)
            {
                if (it.attachment_id < a.attachment_names.size() && a.attachment_names[it.attachment_id] == "quad" &&
                    it.skin_id == 0)
                {
                    quad = &it;
                }
                if (it.attachment_id < a.attachment_names.size() && a.attachment_names[it.attachment_id] == "mesh")
                {
                    CheckEq(static_cast<long long>(it.item_flags), 0, "region-uv: mesh 的 item_flags = 0");
                }
            }
            Check(quad != nullptr, "region-uv: 找到 quad 的绘制项");
            if (quad != nullptr)
            {
                CheckEq(static_cast<long long>(quad->item_flags), static_cast<long long>(SpineDrawItemFlags::kRegion),
                        "region-uv: quad 的 item_flags = kRegion");
                const float kPos[8] = {-32, -16, -32, 16, 32, 16, 32, -16};          // BL, UL, UR, BR
                const float kUv[8] = {0, 0.125f, 0, 0, 0.25f, 0, 0.25f, 0.125f};     // (u,v2) (u,v) (u2,v) (u2,v2)
                bool pos_ok = true;
                bool uv_ok = true;
                for (uint32_t j = 0; j < 4; ++j)
                {
                    float xy[2] = {0, 0};
                    std::memcpy(xy, a.vertex_buffer.data() + (static_cast<size_t>(quad->vertex_base) + j) * 8u, 8);
                    pos_ok = pos_ok && std::fabs(xy[0] - kPos[j * 2]) < 1e-4f && std::fabs(xy[1] - kPos[j * 2 + 1]) < 1e-4f;
                    const size_t uv = static_cast<size_t>(static_cast<uint32_t>(quad->uv_offset + quad->vertex_base + j)) * 2u;
                    uv_ok = uv_ok && a.uv_buffer[uv] == kUv[j * 2] && a.uv_buffer[uv + 1u] == kUv[j * 2 + 1];
                }
                Check(pos_ok, "region-uv: 顶点顺序 BL, UL, UR, BR");
                Check(uv_ok, "region-uv: BL→(u,v2) UL→(u,v) UR→(u2,v) BR→(u2,v2)（v14 错一位 ⇒ 贴图转 90°）");
            }
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
