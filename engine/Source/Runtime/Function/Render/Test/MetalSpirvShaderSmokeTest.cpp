// =============================================================================
// 内置 shader 走 SPIR-V → MSL 的冒烟测试。
//
// 为什么需要它：Apple 上没有 Vulkan，但 MetalRHI 吃的是**同一份内置 SPIR-V**，
// 在 `CreateShaderModule()` 里用 SPIRV-Cross 转成 MSL，再交给运行时的
// `newLibraryWithSource:`。也就是说这一段里有两个环节构建期都验证不到：
//
//   1. **有没有把 SPIR-V 交给 Metal**。pass 里 `CreateShaderModule(<blob>)` 那几段
//      曾经用 `#if defined(Z_HAS_VULKAN)` 包着 —— Apple 不定义它，于是走 `#else`
//      返回 nullptr，pass 直接 throw。这和 `Z_HAS_LUMEN` 是同一类错：`Z_HAS_VULKAN`
//      回答的是"有没有 Vulkan"，不是"内置 shader 是不是 SPIR-V"。现在改用
//      `Z_HAS_SPIRV_SHADERS`（见 engine/CMakeLists.txt）。
//
//   2. **转出来的 MSL 过不过得了 Metal 编译器**。踩过的两个：
//      - `[[sampler(n)]]` 的 n 必须 <= 15，而 `[[texture(n)]]` 可以到 127 ⇒ 描述符
//        槽位（`set*8 + binding`，材质集已经在 17+）不能两边共用；
//      - `subpassInput` 不在 `sampled_images`/`separate_images` 里，漏给槽位的话
//        SPIRV-Cross 自己从 0 编号，会和别的 texture 撞车。
//
//   3. **转出来的 MSL 里的槽位编号，是不是 `CmdBindDescriptorSetsPFN` 会去绑的那一套**。
//      SPIRV-Cross 列资源时的遍历顺序跟我们绑描述符的顺序毫无关系，两边一旦错位，
//      Metal 不报错，shader 只是读到别人的 buffer。
//
// 所以这个测试做四件事（非 Apple 退化成 A + D）：
//   A. 每个 blob 是合法的 SPIR-V（长度 4 对齐 + 魔数）—— 全平台。
//   B. Apple 且没有 Vulkan 时，`Z_HAS_SPIRV_SHADERS` 必须是开的。
//   C. Apple 上真跑一遍 `CreateShaderModule()`：非 nullptr，且 sampler 紧凑编号
//      全部 <= 15（这是 MSL 的硬上限）。
//   D. 主机上用 SPIRV-Cross 转一遍 MSL，直接检查产物里的槽位编号 —— 全平台
//      （SPIRV-Cross 是纯 C++，不需要 Metal device），见 CheckHostMslTranslation()。
//
//   cmake -B build -DZENGINE_BUILD_METAL_SPIRV_SHADER_SMOKE_TEST=ON
//   cmake --build build --target MetalSpirvShaderSmokeTest
//
// D 段是 GPU Spine 那条线加的：它是引擎里唯一用非零 `firstInstance` 的调用方
// （绘制项下标靠 `gl_InstanceIndex` 传），也是唯一在顶点阶段挂 9 个 SSBO 的
// shader，槽位一旦漂就完全静默。以前这只有"在 Mac 上跑编辑器"才能发现。
// =============================================================================

#if defined(__APPLE__)
    #include "CommonPCH/pch.h"
#endif

#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(ZENGINE_METAL_SPIRV_SMOKE_HAS_SPIRV_CROSS)
    #include <spirv_cross.hpp>
    #include <spirv_msl.hpp>
#endif

// 内置 shader 的 SPIR-V 字节（engine/shader/generated/cpp，由 glslangValidator 生成）。
// 这些头在 pass 里是被 `#if defined(Z_HAS_SPIRV_SHADERS)` 包着的，但测试要**无条件**
// 拿得到 —— 否则"宏关错了"这个错自己就把证据吃掉了。
#include <deferred_lighting_frag.h>
#include <deferred_lighting_ssao_frag.h>
#include <deferred_lighting_vert.h>
#include <mesh_directional_light_shadow_frag.h>
#include <mesh_directional_light_shadow_vert.h>
#include <mesh_frag.h>
#include <mesh_gbuffer_frag.h>
#include <mesh_vert.h>
#include <ui_batched_frag.h>
#include <ui_batched_vert.h>
// GPU Spine：见下面 kBuiltinShaders 里的 spine_gpu 一条。
#include <spine_gpu_frag.h>
#include <spine_gpu_vert.h>

#if defined(__APPLE__)
    #include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
    #include "Runtime/Function/Render/Interface/Metal/MetalRHIResource.h"
    #include <Foundation/Foundation.h>
#endif

namespace
{
    int g_Checks   = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    const uint32_t k_msl_max_sampler_index = 15;

    struct ShaderBlob
    {
        const char*              name;
        const std::vector<unsigned char>* bytes;
    };

    // 顺序 = Apple 上场景管线真正用到的顺序（RP1 mesh → 延迟光照 → 阴影 → UI）。
    const ShaderBlob kBuiltinShaders[] = {
        {"mesh_vert", &MESH_VERT},
        {"mesh_gbuffer_frag", &MESH_GBUFFER_FRAG},
        {"mesh_frag", &MESH_FRAG},
        {"deferred_lighting_vert", &DEFERRED_LIGHTING_VERT},
        {"deferred_lighting_frag", &DEFERRED_LIGHTING_FRAG},
        // Metal 真正用的延迟光照（多一个 set1/binding4 的 SSAO 采样，sampler 7 个，必须 <= 15）。
        {"deferred_lighting_ssao_frag", &DEFERRED_LIGHTING_SSAO_FRAG},
        {"mesh_directional_light_shadow_vert", &MESH_DIRECTIONAL_LIGHT_SHADOW_VERT},
        {"mesh_directional_light_shadow_frag", &MESH_DIRECTIONAL_LIGHT_SHADOW_FRAG},
        {"ui_batched_vert", &UI_BATCHED_VERT},
        {"ui_batched_frag", &UI_BATCHED_FRAG},
        // GPU Spine（RP2 / swapchain pass）。两个"引擎里独一份"的地方，全靠这一段钉：
        //   - 顶点阶段 9 个 SSBO（b0..b8，含 deform b7 与 CPU 裁剪顶点 b8），
        //     是内置 shader 里最多的一个，槽位从 8 一直排到 16；
        //   - 唯一用非零 `firstInstance` 的调用方：绘制项下标 = gl_InstanceIndex。
        {"spine_gpu_vert", &SPINE_GPU_VERT},
        {"spine_gpu_frag", &SPINE_GPU_FRAG},
    };

    // ---- A. blob 是合法的 SPIR-V（全平台） ------------------------------------
    void CheckBlobSanity()
    {
        for (const ShaderBlob& blob : kBuiltinShaders)
        {
            const std::vector<unsigned char>& bytes = *blob.bytes;

            char message[256];

            std::snprintf(message, sizeof(message), "%s: blob 非空", blob.name);
            Check(!bytes.empty(), message);

            std::snprintf(message, sizeof(message), "%s: 长度按 4 字节对齐", blob.name);
            Check((bytes.size() % 4) == 0, message);

            // SPIR-V 魔数 0x07230203（小端存进来是 03 02 23 07）。
            const bool magic_ok = bytes.size() >= 4 && bytes[0] == 0x03 && bytes[1] == 0x02 &&
                                  bytes[2] == 0x23 && bytes[3] == 0x07;
            std::snprintf(message, sizeof(message), "%s: 魔数是 SPIR-V", blob.name);
            Check(magic_ok, message);
        }
    }

    // ---- B. Apple 上必须拿到 SPIR-V 那份 ---------------------------------------
    void CheckSpirvGuard()
    {
#if defined(__APPLE__) && !defined(Z_HAS_VULKAN)
        // Apple 编 Metal、没有 Vulkan，但内置 shader 仍然是 SPIR-V（MetalRHI 用
        // SPIRV-Cross 转）。`Z_HAS_SPIRV_SHADERS` 关掉 == pass 拿不到 shader == 启动
        // 就抛异常。这条就是钉死它的。
        #if defined(Z_HAS_SPIRV_SHADERS)
        Check(true, "Apple(无 Vulkan) 上 Z_HAS_SPIRV_SHADERS 是开的");
        #else
        Check(false, "Apple(无 Vulkan) 上 Z_HAS_SPIRV_SHADERS 是开的");
        #endif
#endif
    }

#if defined(__APPLE__)
    // ---- C. 真跑一遍 SPIR-V → MSL ---------------------------------------------
    void CheckMetalTranslation()
    {
        MetalRHI metal_rhi;
        if (!metal_rhi.Initialize())
        {
            std::printf("[skip] 这台机器没有 Metal device，GPU 相关检查跳过\n");
            return;
        }
        RHI* rhi = &metal_rhi;

        for (const ShaderBlob& blob : kBuiltinShaders)
        {
            RHIShader* shader = rhi->CreateShaderModule(*blob.bytes);

            char message[256];
            std::snprintf(message, sizeof(message), "%s: SPIR-V -> MSL 成功", blob.name);
            Check(shader != nullptr, message);

            if (shader != nullptr)
            {
                auto* metal_shader = static_cast<MetalShader*>(shader);
                bool  all_in_range = true;
                for (const auto& entry : metal_shader->samplerIndexBySlot)
                {
                    if (entry.second > k_msl_max_sampler_index)
                    {
                        all_in_range = false;
                    }
                }
                std::snprintf(message,
                              sizeof(message),
                              "%s: sampler 紧凑编号全部 <= %u (共 %u 个)",
                              blob.name,
                              k_msl_max_sampler_index,
                              static_cast<unsigned>(metal_shader->samplerIndexBySlot.size()));
                Check(all_in_range, message);
            }

            if (shader != nullptr)
            {
                rhi->DestroyShaderModule(shader);
            }
        }

        // 反例：`CreateShaderModule` 对非 SPIR-V 必须拒绝，而不是硬着头皮转。
        const std::vector<unsigned char> not_spirv = {0x00, 0x01, 0x02, 0x03};
        Check(rhi->CreateShaderModule(not_spirv) == nullptr, "非 SPIR-V 的字节被拒绝");

        metal_rhi.Shutdown();
    }
#endif

#if defined(ZENGINE_METAL_SPIRV_SMOKE_HAS_SPIRV_CROSS)
    // ---- D. 主机上转一遍 MSL，直接检查产物里的槽位编号 ------------------------
    //
    // SPIRV-Cross 是纯 C++，MSL 后端在 Windows / Linux 上一样编得出、跑得动，所以
    // "Metal 拿到的 MSL 长什么样"不必等到有人开 Mac 才知道。
    //
    // 判据刻意**不是**"我们推算的槽位 == 我们推算的槽位"，而是"转出来的 MSL 文本里
    // 真的写着 [[buffer(8..16)]] / [[texture(8)]] / [[sampler(0)]]" —— 后者才是
    // Metal 编译器看到的数字，也是 CmdBindDescriptorSetsPFN 去绑的下标。
    //
    // 期望值来自 MetalRHI 的常量（见 CheckMetalRhiConstants 的对齐检查）：
    //   buffer  = set * 8 + binding + 8     （前 8 个下标留给顶点 buffer）
    //   texture = set * 8 + binding
    //   sampler = 本 stage 内从 0 起的紧凑编号（MSL 硬上限 15）
    constexpr uint32_t k_msl_descriptor_buffer_offset = 8;
    constexpr uint32_t k_msl_descriptor_set_stride    = 8;

    struct MslResult
    {
        bool        ok     = false;
        std::string error;
        std::string source;
    };

    bool Contains(const std::string& haystack, const std::string& needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    // 抠出 MSL 里所有 `[[xxx(n)]]` 的下标（xxx = buffer / texture / sampler）。
    std::vector<uint32_t> MslIndices(const std::string& msl, const char* kind)
    {
        std::vector<uint32_t> indices;
        const std::string     tag = std::string("[[") + kind + "(";
        size_t                pos = 0;
        while ((pos = msl.find(tag, pos)) != std::string::npos)
        {
            pos += tag.size();
            const size_t end = msl.find(")]]", pos);
            if (end == std::string::npos)
            {
                break;
            }
            indices.push_back(static_cast<uint32_t>(std::strtoul(msl.substr(pos, end - pos).c_str(), nullptr, 10)));
            pos = end + 3;
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    }

    // 与 MetalRHI::CreateShaderModule 同一套配置（含 invariant_float_math：蒙皮公式
    // 必须走 spvFMul 的 fma，Metal 才有资格谈跟 Vulkan 逐位一致）。
    MslResult TranslateToMsl(const std::vector<unsigned char>& bytes)
    {
        MslResult result;
        if (bytes.size() < 4 || (bytes.size() % 4) != 0)
        {
            result.error = "不是 4 字节对齐的 SPIR-V";
            return result;
        }
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        try
        {
            spirv_cross::CompilerMSL compiler(std::move(words));
            const auto               entry_points = compiler.get_entry_points_and_stages();
            if (entry_points.empty())
            {
                result.error = "SPIR-V 里没有入口点";
                return result;
            }
            const std::string entry = entry_points[0].name;
            const auto        model = entry_points[0].execution_model;

            uint32_t next_sampler_index = 0;
            auto     add_msl_binding = [&](const spirv_cross::Resource& resource, bool is_buffer, bool has_sampler) {
                spirv_cross::MSLResourceBinding binding {};
                binding.stage     = model;
                binding.basetype  = compiler.get_type(resource.base_type_id).basetype;
                binding.desc_set  = compiler.has_decoration(resource.id, spv::DecorationDescriptorSet)
                                        ? compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)
                                        : 0u;
                binding.binding   = compiler.has_decoration(resource.id, spv::DecorationBinding)
                                        ? compiler.get_decoration(resource.id, spv::DecorationBinding)
                                        : 0u;
                const uint32_t slot = binding.desc_set * k_msl_descriptor_set_stride + binding.binding;
                if (is_buffer)
                {
                    binding.msl_buffer = slot + k_msl_descriptor_buffer_offset;
                }
                else
                {
                    binding.msl_texture = slot;
                    if (has_sampler)
                    {
                        uint32_t sampler_index = next_sampler_index;
                        if (sampler_index > k_msl_max_sampler_index)
                        {
                            sampler_index = k_msl_max_sampler_index;
                        }
                        else
                        {
                            ++next_sampler_index;
                        }
                        binding.msl_sampler = sampler_index;
                    }
                    else
                    {
                        binding.msl_sampler = slot;
                    }
                }
                compiler.add_msl_resource_binding(binding);
            };

            const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            for (const spirv_cross::Resource& r : resources.uniform_buffers) add_msl_binding(r, true, false);
            for (const spirv_cross::Resource& r : resources.storage_buffers) add_msl_binding(r, true, false);
            for (const spirv_cross::Resource& r : resources.sampled_images) add_msl_binding(r, false, true);
            for (const spirv_cross::Resource& r : resources.separate_images) add_msl_binding(r, false, false);
            for (const spirv_cross::Resource& r : resources.separate_samplers) add_msl_binding(r, false, true);
            for (const spirv_cross::Resource& r : resources.subpass_inputs) add_msl_binding(r, false, false);
            for (const spirv_cross::Resource& r : resources.storage_images) add_msl_binding(r, false, false);

            spirv_cross::CompilerMSL::Options options;
            options.enable_decoration_binding     = true;
            options.set_msl_version(2, 1, 0);
            options.pad_fragment_output_components = true;
            options.invariant_float_math           = true;
            compiler.set_msl_options(options);
            compiler.set_entry_point(entry, model);
            result.source = compiler.compile();
            result.ok     = !result.source.empty();
        }
        catch (const std::exception& e)
        {
            result.error = e.what();
        }
        return result;
    }

    // GPU Spine 是两个"引擎里独一份"，所以单拎出来逐条钉：
    //   - 顶点阶段 9 个 SSBO，是内置 shader 里最多的（b0..b8，含 deform b7 与
    //     CPU 裁剪顶点 b8）；
    //   - 唯一用非零 firstInstance 的调用方：绘制项下标 = gl_InstanceIndex。
    void CheckSpineGpuMsl(const std::string& vert, const std::string& frag)
    {
        Check(!vert.empty() && !frag.empty(), "spine_gpu: 两个 stage 的 MSL 都拿到了");
        if (vert.empty() || frag.empty())
        {
            return;
        }

        // 顶点阶段：9 个 SSBO 正好占满 [[buffer(8..16)]]，一个不多一个不少。
        // 多一个（比如再加一路 SSBO）就顶到 17，跟顶点 buffer 槽位撞车；少一个
        // 说明有 binding 没拿到槽位，SPIRV-Cross 会自己从 0 开始编号。
        // 起点写死成 8（= set0 * 8 + binding0 + 8），不跟着上面的常量走：那条常量
        // 是喂给 SPIRV-Cross 的输入，跟着它比就是自己跟自己比了。
        constexpr uint32_t k_vert_buffer_first = 8;
        constexpr uint32_t k_vert_buffer_count = 9;
        const std::vector<uint32_t> vert_buffers = MslIndices(vert, "buffer");
        bool                        vert_buffers_ok = vert_buffers.size() == k_vert_buffer_count;
        for (uint32_t i = 0; i < k_vert_buffer_count && vert_buffers_ok; ++i)
        {
            vert_buffers_ok = vert_buffers[i] == k_vert_buffer_first + i;
        }
        Check(vert_buffers_ok, "spine_gpu_vert: 9 个 SSBO 正好落在 [[buffer(8..16)]]");

        // 绘制项下标：Metal 的 [[instance_id]] 含 baseInstance（Apple 文档：
        // instance_id 从 baseInstance 递增到 baseInstance + instanceCount - 1），
        // 所以 SpineGpuRenderer 传的 firstInstance = 绘制项下标是有效的。
        Check(Contains(vert, "[[instance_id]]"), "spine_gpu_vert: gl_InstanceIndex -> [[instance_id]]");

        // 顶点下标同样不能被 baseVertex 之外的东西改写。
        Check(Contains(vert, "[[vertex_id]]"), "spine_gpu_vert: gl_VertexIndex -> [[vertex_id]]");

        // invariant_float_math：蒙皮公式走 spvFMul 的 fma。丢了它 Metal 会把
        // fma 拆成 mul+add，跟 Vulkan 的逐位一致性就没了（而且不报错）。
        Check(Contains(vert, "spvFMul"), "spine_gpu_vert: invariant_float_math 生效（spvFMul 在）");

        // 片元阶段：只再要一次 Draws（set0 b5 ⇒ buffer 13）+ 图集（set1 b0 ⇒
        // texture 8、sampler 紧凑编号 0）。
        Check(Contains(frag, "[[buffer(13)]]"), "spine_gpu_frag: Draws 在 [[buffer(13)]]");
        Check(Contains(frag, "[[texture(8)]]"), "spine_gpu_frag: 图集在 [[texture(8)]]");
        Check(Contains(frag, "[[sampler(0)]]"), "spine_gpu_frag: 图集采样器在 [[sampler(0)]]");
    }

    void CheckHostMslTranslation()
    {
        std::string vert_msl;
        std::string frag_msl;
        for (const ShaderBlob& blob : kBuiltinShaders)
        {
            const MslResult msl = TranslateToMsl(*blob.bytes);

            char message[256];
            std::snprintf(message, sizeof(message), "%s: SPIR-V -> MSL 成功", blob.name);
            Check(msl.ok, message);
            if (!msl.ok)
            {
                std::printf("       %s\n", msl.error.c_str());
                continue;
            }

            // sampler 紧凑编号 <= 15：MSL 的硬上限，超了 Metal 编译器直接拒收。
            // 槽位号可以是 17、21 这种数，所以两边各有各的编号表（见 MetalRHI）。
            bool samplers_in_range = true;
            for (uint32_t index : MslIndices(msl.source, "sampler"))
            {
                if (index > k_msl_max_sampler_index)
                {
                    samplers_in_range = false;
                }
            }
            std::snprintf(message, sizeof(message), "%s: MSL 里的 sampler 下标全部 <= %u", blob.name,
                          k_msl_max_sampler_index);
            Check(samplers_in_range, message);

            const std::string name(blob.name);
            if (name == "spine_gpu_vert")
            {
                vert_msl = msl.source;
            }
            else if (name == "spine_gpu_frag")
            {
                frag_msl = msl.source;
            }
        }
        CheckSpineGpuMsl(vert_msl, frag_msl);
    }
#endif

#if defined(ZENGINE_METAL_SPIRV_SMOKE_HAS_SPIRV_CROSS) && defined(ZENGINE_ENGINE_ROOT_DIR)
    // 上面对 MSL 的期望值是字面量，跟 MetalRHI 的常量天然是两份。这两条把两份对齐：
    // MetalRHI 的偏移/步长一改，这里就红，而不是"测试跟着实现一起漂"。
    void CheckMetalRhiConstants()
    {
        const std::string path = std::string(ZENGINE_ENGINE_ROOT_DIR) +
                                 "/Source/Runtime/Function/Render/Interface/Metal/MetalRHI.mm";
        std::ifstream     file(path);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Check(!text.empty(), "MetalRHI.mm 读得到（对齐 MSL 槽位常量用）");
        if (text.empty())
        {
            return;
        }
        Check(Contains(text, "k_msl_descriptor_buffer_offset = 8"), "MetalRHI: buffer 偏移仍是 8");
        Check(Contains(text, "k_msl_descriptor_set_stride = 8"), "MetalRHI: set 步长仍是 8");
        Check(Contains(text, "k_msl_max_sampler_index = 15"), "MetalRHI: sampler 上限仍是 15");
        // 非零 firstInstance 必须真的传到 Metal（Spine 靠它传绘制项下标）。
        Check(Contains(text, "baseInstance:firstInstance"), "MetalRHI: firstInstance 传给了 baseInstance");
    }
#endif
}  // namespace

int main()
{
    std::printf("== 内置 shader: SPIR-V -> MSL ==\n");

    CheckBlobSanity();
    CheckSpirvGuard();

#if defined(__APPLE__)
    CheckMetalTranslation();
#endif

#if defined(ZENGINE_METAL_SPIRV_SMOKE_HAS_SPIRV_CROSS)
    CheckHostMslTranslation();
    #if defined(ZENGINE_ENGINE_ROOT_DIR)
    CheckMetalRhiConstants();
    #endif
#endif

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
