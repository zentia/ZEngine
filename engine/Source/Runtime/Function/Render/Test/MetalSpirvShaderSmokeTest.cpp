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
// 所以这个测试做三件事（非 Apple 退化成前一件）：
//   A. 每个 blob 是合法的 SPIR-V（长度 4 对齐 + 魔数）—— 全平台。
//   B. Apple 且没有 Vulkan 时，`Z_HAS_SPIRV_SHADERS` 必须是开的。
//   C. Apple 上真跑一遍 `CreateShaderModule()`：非 nullptr，且 sampler 紧凑编号
//      全部 <= 15（这是 MSL 的硬上限）。
//
//   cmake -B build -DZENGINE_BUILD_METAL_SPIRV_SHADER_SMOKE_TEST=ON
//   cmake --build build --target MetalSpirvShaderSmokeTest
// =============================================================================

#if defined(__APPLE__)
    #include "CommonPCH/pch.h"
#endif

#include "Runtime/Function/Render/Interface/RHI.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// 内置 shader 的 SPIR-V 字节（engine/shader/generated/cpp，由 glslangValidator 生成）。
// 这些头在 pass 里是被 `#if defined(Z_HAS_SPIRV_SHADERS)` 包着的，但测试要**无条件**
// 拿得到 —— 否则"宏关错了"这个错自己就把证据吃掉了。
#include <deferred_lighting_frag.h>
#include <deferred_lighting_vert.h>
#include <mesh_directional_light_shadow_frag.h>
#include <mesh_directional_light_shadow_vert.h>
#include <mesh_frag.h>
#include <mesh_gbuffer_frag.h>
#include <mesh_vert.h>
#include <ui_batched_frag.h>
#include <ui_batched_vert.h>

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
        {"mesh_directional_light_shadow_vert", &MESH_DIRECTIONAL_LIGHT_SHADOW_VERT},
        {"mesh_directional_light_shadow_frag", &MESH_DIRECTIONAL_LIGHT_SHADOW_FRAG},
        {"ui_batched_vert", &UI_BATCHED_VERT},
        {"ui_batched_frag", &UI_BATCHED_FRAG},
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
}  // namespace

int main()
{
    std::printf("== 内置 shader: SPIR-V -> MSL ==\n");

    CheckBlobSanity();
    CheckSpirvGuard();

#if defined(__APPLE__)
    CheckMetalTranslation();
#endif

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
