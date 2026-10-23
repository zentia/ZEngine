#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
    #include <ZAppShell/AppMain.h>  // CommandLine (wide argv, owns its memory)
    #include <shellapi.h>
    #include <windows.h>
#endif

#ifdef __APPLE__
    #include <crt_externs.h>  // _NSGetEnviron()
    #include <mach-o/dyld.h>  // _NSGetExecutablePath()
    #include <spawn.h>        // posix_spawn / POSIX_SPAWN_SETEXEC
#endif

#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/Launcher/ZEditorLauncher.h"
#include "Editor/RegisterEditor.h"
#include "Editor/ToolSuite/ToolSuite.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Core/Base/CrashHandler.h"
#include "Runtime/Function/Console/ConsoleManager.h"

// https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html
#define ZENGINE_XSTR(s) ZENGINE_STR(s)
#define ZENGINE_STR(s)  #s

namespace
{

    enum class StartupMode
    {
        OpenProject,
        OpenLauncher,
        Abort
    };

    // ---- `-batchmode`：不弹窗、不启动引擎，直接跑某个工具套件的命令行分支 ----
    //
    // 分流点必须在这里（RegisterRuntime() 之前）。START_SYSTEM() 会拉起 RHI、
    // 世界、资源系统，一个"读文件 → 算 → 写文件"的烘焙器全都不需要它们；
    // 让它们初始化只是白白拖慢 CI，而且万一某个系统初始化失败，一个跟它毫无
    // 关系的导出任务会跟着失败。
    struct BatchRequest
    {
        bool                     enabled  = false;
        std::string              suite_id;
        // `-batchmode` 之后的参数。套件自己负责解析；`-suite=` 可能也在这段里
        // （套件会跳过它，见 SpineCookSuite::RunBatch）。
        std::vector<std::string> args;
        // 没有 -batchmode 时，`-suite=` 之后的部分 —— 交互分支的参数。
        std::vector<std::string> suite_args;
    };

    constexpr const char* const kSuitePrefix = "-suite=";

    void ScanBatchRequest(const std::vector<std::string>& argv, BatchRequest& out)
    {
        out = BatchRequest{};
        for (size_t i = 1; i < argv.size(); ++i)
        {
            if (argv[i].rfind(kSuitePrefix, 0) == 0)
            {
                out.suite_id   = argv[i].substr(std::strlen(kSuitePrefix));
                out.suite_args.assign(argv.begin() + static_cast<long>(i) + 1, argv.end());
            }
        }
        for (size_t i = 1; i < argv.size(); ++i)
        {
            if (argv[i] == "-batchmode" || argv[i] == "--batchmode")
            {
                out.enabled = true;
                out.args.assign(argv.begin() + static_cast<long>(i) + 1, argv.end());
                break;
            }
        }
    }

    void PrintAvailableSuites(FILE* stream)
    {
        std::fprintf(stream, "  可用套件：\n");
        const ZEditor::ToolSuiteDesc& level = ZEditor::LevelEditorSuiteDesc();
        std::fprintf(stream,
                     "    %-14s %s%s\n",
                     level.id.c_str(),
                     level.display_name.c_str(),
                     level.supports_batch ? "" : "  (不支持 -batchmode)");
        for (const ZEditor::IToolSuite* suite : ZEditor::AllToolSuites())
        {
            const ZEditor::ToolSuiteDesc& d = suite->Desc();
            std::fprintf(stream,
                         "    %-14s %s%s\n",
                         d.id.c_str(),
                         d.display_name.c_str(),
                         d.supports_batch ? "" : "  (不支持 -batchmode)");
        }
    }

    int RunBatchMode(const BatchRequest& batch)
    {
#ifdef _WIN32
        // ZEditor 是 /SUBSYSTEM:WINDOWS，printf 默认没有地方可去。挂到调用方
        // 的控制台上（没有就新建一个）—— 否则"跑了但看不到输出"跟"挂了"没法区分。
        zengine::appshell::EnsureConsole();
#endif
        if (batch.suite_id.empty())
        {
            std::fprintf(stderr, "-batchmode 需要指定 -suite=<id>。\n\n");
            PrintAvailableSuites(stderr);
            return 1;
        }

        ZEditor::IToolSuite* suite = ZEditor::FindToolSuite(batch.suite_id);
        if (suite == nullptr)
        {
            std::fprintf(stderr, "没有这个工具套件：%s\n\n", batch.suite_id.c_str());
            PrintAvailableSuites(stderr);
            // 关卡编辑器有 desc 但没有 IToolSuite：它要跑 START_SYSTEM() 那一
            // 整套，正好是 -batchmode 想绕开的东西。单独说清楚，别让人以为
            // 拼错了 id。
            if (batch.suite_id == ZEditor::LevelEditorSuiteDesc().id)
            {
                std::fprintf(stderr, "\n关卡编辑器不支持 -batchmode（它就是要启动整个引擎）。\n");
            }
            return 1;
        }

        if (!suite->Desc().supports_batch)
        {
            std::fprintf(stderr, "工具套件 '%s' 不支持 -batchmode。\n", batch.suite_id.c_str());
            return 1;
        }

        return suite->RunBatch(batch.args);
    }

    StartupMode resolveStartupModeFromProjectPath(const std::filesystem::path& project_path, bool has_project_argument)
    {
        if (!has_project_argument || project_path.empty())
        {
            return StartupMode::OpenLauncher;
        }

        return std::filesystem::exists(project_path) ? StartupMode::OpenProject : StartupMode::OpenLauncher;
    }

    // The standalone ZLauncher (an ImGui project-picker) was removed when ImGui
    // was dropped from the engine. The editor now requires an explicit project:
    // launch it with `-p <path-to-.zproject>`. Without one we report and exit.
    int reportMissingProject()
    {
        std::cerr << "ZEditor: no project specified.\n"
                  << "Launch with: ZEditor -p <path-to-.zproject>\n";
        LOG_ERROR(ZEditor, "No project specified. Launch ZEditor with -p <path-to-.zproject>.");
        LogSystem::ForceFlush();
        return 1;
    }

    std::filesystem::path resolveWorkingDirectory(const std::filesystem::path& project_path)
    {
        return std::filesystem::is_directory(project_path) ? project_path : project_path.parent_path();
    }

    void trySetWorkingDirectory(const std::filesystem::path& working_dir)
    {
        try
        {
            std::filesystem::current_path(working_dir);
            LOG_INFO(ZEditor, "Working directory set to: {}", working_dir.generic_string());
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            LOG_ERROR(ZEditor,
                      "Failed to set working directory to {}: {}",
                      working_dir.generic_string(),
                      Encoding::GetFilesystemErrorMessage(e));
        }
    }

    std::filesystem::path resolveConfigFilePath(const std::filesystem::path& executable_path,
                                                const std::string& config_file_path_str)
    {
        if (!config_file_path_str.empty())
        {
            return config_file_path_str;
        }

        return executable_path.parent_path() / "ZEditor.ini";
    }

#ifdef _WIN32
    // `--pie-move-test`: set during command line parsing, consumed by
    // runEditorWin32(). Kept file-static so the several
    // resolveStartupModeFromCommandLine/runEditor* overloads (Win32, Unix, argv
    // based) do not all need a new parameter threaded through them.
    bool g_pie_move_test = false;
#endif

    // `--cvar name=value` (repeatable): console variables to set once every system
    // has started, i.e. before the first frame. Same reason as g_pie_move_test for
    // being file-static. Exists so a run can be configured without the console
    // window -- e.g. `--cvar r.Lumen.Enable=0`, because Lumen's CPU distance-field
    // voxelizer stalls a Debug editor for minutes on the first frames.
    std::vector<std::string> g_startup_cvars;

    void applyStartupConsoleVariables()
    {
        if (g_startup_cvars.empty())
        {
            return;
        }
        ConsoleManager* console = GET_SYSTEM(ConsoleManager);
        if (console == nullptr)
        {
            LOG_ERROR(ZEditor, "--cvar: ConsoleManager is not running; {} startup cvar(s) ignored",
                      g_startup_cvars.size());
            return;
        }
        for (const std::string& assignment : g_startup_cvars)
        {
            // ExecuteString understands `name=value` (and plain console commands);
            // it logs the outcome, including an unknown name, itself.
            if (!console->ExecuteString(assignment))
            {
                LOG_ERROR(ZEditor, "--cvar: '{}' was not applied", assignment);
            }
        }
    }

#ifdef _WIN32

    StartupMode resolveStartupModeFromCommandLine(PreferredRHI& preferred_rhi, float& pie_seconds, bool& pie_stop)
    {
        const auto commandLine = zengine::appshell::CommandLine::Acquire();
        if (!commandLine.valid())
        {
            return StartupMode::Abort;
        }
        const int argc = commandLine.count();
        const wchar_t* const* argv = commandLine.values();

        std::filesystem::path project_path;
        bool has_project_argument = false;
        preferred_rhi = PreferredRHI::Default;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg(argv[i]);
            if ((arg == L"-p" || arg == L"--project") && i + 1 < argc)
            {
                project_path = std::filesystem::path(argv[i + 1]);
                has_project_argument = true;
                ++i;
                continue;
            }
        if (arg == L"--pie" && i + 1 < argc)
        {
            pie_seconds = std::wcstof(argv[i + 1], nullptr);
            ++i;
            continue;
        }
        if (arg == L"--pie-stop")
        {
            pie_stop = true;
            continue;
        }
        if (arg == L"--pie-move-test")
        {
            g_pie_move_test = true;
            continue;
        }
            if (arg == L"--cvar" && i + 1 < argc)
            {
                g_startup_cvars.push_back(ZEditor::PathToUtf8(std::filesystem::path(argv[i + 1])));
                ++i;
                continue;
            }
            if (arg == L"--rhi" && i + 1 < argc)
            {
                const std::wstring_view value(argv[i + 1]);
                if (value == L"vulkan" || value == L"vk" || value == L"Vulkan")
                {
                    preferred_rhi = PreferredRHI::Vulkan;
                }
                else if (value == L"dx12" || value == L"d3d12" || value == L"directx12" || value == L"DX12")
                {
                    preferred_rhi = PreferredRHI::DX12;
                }
                // unknown values silently fall back to Default
                ++i;
                continue;
            }
        }

        // No LocalFree: commandLine owns the array and frees it on scope exit.
        return resolveStartupModeFromProjectPath(project_path, has_project_argument);
    }

    int runEditorWin32(PreferredRHI preferred_rhi, float pie_seconds, bool pie_stop)
    {
        RegisterRuntime(preferred_rhi);
        RegisterEditor();
        START_SYSTEM();
        applyStartupConsoleVariables();

        // `--pie <seconds>`: unattended PIE smoke run. The editor has no stdin
        // console and F5 needs a focused, interactive window, so headless
        // validation has no other way in. Enters play mode, runs for the given
        // wall-clock duration, then shuts down normally (which also exercises
        // ExitPIE's level reload).
        if (pie_seconds > 0.0f)
        {
            if (Editor* editor = GET_SYSTEM(Editor))
            {
                // Only register the request: the startup level is opened later
                // (EditorSceneManager::Initialize), so TogglePlayMode() here
                // would hit BeginPIE with no active level. Editor::Run() enters
                // play mode on the first frame where the level is loaded.
                LOG_INFO(ZEditor, "--pie: queued play mode for {:.1f}s once the level is ready", pie_seconds);
                editor->RequestPieOnLevelReady(pie_seconds);
                if (pie_stop)
                {
                    LOG_INFO(ZEditor, "--pie-stop: will leave play mode normally after {:.1f}s", pie_seconds);
                    editor->RequestPieStopAtEnd();
                }
                if (g_pie_move_test)
                {
                    LOG_INFO(ZEditor, "--pie-move-test: will inject a forward command and assert movement");
                    editor->RequestPieMoveTest();
                }
            }
        }

        // Remember the project so it shows up in the launcher next time. Needed
        // because a project can be opened without ever going through the picker:
        // double clicking a .zproject (once associated) or -p on the command
        // line. Only recorded when it actually loaded.
        if (ProjectInfo* project = GET_SYSTEM(ProjectInfo); project != nullptr && project->IsValid())
        {
            ZEditorLauncher::RememberProject(project->project_file);
        }

        GET_SYSTEM(Editor)->Run(pie_seconds);
        SHUTDOWN_SYSTEM();
        return 0;
    }

    // Show the suite + project picker, then dispatch.
    //
    // The level editor relaunches rather than carrying on in-process:
    // CommandSystem reads the project out of argv during START_SYSTEM(), so
    // handing it one here would mean a second, parallel way to set it.
    // ShellExecute restarts the same exe (GetModuleFileName, not a hardcoded
    // path) with a quoted argument -- project paths contain spaces often enough
    // that this matters.
    //
    // A tool suite runs in-process instead: it does not start the engine, so
    // there is nothing to re-initialise and nothing for a second process to buy.
    int runLauncherAndDispatch()
    {
        ZEditorLauncher::LaunchChoice choice;
        if (!ZEditorLauncher::Run(choice))
        {
            return reportMissingProject();
        }

        if (choice.suite_id != ZEditor::LevelEditorSuiteDesc().id)
        {
            if (ZEditor::IToolSuite* suite = ZEditor::FindToolSuite(choice.suite_id))
            {
                return suite->RunInteractive(choice.project_file, std::vector<std::string>());
            }
            std::cerr << "ZEditor: 没有这个工具套件：" << choice.suite_id << "\n";
            LOG_ERROR(ZEditor, "Unknown tool suite '{}' chosen in the launcher.", choice.suite_id);
            LogSystem::ForceFlush();
            return 1;
        }

        const std::filesystem::path& project_file = choice.project_file;

        wchar_t exe_path[4096] = {};
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));

        const std::wstring arguments = L"-p \"" + project_file.wstring() + L"\"";
        const HINSTANCE    launched =
            ShellExecuteW(nullptr, L"open", exe_path, arguments.c_str(), nullptr, SW_SHOW);

        // ShellExecute reports failure through any value <= 32, not NULL.
        if (reinterpret_cast<INT_PTR>(launched) <= 32)
        {
            LOG_ERROR(ZEditor, "Failed to relaunch ZEditor with project: {}", project_file.string());
            LogSystem::ForceFlush();
            MessageBoxW(nullptr, L"无法启动 ZEditor。", L"ZEditor", MB_OK | MB_ICONERROR);
            return 1;
        }

        return 0;
    }
#else
    StartupMode resolveStartupModeFromCommandLine(int argc, char** argv, std::filesystem::path& project_path,
                                                  PreferredRHI& preferred_rhi, float& pie_seconds, bool& pie_stop)
    {
        bool has_project_argument = false;
        preferred_rhi = PreferredRHI::Default;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);
            if ((arg == "-p" || arg == "--project") && i + 1 < argc)
            {
                project_path = std::filesystem::path(argv[i + 1]);
                has_project_argument = true;
                ++i;
                continue;
            }
            if (arg == "--rhi" && i + 1 < argc)
            {
                const std::string_view value(argv[i + 1]);
                if (value == "vulkan" || value == "vk" || value == "Vulkan")
                {
                    preferred_rhi = PreferredRHI::Vulkan;
                }
                else if (value == "dx12" || value == "d3d12" || value == "directx12" || value == "DX12")
                {
                    preferred_rhi = PreferredRHI::DX12;
                }
                ++i;
                continue;
            }
            if (arg == "--pie" && i + 1 < argc)
            {
                pie_seconds = std::strtof(argv[i + 1], nullptr);
                ++i;
                continue;
            }
            if (arg == "--pie-stop")
            {
                pie_stop = true;
                continue;
            }
            if (arg == "--cvar" && i + 1 < argc)
            {
                g_startup_cvars.emplace_back(argv[i + 1]);
                ++i;
                continue;
            }
        }

        return resolveStartupModeFromProjectPath(project_path, has_project_argument);
    }

    int runEditorUnix(const std::filesystem::path& project_path, PreferredRHI preferred_rhi, float pie_seconds,
                      bool pie_stop)
    {
        const std::filesystem::path working_dir = resolveWorkingDirectory(project_path);
        trySetWorkingDirectory(working_dir);

        RegisterRuntime(preferred_rhi);
        RegisterEditor();
        START_SYSTEM();
        applyStartupConsoleVariables();

        if (pie_seconds > 0.0f)
        {
            if (Editor* editor = GET_SYSTEM(Editor))
            {
                editor->RequestPieOnLevelReady(pie_seconds);
                if (pie_stop)
                {
                    editor->RequestPieStopAtEnd();
                }
            }
        }

        GET_SYSTEM(Editor)->Run(pie_seconds);

        // 记进 Launcher 的最近列表：项目完全可能没经过 Launcher 就打开了 ——
        // 命令行 -p，或者以后做了文件关联之后双击 .zproject。只在真的加载成功
        // 之后记（跟 Win32 分支同一条规则）。
        if (ProjectInfo* project = GET_SYSTEM(ProjectInfo); project != nullptr && project->IsValid())
        {
            ZEditorLauncher::RememberProject(project->project_file);
        }

        SHUTDOWN_SYSTEM();
        // Final safety: BqLog is asynchronous, drain any remaining records.
        LogSystem::ForceFlush();
        return 0;
    }

#ifdef __APPLE__
    // 重启自己并带上 -p <project>。
    //
    // 为什么必须重启：CommandSystem::Initialize() 是从 `_NSGetArgv()` 里解析 -p 的，
    // 进程内没有干净的途径改它（argv 那块内存是内核在 exec 时铺好的定长区域，
    // 往里塞东西是 UB）。Win32 那边用 ShellExecuteW 干的是同一件事。
    //
    // 为什么用 SETEXEC 而不是 fork+exec：Launcher 已经把 Cocoa/NSApplication 拉起来了，
    // fork 之后的子进程带着一份半初始化的 AppKit（Mach port、CoreFoundation run loop
    // 状态全都是脏的），是 macOS 上经典的"fork after Cocoa"崩溃源。SETEXEC 不走 fork，
    // 直接把当前进程映像换成新的 —— 干净，而且 PID 不变，Dock 里不会出现两个图标。
    // 重启时优先走 bundle 那个入口（如果有的话）。
    //
    // `_NSGetExecutablePath()` 给的是 symlink 解链后的真身 —— 也就是
    // bin/<Config>/ZEditor，`ZEditor.app/Contents/MacOS/ZEditor` 只是指向它的链接。
    // 直接 exec 真身的话，重启后的进程就不在 bundle 里了：Dock 图标从 icns 退回
    // PNG 兜底，LaunchServices 也不再把它认成同一个 App（下一次双击 .zproject 会
    // 另起一个实例而不是复用）。所以真身旁边有 bundle、且它的入口确实指向自己时，
    // 从 bundle 那个路径 exec —— 进程映像、PID 都不变，只是"身份"保住了。
    std::filesystem::path PickRelaunchExecutable(const std::filesystem::path& real_exe)
    {
        std::error_code ec;
        const std::filesystem::path exe_dir = real_exe.parent_path();
        const std::filesystem::path exe_name = real_exe.filename();

        for (std::filesystem::directory_iterator it(exe_dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_directory() || it->path().extension() != ".app")
            {
                continue;
            }

            const std::filesystem::path bundle_exe = it->path() / "Contents" / "MacOS" / exe_name;
            std::error_code             link_ec;
            const std::filesystem::path target = std::filesystem::canonical(bundle_exe, link_ec);
            if (!link_ec && target == real_exe)
            {
                return bundle_exe;
            }
        }

        return real_exe;
    }

    int relaunchWithProject(const std::filesystem::path& project_file)
    {
        char     raw_path[4096] = {};
        uint32_t size           = static_cast<uint32_t>(sizeof(raw_path));
        if (_NSGetExecutablePath(raw_path, &size) != 0)
        {
            LOG_ERROR(ZEditor, "Cannot resolve the executable path; relaunch aborted.");
            LogSystem::ForceFlush();
            return 1;
        }

        std::error_code ec;
        const std::filesystem::path real_exe = std::filesystem::absolute(std::filesystem::path(raw_path), ec);
        if (ec)
        {
            LOG_ERROR(ZEditor, "Cannot make '{}' absolute: {}", raw_path, ec.message());
            LogSystem::ForceFlush();
            return 1;
        }

        const std::filesystem::path exe = PickRelaunchExecutable(real_exe);
        LOG_INFO(ZEditor, "Relaunching with project '{}' via '{}'.", project_file.string(), exe.string());

        std::string exe_str = exe.string();
        std::string arg_str = project_file.string();
        std::string flag_str = "-p";

        // posix_spawn 的 argv/envp 要 char* 且必须活到调用结束：用 vector<char>
        // 持有可写缓冲，别把 std::string::c_str() 直接塞进去再改它。
        std::vector<char> exe_buf(exe_str.c_str(), exe_str.c_str() + exe_str.size() + 1);
        std::vector<char> arg_buf(arg_str.c_str(), arg_str.c_str() + arg_str.size() + 1);

        char* child_argv[] = { exe_buf.data(), flag_str.data(), arg_buf.data(), nullptr };

        posix_spawnattr_t attr;
        if (posix_spawnattr_init(&attr) != 0)
        {
            LOG_ERROR(ZEditor, "posix_spawnattr_init failed; relaunch aborted.");
            LogSystem::ForceFlush();
            return 1;
        }
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC);

        // SETEXEC 成功的话下面那一行之后进程映像就被换掉了，BqLog 缓冲区里还没落
        // 盘的东西（包括"是用 bundle 入口重启的"这条）会一起消失 —— 而这条日志正是
        // 判断重启走没走 bundle 的唯一证据。exec 之前必须刷。
        LogSystem::ForceFlush();

        char** const environ_ptr = *_NSGetEnviron();
        const int    rc = posix_spawn(nullptr, exe_buf.data(), nullptr, &attr, child_argv, environ_ptr);

        // 走到这儿说明 exec 失败了（成功的话映像已经换掉，本行不会执行）。
        posix_spawnattr_destroy(&attr);
        LOG_ERROR(ZEditor,
                  "Failed to relaunch ZEditor with project '{}': posix_spawn error {}",
                  arg_str,
                  rc);
        LogSystem::ForceFlush();
        std::fprintf(stderr,
                     "ZEditor: 无法用项目 \"%s\" 重启（posix_spawn 错误 %d）。\n"
                     "请手动启动：ZEditor -p \"%s\"\n",
                     arg_str.c_str(),
                     rc,
                     arg_str.c_str());
        return 1;
    }
#endif

    // 弹 Launcher，然后分流。
    //
    // 没有 -p 时的唯一出口。之前这里是 `reportMissingProject()`：往 stderr 打一行
    // 字再 return 1 —— 双击启动时 stderr 没有终端可去，进程静默退出 1，看上去就是
    // "闪退/崩溃"。有窗口才能把"ZEditor 需要一个项目"真正送到人眼前。
    int runLauncherAndDispatchUnix()
    {
        ZEditorLauncher::LaunchChoice choice;
        if (!ZEditorLauncher::Run(choice))
        {
            return reportMissingProject();
        }

        if (choice.suite_id != ZEditor::LevelEditorSuiteDesc().id)
        {
            if (ZEditor::IToolSuite* suite = ZEditor::FindToolSuite(choice.suite_id))
            {
                return suite->RunInteractive(choice.project_file, std::vector<std::string>());
            }
            std::cerr << "ZEditor: 没有这个工具套件：" << choice.suite_id << "\n";
            PrintAvailableSuites(stderr);
            return 1;
        }

#if defined(__APPLE__)
        return relaunchWithProject(choice.project_file);
#else
        // 这些平台上 Run() 只是个返回 false 的桩（见 ZEditorLauncher.cpp 末尾），
        // 所以这里到不了；留着是为了将来真做了原生 Launcher 时不用改 Main.cpp。
        return reportMissingProject();
#endif
    }
#endif
}  // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // 产品版本：优先用构建系统注入的宏，其次退回 CMake 的 project 版本。
    // 版本串只是人读标签 —— 真正的硬证据是快照里的 build_id（PDB GUID+Age）。
#ifdef ZENGINE_PRODUCT_VERSION
    InstallCrashHandler(ZENGINE_PRODUCT_VERSION);
#else
    InstallCrashHandler("0.1.0");
#endif

    // Initialize UTF-8 locale to handle filesystem error messages properly
    Encoding::InitializeUtf8Locale();

    // `-batchmode`：不弹窗、不启动引擎，直接跑某个工具套件的命令行分支。
    // 必须在任何引擎初始化之前分流（见 BatchRequest 的注释）。
    {
        const std::vector<std::string> argv_utf8 = zengine::appshell::CommandLine::Acquire().Utf8();
        BatchRequest                   batch;
        ScanBatchRequest(argv_utf8, batch);
        if (batch.enabled)
        {
            return RunBatchMode(batch);
        }
        // 只给了 -suite= 而没给 -batchmode：跑该套件的交互分支。
        if (!batch.suite_id.empty() && batch.suite_id != ZEditor::LevelEditorSuiteDesc().id)
        {
            if (ZEditor::IToolSuite* suite = ZEditor::FindToolSuite(batch.suite_id))
            {
                return suite->RunInteractive(std::filesystem::path(), batch.suite_args);
            }
            std::cerr << "ZEditor: 没有这个工具套件：" << batch.suite_id << "\n";
            PrintAvailableSuites(stderr);
            return 1;
        }
    }

    // Unity/Hub 风格：先决定本次启动是打开项目还是转到 Launcher，再进入编辑器初始化。
    PreferredRHI preferred_rhi = PreferredRHI::Default;
    float pie_seconds = 0.0f;
    bool pie_stop = false;
    switch (resolveStartupModeFromCommandLine(preferred_rhi, pie_seconds, pie_stop))
    {
        case StartupMode::OpenLauncher:
            return runLauncherAndDispatch();
        case StartupMode::Abort:
            return 1;
        case StartupMode::OpenProject:
            return runEditorWin32(preferred_rhi, pie_seconds, pie_stop);
    }

    return 1;
}
#else
int main(int argc, char** argv)
{
    // Initialize UTF-8 locale to handle filesystem error messages properly
    Encoding::InitializeUtf8Locale();

    {
        const std::vector<std::string> argv_utf8(argv, argv + argc);
        BatchRequest                   batch;
        ScanBatchRequest(argv_utf8, batch);
        if (batch.enabled)
        {
            return RunBatchMode(batch);
        }
        if (!batch.suite_id.empty() && batch.suite_id != ZEditor::LevelEditorSuiteDesc().id)
        {
            if (ZEditor::IToolSuite* suite = ZEditor::FindToolSuite(batch.suite_id))
            {
                return suite->RunInteractive(std::filesystem::path(), batch.suite_args);
            }
            std::cerr << "ZEditor: 没有这个工具套件：" << batch.suite_id << "\n";
            PrintAvailableSuites(stderr);
            return 1;
        }
    }

    std::filesystem::path project_path;
    PreferredRHI preferred_rhi = PreferredRHI::Default;
    float pie_seconds = 0.0f;
    bool pie_stop = false;
    switch (resolveStartupModeFromCommandLine(argc, argv, project_path, preferred_rhi, pie_seconds, pie_stop))
    {
        case StartupMode::OpenLauncher:
            return runLauncherAndDispatchUnix();
        case StartupMode::Abort:
            return 1;
        case StartupMode::OpenProject:
            return runEditorUnix(project_path, preferred_rhi, pie_seconds, pie_stop);
    }

    return 1;
}
#endif
