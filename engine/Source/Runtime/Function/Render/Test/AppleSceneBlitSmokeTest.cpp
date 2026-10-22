// =============================================================================
// AppleSceneBlit 冒烟测试。
//
// 为什么需要它：Metal 上"把离屏场景搬到屏幕"这一段里，**MSL 是唯一一个构建永远
// 验证不到的环节**。引擎的 shader 是 GLSL → SPIR-V，构建期由 glslangValidator
// 编译；而 AppleSceneBlit 的 MSL 是写死在 .cpp 里的字符串，要等到运行时
// `newLibraryWithSource:` 才第一次见编译器。拼错一个字符，构建照样全绿，画面上
// 是黑的。
//
// 所以这个测试钉三件事：
//
//   1. **MSL 真的能编译 + 管线真的能建**。Apple 上无头创建 MetalRHI（Initialize
//      只要一个 MTLDevice，不需要窗口），拿一个 32×32 的离屏 pass 当 swapchain
//      的替身，跑一遍 AppleSceneBlit::Initialize。这一句同时证伪：MSL 语法、
//      入口点名字、颜色附件格式匹配、blend 附件数量。
//
//   2. **真的画出像素，而且是全屏**。目标图先清成纯黑（0），离屏"场景"涂一条
//      从 0.25 到 0.75 的竖直渐变 —— 于是"没有任何像素等于清屏值"就等价于
//      "全屏三角形铺满了整个视口"。
//
//   3. **上下没颠倒**。渐变的暗端在图像第 0 行（上），亮端在最后一行（下）。
//      NDC 的 y=+1 在屏幕上边而纹理的 v=0 在图像上边，所以 v 必须翻过来；翻错
//      了第一行的值就会接近亮端，断言立刻红。这条是纯文本检查钉不住的 ——
//      它得真跑一遍才知道。
//
//   4. **tonemap + gamma 真的在，而且曲线跟 RP2 一致**。源是线性 HDR
//      （RP1 的 backup_odd，R16G16B16A16_SFLOAT），目标是 BGRA8Unorm（非 sRGB），
//      而 RP2 那套后处理在 Apple 上被整段排除 ⇒ 这步只能由这次全屏采样代劳。
//      测试里放了一份 Uncharted2 + 1/2.2 的 C++ 镜像，要求 GPU 出来的值等于镜像
//      算出来的值 —— 三份实现（tone_mapping.frag / MSL / 这里）就靠这条对齐。
//
//  非 Apple 平台退化成文本 + 纯数学部分：文本契约（set/binding 与
//  [[texture(n)]] 的手工对齐、入口点必须叫 main、顶点数必须是 3、剔除必须关掉、
//  tonemap 的曝光与 gamma 字面量）和 tonemap 曲线本身的性质。
//
//   cmake -B build -DZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST=ON
//   cmake --build build --target AppleSceneBlitSmokeTest
// =============================================================================

// 这个 TU 在 Apple 上被当作 ObjC++ 编译并跳过了 PCH（见 CMakeLists：它要 include
// MetalRHI.h，而那是 Objective-C 头）。RegisterRuntime.cpp 是同样的处境，所以也
// 同样是**显式**把 pch.h 包含进来 —— 否则只在 pch 里出现过的名字就找不到了。
#if defined(__APPLE__)
    #include "CommonPCH/pch.h"
#endif

#include "Runtime/Function/Render/AppleSceneBlit.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// 只有 Apple 上真的建一个 RHI。这里是唯一一处需要认识具体后端的地方：RHI 基类的
// Initialize / Shutdown 是 protected（给系统注册表用的），而测试要直接驱动它。
#if defined(__APPLE__)
    #include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
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

    std::string ReadFileText(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::string();
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    bool Contains(const std::string& text, const char* token)
    {
        return text.find(token) != std::string::npos;
    }

    // =======================================================================
    // Uncharted2 tonemap + 1/2.2 gamma 的 C++ 镜像。
    //
    // 同一条曲线在这个仓库里有三份实现，互相都看不见：
    //   1. engine/shader/glsl/tone_mapping.frag —— RP2 的 tonemap pass（Vulkan/DX12）
    //   2. AppleSceneBlit.cpp 里的 MSL        —— Apple 上 RP2 被排除时唯一的出路
    //   3. 这里                               —— 测试用的对拍基准
    // 下面"GPU 出来的值 == 这个公式算出来的值"那几条断言就是唯一的对齐手段：
    // 改了 1 或 2 的任何系数（曝光 4.5、白点 11.2、gamma 2.2、A..F），差值都会
    // 远超容差。
    // =======================================================================
    float Uncharted2Tonemap(float x)
    {
        const float A = 0.15f;
        const float B = 0.50f;
        const float C = 0.10f;
        const float D = 0.20f;
        const float E = 0.02f;
        const float F = 0.30f;
        return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
    }

    float TonemapGamma(float linear_value)
    {
        const float exposed = Uncharted2Tonemap(linear_value * 4.5f);
        const float white   = 1.0f / Uncharted2Tonemap(11.2f);
        return std::pow(exposed * white, 1.0f / 2.2f);
    }

    // tonemap 曲线本身的性质。这部分不需要 GPU，所以非 Apple 平台也跑 —— 它钉的
    // 是"为什么要做这一步"：线性值直接写进 UNORM 会怎样，做了之后会怎样。
    void CheckTonemapCurve()
    {
        Check(TonemapGamma(0.0f) < 0.001f, "tonemap maps black to black");

        // 中间调被提亮 —— 这正是原来那个 bug 的反面：线性 0.25 以前被当成显示值
        // 直接写进 swapchain，屏幕上就是 64/255；gamma 之后必须明显高于它。
        Check(TonemapGamma(0.25f) > 0.5f,
              "gamma lifts mid-tones (linear 0.25 used to reach the screen as 0.25)");

        // 单调：亮的输入不能变暗。
        Check(TonemapGamma(0.25f) < TonemapGamma(0.5f) && TonemapGamma(0.5f) < TonemapGamma(1.0f),
              "the tonemap curve is monotonically increasing on [0,1]");

        // 真正的 HDR（>1）必须被压回 [0,1] 而不是硬切成纯白：这是"没有 tonemap"
        // 和"有 tonemap"在画面上唯一的区别。
        Check(TonemapGamma(1.0f) < 1.0f && TonemapGamma(2.0f) < 1.0f,
              "HDR values above 1.0 are rolled off instead of clipped to pure white");
    }

    // =======================================================================
    // 1. 文本契约：MSL 与 C++ 两侧的手工对齐
    // =======================================================================
    void CheckSourceContracts()
    {
        const std::string source = ReadFileText(
            std::string(ZENGINE_ENGINE_ROOT_DIR) +
            "/Source/Runtime/Function/Render/AppleSceneBlit.cpp");
        Check(!source.empty(), "AppleSceneBlit.cpp is readable as text (engine root from CMake)");
        if (source.empty())
        {
            return;
        }

        // **入口绝不能叫 main**：Metal 的编译器会直接拒掉
        //   "vertex function cannot be called 'main'"
        // 而这段 MSL 是字符串，构建期根本不进编译器 ⇒ 这条断言是唯一能拦住它的东西。
        // 名字不是 "main" 也没关系：MetalRHI 在 newFunctionWithName:@"main" 落空时
        // 会退回 library.functionNames.firstObject（SPIRV-Cross 生成的 main0 也是
        // 这么被找到的）。它不看 RHIPipelineShaderStageCreateInfo 的 pName。
        Check(!Contains(source, "vertex BlitVertexOut main(") && !Contains(source, "fragment float4 main("),
              "no MSL entry point is named main (Metal's compiler rejects that outright)");
        Check(Contains(source, "vertex BlitVertexOut BlitVertexMain("), "the MSL vertex entry point is BlitVertexMain");
        Check(Contains(source, "fragment float4 BlitFragmentMain("), "the MSL fragment entry point is BlitFragmentMain");

        // 槽位：MetalRHI::CmdBindDescriptorSetsPFN 里
        //   texture_index = set_index * k_msl_descriptor_set_stride + binding
        // set 0 / binding 0 ⇒ [[texture(0)]] 和 [[sampler(0)]]。RHIDescriptorSetLayout
        // 是不透明的，拿不回来 ⇒ 两边只能手工对齐，这里就是那根钉子。
        Check(Contains(source, "[[texture(0)]]"), "the MSL samples from [[texture(0)]]");
        Check(Contains(source, "[[sampler(0)]]"), "the MSL samples with [[sampler(0)]]");
        Check(Contains(source, "binding.binding         = 0;"), "the C++ side declares descriptor binding 0");
        Check(Contains(source, "write.dstBinding      = 0;"), "the C++ side writes descriptor binding 0");
        Check(Contains(source, "RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER"),
              "the descriptor type is COMBINED_IMAGE_SAMPLER (texture + sampler in one slot)");

        // v 必须翻过来：NDC 的 y=+1 在屏幕上边，纹理的 v=0 在图像上边。
        Check(Contains(source, "0.5 - xy.y * 0.5"), "the MSL flips v (NDC +y is screen-top, v=0 is image-top)");

        // 全屏三角形：三个顶点，没有顶点缓冲。
        Check(Contains(source, "CmdDraw(command_buffer, 3, 1, 0, 0)"), "Record draws exactly 3 vertices (fullscreen triangle)");

        // 全屏三角形的绕序和场景网格相反 ⇒ 剔除必须关，剔错了就是整屏黑。
        Check(Contains(source, "RHI_CULL_MODE_NONE"), "the pipeline culls nothing (the triangle's winding is not the scene's)");

        // 只写 swapchain 那一个颜色附件。
        Check(Contains(source, "blend_state.attachmentCount = 1"), "the blend state covers exactly one colour attachment");

        // ---- tonemap + gamma 的字面量 ----
        // 这段 MSL 不进构建期的任何编译器，所以曝光/白点/gamma 这三个数只能像
        // 这样钉。真正的"曲线对不对"由 CheckTonemapCurve + GPU 对拍负责，这里只
        // 保证它们跟 tone_mapping.frag 用的是同一组常量。
        Check(Contains(source, "Uncharted2Tonemap(color * 4.5)"),
              "the MSL applies the same 4.5 exposure as glsl/tone_mapping.frag");
        Check(Contains(source, "Uncharted2Tonemap(float3(11.2))"),
              "the MSL uses the same 11.2 white point as glsl/tone_mapping.frag");
        Check(Contains(source, "pow(color, float3(1.0 / 2.2))"),
              "the MSL gamma-encodes with 1/2.2 (the swapchain is BGRA8Unorm, not sRGB)");
    }

#if defined(__APPLE__)
    // =======================================================================
    // 2. 离屏 target：32×32 的 R8G8B8A8_UNORM，一个颜色附件。
    //
    //    用 UNORM 不用 SRGB：渐变值要能直接数出来，不能被 gamma 编码过一道。
    // =======================================================================
    constexpr uint32_t kSize   = 32;
    constexpr RHIFormat kFormat = RHI_FORMAT_R8G8B8A8_UNORM;

    struct Offscreen
    {
        RHIImage*        image {nullptr};
        RHIDeviceMemory* memory {nullptr};
        RHIImageView*    view {nullptr};
        RHIRenderPass*   render_pass {nullptr};
        RHIFramebuffer*  framebuffer {nullptr};
    };

    Offscreen MakeOffscreen(RHI* rhi, uint32_t width, uint32_t height, RHIImageUsageFlags extra_usage)
    {
        Offscreen target;
        if (rhi == nullptr)
        {
            return target;
        }

        rhi->CreateImage(width,
                         height,
                         kFormat,
                         RHI_IMAGE_TILING_OPTIMAL,
                         RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT |
                             RHI_IMAGE_USAGE_TRANSFER_SRC_BIT | extra_usage,
                         RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         target.image,
                         target.memory,
                         0,
                         1,
                         1);
        rhi->CreateImageView(target.image,
                             kFormat,
                             RHI_IMAGE_ASPECT_COLOR_BIT,
                             RHI_IMAGE_VIEW_TYPE_2D,
                             1,
                             1,
                             target.view);
        if (target.view == nullptr)
        {
            return target;
        }

        RHIAttachmentDescription attachment {};
        attachment.format        = kFormat;
        attachment.samples       = RHI_SAMPLE_COUNT_1_BIT;
        attachment.loadOp        = RHI_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp       = RHI_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout   = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        RHIAttachmentReference color_reference {};
        color_reference.attachment = 0;
        color_reference.layout     = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        RHISubpassDescription subpass {};
        subpass.pipelineBindPoint    = RHI_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &color_reference;

        RHIRenderPassCreateInfo render_pass_info {};
        render_pass_info.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments    = &attachment;
        render_pass_info.subpassCount    = 1;
        render_pass_info.pSubpasses      = &subpass;

        if (!rhi->CreateRenderPass(&render_pass_info, target.render_pass))
        {
            return target;
        }

        RHIFramebufferCreateInfo framebuffer_info {};
        framebuffer_info.sType           = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass      = target.render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments    = &target.view;
        framebuffer_info.width           = width;
        framebuffer_info.height          = height;
        framebuffer_info.layers          = 1;
        rhi->CreateFramebuffer(&framebuffer_info, target.framebuffer);

        return target;
    }

    void DestroyOffscreen(RHI* rhi, Offscreen& target)
    {
        if (rhi == nullptr)
        {
            return;
        }
        if (target.framebuffer != nullptr)
        {
            rhi->DestroyFramebuffer(target.framebuffer);
        }
        if (target.render_pass != nullptr)
        {
            rhi->DestroyRenderPass(target.render_pass);
        }
        if (target.view != nullptr)
        {
            rhi->DestroyImageView(target.view);
        }
        if (target.image != nullptr)
        {
            rhi->DestroyImage(target.image);
        }
        if (target.memory != nullptr)
        {
            rhi->FreeMemory(target.memory);
        }
        target = Offscreen {};
    }

    // 把一张 2D color 图读回 CPU。读不回来就返回空 —— 调用方把它当"没有像素"。
    std::vector<uint8_t> ReadBackPixels(RHI* rhi, RHIImage* image, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> pixels;
        if (rhi == nullptr || image == nullptr || width == 0 || height == 0)
        {
            return pixels;
        }

        const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(width) * static_cast<RHIDeviceSize>(height) * 4u;

        RHIBuffer*       buffer = nullptr;
        RHIDeviceMemory* memory = nullptr;
        rhi->CreateBuffer(bytes,
                          RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buffer,
                          memory);
        if (buffer == nullptr || memory == nullptr)
        {
            return pixels;
        }

        RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
        if (command_buffer != nullptr)
        {
            RHIBufferImageCopy region {};
            // Metal 后端把 bufferRowLength 当**字节**用。
            region.bufferRowLength   = width * 4u;
            region.bufferImageHeight = height;
            region.imageSubresource.aspectMask     = RHI_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel       = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount     = 1;
            region.imageExtent.width  = width;
            region.imageExtent.height = height;
            region.imageExtent.depth  = 1;

            rhi->CmdCopyImageToBuffer(command_buffer,
                                      image,
                                      RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      buffer,
                                      1,
                                      &region);
            rhi->EndSingleTimeCommands(command_buffer);
        }

        void* mapped = nullptr;
        if (rhi->MapMemory(memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            const auto* source = static_cast<const uint8_t*>(mapped);
            pixels.assign(source, source + static_cast<size_t>(bytes));
            rhi->UnmapMemory(memory);
        }

        rhi->FreeMemory(memory);
        rhi->DestroyBuffer(buffer);
        return pixels;
    }

    // 某一行（按图像行号，0 = 顶行）四个通道的平均亮度。
    float RowValue(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t row)
    {
        if (pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(row + 1) * 4u)
        {
            return -1.0f;
        }
        uint32_t sum = 0;
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t i = (static_cast<size_t>(row) * static_cast<size_t>(width) + x) * 4u;
            sum += static_cast<uint32_t>(pixels[i]) + static_cast<uint32_t>(pixels[i + 1]) +
                   static_cast<uint32_t>(pixels[i + 2]);
        }
        return static_cast<float>(sum) / static_cast<float>(width * 3);
    }

    // -----------------------------------------------------------------------
    // 假"场景"：一条竖直渐变，顶行 0.25（≈64）、底行 0.75（≈191）。
    //
    // 这个渐变是用测试自己的一条全屏管线画出来的 —— RHI 里没有 buffer→image 的
    // 上传路径（连 CmdCopyBufferToImage 都没有），CmdClearAttachmentsPFN 在 Metal
    // 上是空实现，CmdBlitImage 又忽略传入的矩形。渲染进去是唯一可行的写法。
    // -----------------------------------------------------------------------
    const char* kGradientVertMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GradientVertexOut
{
    float4 position [[position]];
};

// 不能叫 main —— Metal 编译器直接拒绝名为 main 的入口函数。
vertex GradientVertexOut GradientVertexMain(uint vertex_id [[vertex_id]])
{
    float2 xy = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);
    GradientVertexOut out;
    out.position = float4(xy, 0.0, 1.0);
    return out;
}
)MSL";

    // [[position]] 在 fragment 里是窗口坐标，原点在视口左上角、y 向下增长 ⇒
    // position.y ≈ 0.5 是第 0 行。所以"顶行暗、底行亮"就是这里的写法。
    const char* kGradientFragMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GradientVertexOut
{
    float4 position [[position]];
};

fragment float4 GradientFragmentMain(GradientVertexOut in [[stage_in]])
{
    float t = 0.25 + 0.5 * (in.position.y / 32.0);
    return float4(t, t, t, 1.0);
}
)MSL";

    RHIPipeline* MakeGradientPipeline(RHI* rhi, RHIRenderPass* render_pass)
    {
        if (rhi == nullptr || render_pass == nullptr)
        {
            return nullptr;
        }

        // 零个 descriptor set：渐变只依赖 [[position]]，什么都不用绑。
        RHIPipelineLayoutCreateInfo layout_info {};
        layout_info.sType          = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 0;
        layout_info.pSetLayouts    = nullptr;
        RHIPipelineLayout* layout = nullptr;
        if (!rhi->CreatePipelineLayout(&layout_info, layout) || layout == nullptr)
        {
            return nullptr;
        }

        RHIShader* vert = rhi->CreateShaderModuleFromSource(kGradientVertMsl, ShaderStage::Vertex, "gradient.vert");
        RHIShader* frag = rhi->CreateShaderModuleFromSource(kGradientFragMsl, ShaderStage::Fragment, "gradient.frag");
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
            return nullptr;
        }

        RHIPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = RHI_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = RHI_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        RHIPipelineVertexInputStateCreateInfo vertex_input {};
        vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
        input_assembly.sType    = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        RHIPipelineViewportStateCreateInfo viewport_state {};
        viewport_state.sType         = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;

        RHIPipelineRasterizationStateCreateInfo rasterization {};
        rasterization.sType       = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization.cullMode    = RHI_CULL_MODE_NONE;
        rasterization.frontFace   = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth   = 1.0f;

        RHIPipelineMultisampleStateCreateInfo multisample {};
        multisample.sType                = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState blend {};
        blend.blendEnable    = RHI_FALSE;
        blend.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                               RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo blend_state {};
        blend_state.sType           = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend_state.attachmentCount = 1;
        blend_state.pAttachments    = &blend;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state {};
        dynamic_state.sType             = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates    = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipeline_info {};
        pipeline_info.sType               = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount          = 2;
        pipeline_info.pStages             = stages;
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState   = &multisample;
        pipeline_info.pColorBlendState    = &blend_state;
        pipeline_info.pDynamicState       = &dynamic_state;
        pipeline_info.layout              = layout;
        pipeline_info.renderPass          = render_pass;
        pipeline_info.subpass             = 0;

        RHIPipeline* pipeline = nullptr;
        rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, pipeline);
        rhi->DestroyShaderModule(vert);
        rhi->DestroyShaderModule(frag);
        return pipeline;
    }

    void SetFullViewport(RHI* rhi, RHICommandBuffer* command_buffer)
    {
        RHIViewport viewport {};
        viewport.width    = static_cast<float>(kSize);
        viewport.height   = static_cast<float>(kSize);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);

        RHIRect2D scissor {};
        scissor.extent.width  = kSize;
        scissor.extent.height = kSize;
        rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);
    }
#endif  // __APPLE__
}  // namespace

int main()
{
    std::printf("=== AppleSceneBlit smoke test ===\n");

    // =======================================================================
    // 1. 文本契约（所有平台都跑：MSL 与 C++ 的手工对齐）
    // =======================================================================
    CheckSourceContracts();

    // tonemap 曲线的性质（纯数学，不需要 GPU ⇒ 所有平台都跑）。
    CheckTonemapCurve();

#if defined(__APPLE__)
    // =======================================================================
    // 2. 空值守卫。没有这些，Resize 之前 / RenderPipeline 还没建好时 Record
    //    会拿到 nullptr 然后解引用。
    // =======================================================================
    {
        AppleSceneBlit blit;
        Check(!blit.IsReady(), "a default-constructed AppleSceneBlit is not ready");
        Check(!blit.Initialize(nullptr, nullptr), "Initialize rejects a null RHI");
        blit.Record(nullptr, nullptr);  // 守卫：不能崩
        Check(true, "Record survives null command buffer and null source");
    }

    // =======================================================================
    // 3. 真的跑一遍（只有 Apple 有 Metal）
    // =======================================================================
    {
        MetalRHI metal_rhi;
        RHI*     rhi = &metal_rhi;

        const bool metal_ready = metal_rhi.Initialize();
        Check(metal_ready, "MetalRHI::Initialize succeeds without a window");
        if (!metal_ready)
        {
            std::printf("[skip] no Metal device on this host -- GPU checks skipped\n");
            std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
            return g_Failures == 0 ? 0 : 1;
        }

        // 源：假"场景"（RP1 的 backup_odd 的替身）。目标：swapchain 的替身。
        Offscreen scene  = MakeOffscreen(rhi, kSize, kSize, 0);
        Offscreen screen = MakeOffscreen(rhi, kSize, kSize, 0);
        Check(scene.framebuffer != nullptr && scene.view != nullptr, "the fake offscreen scene target was built");
        Check(screen.framebuffer != nullptr && screen.view != nullptr, "the fake swapchain target was built");

        RHIPipeline* gradient = MakeGradientPipeline(rhi, scene.render_pass);
        Check(gradient != nullptr, "the test's gradient pipeline builds (MSL-from-source works end to end)");

        // ---- 把渐变画进"场景" ----
        if (gradient != nullptr && scene.framebuffer != nullptr)
        {
            RHIClearValue clear {};
            clear.color.float32[3] = 1.0f;

            RHIRenderPassBeginInfo begin {};
            begin.sType              = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass         = scene.render_pass;
            begin.framebuffer        = scene.framebuffer;
            begin.renderArea.extent.width  = kSize;
            begin.renderArea.extent.height = kSize;
            begin.clearValueCount    = 1;
            begin.pClearValues       = &clear;

            RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
            rhi->CmdBeginRenderPassPFN(command_buffer, &begin, RHI_SUBPASS_CONTENTS_INLINE);
            SetFullViewport(rhi, command_buffer);
            rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, gradient);
            rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
            rhi->CmdEndRenderPassPFN(command_buffer);
            rhi->EndSingleTimeCommands(command_buffer);
        }

        const std::vector<uint8_t> scene_pixels = ReadBackPixels(rhi, scene.image, kSize, kSize);
        const float                scene_top    = RowValue(scene_pixels, kSize, 0);
        const float                scene_bottom = RowValue(scene_pixels, kSize, kSize - 1);
        Check(scene_top > 32.0f && scene_top < 100.0f,
              "the fake scene's top row is the dark end of the gradient");
        Check(scene_bottom > 150.0f && scene_bottom < 224.0f,
              "the fake scene's bottom row is the bright end of the gradient");

        // ---- AppleSceneBlit 本体：Initialize 会把 MSL 交给 newLibraryWithSource ----
        AppleSceneBlit blit;
        // 失败路径必须先试：Initialize 内部一失败就 Shutdown，成功之后再试一次
        // 会把刚建好的管线清空。
        Check(!blit.Initialize(rhi, nullptr), "Initialize rejects a null swapchain render pass");

        const bool blit_ready = blit.Initialize(rhi, screen.render_pass);
        Check(blit_ready, "AppleSceneBlit::Initialize builds the fullscreen pipeline (the MSL compiles)");
        Check(blit.IsReady(), "AppleSceneBlit reports ready after Initialize");

        if (blit_ready && screen.framebuffer != nullptr)
        {
            RHIClearValue clear {};  // 纯黑：任何留着这个值的像素就是"没画到"
            clear.color.float32[3] = 1.0f;

            RHIRenderPassBeginInfo begin {};
            begin.sType              = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass         = screen.render_pass;
            begin.framebuffer        = screen.framebuffer;
            begin.renderArea.extent.width  = kSize;
            begin.renderArea.extent.height = kSize;
            begin.clearValueCount    = 1;
            begin.pClearValues       = &clear;

            RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
            rhi->CmdBeginRenderPassPFN(command_buffer, &begin, RHI_SUBPASS_CONTENTS_INLINE);
            SetFullViewport(rhi, command_buffer);

            // 先来一次 null source：必须被守卫生吞，且不能影响后面那次真正的绘制
            // （descriptor 是每次 Record 重写的，这一次什么都没绑）。
            blit.Record(command_buffer, nullptr);
            blit.Record(command_buffer, scene.view);

            rhi->CmdEndRenderPassPFN(command_buffer);
            rhi->EndSingleTimeCommands(command_buffer);
        }

        const std::vector<uint8_t> screen_pixels = ReadBackPixels(rhi, screen.image, kSize, kSize);

        // 全屏覆盖：清屏值是纯黑（0），而渐变最暗也有 0.25（≈64）⇒ 没有一个像素
        // 还停在 0 上，就说明全屏三角形铺满了整个视口。
        uint32_t untouched = 0;
        for (size_t i = 0; i + 4 <= screen_pixels.size(); i += 4)
        {
            if (screen_pixels[i] <= 8 && screen_pixels[i + 1] <= 8 && screen_pixels[i + 2] <= 8)
            {
                ++untouched;
            }
        }
        Check(screen_pixels.size() == static_cast<size_t>(kSize) * kSize * 4u,
              "the swapchain stand-in read back a full 32x32 RGBA image");
        Check(untouched == 0, "every pixel of the swapchain stand-in was written (the triangle covers the viewport)");

        // 上下没颠倒：顶行必须还是暗端。翻过来这里就会读到亮端。
        //
        // 注意这里**不能**再断言"screen_top ≈ scene_top"了 —— blit 现在是有意
        // 非线性的（tonemap + gamma），出来一定比源亮。
        const float screen_top    = RowValue(screen_pixels, kSize, 0);
        const float screen_middle = RowValue(screen_pixels, kSize, kSize / 2);
        const float screen_bottom = RowValue(screen_pixels, kSize, kSize - 1);
        Check(screen_top < screen_middle && screen_middle < screen_bottom,
              "the gradient still runs top-to-bottom after tonemapping (v really is flipped, not mirrored)");
        Check(screen_bottom <= 255.0f && screen_top >= 0.0f,
              "the tonemapped result stays inside the UNORM range");

        // ---- tonemap + gamma 用的就是 C++ 镜像那条曲线 ----
        //
        // 期望值从**读回来的源值**反推，而不是从 0.25/0.75 这两个字面量反推：
        // 场景替身是 UNORM8（真身 RGBA16Float 在这里没法读回来做逐字节比对），
        // 源值先被量化过一道，GPU 采到的就是量化后的值。
        const float scene_middle     = RowValue(scene_pixels, kSize, kSize / 2);
        const float expected_top     = TonemapGamma(scene_top / 255.0f) * 255.0f;
        const float expected_middle  = TonemapGamma(scene_middle / 255.0f) * 255.0f;
        const float expected_bottom  = TonemapGamma(scene_bottom / 255.0f) * 255.0f;
        std::printf("       scene   top=%6.1f mid=%6.1f bottom=%6.1f (linear/255)\n",
                    scene_top, scene_middle, scene_bottom);
        std::printf("       screen  top=%6.1f mid=%6.1f bottom=%6.1f (actual)\n",
                    screen_top, screen_middle, screen_bottom);
        std::printf("       expect  top=%6.1f mid=%6.1f bottom=%6.1f (C++ mirror)\n",
                    expected_top, expected_middle, expected_bottom);
        Check(std::fabs(screen_top - expected_top) < 4.0f,
              "the blit's top row matches the C++ mirror of glsl/tone_mapping.frag");
        Check(std::fabs(screen_middle - expected_middle) < 4.0f,
              "the blit's middle row matches the C++ mirror of glsl/tone_mapping.frag");
        Check(std::fabs(screen_bottom - expected_bottom) < 4.0f,
              "the blit's bottom row matches the C++ mirror of glsl/tone_mapping.frag");

        // gamma 一定把中间调提亮：这是"线性值被直接写进 UNORM"那个 bug 最直观的
        // 反证 —— 源的暗端 ≈0.25，出来必须明显高于它，否则 tonemap 那段被删了。
        Check(screen_top > scene_top + 40.0f,
              "gamma lifts the dark end (a raw copy would have written linear values straight to the UNORM swapchain)");

        DestroyOffscreen(rhi, scene);
        DestroyOffscreen(rhi, screen);
        metal_rhi.Shutdown();
    }
#endif  // __APPLE__

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
