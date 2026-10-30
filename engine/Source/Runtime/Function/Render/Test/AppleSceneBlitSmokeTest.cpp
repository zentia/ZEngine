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
//   5. **颜色分级 LUT 真的被读了**。RP2 的第二个描述符槽（binding 1）绑的是
//      256x16 的条带 LUT。这个仓库里没有任何既有实现可抄（Vulkan 的
//      glsl/color_grading.frag 把 texture() 那行注释掉了，DX12 的 hlsl 写明
//      "bound but unused"），所以条带的轴向、半 texel 内缩、跨切片混合全是新写的。
//      这里钉三层：条带公式的 C++ 镜像在恒等 LUT 上必须是恒等（轴向搞反 /
//      off-by-one 立刻红）；从 AppleRp2Chain.cpp 抽出**真实** MSL 建管线跑一遍
//      恒等 LUT（证明 [[texture(1)]] 真的绑上了 —— 没绑上 Metal 采到的是 0）；
//      再跑一遍常量 LUT（证明输出确实来自 LUT 而不是绕过它）。
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

    /// 读进来统一成 LF。
    ///
    /// 这台机器上这些源文件是 CRLF（git 在 Windows 上检出的行尾），而下面的断言
    /// 用的是跨行字面量，比如 `body_of(cmd_source, "void Execute() override\n    {\n
    /// // 走 RenderSystem")` —— 带 `\r` 的话**永远匹配不上**，于是 4 条断言在
    /// Windows 全红、在 Linux/macOS（LF 检出）全绿。同一个测试在三个宿主上给出
    /// 不同结论，跟被测代码一点关系都没有：那 4 条其实全是对的。
    ///
    /// 在读取处归一，而不是去改那 4 条断言：断言写 LF 是对的，错的是"源文件
    /// 行尾随平台变"这件事泄漏进了比较。
    std::string ReadFileText(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::string();
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string text = buffer.str();

        std::string normalized;
        normalized.reserve(text.size());
        for (size_t at = 0; at < text.size(); ++at)
        {
            if (text[at] == '\r')
            {
                continue;
            }
            normalized.push_back(text[at]);
        }
        return normalized;
    }

    bool Contains(const std::string& text, const char* token)
    {
        return text.find(token) != std::string::npos;
    }

    int CountOccurrences(const std::string& text, const char* token)
    {
        int   count = 0;
        size_t at   = 0;
        while ((at = text.find(token, at)) != std::string::npos)
        {
            ++count;
            at += std::strlen(token);
        }
        return count;
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

    // =======================================================================
    // 256x16 条带 LUT 采样的 C++ 镜像。
    //
    // MSL 里那份（AppleRp2Chain.cpp 的 SampleColorGradingLut）是逐字照抄这个
    // 公式的，两边靠下面的"恒等 LUT 必须是恒等"和 GPU 对拍对齐。
    //
    // 为什么要专门测：条带上 b 轴是**不连续**的（16 个切片并排），硬件的线性
    // 过滤只会在切片内部插值。所以 r / g 靠采样器插值、b 靠手工 mix 两片 ——
    // 三条轴的映射关系各写各的，搞反一个通道在灰度图上完全看不出来（恒等 LUT
    // 下 (r,g,b) 任你怎么换都还是同一个值），只有拿**非灰**输入对拍才抓得到。
    // =======================================================================
    struct float3
    {
        float x, y, z;
    };

    // 恒等 LUT：格点 (i, j, k) 上存的就是 (i/15, j/15, k/15)。
    float3 IdentityLutTexel(int i, int j)
    {
        const int slice = i / 16;
        const int r_in  = i - slice * 16;
        return {static_cast<float>(r_in) / 15.0f,
                static_cast<float>(std::min(std::max(j, 0), 15)) / 15.0f,
                static_cast<float>(slice) / 15.0f};
    }

    // 忠实模拟 Metal 的双线性过滤（含钳边）。
    float3 BilinearIdentityLut(float u, float v)
    {
        const float x = std::min(std::max(u * 256.0f - 0.5f, 0.0f), 255.0f);
        const float y = std::min(std::max(v * 16.0f - 0.5f, 0.0f), 15.0f);

        const int i0 = static_cast<int>(std::floor(x));
        const int j0 = static_cast<int>(std::floor(y));
        const int i1 = std::min(i0 + 1, 255);
        const int j1 = std::min(j0 + 1, 15);
        const float fx = x - static_cast<float>(i0);
        const float fy = y - static_cast<float>(j0);

        const float3 c00 = IdentityLutTexel(i0, j0);
        const float3 c10 = IdentityLutTexel(i1, j0);
        const float3 c01 = IdentityLutTexel(i0, j1);
        const float3 c11 = IdentityLutTexel(i1, j1);

        float3 out {};
        out.x = (c00.x * (1.0f - fx) + c10.x * fx) * (1.0f - fy) + (c01.x * (1.0f - fx) + c11.x * fx) * fy;
        out.y = (c00.y * (1.0f - fx) + c10.y * fx) * (1.0f - fy) + (c01.y * (1.0f - fx) + c11.y * fx) * fy;
        out.z = (c00.z * (1.0f - fx) + c10.z * fx) * (1.0f - fy) + (c01.z * (1.0f - fx) + c11.z * fx) * fy;
        return out;
    }

    // 条带采样（MSL 那份的镜像）。
    float3 SampleIdentityStripLut(float r, float g, float b)
    {
        const float lut_width   = 256.0f;
        const float lut_height  = 16.0f;
        const float slice_count = lut_height;
        const float slice_width = lut_width / slice_count;

        const float gg      = g * (slice_count - 1.0f);
        const float slice_f = b * (slice_count - 1.0f);
        const float slice0  = std::floor(slice_f);
        const float slice1  = std::min(slice0 + 1.0f, slice_count - 1.0f);
        const float frac    = slice_f - slice0;
        const float rr      = r * (slice_width - 1.0f);

        const float v  = (gg + 0.5f) / lut_height;
        const float u0 = (slice0 * slice_width + rr + 0.5f) / lut_width;
        const float u1 = (slice1 * slice_width + rr + 0.5f) / lut_width;

        const float3 c0 = BilinearIdentityLut(u0, v);
        const float3 c1 = BilinearIdentityLut(u1, v);

        return {c0.x + (c1.x - c0.x) * frac, c0.y + (c1.y - c0.y) * frac, c0.z + (c1.z - c0.z) * frac};
    }

    void CheckStripLutMath()
    {
        // 恒等 LUT 必须是恒等变换 —— 且必须是**逐通道**恒等。用非灰输入
        // (0.2, 0.5, 0.9)：轴向搞反（比如把 b 当成了 u 的切片内坐标）会给出
        // (0.9, 0.5, 0.2) 之类的排列，半 texel 内缩写错（少了 +0.5）会给出
        // 整体缩放（0.2*15/16 = 0.1875）—— 两种偏差都远超 0.01 的容差。
        const float3 mid = SampleIdentityStripLut(0.2f, 0.5f, 0.9f);
        Check(std::fabs(mid.x - 0.2f) < 0.01f, "the strip LUT keeps r on the within-slice axis");
        Check(std::fabs(mid.y - 0.5f) < 0.01f, "the strip LUT keeps g on the v axis");
        Check(std::fabs(mid.z - 0.9f) < 0.01f, "the strip LUT keeps b on the slice axis");

        // 八个角必须落在角上：这是"半 texel 内缩"的反证 —— 没有内缩时
        // (1,1,1) 会溢出到切片外，采样器钳边后给出 (0, 1, 1) 这种值。
        const float3 black = SampleIdentityStripLut(0.0f, 0.0f, 0.0f);
        const float3 white = SampleIdentityStripLut(1.0f, 1.0f, 1.0f);
        Check(std::fabs(black.x) < 0.01f && std::fabs(black.y) < 0.01f && std::fabs(black.z) < 0.01f,
              "the strip LUT maps black to the first texel of the first slice");
        Check(std::fabs(white.x - 1.0f) < 0.01f && std::fabs(white.y - 1.0f) < 0.01f &&
                  std::fabs(white.z - 1.0f) < 0.01f,
              "the strip LUT maps white to the last texel of the last slice (half-texel inset, no bleed)");

        // b 必须连续：只采一片（不做跨切片 mix）的话蓝通道会 quantized 成 16 级，
        // 相邻两个 b 之间会有 1/15 ≈ 0.067 的台阶。
        const float3 b0 = SampleIdentityStripLut(0.5f, 0.5f, 0.30f);
        const float3 b1 = SampleIdentityStripLut(0.5f, 0.5f, 0.32f);
        Check(std::fabs(b1.z - b0.z) < 0.02f,
              "the cross-slice blend keeps b continuous (a single slice would band into 16 steps)");
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

    // =======================================================================
    // 1b. RP2 接线的文本契约（所有平台都跑）
    //
    // 钉的是"源图"和"要不要 tonemap"必须来自**同一个**判断这件事。RP2 在真机上
    // 跑起来了却让 blit 再 tonemap 一次（或反过来），画面只是"有点发灰"，没人会
    // 当 bug 报；而这两处散在三个文件里，编译器一个字都不会说。
    // =======================================================================
    void CheckRp2Contracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string blit_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/AppleSceneBlit.cpp");
        const std::string rp2_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/AppleRp2Chain.cpp");
        const std::string system_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");

        Check(!rp2_source.empty(), "AppleRp2Chain.cpp is readable as text (engine root from CMake)");
        Check(!system_source.empty(), "RenderSystem.cpp is readable as text (engine root from CMake)");

        // ---- blit 必须真的有两条管线 ----
        Check(Contains(blit_source, "BlitFragmentPassthrough"),
              "AppleSceneBlit has a second, passthrough fragment entry point");
        Check(Contains(blit_source, "m_PipelinePassthrough"),
              "the passthrough pipeline is a distinct RHIPipeline (not a branch inside one shader)");
        Check(Contains(blit_source, "apply_tonemap ? m_Pipeline : m_PipelinePassthrough"),
              "Record picks the pipeline from apply_tonemap every frame");
        // 两个 MSL 字符串必须**各自**只有一个入口：MetalRHI 找 "main" 落空时会退回
        // library.functionNames.firstObject，一个 library 里放两个函数必然取错。
        Check(Contains(blit_source, "kAppleSceneBlitFragMsl"), "the tonemap MSL is its own string literal");
        Check(Contains(blit_source, "kAppleSceneBlitFragPassthroughMsl"),
              "the passthrough MSL is a separate string literal (one entry function per library)");
        // ---- RP2 的 MSL ----
        Check(!Contains(rp2_source, "vertex Rp2VertexOut main(") && !Contains(rp2_source, "fragment float4 main("),
              "AppleRp2Chain: no MSL entry point is named main (Metal rejects that outright)");
        Check(Contains(rp2_source, "vertex Rp2VertexOut Rp2VertexMain("), "AppleRp2Chain has a vertex entry point");
        Check(Contains(rp2_source, "fragment float4 Rp2FragmentMain("), "AppleRp2Chain has a fragment entry point");
        // uv 映射必须和 blit 逐字相同，否则 RP2 就不是一次恒等搬运，画面会翻。
        Check(Contains(rp2_source, "0.5 - xy.y * 0.5"),
              "AppleRp2Chain flips v the same way AppleSceneBlit does (otherwise the image flips)");
        // 显示变换的三个常量必须和 blit / tone_mapping.frag 一致，否则"RP2 开关"
        // 会让画面亮度跳变。
        Check(Contains(rp2_source, "Uncharted2Tonemap(color * 4.5)"),
              "AppleRp2Chain uses the same 4.5 exposure as the blit did");
        Check(Contains(rp2_source, "Uncharted2Tonemap(float3(11.2))"),
              "AppleRp2Chain uses the same 11.2 white point as the blit did");
        Check(Contains(rp2_source, "pow(color, float3(1.0 / 2.2))"), "AppleRp2Chain gamma-encodes with 1/2.2");
        // RP2 写的是 RP2 自己的附件（backup_even），不是 swapchain —— Metal 没有
        // 持久 swapchain image，step 3（combine_ui）在 Apple 上根本没法绑目标。
        Check(Contains(rp2_source, "getRp2ColorGradingFramebuffer"),
              "AppleRp2Chain renders into the existing RP2 color-grading framebuffer, not the swapchain");

        // ---- "源图"和"要不要 tonemap"必须同源 ----
        Check(Contains(system_source, "IsSceneColorTonemapped()"),
              "RenderSystem asks the pipeline whether the scene colour is already tonemapped");
        Check(Contains(system_source, "!already_tonemapped"),
              "RenderSystem tonemaps in the blit exactly when RP2 did NOT (never both, never neither)");
    }

    // =======================================================================
    // 1c. LUT（颜色分级）的文本契约（所有平台都跑）
    //
    // 钉的是两件编译器一个字都不会说的事：
    //   * binding 1 在 C++ 侧声明了、也写了，MSL 侧用的是 [[texture(1)]] /
    //     [[sampler(1)]]。这三处是手工对齐的（RHIDescriptorSetLayout 不透明），
    //     对不上不会报错 —— Metal 只会让 [[texture(1)]] 采到空，画面变成没有
    //     分级而已，看着就像"这个 LUT 风格比较淡"。
    //   * 条带公式的半 texel 内缩、跨切片钳位、mip 0、以及"LUT 在 tonemap 之后
    //     gamma 之前"这个顺序。
    // =======================================================================
    void CheckLutContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string rp2_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/AppleRp2Chain.cpp");
        const std::string res_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderResource.cpp");
        Check(!rp2_source.empty(), "AppleRp2Chain.cpp is readable as text (LUT contracts)");
        Check(!res_source.empty(), "RenderResource.cpp is readable as text (LUT contracts)");

        // ---- 槽位 1 ----
        Check(Contains(rp2_source, "[[texture(1)]]"), "the LUT is declared as [[texture(1)]] in the MSL");
        Check(Contains(rp2_source, "[[sampler(1)]]"), "the LUT is sampled with [[sampler(1)]] in the MSL");
        Check(Contains(rp2_source, "bindings[1].binding         = 1;"),
              "the C++ side declares descriptor binding 1 (LUT)");
        Check(Contains(rp2_source, "write.dstBinding      = 1;"), "the C++ side writes descriptor binding 1 (LUT)");
        Check(Contains(rp2_source, "layout_info.bindingCount = 2;"),
              "the RP2 descriptor set layout has exactly two bindings (scene + LUT)");

        // ---- 条带公式 ----
        Check(Contains(rp2_source, "const float slice_width = lut_width / slice_count;"),
              "the slice width is derived from the LUT size (256x16 => 16 slices of 16)");
        Check(Contains(rp2_source, "(slice0 * slice_width + r + 0.5) / lut_width"),
              "the r axis gets a half-texel inset (without it r=1 bleeds into the next slice)");
        Check(Contains(rp2_source, "(g + 0.5) / lut_height"), "the g axis gets a half-texel inset");
        Check(Contains(rp2_source, "min(slice0 + 1.0, slice_count - 1.0)"),
              "the cross-slice blend clamps at the last slice instead of wrapping");
        Check(Contains(rp2_source, "level(0.0)"),
              "the LUT is sampled at mip 0 (a lower mip would smear adjacent slices together)");

        // ---- 采样序：tonemap -> LUT -> gamma ----
        const size_t tonemap_at = rp2_source.find("Uncharted2Tonemap(float3(11.2))");
        const size_t lut_at     = rp2_source.find("SampleColorGradingLut(lut_texture");
        const size_t gamma_at   = rp2_source.find("pow(color, float3(1.0 / 2.2))");
        Check(tonemap_at != std::string::npos && lut_at != std::string::npos && gamma_at != std::string::npos,
              "the RP2 fragment still contains all three stages (tonemap, LUT, gamma)");
        Check(tonemap_at < lut_at && lut_at < gamma_at,
              "the LUT is applied after the tonemap and before the gamma encode (the order is a choice, see the .cpp)");

        // ---- 兜底：LUT 没加载成功时不能是"采到空" ----
        Check(Contains(rp2_source, "EnsureFallbackLutTexture"),
              "AppleRp2Chain can build a fallback LUT when the resource is missing");
        Check(Contains(rp2_source, "const uint8_t neutral_lut[]"),
              "the fallback LUT is a neutral texel (a missing LUT must not turn the screen black)");

        // ---- RenderResource 的守卫 ----
        // 这份（Vulkan/Apple）以前没有 empty 检查也没有失败日志，而 DX12 那份
        // twin 有 ⇒ Apple 上 LUT 没配上时是完全静默的。
        Check(Contains(res_source, "no color grading map configured"),
              "RenderResource warns when no color grading map is configured");
        Check(Contains(res_source, "failed to load color grading LUT"),
              "RenderResource warns when the color grading LUT fails to load (it used to dereference null)");

        // ---- LUT 必须走**未压缩**的源图 ----
        //
        // TextureImporter 会把 `asset/texture/lut/*.png` 一起烤成 BC7 的
        // `_Generated/Textures/.../*.zasset`（真机日志里确实有），而
        // `LoadTexture` 默认优先返回烤好的那份。4x4 块压缩的查表 = 色带 + 错色，
        // 而且不报任何错。这条钉的是"LUT 显式跳过 cooked 产物"。
        Check(Contains(res_source, "UploadColorGradingLut"),
              "the LUT upload is a separate entry point (so it can run without the whole global resource upload)");
        Check(Contains(res_source, "LoadTexture(color_grading_path, false, false)"),
              "the LUT is loaded uncooked -- a BC7/ASTC compressed LUT would band and miscolour");

        // ---- Apple 那条精简版 RenderSystem::Initialize 必须自己上传 LUT ----
        //
        // 它**从不**调用 UploadGlobalRenderResource（IBL 靠 EnsureFallbackIblTextures
        // 兜底），所以 LUT 在 Apple 上从来没上过 GPU —— 真机日志里
        // `AppleRp2Chain ... ready (lut=FALSE)` 就是这么来的。少了这一句，"接了
        // LUT"只是代码接了，画面上永远是那张 1x1 中性兜底。
        const std::string system_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        Check(!system_source.empty(), "RenderSystem.cpp is readable as text (Apple LUT upload)");
        Check(Contains(system_source, "UploadColorGradingLut(m_Rhi"),
              "the Apple RenderSystem::Initialize uploads the LUT itself (it never calls UploadGlobalRenderResource)");
    }

    // =======================================================================
    // IBL 的文本契约。
    //
    // 三件事，每一件都能单独退化成"画面看不出错、日志也不报错"：
    //   1. MetalRHI 上传时用的是**按格式算出来**的每 texel 字节数，不是写死的 4。
    //   2. Apple 那条精简版 Initialize 自己上传 IBL，而且排在
    //      RenderPipeline::Initialize 之前（RP1 的 SetupDescriptorSets 在那里
    //      就把 descriptor 写死了）。
    //   3. SetupDescriptorSets 那条"设备掉了"的日志改成"还没上传"—— 它其实
    //      每帧都在骗人。
    // =======================================================================
    void CheckIblContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string metal_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Interface/Metal/MetalRHI.mm");
        const std::string res_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderResource.cpp");
        const std::string res_header =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderResource.h");
        const std::string system_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        const std::string rp1_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Passes/MainCameraRp1Pass.cpp");
        Check(!metal_source.empty(), "MetalRHI.mm is readable as text (IBL contracts)");
        Check(!res_source.empty(), "RenderResource.cpp is readable as text (IBL contracts)");
        Check(!system_source.empty(), "RenderSystem.cpp is readable as text (IBL contracts)");
        Check(!rp1_source.empty(), "MainCameraRp1Pass.cpp is readable as text (IBL contracts)");

        // ---- 每 texel 字节数（HDR 是 16 字节/texel，硬编码 4 上传出来是垃圾）----
        Check(Contains(metal_source, "uint32_t BytesPerTexel(RHIFormat format)"),
              "MetalRHI has a bytes-per-texel helper (the upload used to hardcode 4 bytes per texel)");
        Check(Contains(metal_source, "case RHI_FORMAT_R32G32B32A32_SFLOAT: return 16u;"),
              "an RGBA32F texel is 16 bytes, not 4 (stbi_loadf HDR data)");
        Check(Contains(metal_source, "case RHI_FORMAT_R8G8B8A8_UNORM:      return 4u;"),
              "an RGBA8 texel is still 4 bytes (the fallback textures)");
        // 两处上传都必须走 helper，而不是各自再写一遍 `* 4u`。
        const size_t global_image_at = metal_source.find("void MetalRHI::CreateGlobalImage");
        const size_t cube_map_at     = metal_source.find("void MetalRHI::CreateCubeMap");
        Check(global_image_at != std::string::npos && cube_map_at != std::string::npos &&
                  global_image_at < cube_map_at,
              "MetalRHI::CreateGlobalImage and CreateCubeMap are both present (in that order)");
        const std::string upload_region = metal_source.substr(global_image_at, cube_map_at - global_image_at);
        Check(!Contains(upload_region, "* 4u"),
              "CreateGlobalImage no longer hardcodes a 4-byte row pitch");
        Check(Contains(upload_region, "BytesPerTexel(texture_image_format)"),
              "CreateGlobalImage derives the row pitch from the format");

        // ---- mip 链（shader 里 lod = roughness * 8，没有 mip 链粗糙反射就是镜面）----
        Check(Contains(metal_source, "generateMipmapsForTexture:"),
              "MetalRHI generates the cubemap mip chain (specular IBL samples lod = roughness * 8)");
        Check(Contains(metal_source, "MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite"),
              "the cube texture is created with ShaderWrite too (generateMipmapsForTexture needs it)");

        // ---- Apple 自己上传 IBL ----
        Check(Contains(res_header, "void UploadIblResources(RHI* rhi, const LevelIBLResourceDesc& ibl_desc);"),
              "RenderResource exposes UploadIblResources (so Apple can upload IBL without the whole global upload)");
        Check(Contains(res_source, "void RenderResource::UploadIblResources"),
              "UploadIblResources is defined");
        Check(Contains(res_source, "UploadIblResources(rhi, level_resource_desc.m_IblResourceDesc)"),
              "the full UploadGlobalRenderResource still uploads IBL through the new entry point");
        Check(Contains(system_source, "render_resource->UploadIblResources(m_Rhi, ibl_desc);"),
              "the Apple RenderSystem::Initialize uploads the IBL itself");

        // ---- 顺序：必须在 RenderPipeline::Initialize 之前 ----
        const size_t ibl_at          = system_source.find("render_resource->UploadIblResources(m_Rhi, ibl_desc);");
        const size_t pipeline_at     = system_source.find("m_RenderPipeline->Initialize(pipeline_init_info);");
        const size_t color_grade_at  = system_source.find("render_resource->UploadColorGradingLut(m_Rhi");
        Check(ibl_at != std::string::npos && pipeline_at != std::string::npos && ibl_at < pipeline_at,
              "the IBL upload happens before RenderPipeline::Initialize (RP1 writes the descriptors there)");
        Check(color_grade_at != std::string::npos && color_grade_at < pipeline_at,
              "the LUT upload also happens before RenderPipeline::Initialize");

        // ---- 那条误导性的日志 ----
        Check(!Contains(rp1_source, "IBL textures unavailable"),
              "MainCameraRp1Pass no longer blames a removed device for 'IBL not uploaded yet'");
        Check(Contains(rp1_source, "no IBL textures bound yet"),
              "MainCameraRp1Pass says the IBL simply has not been uploaded yet");
        // "环境光其实是 4x4 中性灰"这件事以前在日志里完全看不出来。
        Check(Contains(rp1_source, "'real' = the uploaded HDR cubemaps, 'fallback' = the 4x4 neutral stand-in"),
              "MainCameraRp1Pass logs whether it bound the real HDR cubemaps or the neutral fallback");
    }

    // =======================================================================
    // 1d. RenderSwapData::m_LevelResourceDesc 的生产者（所有平台都跑）
    //
    // 钉的是"这个字段很久以前就有人读了，但全库没有一个写它的人"这件事。两个
    // ProcessSwapData() 都读它、IsReadyToSwap() 拿它当门闩 —— 编译期一切正常，
    // 于是"关卡热重载刷新全局资源"在任何平台都是死的，而且死得悄无声息。
    //
    // 契约分三层：生产者存在且唯一、有人触发它（关卡重载）、上传之后真的重绑。
    // =======================================================================
    void CheckLevelResourceProducerContracts()
    {
        const std::string engine_root  = ZENGINE_ENGINE_ROOT_DIR;
        const std::string swap_header  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSwapContext.h");
        const std::string swap_source  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSwapContext.cpp");
        const std::string sys_header   = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.h");
        const std::string sys_source   = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        Check(!swap_header.empty(), "RenderSwapContext.h is readable as text (level resource producer)");
        Check(!swap_source.empty(), "RenderSwapContext.cpp is readable as text (level resource producer)");
        Check(!sys_header.empty(), "RenderSystem.h is readable as text (level resource producer)");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (level resource producer)");

        // ---- 生产者存在 ----
        Check(Contains(swap_header, "void SetLevelResourceDesc(const LevelResourceDesc& desc);"),
              "RenderSwapData has a SetLevelResourceDesc producer (m_LevelResourceDesc had none anywhere)");
        Check(Contains(swap_source, "void RenderSwapData::SetLevelResourceDesc"),
              "SetLevelResourceDesc is defined");
        Check(CountOccurrences(sys_source, "SetLevelResourceDesc(") == 1,
              "RenderSystem writes m_LevelResourceDesc from exactly one place");

        // ---- 去重用的比较 ----
        Check(Contains(swap_header, "inline bool operator==(const LevelResourceDesc& a, const LevelResourceDesc& b)"),
              "LevelResourceDesc is comparable (the producer de-duplicates a still-pending identical request)");

        // ---- 触发点：关卡重载 ----
        Check(Contains(sys_header, "void RequestGlobalResourceUpload();"),
              "RenderSystem exposes RequestGlobalResourceUpload (the only producer of m_LevelResourceDesc)");
        Check(Contains(sys_source, "void RenderSystem::RequestGlobalResourceUpload()"),
              "RequestGlobalResourceUpload is defined");
        Check(Contains(sys_source, "logic_swap_data.SetLevelResourceDesc(desc);"),
              "RequestGlobalResourceUpload writes the LOGIC swap data (the render thread drains it)");
        // Apple 上 ResubmitActiveLevelRenderers 以前是空桩，编辑器重载场景什么都不做。
        Check(!Contains(sys_source, "void RenderSystem::ResubmitActiveLevelRenderers() {}"),
              "RenderSystem::ResubmitActiveLevelRenderers is no longer an empty Apple stub");
        Check(CountOccurrences(sys_source, "void RenderSystem::ResubmitActiveLevelRenderers(") == 1,
              "there is exactly one ResubmitActiveLevelRenderers (platform-neutral, so Apple runs it too)");
        const size_t resubmit_at = sys_source.find("void RenderSystem::ResubmitActiveLevelRenderers(");
        const size_t request_in_resubmit =
            sys_source.find("RequestGlobalResourceUpload();", resubmit_at);
        Check(resubmit_at != std::string::npos && request_in_resubmit != std::string::npos,
              "ResubmitActiveLevelRenderers requests a global resource upload (level reload now refreshes IBL)");

        // ---- 上传之后必须重绑：两个 ProcessSwapData 都要 ----
        Check(Contains(sys_source, "Runtime/Function/Render/Passes/MainCameraRp1Pass.h"),
              "RenderSystem includes MainCameraRp1Pass.h (needed to rebind the mesh-global IBL descriptors)");
        Check(CountOccurrences(sys_source, "RefreshMeshGlobalIblDescriptors()") == 2,
              "both ProcessSwapData overloads rebind the IBL descriptors after the upload (Apple + PC)");
        const size_t apple_upload_at =
            sys_source.find("UploadIblResources(m_Rhi, swap_data.m_LevelResourceDesc->m_IblResourceDesc)");
        const size_t full_upload_at =
            sys_source.find("UploadGlobalRenderResource(m_Rhi, *swap_data.m_LevelResourceDesc)");
        const size_t rebind_1_at = sys_source.find("RefreshMeshGlobalIblDescriptors()");
        const size_t rebind_2_at =
            sys_source.find("RefreshMeshGlobalIblDescriptors()", rebind_1_at + 1);
        Check(apple_upload_at != std::string::npos && rebind_1_at != std::string::npos &&
                  apple_upload_at < rebind_1_at,
              "the Apple upload happens before the first rebind (otherwise the descriptors point at old textures)");
        Check(full_upload_at != std::string::npos && rebind_2_at != std::string::npos &&
                  full_upload_at < rebind_2_at,
              "the full upload happens before the second rebind");

        // ---- Apple 那条走的是"只补 IBL + LUT"，不是整套全局上传 ----
        Check(CountOccurrences(sys_source, "UploadGlobalRenderResource(m_Rhi, *swap_data.m_LevelResourceDesc)") == 1,
              "only the non-Apple ProcessSwapData consumes the swap data through the full "
              "UploadGlobalRenderResource (Apple's trimmed path never does -- it drags in "
              "texture streaming / MegaLights)");
        Check(Contains(sys_source, "swap_data.m_LevelResourceDesc->m_ColorGradingResourceDesc.m_ColorGradingMap"),
              "the Apple ProcessSwapData also refreshes the color grading LUT");

        // ---- 去重的边界：不能拿"上次已上传的那一份"去重，否则永远不重绑 ----
        Check(Contains(sys_source, "identical request still pending"),
              "the de-dup only skips a request that is still pending (a level reload always re-uploads)");

        // ---- swap 门闩：Apple 的 ProcessSwapData 必须把每个门闩字段都清掉 ----
        //
        // IsReadyToSwap() 是拿"render buffer 里还有没有待处理项"当门闩的。Apple 那份
        // ProcessSwapData 以前只处理三件事（全局资源 / GameObject 增删改 / 坐标轴），
        // 粒子、相机、场景视口一概不碰 ⇒ 只要场景里有一个粒子发射器，swap() 就永久
        // 不再被调用，游戏线程之后写的一切（包括上面那个生产者）全都送不到渲染线程。
        const size_t apple_psd_at = sys_source.find("void RenderSystem::ProcessSwapData()");
        const size_t else_at      = sys_source.find("\n#else", apple_psd_at);
        Check(apple_psd_at != std::string::npos && else_at != std::string::npos,
              "the Apple ProcessSwapData is the first of the two overloads (checked up to the #else)");
        const std::string apple_psd = sys_source.substr(apple_psd_at, else_at - apple_psd_at);
        Check(Contains(apple_psd, "ResetPartilceBatchSwapData()"),
              "the Apple ProcessSwapData drains the particle submit request (IsReadyToSwap gate)");
        Check(Contains(apple_psd, "ResetEmitterTickSwapData()"),
              "the Apple ProcessSwapData drains the emitter tick request (IsReadyToSwap gate)");
        Check(Contains(apple_psd, "ResetEmitterTransformSwapData()"),
              "the Apple ProcessSwapData drains the emitter transform request (IsReadyToSwap gate)");
        Check(Contains(apple_psd, "ResetCameraSwapData()"),
              "the Apple ProcessSwapData drains the camera swap data (IsReadyToSwap gate)");
        Check(Contains(apple_psd, "ResetSceneViewportSwapData()"),
              "the Apple ProcessSwapData drains the scene viewport update (IsReadyToSwap gate)");
        Check(Contains(apple_psd, "IsReadyToSwap"),
              "the Apple ProcessSwapData explains WHY those unused fields must still be drained");

        // ---- 门闩清在哪，比"清没清"更重要：必须在早退之前 ----
        //
        // Apple 的 ProcessSwapData 开头有一个
        // `if (!m_RenderScene || !m_RenderResource || m_Rhi == nullptr) return;`。
        // 门闩字段的清理只要写在那个 return 之后，早退被触发一次（关卡切换的瞬间
        // 就够）就永远没人清 ⇒ 整条 swap 通道死锁。以前粒子那三个 Reset 恰好就写在
        // 那里，这次一并上移，并且改成走共享的 ConsumeParticleSwapData。
        // 必须带 `if (`：正文注释里引用了同一句早退，光搜条件会先命中注释。
        const size_t early_return_at = apple_psd.find("if (!m_RenderScene || !m_RenderResource");
        Check(early_return_at != std::string::npos,
              "the Apple ProcessSwapData still has the not-ready early return (the hazard being guarded)");
        Check(Contains(apple_psd, "m_RenderPipeline->ConsumeParticleSwapData(swap_data, m_SwapContext)"),
              "the Apple ProcessSwapData drains particles through the SHARED ConsumeParticleSwapData "
              "(same code path as non-Apple; a future Metal ParticlePass is picked up automatically)");
        const size_t consume_at = apple_psd.find("ConsumeParticleSwapData(swap_data, m_SwapContext)");
        Check(consume_at != std::string::npos && consume_at < early_return_at,
              "the particle drain happens BEFORE the early return (otherwise one early return deadlocks swap)");
        const size_t camera_reset_at = apple_psd.find("ResetCameraSwapData()");
        Check(camera_reset_at != std::string::npos && camera_reset_at < early_return_at,
              "the camera drain happens BEFORE the early return (IsReadyToSwap gate)");
        const size_t viewport_reset_at = apple_psd.find("ResetSceneViewportSwapData()");
        Check(viewport_reset_at != std::string::npos && viewport_reset_at < early_return_at,
              "the scene viewport drain happens BEFORE the early return (IsReadyToSwap gate)");

        // ---- 关卡卸载的"另一半"：清理 ----
        //
        // 关卡重载是两步：先 ClearForLevelReloading（丢弃旧数据），再
        // ResubmitActiveLevelRenderers（重建）。阶段 49 只补了后一半，前一半在
        // Apple 上还是空桩 ⇒ 实体残留、instance id 分配器只涨不回、交换数据里
        // 留着已销毁对象的 desc。这类"功能打通之后历史空桩变成洞"是最难发现的
        // 一种：它当初是对的，是别的改动把它变成错的。
        Check(!Contains(sys_source, "void RenderSystem::ClearForLevelReloading() {}"),
              "RenderSystem::ClearForLevelReloading is no longer an empty Apple stub");
        Check(CountOccurrences(sys_source, "void RenderSystem::ClearForLevelReloading(") == 1,
              "there is exactly one ClearForLevelReloading (platform-neutral, so Apple runs it too)");
        const size_t clear_at = sys_source.find("void RenderSystem::ClearForLevelReloading(");
        Check(clear_at != std::string::npos, "ClearForLevelReloading is defined");
        // 只切到函数体的收尾大括号，别写死长度 —— 加注释就会把窗口撑爆。
        const size_t clear_end = sys_source.find("\n}", clear_at);
        const std::string clear_body =
            (clear_end == std::string::npos) ? sys_source.substr(clear_at, 900)
                                             : sys_source.substr(clear_at, clear_end - clear_at);
        Check(Contains(clear_body, "m_RenderScene->ClearForLevelReloading()"),
              "it clears the RENDER SCENE (entities / mesh id map / the instance id allocator)");
        Check(Contains(clear_body, "m_RenderScene != nullptr"),
              "it guards the render scene pointer (Apple can reach here before the scene exists)");
        Check(Contains(clear_body, "ResetGameObjectResourceSwapData()") &&
                  Contains(clear_body, "ResetGameObjectToDelete()") &&
                  Contains(clear_body, "ResetLogicGameObjectSwapData()"),
              "it also drops the still-queued GameObjects from BOTH swap buffers "
              "(those descs reference objects the caller is about to unload)");
    }

    // =======================================================================
    // 视口/裁剪矩形链路的契约。
    //
    // MainCameraRp1Pass::SetViewportScissor（5 处）等平台无关代码拿视口用
    // TryGetRenderSceneViewport()（场景）或直接 `m_Rhi->GetViewport(type)`，
    // 拿**裁剪矩形**一律退化到 `GetSwapchainInfo().scissor[type]`。
    //
    // Apple 上有两个洞：
    //   1. TryGetRenderSceneViewport 是 `return false;` 的桩 —— 所有消费者走退化；
    //   2. MetalRHI::UpdateViewport 只写 m_Viewports、不动 m_Scissors —— scissor
    //      只有 SyncLayerMetrics（resize）会重算，永远是整窗。DX12 那边
    //      UpdateViewport 是同时写 m_Viewports[id] 和 m_Scissors[id] 的。
    //      真机探针证实：修复前 game_sc=(0,0) 2560x1440（编辑器场景面板挪动后
    //      裁剪矩形不跟），修复后 game_sc=(389,104) 1524x915 跟着面板走。
    // =======================================================================
    void CheckSceneViewportContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string sys_source  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        const std::string metal_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Interface/Metal/MetalRHI.mm");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (scene viewport)");
        Check(!metal_source.empty(), "MetalRHI.mm is readable as text (scene viewport)");

        // ---- 桩必须已经不存在 ----
        Check(!Contains(sys_source, "bool RenderSystem::TryGetRenderSceneViewport(RHIViewport&, RHIRect2D&) const"),
              "the Apple TryGetRenderSceneViewport stub (unnamed params + return false) is gone");
        const size_t apple_tgsv_at = sys_source.find("bool RenderSystem::TryGetRenderSceneViewport(");
        const size_t else_at      = sys_source.find("\n#else", apple_tgsv_at);
        Check(apple_tgsv_at != std::string::npos && else_at != std::string::npos,
              "there is an Apple-side TryGetRenderSceneViewport before the #else");
        const std::string apple_tgsv = sys_source.substr(apple_tgsv_at, else_at - apple_tgsv_at);
        Check(!Contains(apple_tgsv, "return false;\n}"),
              "the Apple TryGetRenderSceneViewport no longer unconditionally returns false");
        Check(Contains(apple_tgsv, "m_Rhi->GetViewport(ViewportType::scene)"),
              "the Apple TryGetRenderSceneViewport reads the scene viewport straight from the RHI");
        Check(Contains(apple_tgsv, "GetSwapchainInfo().scissor[static_cast<uint32_t>(ViewportType::scene)]"),
              "the Apple TryGetRenderSceneViewport also returns the scene SCISSOR (not just the viewport)");
        Check(Contains(apple_tgsv, "没有渲染线程") || Contains(apple_tgsv, "没有**渲染线程"),
              "the Apple TryGetRenderSceneViewport explains WHY it skips the swap round-trip");

        // ---- 两个定义：Apple 一份、非 Apple 一份 ----
        Check(CountOccurrences(sys_source, "bool RenderSystem::TryGetRenderSceneViewport(") == 2,
              "RenderSystem::TryGetRenderSceneViewport has exactly two definitions (Apple + non-Apple)");

        // ---- Metal 的 UpdateViewport 必须同时写 scissor（对齐 DX12）----
        const size_t metal_uv_at = metal_source.find("void MetalRHI::UpdateViewport(");
        Check(metal_uv_at != std::string::npos, "MetalRHI::UpdateViewport exists");
        const size_t metal_uv_end = metal_source.find("\n}", metal_uv_at);
        const std::string metal_uv =
            metal_source.substr(metal_uv_at,
                                metal_uv_end == std::string::npos ? 900 : (metal_uv_end - metal_uv_at));
        Check(Contains(metal_uv, "m_Viewports[index] = viewport;"),
              "MetalRHI::UpdateViewport still writes the viewport");
        Check(Contains(metal_uv, "m_Scissors[index]"),
              "MetalRHI::UpdateViewport also writes the SCISSOR (DX12RHI::UpdateViewport does; "
              "without it GetSwapchainInfo().scissor[scene] stays at the full-window value)");
        const size_t scissor_at = metal_uv.find("m_Scissors[index]");
        Check(scissor_at != std::string::npos && scissor_at > metal_uv.find("m_Viewports[index]"),
              "the scissor write sits after the viewport write inside the same bounds check");

        // ---- Apple 的 UpdateViewport 要把面板矩形真的送进 RHI，并且可观测 ----
        const size_t apple_uv_at = sys_source.find("void RenderSystem::UpdateViewport(");
        Check(apple_uv_at != std::string::npos, "RenderSystem::UpdateViewport exists (Apple overload)");
        const std::string apple_uv = sys_source.substr(apple_uv_at, 1600);
        Check(Contains(apple_uv, "m_Rhi->UpdateViewport(viewport_id, viewport);"),
              "the Apple UpdateViewport forwards the panel rect to the RHI");
        Check(Contains(apple_uv, "AppleViewport["),
              "the Apple UpdateViewport logs viewport rects when they change (otherwise the link is silent; "
              "both game and scene are logged because the main camera uses game on Apple)");
    }

    // =======================================================================
    // Apple 版 Initialize 的相机来源契约。
    //
    // 以前两台相机是硬编码的 `LookAt((0,-10,5), ZERO, UNIT_Z)` + `SetAspect(16/9)`，
    // 完全不看 GlobalRenderingRes::m_CameraConfig。那份精简版 Initialize 当初只为
    // 了把 UI 画出来，硬编码没有代价；场景管线打通之后就成了洞：
    //   * pose 对不上（真机探针：配置 (-5,0,3)，硬编码 (0,-10,5)）；
    //   * **aspect 对不上**：配置 1280x768 = 1.667，硬编码 16:9 = 1.778，而编辑器
    //     game 视口面板就是 1524x915 = 1.666 ⇒ 画面被横向拉伸且**不报任何错**
    //     （和 0x0 视口同一类）。
    //
    // 顺带：探针还否掉了一个假设 —— 这个工程的关卡里根本没有主相机组件
    // （`GetMainCameraComponent()` 返回 nullptr，`main_cam_valid=0`），所以
    // `SyncGameCameraFromMainCamera` 的空桩今天在两个平台上都是空转，不是洞。
    // =======================================================================
    void CheckCameraConfigContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string sys_source  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (camera config)");

        // Apple 版 Initialize：从第一个 `bool RenderSystem::Initialize()` 到下一个 `#else`。
        const size_t init_at = sys_source.find("bool RenderSystem::Initialize()");
        Check(init_at != std::string::npos, "RenderSystem::Initialize exists");
        const size_t init_else = sys_source.find("\n#else", init_at);
        Check(init_else != std::string::npos, "the Apple Initialize is followed by an #else (non-Apple twin)");
        const std::string apple_init = sys_source.substr(init_at, init_else - init_at);
        Check(Contains(apple_init, "m_Cameras.clear();"), "the Apple Initialize builds the camera list");

        // ---- 顺序契约：全局资源必须在建相机**之前**读 ----
        const size_t load_at    = apple_init.find("loadAsset<GlobalRenderingRes>(");
        const size_t push_at    = apple_init.find("m_Cameras.push_back");
        const size_t camcfg_at  = apple_init.find("m_CameraConfig");
        Check(load_at != std::string::npos, "the Apple Initialize loads GlobalRenderingRes");
        Check(push_at != std::string::npos, "the Apple Initialize pushes cameras");
        Check(load_at < push_at,
              "GlobalRenderingRes is loaded BEFORE the cameras are built (otherwise there is nothing to read)");
        Check(camcfg_at != std::string::npos && camcfg_at < push_at,
              "the cameras are built from m_CameraConfig (before the first push_back)");

        // ---- 三件配置都真的用上 ----
        Check(Contains(apple_init, "m_CameraConfig.m_Pose"),
              "the camera pose comes from m_CameraConfig (not a hard-coded LookAt)");
        Check(Contains(apple_init, "m_CameraConfig.m_ZNear") && Contains(apple_init, "m_CameraConfig.m_ZFar"),
              "znear / zfar come from m_CameraConfig");
        Check(Contains(apple_init, "m_CameraConfig.m_Aspect"),
              "the aspect comes from m_CameraConfig (a wrong aspect silently stretches the frame)");

        // ---- 硬编码只剩兜底，不能是唯一路径 ----
        Check(Contains(apple_init, "has_camera_config"),
              "there is an explicit has/not-have branch, so the hard-coded pose is only a fallback");
        Check(Contains(apple_init, "Vector3(0.0f, -10.0f, 5.0f)"),
              "the hard-coded pose survives as the fallback (config missing must still produce a camera)");
        Check(Contains(apple_init, "16.0f / 9.0f"),
              "the hard-coded 16:9 survives as the fallback aspect");

        // ---- 只 load 一次：同一份指针给相机和 IBL/LUT 用 ----
        Check(CountOccurrences(apple_init, "loadAsset<GlobalRenderingRes>(") == 1,
              "GlobalRenderingRes is loaded exactly once in the Apple Initialize "
              "(the camera block and the IBL/LUT block share one pointer)");

        // ---- 可观测：到底用了配置还是兜底，必须能看出来 ----
        Check(Contains(apple_init, "cameras from"),
              "the Apple Initialize logs whether the cameras came from config or from the fallback "
              "(a silently wrong camera looks exactly like a correct one)");
        Check(Contains(sys_source, "game_cam="),
              "the Apple heartbeat carries the game camera position (the only way to falsify this on device)");
        Check(Contains(sys_source, "cam_sync="),
              "the Apple heartbeat also carries cam_sync (game camera vs. the level's main camera component)");
    }

    // =======================================================================
    // Apple 版 Initialize 的**光照**契约。
    //
    // `m_AmbientLight` / `m_DirectionalLight` 以前**只有非 Apple 那条 Initialize 设过**，
    // Apple 这一条从来没设 ⇒ RenderScene 里一直是默认构造的 (0,0,0)。而
    // `RenderResourcePerFrame` 正是拿它们填每帧 UBO 的（scene_directional_light.color /
    // ambient_light）⇒ Apple 上主光完全不参与、环境光为 0，画面只剩 IBL 在照，
    // 而且**不报任何错**。
    //
    // 真机探针：修复前 dir_light=(0,0,0) dir_color=(0,0,0) ambient=(0,0,0)；修复后
    // dir_light=(0.577,0.577,0.577) dir_color=(1,1,1) ambient=(0.03,0.03,0.03)。
    // （观测到的方向与 rendering.global.json 里的 y:-0.5 符号相反 —— 那是 JSON
    // 反序列化里的坐标转换，两个平台走的是同一份代码，与本次改动无关。）
    // =======================================================================
    void CheckGlobalLightingContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string sys_source  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (global lighting)");

        const size_t init_at = sys_source.find("bool RenderSystem::Initialize()");
        Check(init_at != std::string::npos, "RenderSystem::Initialize exists (global lighting)");
        const size_t init_else = sys_source.find("\n#else", init_at);
        Check(init_else != std::string::npos, "the Apple Initialize is followed by an #else (global lighting)");
        const std::string apple_init = sys_source.substr(init_at, init_else - init_at);

        // ---- 三样必须都在 Apple 版 Initialize 里 ----
        // 注意必须搜**赋值**（带 ` =`）：心跳探针里也有 `m_RenderScene->m_AmbientLight`，
        // 只搜名字会被探针自己蒙过去。
        Check(Contains(apple_init, "m_RenderScene->m_AmbientLight ="),
              "the Apple Initialize sets the AMBIENT light (it used to stay at the default (0,0,0))");
        Check(Contains(apple_init, "m_DirectionalLight.m_Direction ="),
              "the Apple Initialize sets the directional light DIRECTION");
        Check(Contains(apple_init, "m_DirectionalLight.m_Color ="),
              "the Apple Initialize sets the directional light COLOR");
        Check(Contains(apple_init, "normalisedCopy()"),
              "the direction is normalised on the way in (the shader expects a unit vector)");

        // ---- 必须来自全局资源，而不是另一个硬编码 ----
        Check(Contains(apple_init, "global_rendering_res->m_AmbientLight"),
              "the ambient light comes from GlobalRenderingRes");
        Check(Contains(apple_init, "global_rendering_res->m_DirectionalLight"),
              "the directional light comes from GlobalRenderingRes");

        // ---- 顺序：必须在建 RenderScene 之后、建 RenderPipeline 之前 ----
        const size_t scene_at   = apple_init.find("m_RenderScene = std::make_shared<RenderScene>()");
        const size_t ambient_at = apple_init.find("m_RenderScene->m_AmbientLight");
        Check(scene_at != std::string::npos && ambient_at != std::string::npos && scene_at < ambient_at,
              "the lighting is applied AFTER the RenderScene exists (otherwise it writes into nothing)");

        // ---- 可观测：这东西错了不报错，只能靠心跳看出来 ----
        Check(Contains(sys_source, "dir_light="),
              "the Apple heartbeat carries the directional light direction");
        Check(Contains(sys_source, "dir_color="),
              "the Apple heartbeat carries the directional light color");
        Check(Contains(sys_source, "ambient="),
              "the Apple heartbeat carries the ambient light (a zero light looks like a correct dim scene)");
    }

    // -------------------------------------------------------------------------
    // 阶段 55：点光源同步。
    //
    // 非 Apple 有两条路会调 `SyncPointLightsFromLevel`（TickRenderThread 和
    // RenderUpdateSceneCmd::Execute），Apple 的精简 Tick 从来没调过 ⇒
    // `m_PointLightList` 恒为空：场景里摆了 LightComponent 也一个都不亮，而且
    // **不报错**（画面只是"少了几处亮"，没人会去查）。真机探针是
    // `plights=0 gos=34 lcmp=2` —— 有灯、没同步。
    //
    // 这里钉两件事：调用真的在 Apple Tick 里、而且排在 UpdatePerFrameBuffer
    // 之前（那一步才把点光源写进每帧 UBO，排后面就晚一帧且首帧为 0）。
    // 另外把探针关卡本身也钉住，防止它退化成"没有灯的普通关卡"——那样这个
    // 洞又会变回"今天无法证伪"。
    // -------------------------------------------------------------------------
    void CheckPointLightSyncContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string sys_source  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (point light sync)");

        // Apple 版 Tick 的切片：从函数签名到下一个顶层函数定义为止。
        // 只匹配到 `(float` 为止：形参有没有名字（Apple 那条以前是
        // `Tick(float)`，阶段 58 为了拿 delta_time 改成 `Tick(float delta_time)`）
        // 不影响这里的语义，钉死右括号会让一次无关的改名把整段契约打成红。
        const size_t tick_at  = sys_source.find("void RenderSystem::Tick(float");
        const size_t tick_end = sys_source.find("\nvoid RenderSystem::", tick_at + 1);
        Check(tick_at != std::string::npos, "RenderSystem::Tick exists (point light sync)");
        Check(tick_end != std::string::npos, "RenderSystem::Tick is followed by another definition");
        const std::string apple_tick = sys_source.substr(tick_at, tick_end - tick_at);

        // 必须搜**调用**（带实参），不能只搜名字：上面那段中文注释里就写着
        // `SyncPointLightsFromLevel`，只搜名字会被注释自己蒙过去。
        const size_t sync_at = apple_tick.find("m_RenderScene->SyncPointLightsFromLevel(active_level)");
        Check(sync_at != std::string::npos,
              "the Apple Tick syncs point lights from the level (m_PointLightList used to stay empty forever)");

        // 活跃关卡必须来自 WorldManager —— 写死 nullptr 的话函数会直接 return，
        // 列表清空成 0，症状跟没调一模一样。
        Check(Contains(apple_tick, "world_manager->getCurrentActiveLevel()"),
              "the point light sync reads the ACTIVE level from WorldManager");

        // 顺序：必须排在 UpdatePerFrameBuffer 之前。
        // 同样要搜**调用**而不是名字：同步点上面那段中文注释为了说明"为什么排在
        // 前面"也写了 UpdatePerFrameBuffer，只搜名字会被注释自己抢先匹配到。
        const size_t ubo_at = apple_tick.find("UpdatePerFrameBuffer(m_RenderScene, camera)");
        Check(ubo_at != std::string::npos && sync_at < ubo_at,
              "the point lights are synced BEFORE UpdatePerFrameBuffer (that is what writes them into the UBO)");

        // 可观测：plights 单独看无法区分"没灯"和"有灯没同步"，所以必须连
        // 关卡里的 LightComponent 数量一起打。
        Check(Contains(sys_source, "plights="), "the Apple heartbeat carries the synced point light count");
        Check(Contains(sys_source, "lcmp="),
              "the Apple heartbeat carries the LightComponent count (plights=0 alone cannot tell an empty "
              "level from a missing sync)");
        Check(Contains(sys_source, "gos="), "the Apple heartbeat carries the level's GameObject count");

        // 探针关卡本身：两个 enabled 的 LightComponent，flux 不同以便区分。
        const std::string probe_source = ReadFileText(engine_root + "/asset/level/light_probe.scene");
        Check(!probe_source.empty(), "asset/level/light_probe.scene exists (the level that falsified this hole)");
        size_t light_objects = 0;
        for (size_t at = probe_source.find("type: LightComponent"); at != std::string::npos;
             at        = probe_source.find("type: LightComponent", at + 1))
        {
            ++light_objects;
        }
        Check(light_objects == 2, "light_probe.scene carries exactly 2 LightComponents");
        Check(Contains(probe_source, "ProbeLight_A") && Contains(probe_source, "ProbeLight_B"),
              "light_probe.scene names both probe lights (so a heartbeat count can be attributed)");
        // 序列化键名是 flux / radius，不是 m_Flux / m_Radius（ZHeaderTool 剥 `m_`）。
        // 写错键名不会报错，只会静默落回默认值 —— 那样探针就证伪不了任何东西。
        Check(Contains(probe_source, "flux:") && !Contains(probe_source, "m_Flux"),
              "light_probe.scene uses the real serialised key 'flux' (not m_Flux, which silently falls back "
              "to the default)");
    }

    // -------------------------------------------------------------------------
    // 阶段 60：编辑器 gizmo（axis）。
    //
    // "历史空桩在功能打通之后变成洞"的第七例，这次是**两半一起缺**：Apple 上
    // `RenderSystem::CreateAxis` 是空体（不上传 mesh），而 RP1 里**根本没有**
    // axis 的绘制路径（DX12 走 DX12MainCameraPass::DrawAxis、Vulkan 走
    // MainCameraPass::DrawAxis，Metal 两头都不沾）；就连"每帧把 show_axis /
    // selected_axis 传进 pass"这一步也没有。只补上传会得到一堆没人画的 mesh，
    // 只补绘制会因为没有资源而每帧提前返回 —— 两半必须一起有。
    //
    // 顺带的地基修复：Metal 的 CmdBindDescriptorSets 曾把 dynamic offset 直接
    // (void) 掉 ⇒ 所有 *_DYNAMIC 绑定读的都是第一帧的 per-frame 数据。
    // -------------------------------------------------------------------------
    void CheckAxisContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string rp1_header =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Passes/MainCameraRp1Pass.h");
        const std::string rp1_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Passes/MainCameraRp1Pass.cpp");
        const std::string module_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Pipeline/DesktopRenderPipelineModule.cpp");
        const std::string metal_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Interface/Metal/MetalRHI.mm");
        const std::string sys_source  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        Check(!rp1_header.empty(), "MainCameraRp1Pass.h is readable as text (axis)");
        Check(!rp1_source.empty(), "MainCameraRp1Pass.cpp is readable as text (axis)");
        Check(!module_source.empty(), "DesktopRenderPipelineModule.cpp is readable as text (axis)");
        Check(!metal_source.empty(), "MetalRHI.mm is readable as text (axis dynamic offset)");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (axis upload)");

        auto count_of = [](const std::string& haystack, const std::string& needle) {
            size_t count = 0;
            size_t at    = 0;
            while ((at = haystack.find(needle, at)) != std::string::npos)
            {
                ++count;
                at += needle.size();
            }
            return count;
        };
        auto body_of = [](const std::string& source, const std::string& signature) {
            const size_t at = source.find(signature);
            if (at == std::string::npos)
            {
                return std::string {};
            }
            const size_t end = source.find("\nvoid ", at + 1);
            return source.substr(at, end == std::string::npos ? source.size() - at : end - at);
        };

        // ---- 上传那一半（RenderSystem 平台无关尾巴）----
        // CreateAxis 的空桩形态是 `void RenderSystem::CreateAxis(std::array<RenderEntity, 3>,
        // std::array<RenderMeshData, 3>) {}` —— 现在必须是真实现，而且成功也要有日志
        // （"根本没被调"和"调了但全失败"以前在日志里长得一模一样）。
        const std::string create_axis = body_of(sys_source, "void RenderSystem::CreateAxis(");
        Check(!create_axis.empty() && create_axis.size() > 200,
              "RenderSystem::CreateAxis is a real implementation, not an empty stub");
        Check(Contains(create_axis, "UploadGameObjectRenderResource"),
              "CreateAxis uploads the axis meshes to the GPU");
        Check(Contains(create_axis, "axis meshes uploaded"),
              "CreateAxis logs success too -- silence used to make 'never called' and "
              "'called but failed' indistinguishable");
        const std::string set_visible_axis = body_of(sys_source, "void RenderSystem::SetVisibleAxis(");
        Check(Contains(set_visible_axis, "m_VisibleAxisUpdatePending = true"),
              "SetVisibleAxis writes the swap data (the Apple stub only flipped the pipeline flag, so "
              "m_RenderAxis never received a value)");

        // ---- 绘制那一半（RP1）----
        Check(Contains(rp1_header, "_render_pipeline_type_axis") && Contains(rp1_header, "_axis,"),
              "MainCameraRp1Pass has the axis layout / pipeline slots");
        Check(Contains(rp1_header, "void DrawAxis(ViewportType"),
              "MainCameraRp1Pass declares the axis draw entry");

        const std::string setup_axis_pipeline = body_of(rp1_source, "void MainCameraRp1Pass::SetupAxisPipeline()");
        Check(!setup_axis_pipeline.empty() && Contains(setup_axis_pipeline, "CreateShaderModule(AXIS_VERT)") &&
                  Contains(setup_axis_pipeline, "CreateShaderModule(AXIS_FRAG)"),
              "SetupAxisPipeline builds the axis pipeline from the generated SPIR-V (Metal cross-compiles it to MSL)");
        Check(Contains(setup_axis_pipeline, "-- editor gizmo stays hidden"),
              "axis pipeline failures degrade with a log instead of throwing -- a missing gizmo must "
              "not take the whole scene down");
        Check(Contains(setup_axis_pipeline, "getForwardLightingRenderPass()"),
              "the axis pipeline targets the forward-lighting render pass (the last pass RP1 opens)");

        const std::string setup_axis_ds = body_of(rp1_source, "void MainCameraRp1Pass::SetupAxisDescriptorSet()");
        Check(Contains(setup_axis_ds, "m_AxisInefficientStorageBuffer"),
              "SetupAxisDescriptorSet binds the axis storage buffer that RenderResource already creates "
              "(the guard is 'Z_HAS_VULKAN || __APPLE__', so it exists on Metal)");
        Check(Contains(setup_axis_ds, "RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC"),
              "the axis per-frame binding is dynamic, matching the other backends");

        const std::string draw_axis = body_of(rp1_source, "void MainCameraRp1Pass::DrawAxis(ViewportType");
        Check(Contains(draw_axis, "if (!m_IsShowAxis)"),
              "DrawAxis is gated on the show flag");
        Check(Contains(draw_axis, "p_axis_node == nullptr"),
              "DrawAxis distinguishes 'nothing selected' (no axis node) from other skips");
        Check(Contains(draw_axis, "TryGetRenderSceneViewport"),
              "DrawAxis draws into the editor scene viewport (and can say so when it is 0x0)");
        Check(Contains(draw_axis, "m_RenderAxis.has_value()"),
              "DrawAxis requires an axis entity submitted through SetVisibleAxis");
        Check(Contains(draw_axis, "getMeshDrawData(m_VisiableNodes.p_axis_node->ref_mesh)"),
              "DrawAxis draws the uploaded axis mesh (search the call form -- comments mentioning "
              "the name would fool a name-only check)");
        Check(Contains(draw_axis, "CmdDrawIndexedPFN"),
              "DrawAxis actually issues the indexed draw");

        // axis 挂在 forward pass 里（pass 开着的时候才允许录命令）。
        const std::string forward_pass = body_of(rp1_source, "void MainCameraRp1Pass::DrawForwardLightingPass(");
        Check(Contains(forward_pass, "DrawAxis(viewport_type)"),
              "the forward-lighting pass draws the axis (recording outside a render pass is illegal)");

        // 状态必须每帧从 pipeline 传进 pass —— Apple 的 draw list lambda 以前不传，
        // m_IsShowAxis 恒为 false，就算有绘制路径也什么都不画。
        const std::string apple_draw_list = body_of(module_source, "void DesktopRenderPipelineModule::BuildDrawLists(");
        Check(Contains(apple_draw_list, "rp1->m_IsShowAxis   = m_Pipeline.m_IsShowAxis;") &&
                  Contains(apple_draw_list, "rp1->m_SelectedAxis = m_Pipeline.m_SelectedAxis;"),
              "the Apple draw list forwards show_axis / selected_axis into RP1 every frame "
              "(the DX12 branch always did; the Apple branch never did)");

        // ---- 地基：Metal 的 dynamic offset ----
        // 以前这里是 (void)pDynamicOffsets —— 所有 *_DYNAMIC 绑定读的是第一帧的
        // per-frame 数据。契约不许那个 (void) 回来。
        const std::string bind_desc_sets =
            body_of(metal_source, "void MetalRHI::CmdBindDescriptorSetsPFN(");
        Check(!bind_desc_sets.empty() && !Contains(bind_desc_sets, "(void)pDynamicOffsets"),
              "MetalRHI::CmdBindDescriptorSets no longer discards the dynamic offsets");
        Check(Contains(bind_desc_sets, "RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC"),
              "MetalRHI recognises dynamic buffer bindings (static offsets alone meant "
              "every per-frame read was stuck on frame 0)");
        Check(Contains(bind_desc_sets, "dynamic_cursor"),
              "MetalRHI numbers the dynamic bindings in Vulkan order (set order, then binding index)");
        // 上面三条只看"有没有这套机制"，删掉真正生效的那一行它们照样绿 ⇒ 必须再盯
        // **把偏移加进 bind_offset** 这一步（变异测出来过）。
        Check(Contains(bind_desc_sets, "bind_offset += pDynamicOffsets[dynamic_cursor]"),
              "MetalRHI actually adds the dynamic offset to the bound buffer offset -- knowing which "
              "binding is dynamic is worthless if the offset never reaches setVertexBuffer");
    }

    // -------------------------------------------------------------------------
    // 阶段 58：Nanite 的每帧推进必须有生产者，而且**每一条** Tick 路径都得有。
    //
    // 以前"推进 Nanite"这句话只有 `BuildRenderSystemFrameCommands` 的 DX12 分支里
    // 有，另外三条路径（并行的非 DX12、单线程的 DX12 / 非 DX12 —— Apple/Metal 走
    // 的就是最后这条）都没有 ⇒ Nanite 在真机上一次都没跑过，集群构建 / 裁剪 / 批次
    // 组装全是死代码，而且**不报任何错**（真机日志里 Nanite 一行都没有）。
    //
    // 这是"历史空桩在功能打通之后变成洞"的第六例，形态是"漏抄"：修法是把四条路径
    // 统一到 `RenderSystem::UpdateNaniteForFrame()` 这一个入口上，所以契约盯的是
    // "每个分支都真的调了它"，而不是"某个地方出现过 Nanite 这个字"。
    // -------------------------------------------------------------------------
    // =======================================================================
    // 1i. 粒子（所有平台都跑）
    //
    // Metal 上以前**根本没有粒子 pass**：`ParticlePass.cpp` 经传递 include 拉进某个
    // 后端自己的平台头，于是被 `ZENGINE_APPLE_EXCLUDED_PASSES` 整 TU 排除。画面上
    // 的表现是"场景里没有粒子"，和"有 pass 但没 emitter"、"有 emitter 但没画"长得
    // 一模一样 —— 只能靠探针分辨，所以心跳里 has / emit / draw / parts 分开打。
    //
    // 这一节钉四层：解耦本身、Apple 名单、运行时接线、**每帧推进**。最后一层是
    // 阶段 61 真正踩到的洞：粒子的两步推进原本只写在 `SubmitDrawLists` 里，而
    // Apple 那条精简 Tick **根本不走 SubmitDrawLists**（直接 `draw_list.ExecuteAll()`）
    // ⇒ 建管线、接深度法线、送 swap 数据全都做了，唯独 `Simulate()` 一次没跑，
    // 存活数恒为 0，且不报任何错。
    // =======================================================================
    void CheckParticleContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string particle_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Passes/ParticlePass.cpp");
        const std::string particle_header =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Passes/ParticlePass.h");
        const std::string rp1_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Passes/MainCameraRp1Pass.cpp");
        const std::string rp1_header =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Passes/MainCameraRp1Pass.h");
        const std::string module_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Pipeline/DesktopRenderPipelineModule.cpp");
        const std::string base_source = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderPipelineBase.cpp");
        const std::string base_header = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderPipelineBase.h");
        const std::string sys_source   = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        const std::string cmake_source = ReadFileText(engine_root + "/Source/Runtime/CMakeLists.txt");
        const std::string editor_source =
            ReadFileText(engine_root + "/Source/Editor/EditorApplication/EditorApplication.cpp");
        const std::string game_object_source = ReadFileText(engine_root + "/Source/Runtime/BaseClasses/GameObject.cpp");

        Check(!particle_source.empty(), "ParticlePass.cpp is readable as text (particle decoupling)");
        Check(!particle_header.empty(), "ParticlePass.h is readable as text (particle decoupling)");
        Check(!rp1_source.empty(), "MainCameraRp1Pass.cpp is readable as text (particle wiring)");
        Check(!module_source.empty(), "DesktopRenderPipelineModule.cpp is readable as text (particle wiring)");
        Check(!base_source.empty(), "RenderPipelineBase.cpp is readable as text (particle per-frame advance)");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (particle per-frame advance)");
        Check(!cmake_source.empty(), "Runtime/CMakeLists.txt is readable as text (Apple exclusion list)");
        Check(!editor_source.empty(), "EditorApplication.cpp is readable as text (edit-mode tick whitelist)");
        Check(!game_object_source.empty(), "GameObject.cpp is readable as text (edit-mode tick dispatch)");

        // ---- 1. 解耦：这个 TU 不再拖任何后端私有的头 ----
        //
        // 只看 `#include` 那一行本身：正文注释里讲这段历史时**不许**逐字引用被禁的
        // 字面量（否则注释自己就把契约蒙过去了），所以逐行扫比全文 find 更靠得住。
        auto include_line_mentions = [](const std::string& src, const std::string& needle) {
            size_t at = 0;
            while ((at = src.find("#include", at)) != std::string::npos)
            {
                const size_t eol  = src.find('\n', at);
                const std::string line =
                    src.substr(at, (eol == std::string::npos ? src.size() : eol) - at);
                if (line.find(needle) != std::string::npos)
                {
                    return true;
                }
                at += 8;
            }
            return false;
        };
        // ⚠️ 逐行扫**代码行**（跳过整行注释）。
        //
        // 没有它，"把一行代码注释掉"这种最普通的回归会**照样命中**契约：变异测试
        // 里把 `registerEdtorTickComponent("ParticleComponent")` 注释掉，全文 find
        // 依然找得到那个串，319 条检查全绿 —— 这是"文本契约被自己的注释蒙过去"
        // 的第 5 次。凡是钉"调用点"的断言都必须走这个，不能走全文 Contains。
        auto code_line_find = [](const std::string& src, const std::string& needle) {
            size_t at = 0;
            while (at < src.size())
            {
                size_t eol = src.find('\n', at);
                if (eol == std::string::npos)
                {
                    eol = src.size();
                }
                const std::string line  = src.substr(at, eol - at);
                const size_t      first = line.find_first_not_of(" \t");
                const bool        commented =
                    first != std::string::npos && line.compare(first, 2, "//") == 0;
                const size_t hit = line.find(needle);
                if (!commented && hit != std::string::npos)
                {
                    return at + hit;
                }
                at = eol + 1;
            }
            return std::string::npos;
        };
        auto code_line_mentions = [&code_line_find](const std::string& src, const std::string& needle) {
            return code_line_find(src, needle) != std::string::npos;
        };

        Check(!include_line_mentions(particle_source, "ulkan"),
              "ParticlePass.cpp includes no backend-private header (that is what excluded it on Apple)");
        Check(!include_line_mentions(particle_header, "ulkan"),
              "ParticlePass.h includes no backend-private header either");
        Check(CountOccurrences(particle_source, "VkDeviceSize") == 0,
              "ParticlePass.cpp uses the RHI device-size typedef, not the backend one");
        Check(CountOccurrences(particle_source, "VkSpecializationMapEntry") == 0,
              "ParticlePass.cpp uses the RHI specialization-map typedef, not the backend one");
        Check(CountOccurrences(particle_source, "VkPipelineStageFlags") == 0,
              "ParticlePass.cpp uses the RHI pipeline-stage typedef, not the backend one");
        Check(CountOccurrences(particle_source, "VK_TRUE") == 0,
              "ParticlePass.cpp uses RHI_TRUE, not the backend boolean constant");
        Check(CountOccurrences(particle_source, "vkCreatePipelineCache") == 0,
              "the dead direct backend-API call is gone (it was the only one in this TU)");

        // ---- 2. Apple 的排除名单里不再有 ParticlePass ----
        const size_t excl_at  = cmake_source.find("set(ZENGINE_APPLE_EXCLUDED_PASSES");
        const size_t excl_end = cmake_source.find("list(FILTER", excl_at == std::string::npos ? 0 : excl_at);
        Check(excl_at != std::string::npos && excl_end != std::string::npos && excl_at < excl_end,
              "the Apple exclusion list is one set() immediately followed by a list(FILTER)");
        const std::string excl_block =
            (excl_at == std::string::npos || excl_end == std::string::npos)
                ? std::string()
                : cmake_source.substr(excl_at, excl_end - excl_at);
        Check(!excl_block.empty() && !Contains(excl_block, "ParticlePass"),
              "ParticlePass is no longer in the Apple-excluded passes (Metal really builds it)");

        // ---- 3. 运行时接线：建 pass、递给 RP1、在 forward 那一站画 ----
        Check(code_line_mentions(module_source, "m_Pipeline.m_ParticlePass = std::make_shared<ParticlePass>()"),
              "the desktop pipeline creates the particle pass");
        Check(code_line_mentions(rp1_header, "SetParticlePass("), "MainCameraRp1Pass exposes SetParticlePass()");
        Check(code_line_mentions(module_source, "rp1->SetParticlePass("),
              "the particle pass is handed to the RP1 pass (Apple draws billboards in the forward station)");
        Check(code_line_mentions(rp1_source, "m_ParticlePass->Draw()"),
              "RP1 draws the particle billboards (once per pass, inside the forward lighting station)");

        // ⚠️ 两个同名函数都要调。只调 RP1 这个（递深度图 / 法线 / render pass）是
        // **不够的**：pass 自己的那个才负责映射 uniform buffer，漏掉它 `Draw()` 会
        // 往两个 nullptr 里 memcpy（KERN_INVALID_ADDRESS at 0x0，栈在
        // `_platform_memmove`）。这个崩溃实打实踩过一次。
        const size_t rp1_setup_at  = rp1_source.find("void MainCameraRp1Pass::SetupParticlePass()");
        const size_t rp1_setup_end = rp1_source.find("\n}", rp1_setup_at == std::string::npos ? 0 : rp1_setup_at);
        Check(rp1_setup_at != std::string::npos && rp1_setup_end != std::string::npos,
              "MainCameraRp1Pass::SetupParticlePass is defined (sliced to its closing brace)");
        const std::string rp1_setup_body =
            (rp1_setup_at == std::string::npos || rp1_setup_end == std::string::npos)
                ? std::string()
                : rp1_source.substr(rp1_setup_at, rp1_setup_end - rp1_setup_at);
        // 必须带 `m_ParticlePass->`：正文注释里引用的是 `ParticlePass::SetupParticlePass()`，
        // 光搜函数名会先命中注释 —— 那正是"注释把契约蒙过去"的老毛病。
        Check(!rp1_setup_body.empty() && code_line_mentions(rp1_setup_body, "m_ParticlePass->SetupParticlePass()"),
              "it also calls the pass's OWN SetupParticlePass (uniform buffers are mapped there)");
        Check(code_line_mentions(rp1_setup_body, "SetDepthAndNormalImage("),
              "it hands the depth + normal images to the particle pass (collision reads them)");
        Check(code_line_mentions(rp1_setup_body, "SetRenderPassHandle("),
              "it hands the forward lighting render pass to the particle pass (billboards draw there)");

        // ---- 4. 消费侧的判据是"有没有粒子 pass"，不是"是不是某个后端" ----
        const size_t consume_at  = module_source.find("void DesktopRenderPipelineModule::ConsumeParticleSwapData(");
        const size_t consume_end = module_source.find("\n}", consume_at == std::string::npos ? 0 : consume_at);
        Check(consume_at != std::string::npos && consume_end != std::string::npos,
              "DesktopRenderPipelineModule::ConsumeParticleSwapData is defined (sliced to its closing brace)");
        const std::string consume_body =
            (consume_at == std::string::npos || consume_end == std::string::npos)
                ? std::string()
                : module_source.substr(consume_at, consume_end - consume_at);
        Check(!consume_body.empty() && !Contains(consume_body, "Z_HAS_VULKAN"),
              "the particle swap drain is not gated on a backend macro any more (Metal used to drop it)");
        Check(code_line_mentions(consume_body, "if (!m_Pipeline.m_ParticlePass)"),
              "the real gate is 'does this pipeline have a particle pass'");
        Check(code_line_mentions(consume_body, "SetTickIndices("),
              "the emitter tick request reaches the pass (without it Simulate() loops over nothing)");

        // ---- 5. 每帧推进：夹在 draw list 录制的两头，且两条 Tick 路径都要有 ----
        Check(code_line_mentions(base_header, "void PrepareParticlesForDraw(RHICommandBuffer* command_buffer);"),
              "RenderPipelineBase exposes PrepareParticlesForDraw (called before the draw list is recorded)");
        Check(code_line_mentions(base_header, "void AdvanceParticlesAfterDraw();"),
              "RenderPipelineBase exposes AdvanceParticlesAfterDraw (called after the draw list is recorded)");
        const size_t advance_at  = base_source.find("void RenderPipelineBase::AdvanceParticlesAfterDraw()");
        const size_t advance_end = base_source.find("\n}", advance_at == std::string::npos ? 0 : advance_at);
        Check(advance_at != std::string::npos && advance_end != std::string::npos,
              "RenderPipelineBase::AdvanceParticlesAfterDraw is defined (sliced to its closing brace)");
        const std::string advance_body =
            (advance_at == std::string::npos || advance_end == std::string::npos)
                ? std::string()
                : base_source.substr(advance_at, advance_end - advance_at);
        Check(!advance_body.empty() && code_line_mentions(advance_body, "CopyNormalAndDepthImage()"),
              "the advance step copies this frame's depth + normal images");
        Check(code_line_mentions(advance_body, "Simulate()"),
              "the advance step runs the particle simulation (this is what used to never run on Metal)");

        // Apple 那条精简 Tick：不走 SubmitDrawLists，必须自己叫一遍。
        const size_t apple_tick_at  = sys_source.find("void RenderSystem::Tick(float");
        const size_t apple_tick_end = sys_source.find("\nvoid ", apple_tick_at + 1);
        Check(apple_tick_at != std::string::npos && apple_tick_end != std::string::npos,
              "RenderSystem::Tick is the Apple tick (sliced to the next top-level function)");
        const std::string apple_tick =
            (apple_tick_at == std::string::npos || apple_tick_end == std::string::npos)
                ? std::string()
                : sys_source.substr(apple_tick_at, apple_tick_end - apple_tick_at);
        const size_t exec_at = code_line_find(apple_tick, "draw_list.ExecuteAll()");
        const size_t prep_at = code_line_find(apple_tick, "PrepareParticlesForDraw(");
        const size_t adv_at  = code_line_find(apple_tick, "AdvanceParticlesAfterDraw(");
        Check(exec_at != std::string::npos, "the Apple tick records its draw list with ExecuteAll()");
        Check(prep_at != std::string::npos && exec_at != std::string::npos && prep_at < exec_at,
              "the Apple tick hands the command buffer to the particle pass BEFORE ExecuteAll()");
        Check(adv_at != std::string::npos && exec_at != std::string::npos && adv_at > exec_at,
              "the Apple tick advances the particles AFTER ExecuteAll() (Apple never calls SubmitDrawLists)");

        // ---- 6. 编辑态 tick：ParticleComponent::Tick 是 tick 请求的唯一生产者 ----
        Check(code_line_mentions(editor_source, "registerEdtorTickComponent(\"ParticleComponent\")"),
              "ParticleComponent is in the edit-mode tick whitelist (its Tick is the only tick producer)");
        Check(code_line_mentions(game_object_source, "g_editorTickComponentTypes"),
              "GameObject::Tick really gates on that whitelist when not playing (context for the check above)");

        // ---- 7. prefab 定义树不许跑 post-load（1 个发射器只能开 1 份 GPU 资源）----
        //
        // `GameObject::ExpandDefinition` 把 prefab 的组件并进场景对象，随后整棵定义
        // 树就被丢掉 —— 它不在任何 Level 里、永不 tick、也没人析构。以前这一步照常
        // 跑 PostLoadResource ⇒ 模板对象上的组件**先**按模板自己的变换初始化一遍
        // （粒子发射器在原点开一份 GPU 资源 + 拿一个 emitter id），场景实例再初始
        // 化一遍 ⇒ 场景里 1 个发射器开出 2 份资源、心跳 `emit=2` 而 tick 只有 1 份。
        const std::string prefab_source =
            ReadFileText(engine_root + "/Source/Runtime/Resource/Prefab/PrefabUtility.cpp");
        const std::string prefab_header =
            ReadFileText(engine_root + "/Source/Runtime/Resource/Prefab/PrefabUtility.h");
        const std::string placement_source =
            ReadFileText(engine_root + "/Source/Editor/EditorScenePlacement/EditorScenePlacement.cpp");
        Check(!prefab_source.empty(), "PrefabUtility.cpp is readable as text (definition post-load)");
        Check(!prefab_header.empty(), "PrefabUtility.h is readable as text (definition post-load)");
        Check(!placement_source.empty(), "EditorScenePlacement.cpp is readable as text (definition post-load)");

        Check(code_line_mentions(prefab_header, "static GameObject* Instantiate(PrefabAsset* asset, bool invoke_post_load = true);"),
              "PrefabUtility::Instantiate takes an invoke_post_load flag (defaults to the old behaviour)");
        Check(code_line_mentions(prefab_header, "static GameObject* InstantiateFromPath(const std::filesystem::path& prefab_path,"),
              "PrefabUtility::InstantiateFromPath forwards it");
        // 生效那一步：那个 flag 必须真的挡住 PostLoadResource，而不只是传进来。
        const size_t guard_at = code_line_find(prefab_source, "if (m_InvokePostLoad)");
        const size_t plr_at   = code_line_find(prefab_source, "component->PostLoadResource(go)");
        Check(guard_at != std::string::npos && plr_at != std::string::npos && guard_at < plr_at,
              "the flag actually gates the single PostLoadResource call site in the prefab driver");
        Check(CountOccurrences(prefab_source, "component->PostLoadResource(go)") == 1,
              "there is exactly one PostLoadResource call site to gate (a second one would leak again)");
        // flag 必须**真的传下去**（两条路各一个转发点），否则签名改了等于没改。
        //
        // 这里只钉"driver.Run( 这一行带着 invoke_post_load"，不写死实参变量名：
        // 以前写成 `driver.Run(root, graph_gameobjects, invoke_post_load)`，后来
        // 那两个参数改名成 clone.clone_root / clone_gameobjects，功能没变而断言
        // 变红 —— 盯变量名只会让人在改名时误以为转发被删了。
        const size_t driver_run_at = code_line_find(prefab_source, "driver.Run(");
        Check(driver_run_at != std::string::npos &&
                  prefab_source.substr(driver_run_at,
                                       prefab_source.find('\n', driver_run_at) - driver_run_at)
                      .find("invoke_post_load") != std::string::npos,
              "the .prefab (yaml) path forwards the flag to the driver");
        Check(code_line_mentions(prefab_source, "return Instantiate(asset, invoke_post_load);"),
              "the binary .zasset path forwards the flag too");

        // definition 那一支必须显式关掉；真正的实例化（拖 prefab 进场景）必须照旧开。
        const size_t expand_at  = game_object_source.find("void GameObject::ExpandDefinition()");
        const size_t expand_end = game_object_source.find("\n}", expand_at == std::string::npos ? 0 : expand_at);
        Check(expand_at != std::string::npos && expand_end != std::string::npos,
              "GameObject::ExpandDefinition is defined (sliced to its closing brace)");
        const std::string expand_body =
            (expand_at == std::string::npos || expand_end == std::string::npos)
                ? std::string()
                : game_object_source.substr(expand_at, expand_end - expand_at);
        // 搜的是**调用形式**（带 `/*invoke_post_load=*/false`），正文注释里那句
        // `invoke_post_load = false` 是带空格的另一种写法，不会命中 —— 但保险起见
        // 仍然走逐行扫代码。
        Check(!expand_body.empty() &&
                  code_line_mentions(expand_body, "InstantiateFromPath(prefab_path, /*invoke_post_load=*/false)"),
              "definition expansion turns post-load OFF (its components get post-loaded by load() instead)");
        // 场景拖放这条路现在走 PrefabSceneBinding::InstantiateIntoLevel →
        // PrefabUtilityEditor::InstantiateAsPrefab，**不再经过**
        // PrefabUtility::InstantiateFromPath（那条才是带 invoke_post_load 的）。
        // 意图没变：拖进来的 prefab 是真对象，子树要真的 post-load 一遍。
        const std::string prefab_editor_source =
            ReadFileText(engine_root + "/Source/Editor/Prefab/PrefabUtilityEditor.cpp");
        Check(!prefab_editor_source.empty(), "PrefabUtilityEditor.cpp is readable as text (scene placement)");

        const size_t placement_call = code_line_find(placement_source, "PrefabSceneBinding::InstantiateIntoLevel(");
        Check(placement_call != std::string::npos,
              "the scene-placement path goes through PrefabSceneBinding::InstantiateIntoLevel");
        Check(code_line_mentions(prefab_editor_source, "comp->PostLoadResource(clone_go)"),
              "the scene-placement path still instantiates with post-load ON "
              "(dropping a prefab is a real object -- the clone subtree is post-loaded)");
        // 取到行尾，别写死长度；`placement_call` 为 npos 时**不能** substr（会抛
        // out_of_range 把整个测试带崩，变异就报不成"几条红"了）。
        std::string placement_line;
        if (placement_call != std::string::npos)
        {
            const size_t eol = placement_source.find('\n', placement_call);
            placement_line   = placement_source.substr(
                placement_call, (eol == std::string::npos ? placement_source.size() : eol) - placement_call);
        }
        // 这一条以前写成 `placement_call == npos || !Contains(...)`：调用点一旦
        // 找不到（重构改名就是这么发生的），它就**恒真** —— 典型的空断言，看起来
        // 绿，其实什么都没查。现在要求先找到调用点。
        Check(placement_call != std::string::npos && !Contains(placement_line, "invoke_post_load"),
              "the scene-placement call passes no flag (it must keep the default = post-load)");

        // ---- 8. 心跳：三个数分开打，才能分辨"没 pass / 没 emitter / 没画" ----
        Check(code_line_mentions(sys_source, "particle=(has="),
              "the Apple heartbeat reports the particle state (has/emit/draw/parts/vp separately)");
        Check(code_line_mentions(particle_header, "GetLastDrawnParticles()"),
              "the pass exposes the drawn particle count (0 = alive count never advanced)");
    }

    void CheckNaniteTickContracts()
    {
        const std::string engine_root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string sys_source  = ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        const std::string cmd_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/RenderSystemFrameCommands.cpp");
        Check(!sys_source.empty(), "RenderSystem.cpp is readable as text (nanite tick)");
        Check(!cmd_source.empty(), "RenderSystemFrameCommands.cpp is readable as text (nanite tick)");

        auto count_of = [](const std::string& haystack, const std::string& needle) {
            size_t count = 0;
            size_t at    = 0;
            while ((at = haystack.find(needle, at)) != std::string::npos)
            {
                ++count;
                at += needle.size();
            }
            return count;
        };
        // 从函数签名切到下一个顶层函数定义为止。
        auto body_of = [](const std::string& source, const std::string& signature) {
            const size_t at  = source.find(signature);
            const size_t end = source.find("\nvoid ", at + 1);
            if (at == std::string::npos)
            {
                return std::string {};
            }
            return source.substr(at, end == std::string::npos ? source.size() - at : end - at);
        };

        // 1. 唯一入口存在。
        Check(Contains(sys_source, "void RenderSystem::UpdateNaniteForFrame(float"),
              "RenderSystem has exactly one Nanite-per-frame entry point");

        // 2. Apple 那条精简 Tick 必须调它（这是真机走的路）。
        const std::string apple_tick = body_of(sys_source, "void RenderSystem::Tick(float");
        Check(Contains(apple_tick, "UpdateNaniteForFrame(delta_time)"),
              "the Apple tick advances Nanite every frame (it used to be dead code on Metal)");

        // 3. 单线程路径的两条分支（DX12 / 非 DX12）也都要调。
        const std::string tick_render_thread = body_of(sys_source, "void RenderSystem::TickRenderThread(");
        Check(count_of(tick_render_thread, "UpdateNaniteForFrame(delta_time)") >= 2,
              "both branches of TickRenderThread advance Nanite (DX12 and non-DX12)");

        // 4. 并行路径：DX12 分支把 RenderNaniteCmd 排进队列；**非 DX12（= Vulkan）分支不许排** ——
        //    Nanite 的 GPU 剔除往 GetCurrentCommandBuffer() 里录，而 Vulkan 的帧命令缓冲归 RHI
        //    线程、要到 RHIPrepareContextCmd 才设上：第一帧是空句柄（实测 vkCmdBindPipeline AV），
        //    之后是 RHI 线程正在录的那条。在把它挪成 RHI 命令之前，这一支保持不推进。
        //    （Apple 不走这里：它在 `#if defined(__APPLE__)` 的 Tick 里、PrepareContext 之后调。）
        const std::string build_cmds = body_of(cmd_source, "void BuildRenderSystemFrameCommands(");
        Check(count_of(build_cmds, "RenderNaniteCmd, &render_system") == 1,
              "only the DX12 branch of BuildRenderSystemFrameCommands enqueues RenderNaniteCmd "
              "(the Vulkan frame command buffer belongs to the RHI thread)");

        // 5. 帧命令自己不许再抄一遍判空逻辑 —— 走共享入口，否则将来改一处漏三处。
        const std::string nanite_cmd = body_of(cmd_source, "void Execute() override\n    {\n        // 走 RenderSystem");
        Check(Contains(nanite_cmd, "system->UpdateNaniteForFrame(delta_time)"),
              "RenderNaniteCmd delegates to the shared entry point instead of duplicating it");

        // 6. 顺序：Apple Tick 里必须在可见性更新之后、PreparePassData 之前。
        //    依旧要搜**调用形式**，注释里那句"可见性更新之后"会把只搜名字的写法蒙过去。
        const size_t nanite_at = apple_tick.find("UpdateNaniteForFrame(delta_time)");
        const size_t publish_at = apple_tick.find("PublishVisibleNodes()");
        const size_t prepare_at = apple_tick.find("PreparePassData(m_RenderResource)");
        Check(publish_at != std::string::npos && nanite_at != std::string::npos && publish_at < nanite_at,
              "Nanite runs after the visibility update (it reads the scene instances)");
        Check(prepare_at != std::string::npos && nanite_at < prepare_at,
              "and before PreparePassData (which uploads this frame's GPU resources)");

        // 7. 心跳探针：Nanite 静默退化时不报错，只能靠探针分辨。
        Check(Contains(sys_source, "nanite=(has={} inst={} batches={} vis={} ngpu={})"),
              "the Apple heartbeat carries a Nanite probe (a silent fallback prints nothing)");
        Check(Contains(sys_source, "m_RenderPipeline->GetNanitePass()"),
              "that probe reads the real Nanite pass (has/inst/batches/vis together tell apart "
              "'no pass' / 'no instances' / 'all culled')");
        // ngpu：Nanite 的 GPU 路径是**静默**退化到 CPU 的（inst/batches 照常产出），
        // 所以"这一帧的集群裁决在 kernel 里还是 CPU 上"只能靠这一个数分辨。
        Check(Contains(sys_source, "IsGPUComputeReady()"),
              "the heartbeat also reports whether the GPU path is live (ngpu) -- Nanite falls back "
              "to CPU without any log, so this number is the only way to tell");

        // ---- 阶段 59：GPU 路径的每一步都要能被证伪 ----
        //
        // 真机上曾经出现过一次**误判**：我 grep 日志找那条一次性成功日志，两个错误
        // 叠加（bash grep -i "a\|b" 假阴性 + BqLog 按天 rotate 后查错了文件）⇒ 明明
        // 跑着的东西被我当成没跑。结论是：能自动验的就别靠人肉 grep。
        const std::string nanite_source =
            ReadFileText(engine_root + "/Source/Runtime/Function/Render/Nanite/NaniteRenderer.cpp");
        Check(!nanite_source.empty(), "NaniteRenderer.cpp is readable as text (nanite gpu diag)");

        // 五个出口 + r.Nanite.GPU=0 那一个，每个都得有诊断，否则"卡在哪一步"不可知。
        const std::string prepare_body = body_of(nanite_source, "bool NaniteRenderer::PrepareGPUData(");
        Check(count_of(prepare_body, "diag(") >= 4,
              "every exit of PrepareGPUData carries a one-shot diagnostic (a silent fallback is "
              "otherwise unfalsifiable)");
        Check(Contains(nanite_source, "use_gpu_culling is off"),
              "the 'r.Nanite.GPU=0' exit is diagnosed too (it is the quietest one -- PrepareGPUData "
              "is never even called)");
        // "管线建成了" ≠ "真的发了 dispatch"，两边都得有日志。
        Check(Contains(nanite_source, "nanite: dispatched cluster culling"),
              "DispatchClusterCulling logs once too -- a built pipeline does not prove a dispatch");
        Check(Contains(nanite_source, "m_DispatchDiagLogged"),
              "that dispatch log is one-shot (it would otherwise spam every frame)");
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

    // 某一行某一通道的平均值。LUT 的三条轴各走各的 ⇒ 必须逐通道对拍，
    // 只看三通道均值的话轴向搞反根本看不出来。
    float RowChannel(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t row, uint32_t channel)
    {
        if (pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(row + 1) * 4u)
        {
            return -1.0f;
        }
        uint32_t sum = 0;
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t i = (static_cast<size_t>(row) * static_cast<size_t>(width) + x) * 4u;
            sum += static_cast<uint32_t>(pixels[i + channel]);
        }
        return static_cast<float>(sum) / static_cast<float>(width);
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

    // =======================================================================
    // 恒等 LUT / 常量 LUT / 彩色场景：都是"按 [[position]] 算一个值写出去"。
    //
    // 恒等 LUT 是这份测试里最关键的一张图：格点值等于自己的坐标，于是
    // "LUT 采样"必须退化成恒等变换。任何轴向错误、内缩错误、跨切片错误都会
    // 让它不再是恒等 —— 但它**只在输入非灰时才看得出来**（恒等 LUT 下把
    // (r,g,b) 换成 (b,g,r)，灰度输入给出完全相同的值）。所以场景那张图特意
    // 做成三条通道各走各的斜坡。
    // =======================================================================
    const char* kIdentityLutFragMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GradientVertexOut
{
    float4 position [[position]];
};

// 256x16：切片 = floor(x/16)，切片内 r = x - slice*16，g = y。
fragment float4 IdentityLutFragmentMain(GradientVertexOut in [[stage_in]])
{
    float x     = floor(in.position.x);
    float y     = floor(in.position.y);
    float slice = floor(x / 16.0);
    float r_in  = x - slice * 16.0;
    return float4(r_in / 15.0, y / 15.0, slice / 15.0, 1.0);
}
)MSL";

    const char* kConstantLutFragMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GradientVertexOut
{
    float4 position [[position]];
};

// 整张 LUT 一个值：只要 shader 真的去读了 binding 1，输出就一定是这个值的
// gamma。它和"恒等"是两个独立可辨的签名 —— 绕过 LUT 的话两者会同归于 blit 值。
fragment float4 ConstantLutFragmentMain(GradientVertexOut in [[stage_in]])
{
    return float4(0.25, 0.5, 0.75, 1.0);
}
)MSL";

    // 彩色场景：R 从 0.25 爬到 0.75，G 恒 0.5，B 从 0.75 降到 0.25。
    // 三条通道分开走 ⇒ LUT 的轴向一旦搞反，逐通道对拍立刻红。
    const char* kColorGradientFragMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GradientVertexOut
{
    float4 position [[position]];
};

fragment float4 ColorGradientFragmentMain(GradientVertexOut in [[stage_in]])
{
    float t = in.position.y / 32.0;
    return float4(0.25 + 0.5 * t, 0.5, 0.75 - 0.5 * t, 1.0);
}
)MSL";

    // =======================================================================
    // IBL cubemap 的替身：binding 0 是一张 RGBA32F cubemap（真 IBL 的格式，
    // stbi_loadf 出来的就是 4 通道 float），从 +X 方向采一次直接写出去。
    //
    // 这条 GPU 检查钉的是 MetalRHI 上传路径的**每 texel 字节数**：那两处以前
    // 硬编码 `width * 4`，对 R8G8B8A8 的兜底图是对的，对 HDR 的 16 字节/texel
    // 是错的 —— replaceRegion 会按错的 row pitch 去读源内存。面内 4 个 texel
    // 特意做成不相等的（x/y 各带一个斜坡），pitch 错了采出来的中心值就变了。
    // =======================================================================
    const char* kIblCubeFragMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GradientVertexOut
{
    float4 position [[position]];
};

fragment float4 IblCubeFragmentMain(GradientVertexOut in [[stage_in]],
                                    texturecube<float> ibl_cube [[texture(0)]],
                                    sampler ibl_sampler [[sampler(0)]])
{
    float3 c = ibl_cube.sample(ibl_sampler, float3(1.0, 0.0, 0.0)).rgb;
    return float4(c, 1.0);
}
)MSL";

    // 从 AppleRp2Chain.cpp 里把真实的 MSL 字符串抠出来。
    //
    // 为什么不复制一份到测试里：复制品和真身会各自漂移，而真身只在运行时才
    // 见编译器 —— 复制品过了不代表真身过了。这个测试里跑的就是**真身那段
    // 字符串**，所以 MSL 的编译、槽位、公式三者一次全验。
    std::string ExtractMslBetween(const std::string& source, const char* name)
    {
        const std::string begin_marker = std::string(name) + " = R\"MSL(";
        const size_t      begin        = source.find(begin_marker);
        if (begin == std::string::npos)
        {
            return std::string();
        }
        const size_t start = begin + begin_marker.size();
        const size_t end   = source.find(")MSL\"", start);
        if (end == std::string::npos)
        {
            return std::string();
        }
        return source.substr(start, end - start);
    }

    RHIPipeline* MakeGradientPipeline(RHI* rhi, RHIRenderPass* render_pass, const char* frag_msl)
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
        RHIShader* frag = rhi->CreateShaderModuleFromSource(frag_msl, ShaderStage::Fragment, "gradient.frag");
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

    void SetViewport(RHI* rhi, RHICommandBuffer* command_buffer, uint32_t width, uint32_t height)
    {
        RHIViewport viewport {};
        viewport.width    = static_cast<float>(width);
        viewport.height   = static_cast<float>(height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);

        RHIRect2D scissor {};
        scissor.extent.width  = width;
        scissor.extent.height = height;
        rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);
    }

    void SetFullViewport(RHI* rhi, RHICommandBuffer* command_buffer)
    {
        SetViewport(rhi, command_buffer, kSize, kSize);
    }

    // -----------------------------------------------------------------------
    // RP2 的替身：两个 COMBINED_IMAGE_SAMPLER（0 = 场景，1 = LUT）+ 从
    // AppleRp2Chain.cpp 抠出来的真身 MSL。
    //
    // 不能直接用 AppleRp2Chain：它要 MainCameraFramebufferResources，那东西
    // 要一整套 GBuffer / 光照附件。这里只需要它**那段 shader**和**那个槽位
    // 布局**，于是照抄布局、复用真身 shader 文本。
    // -----------------------------------------------------------------------
    struct Rp2Harness
    {
        RHIDescriptorSetLayout* layout {nullptr};
        RHIPipelineLayout* pipeline_layout {nullptr};
        RHIDescriptorSet* set {nullptr};
        RHISampler* sampler {nullptr};
        RHIPipeline* pipeline {nullptr};
    };

    bool MakeRp2Harness(RHI* rhi, RHIRenderPass* render_pass, const std::string& vert_msl, const std::string& frag_msl,
                        Rp2Harness& out)
    {
        if (rhi == nullptr || render_pass == nullptr || vert_msl.empty() || frag_msl.empty())
        {
            return false;
        }

        RHIDescriptorSetLayoutBinding bindings[2] {};
        for (uint32_t i = 0; i < 2; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = RHI_SHADER_STAGE_FRAGMENT_BIT;
        }

        RHIDescriptorSetLayoutCreateInfo layout_info {};
        layout_info.sType        = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 2;
        layout_info.pBindings    = bindings;
        if (!rhi->CreateDescriptorSetLayout(&layout_info, out.layout) || out.layout == nullptr)
        {
            return false;
        }

        RHIPipelineLayoutCreateInfo pipeline_layout_info {};
        pipeline_layout_info.sType          = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts    = &out.layout;
        if (!rhi->CreatePipelineLayout(&pipeline_layout_info, out.pipeline_layout) || out.pipeline_layout == nullptr)
        {
            return false;
        }

        RHIDescriptorSetAllocateInfo alloc_info {};
        alloc_info.sType              = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool     = rhi->GetDescriptorPoor();
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &out.layout;
        if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&alloc_info, out.set) || out.set == nullptr)
        {
            return false;
        }

        RHISamplerCreateInfo sampler_info {};
        sampler_info.sType        = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter    = RHI_FILTER_LINEAR;
        sampler_info.minFilter    = RHI_FILTER_LINEAR;
        sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!rhi->CreateSampler(&sampler_info, out.sampler) || out.sampler == nullptr)
        {
            return false;
        }

        RHIShader* vert = rhi->CreateShaderModuleFromSource(vert_msl.c_str(), ShaderStage::Vertex, "rp2.vert");
        RHIShader* frag = rhi->CreateShaderModuleFromSource(frag_msl.c_str(), ShaderStage::Fragment, "rp2.frag");
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
        pipeline_info.layout              = out.pipeline_layout;
        pipeline_info.renderPass          = render_pass;
        pipeline_info.subpass             = 0;

        rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, out.pipeline);
        rhi->DestroyShaderModule(vert);
        rhi->DestroyShaderModule(frag);
        return out.pipeline != nullptr;
    }

    // 把 scene 过一遍 RP2（tonemap -> LUT -> gamma）写进 target。
    void RunRp2(RHI*            rhi,
                Rp2Harness&     harness,
                Offscreen&      target,
                RHIImageView*   scene_view,
                RHIImageView*   lut_view)
    {
        if (rhi == nullptr || harness.pipeline == nullptr || target.framebuffer == nullptr)
        {
            return;
        }

        RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
        if (command_buffer == nullptr)
        {
            return;
        }

        RHIDescriptorImageInfo image_infos[2] {};
        for (uint32_t i = 0; i < 2; ++i)
        {
            image_infos[i].sampler     = harness.sampler;
            image_infos[i].imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        image_infos[0].imageView = scene_view;
        image_infos[1].imageView = lut_view;

        RHIWriteDescriptorSet writes[2] {};
        for (uint32_t i = 0; i < 2; ++i)
        {
            writes[i].sType           = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = harness.set;
            writes[i].dstBinding      = i;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &image_infos[i];
        }
        rhi->UpdateDescriptorSets(2, writes, 0, nullptr);

        RHIClearValue clear {};
        clear.color.float32[3] = 1.0f;

        RHIRenderPassBeginInfo begin {};
        begin.sType                  = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass             = target.render_pass;
        begin.framebuffer            = target.framebuffer;
        begin.renderArea.extent.width  = kSize;
        begin.renderArea.extent.height = kSize;
        begin.clearValueCount        = 1;
        begin.pClearValues           = &clear;

        rhi->CmdBeginRenderPassPFN(command_buffer, &begin, RHI_SUBPASS_CONTENTS_INLINE);
        SetFullViewport(rhi, command_buffer);
        rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, harness.pipeline);
        rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                      RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                      harness.pipeline_layout,
                                      0,
                                      1,
                                      &harness.set,
                                      0,
                                      nullptr);
        rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
        rhi->CmdEndRenderPassPFN(command_buffer);
        rhi->EndSingleTimeCommands(command_buffer);
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

    // RP2 接线的文本契约（Apple 专属的两条管线 + RenderSystem 的同源判据）。
    CheckRp2Contracts();

    // LUT（颜色分级）的文本契约：槽位 1 的三处手工对齐 + 条带公式 + 采样序。
    CheckLutContracts();

    // 条带采样的数学镜像（纯数学，不需要 GPU ⇒ 所有平台都跑）。
    CheckStripLutMath();

    // IBL 的文本契约：Metal 上传的每 texel 字节数 + Apple 自己上传 + 顺序 + 日志。
    CheckIblContracts();
    CheckLevelResourceProducerContracts();

    // 场景视口的文本契约：Apple 的桩已删 + Metal 的 scissor 同步。
    CheckSceneViewportContracts();

    // Apple 版 Initialize 的相机来源契约：pose / near-far / aspect 必须来自
    // GlobalRenderingRes::m_CameraConfig，硬编码只能当兜底。
    CheckCameraConfigContracts();

    // Apple 版 Initialize 的光照契约：环境光 + 方向光必须真的设进 RenderScene。
    CheckGlobalLightingContracts();

    // Apple 版 Tick 的点光源同步契约 + 探针关卡本身（阶段 55）。
    CheckPointLightSyncContracts();
    CheckNaniteTickContracts();

    // 编辑器 gizmo（axis）：上传 + 绘制两半 + Metal dynamic offset（阶段 60）。
    CheckAxisContracts();

    // 粒子：解耦 + Apple 名单 + 运行时接线 + **每帧推进**（阶段 61）。
    CheckParticleContracts();

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
        blit.Record(nullptr, nullptr, true);  // 守卫：不能崩
        Check(true, "Record survives null command buffer and null source");
    }

    // =======================================================================
    // 3. 真的跑一遍（只有 Apple 有 Metal）
    //
    // 包一个 autorelease pool：MetalRHI 对 autorelease 的 command buffer / encoder
    // 的所有权一旦算错（多 release 一次），这里的 drain 就会野指针崩溃。没有 pool 时
    // 这类错永远看不出来（2026-10-26 修过一次，见 AppleSsaoSmokeTest §5）。
    // =======================================================================
    @autoreleasepool
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

        RHIPipeline* gradient = MakeGradientPipeline(rhi, scene.render_pass, kGradientFragMsl);
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
            blit.Record(command_buffer, nullptr, true);
            blit.Record(command_buffer, scene.view, true);

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

        // ---- 直通那条管线：RP2 起来之后 blit 走的就是它 ----
        //
        // 这是整条 RP2 接线里最容易悄悄坏掉的一环：两条管线共用同一个
        // pipeline layout / descriptor，只有 fragment 函数不同。挑错了不会报错，
        // 只会让画面二次 tonemap（发灰）—— 而"发灰"在真机上没有人会当成 bug 报。
        //
        // 断言方式是"照抄源值"：源是 UNORM8，直通出来必须还是同一个值（±2）。
        // 同时必须**不等于**刚才 tonemap 出来的那组值 —— 否则说明
        // `apply_tonemap=false` 被忽略了。
        Offscreen passthrough = MakeOffscreen(rhi, kSize, kSize, 0);
        Check(passthrough.framebuffer != nullptr, "the passthrough target was built");
        if (blit_ready && passthrough.framebuffer != nullptr)
        {
            RHIClearValue clear {};
            clear.color.float32[3] = 1.0f;

            RHIRenderPassBeginInfo begin {};
            begin.sType                  = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass             = passthrough.render_pass;
            begin.framebuffer            = passthrough.framebuffer;
            begin.renderArea.extent.width  = kSize;
            begin.renderArea.extent.height = kSize;
            begin.clearValueCount        = 1;
            begin.pClearValues           = &clear;

            RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
            rhi->CmdBeginRenderPassPFN(command_buffer, &begin, RHI_SUBPASS_CONTENTS_INLINE);
            SetFullViewport(rhi, command_buffer);
            blit.Record(command_buffer, scene.view, false);
            rhi->CmdEndRenderPassPFN(command_buffer);
            rhi->EndSingleTimeCommands(command_buffer);
        }

        const std::vector<uint8_t> passthrough_pixels = ReadBackPixels(rhi, passthrough.image, kSize, kSize);
        if (!passthrough_pixels.empty())
        {
            const float pt_top    = RowValue(passthrough_pixels, kSize, 0);
            const float pt_middle = RowValue(passthrough_pixels, kSize, kSize / 2);
            const float pt_bottom = RowValue(passthrough_pixels, kSize, kSize - 1);
            std::printf("       passthrough top=%6.1f mid=%6.1f bottom=%6.1f (source: %6.1f/%6.1f/%6.1f)\n",
                        pt_top, pt_middle, pt_bottom, scene_top, scene_middle, scene_bottom);

            // 直通 = 原样搬运：和源逐行相等（UNORM8 → UNORM8，只有采样精度误差）。
            Check(std::fabs(pt_top - scene_top) < 2.0f, "the passthrough pipeline copies the top row verbatim");
            Check(std::fabs(pt_middle - scene_middle) < 2.0f,
                  "the passthrough pipeline copies the middle row verbatim");
            Check(std::fabs(pt_bottom - scene_bottom) < 2.0f,
                  "the passthrough pipeline copies the bottom row verbatim");

            // 而且一定**不是** tonemap 出来的那组值 —— 这两组差了 40+ 个灰阶，
            // 误用 tonemap 管线一眼就能看出来。
            Check(std::fabs(pt_top - screen_top) > 40.0f,
                  "apply_tonemap=false really skips the tonemap (not the same numbers as the tonemap run)");
        }
        DestroyOffscreen(rhi, passthrough);

        // ===================================================================
        // 4. 颜色分级 LUT：真身 MSL + 真身槽位布局，跑两遍
        //
        //    这是全库第一次真的采样那张 256x16 条带（Vulkan 的
        //    glsl/color_grading.frag 把 texture() 注释掉了，DX12 的 hlsl 写明
        //    "bound but unused"），所以没有既有结果可对拍 —— 只能靠"恒等 LUT
        //    必须是恒等"这条性质。
        // ===================================================================
        {
            const std::string rp2_source = ReadFileText(
                std::string(ZENGINE_ENGINE_ROOT_DIR) + "/Source/Runtime/Function/Render/AppleRp2Chain.cpp");
            const std::string rp2_vert = ExtractMslBetween(rp2_source, "kAppleRp2VertMsl");
            const std::string rp2_frag = ExtractMslBetween(rp2_source, "kAppleRp2FragMsl");
            Check(!rp2_vert.empty() && !rp2_frag.empty(),
                  "the real RP2 MSL strings were extracted from AppleRp2Chain.cpp");
            Check(Contains(rp2_frag, "SampleColorGradingLut(lut_texture"),
                  "the extracted RP2 fragment really calls the LUT sampler");

            Offscreen lut_identity = MakeOffscreen(rhi, 256, 16, 0);
            Check(lut_identity.framebuffer != nullptr, "the 256x16 stand-in LUT target was built");

            Offscreen color_scene = MakeOffscreen(rhi, kSize, kSize, 0);
            Check(color_scene.framebuffer != nullptr, "the coloured scene target was built");

            RHIPipeline* identity_lut_pipeline =
                MakeGradientPipeline(rhi, lut_identity.render_pass, kIdentityLutFragMsl);
            Check(identity_lut_pipeline != nullptr, "the identity-LUT pipeline builds");

            RHIPipeline* color_scene_pipeline =
                MakeGradientPipeline(rhi, color_scene.render_pass, kColorGradientFragMsl);
            Check(color_scene_pipeline != nullptr, "the coloured-gradient pipeline builds");

            // ---- 把恒等 LUT 画进 256x16 ----
            if (identity_lut_pipeline != nullptr)
            {
                RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
                RHIClearValue     clear {};
                clear.color.float32[3] = 1.0f;
                RHIRenderPassBeginInfo begin {};
                begin.sType                  = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin.renderPass             = lut_identity.render_pass;
                begin.framebuffer            = lut_identity.framebuffer;
                begin.renderArea.extent.width  = 256;
                begin.renderArea.extent.height = 16;
                begin.clearValueCount        = 1;
                begin.pClearValues           = &clear;
                rhi->CmdBeginRenderPassPFN(command_buffer, &begin, RHI_SUBPASS_CONTENTS_INLINE);
                SetViewport(rhi, command_buffer, 256, 16);
                rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, identity_lut_pipeline);
                rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
                rhi->CmdEndRenderPassPFN(command_buffer);
                rhi->EndSingleTimeCommands(command_buffer);
            }

            // ---- 把彩色场景画进 32x32 ----
            if (color_scene_pipeline != nullptr)
            {
                RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
                RHIClearValue     clear {};
                clear.color.float32[3] = 1.0f;
                RHIRenderPassBeginInfo begin {};
                begin.sType                  = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin.renderPass             = color_scene.render_pass;
                begin.framebuffer            = color_scene.framebuffer;
                begin.renderArea.extent.width  = kSize;
                begin.renderArea.extent.height = kSize;
                begin.clearValueCount        = 1;
                begin.pClearValues           = &clear;
                rhi->CmdBeginRenderPassPFN(command_buffer, &begin, RHI_SUBPASS_CONTENTS_INLINE);
                SetFullViewport(rhi, command_buffer);
                rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, color_scene_pipeline);
                rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
                rhi->CmdEndRenderPassPFN(command_buffer);
                rhi->EndSingleTimeCommands(command_buffer);
            }

            const std::vector<uint8_t> color_pixels = ReadBackPixels(rhi, color_scene.image, kSize, kSize);
            Check(!color_pixels.empty(), "the coloured scene read back");

            Rp2Harness harness {};
            const bool harness_ok =
                MakeRp2Harness(rhi, screen.render_pass, rp2_vert, rp2_frag, harness);
            Check(harness_ok,
                  "the real RP2 MSL compiles and builds a two-binding pipeline (LUT slot really is wired up)");

            if (harness_ok && !color_pixels.empty() && lut_identity.view != nullptr)
            {
                // ---- 第一遍：恒等 LUT ----
                //
                // 期望值 = 逐通道 TonemapGamma(场景)。恒等 LUT 不改变任何东西，
                // 所以出来必须和"没有 LUT"完全一致；而三条通道在场景里各走各的
                // （顶行 R≈0.25 / G=0.5 / B≈0.75）⇒ 轴向一旦搞反，某一个通道
                // 就会拿到别人的期望值，差值 50+ 个灰阶。
                RunRp2(rhi, harness, screen, color_scene.view, lut_identity.view);
                const std::vector<uint8_t> graded = ReadBackPixels(rhi, screen.image, kSize, kSize);

                // 不能取正中间那一行：R 的斜坡（0.25→0.75）和 B 的斜坡
                // （0.75→0.25）在 t=0.5 处交叉，两条通道值相同 ⇒ 轴向搞反也
                // 看不出来。取靠上的行，t≈0.125 ⇒ R≈0.31 / B≈0.69。
                const uint32_t row = 4;
                const float    src[3] = {RowChannel(color_pixels, kSize, row, 0),
                                         RowChannel(color_pixels, kSize, row, 1),
                                         RowChannel(color_pixels, kSize, row, 2)};
                const float    expect[3] = {TonemapGamma(src[0] / 255.0f) * 255.0f,
                                            TonemapGamma(src[1] / 255.0f) * 255.0f,
                                            TonemapGamma(src[2] / 255.0f) * 255.0f};
                const float    got[3] = {RowChannel(graded, kSize, row, 0),
                                         RowChannel(graded, kSize, row, 1),
                                         RowChannel(graded, kSize, row, 2)};
                std::printf("       lut(identity) src=%6.1f/%6.1f/%6.1f  expect=%6.1f/%6.1f/%6.1f  got=%6.1f/%6.1f/%6.1f\n",
                            src[0], src[1], src[2], expect[0], expect[1], expect[2], got[0], got[1], got[2]);

                Check(std::fabs(got[0] - expect[0]) < 5.0f,
                      "with an identity LUT the red channel comes back unchanged (r is the within-slice axis)");
                Check(std::fabs(got[1] - expect[1]) < 5.0f,
                      "with an identity LUT the green channel comes back unchanged (g is the v axis)");
                Check(std::fabs(got[2] - expect[2]) < 5.0f,
                      "with an identity LUT the blue channel comes back unchanged (b is the slice axis)");
                // 三条通道必须互相不同 —— 否则上面三条是"碰巧都相等"，
                // 轴向搞反也照样能过。
                Check(std::fabs(expect[0] - expect[2]) > 30.0f,
                      "the coloured scene separates r and b enough for a swapped axis to be visible");
                // 没绑上 LUT 时 Metal 采到的是空 ⇒ 输出 0。
                Check(got[0] > 20.0f && got[1] > 20.0f && got[2] > 20.0f,
                      "the RP2 output is not black (binding 1 really is bound -- an unbound LUT samples as zero)");

                // ---- 第二遍：常量 LUT ----
                //
                // 恒等那遍有可能"绕过 LUT 也长得一样"（恒等 = 什么都不做），
                // 所以再来一张整张都是 (0.25, 0.5, 0.75) 的 LUT：只要 shader
                // 真的读了 binding 1，输出就必然是这三个值的 gamma，和第一遍
                // 的结果差得远。
                Offscreen lut_constant = MakeOffscreen(rhi, 256, 16, 0);
                RHIPipeline* constant_lut_pipeline =
                    lut_constant.framebuffer != nullptr
                        ? MakeGradientPipeline(rhi, lut_constant.render_pass, kConstantLutFragMsl)
                        : nullptr;
                if (constant_lut_pipeline != nullptr)
                {
                    RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
                    RHIClearValue     clear {};
                    clear.color.float32[3] = 1.0f;
                    RHIRenderPassBeginInfo begin {};
                    begin.sType                  = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                    begin.renderPass             = lut_constant.render_pass;
                    begin.framebuffer            = lut_constant.framebuffer;
                    begin.renderArea.extent.width  = 256;
                    begin.renderArea.extent.height = 16;
                    begin.clearValueCount        = 1;
                    begin.pClearValues           = &clear;
                    rhi->CmdBeginRenderPassPFN(command_buffer, &begin, RHI_SUBPASS_CONTENTS_INLINE);
                    SetViewport(rhi, command_buffer, 256, 16);
                    rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            constant_lut_pipeline);
                    rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
                    rhi->CmdEndRenderPassPFN(command_buffer);
                    rhi->EndSingleTimeCommands(command_buffer);
                }

                if (lut_constant.view != nullptr)
                {
                    RunRp2(rhi, harness, screen, color_scene.view, lut_constant.view);
                    const std::vector<uint8_t> flat = ReadBackPixels(rhi, screen.image, kSize, kSize);

                    // UNORM8 里存的是 round(0.25*255)=64 / 128 / 191，再走 1/2.2。
                    const float unorm[3]  = {64.0f / 255.0f, 128.0f / 255.0f, 191.0f / 255.0f};
                    const float expect2[3] = {std::pow(unorm[0], 1.0f / 2.2f) * 255.0f,
                                              std::pow(unorm[1], 1.0f / 2.2f) * 255.0f,
                                              std::pow(unorm[2], 1.0f / 2.2f) * 255.0f};
                    const float got2[3] = {RowChannel(flat, kSize, row, 0),
                                           RowChannel(flat, kSize, row, 1),
                                           RowChannel(flat, kSize, row, 2)};
                    std::printf("       lut(constant) expect=%6.1f/%6.1f/%6.1f  got=%6.1f/%6.1f/%6.1f\n",
                                expect2[0], expect2[1], expect2[2], got2[0], got2[1], got2[2]);

                    Check(std::fabs(got2[0] - expect2[0]) < 6.0f,
                          "a constant LUT overrides the red channel (the shader really reads binding 1)");
                    Check(std::fabs(got2[1] - expect2[1]) < 6.0f,
                          "a constant LUT overrides the green channel");
                    Check(std::fabs(got2[2] - expect2[2]) < 6.0f,
                          "a constant LUT overrides the blue channel");
                    // 而且必须明显不同于"恒等 LUT"那一遍 —— 两遍一样说明 LUT 被绕过了。
                    Check(std::fabs(got2[0] - got[0]) > 20.0f || std::fabs(got2[2] - got[2]) > 20.0f,
                          "the constant LUT and the identity LUT give visibly different results (LUT is not bypassed)");
                }
                DestroyOffscreen(rhi, lut_constant);
            }

            DestroyOffscreen(rhi, lut_identity);
            DestroyOffscreen(rhi, color_scene);
        }

        // ===================================================================
        // IBL cubemap 上传：真跑一遍 MetalRHI::CreateCubeMap。
        //
        // 这是整份测试里唯一能证伪"每 texel 字节数"的地方 —— 编译期看不见
        // replaceRegion 的 row pitch，日志也不会报，只有真采一次才看得出数据
        // 是不是错位/越界。格式特意用 R32G32B32A32_SFLOAT（真 IBL 的格式，
        // 16 字节/texel），而不是兜底图的 R8G8B8A8。
        // ===================================================================
        {
            constexpr uint32_t kCubeSize = 4;
            // 面内不是常数：r 沿 x 爬、g 沿 y 爬。pitch 错了（按 4 字节/texel
            // 读一份 16 字节/texel 的数据）中心采样值立刻不一样。
            std::array<std::vector<float>, 6> faces {};
            for (uint32_t face = 0; face < 6; ++face)
            {
                faces[face].resize(static_cast<size_t>(kCubeSize) * kCubeSize * 4u);
                for (uint32_t y = 0; y < kCubeSize; ++y)
                {
                    for (uint32_t x = 0; x < kCubeSize; ++x)
                    {
                        const size_t i = (static_cast<size_t>(y) * kCubeSize + x) * 4u;
                        faces[face][i + 0] = 0.30f + 0.10f * static_cast<float>(x);
                        faces[face][i + 1] = 0.30f + 0.10f * static_cast<float>(y);
                        faces[face][i + 2] = 0.60f;
                        faces[face][i + 3] = 1.0f;
                    }
                }
            }
            std::array<void*, 6> face_ptrs = {faces[0].data(), faces[1].data(), faces[2].data(),
                                              faces[3].data(), faces[4].data(), faces[5].data()};

            RHIImage*      cube_image = nullptr;
            RHIImageView*  cube_view  = nullptr;
            rhi->CreateCubeMap(cube_image,
                               cube_view,
                               nullptr,
                               kCubeSize,
                               kCubeSize,
                               face_ptrs,
                               RHI_FORMAT_R32G32B32A32_SFLOAT,
                               1);
            Check(cube_image != nullptr && cube_view != nullptr,
                  "MetalRHI::CreateCubeMap builds an RGBA32F cubemap (the real IBL format)");

            if (cube_view != nullptr)
            {
                Offscreen cube_target = MakeOffscreen(rhi, kSize, kSize, 0);
                Rp2Harness cube_harness {};
                const bool cube_ok = MakeRp2Harness(rhi,
                                                    cube_target.render_pass,
                                                    kGradientVertMsl,
                                                    kIblCubeFragMsl,
                                                    cube_harness);
                Check(cube_ok, "the cubemap sampling pipeline builds (MSL texturecube + sampler)");

                if (cube_ok && cube_target.framebuffer != nullptr)
                {
                    // 复用 RunRp2：它绑 binding 0 和 1，而这段 shader 只用 0，
                    // binding 1 给一张合法但无关的图即可。
                    RunRp2(rhi, cube_harness, cube_target, cube_view, scene.view);
                    const std::vector<uint8_t> cube_pixels = ReadBackPixels(rhi, cube_target.image, kSize, kSize);

                    // 方向 (1,0,0) 打在 +X 面（Metal 的第 0 个 slice）正中心 ⇒
                    // 4x4 面内双线性落在 x=y=1.5：
                    //   r = 0.30 + 0.10*1.5 = 0.45 -> 115
                    //   g = 同上              -> 115
                    //   b = 0.60              -> 153
                    const float got_c[3] = {RowChannel(cube_pixels, kSize, kSize / 2, 0),
                                            RowChannel(cube_pixels, kSize, kSize / 2, 1),
                                            RowChannel(cube_pixels, kSize, kSize / 2, 2)};
                    std::printf("       ibl cube expect=115.0/115.0/153.0  got=%6.1f/%6.1f/%6.1f\n",
                                got_c[0], got_c[1], got_c[2]);

                    Check(std::fabs(got_c[0] - 115.0f) < 4.0f && std::fabs(got_c[1] - 115.0f) < 4.0f &&
                              std::fabs(got_c[2] - 153.0f) < 4.0f,
                          "the RGBA32F cubemap round-trips through CreateCubeMap with the right row pitch");
                    // 全 0 意味着"上传根本没发生"（格式未知 / 设备没起来）。
                    Check(got_c[0] > 8.0f && got_c[1] > 8.0f && got_c[2] > 8.0f,
                          "the sampled cubemap is not black (the upload really happened)");
                }
                DestroyOffscreen(rhi, cube_target);
            }

            if (cube_view != nullptr)
            {
                rhi->DestroyImageView(cube_view);
            }
            if (cube_image != nullptr)
            {
                rhi->DestroyImage(cube_image);
            }
        }

        DestroyOffscreen(rhi, scene);
        DestroyOffscreen(rhi, screen);
        metal_rhi.Shutdown();
    }
#endif  // __APPLE__

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
