// =============================================================================
// ZEditor Launcher 接线冒烟测试
// -----------------------------------------------------------------------------
// 钉的是"双击 ZEditor 闪退"这条链路的接线，不是 Launcher 的像素 —— 窗口长什么样
// 只能靠人眼，但"没有 -p 时到底走哪条路"是可以被机器钉死的，而且它正是历史上
// 出过事的地方：
//
//   case StartupMode::OpenLauncher:
//       return reportMissingProject();     // ← 双击时这一行就是"闪退"
//
// reportMissingProject() 往 stderr 打一行字再 return 1。从终端跑能看见那行字；
// 从 Finder 双击启动的进程没有终端，谁也看不见，进程静默退出 1 —— 用户看到的就是
// "一点反应都没有 / 闪一下就没了"。所以 Launcher 真正要防的回归不是"窗口少了个
// 按钮"，而是"又被改回 reportMissingProject()"或者"重启那条链断了一环"。
//
// 纯文本契约：Main.cpp 的启动分支在 START_SYSTEM() 之前，任何测试进程都跑不到它
// （跑到就已经把 RHI / 世界 / 资源系统全拉起来了），所以只能把源码当文本读。
//
// 自带的双向自检见文件末尾 —— "全库扫一遍结果是 0 处"这种判据，扫描器自己写错的
// 时候会恒绿，必须拿已知样本证明它真的会红。
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

/// 逐行找 `needle`，跳过整行都是注释的行。
///
/// 为什么不直接 `text.find(needle)`：把一行代码注释掉之后全文 find 照样命中，
/// 契约就瞎了 —— 已经踩过一次的坑（见 doc/APPLE_ACCEPTANCE.md §文本契约）。
bool CodeLineFind(const std::string& text, const std::string& needle)
{
    size_t line_begin = 0;
    while (line_begin <= text.size())
    {
        size_t line_end = text.find('\n', line_begin);
        if (line_end == std::string::npos)
        {
            line_end = text.size();
        }

        const std::string line = text.substr(line_begin, line_end - line_begin);
        const size_t      first_non_space = line.find_first_not_of(" \t");
        const bool        whole_line_comment =
            first_non_space != std::string::npos && line.compare(first_non_space, 2, "//") == 0;

        if (!whole_line_comment && line.find(needle) != std::string::npos)
        {
            return true;
        }

        if (line_end == text.size())
        {
            break;
        }
        line_begin = line_end + 1;
    }
    return false;
}

size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += needle.size();
    }
    return count;
}

/// 从 `anchor` 第一次出现处开始，往后 `window` 个字符里找 `needle`。
///
/// `substr(npos, N)` 会抛 out_of_range 把整个测试进程带崩（留下的 .ips 会被误当成
/// 真机崩溃），所以这里先判 npos 再切，并且只切到串尾。
bool FoundAfter(const std::string& text, const std::string& anchor, const std::string& needle, size_t window)
{
    const size_t at = text.find(anchor);
    if (at == std::string::npos)
    {
        return false;
    }
    const std::string tail = text.substr(at, text.size() - at);
    return tail.size() > 0 && tail.find(needle, 0) != std::string::npos && tail.find(needle) < window;
}

}  // namespace

int main()
{
    const std::string main_cpp   = ReadFile(ZEDITOR_MAIN_CPP);
    const std::string launcher_h = ReadFile(ZEDITOR_LAUNCHER_DIR "/ZEditorLauncher.h");
    const std::string launcher_win = ReadFile(ZEDITOR_LAUNCHER_DIR "/ZEditorLauncher.cpp");
    const std::string launcher_mac = ReadFile(ZEDITOR_LAUNCHER_DIR "/ZEditorLauncher.mm");

    Check(!main_cpp.empty(), "Main.cpp 可读");
    Check(!launcher_h.empty(), "ZEditorLauncher.h 可读");
    Check(!launcher_win.empty(), "ZEditorLauncher.cpp 可读");
    Check(!launcher_mac.empty(), "ZEditorLauncher.mm 可读");

    // ---- 1. 两个平台的 OpenLauncher 分支都必须接到 Launcher 上 ----
    //
    // Main.cpp 里有两处 `case StartupMode::OpenLauncher:`：Win32 一处、Unix 一处。
    // 少一处就说明有人把一个平台改回去了。
    Check(CountOccurrences(main_cpp, "case StartupMode::OpenLauncher:") == 2,
          "Main.cpp 里 Win32 + Unix 各有一个 OpenLauncher 分支");

    Check(CodeLineFind(main_cpp, "return runLauncherAndDispatch();"),
          "Win32 的 OpenLauncher 分支走 Launcher");

    // Unix 那条：函数名带 Unix 后缀，跟 Win32 那个区分开。
    Check(CodeLineFind(main_cpp, "return runLauncherAndDispatchUnix();"),
          "Unix 的 OpenLauncher 分支走 Launcher（不是 reportMissingProject）");

    // 反面：Unix 的 OpenLauncher case 自己不许直接落到 reportMissingProject()。
    // 判"全文不含 reportMissingProject()"是错的 —— runLauncherAndDispatchUnix()
    // 内部在 Launcher 被放弃后仍然要调它（那是合理的兜底）。所以只盯 switch 里
    // 从最后一个 OpenLauncher case 起到文件尾这一段（它之后只剩 Abort/OpenProject
    // 两个 case），这一段里出现 reportMissingProject() 就说明有人改回去了。
    {
        const size_t at = main_cpp.rfind("case StartupMode::OpenLauncher:");
        Check(at != std::string::npos, "能定位到最后一个 OpenLauncher 分支（Unix）");
        if (at != std::string::npos)
        {
            const std::string tail   = main_cpp.substr(at, main_cpp.size() - at);
            const size_t      launch = tail.find("runLauncherAndDispatchUnix()");
            const size_t      report = tail.find("reportMissingProject()");
            Check(launch != std::string::npos,
                  "Unix 的 OpenLauncher case 之后能看到 runLauncherAndDispatchUnix()");
            Check(report == std::string::npos,
                  "Unix 的 OpenLauncher 分支没有退回 reportMissingProject()（那正是双击闪退的那一行）");
        }
    }

    // ---- 2. 重启自己那条链 ----
    //
    // CommandSystem::Initialize() 从 _NSGetArgv() 读 -p，进程内改 argv 不安全，所以
    // 选完项目必须重启。三件事缺一不可：拿自己的路径、用 SETEXEC（不 fork）、真的
    // posix_spawn。
    Check(CodeLineFind(main_cpp, "int relaunchWithProject("), "存在 relaunchWithProject()");
    Check(CodeLineFind(main_cpp, "_NSGetExecutablePath"), "重启用的是自己的可执行路径（不写死）");
    Check(CodeLineFind(main_cpp, "posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC)"),
          "重启用 POSIX_SPAWN_SETEXEC（不 fork：Cocoa 起来之后 fork 会崩）");
    Check(CodeLineFind(main_cpp, "posix_spawn("), "真的调用了 posix_spawn");

    // 反面：Main.cpp 里不许出现 fork()。
    Check(!CodeLineFind(main_cpp, "fork("), "Main.cpp 里没有 fork()");

    // ---- 3. 打开成功的项目要回写最近列表 ----
    //
    // 不然 Launcher 永远只有"第一次手动浏览"那一条，跟没做一样。
    Check(FoundAfter(main_cpp, "int runEditorUnix(", "ZEditorLauncher::RememberProject", 4000),
          "runEditorUnix() 里回写了最近项目（命令行 -p 打开的项目下次也要出现在 Launcher 里）");

    // ---- 4. 三份实现不能同时编 ----
    //
    // ZEditorLauncher.cpp 整个是 #ifdef _WIN32，ZEditorLauncher.mm 整个是
    // #if defined(__APPLE__)，剩下的是 POSIX 兜底桩。任何一个 guard 掉了就是
    // 重复符号 / Linux 链接不过。
    Check(CodeLineFind(launcher_win, "#ifdef _WIN32"), "Win32 实现仍然整体包在 _WIN32 里");
    Check(CodeLineFind(launcher_win, "#if !defined(_WIN32) && !defined(__APPLE__)"),
          "非 Win32 / 非 Apple 有兜底桩（否则 Linux 链接不过）");
    Check(CodeLineFind(launcher_mac, "#if defined(__APPLE__)"), "Apple 实现整体包在 __APPLE__ 里");

    // ---- 5. Apple 实现确实是个窗口，不是另一个空桩 ----
    Check(CodeLineFind(launcher_mac, "NSOpenPanel"), "Apple Launcher 有打开项目的面板");
    Check(CodeLineFind(launcher_mac, "@interface ZLauncherController"),
          "Apple Launcher 有 AppKit 窗口控制器");
    Check(CodeLineFind(launcher_mac, "NSApplicationActivationPolicyRegular"),
          "Apple Launcher 设了 Regular 激活策略（否则窗口拿不到焦点、不进 Dock）");
    Check(CodeLineFind(launcher_mac, "bool Run(LaunchChoice&"), "Apple 侧实现了 Run()");
    Check(CodeLineFind(launcher_mac, "void RememberProject(const std::filesystem::path&"),
          "Apple 侧实现了 RememberProject()");

    // ---- 6. 契约头里的约定没被改 ----
    Check(launcher_h.find("bool Run(LaunchChoice& out_choice);") != std::string::npos,
          "ZEditorLauncher.h 的 Run() 签名没变");

    // -------------------------------------------------------------------------
    // 扫描器自检
    //
    // 上面有两条判据是"扫不到才算对"（Main.cpp 里不许有 fork()）。扫描器自己写错
    // 的话它们恒绿 —— 用已知样本证明扫描器真的会报出来，也真的会对干净样本闭嘴。
    // -------------------------------------------------------------------------
    {
        const std::string dirty = "int x = 0;\n    pid_t p = fork();\n    return p;\n";
        Check(CodeLineFind(dirty, "fork("), "自检：扫描器能从真实代码行里找出 fork()");

        const std::string clean = "int x = 0;\n    return x;\n";
        Check(!CodeLineFind(clean, "fork("), "自检：扫描器对干净样本报 0 处");

        // 整行注释不算"存在" —— 这是这套契约踩过最多次的坑。
        const std::string commented = "//     return runLauncherAndDispatchUnix();\n    return 1;\n";
        Check(!CodeLineFind(commented, "runLauncherAndDispatchUnix()"),
              "自检：整行注释里的调用不算命中");

        const std::string real = "// a comment\n    return runLauncherAndDispatchUnix();\n";
        Check(CodeLineFind(real, "runLauncherAndDispatchUnix()"), "自检：真实代码行仍然命中");
    }

    std::fprintf(stderr, "ZEditorLauncherWiringSmokeTest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
