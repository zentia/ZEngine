// =====================================================================
// ShaderLabPropertyBlobTest
// ---------------------------------------------------------------------
// Pins down "property name -> which bytes of the material UBO". Reflection
// (ShaderCbufferReflectionTest) answers "_Amount lives at byte 20 of
// UnityPerMaterial"; this answers "so write a float at byte 20 of the UBO, and
// only mark that one float as overridden".
//
// It is the last CPU-only link in the ShaderLab runtime chain. Everything after
// it (staging buffer -> CopyBuffer -> descriptor set) needs a GPU, so this is
// the only part of the write path that can be verified on a machine with no
// GPU and no shader compiler.
//
// The invariant that matters most: **a per-float mask, not a per-property one**.
// `SetFloat("_Amount", v)` must not stomp the neighbouring components of the
// same property, and an override must not silently revert untouched components
// to 0. Cases F/G pin that down.
//
// Cases:
//   A a slot table built the way the real UnityPerMaterial block looks
//   B writing a scalar touches exactly one float and one mask bit
//   C writing a vec4 touches four floats
//   D SetNamedFloat on an unknown name returns false and writes nothing
//   E a slot that runs past the 128-byte blob is rejected, not clamped
//   F the mask is per float: writing component 1 leaves component 0 alone
//   G ApplyOverride copies only masked floats; unmasked ones keep the base value
//   H ApplyOverride ORs the masks, so the result reads as "current value"
//   I a matrix-ish slot (size not a multiple of 4) is refused by the scalar path
//   J defaults are written unmasked -- asset values are not "user set these"
//   K SetNamedVector on a float property writes 1 float, not 4
//   L GetNamedVector zero-fills the tail a shorter property cannot fill
//   M FindSlot is exact-match and empty names never match
//   N Clear() wipes bytes and mask
//   O SetFloat past float 31 is a no-op instead of a buffer overrun
//
// Backend-free and ZRuntime-free: ShaderPropertyBlob.h is header-only and
// depends on nothing but <cstring> / <string> / <vector>.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderPropertyBlob.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ZEngine::ShaderLab;

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            std::fprintf(stderr, "  [FAIL] %s\n", name);
            ++g_Failures;
        }
        else
        {
            std::fprintf(stdout, "  [ ok ] %s\n", name);
        }
    }

    bool Nearly(float a, float b)
    {
        return std::fabs(a - b) < 1e-6f;
    }

    ShaderPropertySlot Slot(const char* name, uint32_t offset, uint32_t size, uint32_t components)
    {
        ShaderPropertySlot slot;
        slot.name = name;
        slot.offset = offset;
        slot.size = size;
        slot.components = components;
        return slot;
    }

    /// 铺一张"真实感"的落点表：抄自 engine/shader/example_standard.shader 的
    /// UnityPerMaterial（HLSL cbuffer 打包），_BaseColor@0、_BaseMap_ST@16、
    /// _Metallic@32、_Smoothness@36、_NormalScale@40、_Cutoff@44。
    std::vector<ShaderPropertySlot> StandardLitLayout()
    {
        std::vector<ShaderPropertySlot> slots;
        slots.push_back(Slot("_BaseColor", 0, 16, 4));
        slots.push_back(Slot("_BaseMap_ST", 16, 16, 4));
        slots.push_back(Slot("_Metallic", 32, 4, 1));
        slots.push_back(Slot("_Smoothness", 36, 4, 1));
        slots.push_back(Slot("_NormalScale", 40, 4, 1));
        slots.push_back(Slot("_Cutoff", 44, 4, 1));
        return slots;
    }
}  // namespace

int main()
{
    // ---- A: 落点表长什么样 --------------------------------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        Check(slots.size() == 6, "A the layout carries 6 properties");
        Check(FindSlot(slots, "_Metallic")->offset == 32, "A b _Metallic is at byte 32");
        Check(slots[2].FloatCount() == 1, "A c a float property holds 1 float");
        Check(slots[0].FloatCount() == 4, "A d a float4 property holds 4 floats");
    }

    // ---- B: 写标量只动一个 float -------------------------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob blob;

        const bool written = SetNamedFloat(slots, blob, "_Smoothness", 0.75f);
        Check(written, "B SetNamedFloat returns true for a known float property");
        Check(Nearly(blob.GetFloat(9), 0.75f), "B b byte 36 == float 9 holds 0.75");
        Check(blob.mask == (1u << 9), "B c exactly one mask bit is set (float 9)");
        Check(Nearly(blob.GetFloat(8), 0.0f), "B d the neighbouring float is untouched");
    }

    // ---- C: 写 vec4 动四个 float -------------------------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob blob;

        const float color[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        const bool written = SetNamedVector(slots, blob, "_BaseColor", color);
        Check(written, "C SetNamedVector accepts a float4 property");
        Check(Nearly(blob.GetFloat(0), 0.1f) && Nearly(blob.GetFloat(1), 0.2f) &&
                  Nearly(blob.GetFloat(2), 0.3f) && Nearly(blob.GetFloat(3), 0.4f),
              "C b all four floats land in order");
        Check(blob.mask == 0xFu, "C c the mask covers floats 0..3");
    }

    // ---- D: 名字不认识 -> false 且一个字节都不写 ----------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob blob;

        const bool written = SetNamedFloat(slots, blob, "_NotDeclared", 1.0f);
        Check(!written, "D an unknown name returns false");
        Check(blob.mask == 0, "D b nothing was marked overridden");

        float value = 123.0f;
        Check(!GetNamedFloat(slots, blob, "_NotDeclared", value), "D d GetNamedFloat returns false");
        Check(Nearly(value, 123.0f), "D e and leaves the out-param alone");
    }

    // ---- E: 越界的落点被拒绝，不是被截断 ------------------------------------
    {
        std::vector<ShaderPropertySlot> slots;
        // 124 + 8 = 132 > 128：尾巴伸出去了。
        slots.push_back(Slot("_TooFar", 124, 8, 2));
        ShaderPropertyBlob blob;

        Check(slots[0].FloatCount() == 0, "E a slot that overruns the blob holds 0 floats");
        const float values[2] = {1.0f, 2.0f};
        Check(!WriteSlot(blob, slots[0], values, 2), "E b writing it is refused");
        Check(blob.mask == 0, "E c and nothing was written");

        // 正好贴着边界的应当放行。
        slots.clear();
        slots.push_back(Slot("_JustFits", 120, 8, 2));
        Check(slots[0].FloatCount() == 2, "E d a slot ending exactly at 128 is fine");
        Check(WriteSlot(blob, slots[0], values, 2), "E e and it writes");
        Check(Nearly(blob.GetFloat(30), 1.0f) && Nearly(blob.GetFloat(31), 2.0f), "E f both floats land");
    }

    // ---- F: 掩码按 float 记，不是按属性记 ----------------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob blob;

        // 先铺资产默认值（不打掩码），再只 override 第 1 个分量。
        const float defaults[4] = {9.0f, 9.0f, 9.0f, 9.0f};
        WriteSlot(blob, *FindSlot(slots, "_BaseColor"), defaults, 4);
        blob.mask = 0;

        Check(WriteSlotComponent(blob, *FindSlot(slots, "_BaseColor"), 1, 5.0f), "F writing one component works");
        Check(Nearly(blob.GetFloat(1), 5.0f), "F b component 1 changed");
        Check(Nearly(blob.GetFloat(0), 9.0f) && Nearly(blob.GetFloat(2), 9.0f),
              "F c components 0 and 2 keep the asset value");
        Check(blob.mask == (1u << 1), "F d only component 1's mask bit is set");
    }

    // ---- G: ApplyOverride 只搬掩码标记过的 float ---------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob base;
        const float base_values[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        WriteSlot(base, *FindSlot(slots, "_BaseColor"), base_values, 4);
        base.mask = 0;

        ShaderPropertyBlob override_blob;
        override_blob.SetFloat(2, 42.0f);  // 只改第 3 个分量

        ApplyOverride(override_blob, base);
        Check(Nearly(base.GetFloat(2), 42.0f), "G the overridden float is copied");
        Check(Nearly(base.GetFloat(0), 1.0f) && Nearly(base.GetFloat(1), 2.0f) && Nearly(base.GetFloat(3), 4.0f),
              "G b the unmasked floats keep the base value");
    }

    // ---- H: ApplyOverride 把掩码并上去 -------------------------------------
    {
        ShaderPropertyBlob base;
        base.mask = (1u << 5);
        ShaderPropertyBlob override_blob;
        override_blob.SetFloat(7, 1.0f);

        ApplyOverride(override_blob, base);
        Check(base.mask == ((1u << 5) | (1u << 7)), "H the masks are OR'd together");
        Check(base.HasFloat(5) && base.HasFloat(7), "H b both read as set");
    }

    // ---- I: 矩阵 / 数组走不了标量路径 ---------------------------------------
    {
        std::vector<ShaderPropertySlot> slots;
        ShaderPropertyBlob blob;

        // float3x3 在 std140 下是 48 字节。一次 SetFloat / SetVector 表达不了它的
        // 值 —— 按 components(=3) 写只会写出个半截矩阵，所以整条拒绝。
        slots.push_back(Slot("_Rotation", 0, 48, 3));
        Check(slots[0].FloatCount() == 0, "I a matrix slot is refused by the scalar path");
        const float matrix_values[3] = {1.0f, 0.0f, 0.0f};
        Check(!WriteSlot(blob, slots[0], matrix_values, 3), "I b writing it fails");
        Check(blob.mask == 0, "I c and nothing was written");

        // 数组同理：`float _Curve[4]` 在 std140 下占 64 字节。
        slots.clear();
        slots.push_back(Slot("_Curve", 0, 64, 1));
        Check(slots[0].FloatCount() == 0, "I d an array slot is refused too");

        // size 不是 4 的倍数 = 解析没认出来的类型，同样拒。
        slots.clear();
        slots.push_back(Slot("_Broken", 0, 7, 1));
        Check(slots[0].FloatCount() == 0, "I e a size that is not a multiple of 4 is refused");
        const float value = 1.0f;
        Check(!WriteSlot(blob, slots[0], &value, 1), "I f and writing it fails");
    }

    // ---- J: 默认值是不带掩码地铺进去的 -------------------------------------
    {
        ShaderPropertyBlob defaults;
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        Check(WriteSlot(defaults, *FindSlot(slots, "_BaseColor"), color, 4), "J defaults can be written");
        Check(defaults.mask != 0, "J b WriteSlot itself marks the floats (as any explicit write does)");
        // 真正保证"默认值不算 override"的是 BuildShaderPropertySlots 末尾那句
        // `out_defaults.mask = 0;` —— 这里钉住清零之后的行为。
        defaults.mask = 0;
        Check(!defaults.AnyOverride(), "J c after clearing, AnyOverride() is false");
    }

    // ---- K: 对标量属性调 SetNamedVector 只写 1 个 float ---------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob blob;

        const float values[4] = {7.0f, 8.0f, 9.0f, 10.0f};
        Check(SetNamedVector(slots, blob, "_Cutoff", values), "K SetNamedVector accepts a float property");
        Check(Nearly(blob.GetFloat(11), 7.0f), "K b byte 44 == float 11 got the first value");
        Check(Nearly(blob.GetFloat(12), 0.0f), "K c float 12 (_Cutoff 的邻居) 没被污染");
        Check(blob.mask == (1u << 11), "K d only one mask bit is set");
    }

    // ---- L: GetNamedVector 把读不到的尾巴填 0 -------------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob blob;
        const float value = 3.0f;
        WriteSlotComponent(blob, *FindSlot(slots, "_Cutoff"), 0, value);

        float out_values[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        Check(GetNamedVector(slots, blob, "_Cutoff", out_values), "L GetNamedVector succeeds on a float property");
        Check(Nearly(out_values[0], 3.0f), "L b component 0 is the written value");
        Check(Nearly(out_values[1], 0.0f) && Nearly(out_values[2], 0.0f) && Nearly(out_values[3], 0.0f),
              "L c the tail is zero-filled, not left as caller garbage");
    }

    // ---- M: FindSlot 是精确匹配 --------------------------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        Check(FindSlot(slots, "_Metallic") != nullptr, "M an exact name is found");
        Check(FindSlot(slots, "_metallic") == nullptr, "M b matching is case-sensitive");
        Check(FindSlot(slots, "_Metal") == nullptr, "M c a prefix is not a match");
        Check(FindSlot(slots, "") == nullptr, "M d an empty name never matches");
    }

    // ---- N: Clear 清字节也清掩码 -------------------------------------------
    {
        const std::vector<ShaderPropertySlot> slots = StandardLitLayout();
        ShaderPropertyBlob blob;
        SetNamedFloat(slots, blob, "_Metallic", 1.0f);
        Check(blob.AnyOverride(), "N the blob has an override before Clear");

        blob.Clear();
        Check(!blob.AnyOverride(), "N b Clear() wipes the mask");
        Check(Nearly(blob.GetFloat(8), 0.0f), "N c and the bytes");
    }

    // ---- O: 越界下标是 no-op -----------------------------------------------
    {
        ShaderPropertyBlob blob;
        blob.SetFloat(31, 1.0f);
        Check(blob.HasFloat(31), "O float 31 is the last valid slot");

        blob.SetFloat(32, 1.0f);
        Check(!blob.HasFloat(32), "O b float 32 is ignored, not written out of bounds");
        Check(Nearly(blob.GetFloat(32), 0.0f), "O c and reads back as 0");
        Check(blob.mask == (1u << 31), "O d the mask was not corrupted");
    }

    std::fprintf(stdout, "\n%s: %d checks, %d failures\n",
                 g_Failures == 0 ? "PASS" : "FAIL", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
