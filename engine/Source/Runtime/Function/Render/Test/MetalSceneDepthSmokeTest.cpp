// =============================================================================
// Metal 场景深度 + 延迟光照输出附件 冒烟测试
// -----------------------------------------------------------------------------
// 钉的是 2026-10-23 两个"通道全接好、却在 Metal 上完全没生效"的洞（真机 Metal
// 校验层 + 截图实锤）：
//
//   洞 #16 场景深度图不存在。MetalRHI 声明了 m_DepthImageFormat / m_DepthImageView
//     却**没有任何赋值点**，GetDepthImageInfo() 恒返回 {UNDEFINED, nullptr}。所有
//     深度附件的格式与句柄都取自它 ⇒ nullptr 的 view 在 CreateFramebuffer 里表示
//     "这一槽是 swapchain drawable"，深度槽变成第 N 张颜色附件，深度测试/深度写入
//     完全不存在（校验层每秒刷几千条 "MTLDepthStencilDescriptor sets depth test
//     but MTLRenderPassDescriptor has a nil depthAttachment texture"）。方向光/点
//     光 shadow、Pick、DebugDraw 的深度格式也全取自它，一并受害。
//
//   洞 #17 延迟光照输出写错附件。Metal 没有 subpass，"input attachment" 编译后就
//     是普通采样纹理，根本不需要挂进 framebuffer；而 Metal 的 CmdBeginRenderPass
//     是按"第几个非深度附件"给颜色槽编号的，把 GBufferA/B/C 一并挂上后它们占走
//     color 0/1/2，延迟光照的 out_color（location 0）写进了 GBufferA，真正的场景色
//     backup_odd 每帧只剩 CLEAR 的清屏色 —— 视口一片 0.29/0.345/0.435 纯色，
//     15 个可见实体全部不可见（截图为证），不报任何错。
//
// 为什么是文本契约：两件事都发生在"真的有一块 GPU、真的跑着一个编辑器"的时候，
// 测试进程既建不出 Metal 设备，也到不了那一帧。
//
// ⚠️ 钉的都是"生效那一步"，不是"机制存在"：
//   - 深度图钉到 usage 的 SAMPLED 位（没有它延迟光照采样不了）与 recreate 守卫；
//   - 输出附件钉到"metal_attachments[0] 必须是 BackupOdd"这一行；
//   - clear 值钉到"场景清屏色必须在 metal 下标 0"（CmdBeginRenderPass 用附件下标
//     pClearValues[i] 取 clear 值，不是颜色槽下标 —— 挂错了下标，清屏色就丢了）。
// =============================================================================

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what)
{
    ++g_checks;
    if (condition)
    {
        return;
    }
    ++g_failed;
    std::fprintf(stderr, "[FAIL] %s\n", what.c_str());
}

std::string ReadFile(const std::string& path)
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

/// 逐行找 `needle`，跳过整行都是注释的行，返回首次命中的绝对位置。
///
/// 为什么不直接 text.find()：把一行代码注释掉之后全文 find 照样命中，契约就瞎了
/// —— 而且本文件的注释里就逐字出现了被搜索的代码（解释历史必须提到它）。
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
}

size_t CodeLineFind(const std::string& src, const std::string& needle)
{
    return CodeLineFindFrom(src, needle, 0);
}

/// 找同时含 `a` 和 `b` 的第一条代码行。
///
/// 单个 needle 不够用的时候（比如 `metal_attachments[0]` 单独出现没法说明它绑的是
/// BackupOdd），要求两个片段出现在同一行。
size_t CodeLineWithBoth(const std::string& src, const std::string& a, const std::string& b)
{
    size_t at = 0;
    while ((at = CodeLineFindFrom(src, a, at)) != std::string::npos)
    {
        size_t eol = src.find('\n', at);
        if (eol == std::string::npos)
        {
            eol = src.size();
        }
        if (src.find(b, at) < eol && src.find(b, at) != std::string::npos)
        {
            return at;
        }
        at = eol + 1;
    }
    return std::string::npos;
}

/// 截取 `from` 代码行到 `until` 代码行之间的片段（用于"某个初始化器块内部"的断言）。
std::string BlockBetween(const std::string& src, const std::string& from, const std::string& until)
{
    const size_t begin = CodeLineFind(src, from);
    if (begin == std::string::npos)
    {
        return {};
    }
    const size_t end = CodeLineFindFrom(src, until, begin);
    return src.substr(begin, (end == std::string::npos ? src.size() : end) - begin);
}

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
}  // namespace

int main()
{
    const std::string metal_h   = ReadFile(ZENGINE_METAL_RHI_H);
    const std::string metal_mm  = ReadFile(ZENGINE_METAL_RHI_MM);
    const std::string fb_cpp    = ReadFile(ZENGINE_FRAMEBUFFER_CPP);
    const std::string rp1_cpp   = ReadFile(ZENGINE_RP1_CPP);
    const std::string render_cp = ReadFile(ZENGINE_RENDER_SYSTEM_CPP);

    // ---- 自检 0：文件都真的读到了（读不到就全绿，等于没测） ----
    Check(!metal_h.empty(), "selftest: MetalRHI.h is non-empty");
    Check(!metal_mm.empty(), "selftest: MetalRHI.mm is non-empty");
    Check(!fb_cpp.empty(), "selftest: MainCameraFramebufferResources.cpp is non-empty");
    Check(!rp1_cpp.empty(), "selftest: MainCameraRp1Pass.cpp is non-empty");
    Check(!render_cp.empty(), "selftest: RenderSystem.cpp is non-empty");

    // ---- §1 洞 #16：场景深度图（MetalRHI） ----

    // 生效那一步：默认格式必须是深度格式。以前是 {RHI_FORMAT_UNDEFINED}，整条
    // 深度链（主相机 / 方向光点光 shadow / Pick / DebugDraw）拿到的全是 UNDEFINED。
    Check(CodeLineFind(metal_h, "RHIFormat m_DepthImageFormat {RHI_FORMAT_D32_SFLOAT}") !=
              std::string::npos,
          "MetalRHI.h: m_DepthImageFormat defaults to D32_SFLOAT");

    Check(CodeLineFind(metal_h, "void EnsureSceneDepthImage();") != std::string::npos,
          "MetalRHI.h: declares EnsureSceneDepthImage()");
    Check(CodeLineFind(metal_h, "void DestroySceneDepthImage();") != std::string::npos,
          "MetalRHI.h: declares DestroySceneDepthImage()");

    // 生效那一步：EnsureSceneDepthImage() 必须把建出来的图交回给成员持有。声明个
    // 成员不等于持有 —— 光有成员、没有这行赋值，GetDepthImageInfo() 照样是 nullptr。
    Check(CodeLineWithBoth(metal_mm, "m_DepthImage", "= image;") != std::string::npos,
          "MetalRHI.mm: EnsureSceneDepthImage stores the created image in m_DepthImage");

    // SAMPLED 是生效位：没有它，延迟光照反推世界坐标的 in_scene_depth 采样不了。
    Check(CodeLineFind(metal_mm,
                       "RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT") !=
              std::string::npos,
          "MetalRHI.mm: depth image created with DEPTH_STENCIL_ATTACHMENT | SAMPLED");

    // recreate 守卫的生效那一步：尺寸一致才复用，不一致就重建。
    Check(CodeLineFind(metal_mm, "m_DepthImageExtent.width == m_SwapchainExtent.width") !=
              std::string::npos,
          "MetalRHI.mm: depth image recreated when swapchain size changes");

    // 销毁必须真的释放（MetalRHI::DestroyImage/DestroyImageView 是空实现）。
    Check(CodeLineFind(metal_mm, "delete (MetalImageView*)m_DepthImageView") != std::string::npos,
          "MetalRHI.mm: DestroySceneDepthImage deletes the image view wrapper");
    Check(CodeLineFind(metal_mm, "delete (MetalImage*)m_DepthImage") != std::string::npos,
          "MetalRHI.mm: DestroySceneDepthImage deletes the image wrapper");

    // 调用点：CreateSwapchain / RecreateSwapchain / PrepareBeforePass 三处，一个不能少。
    // （PrepareBeforePass 那处是每帧真正生效的入口：Apple 的 swapchain 尺寸第一帧才就绪。）
    Check(CountCodeLines(metal_mm, "EnsureSceneDepthImage();") == 3,
          "MetalRHI.mm: EnsureSceneDepthImage() called from exactly 3 sites");

    // 顺序契约：PrepareBeforePass 里必须先建深度图、再触发 recreate 回调 —— 回调里
    // MainCameraFramebufferResources 会用 GetDepthImageInfo() 重建 framebuffer，
    // 那一刻 view 必须已经是新尺寸的。
    {
        const size_t prepare = CodeLineFind(metal_mm, "bool MetalRHI::PrepareBeforePass(");
        Check(prepare != std::string::npos, "MetalRHI.mm: found PrepareBeforePass");
        if (prepare != std::string::npos)
        {
            const size_t ensure = CodeLineFindFrom(metal_mm, "EnsureSceneDepthImage();", prepare);
            const size_t recreate =
                CodeLineFindFrom(metal_mm, "passUpdateAfterRecreateSwapchain();", prepare);
            Check(ensure != std::string::npos && recreate != std::string::npos && ensure < recreate,
                  "MetalRHI.mm: PrepareBeforePass ensures depth image BEFORE the recreate callback");
        }
    }

    // swapchain 清理必须连带销毁深度图（否则换窗口尺寸 / 关闭时泄漏一张全屏深度图）。
    {
        const size_t clear = CodeLineFind(metal_mm, "void MetalRHI::ClearSwapchain()");
        Check(clear != std::string::npos, "MetalRHI.mm: found ClearSwapchain");
        if (clear != std::string::npos)
        {
            Check(CodeLineFindFrom(metal_mm, "DestroySceneDepthImage();", clear) !=
                      std::string::npos,
                  "MetalRHI.mm: ClearSwapchain destroys the scene depth image");
        }
    }

    // ---- §2 洞 #17：延迟光照输出附件（MainCameraFramebufferResources.cpp） ----

    // 生效那一步：Metal 的 deferred 附件表 [0] 必须是 BackupOdd（唯一的颜色输出）。
    Check(CodeLineWithBoth(fb_cpp, "metal_attachments[0]", "kAttachment_BackupOdd") !=
              std::string::npos,
          "FramebufferResources: Metal deferred attachment[0] is BackupOdd");
    Check(CodeLineWithBoth(fb_cpp, "metal_attachments[1]", "kAttachment_Depth") !=
              std::string::npos,
          "FramebufferResources: Metal deferred attachment[1] is Depth");
    Check(CodeLineFind(fb_cpp, "pass_attachment_count = 2") != std::string::npos,
          "FramebufferResources: Metal deferred render pass has exactly 2 attachments");
    Check(CodeLineFind(fb_cpp, "input_attachment_count = 0") != std::string::npos,
          "FramebufferResources: Metal deferred pass has no input attachments");

    // framebuffer 侧同一步：Metal 的附件表里 BackupOdd 必须在 Depth **之前**（顺序
    // 就是附件下标；反过来的话输出会落到深度槽的位置上）。块范围严格限定在
    // metal_attachments 初始化器内 —— 放到 default_attachments 里去找同样能命中，
    // 那条 5 张表的顺序是另一回事，不能拿来给 Metal 背书。
    {
        const std::string block =
            BlockBetween(fb_cpp, "RHIImageView* metal_attachments[2]", "default_attachments");
        Check(!block.empty(), "FramebufferResources: found the Metal framebuffer attachment table");
        const size_t odd   = CodeLineFind(block, "_main_camera_pass_backup_buffer_odd");
        const size_t depth = CodeLineFind(block, "GetDepthImageInfo().depth_image_view");
        Check(odd != std::string::npos && depth != std::string::npos && odd < depth,
              "FramebufferResources: Metal deferred framebuffer [0]=BackupOdd before [1]=Depth");
    }
    Check(CodeLineFind(fb_cpp, "is_metal ? 2u : 5u") != std::string::npos,
          "FramebufferResources: Metal deferred framebuffer attachment count is 2");

    // ---- §3 clear 值下标（MainCameraRp1Pass.cpp） ----
    // CmdBeginRenderPass 用 pClearValues[附件下标] 取清屏值：Metal 附件表里 BackupOdd
    // 在 [0]，场景清屏色就必须放在 [0] —— 放在老的第 4 格等于整张场景色每帧被清成
    // (0,0,0,0)。
    Check(CodeLineWithBoth(rp1_cpp, "clear_values_metal[0].color", "k_scene_clear_r") !=
              std::string::npos,
          "Rp1Pass: Metal clear value[0] carries the scene clear color");
    Check(CodeLineWithBoth(rp1_cpp, "clear_values_metal[1].depthStencil", "1.0f") !=
              std::string::npos,
          "Rp1Pass: Metal clear value[1] is the depth clear value");
    // ⚠️ 不能全文搜 `clearValueCount = 2`：同一份文件里后处理那一趟也是 2，把 Metal
    // 这趟改成 5 之后全文照样命中 ⇒ 等于没钉。必须钉在 Metal 分支自己的块里。
    {
        const std::string block =
            BlockBetween(rp1_cpp, "RHIClearValue clear_values_metal[2];", "clear_values[0].color");
        Check(!block.empty(), "Rp1Pass: found the Metal clear-value block");
        Check(CodeLineFind(block, "renderpass_begin_info.clearValueCount = 2") != std::string::npos,
              "Rp1Pass: Metal deferred pass clears exactly 2 attachments");
    }

    // ---- §4 心跳探针（RenderSystem.cpp） ----
    Check(CodeLineFind(render_cp, "depth=(view={} fmt={})") != std::string::npos,
          "RenderSystem: heartbeat carries depth=(view fmt) probe");
    Check(CodeLineFind(render_cp, "GetDepthImageInfo().depth_image_view != nullptr") !=
              std::string::npos,
          "RenderSystem: depth probe reads GetDepthImageInfo().depth_image_view");

    // ---- §5 双向自检：扫描器自己坏了会恒绿 ----
    // 正向：已知存在的串必须找得到、且计数精确（定义行不含 "();"，不该被计入调用点）。
    Check(CountCodeLines(metal_mm, "void MetalRHI::ClearSwapchain()") == 1,
          "selftest: scanner finds exactly one ClearSwapchain definition");
    // 反向：已知不存在的串必须是 0 条（如果这条红了说明扫描器把注释外的什么都算命中）。
    Check(CountCodeLines(metal_mm, "ZZZ-NO-SUCH-TOKEN-IN-METALRHI") == 0,
          "selftest: scanner reports 0 for an absent token");
    Check(CodeLineFind(metal_h, "RHIFormat m_DepthImageFormat {RHI_FORMAT_UNDEFINED}") ==
              std::string::npos,
          "selftest: m_DepthImageFormat is no longer UNDEFINED");

    std::fprintf(stderr, "MetalSceneDepthSmokeTest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
