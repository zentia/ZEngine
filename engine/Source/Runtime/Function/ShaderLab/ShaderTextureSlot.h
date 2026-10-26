#pragma once

//
// ShaderLab 贴图属性 -> 材质描述符集槽位。
//
// 材质描述符集一共 6 个 binding，写死在 MainCameraPass 里：
//
//   0 = 材质 UBO（128 字节，见 ShaderPropertyBlob.h）
//   1..5 = base_color / metallic_roughness / normal / occlusion / emissive
//
// 自定义 ShaderLab shader 声明的 `2D` 属性（_MainTex / _BumpMap / _MaskTex ...）
// **复用这 5 个槽**，按 Properties{} 里的声明顺序分配。为什么不另开槽：
//
//   - VulkanRHI::CreateDescriptorPool() 里 COMBINED_IMAGE_SAMPLER 的池大小是
//     `3 + 5 * m_MaxMaterialCount`，加一个槽就要同步改这个乘数；而 layout 是
//     所有材质共用的一份，加槽等于给所有材质都加。
//   - bindless 也不能拿来解这件事：RHIBindlessTextureManager 的注释明确要求
//     调用方先 supportsBindlessTextures() 守卫，后端不一定支持。
//
// 于是代价很清楚：**一个 shader 最多 5 张 2D 贴图，且改 .shader 里属性的声明
// 顺序会换槽位**。换槽位不是静默错 —— 槽位是导入期算好存进资产的，改了属性
// 顺序就会重新导入，GLSL 侧的 binding 声明跟着变，读错的话画面立刻不对。
//
// 脚本侧仍然是**完全具名**的：SetTexture("_BumpMap", tex) 不用管它在第几槽，
// 名字 -> 槽位的翻译在这里一次性做完。
//
// 和 ShaderPropertyBlob.h 一样是 header-only：Runtime/CMakeLists.txt 在 Web /
// Apple 配置下会把 `Function/ShaderLab/*.cpp` 整目录剔除，而消费方（RenderType.h
// 及其下游）全配置都编。
//

#include <cstdint>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{
    /// 材质描述符集里留给贴图的槽数（binding 1..5）。
    inline constexpr uint32_t k_MaterialTextureSlotCount = 5u;

    /// 没分到槽。要么不是 2D 属性，要么超出了 5 张。
    inline constexpr int32_t k_NoTextureSlot = -1;

    /// 只有 2D 占槽。Cube / 3D 在材质描述符集里没有对应的 binding —— 分个槽
    /// 给它只会让 shader 读到一张根本没绑定的贴图。
    inline bool IsTextureSlotType(const std::string& type)
    {
        return type == "2D" || type == "2d" || type == "sampler2D";
    }

    /// 分配函数的输入：一个属性的名字与 ShaderLab 类型串（PropertyTypeToString
    /// 的产物）。按 Properties{} 的**声明顺序**传进来。
    struct ShaderTextureSlotInput
    {
        std::string name;
        std::string type;
    };

    /// 分配结果。`slots` 与输入属性一一对应，非 2D 属性和超出第 5 张的都是
    /// k_NoTextureSlot。
    struct ShaderTextureSlotTable
    {
        std::vector<int32_t> slots;
        /// 真正拿到槽的条数（<= k_MaterialTextureSlotCount）。
        uint32_t assigned {0};
        /// 第 6 张及以后的条数。>0 意味着这个 shader 有贴图永远绑不上。
        uint32_t overflow {0};

        int32_t SlotAt(size_t index) const
        {
            return index < slots.size() ? slots[index] : k_NoTextureSlot;
        }
    };

    /// 按声明顺序给 2D 属性分配 0..4。
    inline ShaderTextureSlotTable AssignTextureSlots(const std::vector<ShaderTextureSlotInput>& properties)
    {
        ShaderTextureSlotTable table;
        table.slots.reserve(properties.size());
        for (const ShaderTextureSlotInput& property : properties)
        {
            if (!IsTextureSlotType(property.type))
            {
                table.slots.push_back(k_NoTextureSlot);
                continue;
            }
            if (table.assigned < k_MaterialTextureSlotCount)
            {
                table.slots.push_back(static_cast<int32_t>(table.assigned));
                ++table.assigned;
            }
            else
            {
                table.slots.push_back(k_NoTextureSlot);
                ++table.overflow;
            }
        }
        return table;
    }

    /// 单个贴图属性的绑定：属性名 -> 槽位。运行时落点表就长这样。
    struct ShaderTextureSlotBinding
    {
        std::string name;
        int32_t slot {k_NoTextureSlot};

        bool HasSlot() const { return slot >= 0 && static_cast<uint32_t>(slot) < k_MaterialTextureSlotCount; }
    };

    inline const ShaderTextureSlotBinding* FindTextureSlotBinding(const std::vector<ShaderTextureSlotBinding>& bindings,
                                                                 const char* name)
    {
        if (name == nullptr)
        {
            return nullptr;
        }
        for (const ShaderTextureSlotBinding& binding : bindings)
        {
            if (binding.name == name)
            {
                return &binding;
            }
        }
        return nullptr;
    }

    /// 内置 StandardLit 的贴图别名 -> 槽位。
    ///
    /// 内置 shader 没有 Properties{} 声明（它的 UBO 是那套写死的固定布局），
    /// 所以走不了上面那条反射路径。但脚本对自己材质的 `_MainTex` SetTexture
    /// 是 Unity 里最常见的用法之一，这里给一张小别名表接住。
    ///
    /// 槽位顺序与 MaterialSourceDesc 里那五个 File 字段一致。
    inline int32_t FindBuiltinTextureSlot(const char* name)
    {
        if (name == nullptr)
        {
            return k_NoTextureSlot;
        }
        const std::string key = name;
        if (key == "_MainTex" || key == "_BaseMap" || key == "_BaseColorMap")
        {
            return 0;
        }
        if (key == "_MetallicRoughnessMap" || key == "_MetallicGlossMap" || key == "_MRMap")
        {
            return 1;
        }
        if (key == "_BumpMap" || key == "_NormalMap")
        {
            return 2;
        }
        if (key == "_OcclusionMap")
        {
            return 3;
        }
        if (key == "_EmissionMap" || key == "_EmissiveMap")
        {
            return 4;
        }
        return k_NoTextureSlot;
    }

}  // namespace ZEngine::ShaderLab
