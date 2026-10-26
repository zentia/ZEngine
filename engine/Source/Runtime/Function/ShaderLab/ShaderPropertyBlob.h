#pragma once

//
// ShaderLab 具名属性 -> 材质 UBO 字节。
//
// 反射（ShaderCbufferReflection）回答的是"属性 `_Amount` 在 UnityPerMaterial 里的
// 第几个字节"；这一层回答的是"于是往材质 UBO 的第几个字节写几个 float"。两者分开
// 是因为前者要读 shader 源码（导入期跑一次），后者每帧都要跑。
//
// 两个刻意的约束：
//
//   1. **header-only，且只依赖 <cstring> / <string> / <vector>**。不碰 RHI、不碰
//      RenderType.h，于是配套测试能在没有 GPU / 没有 shader 编译器的机器上直接编
//      出来跑。也因为 Runtime/CMakeLists.txt 在 Web / Apple 等配置下会把
//      `Function/ShaderLab/*.cpp` 整目录剔除，而 RenderResourceBase.cpp（全配置
//      都编）要用这层 —— 有 .cpp 就会在那些配置上链接不到符号。
//
//   2. **128 字节 == sizeof(MeshMaterialUniform)**。材质 UBO 一共就这么大，内置
//      StandardLit 用它装那套固定布局（baseColorFactor / metallicFactor / ... /
//      customParams[4]），自定义 ShaderLab 材质用它装**反射出来的布局**。两者互斥，
//      由 GpuPBRMaterial::shaderlab_property_layout 区分 —— 同一块字节按两套布局
//      解释会静默读错，所以这个开关必须由材质数据决定，不能猜。
//

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ZEngine::ShaderLab
{
    /// 材质 UBO 的字节数。改这个数必须同步改 MeshMaterialUniform。
    inline constexpr uint32_t k_ShaderPropertyBlobBytes = 128u;

    /// 128 字节 = 32 个 float，正好用一个 uint32_t 做"每 float 是否被覆盖"的掩码。
    inline constexpr uint32_t k_ShaderPropertyBlobFloats = k_ShaderPropertyBlobBytes / sizeof(float);  // 32
    static_assert(k_ShaderPropertyBlobFloats == 32u, "blob mask is one uint32_t; keep the blob at 32 floats");

    /// 一个属性在 UBO 里的落点。由导入期算好（ShaderRes::ShaderPropertyDesc 的
    /// m_CbufferOffset / m_CbufferSize / m_CbufferComponents 抄过来）。
    struct ShaderPropertySlot
    {
        std::string name;
        uint32_t offset {0};
        uint32_t size {0};
        /// 分量数 1..4。矩阵记行数（那种走不了下面这套标量写入路径）。
        uint32_t components {0};

        bool HasSlot() const { return size > 0 && offset < k_ShaderPropertyBlobBytes; }

        /// 这条属性能吃几个 float。0 = 不该用标量路径写。
        ///
        /// 拒掉的三类：越界、size 不是 4 的倍数（解析没认出来的类型）、以及
        /// **超过 16 字节** —— 那只能是矩阵或数组，脚本一次 SetFloat / SetVector
        /// 根本表达不了它的值，按 components 写前几个 float 只是写出个半截矩阵。
        /// 宁可让 SetFloat 返回 false。
        uint32_t FloatCount() const
        {
            if (!HasSlot() || size > 16u || (offset % sizeof(float)) != 0 || (size % sizeof(float)) != 0)
            {
                return 0;
            }
            const uint32_t by_size = size / sizeof(float);
            const uint32_t count = (components > 0 && components < by_size) ? components : by_size;
            // 尾巴必须整个落在 blob 里：半个 float 写进去会把邻居的低位字节冲掉。
            return (offset + count * sizeof(float) <= k_ShaderPropertyBlobBytes) ? count : 0;
        }
    };

    /// 材质 UBO 的 CPU 侧镜像 + "哪些 float 被显式设过"的掩码。
    ///
    /// 掩码按 **float** 而不是按属性记：`SetFloat("_Amount", v)` 只该改那个标量，
    /// 不该顺手把同一条属性里的其它分量 override 成 0 —— 没覆盖的分量继续跟着资产
    /// / 上次的值走。跟 MaterialInstanceOverride::custom_component_mask 是同一个
    /// 思路，只是粒度从"4 个 vec4 的 16 个分量"放宽到"32 个 float"。
    struct ShaderPropertyBlob
    {
        uint8_t bytes[k_ShaderPropertyBlobBytes] {};
        uint32_t mask {0};

        void Clear()
        {
            std::memset(bytes, 0, sizeof(bytes));
            mask = 0;
        }

        /// 第 `float_index` 个 float（不是字节）被显式设过吗。
        bool HasFloat(uint32_t float_index) const
        {
            return float_index < k_ShaderPropertyBlobFloats && (mask & (1u << float_index)) != 0;
        }

        void SetFloat(uint32_t float_index, float value)
        {
            if (float_index >= k_ShaderPropertyBlobFloats)
            {
                return;
            }
            std::memcpy(bytes + float_index * sizeof(float), &value, sizeof(float));
            mask |= 1u << float_index;
        }

        /// 写值但**不打掩码**：铺资产默认值时用这个 —— 默认值不算"用户显式设过"。
        void SetFloatUnmasked(uint32_t float_index, float value)
        {
            if (float_index >= k_ShaderPropertyBlobFloats)
            {
                return;
            }
            std::memcpy(bytes + float_index * sizeof(float), &value, sizeof(float));
        }

        float GetFloat(uint32_t float_index) const
        {
            if (float_index >= k_ShaderPropertyBlobFloats)
            {
                return 0.0f;
            }
            float value = 0.0f;
            std::memcpy(&value, bytes + float_index * sizeof(float), sizeof(float));
            return value;
        }

        bool AnyOverride() const { return mask != 0; }
    };
    static_assert(sizeof(ShaderPropertyBlob) == k_ShaderPropertyBlobBytes + sizeof(uint32_t),
                  "ShaderPropertyBlob must stay POD-sized: it is memcpy'd into the UBO");

    /// 按属性名找落点。没有返回 nullptr。
    inline const ShaderPropertySlot* FindSlot(const std::vector<ShaderPropertySlot>& slots, std::string_view name)
    {
        if (name.empty())
        {
            return nullptr;
        }
        for (const ShaderPropertySlot& slot : slots)
        {
            if (slot.name == name)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    /// 把 `values[0..count)` 写进这条属性。**整条**写不完就一个字节都不写并返回
    /// false —— 半条属性是最难查的一类 bug。
    inline bool WriteSlot(ShaderPropertyBlob& blob,
                          const ShaderPropertySlot& slot,
                          const float* values,
                          uint32_t count)
    {
        if (values == nullptr)
        {
            return false;
        }

        const uint32_t capacity = slot.FloatCount();
        if (capacity == 0 || count == 0 || count > capacity)
        {
            return false;
        }

        const uint32_t first_float = slot.offset / sizeof(float);
        for (uint32_t index = 0; index < count; ++index)
        {
            blob.SetFloat(first_float + index, values[index]);
        }
        return true;
    }

    /// 只改这条属性的第 `component` 个分量，其余保持原值。
    inline bool WriteSlotComponent(ShaderPropertyBlob& blob,
                                   const ShaderPropertySlot& slot,
                                   uint32_t component,
                                   float value)
    {
        if (component >= slot.FloatCount())
        {
            return false;
        }
        blob.SetFloat(slot.offset / sizeof(float) + component, value);
        return true;
    }

    inline bool ReadSlot(const ShaderPropertyBlob& blob,
                         const ShaderPropertySlot& slot,
                         float* out_values,
                         uint32_t count)
    {
        if (out_values == nullptr)
        {
            return false;
        }

        const uint32_t capacity = slot.FloatCount();
        if (capacity == 0 || count == 0 || count > capacity)
        {
            return false;
        }

        const uint32_t first_float = slot.offset / sizeof(float);
        for (uint32_t index = 0; index < count; ++index)
        {
            out_values[index] = blob.GetFloat(first_float + index);
        }
        return true;
    }

    inline bool ReadSlotComponent(const ShaderPropertyBlob& blob,
                                  const ShaderPropertySlot& slot,
                                  uint32_t component,
                                  float& out_value)
    {
        if (component >= slot.FloatCount())
        {
            return false;
        }
        out_value = blob.GetFloat(slot.offset / sizeof(float) + component);
        return true;
    }

    /// 按名字写一条标量属性。名字不认识 / 落点不可用返回 false —— 调用方据此让
    /// SetFloat 返回 false，而不是静默往 UBO 里瞎写一个猜出来的偏移。
    inline bool SetNamedFloat(const std::vector<ShaderPropertySlot>& slots,
                              ShaderPropertyBlob& blob,
                              std::string_view name,
                              float value)
    {
        const ShaderPropertySlot* slot = FindSlot(slots, name);
        if (slot == nullptr)
        {
            return false;
        }
        return WriteSlotComponent(blob, *slot, 0, value);
    }

    /// 按名字读。没设过返回 false（调用方据此回落到资产默认值）。
    inline bool GetNamedFloat(const std::vector<ShaderPropertySlot>& slots,
                              const ShaderPropertyBlob& blob,
                              std::string_view name,
                              float& out_value)
    {
        const ShaderPropertySlot* slot = FindSlot(slots, name);
        if (slot == nullptr)
        {
            return false;
        }
        return ReadSlotComponent(blob, *slot, 0, out_value);
    }

    /// 按名字写一条向量属性。写 `min(4, 这条属性的分量数)` 个 float —— 对着一条
    /// `float _Amount` 调 SetVector 只会写它的第一个分量，而不是把后面三个属性冲掉。
    inline bool SetNamedVector(const std::vector<ShaderPropertySlot>& slots,
                               ShaderPropertyBlob& blob,
                               std::string_view name,
                               const float values[4])
    {
        const ShaderPropertySlot* slot = FindSlot(slots, name);
        if (slot == nullptr)
        {
            return false;
        }
        uint32_t count = slot->FloatCount();
        if (count == 0)
        {
            return false;
        }
        if (count > 4)
        {
            count = 4;
        }
        return WriteSlot(blob, *slot, values, count);
    }

    inline bool GetNamedVector(const std::vector<ShaderPropertySlot>& slots,
                               const ShaderPropertyBlob& blob,
                               std::string_view name,
                               float out_values[4])
    {
        const ShaderPropertySlot* slot = FindSlot(slots, name);
        if (slot == nullptr)
        {
            return false;
        }
        uint32_t count = slot->FloatCount();
        if (count == 0)
        {
            return false;
        }
        if (count > 4)
        {
            count = 4;
        }
        for (uint32_t index = count; index < 4; ++index)
        {
            out_values[index] = 0.0f;
        }
        return ReadSlot(blob, *slot, out_values, count);
    }

    /// 把 override 盖到 target 上：只动 mask 标记过的那些 float。
    ///
    /// 调用顺序是"资产默认值打底 -> ApplyOverride(per-renderer)"。反过来会把没被
    /// 覆盖的分量留在默认值上，看起来像 override 没生效。
    inline void ApplyOverride(const ShaderPropertyBlob& override_blob, ShaderPropertyBlob& target)
    {
        for (uint32_t float_index = 0; float_index < k_ShaderPropertyBlobFloats; ++float_index)
        {
            if (!override_blob.HasFloat(float_index))
            {
                continue;
            }
            target.SetFloatUnmasked(float_index, override_blob.GetFloat(float_index));
        }
        // 掩码也并上去，这样"当前生效值"一次就能读出来，不必再回头查 override。
        target.mask |= override_blob.mask;
    }

}  // namespace ZEngine::ShaderLab
