// =====================================================================
// ShaderLabTextureSlotTest
// ---------------------------------------------------------------------
// Pins down "2D property name -> which of the 5 material texture bindings".
// Reflection answers "_BumpMap is the second 2D property declared in
// Properties{}"; this answers "so it binds to descriptor binding 2", and
// "SetTexture(\"_BumpMap\", t) therefore writes slot 1 of the override array".
//
// The rule being pinned is **declaration order**, and it is worth stating why
// it is order and not a hash or a name-sorted index: the material descriptor
// set has exactly 6 bindings (0 = UBO, 1..5 = the five PBR textures) and the
// pool is sized `3 + 5 * m_MaxMaterialCount`. Adding bindings for custom
// shaders would mean touching both, and bindless is not available as an
// escape hatch (RHIBindlessTextureManager requires a
// supportsBindlessTextures() guard). Reusing 1..5 costs nothing and caps a
// shader at 5 textures.
//
// The consequence worth testing: **reordering properties in a .shader moves
// them to a different slot**. That is not a silent corruption (the GLSL side
// declares bindings from the same table), but it is a real footgun, so cases
// D/E pin the direction of the mapping.
//
// Cases:
//   A 2D properties get 0,1,2... in declaration order
//   B non-2D properties never take a slot and do not shift the counter
//   C the 6th and later 2D properties get k_NoTextureSlot and count as overflow
//   D Cube / 3D take no slot either (there is no binding for them)
//   E reordering the declarations moves the slots with them
//   F SlotAt past the end is k_NoTextureSlot (callers must not read OOB)
//   G FindTextureSlotBinding is exact-match; unknown and null both miss
//   H a binding table built the way the runtime builds it (slots only)
//   I FindBuiltinTextureSlot maps the StandardLit aliases
//   J FindBuiltinTextureSlot rejects unknown names and null
//   K IsTextureSlotType is case-tolerant only about the "2D" spelling
//
// Backend-free and ZRuntime-free: ShaderTextureSlot.h is header-only and
// depends on nothing but <cstdint> / <string> / <vector>.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderTextureSlot.h"

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

    ShaderTextureSlotInput Prop(const char* name, const char* type)
    {
        ShaderTextureSlotInput input;
        input.name = name;
        input.type = type;
        return input;
    }

    /// 一张"真实感"的属性表：抄自典型 Unity shader 的 Properties{}，2D 与标量混排。
    std::vector<ShaderTextureSlotInput> MixedProperties()
    {
        std::vector<ShaderTextureSlotInput> props;
        props.push_back(Prop("_MainTex", "2D"));
        props.push_back(Prop("_BaseColor", "Color"));
        props.push_back(Prop("_BumpMap", "2D"));
        props.push_back(Prop("_Smoothness", "Range"));
        props.push_back(Prop("_MaskTex", "2D"));
        return props;
    }
}  // namespace

int main()
{
    // ---- A: 按声明顺序分配 --------------------------------------------------
    {
        const std::vector<ShaderTextureSlotInput> props = MixedProperties();
        const ShaderTextureSlotTable table = AssignTextureSlots(props);

        Check(table.SlotAt(0) == 0, "A _MainTex (1st 2D) takes slot 0");
        Check(table.SlotAt(2) == 1, "A b _BumpMap (2nd 2D) takes slot 1");
        Check(table.SlotAt(4) == 2, "A c _MaskTex (3rd 2D) takes slot 2");
        Check(table.assigned == 3, "A d three slots are assigned");
        Check(table.overflow == 0, "A e nothing overflowed");
    }

    // ---- B: 非 2D 属性不占槽，也不推动计数器 --------------------------------
    {
        const std::vector<ShaderTextureSlotInput> props = MixedProperties();
        const ShaderTextureSlotTable table = AssignTextureSlots(props);

        Check(table.SlotAt(1) == k_NoTextureSlot, "B a Color property takes no slot");
        Check(table.SlotAt(3) == k_NoTextureSlot, "B b a Range property takes no slot");
        // 关键：_BumpMap 前面有一个 Color，它必须仍然是槽 1 而不是槽 2。
        Check(table.SlotAt(2) == 1, "B c the counter only advances on 2D properties");
    }

    // ---- C: 第 6 张及以后拿不到槽 ------------------------------------------
    {
        std::vector<ShaderTextureSlotInput> props;
        for (int i = 0; i < 7; ++i)
        {
            props.push_back(Prop(("tex" + std::to_string(i)).c_str(), "2D"));
        }
        const ShaderTextureSlotTable table = AssignTextureSlots(props);

        Check(table.assigned == k_MaterialTextureSlotCount, "C the first five get slots");
        Check(table.overflow == 2, "C b the last two are counted as overflow");
        Check(table.SlotAt(5) == k_NoTextureSlot, "C c the 6th 2D property has no slot");
        Check(table.SlotAt(6) == k_NoTextureSlot, "C d the 7th 2D property has no slot");
    }

    // ---- D: Cube / 3D 也不占槽 ---------------------------------------------
    {
        std::vector<ShaderTextureSlotInput> props;
        props.push_back(Prop("_Cube", "Cube"));
        props.push_back(Prop("_Vol", "3D"));
        props.push_back(Prop("_MainTex", "2D"));
        const ShaderTextureSlotTable table = AssignTextureSlots(props);

        Check(table.SlotAt(0) == k_NoTextureSlot, "D a Cube takes no slot");
        Check(table.SlotAt(1) == k_NoTextureSlot, "D b 3D takes no slot");
        Check(table.SlotAt(2) == 0, "D c the 2D property still starts at slot 0");
        Check(table.assigned == 1, "D d exactly one slot assigned");
    }

    // ---- E: 改声明顺序 = 换槽位 --------------------------------------------
    {
        std::vector<ShaderTextureSlotInput> props;
        props.push_back(Prop("_BumpMap", "2D"));
        props.push_back(Prop("_MainTex", "2D"));
        const ShaderTextureSlotTable table = AssignTextureSlots(props);

        Check(table.SlotAt(0) == 0, "E _BumpMap now binds to slot 0");
        Check(table.SlotAt(1) == 1, "E b _MainTex moved to slot 1");
        // 这是这个方案已知的代价，钉在这里是为了让人改规则时先看一眼上面的注释。
        Check(k_MaterialTextureSlotCount == 5u, "E c the cap is still 5 slots");
    }

    // ---- F: 越界读取安全 ----------------------------------------------------
    {
        const std::vector<ShaderTextureSlotInput> props = MixedProperties();
        const ShaderTextureSlotTable table = AssignTextureSlots(props);

        Check(table.SlotAt(99) == k_NoTextureSlot, "F SlotAt past the end returns k_NoTextureSlot");
        Check(AssignTextureSlots({}).slots.empty(), "F b an empty property list yields an empty table");
    }

    // ---- G: 按名字查绑定 ----------------------------------------------------
    {
        std::vector<ShaderTextureSlotBinding> bindings;
        bindings.push_back(ShaderTextureSlotBinding {"_MainTex", 0});
        bindings.push_back(ShaderTextureSlotBinding {"_BumpMap", 1});

        const ShaderTextureSlotBinding* found = FindTextureSlotBinding(bindings, "_BumpMap");
        Check(found != nullptr && found->slot == 1, "G FindTextureSlotBinding finds by name");
        Check(found != nullptr && found->HasSlot(), "G b a valid slot reads as HasSlot()");
        Check(FindTextureSlotBinding(bindings, "_Nope") == nullptr, "G c an unknown name misses");
        Check(FindTextureSlotBinding(bindings, nullptr) == nullptr, "G d a null name misses");
    }

    // ---- H: 运行时那张表（只收真正拿到槽的属性） ----------------------------
    {
        const std::vector<ShaderTextureSlotInput> props = MixedProperties();
        const ShaderTextureSlotTable table = AssignTextureSlots(props);

        std::vector<ShaderTextureSlotBinding> bindings;
        for (size_t i = 0; i < props.size(); ++i)
        {
            if (table.SlotAt(i) == k_NoTextureSlot)
            {
                continue;
            }
            bindings.push_back(ShaderTextureSlotBinding {props[i].name, table.SlotAt(i)});
        }

        Check(bindings.size() == 3, "H only the 2D properties enter the binding table");
        Check(bindings[1].name == "_BumpMap" && bindings[1].slot == 1, "H b ordering follows declaration order");
        // SetTexture 对第 6 张之后的属性必须返回 false，而不是绑到槽 0 上。
        std::vector<ShaderTextureSlotInput> too_many;
        for (int i = 0; i < 6; ++i)
        {
            too_many.push_back(Prop(("t" + std::to_string(i)).c_str(), "2D"));
        }
        const ShaderTextureSlotTable big = AssignTextureSlots(too_many);
        Check(big.SlotAt(5) == k_NoTextureSlot, "H c the 6th texture would have no binding at all");
    }

    // ---- I: 内置 StandardLit 的贴图别名 ------------------------------------
    {
        Check(FindBuiltinTextureSlot("_MainTex") == 0, "I _MainTex is slot 0");
        Check(FindBuiltinTextureSlot("_BaseMap") == 0, "I b _BaseMap is the same slot");
        Check(FindBuiltinTextureSlot("_MetallicGlossMap") == 1, "I c metallic/roughness is slot 1");
        Check(FindBuiltinTextureSlot("_BumpMap") == 2, "I d the normal map is slot 2");
        Check(FindBuiltinTextureSlot("_OcclusionMap") == 3, "I e occlusion is slot 3");
        Check(FindBuiltinTextureSlot("_EmissionMap") == 4, "I f emissive is slot 4");
    }

    // ---- J: 别名表不认识的名字 ---------------------------------------------
    {
        Check(FindBuiltinTextureSlot("_Whatever") == k_NoTextureSlot, "J an unknown name is rejected");
        Check(FindBuiltinTextureSlot(nullptr) == k_NoTextureSlot, "J b a null name is rejected");
        Check(FindBuiltinTextureSlot("") == k_NoTextureSlot, "J c an empty name is rejected");
        // 一个自定义 shader 的名字不该被内置表接住 —— 那条路走反射表。
        Check(FindBuiltinTextureSlot("_MaskTex") == k_NoTextureSlot, "J d a custom property name is not in the builtin table");
    }

    // ---- K: 类型判定 --------------------------------------------------------
    {
        Check(IsTextureSlotType("2D"), "K \"2D\" is a texture type");
        Check(IsTextureSlotType("2d"), "K b lower-case spelling is accepted");
        Check(IsTextureSlotType("sampler2D"), "K c sampler2D is accepted");
        Check(!IsTextureSlotType("Cube"), "K d Cube is not (no binding exists for it)");
        Check(!IsTextureSlotType("Float"), "K e Float is not");
        Check(!IsTextureSlotType(""), "K f an empty type is not");
    }

    std::fprintf(stdout, "\n%s: %d checks, %d failures\n", g_Failures == 0 ? "PASS" : "FAIL", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
