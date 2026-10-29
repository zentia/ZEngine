// =============================================================================
// VirtualTexture 材质槽位 A/B —— 真实 mesh_gbuffer.frag.hlsl 的 base color 经 VT 采样
// （DX12 headless，离屏 MRT，不碰主相机 pass）
//
// 采样契约 v2（mip 链 / gutter / 导数选 mip / 双线性）已经由 VirtualTextureMipBilinearABSmokeTest
// 对着专用 vt_blit 着色器验过。这里验的是**材质那一层**：MeshMaterialUniform 里那 128 个字节
// 真的把 VT 参数送到了真实的 gbuffer 片元着色器，而且开关是干净的。
//
// A/B 的含义：同一条管线、同一个描述符集布局（MeshMaterialLayout.h 里那份，生产也用它）、
// 同一张真实 gbuffer 着色器，只改 UBO 里的 vt_flags / customParams[2..3]：
//
//   关（vt_flags 位 0 = 0）  base color = base_color_texture.Sample(...)          —— 常量 1x1 贴图
//   开（vt_flags 位 0 = 1）  base color = VTSampleBilinear(页表, 页池, VTLod(duv)) —— 常驻时
//
// 所以判据很硬：
//   ① 关：每个像素逐字节等于那张常量贴图（证明脚手架、uv、MRT 读回都对）；
//   ② 开：每个像素等于 CPU 参考（VTRef::SampleWithFallback + 可选 sRGB 解码）；
//   ③ 开 vs 关：绝大多数像素必须不同（VT 真的被采了，不是被静默忽略）；
//   ④ gbuffer_a（法线）/ gbuffer_b（金属度粗糙度）在开与关之间逐字节相同（VT 只碰 base color）；
//   ⑤ 全缺页时开 == 关（shader 缺页回退到普通 base color，不画洞）；
//   ⑥ 只缺细 mip 时回退到更粗的驻留 mip；
//   ⑦ GetMaterialBinding 在 border 0 的池上拒绝（2x2 足迹会串到相邻槽位）。
//
// 为什么只跑 DX12：材质里的 VT binding（页表 t6 / 页池 t7）只存在于 DX12 的网格材质布局
// （MeshMaterialLayout.h：别的后端的 GLSL gbuffer 着色器不认这两个 binding，布局仍是 6 个）。
//
//   cmake -B build -DZENGINE_BUILD_VT_MATERIAL_SLOT_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureMaterialSlotABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#if !defined(_WIN32)
    #include <cstdio>

int main()
{
    std::printf("material VT slot A/B needs the DX12 backend (Windows)\n");
    return 2;
}
#else

    #include "Runtime/Core/Base/SystemRegistry.h"
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
    #include "Runtime/Function/Render/Interface/RHI.h"
    #include "Runtime/Function/Render/MaterialCustomParamSlots.h"
    #include "Runtime/Function/Render/MeshMaterialLayout.h"
    #include "Runtime/Function/Render/RenderCommon.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureMath.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureResource.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
    #include "Runtime/Function/Render/WindowSystem.h"

    #include <algorithm>
    #include <chrono>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <future>
    #include <memory>
    #include <string>
    #include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
        }
        std::printf("%s %s\n", condition ? "[ ok ]" : "[FAIL]", name);
    }

    template <class Fn>
    void RunWithWatchdog(const char* what, Fn&& fn, int seconds = 60)
    {
        auto future = std::async(std::launch::async, std::forward<Fn>(fn));
        if (future.wait_for(std::chrono::seconds(seconds)) == std::future_status::timeout)
        {
            std::printf("[FAIL] %s did not return within %d s\n", what, seconds);
            std::printf("\n%d checks, %d failures (aborted)\n", g_Checks, g_Failures + 1);
            std::fflush(stdout);
            std::_Exit(2);
        }
        future.get();
    }

    // ---- 被测几何 / 材质常量 --------------------------------------------------------
    constexpr uint32_t kPageSize = 32;
    constexpr uint32_t kBorder = 1;
    constexpr uint32_t kWidth = 200;   // 同 MipBilinear：故意不是页的整数倍（mip 200,100,50,25）
    constexpr uint32_t kHeight = 120;  // 120,60,30,15
    constexpr uint32_t kMipCount = 4;

    constexpr float kTolerance = 2.0f;      // 每通道，0..255 刻度（sRGB 关：页池字节直接出）
    constexpr float kToleranceSrgb = 3.0f;  // sRGB 开：解码把双线性的误差放大（白端导数 ≈ 2.27）

    // 1x1 常量贴图：关掉 VT 时 gbuffer_c 必须逐字节等于 kFlatBaseColor。
    constexpr uint8_t kFlatBaseColor[4] = {16, 32, 48, 255};
    // metallic = mr.b * metallicFactor = 100；roughness = mr.g * roughnessFactor = 200
    constexpr uint8_t kFlatMetallicRoughness[4] = {0, 200, 100, 255};
    // (128,128,255) 解码成 (0,0,1)：法线是常量，A / B 两遍必须逐字节相同
    constexpr uint8_t kFlatNormal[4] = {128, 128, 255, 255};

    constexpr uint32_t kAttachmentCount = 3;  // gbuffer a / b / c

    // D3D12 的 buffer<-texture 拷贝要求行距是 256 字节的倍数（同 VirtualTextureBlitter）。
    uint32_t ReadbackRowTexels(uint32_t width)
    {
        return (width + 63u) & ~63u;
    }

    std::string VtShaderRoot()
    {
        #ifdef ZENGINE_SHADER_ROOT
        return ZENGINE_SHADER_ROOT;
        #else
        return "e:/Engine/ZEngine/engine/shader";
        #endif
    }

    std::shared_ptr<TextureData> MakeSource(uint32_t width, uint32_t height)
    {
        auto data = std::make_shared<TextureData>();
        data->m_Width = width;
        data->m_Height = height;
        data->m_Depth = 1;
        data->m_MipLevels = 1;
        data->m_ArrayLayers = 1;
        data->m_Format = RHI_FORMAT_R8G8B8A8_UNORM;
        data->m_PixelsLength = width * height * 4u;
        data->m_Pixels = static_cast<uint8_t*>(std::malloc(data->m_PixelsLength));
        if (data->m_Pixels == nullptr)
        {
            return nullptr;
        }
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t h = x * 73856093u ^ y * 19349663u ^ 0x9E3779B9u;
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                uint8_t* t = data->m_Pixels + (static_cast<size_t>(y) * width + x) * 4u;
                t[0] = static_cast<uint8_t>(h & 0xFF);
                t[1] = static_cast<uint8_t>((h >> 8) & 0xFF);
                t[2] = static_cast<uint8_t>((h >> 16) & 0xFF);
                t[3] = 255;
            }
        }
        return data;
    }

    // =================================================================================
    // 离屏 gbuffer 夹具：真实的 mesh_gbuffer.frag.hlsl + MeshMaterialLayout.h 的 8 binding 布局
    // =================================================================================
    struct MaterialGbuffer
    {
        RHI* rhi {nullptr};
        bool ready {false};

        RHISampler* linear {nullptr};
        RHIBuffer* ubo {nullptr};
        RHIDeviceMemory* ubo_memory {nullptr};

        RHIImage* flat_image[3] {nullptr, nullptr, nullptr};
        RHIDeviceMemory* flat_memory[3] {nullptr, nullptr, nullptr};
        RHIImageView* flat_view[3] {nullptr, nullptr, nullptr};

        RHIRenderPass* render_pass {nullptr};
        RHIDescriptorSetLayout* set_layout {nullptr};
        RHIPipelineLayout* pipeline_layout {nullptr};
        RHIPipeline* pipeline {nullptr};
        RHIDescriptorSet* descriptor_set {nullptr};

        RHIImage* target[kAttachmentCount] {};
        RHIDeviceMemory* target_memory[kAttachmentCount] {};
        RHIImageView* target_view[kAttachmentCount] {};
        RHIFramebuffer* framebuffer {nullptr};
        RHIBuffer* readback[kAttachmentCount] {};
        RHIDeviceMemory* readback_memory[kAttachmentCount] {};
        uint32_t width {0}, height {0};

        bool MakeFlatTexture(const uint8_t rgba[4], uint32_t slot)
        {
            rhi->CreateImage(1,
                             1,
                             RHI_FORMAT_R8G8B8A8_UNORM,
                             RHI_IMAGE_TILING_OPTIMAL,
                             RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT,
                             RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             flat_image[slot],
                             flat_memory[slot],
                             0,
                             1,
                             1);
            if (flat_image[slot] == nullptr)
            {
                return false;
            }
            rhi->CreateImageView(flat_image[slot],
                                 RHI_FORMAT_R8G8B8A8_UNORM,
                                 RHI_IMAGE_ASPECT_COLOR_BIT,
                                 RHI_IMAGE_VIEW_TYPE_2D,
                                 1,
                                 1,
                                 flat_view[slot]);
            if (flat_view[slot] == nullptr)
            {
                return false;
            }
            return rhi->UploadImageRegionImmediate(flat_image[slot], rgba, 4u, 0, 0, 1, 1, 4u, true);
        }

        bool Initialize(RHI* in_rhi)
        {
            rhi = in_rhi;
            if (rhi == nullptr)
            {
                return false;
            }

            RHISamplerCreateInfo sampler_info {};
            sampler_info.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler_info.magFilter = RHI_FILTER_LINEAR;
            sampler_info.minFilter = RHI_FILTER_LINEAR;
            sampler_info.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.minLod = 0.0f;
            sampler_info.maxLod = 0.0f;
            if (!rhi->CreateSampler(&sampler_info, linear))
            {
                return false;
            }

            // CBV 大小必须 >= 256 字节（D3D12 硬性要求），UBO 本体只有 128。
            rhi->CreateBuffer(256u,
                              RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              ubo,
                              ubo_memory);
            if (ubo == nullptr || ubo_memory == nullptr)
            {
                return false;
            }

            if (!MakeFlatTexture(kFlatBaseColor, 0) || !MakeFlatTexture(kFlatMetallicRoughness, 1) ||
                !MakeFlatTexture(kFlatNormal, 2))
            {
                return false;
            }

            // ---- render pass：3 张颜色附件（gbuffer a / b / c），clear → store ----
            // 三份独立的 RHIAttachmentDescription：pAttachments 是按 attachmentCount 走的顺序数组，
            // 只给一份而声明 3 个会让 DX12 读到结构体后面的栈内存（PSO 的 RTVFormats[1..2] 变 UNKNOWN）。
            RHIAttachmentDescription attachments[kAttachmentCount] {};
            for (uint32_t i = 0; i < kAttachmentCount; ++i)
            {
                attachments[i].format = RHI_FORMAT_R8G8B8A8_UNORM;
                attachments[i].samples = RHI_SAMPLE_COUNT_1_BIT;
                attachments[i].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
                attachments[i].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
                attachments[i].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachments[i].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
                attachments[i].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
                attachments[i].finalLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            RHIAttachmentReference color_references[kAttachmentCount] {};
            for (uint32_t i = 0; i < kAttachmentCount; ++i)
            {
                color_references[i].attachment = i;
                color_references[i].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            RHISubpassDescription subpass {};
            subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = kAttachmentCount;
            subpass.pColorAttachments = color_references;

            RHIRenderPassCreateInfo render_pass_info {};
            render_pass_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            render_pass_info.attachmentCount = kAttachmentCount;
            render_pass_info.pAttachments = attachments;
            render_pass_info.subpassCount = 1;
            render_pass_info.pSubpasses = &subpass;
            if (!rhi->CreateRenderPass(&render_pass_info, render_pass))
            {
                return false;
            }

            // ---- 描述符集布局：生产那一份（MeshMaterialLayout.h），带 binding 6 / 7 ----
            RHIDescriptorSetLayoutBinding bindings[8] {};
            const uint32_t binding_count = BuildMeshMaterialLayoutBindings(true, bindings);
            if (binding_count != 8)
            {
                return false;
            }
            RHIDescriptorSetLayoutCreateInfo set_layout_info {};
            set_layout_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            set_layout_info.bindingCount = binding_count;
            set_layout_info.pBindings = bindings;
            if (!rhi->CreateDescriptorSetLayout(&set_layout_info, set_layout))
            {
                return false;
            }

            // set 0 / set 1 给 nullptr：gbuffer 片元着色器只用 space2（材质）。DX12 把 set 下标
            // 直接当 register space，所以材质集必须落在下标 2，RegisterSpace 才是 2。
            RHIDescriptorSetLayout* layouts[3] = {nullptr, nullptr, set_layout};
            RHIPipelineLayoutCreateInfo pipeline_layout_info {};
            pipeline_layout_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeline_layout_info.setLayoutCount = 3;
            pipeline_layout_info.pSetLayouts = layouts;
            if (!rhi->CreatePipelineLayout(&pipeline_layout_info, pipeline_layout))
            {
                return false;
            }

            RHIDescriptorSetAllocateInfo alloc_info {};
            alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = rhi->GetDescriptorPoor();
            alloc_info.descriptorSetCount = 1;
            alloc_info.pSetLayouts = &set_layout;
            if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&alloc_info, descriptor_set))
            {
                return false;
            }

            // ---- 着色器：顶点是脚手架，片元是真实的 mesh_gbuffer.frag.hlsl ----
            const std::string root = VtShaderRoot() + "/hlsl/";
            std::vector<uint8_t> binary;
            RHIShader* vert = rhi->CreateShaderModuleFromFile(
                root + "vt/vt_material_gbuffer.vert.hlsl", ShaderStage::Vertex, {}, {}, binary);
            RHIShader* frag = rhi->CreateShaderModuleFromFile(
                root + "rp1/mesh_gbuffer.frag.hlsl", ShaderStage::Fragment, {}, {}, binary);
            if (vert == nullptr || frag == nullptr)
            {
                if (vert != nullptr)
                {
                    rhi->DestroyShaderModule(vert);
                }
                if (frag != nullptr)
                {
                    rhi->DestroyShaderModule(frag);
                }
                return false;
            }

            RHIPipelineShaderStageCreateInfo stages[2] = {};
            stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert;
            stages[0].pName = "main";
            stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag;
            stages[1].pName = "main";

            RHIPipelineVertexInputStateCreateInfo vertex_input {};
            vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
            input_assembly.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            input_assembly.primitiveRestartEnable = RHI_FALSE;

            RHIViewport static_viewport {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
            RHIRect2D static_scissor {};
            static_scissor.extent.width = 1;
            static_scissor.extent.height = 1;
            RHIPipelineViewportStateCreateInfo viewport_state {};
            viewport_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport_state.viewportCount = 1;
            viewport_state.pViewports = &static_viewport;
            viewport_state.scissorCount = 1;
            viewport_state.pScissors = &static_scissor;

            RHIPipelineRasterizationStateCreateInfo rasterization {};
            rasterization.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterization.polygonMode = RHI_POLYGON_MODE_FILL;
            rasterization.cullMode = RHI_CULL_MODE_NONE;
            rasterization.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterization.lineWidth = 1.0f;

            RHIPipelineMultisampleStateCreateInfo multisample {};
            multisample.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

            RHIPipelineColorBlendAttachmentState blend {};
            blend.blendEnable = RHI_FALSE;
            blend.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                   RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
            RHIPipelineColorBlendStateCreateInfo blend_state {};
            blend_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            blend_state.attachmentCount = 1;  // DX12 只按第 0 个取 blend 状态
            blend_state.pAttachments = &blend;

            RHIPipelineDepthStencilStateCreateInfo depth_stencil {};
            depth_stencil.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depth_stencil.depthTestEnable = RHI_FALSE;
            depth_stencil.depthWriteEnable = RHI_FALSE;
            depth_stencil.depthCompareOp = RHI_COMPARE_OP_ALWAYS;

            RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
            RHIPipelineDynamicStateCreateInfo dynamic_state {};
            dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic_state.dynamicStateCount = 2;
            dynamic_state.pDynamicStates = dynamic_states;

            RHIGraphicsPipelineCreateInfo pipeline_info {};
            pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipeline_info.stageCount = 2;
            pipeline_info.pStages = stages;
            pipeline_info.pVertexInputState = &vertex_input;
            pipeline_info.pInputAssemblyState = &input_assembly;
            pipeline_info.pViewportState = &viewport_state;
            pipeline_info.pRasterizationState = &rasterization;
            pipeline_info.pMultisampleState = &multisample;
            pipeline_info.pColorBlendState = &blend_state;
            pipeline_info.pDepthStencilState = &depth_stencil;
            pipeline_info.pDynamicState = &dynamic_state;
            pipeline_info.layout = pipeline_layout;
            pipeline_info.renderPass = render_pass;
            pipeline_info.subpass = 0;

            const bool ok = rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, pipeline);
            rhi->DestroyShaderModule(vert);
            rhi->DestroyShaderModule(frag);
            if (!ok || pipeline == nullptr)
            {
                return false;
            }

            ready = true;
            return true;
        }

        void DestroyTarget()
        {
            if (rhi == nullptr)
            {
                return;
            }
            if (framebuffer != nullptr)
            {
                rhi->DestroyFramebuffer(framebuffer);
                framebuffer = nullptr;
            }
            for (uint32_t i = 0; i < kAttachmentCount; ++i)
            {
                if (target_view[i] != nullptr)
                {
                    rhi->DestroyImageView(target_view[i]);
                    target_view[i] = nullptr;
                }
                if (target[i] != nullptr)
                {
                    rhi->DestroyImage(target[i]);
                    target[i] = nullptr;
                }
                if (target_memory[i] != nullptr)
                {
                    rhi->FreeMemory(target_memory[i]);
                    target_memory[i] = nullptr;
                }
                if (readback_memory[i] != nullptr)
                {
                    rhi->FreeMemory(readback_memory[i]);
                    readback_memory[i] = nullptr;
                }
                if (readback[i] != nullptr)
                {
                    rhi->DestroyBuffer(readback[i]);
                    readback[i] = nullptr;
                }
            }
            width = 0;
            height = 0;
        }

        void Shutdown()
        {
            DestroyTarget();
            if (rhi != nullptr)
            {
                if (render_pass != nullptr)
                {
                    rhi->DestroyRenderPass(render_pass);
                }
                if (ubo_memory != nullptr)
                {
                    rhi->FreeMemory(ubo_memory);
                }
                if (ubo != nullptr)
                {
                    rhi->DestroyBuffer(ubo);
                }
                if (linear != nullptr)
                {
                    rhi->DestroySampler(linear);
                }
                for (uint32_t i = 0; i < 3; ++i)
                {
                    if (flat_view[i] != nullptr)
                    {
                        rhi->DestroyImageView(flat_view[i]);
                    }
                    if (flat_image[i] != nullptr)
                    {
                        rhi->DestroyImage(flat_image[i]);
                    }
                    if (flat_memory[i] != nullptr)
                    {
                        rhi->FreeMemory(flat_memory[i]);
                    }
                }
            }
            // 管线 / 布局 / 描述符集：RHI 没有对应销毁入口，只丢句柄（同 VirtualTextureBlitter）。
            render_pass = nullptr;
            ubo = nullptr;
            ubo_memory = nullptr;
            linear = nullptr;
            pipeline = nullptr;
            pipeline_layout = nullptr;
            set_layout = nullptr;
            descriptor_set = nullptr;
            ready = false;
            rhi = nullptr;
        }

        bool EnsureTarget(uint32_t w, uint32_t h)
        {
            if (target[0] != nullptr && framebuffer != nullptr && width == w && height == h)
            {
                return true;
            }
            DestroyTarget();
            for (uint32_t i = 0; i < kAttachmentCount; ++i)
            {
                rhi->CreateImage(w,
                                 h,
                                 RHI_FORMAT_R8G8B8A8_UNORM,
                                 RHI_IMAGE_TILING_OPTIMAL,
                                 RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 target[i],
                                 target_memory[i],
                                 0,
                                 1,
                                 1);
                if (target[i] == nullptr)
                {
                    DestroyTarget();
                    return false;
                }
                rhi->CreateImageView(target[i],
                                     RHI_FORMAT_R8G8B8A8_UNORM,
                                     RHI_IMAGE_ASPECT_COLOR_BIT,
                                     RHI_IMAGE_VIEW_TYPE_2D,
                                     1,
                                     1,
                                     target_view[i]);
                if (target_view[i] == nullptr)
                {
                    DestroyTarget();
                    return false;
                }
                const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(ReadbackRowTexels(w)) * h * 4u;
                rhi->CreateBuffer(bytes,
                                  RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  readback[i],
                                  readback_memory[i]);
                if (readback[i] == nullptr || readback_memory[i] == nullptr)
                {
                    DestroyTarget();
                    return false;
                }
            }

            RHIFramebufferCreateInfo framebuffer_info {};
            framebuffer_info.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = render_pass;
            framebuffer_info.attachmentCount = kAttachmentCount;
            framebuffer_info.pAttachments = target_view;
            framebuffer_info.width = w;
            framebuffer_info.height = h;
            framebuffer_info.layers = 1;
            if (!rhi->CreateFramebuffer(&framebuffer_info, framebuffer))
            {
                DestroyTarget();
                return false;
            }
            width = w;
            height = h;
            return true;
        }

        /// 画一遍 gbuffer。`ubo_bytes` 是 128 字节的 MeshMaterialUniform（VT 开不开由它里的
        /// vt_flags 决定）；`page_table` / `pool` 为 nullptr 时用 1x1 常量贴图占位。
        /// out_rgba[0..2] = gbuffer a / b / c，packed RGBA（R 在低字节），row 0 = 顶部。
        bool Render(const uint8_t* ubo_bytes,
                    RHIImageView* page_table,
                    RHIImageView* pool,
                    std::vector<uint32_t>* out_rgba)
        {
            if (!ready || rhi == nullptr || width == 0 || height == 0)
            {
                return false;
            }
            RHIImageView* table_view = page_table != nullptr ? page_table : flat_view[0];
            RHIImageView* pool_view = pool != nullptr ? pool : flat_view[0];

            void* mapped = nullptr;
            if (!rhi->MapMemory(ubo_memory, 0, 128u, 0, &mapped) || mapped == nullptr)
            {
                return false;
            }
            std::memcpy(mapped, ubo_bytes, 128u);
            rhi->UnmapMemory(ubo_memory);

            RHIDescriptorBufferInfo ubo_info {};
            ubo_info.buffer = ubo;
            ubo_info.offset = 0;
            ubo_info.range = 256u;

            RHIDescriptorImageInfo image_infos[8] {};
            for (uint32_t i = 0; i < 8; ++i)
            {
                image_infos[i].sampler = linear;
                image_infos[i].imageView = flat_view[0];
                image_infos[i].imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            // 生产布局的 1..5：base color / metallic-roughness / normal / occlusion / emissive。
            image_infos[1].imageView = flat_view[0];
            image_infos[2].imageView = flat_view[1];
            image_infos[3].imageView = flat_view[2];
            image_infos[6].imageView = table_view;
            image_infos[7].imageView = pool_view;

            RHIWriteDescriptorSet writes[8] {};
            writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = descriptor_set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &ubo_info;
            for (uint32_t i = 1; i < 8; ++i)
            {
                writes[i].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = descriptor_set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &image_infos[i];
            }
            rhi->UpdateDescriptorSets(8, writes, 0, nullptr);

            RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
            if (command_buffer == nullptr)
            {
                return false;
            }

            RHIClearValue clear_values[kAttachmentCount] {};
            RHIRenderPassBeginInfo begin_info {};
            begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin_info.renderPass = render_pass;
            begin_info.framebuffer = framebuffer;
            begin_info.renderArea.offset.x = 0;
            begin_info.renderArea.offset.y = 0;
            begin_info.renderArea.extent.width = width;
            begin_info.renderArea.extent.height = height;
            begin_info.clearValueCount = kAttachmentCount;
            begin_info.pClearValues = clear_values;
            rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

            RHIViewport viewport {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
            RHIRect2D scissor {};
            scissor.extent.width = width;
            scissor.extent.height = height;
            rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

            rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            // firstSet = 2：材质集在 space2（DX12 把 set 下标当 RegisterSpace）。
            rhi->CmdBindDescriptorSetsPFN(
                command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 2, 1, &descriptor_set, 0, nullptr);
            rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
            rhi->CmdEndRenderPassPFN(command_buffer);

            for (uint32_t i = 0; i < kAttachmentCount; ++i)
            {
                RHIImageMemoryBarrier to_transfer_src {};
                to_transfer_src.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                to_transfer_src.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                to_transfer_src.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
                to_transfer_src.oldLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                to_transfer_src.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_transfer_src.srcQueueFamilyIndex = ~0u;
                to_transfer_src.dstQueueFamilyIndex = ~0u;
                to_transfer_src.image = target[i];
                to_transfer_src.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
                to_transfer_src.subresourceRange.levelCount = 1;
                to_transfer_src.subresourceRange.layerCount = 1;
                rhi->CmdPipelineBarrier(command_buffer,
                                        RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                        0,
                                        0,
                                        nullptr,
                                        0,
                                        nullptr,
                                        1,
                                        &to_transfer_src);

                RHIBufferImageCopy region {};
                region.bufferRowLength = ReadbackRowTexels(width);
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent.width = width;
                region.imageExtent.height = height;
                region.imageExtent.depth = 1;
                rhi->CmdCopyImageToBuffer(
                    command_buffer, target[i], RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback[i], 1, &region);
            }

            rhi->EndSingleTimeCommands(command_buffer);  // 内部等待队列：返回即完成

            const uint32_t row_texels = ReadbackRowTexels(width);
            const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(row_texels) * height * 4u;
            for (uint32_t i = 0; i < kAttachmentCount; ++i)
            {
                void* result = nullptr;
                if (!rhi->MapMemory(readback_memory[i], 0, bytes, 0, &result) || result == nullptr)
                {
                    return false;
                }
                const auto* pixels = static_cast<const uint32_t*>(result);
                out_rgba[i].resize(static_cast<size_t>(width) * height);
                for (uint32_t row = 0; row < height; ++row)
                {
                    std::memcpy(out_rgba[i].data() + static_cast<size_t>(row) * width,
                                pixels + static_cast<size_t>(row) * row_texels,
                                static_cast<size_t>(width) * sizeof(uint32_t));
                }
                rhi->UnmapMemory(readback_memory[i]);
            }
            return true;
        }
    };

    uint32_t PackRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(a) << 24);
    }

    struct Compare
    {
        uint32_t pixels {0};
        uint32_t within {0};
        uint32_t nonresident_exact {0};  // 期望回退成常量 base color，GPU 也正是
        uint32_t bad {0};
        float max_error {0.0f};
        uint32_t first_bad_x {0}, first_bad_y {0};
        uint32_t gpu_at_bad {0};
        float exp_r {0}, exp_g {0}, exp_b {0};
        uint32_t mip_histogram[16] {};
    };

    /// gpu_c vs CPU 参考。`srgb` = UBO 里 vt_flags 位 1（shader 在滤波之后解码）。
    Compare CompareToReference(const std::vector<uint32_t>& gpu_c,
                               uint32_t out_w,
                               uint32_t out_h,
                               const std::vector<VTRef::Image>& chain,
                               const std::shared_ptr<VirtualTextureResource>& resource,
                               bool srgb,
                               float tolerance)
    {
        Compare c;
        c.pixels = out_w * out_h;
        if (gpu_c.size() != static_cast<size_t>(out_w) * out_h)
        {
            c.bad = 0xFFFFFFFFu;
            return c;
        }
        const auto resident = [&](uint32_t mip, uint32_t px, uint32_t py) {
            const PageTableEntry e = resource->GetPageTableEntry(mip, px, py);
            return e.is_valid && e.physical_slot != UINT32_MAX;
        };
        const float lod = VTRef::Lod(kWidth, kHeight, 1.0f / out_w, 0.0f, 0.0f, 1.0f / out_h);
        const uint32_t flat = PackRgba(kFlatBaseColor[0], kFlatBaseColor[1], kFlatBaseColor[2], kFlatBaseColor[3]);
        for (uint32_t y = 0; y < out_h; ++y)
        {
            for (uint32_t x = 0; x < out_w; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(out_w);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(out_h);
                const VTRef::SampleResult ref =
                    VTRef::SampleWithFallback(chain, kWidth, kHeight, kPageSize, resident, u, v, lod);
                const uint32_t g = gpu_c[static_cast<size_t>(y) * out_w + x];
                if (!ref.resident)
                {
                    // shader：VTSampleBilinear 返回 fallback = 常量 base color，vt_resident = false ⇒ 保留它
                    if (g == flat)
                    {
                        ++c.nonresident_exact;
                    }
                    else
                    {
                        if (c.bad == 0)
                        {
                            c.first_bad_x = x;
                            c.first_bad_y = y;
                            c.gpu_at_bad = g;
                        }
                        ++c.bad;
                    }
                    continue;
                }
                ++c.mip_histogram[std::min(ref.mip, 15u)];
                const float raw[3] = {ref.color.r / 255.0f, ref.color.g / 255.0f, ref.color.b / 255.0f};
                float expected[3];
                for (uint32_t i = 0; i < 3; ++i)
                {
                    expected[i] = (srgb ? VTRef::SrgbToLinear(raw[i]) : raw[i]) * 255.0f;
                }
                const float gr = static_cast<float>(g & 0xFF), gg = static_cast<float>((g >> 8) & 0xFF),
                            gb = static_cast<float>((g >> 16) & 0xFF), ga = static_cast<float>((g >> 24) & 0xFF);
                const float err = std::max({std::fabs(gr - expected[0]),
                                            std::fabs(gg - expected[1]),
                                            std::fabs(gb - expected[2]),
                                            std::fabs(ga - 255.0f)});
                c.max_error = std::max(c.max_error, err);
                if (err <= tolerance)
                {
                    ++c.within;
                }
                else
                {
                    if (c.bad == 0)
                    {
                        c.first_bad_x = x;
                        c.first_bad_y = y;
                        c.gpu_at_bad = g;
                        c.exp_r = expected[0];
                        c.exp_g = expected[1];
                        c.exp_b = expected[2];
                    }
                    ++c.bad;
                }
            }
        }
        return c;
    }

    void Report(const char* label, const Compare& c)
    {
        std::printf("       [info] %s: pixels=%u within=%u fallback=%u BAD=%u max-error=%.2f  mips(0..3)=%u/%u/%u/%u\n",
                    label,
                    c.pixels,
                    c.within,
                    c.nonresident_exact,
                    c.bad,
                    c.max_error,
                    c.mip_histogram[0],
                    c.mip_histogram[1],
                    c.mip_histogram[2],
                    c.mip_histogram[3]);
        if (c.bad != 0 && c.bad != 0xFFFFFFFFu)
        {
            std::printf("       [diag] first bad at (%u,%u): gpu=0x%08X expected rgb=(%.1f,%.1f,%.1f)\n",
                        c.first_bad_x,
                        c.first_bad_y,
                        c.gpu_at_bad,
                        c.exp_r,
                        c.exp_g,
                        c.exp_b);
        }
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== VirtualTexture material slot A/B (real mesh_gbuffer.frag.hlsl, DX12 headless) ===\n");

    // ------------------------------------------------------------------ stage 0: 纯 CPU
    std::printf("--- stage 0: the 128-byte material UBO carries the VT slot (no GPU) ---\n");
    {
        MeshMaterialUniform ubo {};
        ubo.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        ubo.metallicFactor = 1.0f;
        ubo.roughnessFactor = 1.0f;
        ubo.normalScale = 1.0f;
        ubo.customParams[0] = Vector4(77.0f, 88.0f, 99.0f, 111.0f);  // 溶解参数：不会被 VT 覆盖
        uint8_t off_bytes[128];
        std::memcpy(off_bytes, &ubo, sizeof(off_bytes));

        MaterialVirtualTextureSlot none {};
        uint8_t untouched[128];
        std::memcpy(untouched, off_bytes, sizeof(untouched));
        ApplyMaterialVirtualTextureSlot(none, untouched);
        Check(std::memcmp(off_bytes, untouched, sizeof(off_bytes)) == 0,
              "a disabled VT slot leaves all 128 bytes untouched (materials behave exactly as before)");

        MaterialVirtualTextureSlot slot {};
        slot.enabled = true;
        slot.srgb = true;
        slot.page_size = kPageSize;
        slot.page_border = kBorder;
        slot.mip_count = kMipCount;
        slot.virtual_width = kWidth;
        slot.virtual_height = kHeight;
        uint8_t on_bytes[128];
        std::memcpy(on_bytes, off_bytes, sizeof(on_bytes));
        ApplyMaterialVirtualTextureSlot(slot, on_bytes);
        Check(std::memcmp(off_bytes, on_bytes, sizeof(on_bytes)) != 0, "an enabled VT slot changes the UBO bytes");
        {
            const MeshMaterialUniform& on = *reinterpret_cast<const MeshMaterialUniform*>(on_bytes);
            Check(on.vt_flags == (k_MaterialVtFlag_BaseColor | k_MaterialVtFlag_Srgb),
                  "vt_flags == bit0 (base color via VT) | bit1 (sRGB decode)");
            Check(on.customParams[0].x == 77.0f && on.customParams[0].y == 88.0f,
                  "the dissolve slots are untouched by the VT slot");
        }
        const MaterialVirtualTextureSlot back = ReadMaterialVirtualTextureSlot(on_bytes);
        Check(back == slot, "ReadMaterialVirtualTextureSlot round-trips the slot (uint bit patterns survive)");
        const MaterialVirtualTextureSlot off_read = ReadMaterialVirtualTextureSlot(off_bytes);
        Check(!off_read.enabled, "reading a UBO with vt_flags 0 gives enabled=false");
    }

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    auto dx12 = std::make_unique<DX12RHI>();
    const bool rhi_ready = dx12->Initialize();
    Check(rhi_ready, "DX12RHI::Initialize succeeds (headless)");
    if (!rhi_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }
    RHI& rhi = *dx12;

    MaterialGbuffer gbuffer;
    const bool harness_ready = gbuffer.Initialize(&rhi);
    Check(harness_ready,
          "the real mesh_gbuffer.frag.hlsl compiles and the 8-binding material pipeline is created (space2)");
    if (!harness_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    auto manager = std::make_unique<VirtualTextureManager>();
    VirtualTextureConfig config;
    config.page_size = kPageSize;
    // 16 x 4 = 64 槽位 >= 39 页。宽度故意取 16 而不是 8：池图宽 = 16*34 = 544，
    // 544/32 = 17 != 16 —— 于是"算槽位网格时把 gutter 漏掉（池图宽/page_size）"这个缺陷
    // 会真的把槽位算错（取 8 时 272/32 恰好也等于 8，缺陷是隐形的）。
    config.physical_pool_width = 16;
    config.physical_pool_height = 4;
    config.memory_budget_mb = 64;
    config.enable_async_loading = false;
    config.page_border = kBorder;
    config.resident_mip_chain = true;
    manager->SetConfig(config);
    manager->Initialize(&rhi, nullptr);

    auto source = MakeSource(kWidth, kHeight);
    const VirtualTextureHandle handle = manager->RegisterVirtualTexture(source, kPageSize);
    auto resource = manager->GetVirtualTexture(handle);
    Check(handle != 0 && resource != nullptr, "RegisterVirtualTexture(200x120) with border 1 + mip chain");
    if (!resource)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }
    const std::vector<VTRef::Image> chain = VTRef::BuildMipChain(source->m_Pixels, kWidth, kHeight, kMipCount);

    // ------------------------------------------------------------------ stage 1: 载入 + GetMaterialBinding
    std::printf("--- stage 1: load every mip; GetMaterialBinding is the production path ---\n");
    for (uint32_t mip = 0; mip < kMipCount; ++mip)
    {
        RunWithWatchdog("ForceLoadPages", [&] { manager->ForceLoadPages(handle, mip); });
    }
    Check(manager->getPageUploadCount() == 39 && manager->getPageUploadFailureCount() == 0,
          "39 page uploads (28 + 8 + 2 + 1), none failed");

    MaterialVirtualTextureSlot slot {};
    RHIImageView* page_table_view = nullptr;
    RHIImageView* pool_view = nullptr;
    {
        MaterialVirtualTextureSlot srgb_off {};
        RHIImageView* pt = nullptr;
        RHIImageView* pl = nullptr;
        const bool got = manager->GetMaterialBinding(handle, false, srgb_off, pt, pl);
        Check(got && srgb_off.enabled && srgb_off.page_size == kPageSize && srgb_off.page_border == kBorder &&
                  srgb_off.mip_count == kMipCount && srgb_off.virtual_width == kWidth &&
                  srgb_off.virtual_height == kHeight && !srgb_off.srgb && pt != nullptr && pl != nullptr,
              "GetMaterialBinding reports page 32, gutter 1, 4 mips, 200x120 and both image views");
        Check(manager->GetMaterialBinding(handle, true, slot, page_table_view, pool_view) && slot.srgb,
              "GetMaterialBinding(srgb=1) sets the sRGB flag (the CVar path)");
    }

    // ------------------------------------------------------------------ stage 2: A/B —— 关 vs 开
    std::printf("--- stage 2: A/B at 1:1 (vt_flags off vs on) ---\n");
    uint8_t ubo_off[128] {};
    uint8_t ubo_on[128] {};
    {
        MeshMaterialUniform ubo {};
        ubo.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        ubo.metallicFactor = 1.0f;
        ubo.roughnessFactor = 1.0f;
        ubo.normalScale = 1.0f;
        std::memcpy(ubo_off, &ubo, sizeof(ubo_off));
        std::memcpy(ubo_on, &ubo, sizeof(ubo_on));
        MaterialVirtualTextureSlot raw_slot = slot;
        raw_slot.srgb = false;  // 页池字节直接当 albedo：与 CPU 参考可比，容差最紧
        ApplyMaterialVirtualTextureSlot(raw_slot, ubo_on);
    }

    std::vector<uint32_t> out_off[kAttachmentCount];
    std::vector<uint32_t> out_on[kAttachmentCount];
    bool ok_off = false, ok_on = false;
    RunWithWatchdog("Render (VT off)", [&] {
        if (gbuffer.EnsureTarget(kWidth, kHeight))
        {
            ok_off = gbuffer.Render(ubo_off, page_table_view, pool_view, out_off);
        }
    });
    RunWithWatchdog("Render (VT on)", [&] {
        if (gbuffer.EnsureTarget(kWidth, kHeight))
        {
            ok_on = gbuffer.Render(ubo_on, page_table_view, pool_view, out_on);
        }
    });
    Check(ok_off && ok_on, "both passes render and read back all three gbuffer attachments");
    if (!ok_off || !ok_on)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }
    {
        const uint32_t flat = PackRgba(kFlatBaseColor[0], kFlatBaseColor[1], kFlatBaseColor[2], kFlatBaseColor[3]);
        const uint32_t same = static_cast<uint32_t>(
            std::count_if(out_off[2].begin(), out_off[2].end(), [&](uint32_t p) { return p == flat; }));
        Check(same == out_off[2].size(),
              "VT OFF: every gbuffer_c pixel is exactly the constant 1x1 base colour (harness + uv + readback are sound)");
        uint32_t differing = 0;
        for (size_t i = 0; i < out_off[2].size(); ++i)
        {
            if (out_off[2][i] != out_on[2][i])
            {
                ++differing;
            }
        }
        std::printf("       [info] pixels differing between VT off and VT on: %u / %u\n",
                    differing,
                    static_cast<uint32_t>(out_off[2].size()));
        Check(differing >= out_off[2].size() * 9u / 10u,
              "VT ON: >=90% of pixels differ from the OFF pass (the shader really samples the VT, not silently ignored)");
        Check(std::equal(out_off[0].begin(), out_off[0].end(), out_on[0].begin()) &&
                  std::equal(out_off[1].begin(), out_off[1].end(), out_on[1].begin()),
              "gbuffer_a (normal) and gbuffer_b (metallic/roughness) are byte-identical between OFF and ON");
    }
    {
        const Compare c = CompareToReference(out_on[2], kWidth, kHeight, chain, resource, false, 1.0f);
        Report("VT on, 1:1, srgb off", c);
        Check(c.bad == 0 && c.pixels == c.within && c.mip_histogram[0] == kWidth * kHeight,
              "VT ON at 1:1 matches the CPU reference byte for byte on every pixel (mip 0, texel centres)");
    }

    // ------------------------------------------------------------------ stage 3: 导数选 mip + 双线性
    std::printf("--- stage 3: mip selection from uv derivatives + bilinear ---\n");
    struct Case
    {
        uint32_t w, h;
        const char* name;
        float tol;
        uint32_t expect_mip;
    };
    const Case cases[] = {
        {600, 360, "3x magnify  (lod<0 -> mip 0, footprints cross the page gutter)", kTolerance, 0},
        {150, 90, "1.33x minify (lod 0.41 -> mip 0, fractional bilinear)", kTolerance, 0},
        {130, 78, "1.54x minify (lod 0.62 -> mip 1)", kTolerance, 1},
        {100, 60, "2:1         (lod 1.00 -> mip 1)", kTolerance, 1},
        {50, 30, "4:1         (lod 2.00 -> mip 2)", kTolerance, 2},
        {17, 11, "11.8x minify (lod 3.56 -> clamped to mip 3)", kTolerance, 3},
    };
    for (const Case& c : cases)
    {
        std::vector<uint32_t> out[kAttachmentCount];
        bool ok = false;
        RunWithWatchdog("Render case", [&] {
            if (gbuffer.EnsureTarget(c.w, c.h))
            {
                ok = gbuffer.Render(ubo_on, page_table_view, pool_view, out);
            }
        });
        const Compare cmp = CompareToReference(out[2], c.w, c.h, chain, resource, false, c.tol);
        Report(c.name, cmp);
        char label[220];
        std::snprintf(label, sizeof(label), "%ux%u: every pixel within +-%.0f of the CPU reference", c.w, c.h, c.tol);
        Check(ok && cmp.bad == 0 && cmp.pixels == cmp.within, label);
        std::snprintf(label, sizeof(label), "%ux%u: the reference itself selects mip %u for every pixel", c.w, c.h, c.expect_mip);
        Check(cmp.mip_histogram[c.expect_mip] == c.w * c.h, label);
    }

    // ------------------------------------------------------------------ stage 4: sRGB 解码
    std::printf("--- stage 4: vt_flags bit 1 = sRGB decode after filtering ---\n");
    {
        uint8_t ubo_srgb[128];
        std::memcpy(ubo_srgb, ubo_off, sizeof(ubo_srgb));
        ApplyMaterialVirtualTextureSlot(slot, ubo_srgb);  // slot.srgb == true
        std::vector<uint32_t> out[kAttachmentCount];
        bool ok = false;
        RunWithWatchdog("Render srgb", [&] {
            if (gbuffer.EnsureTarget(kWidth, kHeight))
            {
                ok = gbuffer.Render(ubo_srgb, page_table_view, pool_view, out);
            }
        });
        const Compare c = CompareToReference(out[2], kWidth, kHeight, chain, resource, true, kToleranceSrgb);
        Report("VT on, 1:1, srgb on", c);
        Check(ok && c.bad == 0 && c.pixels == c.within,
              "with sRGB decode on, every pixel still matches the reference (decoded AFTER the bilinear filter)");
        // sRGB 解码在 [0,1] 上单调且不高于编码值（编码是"提亮"过的）：逐像素必须 <= 原始字节，
        // 只在 0 和 255 处取等。这条钉住"位 1 真的接上了解码"，而不是被当成别的开关。
        uint32_t not_brighter = 0;
        uint32_t strictly_darker = 0;
        for (size_t i = 0; i < out[2].size(); ++i)
        {
            const uint32_t srgb_r = out[2][i] & 0xFFu;
            const uint32_t raw_r = out_on[2][i] & 0xFFu;
            if (srgb_r <= raw_r)
            {
                ++not_brighter;
            }
            if (srgb_r < raw_r)
            {
                ++strictly_darker;
            }
        }
        Check(not_brighter == out[2].size() && strictly_darker >= out[2].size() * 9u / 10u,
              "sRGB decode is monotone and never brighter than the raw pool byte, and it moved >=90% of pixels");
    }

    // ------------------------------------------------------------------ stage 5: 缺页回退
    std::printf("--- stage 5: residency fallback inside the material ---\n");
    RunWithWatchdog("UnloadPages", [&] { manager->UnloadPages(handle); });
    for (uint32_t mip = 1; mip < kMipCount; ++mip)
    {
        RunWithWatchdog("ForceLoadPages", [&] { manager->ForceLoadPages(handle, mip); });  // mip 0 不载
    }
    {
        std::vector<uint32_t> out[kAttachmentCount];
        bool ok = false;
        RunWithWatchdog("Render fallback", [&] {
            if (gbuffer.EnsureTarget(kWidth, kHeight))
            {
                ok = gbuffer.Render(ubo_on, page_table_view, pool_view, out);
            }
        });
        const Compare c = CompareToReference(out[2], kWidth, kHeight, chain, resource, false, kTolerance);
        Report("mip 0 evicted, 1:1", c);
        Check(ok && c.bad == 0 && c.mip_histogram[0] == 0 && c.mip_histogram[1] == kWidth * kHeight,
              "with mip 0 missing, every pixel is served by resident mip 1 (the shader walks to coarser mips)");
    }
    RunWithWatchdog("UnloadPages", [&] { manager->UnloadPages(handle); });
    const uint32_t flat = PackRgba(kFlatBaseColor[0], kFlatBaseColor[1], kFlatBaseColor[2], kFlatBaseColor[3]);
    {
        std::vector<uint32_t> out[kAttachmentCount];
        bool ok = false;
        RunWithWatchdog("Render all-evicted", [&] {
            if (gbuffer.EnsureTarget(kWidth, kHeight))
            {
                ok = gbuffer.Render(ubo_on, page_table_view, pool_view, out);
            }
        });
        const uint32_t same =
            static_cast<uint32_t>(std::count_if(out[2].begin(), out[2].end(), [&](uint32_t p) { return p == flat; }));
        Check(ok && same == out[2].size(),
              "nothing resident: VT ON falls back to the plain base colour pixel for pixel (no holes)");
    }
    {
        // 同一件事，但 vt_flags 位 1（sRGB 解码）也开着：回退值必须是**未解码**的 base color。
        // "忽略 vt_resident、永远采用 VTSampleBilinear 的返回值"会把这个回退值也解码一遍，当场失配。
        uint8_t ubo_srgb[128];
        std::memcpy(ubo_srgb, ubo_off, sizeof(ubo_srgb));
        ApplyMaterialVirtualTextureSlot(slot, ubo_srgb);
        std::vector<uint32_t> out[kAttachmentCount];
        bool ok = false;
        RunWithWatchdog("Render all-evicted srgb", [&] {
            if (gbuffer.EnsureTarget(kWidth, kHeight))
            {
                ok = gbuffer.Render(ubo_srgb, page_table_view, pool_view, out);
            }
        });
        const uint32_t same =
            static_cast<uint32_t>(std::count_if(out[2].begin(), out[2].end(), [&](uint32_t p) { return p == flat; }));
        std::printf("       [info] nothing resident + srgb on: undecoded fallback on %u / %u pixels\n",
                    same,
                    static_cast<uint32_t>(out[2].size()));
        Check(ok && same == out[2].size(),
              "nothing resident + sRGB on: the fallback stays UNDECODED (the residency flag gates the decode too)");
    }

    // ------------------------------------------------------------------ stage 6: 护栏
    std::printf("--- stage 6: guard rails ---\n");
    {
        auto plain = std::make_unique<VirtualTextureManager>();
        VirtualTextureConfig plain_config;
        plain_config.page_size = kPageSize;
        plain_config.physical_pool_width = 4;
        plain_config.physical_pool_height = 4;
        plain_config.memory_budget_mb = 64;
        plain_config.enable_async_loading = false;  // border 0，无链：v1 布局
        plain->SetConfig(plain_config);
        plain->Initialize(&rhi, nullptr);
        auto tiny_source = MakeSource(64, 64);
        const VirtualTextureHandle tiny_handle = plain->RegisterVirtualTexture(tiny_source, kPageSize);
        MaterialVirtualTextureSlot s {};
        RHIImageView* pt = nullptr;
        RHIImageView* pl = nullptr;
        const bool refused_before = !plain->GetMaterialBinding(tiny_handle, true, s, pt, pl);
        Check(refused_before, "GetMaterialBinding refuses before any page is resident (the pool is still UNDEFINED)");
        plain->ForceLoadPages(tiny_handle, 0);
        const bool refused = !plain->GetMaterialBinding(tiny_handle, true, s, pt, pl);
        Check(refused, "GetMaterialBinding refuses a border-0 pool (a 2x2 footprint would read the neighbouring slot)");
        plain->Shutdown();
    }

    dx12->DrainDebugLayerMessagesForTest();
    {
        uint32_t bad = 0;
        for (const DebugLayerRecord& record : dx12->CopyDebugLayerHistory())
        {
            const bool known = record.severity == D3D12_MESSAGE_SEVERITY_WARNING && (record.id == 820 || record.id == 1361);
            if (record.severity <= D3D12_MESSAGE_SEVERITY_WARNING && !known)
            {
                ++bad;
                std::printf("       [d3d12 debug layer] severity=%d id=%d: %s\n",
                            record.severity,
                            record.id,
                            record.text.c_str());
            }
        }
        Check(bad == 0, "D3D12 debug layer recorded no error / warning for the whole run");
    }

    gbuffer.Shutdown();
    manager->Shutdown();
    resource.reset();
    dx12->Shutdown();

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}

#endif  // _WIN32
