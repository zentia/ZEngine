#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{
    /// 常量缓冲的打包规则。
    ///
    /// 两套规则不一样，**不能混用** —— 同一个 cbuffer 声明在 GLSL 和 HLSL 下会
    /// 算出不同的 offset。分岔点在 vec2/vec3 上：
    ///
    ///   struct { float3 a; float2 b; };
    ///     std140      : a@0..12，b 要求 8 字节对齐 → 挪到 16，总 24
    ///     HLSL cbuffer: b 从 12 开始占 12..16，没跨 16 字节边界 → b@12，总 16
    ///
    /// 选错规则不会报错，只会让属性值写进错的字节 —— 所以这个枚举必须跟着
    /// shader 源语言走，不能写死。
    enum class ShaderCbufferPacking
    {
        /// GLSL / SPIR-V（Vulkan、Metal）。vec3 与 vec4 都要 16 字节对齐，
        /// 数组元素 stride 至少 16。
        Std140,
        /// HLSL / DXIL（DX12）。标量 4 字节对齐；向量和矩阵行**不能跨越 16 字节
        /// 寄存器边界**，跨了就整体挪到下一个寄存器。
        HlslCbuffer
    };

    /// 一个 shader 属性在常量缓冲里的落点。
    struct ShaderPropertyLayout
    {
        std::string name;
        /// 从 cbuffer 起始算的字节偏移。
        uint32_t offset {0};
        /// 占用的字节数（float3 = 12，float4 = 16，float4x4 = 64）。
        uint32_t size {0};
        /// 分量数 1..4。矩阵是**行数**。
        uint32_t components {0};
        /// 数组长度，非数组为 1。
        uint32_t array_count {1};
        /// 单个元素自身的字节数，**不含**数组 stride 被撑开的部分。
        /// `float _Curve[4]` 的 size 是 64（每个元素占满一个寄存器），
        /// 但 element_size 是 4 —— 写数组时要按 stride 走，读类型时要按这个走。
        uint32_t element_size {0};
        bool is_matrix {false};

        uint32_t ElementSize() const { return element_size; }
        /// 相邻两个元素起始地址之间的字节数。
        uint32_t ElementStride() const { return array_count > 1 ? size / array_count : size; }
    };

    /// 一个常量缓冲的完整布局。
    struct ShaderCbufferLayout
    {
        std::string buffer_name;
        std::vector<ShaderPropertyLayout> properties;
        /// 整个 cbuffer 的字节大小（std140 / HLSL 都会向上补齐到 16 的倍数）。
        uint32_t size {0};
        /// 源码里到底有没有这个 cbuffer。false 时 properties 为空 —— 调用方要
        /// 能区分「shader 没有属性」和「解析没找到」，前者是正常的。
        bool found {false};

        /// 按属性名查落点。查不到返回 nullptr（属性没被这个 cbuffer 引用）。
        const ShaderPropertyLayout* Find(const std::string& name) const;
    };

    /// 从 shader 源码里解析出指定常量缓冲的布局。
    ///
    /// 处理的写法：
    ///   - `CBUFFER_START(UnityPerMaterial) ... CBUFFER_END`（Unity 宏，见
    ///     engine/shader/include/common.h:31）—— HLSL
    ///   - 直接的 `cbuffer UnityPerMaterial { ... }` —— HLSL
    ///   - `layout(std140, binding = 0) uniform UnityPerMaterial { ... };`
    ///     —— GLSL / SPIR-V，Vulkan 与 Metal 走这条
    ///   - 注释（`//` / `/* */`）与预处理行（`#...`）会被跳过
    ///   - 尾部的 `: packoffset(c0)` / `: register(b0)` 会被忽略
    ///   - 非块的 `uniform sampler2D _Foo;` 会自动跳过，不会挡住后面的块
    ///
    /// 这是**文本解析**而不是 SPIR-V 反射，原因见 ShaderLab 链路报告：spirv-cross
    /// 现在只在 Apple 链接且 `SPIRV_CROSS_ENABLE_REFLECT=OFF`。文本解析的好处是
    /// 跨平台一致、不依赖任何外部库、纯 CPU 可测；代价是超出解析器能力的写法会
    /// 查不到属性（表现为 SetFloat 返回 false，而不是崩）。
    ///
    /// @param source_code  shader 源码。可以只含 CBUFFER 块，也可以是一整份
    ///                     HLSLPROGRAM / GLSLPROGRAM。
    /// @param buffer_name  要找的 cbuffer 名字，例如 "UnityPerMaterial"。
    /// @param packing      打包规则，跟着 shader 源语言走。
    ShaderCbufferLayout ReflectCbuffer(const std::string& source_code,
                                       const std::string& buffer_name,
                                       ShaderCbufferPacking packing);
}  // namespace ZEngine::ShaderLab
