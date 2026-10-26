// =============================================================================
// AppleSsaoSmokeTest —— SSAO（AppleSsaoPass + 延迟光照的 AO 槽）。
// -----------------------------------------------------------------------------
// 为什么要真跑 GPU：AppleSsaoPass 的 MSL 是字符串，只在运行时 newLibraryWithSource:
// 才见编译器（拼错一个字符：构建全绿、编辑器里 SSAO 静默降级成 1x1 白图）；而它最
// 容易错的两件事 —— 样本点投影回屏幕时 NDC→像素的 y 方向、以及样本跨进另一个视口
// —— 在画面上只是"遮蔽位置有点怪"，不报任何错。这两条只有拿一个已知几何的合成
// 场景真跑一遍、读回像素才分辨得出来。
//
// 场景全在 NDC 里造：proj_view = 单位阵 ⇒ 世界坐标 = (ndc.x, ndc.y, depth)，相机在
// (0,0,-1) 朝 +z 看。深度写进一张 R32F 颜色图（AO 的 MSL 只 read().r，和 D32 深度图
// 走同一条代码），法线写进 RGBA8（n*0.5+0.5，与 GBufferA 同编码）。
//
//   台阶：左半 z=0.6（地面），右半 z=0.4（凸台），ndc.y>0.75 是天空（depth=1）。
//         ⇒ 地面贴着凸台的一条必须被遮蔽；离边远的地面、凸台顶、天空必须是 1。
//   斜坡：z = 0.5 + 0.15x + 0.15y，法线与之严格一致。正确的实现在平面上**一点遮蔽
//         都没有**；NDC→像素映射的 x 或 y 翻反 ⇒ 读回来的深度不在这个平面上 ⇒ 平面
//         上冒出假遮蔽。再拆成左右两个半屏视口（每个视口自己的 NDC），接缝两侧深度
//         差 0.3 ⇒ 样本一旦跨进另一个视口也会冒出假遮蔽。
//
// 非 Apple 平台只跑纯 CPU / 文本部分。
// =============================================================================

#if defined(__APPLE__)
    #include "CommonPCH/pch.h"
#endif

#include "Runtime/Function/Render/AppleSsaoPass.h"
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

#if defined(__APPLE__)
    #include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
    #include <Foundation/Foundation.h>
#endif

namespace
{
    int g_Checks   = 0;
    int g_Failures = 0;

    void Check(bool condition, const std::string& name)
    {
        ++g_Checks;
        if (condition)
        {
            std::printf("[ ok ] %s\n", name.c_str());
            return;
        }
        ++g_Failures;
        std::printf("[FAIL] %s\n", name.c_str());
    }

    std::string ReadFileText(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // 逐行找，跳过整行注释（// 开头）。注释掉一行代码之后全文 find 照样命中 —— 契约就瞎了。
    size_t CodeLineFindFrom(const std::string& src, const std::string& needle, size_t from)
    {
        size_t at = from;
        while (at < src.size())
        {
            size_t eol = src.find('\n', at);
            if (eol == std::string::npos)
            {
                eol = src.size();
            }
            const std::string line  = src.substr(at, eol - at);
            const size_t      first = line.find_first_not_of(" \t");
            const bool commented    = first != std::string::npos && line.compare(first, 2, "//") == 0;
            const size_t hit        = line.find(needle);
            if (!commented && hit != std::string::npos)
            {
                return at + hit;
            }
            at = eol + 1;
        }
        return std::string::npos;
    }

    size_t CodeLineFind(const std::string& src, const std::string& needle) { return CodeLineFindFrom(src, needle, 0); }

    int CountCodeLines(const std::string& src, const std::string& needle)
    {
        int    count = 0;
        size_t at    = 0;
        while ((at = CodeLineFindFrom(src, needle, at)) != std::string::npos)
        {
            ++count;
            at += needle.size();
        }
        return count;
    }

    // 从 `from` 那一行到 `until` 那一行之间的代码（判"某个函数体 / 分支内"用）。
    std::string BlockBetween(const std::string& src, const std::string& from, const std::string& until)
    {
        const size_t begin = CodeLineFind(src, from);
        if (begin == std::string::npos)
        {
            return {};
        }
        const size_t end = CodeLineFindFrom(src, until, begin + from.size());
        return src.substr(begin, (end == std::string::npos ? src.size() : end) - begin);
    }

    int CountOccurrences(const std::string& text, const std::string& token)
    {
        int    count = 0;
        size_t at    = 0;
        while ((at = text.find(token, at)) != std::string::npos)
        {
            ++count;
            at += token.size();
        }
        return count;
    }

    // =========================================================================
    // §1 纯 CPU：参数块打包
    // =========================================================================
    void CheckGpuParams()
    {
        Check(sizeof(AppleSsaoGpuParams) == 192, "AppleSsaoGpuParams is 192 bytes (mirrors MSL SsaoParams)");
        Check(sizeof(AppleSsaoGpuParams) <= AppleSsaoPass::kParamsStride, "one params block fits in a 256-byte slot");

        // 一个非对称的矩阵：转置 / 抄错一个都会让下面的乘积不是单位阵。
        AppleSsaoViewport vp {};
        vp.proj_view = Matrix4x4(1.2f, 0.1f, 0.0f, 0.3f,
                                 0.0f, 0.9f, 0.2f, -0.1f,
                                 0.05f, 0.0f, 1.1f, 0.4f,
                                 0.0f, 0.02f, 0.3f, 1.0f);
        vp.camera_position = Vector3(1.0f, 2.0f, 3.0f);
        vp.viewport        = {10.0f, 20.0f, 300.0f, 200.0f, 0.0f, 1.0f};

        AppleSsaoSettings settings {};
        settings.radius                   = 0.7f;
        settings.intensity                = 1.5f;
        settings.direct_lighting_strength = 0.3f;
        settings.sample_count             = 9;

        const AppleSsaoGpuParams p = AppleSsaoPass::MakeGpuParams(vp, settings);

        Check(std::memcmp(p.proj_view, &vp.proj_view, sizeof(p.proj_view)) == 0,
              "params: proj_view is a byte copy of the Matrix4x4 (same layout as MainCameraPerFrame)");

        // GPU 侧按列主序读同一段字节：G = Sᵀ、Ginv = inv(S)ᵀ。验 G * Ginv == I。
        auto at = [](const float* m, int row, int col) { return m[col * 4 + row]; };  // 列主序
        float max_err = 0.0f;
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    sum += at(p.proj_view, r, k) * at(p.inv_proj_view, k, c);
                }
                max_err = std::max(max_err, std::fabs(sum - (r == c ? 1.0f : 0.0f)));
            }
        }
        Check(max_err < 1e-4f, "params: inv_proj_view is the inverse of proj_view as the GPU reads them");

        Check(p.camera_position[0] == 1.0f && p.camera_position[1] == 2.0f && p.camera_position[2] == 3.0f,
              "params: camera position packed");
        Check(p.viewport[0] == 10.0f && p.viewport[1] == 20.0f && p.viewport[2] == 300.0f && p.viewport[3] == 200.0f,
              "params: viewport packed as x, y, width, height (pixels)");
        Check(p.settings[0] == 0.7f && p.settings[1] == 1.5f && p.settings[2] == 0.3f && p.settings[3] == 9.0f,
              "params: settings packed as radius, intensity, direct strength, sample count");
        Check(p.blur[0] > 0.0f && p.blur[0] < 1.0f, "params: blur plane tolerance is a fraction of the radius");
    }

    // =========================================================================
    // §2 MSL 源码契约
    // =========================================================================
    void CheckMslSource()
    {
        const std::string vert = AppleSsaoPass::BuildVertexSource();
        const std::string ao   = AppleSsaoPass::BuildFragmentSource(AppleSsaoPass::Stage::Ao);
        const std::string bh   = AppleSsaoPass::BuildFragmentSource(AppleSsaoPass::Stage::BlurH);
        const std::string bv   = AppleSsaoPass::BuildFragmentSource(AppleSsaoPass::Stage::BlurV);

        Check(!vert.empty() && !ao.empty() && !bh.empty() && !bv.empty(), "MSL: all four sources build");

        // 每个 library 只能有一个入口（CreateShaderModuleFromSource 找 "main" 落空就取
        // functionNames.firstObject）；入口名绝不能叫 main（Metal 直接拒绝）。
        Check(CountOccurrences(vert, "\nvertex ") == 1 && CountOccurrences(vert, "\nfragment ") == 0,
              "MSL: vertex library has exactly one entry point");
        for (const std::string* s : {&ao, &bh, &bv})
        {
            Check(CountOccurrences(*s, "\nfragment ") == 1 && CountOccurrences(*s, "\nvertex ") == 0,
                  "MSL: fragment library has exactly one entry point");
            Check(s->find(" main(") == std::string::npos, "MSL: no entry point is called main");
            // 槽位：参数 [[buffer(8)]] = set0/binding0 + 8；depth/normal = texture(1)/(2)。
            Check(s->find("[[buffer(" + std::to_string(AppleSsaoPass::kParamsBinding + 8) + ")]]") != std::string::npos,
                  "MSL: params at [[buffer(binding 0 + 8)]]");
            Check(s->find("[[texture(" + std::to_string(AppleSsaoPass::kDepthBinding) + ")]]") != std::string::npos &&
                      s->find("[[texture(" + std::to_string(AppleSsaoPass::kNormalBinding) + ")]]") != std::string::npos,
                  "MSL: depth / normal at [[texture(1)]] / [[texture(2)]]");
            // 导数必须在第一个分支之前取（分支里 dfdx/dfdy 未定义）。
            const size_t entry = s->find("\nfragment ");
            const size_t deriv = s->find("dfdx(in.ndc.x)", entry);
            const size_t first_if = s->find("if (", entry);
            Check(deriv != std::string::npos && first_if != std::string::npos && deriv < first_if,
                  "MSL: NDC derivatives are taken before the first branch");
        }
        Check(bh.find("[[texture(" + std::to_string(AppleSsaoPass::kAoInputBinding) + ")]]") != std::string::npos &&
                  bv.find("[[texture(" + std::to_string(AppleSsaoPass::kAoInputBinding) + ")]]") != std::string::npos,
              "MSL: blur passes read the previous AO at [[texture(3)]]");

        // 模糊方向 / 最终趟。
        Check(bh.find("#define SSAO_BLUR_DIRECTION float2(1.0, 0.0)") != std::string::npos &&
                  bh.find("#define SSAO_BLUR_FINAL 0") != std::string::npos,
              "MSL: blurH is horizontal and not final");
        Check(bv.find("#define SSAO_BLUR_DIRECTION float2(0.0, 1.0)") != std::string::npos &&
                  bv.find("#define SSAO_BLUR_FINAL 1") != std::string::npos,
              "MSL: blurV is vertical and final");

        // SsaoParams 字段顺序必须和 C++ 的 AppleSsaoGpuParams 一致。
        {
            const char* fields[] = {"proj_view;", "inv_proj_view;", "camera_position;", "viewport;", "settings;", "blur;"};
            size_t      last     = ao.find("struct SsaoParams");
            bool        ordered  = last != std::string::npos;
            for (const char* f : fields)
            {
                const size_t at = ordered ? ao.find(f, last) : std::string::npos;
                ordered         = ordered && at != std::string::npos;
                last            = at;
            }
            Check(ordered, "MSL: SsaoParams field order matches AppleSsaoGpuParams");
        }

        // 生效那一步：样本点 → 像素用的是导数（不是写死的 y 翻转）。
        Check(ao.find("in.position.xy + (ndc_s - in.ndc) / ndc_per_px") != std::string::npos,
              "MSL: sample pixel = fragment pixel + NDC delta / (dNDC/dpixel)");
        Check(ao.find("SsaoReconstruct(p, ndc_center, depth_s)") != std::string::npos,
              "MSL: occluder is reconstructed at the NDC of the texel actually read (no slope self-occlusion)");
        Check(ao.find("if (!SsaoInsideViewport(p, px))") != std::string::npos,
              "MSL: samples outside this viewport are rejected");
        // URP 的 Alchemy 常数。
        Check(ao.find("kBeta    = 0.002") != std::string::npos && ao.find("pow(saturate(") != std::string::npos &&
                  ao.find(", 0.6)") != std::string::npos,
              "MSL: Alchemy constants (kBeta 0.002, contrast 0.6) as in URP");
    }

    // =========================================================================
    // §3 接线契约（shader 变体 / RP1 / 模块 / 心跳 / CVar）
    // =========================================================================
    void CheckWiringContracts()
    {
#if defined(ZENGINE_ENGINE_ROOT_DIR)
        const std::string root   = ZENGINE_ENGINE_ROOT_DIR;
        const std::string frag   = ReadFileText(root + "/shader/glsl/deferred_lighting.frag");
        const std::string fragao = ReadFileText(root + "/shader/glsl/deferred_lighting_ssao.frag");
        const std::string main_i = ReadFileText(root + "/shader/include/deferred_lighting_main.inl");
        const std::string light  = ReadFileText(root + "/shader/include/mesh_lighting.inl");
        const std::string rp1    = ReadFileText(root + "/Source/Runtime/Function/Render/Passes/MainCameraRp1Pass.cpp");
        const std::string mod    = ReadFileText(root + "/Source/Runtime/Function/Render/Pipeline/DesktopRenderPipelineModule.cpp");
        const std::string sys    = ReadFileText(root + "/Source/Runtime/Function/Render/RenderSystem.cpp");
        const std::string cmds   = ReadFileText(root + "/Source/Runtime/Function/Console/RuntimeConsoleCommands.cpp");

        Check(!frag.empty() && !fragao.empty() && !main_i.empty() && !light.empty() && !rp1.empty() && !mod.empty() &&
                  !sys.empty() && !cmds.empty(),
              "selftest: all wiring sources are readable");

        // ---- shader 变体 ----
        Check(CodeLineFind(fragao, "#define Z_DEFERRED_SCREEN_SPACE_AO 1") != std::string::npos,
              "shader: deferred_lighting_ssao.frag defines Z_DEFERRED_SCREEN_SPACE_AO");
        Check(CodeLineFind(frag, "Z_DEFERRED_SCREEN_SPACE_AO 1") == std::string::npos,
              "shader: plain deferred_lighting.frag does NOT define it (non-Metal SPIR-V unchanged)");
        Check(CodeLineFind(frag, "#include \"deferred_lighting_main.inl\"") != std::string::npos &&
                  CodeLineFind(fragao, "#include \"deferred_lighting_main.inl\"") != std::string::npos,
              "shader: both variants share deferred_lighting_main.inl");
        Check(CodeLineFind(main_i, "layout(set = 1, binding = 4) uniform highp sampler2D in_screen_space_ao;") !=
                  std::string::npos,
              "shader: AO sampled from set 1 / binding 4");
        {
            const std::string block = BlockBetween(light, "#ifdef Z_DEFERRED_SCREEN_SPACE_AO", "#endif");
            Check(!block.empty(), "shader: mesh_lighting.inl has the SSAO block");
            Check(CodeLineFind(block, "La   = La * indirect_ao;") != std::string::npos,
                  "shader: ambient term x indirect AO");
            Check(CodeLineFind(block, "Libl = kD * diffuse * indirect_ao + specular * specular_ao;") != std::string::npos,
                  "shader: IBL diffuse x AO, IBL specular x specular occlusion");
            Check(CodeLineFind(block, "Lo   = Lo * direct_ao;") != std::string::npos,
                  "shader: direct light x direct AO multiplier");
            const size_t block_at  = light.find("#ifdef Z_DEFERRED_SCREEN_SPACE_AO");
            const size_t result_at = CodeLineFind(light, "result_color = Lo + La + Libl;");
            Check(block_at != std::string::npos && result_at != std::string::npos && block_at < result_at,
                  "shader: SSAO block is applied before the final sum");
        }

        // ---- RP1 ----
        {
            const std::string metal_frag = BlockBetween(rp1, "if (std::strcmp(dx12_hlsl_path, \"deferred_lighting.frag.hlsl\") == 0)",
                                                        "return nullptr;");
            Check(CodeLineFind(metal_frag, "return rhi->CreateShaderModule(DEFERRED_LIGHTING_SSAO_FRAG);") != std::string::npos,
                  "RP1: Metal deferred lighting uses the SSAO shader variant");
        }
        {
            // 生效那一步：布局里必须真有 binding 4（MetalRHI 只更新布局里存在的 binding）。
            const std::string layout = BlockBetween(rp1, "uint32_t gbuffer_lighting_binding_count = 4;",
                                                    "gbuffer_lighting_global_layout_create_info.bindingCount");
            Check(CodeLineFind(layout, "if (m_Rhi->getGraphicsAPI() == GraphicsAPI::Metal)") != std::string::npos,
                  "RP1: binding 4 is Metal-only");
            Check(CodeLineFind(layout, "gbuffer_lighting_global_layout_bindings[4].binding = 4;") != std::string::npos,
                  "RP1: deferred layout declares binding 4");
            Check(CodeLineFind(layout, "gbuffer_lighting_binding_count = 5;") != std::string::npos,
                  "RP1: deferred layout has 5 bindings on Metal");
            Check(CodeLineFind(rp1, "gbuffer_lighting_global_layout_create_info.bindingCount = gbuffer_lighting_binding_count;") !=
                      std::string::npos,
                  "RP1: layout create info uses the per-backend binding count");
        }
        {
            const std::string refresh = BlockBetween(rp1, "void MainCameraRp1Pass::RefreshDeferredLightingInputAttachments()",
                                                     "RHIImageView* MainCameraRp1Pass::ResolveAmbientOcclusionView()");
            Check(CodeLineFind(refresh, "ambient_occlusion_write.dstBinding = 4;") != std::string::npos,
                  "RP1: deferred refresh writes binding 4");
            Check(CodeLineFind(refresh, "ambient_occlusion_info.imageView = ResolveAmbientOcclusionView();") != std::string::npos,
                  "RP1: binding 4 gets the resolved AO view");
            const std::string resolve = BlockBetween(rp1, "RHIImageView* MainCameraRp1Pass::ResolveAmbientOcclusionView()",
                                                     "void MainCameraRp1Pass::DrawAmbientOcclusionPass()");
            Check(CodeLineFind(resolve, "m_AmbientOcclusionPass->GetOutputView()") != std::string::npos,
                  "RP1: real AO view comes from AppleSsaoPass::GetOutputView()");
            Check(CodeLineFind(resolve, "const uint8_t white[] = {255, 255, 255, 255};") != std::string::npos,
                  "RP1: fallback is a 1x1 WHITE texture (AO = 1 = unoccluded)");
            Check(CodeLineFind(resolve, "m_AmbientOcclusionBound = true;") != std::string::npos,
                  "RP1: bound probe set when the real view is bound");
        }
        {
            const std::string draw = BlockBetween(rp1, "void MainCameraRp1Pass::DrawAmbientOcclusionPass()",
                                                  "void MainCameraRp1Pass::RefreshMeshGlobalIblDescriptors()");
            Check(CodeLineFind(draw, "ResolveViewportScissor(viewport_type, out.viewport, out.scissor);") != std::string::npos,
                  "RP1: SSAO viewports come from the same resolver as deferred lighting");
            Check(CodeLineFind(draw, "SetPerViewportData(viewport_type);") != std::string::npos,
                  "RP1: SSAO uses the per-viewport camera data");
            Check(CodeLineFind(draw, "out.proj_view       = m_MainCameraPerFrame.proj_view_matrix;") != std::string::npos,
                  "RP1: SSAO proj_view is the deferred lighting proj_view");
            Check(CodeLineFind(draw, "enabled ? viewport_count : 0u,") != std::string::npos,
                  "RP1: r.SSAO.Enable=0 still calls Record with 0 viewports (clears the output)");
            Check(CodeLineFind(draw, "m_FbResources->getAttachmentView(_main_camera_pass_gbuffer_a)") != std::string::npos,
                  "RP1: normals come from GBufferA");
            Check(CodeLineFind(draw, "m_Rhi->GetDepthImageInfo().depth_image_view") != std::string::npos,
                  "RP1: depth comes from the scene depth image");
        }
        {
            const std::string setvp = BlockBetween(rp1, "void MainCameraRp1Pass::SetViewportScissor(ViewportType viewport_type)",
                                                   "void MainCameraRp1Pass::ResolveViewportScissor(");
            Check(CodeLineFind(setvp, "ResolveViewportScissor(viewport_type, viewport, scissor);") != std::string::npos,
                  "RP1: SetViewportScissor delegates to the shared resolver");
        }

        // ---- 模块：顺序 + 借用 ----
        {
            const std::string lambda = BlockBetween(mod, "rp1->DrawGBufferPass(show_skybox);", "rp1->DrawForwardLightingPass(show_skybox);");
            const size_t ao  = CodeLineFind(lambda, "rp1->DrawAmbientOcclusionPass();");
            const size_t def = CodeLineFind(lambda, "rp1->DrawDeferredLightingPass(show_skybox);");
            Check(ao != std::string::npos && def != std::string::npos && ao < def,
                  "module: SSAO is recorded after GBuffer and BEFORE deferred lighting");
            Check(CodeLineFind(mod, "rp1->SetAmbientOcclusionPass(m_AppleSsao.get());") != std::string::npos,
                  "module: RP1 borrows the module-owned AppleSsaoPass");
            Check(CodeLineFind(mod, "if (!m_AppleSsao->Initialize(m_Pipeline.m_Rhi))") != std::string::npos,
                  "module: AppleSsaoPass init failure is handled (degrade, not crash)");
            Check(CodeLineFind(mod, "rp1->SetAmbientOcclusionPass(nullptr);") != std::string::npos,
                  "module: Shutdown releases the borrow");
            Check(CodeLineFind(mod, "stats.bound = rp1->IsAmbientOcclusionBound() ? 1u : 0u;") != std::string::npos,
                  "module: stats report the 'bound' (effective) bit from RP1");
        }

        // ---- 心跳 + CVar ----
        Check(CodeLineFind(sys, "\"ssao=(en={} ready={} ran={} vp={} bound={}) \"") != std::string::npos,
              "RenderSystem: heartbeat carries ssao=(en ready ran vp bound)");
        Check(CodeLineFind(sys, "ssao_stats.bound,") != std::string::npos, "RenderSystem: heartbeat passes the bound bit");
        Check(CodeLineFind(cmds, "SsaoSettings::RegisterConsoleVariables(console);") != std::string::npos,
              "console: r.SSAO.* registered");

        // ---- 扫描器双向自检 ----
        Check(CountCodeLines(rp1, "void MainCameraRp1Pass::DrawAmbientOcclusionPass()") == 1,
              "selftest: scanner finds exactly one DrawAmbientOcclusionPass definition");
        Check(CountCodeLines(rp1, "ZZZ-NO-SUCH-TOKEN-IN-RP1") == 0, "selftest: scanner reports 0 for an absent token");
#else
        Check(true, "wiring contracts skipped (no ZENGINE_ENGINE_ROOT_DIR)");
#endif
    }

    // =========================================================================
    // §5 Metal 对象所有权（MRR）+ 每帧 autorelease pool
    // =========================================================================
    //
    // 扫描器：找"把一个局部变量 retain 之后紧跟着 release 它"的两行。这个写法只有在
    // 该局部来自 alloc / new* / copy（+1，调用方本来就持有）时才对 —— 那是"把所有权
    // 转给成员"。来自其它方法（`commandBuffer`、`computeCommandEncoder`、
    // `renderCommandEncoderWithDescriptor:` ……）的是 autorelease 对象，不归调用方
    // 所有，retain 后再 release 等于把 pool 那份也还了，收尾时再 release 就是多一次。
    int CountAutoreleasedRetainRelease(const std::string& src, std::string* first_hit)
    {
        std::vector<std::string> lines;
        {
            std::istringstream in(src);
            std::string        line;
            while (std::getline(in, line))
            {
                lines.push_back(line);
            }
        }
        auto trim = [](const std::string& l) {
            const size_t a = l.find_first_not_of(" \t");
            return a == std::string::npos ? std::string() : l.substr(a);
        };
        int hits = 0;
        for (size_t i = 0; i + 1 < lines.size(); ++i)
        {
            const std::string cur = trim(lines[i]);
            if (cur.rfind("//", 0) == 0)
            {
                continue;
            }
            // `= [name retain];`
            const size_t open = cur.find("= [");
            const size_t tail = cur.find(" retain]");
            if (open == std::string::npos || tail == std::string::npos || tail < open)
            {
                continue;
            }
            const std::string name = cur.substr(open + 3, tail - open - 3);
            if (name.empty() || name.find_first_of(" []") != std::string::npos)
            {
                continue;  // [[x foo] retain] 这类没有中间局部的写法不在此列
            }
            if (trim(lines[i + 1]) != "[" + name + " release];")
            {
                continue;
            }
            // 往回找这个局部的来源：`name = [receiver selector...`
            std::string selector;
            for (size_t back = 1; back <= 20 && back <= i; ++back)
            {
                const std::string prev = trim(lines[i - back]);
                const size_t      at   = prev.find(name + " = [");
                if (at == std::string::npos)
                {
                    continue;
                }
                std::string rhs = prev.substr(at + name.size() + 4);
                // 跳过 receiver（可能是 [[X alloc] init] 这种嵌套，第一个词就是 X）
                const size_t sp = rhs.find(' ');
                if (sp != std::string::npos)
                {
                    rhs = rhs.substr(sp + 1);
                }
                selector = rhs;
                if (prev.find("alloc]") != std::string::npos)
                {
                    selector = "alloc";
                }
                break;
            }
            const bool owned = selector.rfind("new", 0) == 0 || selector.rfind("alloc", 0) == 0 ||
                               selector.rfind("copy", 0) == 0 || selector.rfind("mutableCopy", 0) == 0;
            if (!owned)
            {
                ++hits;
                if (first_hit != nullptr && first_hit->empty())
                {
                    *first_hit = cur + " / origin: " + (selector.empty() ? "<not found>" : selector.substr(0, 60));
                }
            }
        }
        return hits;
    }

    void CheckMetalOwnershipContracts()
    {
        // ---- 扫描器双向自检（扫描器自己写错会恒绿）----
        const std::string bad =
            "    id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:d];\n"
            "    x->renderEncoder = [encoder retain];\n"
            "    [encoder release];\n";
        const std::string good =
            "    id<MTLRenderPipelineState> state = [m_Device newRenderPipelineStateWithDescriptor:d error:&e];\n"
            "    p->renderState = [state retain];\n"
            "    [state release];\n"
            "    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];\n"
            "    p->desc = [dd retain];\n"
            "    [dd release];\n";
        Check(CountAutoreleasedRetainRelease(bad, nullptr) == 1,
              "selftest: ownership scanner flags retain+release of an autoreleased encoder");
        Check(CountAutoreleasedRetainRelease(good, nullptr) == 0,
              "selftest: ownership scanner accepts retain+release of new*/alloc'd (+1) objects");

#if defined(ZENGINE_ENGINE_ROOT_DIR)
        const std::string root  = ZENGINE_ENGINE_ROOT_DIR;
        const std::string metal = ReadFileText(root + "/Source/Runtime/Function/Render/Interface/Metal/MetalRHI.mm");
        const std::string app   = ReadFileText(root + "/Source/Runtime/Application/Application.cpp");
        const std::string pool  = ReadFileText(root + "/Source/Runtime/Core/Base/ScopedAutoreleasePool.mm");
        Check(!metal.empty() && !app.empty() && !pool.empty(), "selftest: ownership sources are readable");

        std::string hit;
        const int   hits = CountAutoreleasedRetainRelease(metal, &hit);
        if (hits != 0)
        {
            std::printf("       first offending site: %s\n", hit.c_str());
        }
        Check(hits == 0, "MetalRHI.mm: no autoreleased Metal object is retained and then immediately released");
        // 生效那一步：6 个取 autorelease 对象的地方都必须是"retain 这一份归我们"。
        Check(CountCodeLines(metal, "= [[m_CommandQueue commandBuffer] retain];") == 3,
              "MetalRHI.mm: the 3 frame / single-time / RHI command buffers keep exactly one retain");
        Check(CountCodeLines(metal, "metal_buffer->computeEncoder = [[metal_buffer->commandBuffer computeCommandEncoder] retain];") == 1,
              "MetalRHI.mm: compute encoder is created in exactly one place (BeginComputeEncoder) and retained once");
        Check(CountCodeLines(metal, "metal_buffer->renderEncoder = [encoder retain];") == 1,
              "MetalRHI.mm: render encoder retained once");

        // 每帧 pool：TickOneFrame 的第一条语句。
        {
            const std::string body = BlockBetween(app, "bool Application::TickOneFrame(float simulation_delta_time",
                                                  "TickDeferredConsoleCommands();");
            Check(CodeLineFind(body, "ScopedAutoreleasePool frame_autorelease_pool;") != std::string::npos,
                  "Application: TickOneFrame opens a per-frame autorelease pool before any frame work");
        }
        Check(CodeLineFind(pool, "[(NSAutoreleasePool*)m_Pool drain];") != std::string::npos,
              "ScopedAutoreleasePool: destructor drains the pool");
#endif
    }

#if defined(__APPLE__)
    // =========================================================================
    // §4 GPU
    // =========================================================================
    constexpr uint32_t kSize = 64;

    struct Target
    {
        RHIImage*        image {nullptr};
        RHIDeviceMemory* memory {nullptr};
        RHIImageView*    view {nullptr};
        RHIRenderPass*   render_pass {nullptr};
        RHIFramebuffer*  framebuffer {nullptr};
    };

    Target MakeTarget(RHI* rhi, RHIFormat format)
    {
        Target t;
        rhi->CreateImage(kSize, kSize, format, RHI_IMAGE_TILING_OPTIMAL,
                         RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.image, t.memory, 0, 1, 1);
        rhi->CreateImageView(t.image, format, RHI_IMAGE_ASPECT_COLOR_BIT, RHI_IMAGE_VIEW_TYPE_2D, 1, 1, t.view);
        if (t.view == nullptr)
        {
            return t;
        }
        RHIAttachmentDescription attachment {};
        attachment.format        = format;
        attachment.samples       = RHI_SAMPLE_COUNT_1_BIT;
        attachment.loadOp        = RHI_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp       = RHI_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout   = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        RHIAttachmentReference ref {};
        ref.attachment = 0;
        ref.layout     = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        RHISubpassDescription subpass {};
        subpass.pipelineBindPoint    = RHI_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &ref;
        RHIRenderPassCreateInfo rp {};
        rp.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1;
        rp.pAttachments    = &attachment;
        rp.subpassCount    = 1;
        rp.pSubpasses      = &subpass;
        rhi->CreateRenderPass(&rp, t.render_pass);
        RHIFramebufferCreateInfo fb {};
        fb.sType           = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = t.render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &t.view;
        fb.width           = kSize;
        fb.height          = kSize;
        fb.layers          = 1;
        rhi->CreateFramebuffer(&fb, t.framebuffer);
        return t;
    }

    std::vector<uint8_t> ReadBack(RHI* rhi, RHIImage* image, uint32_t bytes_per_pixel)
    {
        std::vector<uint8_t> pixels;
        if (image == nullptr)
        {
            return pixels;
        }
        const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(kSize) * kSize * bytes_per_pixel;
        RHIBuffer*       buffer   = nullptr;
        RHIDeviceMemory* memory   = nullptr;
        rhi->CreateBuffer(bytes, RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer, memory);
        if (buffer == nullptr || memory == nullptr)
        {
            return pixels;
        }
        if (RHICommandBuffer* cmd = rhi->BeginSingleTimeCommands())
        {
            RHIBufferImageCopy region {};
            region.bufferRowLength                 = kSize * bytes_per_pixel;  // Metal：字节
            region.bufferImageHeight               = kSize;
            region.imageSubresource.aspectMask     = RHI_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount     = 1;
            region.imageExtent                     = {kSize, kSize, 1};
            rhi->CmdCopyImageToBuffer(cmd, image, RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            rhi->EndSingleTimeCommands(cmd);
        }
        void* mapped = nullptr;
        if (rhi->MapMemory(memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            pixels.assign(static_cast<const uint8_t*>(mapped), static_cast<const uint8_t*>(mapped) + bytes);
        }
        rhi->FreeMemory(memory);
        rhi->DestroyBuffer(buffer);
        return pixels;
    }

    // 场景 fragment：深度（写进 R32F 的 r）或法线（RGBA8）。顶点函数复用 AppleSsaoPass
    // 的真身（同一个 ndc 约定 ⇒ 场景和 AO 用的是同一套视口变换）。
    std::string SceneFragment(const char* body)
    {
        return std::string(R"MSL(
#include <metal_stdlib>
using namespace metal;
struct SsaoVertexOut
{
    float4 position [[position]];
    float2 ndc;
};
fragment float4 SceneFragmentMain(SsaoVertexOut in [[stage_in]])
{
)MSL") + body + "\n}\n";
    }

    const char* kStepDepth  = "    return float4(in.ndc.y > 0.75 ? 1.0 : (in.ndc.x > 0.0 ? 0.4 : 0.6), 0.0, 0.0, 1.0);";
    const char* kFlatNormal = "    return float4(0.5, 0.5, 0.0, 1.0);";  // N = (0,0,-1)，朝向相机
    const char* kRampDepth  = "    return float4(0.5 + 0.15 * in.ndc.x + 0.15 * in.ndc.y, 0.0, 0.0, 1.0);";
    const char* kRampNormal = "    return float4(normalize(float3(0.15, 0.15, -1.0)) * 0.5 + 0.5, 1.0);";

    RHIPipeline* MakeScenePipeline(RHI* rhi, RHIRenderPass* render_pass, const std::string& frag_msl)
    {
        RHIPipelineLayoutCreateInfo layout_info {};
        layout_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        RHIPipelineLayout* layout = nullptr;
        rhi->CreatePipelineLayout(&layout_info, layout);

        RHIShader* vert = rhi->CreateShaderModuleFromSource(AppleSsaoPass::BuildVertexSource(), ShaderStage::Vertex, "scene.vert");
        RHIShader* frag = rhi->CreateShaderModuleFromSource(frag_msl, ShaderStage::Fragment, "scene.frag");
        if (layout == nullptr || vert == nullptr || frag == nullptr)
        {
            return nullptr;
        }
        RHIPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = RHI_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1]        = stages[0];
        stages[1].stage  = RHI_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;

        RHIPipelineVertexInputStateCreateInfo vi {};
        vi.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        RHIPipelineInputAssemblyStateCreateInfo ia {};
        ia.sType    = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        RHIPipelineViewportStateCreateInfo vs {};
        vs.sType         = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vs.viewportCount = 1;
        vs.scissorCount  = 1;
        RHIPipelineRasterizationStateCreateInfo rs {};
        rs.sType       = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = RHI_POLYGON_MODE_FILL;
        rs.cullMode    = RHI_CULL_MODE_NONE;
        rs.frontFace   = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;
        RHIPipelineMultisampleStateCreateInfo ms {};
        ms.sType                = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;
        RHIPipelineColorBlendAttachmentState blend {};
        blend.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT |
                               RHI_COLOR_COMPONENT_A_BIT;
        RHIPipelineColorBlendStateCreateInfo bs {};
        bs.sType           = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        bs.attachmentCount = 1;
        bs.pAttachments    = &blend;
        RHIDynamicState dyn[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo ds {};
        ds.sType             = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates    = dyn;

        RHIGraphicsPipelineCreateInfo info {};
        info.sType               = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pVertexInputState   = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState      = &vs;
        info.pRasterizationState = &rs;
        info.pMultisampleState   = &ms;
        info.pColorBlendState    = &bs;
        info.pDynamicState       = &ds;
        info.layout              = layout;
        info.renderPass          = render_pass;
        RHIPipeline* pipeline = nullptr;
        rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &info, pipeline);
        rhi->DestroyShaderModule(vert);
        rhi->DestroyShaderModule(frag);
        return pipeline;
    }

    // 两个半屏视口还是一个全屏视口。
    std::vector<AppleSsaoViewport> MakeViewports(bool split)
    {
        std::vector<AppleSsaoViewport> out;
        const uint32_t n = split ? 2u : 1u;
        for (uint32_t i = 0; i < n; ++i)
        {
            AppleSsaoViewport v {};
            const float w   = static_cast<float>(kSize) / static_cast<float>(n);
            v.viewport      = {w * static_cast<float>(i), 0.0f, w, static_cast<float>(kSize), 0.0f, 1.0f};
            v.scissor       = {{static_cast<int32_t>(w * i), 0}, {static_cast<uint32_t>(w), kSize}};
            v.proj_view     = Matrix4x4::IDENTITY;
            v.camera_position = Vector3(0.0f, 0.0f, -1.0f);
            out.push_back(v);
        }
        return out;
    }

    // 把场景（深度 + 法线）按给定视口画进两张图。
    void DrawScene(RHI* rhi, RHICommandBuffer* cmd, Target& target, RHIPipeline* pipeline,
                   const std::vector<AppleSsaoViewport>& viewports)
    {
        RHIClearValue clear {};
        clear.color.float32[0] = 1.0f;  // 深度图清成 1 = 天空；法线图无所谓
        RHIRenderPassBeginInfo begin {};
        begin.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass      = target.render_pass;
        begin.framebuffer     = target.framebuffer;
        begin.renderArea      = {{0, 0}, {kSize, kSize}};
        begin.clearValueCount = 1;
        begin.pClearValues    = &clear;
        rhi->CmdBeginRenderPassPFN(cmd, &begin, RHI_SUBPASS_CONTENTS_INLINE);
        rhi->CmdBindPipelinePFN(cmd, RHI_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (const AppleSsaoViewport& v : viewports)
        {
            rhi->CmdSetViewportPFN(cmd, 0, 1, &v.viewport);
            rhi->CmdSetScissorPFN(cmd, 0, 1, &v.scissor);
            rhi->CmdDraw(cmd, 3, 1, 0, 0);
        }
        rhi->CmdEndRenderPassPFN(cmd);
    }

    struct SceneResult
    {
        bool                 recorded {false};
        std::vector<uint8_t> ao;     // RG8
        std::vector<float>   depth;  // R32F
    };

    SceneResult RunScene(RHI* rhi, AppleSsaoPass& pass, const char* depth_body, const char* normal_body, bool split,
                         const AppleSsaoSettings& settings)
    {
        SceneResult result;
        Target depth  = MakeTarget(rhi, RHI_FORMAT_R32_SFLOAT);
        Target normal = MakeTarget(rhi, RHI_FORMAT_R8G8B8A8_UNORM);
        RHIPipeline* depth_pipeline  = MakeScenePipeline(rhi, depth.render_pass, SceneFragment(depth_body));
        RHIPipeline* normal_pipeline = MakeScenePipeline(rhi, normal.render_pass, SceneFragment(normal_body));
        if (depth.framebuffer == nullptr || normal.framebuffer == nullptr || depth_pipeline == nullptr ||
            normal_pipeline == nullptr)
        {
            return result;
        }
        const std::vector<AppleSsaoViewport> viewports = MakeViewports(split);
        // 每次录制一个 pool，录完立刻 drain（MetalRHI 还活着）：任何对 autorelease 的
        // command buffer / encoder 多做一次 release，都会在这里的 drain 上野指针崩溃。
        // 这正是 2026-10-26 修掉的那类问题的复现器 —— 没有 pool 时它永远看不出来。
        @autoreleasepool
        {
            if (RHICommandBuffer* cmd = rhi->BeginSingleTimeCommands())
            {
                DrawScene(rhi, cmd, depth, depth_pipeline, viewports);
                DrawScene(rhi, cmd, normal, normal_pipeline, viewports);
                result.recorded = pass.Record(cmd, depth.view, normal.view, {kSize, kSize}, viewports.data(),
                                              static_cast<uint32_t>(viewports.size()), settings);
                rhi->EndSingleTimeCommands(cmd);
            }
        }
        if (result.recorded)
        {
            result.ao = ReadBack(rhi, pass.GetOutputImage(), 2);
        }
        const std::vector<uint8_t> raw = ReadBack(rhi, depth.image, 4);
        result.depth.resize(raw.size() / 4);
        if (!raw.empty())
        {
            std::memcpy(result.depth.data(), raw.data(), raw.size());
        }
        return result;
    }

    float AoAt(const SceneResult& r, uint32_t x, uint32_t y, uint32_t channel)
    {
        return r.ao[(static_cast<size_t>(y) * kSize + x) * 2 + channel] / 255.0f;
    }
    float DepthAt(const SceneResult& r, uint32_t x, uint32_t y) { return r.depth[static_cast<size_t>(y) * kSize + x]; }

    void CheckGpu()
    {
        {
            AppleSsaoPass pass;
            Check(!pass.IsReady(), "a default-constructed AppleSsaoPass is not ready");
            Check(!pass.Initialize(nullptr), "Initialize rejects a null RHI");
            Check(pass.GetOutputView() == nullptr, "no output before any Record");
        }

        MetalRHI metal_rhi;
        RHI*     rhi = &metal_rhi;
        if (!metal_rhi.Initialize())
        {
            std::printf("[skip] no Metal device -- GPU checks skipped\n");
            return;
        }

        AppleSsaoPass pass;
        const bool ready = pass.Initialize(rhi);
        Check(ready, "AppleSsaoPass::Initialize: all three MSL stages compile + pipelines build (headless Metal)");
        if (!ready)
        {
            return;
        }

        Check(!pass.Record(nullptr, nullptr, nullptr, {kSize, kSize}, nullptr, 0, {}) && pass.GetOutputView() == nullptr,
              "Record with null inputs returns false and leaves no output");

        AppleSsaoSettings settings {};
        settings.radius                   = 0.3f;  // ≈ 9.6 px（64 px 横跨 2 个 NDC 单位）
        settings.intensity                = 1.0f;
        settings.direct_lighting_strength = 0.5f;
        settings.sample_count             = 16;
        settings.blur                     = true;

        // ---- 台阶 ----
        for (const bool blur : {true, false})
        {
            settings.blur = blur;
            const std::string tag = blur ? " [blur]" : " [raw]";
            const SceneResult r = RunScene(rhi, pass, kStepDepth, kFlatNormal, false, settings);
            Check(r.recorded && pass.GetOutputView() != nullptr, "step: Record succeeded and exposes an output" + tag);
            if (r.ao.size() != kSize * kSize * 2 || r.depth.size() != kSize * kSize)
            {
                Check(false, "step: read back AO and depth" + tag);
                continue;
            }

            // 用读回来的深度给像素分类（不猜 y 轴朝向）。
            float sky_min = 1.0f, far_floor_min = 1.0f, far_block_min = 1.0f, edge_sum = 0.0f, g_err = 0.0f;
            int   sky_n = 0, far_floor_n = 0, far_block_n = 0, edge_n = 0, bad_n = 0;
            for (uint32_t y = 0; y < kSize; ++y)
            {
                for (uint32_t x = 0; x < kSize; ++x)
                {
                    const float d  = DepthAt(r, x, y);
                    const float ao = AoAt(r, x, y, 0);
                    const float g  = AoAt(r, x, y, 1);
                    g_err          = std::max(g_err, std::fabs(g - (1.0f - settings.direct_lighting_strength * (1.0f - ao))));
                    if (d >= 1.0f)
                    {
                        sky_min = std::min(sky_min, std::min(ao, g));
                        ++sky_n;
                        continue;
                    }
                    // 到台阶边（x = 32）的距离；离天空边远一点免得天空边界干扰。
                    // 地面（d=0.6）在左半、凸台（d=0.4）在右半 —— 这一点由 NDC 决定，下面那条
                    // "地面在左"的断言顺带证明了 ndc.x 与像素 x 同向。
                    const int dist_edge = d > 0.5f ? 31 - static_cast<int>(x) : static_cast<int>(x) - 32;
                    bool near_sky = false;
                    for (int dy = -12; dy <= 12 && !near_sky; ++dy)
                    {
                        const int yy = static_cast<int>(y) + dy;
                        near_sky = yy >= 0 && yy < static_cast<int>(kSize) && DepthAt(r, x, static_cast<uint32_t>(yy)) >= 1.0f;
                    }
                    if (near_sky)
                    {
                        continue;
                    }
                    if (dist_edge < 0)
                    {
                        ++bad_n;  // 地面出现在右半 / 凸台出现在左半
                        continue;
                    }
                    if (d > 0.5f && dist_edge >= 14)
                    {
                        far_floor_min = std::min(far_floor_min, ao);
                        ++far_floor_n;
                    }
                    else if (d < 0.5f && dist_edge >= 6)
                    {
                        far_block_min = std::min(far_block_min, ao);
                        ++far_block_n;
                    }
                    else if (d > 0.5f && dist_edge <= 2)
                    {
                        edge_sum += ao;
                        ++edge_n;
                    }
                }
            }
            const float edge_mean = edge_n > 0 ? edge_sum / static_cast<float>(edge_n) : 1.0f;
            Check(bad_n == 0, "step: every pixel's depth matches the left-floor / right-block layout" + tag);
            Check(DepthAt(r, 4, kSize / 2) > 0.5f && DepthAt(r, kSize - 5, kSize / 2) < 0.5f,
                  "step: synthetic scene has the floor on the left and the block on the right" + tag);
            std::printf("       step%s: sky_n=%d far_floor_n=%d (min %.3f) far_block_n=%d (min %.3f) edge_n=%d (mean %.3f) g_err=%.4f\n",
                        tag.c_str(), sky_n, far_floor_n, far_floor_min, far_block_n, far_block_min, edge_n, edge_mean, g_err);

            Check(sky_n > 0 && sky_min >= 0.999f, "step: sky pixels are exactly unoccluded (r = g = 1)" + tag);
            Check(far_floor_n > 0 && far_floor_min >= 0.97f, "step: flat floor far from the step is unoccluded" + tag);
            Check(far_block_n > 0 && far_block_min >= 0.97f, "step: top of the raised block is unoccluded" + tag);
            Check(edge_n > 0 && edge_mean <= 0.85f, "step: floor right next to the raised block IS occluded" + tag);
            Check(g_err <= 3.0f / 255.0f, "step: g = lerp(1, r, DirectLightingStrength) everywhere" + tag);
        }

        // ---- 斜坡：单视口 + 两个半屏视口 ----
        settings.blur = true;
        for (const bool split : {false, true})
        {
            const std::string tag = split ? " [two viewports]" : " [one viewport]";
            const SceneResult r   = RunScene(rhi, pass, kRampDepth, kRampNormal, split, settings);
            Check(r.recorded && r.ao.size() == kSize * kSize * 2, "ramp: Record + read back" + tag);
            if (r.ao.size() != kSize * kSize * 2)
            {
                continue;
            }
            float min_ao = 1.0f;
            for (uint32_t i = 0; i < kSize * kSize; ++i)
            {
                min_ao = std::min(min_ao, r.ao[i * 2] / 255.0f);
            }
            std::printf("       ramp%s: min r = %.3f (pass.vp=%u)\n", tag.c_str(), min_ao, pass.GetLastViewportCount());
            Check(min_ao >= 0.96f,
                  "ramp: a plane consistent with its normals has NO occlusion (NDC->pixel mapping + viewport bounds)" + tag);
            Check(pass.GetLastViewportCount() == (split ? 2u : 1u), "ramp: viewport count reported" + tag);
        }

        // ---- 只有左半一个视口：右半没人画，必须是清屏值 (1, 1) = 不遮蔽 ----
        //
        // 编辑器里两个视口之外的区域（以及某一帧某个视口无效时）都靠这一条：清成 0
        // 的话，延迟光照在那些像素上读到 0，环境光 / IBL 整片消失。
        {
            std::vector<AppleSsaoViewport> left = MakeViewports(true);
            left.resize(1);
            Target depth  = MakeTarget(rhi, RHI_FORMAT_R32_SFLOAT);
            Target normal = MakeTarget(rhi, RHI_FORMAT_R8G8B8A8_UNORM);
            RHIPipeline* depth_pipeline  = MakeScenePipeline(rhi, depth.render_pass, SceneFragment(kStepDepth));
            RHIPipeline* normal_pipeline = MakeScenePipeline(rhi, normal.render_pass, SceneFragment(kFlatNormal));
            bool recorded = false;
            if (depth_pipeline != nullptr && normal_pipeline != nullptr)
            {
                if (RHICommandBuffer* cmd = rhi->BeginSingleTimeCommands())
                {
                    // 场景画满全屏（右半有深度），但 AO 只给左半一个视口。
                    const std::vector<AppleSsaoViewport> full = MakeViewports(false);
                    DrawScene(rhi, cmd, depth, depth_pipeline, full);
                    DrawScene(rhi, cmd, normal, normal_pipeline, full);
                    recorded = pass.Record(cmd, depth.view, normal.view, {kSize, kSize}, left.data(), 1, settings);
                    rhi->EndSingleTimeCommands(cmd);
                }
            }
            const std::vector<uint8_t> ao = recorded ? ReadBack(rhi, pass.GetOutputImage(), 2) : std::vector<uint8_t>();
            uint8_t right_min = 255;
            for (uint32_t y = 0; y < kSize && ao.size() == kSize * kSize * 2; ++y)
            {
                for (uint32_t x = kSize / 2; x < kSize; ++x)
                {
                    const size_t i = (static_cast<size_t>(y) * kSize + x) * 2;
                    right_min      = std::min(right_min, std::min(ao[i], ao[i + 1]));
                }
            }
            Check(recorded && ao.size() == kSize * kSize * 2 && right_min == 255,
                  "outside every viewport the AO target keeps its clear value (1, 1) = unoccluded");
        }

        // ---- 关掉 = 0 个视口：必须清掉输出 ----
        {
            const std::vector<AppleSsaoViewport> vps = MakeViewports(false);
            Target depth  = MakeTarget(rhi, RHI_FORMAT_R32_SFLOAT);
            Target normal = MakeTarget(rhi, RHI_FORMAT_R8G8B8A8_UNORM);
            bool recorded = true;
            if (RHICommandBuffer* cmd = rhi->BeginSingleTimeCommands())
            {
                recorded = pass.Record(cmd, depth.view, normal.view, {kSize, kSize}, vps.data(), 0, settings);
                rhi->EndSingleTimeCommands(cmd);
            }
            Check(!recorded && pass.GetOutputView() == nullptr,
                  "disabled (0 viewports): Record returns false and GetOutputView() is nullptr (no stale image)");
        }
    }
#endif  // __APPLE__
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 崩了也要看得见最后一条
    std::printf("=== AppleSsao smoke test ===\n");
    CheckGpuParams();
    CheckMslSource();
    CheckWiringContracts();
    CheckMetalOwnershipContracts();
#if defined(__APPLE__)
    // 外层再包一个 pool：MetalRHI 析构之后再 drain，覆盖"pool 比 RHI 活得久"的顺序。
    // （2026-10-23 这里包 pool 会在 pop 时 SIGSEGV —— MetalRHI 对 autorelease 的
    //  encoder / command buffer 多 release 了一次；2026-10-26 修掉，见 §5。）
    @autoreleasepool
    {
        CheckGpu();
    }
#endif
    std::printf("\nAppleSsaoSmokeTest: %d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
