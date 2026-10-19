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

#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/Launcher/ZEditorLauncher.h"
#include "Editor/RegisterEditor.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Core/Base/CrashHandler.h"

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

    // Show the project picker, then relaunch ourselves with -p <project>.
    //
    // Relaunching rather than carrying on in-process: CommandSystem reads the
    // project out of argv during START_SYSTEM(), so handing it one here would
    // mean a second, parallel way to set it. ShellExecute restarts the same exe
    // (GetModuleFileName, not a hardcoded path) with a quoted argument -- project
    // paths contain spaces often enough that this matters.
    int runLauncherAndRelaunch()
    {
        std::filesystem::path project_file;
        if (!ZEditorLauncher::Run(project_file))
        {
            return reportMissingProject();
        }

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
        SHUTDOWN_SYSTEM();
        // Final safety: BqLog is asynchronous, drain any remaining records.
        LogSystem::ForceFlush();
        return 0;
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

    // Unity/Hub 风格：先决定本次启动是打开项目还是转到 Launcher，再进入编辑器初始化。
    PreferredRHI preferred_rhi = PreferredRHI::Default;
    float pie_seconds = 0.0f;
    bool pie_stop = false;
    switch (resolveStartupModeFromCommandLine(preferred_rhi, pie_seconds, pie_stop))
    {
        case StartupMode::OpenLauncher:
            return runLauncherAndRelaunch();
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

    std::filesystem::path project_path;
    PreferredRHI preferred_rhi = PreferredRHI::Default;
    float pie_seconds = 0.0f;
    bool pie_stop = false;
    switch (resolveStartupModeFromCommandLine(argc, argv, project_path, preferred_rhi, pie_seconds, pie_stop))
    {
        case StartupMode::OpenLauncher:
            return reportMissingProject();
        case StartupMode::Abort:
            return 1;
        case StartupMode::OpenProject:
            return runEditorUnix(project_path, preferred_rhi, pie_seconds, pie_stop);
    }

    return 1;
}
#endif
