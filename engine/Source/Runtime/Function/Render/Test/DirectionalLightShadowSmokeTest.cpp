// =============================================================================
// 方向光阴影（Apple / Metal）接线冒烟测试
// -----------------------------------------------------------------------------
// 钉的是两件"代码全在、全接好了，却因为一条判据 / 一次漏调而完全不生效，而且不报
// 任何错"的事 —— 这正是这个仓库里出过最多次的一类缺陷：
//
//   (1) 阴影门。`DirectionalLightShadowPass::DrawModel()` 里控制"到底画不画
//       mesh"的判据以前问的是 `m_Rhi->IsPointLightShadowEnabled()`。那是历史遗留
//       名：它在**所有**后端上实际挡的是"mesh shadow map 绘制"整体（方向光 + 点
//       光）。Metal 只能共用这一个开关并答 false，于是方向光 shadow map 每帧只
//       clear、一个物体都不画 —— 而 pass 建了、4096² 的 attachment 建了、draw
//       list 里每帧排了 `DirectionalShadow`、主相机也通过 SetShadowImageViews()
//       拿到了 view。整条路只差那一条判据，画面上却只是"没有阴影"。
//
//   (2) storage ring buffer 复位。`RenderResource::ResetRingBufferOffset()` 只有
//       两个调用点，分别在 `SubmitDrawLists()` 的 DX12 分支和 Vulkan 分支里。而
//       Apple 那条精简 Tick 从不走 `SubmitDrawLists`（那个函数的 Apple 分支压根
//       没有调用者），所以这个复位在 Apple 上一次都没发生过。ring buffer 的
//       `End` 是只增的，于是每帧累积，直到撞上 pass 里那个上限 assert。以前只有
//       主相机在用、每帧用量小，能撑几千帧；打开方向光阴影之后用量变大，
//       frame=2641 就 SIGABRT 了（ips 里是
//       `__assert_rtn` ← `DirectionalLightShadowPass::DrawModel`）。
//
// 为什么只能是文本契约：这两条都发生在"真的有一块 GPU、真的跑着一个编辑器"的时
// 候。测试进程既建不出 Metal 设备，也到不了 RenderSystem::Tick 那一帧。
//
// 自带的双向自检见文件末尾 —— "全库扫一遍结果是 0 处"这种判据，扫描器自己写错的
// 时候会恒绿，必须拿已知样本证明它真的会红。
// =============================================================================

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
/// 为什么不直接 `text.find(needle)`：把一行代码注释掉之后全文 find 照样命中，
/// 契约就瞎了 —— 这套契约已经踩过很多次这个坑。本文件里那条
/// "方向光判据里不再出现旧开关"的判据尤其依赖它：旧开关的名字在**注释**里是出现
/// 的（解释历史的时候必须提到它），只有跳注释的扫描器才分辨得出代码里到底有没有。
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

bool CodeLineMentions(const std::string& src, const std::string& needle)
{
    return CodeLineFind(src, needle) != std::string::npos;
}

/// 从 `from` 开始逐行扫（跳过整行注释），返回第一条含 `return` 的代码行。
///
/// 用来钉"某个函数到底 return 了什么"。直接全文搜 `return true` 是不行的：解释
/// 历史的注释里就有"以前答 false"这类文字，全文搜永远分不清注释和函数体。
std::string FirstReturnLineFrom(const std::string& src, size_t from)
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
        if (!commented && line.find("return") != std::string::npos)
        {
            return line;
        }
        at = eol + 1;
    }
    return {};
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
    const std::string rhi_h        = ReadFile(ZENGINE_RHI_H);
    const std::string metal_h      = ReadFile(ZENGINE_METAL_RHI_H);
    const std::string metal_mm     = ReadFile(ZENGINE_METAL_RHI_MM);
    const std::string dir_pass_cpp = ReadFile(ZENGINE_DIRECTIONAL_LIGHT_PASS_CPP);
    const std::string render_cpp   = ReadFile(ZENGINE_RENDER_SYSTEM_CPP);
    const std::string scene_h      = ReadFile(ZENGINE_RENDER_SCENE_H);
    const std::string scene_cpp    = ReadFile(ZENGINE_RENDER_SCENE_CPP);

    Check(!rhi_h.empty(), "RHI.h 读得到");
    Check(!metal_h.empty(), "MetalRHI.h 读得到");
    Check(!metal_mm.empty(), "MetalRHI.mm 读得到");
    Check(!dir_pass_cpp.empty(), "DirectionalLightPass.cpp 读得到");
    Check(!render_cpp.empty(), "RenderSystem.cpp 读得到");
    Check(!scene_h.empty(), "RenderScene.h 读得到");
    Check(!scene_cpp.empty(), "RenderScene.cpp 读得到");

    // ---- 1. 方向光有自己独立的阴影开关 -------------------------------------
    //
    // 三条分开钉是因为它们能各自被独立破坏：删掉基类声明 → 其他后端编译不过；
    // 删掉 Metal 的 override → Metal 退回默认（= 旧的共用开关 = false）；
    // 把 Metal 的 override 改成 return false → 回到"只 clear 不画"。
    Check(CodeLineMentions(rhi_h, "IsDirectionalLightShadowEnabled"),
          "RHI 基类声明了方向光专用的阴影开关");
    Check(CodeLineMentions(metal_h, "IsDirectionalLightShadowEnabled"),
          "MetalRHI 覆盖了方向光专用的阴影开关");
    Check(CodeLineMentions(metal_mm, "MetalRHI::IsDirectionalLightShadowEnabled"),
          "MetalRHI 有方向光阴影开关的实现");

    // ---- 2. 钉"生效那一步"：Metal 上方向光必须真的开 ------------------------
    //
    // 只钉"有 override / 有实现"是不够的 —— 机制一个不少，return 改成 false 照样全
    // 绿，而那正是原来的样子。所以取出函数体里第一条 return 行看它到底答什么。
    {
        const size_t impl_at = CodeLineFind(metal_mm, "MetalRHI::IsDirectionalLightShadowEnabled");
        Check(impl_at != std::string::npos, "能定位到 Metal 方向光阴影开关的实现");

        const std::string ret = FirstReturnLineFrom(metal_mm, impl_at);
        Check(!ret.empty(), "Metal 方向光阴影开关里有一条 return");
        Check(ret.find("true") != std::string::npos,
              "Metal 方向光阴影开关 return true（改回 false 就是「只 clear 不画」）");
        Check(ret.find("false") == std::string::npos, "Metal 方向光阴影开关不再 return false");
    }

    // ---- 3. 点光维持关闭 ---------------------------------------------------
    //
    // 点光阴影走的是 geometry shader + 2D array 那条路，Metal 上没有 geometry
    // shader，从未验证过。这次只开方向光，点光必须留在原样 —— 否则就是把一个未验
    // 证的路径一起放出来。
    {
        const size_t impl_at = CodeLineFind(metal_mm, "MetalRHI::IsPointLightShadowEnabled");
        Check(impl_at != std::string::npos, "Metal 仍然实现了点光阴影开关");

        const std::string ret = FirstReturnLineFrom(metal_mm, impl_at);
        Check(!ret.empty(), "Metal 点光阴影开关里有一条 return");
        Check(ret.find("false") != std::string::npos,
              "Metal 点光阴影开关仍然 return false（未验证路径不许顺手打开）");
    }

    // ---- 4. 方向光 pass 真的改用了新开关 -----------------------------------
    //
    // 两条一起钉：改用了新的 **且** 不再用旧的。只钉前者的话，把判据写回旧开关时
    // 新开关可能还留在别处（比如日志），契约照样绿。
    Check(CodeLineMentions(dir_pass_cpp, "m_Rhi->IsDirectionalLightShadowEnabled()"),
          "方向光 shadow draw 的判据问的是方向光专用开关");
    Check(!CodeLineMentions(dir_pass_cpp, "m_Rhi->IsPointLightShadowEnabled()"),
          "方向光 shadow draw 的判据不再问那个历史遗留的共用开关");
    Check(CountCodeLines(dir_pass_cpp, "m_Rhi->IsDirectionalLightShadowEnabled()") == 1,
          "方向光阴影判据只有一处（多一处说明有人新开了条没被契约覆盖的路）");

    // ---- 5. Apple 的 Tick 每帧复位 storage ring buffer ---------------------
    //
    // 这是那个 SIGABRT 的直接成因。钉三件事：调用真的在 Apple 的 Tick 里、排在粒
    // 子推进之前（粒子也往同一个 buffer 写）、排在 draw list 执行之前（pass 的
    // Draw() 才是真正往里写数据的人）。
    const size_t reset_at = CodeLineFind(render_cpp, "ResetRingBufferOffset(");
    Check(reset_at != std::string::npos, "Apple 的 Tick 里调用了 ring buffer 复位");

    if (reset_at != std::string::npos)
    {
        const size_t particles_at = render_cpp.find("PrepareParticlesForDraw(", reset_at);
        Check(particles_at != std::string::npos && particles_at > reset_at,
              "ring buffer 复位排在粒子推进之前（粒子也往同一个 buffer 里写）");

        const size_t execute_at = render_cpp.find("draw_list.ExecuteAll()", reset_at);
        Check(execute_at != std::string::npos && execute_at > reset_at,
              "ring buffer 复位排在 draw list 执行之前");
    }

    // ---- 6. 心跳探针还在 ---------------------------------------------------
    //
    // 上面那几条只能证明"代码长这样"，证明不了"真机上这条路在跑"。那个证据来自
    // 心跳里的 `shadow=(dir_en=... dir_nodes=... point_en=...)`：dir_en 是开关，
    // dir_nodes 是这一帧真往 shadow map 里画的物体数。探针被删掉的话，下次同样的
    // 静默失效又会没人发现。
    Check(CodeLineMentions(render_cpp, "shadow=(dir_en="), "心跳格式串里有 shadow 段");
    Check(CodeLineMentions(render_cpp, "dir_shadow_enabled"), "心跳实参里有方向光阴影开关");
    Check(CodeLineMentions(render_cpp, "dir_shadow_nodes"), "心跳实参里有方向光阴影物体数");
    Check(CodeLineMentions(render_cpp, "GetDirectionalLightVisibleMeshNodes()"),
          "心跳的物体数取自方向光可见节点列表");
    Check(CodeLineMentions(scene_h, "GetDirectionalLightVisibleMeshNodes"),
          "RenderScene 声明了方向光可见节点 getter");
    Check(CodeLineMentions(scene_cpp, "RenderScene::GetDirectionalLightVisibleMeshNodes"),
          "RenderScene 实现了方向光可见节点 getter");

    // ---- 双向自检 ----------------------------------------------------------
    //
    // "扫描结果是 0 处"这种判据，扫描器自己写错的时候会恒绿。下面这几条拿已知样本
    // 证明扫描器真的会红 —— 尤其前两条，它们顺带证明了 §4 那条
    // "不再问旧开关"的判据是有意义的：那个名字在这份源码的注释里**是**出现的。
    {
        // 旧开关的名字在 DirectionalLightPass.cpp 的注释里出现（解释历史必须提到
        // 它），全文 find 能找到；但代码里不该有，跳注释的扫描器必须报 0 处。
        Check(dir_pass_cpp.find("IsPointLightShadowEnabled") != std::string::npos,
              "自检：旧开关的名字在注释里确实存在（否则下一条就是空判据）");
        Check(!CodeLineMentions(dir_pass_cpp, "IsPointLightShadowEnabled()"),
              "自检：跳注释的扫描器在该文件里报 0 处");

        const std::string dirty = "int x = 0;\n    foo();\n    return 1;\n";
        Check(CodeLineMentions(dirty, "foo()"), "自检：扫描器能从真实代码行里找出调用");

        const std::string clean = "int x = 0;\n    return x;\n";
        Check(!CodeLineMentions(clean, "foo()"), "自检：扫描器对干净样本报 0 处");

        const std::string commented = "//     bar();\n    return 1;\n";
        Check(!CodeLineMentions(commented, "bar()"), "自检：整行注释里的调用不算命中");

        const std::string real = "// a comment\n    bar();\n";
        Check(CodeLineMentions(real, "bar()"), "自检：真实代码行仍然命中");

        // FirstReturnLineFrom 同样不许被注释糊弄：注释里常有"以前答 false"这类话。
        const std::string with_comment =
            "// previously it answered false\n    return true;\n";
        Check(FirstReturnLineFrom(with_comment, 0).find("true") != std::string::npos,
              "自检：取 return 行时跳过注释，拿到的是真的 return true");

        const std::string false_impl = "bool F()\n{\n    return false;\n}\n";
        Check(FirstReturnLineFrom(false_impl, 0).find("false") != std::string::npos,
              "自检：取 return 行也能正确拿到 return false");
    }

    std::fprintf(stderr, "DirectionalLightShadowSmokeTest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
