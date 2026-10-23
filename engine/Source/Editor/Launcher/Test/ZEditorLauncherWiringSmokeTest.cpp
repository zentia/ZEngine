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

/// CodeLineFind() 的位置版：返回第一个"非整行注释"命中的偏移，没有则 npos。
///
/// 判"某件事必须排在另一件事之前"时不能用 find()：这个仓库的注释里到处写着被
/// 检查的标识符（而且这是故意的 —— 注释要解释为什么），find() 会先命中注释里的
/// 那一份，把顺序判据变成恒绿。
size_t CodeLineFindPosition(const std::string& text, const std::string& needle)
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

        if (!whole_line_comment)
        {
            const size_t at = line.find(needle);
            if (at != std::string::npos)
            {
                return line_begin + at;
            }
        }

        if (line_end == text.size())
        {
            break;
        }
        line_begin = line_end + 1;
    }
    return std::string::npos;
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
    const std::string command_cpp = ReadFile(ZEDITOR_COMMANDSYSTEM_CPP);
    const std::string config_cpp = ReadFile(ZEDITOR_CONFIGMANAGER_CPP);
    const std::string bundle_cmake = ReadFile(ZEDITOR_BUNDLE_CMAKE);

    Check(!main_cpp.empty(), "Main.cpp 可读");
    Check(!launcher_h.empty(), "ZEditorLauncher.h 可读");
    Check(!launcher_win.empty(), "ZEditorLauncher.cpp 可读");
    Check(!launcher_mac.empty(), "ZEditorLauncher.mm 可读");
    Check(!command_cpp.empty(), "CommandSystem.cpp 可读");
    Check(!config_cpp.empty(), "ConfigManager.cpp 可读");
    Check(!bundle_cmake.empty(), "MakeMacOSAppBundle.cmake 可读");

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

    // ---- 7. 从 .app bundle 启动时不能找不到 ZEditor.ini ----
    //
    // bundle 里 Contents/MacOS/ZEditor 只是指向真身 bin/<Config>/ZEditor 的相对
    // symlink，而 ini / asset / resource 全在真身旁边。用 argv[0] 的父目录拼 ini
    // 会落到 Contents/MacOS 底下 —— 找不到，ConfigManager 起不来，进程抛异常退出。
    // 双击时还多一层：CWD 是 /，连"相对路径碰巧能命中"都没有，看起来就是闪退。
    Check(CodeLineFind(command_cpp, "executable_path = DereferenceExecutablePath("),
          "Apple 分支用解链后的 exe 路径（不是 argv[0] 的原样父目录）");
    Check(CodeLineFind(command_cpp, "std::filesystem::canonical(path, ec)"),
          "解链用 canonical 走完整条 symlink 链（不是只解一层的 read_symlink）");
    Check(CodeLineFind(command_cpp, "return path;"),
          "解链失败时退回原路径（canonical 的另一份重载会抛异常，启动期抛了就是崩）");

    // 顺序：解链必须发生在"拿 exe 父目录拼 ini"之前。只判"存在"是不够的 ——
    // 调用挪到后面去照样命中。
    {
        const size_t deref  = CodeLineFindPosition(command_cpp, "executable_path = DereferenceExecutablePath(");
        const size_t config = CodeLineFindPosition(command_cpp, "config_file_path = executable_path.parent_path()");
        Check(deref != std::string::npos && config != std::string::npos, "能定位到解链点和 ini 拼接点");
        Check(deref != std::string::npos && config != std::string::npos && deref < config,
              "解链排在拼 ini 之前（否则拼的还是 bundle 内部的目录）");
    }

    // 崩之前必须留下线索：BqLog 是异步的，不刷这一下，唯一说清楚"到底去哪儿找过
    // ini"的那行日志会随缓冲区一起消失，而双击启动时没有终端可看。
    Check(FoundAfter(config_cpp, "cannot open config file", "ZEngineForceFlushLogs()", 600),
          "ini 打不开时先刷日志再返回 false（双击没有终端，日志是唯一线索）");

    // ---- 8. .app bundle 的三件事：icns、写进 plist、真身 symlink ----
    Check(CodeLineFind(bundle_cmake, "iconutil"), "bundle 里真的生成 .icns");
    Check(CodeLineFind(bundle_cmake, "CFBundleIconFile"), "Info.plist 写了 CFBundleIconFile（Dock / 访达的图标来源）");
    Check(CodeLineFind(bundle_cmake, "create_symlink"), "Contents/MacOS 是可执行文件的 symlink，不是拷贝");
    Check(CodeLineFind(bundle_cmake, "../../../${EXE_FILE}"),
          "symlink 是相对路径（bin/ 被拷走 / 换机器都不会失效）");

    // ---- 9. 从 bundle 里启动时让位给 bundle 自己的图标 ----
    //
    // 不然 Launcher 会拿同一张 PNG 去 setApplicationIconImage: 把 icns 盖掉 ——
    // 视觉上没区别，但那样 icns 到底生效了没有就永远不可证伪。
    Check(CodeLineFind(launcher_mac, "objectForInfoDictionaryKey:@\"CFBundleIconFile\""),
          "Launcher 认得 bundle 自己的图标配置");
    {
        const size_t bundle_check = CodeLineFindPosition(launcher_mac, "objectForInfoDictionaryKey:@\"CFBundleIconFile\"");
        const size_t png_override = CodeLineFindPosition(launcher_mac, "setApplicationIconImage:");
        Check(bundle_check != std::string::npos && png_override != std::string::npos,
              "能定位到 bundle 让位分支和 PNG 兜底");
        Check(bundle_check != std::string::npos && png_override != std::string::npos && bundle_check < png_override,
              "bundle 让位排在 PNG 兜底之前（否则 icns 永远被盖掉）");

        // 让位分支还必须真的 return —— 顺序对了但漏了 return，流程照样会掉下去
        // 执行 PNG 覆盖，icns 还是被盖。顺序判据抓不到这个。
        const std::string from_bundle_check =
            (bundle_check == std::string::npos) ? std::string() : launcher_mac.substr(bundle_check, 700);
        Check(!from_bundle_check.empty() && from_bundle_check.find("return;") != std::string::npos,
              "bundle 让位分支真的 return（不然流程照样掉到 PNG 覆盖上）");
    }

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

        // 位置版扫描器同样不许被注释糊弄 —— 上面的"解链排在拼 ini 之前""让位排在
        // PNG 兜底之前"两条判据都靠它。
        const std::string only_comment = "//     executable_path = DereferenceExecutablePath(x);\n    int y = 0;\n";
        Check(CodeLineFindPosition(only_comment, "executable_path = DereferenceExecutablePath(") == std::string::npos,
              "自检：位置扫描器跳过整行注释");

        const std::string two_lines = "    int a = 0;\n    executable_path = DereferenceExecutablePath(x);\n";
        Check(CodeLineFindPosition(two_lines, "executable_path = DereferenceExecutablePath(") != std::string::npos,
              "自检：位置扫描器能定位真实代码行");
    }

    std::fprintf(stderr, "ZEditorLauncherWiringSmokeTest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
