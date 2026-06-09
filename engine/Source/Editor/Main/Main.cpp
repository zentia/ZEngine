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
    #include <shellapi.h>
    #include <windows.h>
#endif

#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/RegisterEditor.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Core/Base/CrashHandler.h"

// https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html
#define ZENGINE_XSTR(s) ZENGINE_STR(s)
#define ZENGINE_STR(s)  #s

namespace
{
#ifdef _WIN32
    // TEMPORARY crash diagnostic: write a symbolized callstack to crash_stack.txt on any
    // unhandled SEH exception (covers render-thread access violations -- the filter is
    // process-wide and runs on the faulting thread). RelWithDebInfo PDBs sit next to the exe
    // so SymFromAddr resolves names. Remove once the resize crash is fixed.
    LONG WINAPI ZEngineCrashHandler(EXCEPTION_POINTERS* ep)
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);

        std::ofstream out("E:/Engine/ZEngine/crash_stack.txt", std::ios::trunc);
        if (out.is_open() && ep != nullptr && ep->ExceptionRecord != nullptr)
        {
            out << "ZEditor crash. ExceptionCode=0x" << std::hex << ep->ExceptionRecord->ExceptionCode
                << " at " << ep->ExceptionRecord->ExceptionAddress << std::dec
                << " tid=" << GetCurrentThreadId() << "\n\n";

            CONTEXT* ctx = ep->ContextRecord;
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = ctx->Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx->Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx->Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            for (int i = 0; i < 64; ++i)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, ctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                {
                    break;
                }
                if (frame.AddrPC.Offset == 0)
                {
                    break;
                }

                const DWORD64 addr = frame.AddrPC.Offset;
                char symbol_buffer[sizeof(SYMBOL_INFO) + 512] = {};
                SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement = 0;

                out << "[" << i << "] 0x" << std::hex << addr << std::dec << "  ";
                if (SymFromAddr(process, addr, &displacement, symbol))
                {
                    out << symbol->Name << " +0x" << std::hex << displacement << std::dec;
                }
                else
                {
                    out << "<no symbol>";
                }

                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD line_displacement = 0;
                if (SymGetLineFromAddr64(process, addr, &line_displacement, &line) && line.FileName != nullptr)
                {
                    out << "  (" << line.FileName << ":" << line.LineNumber << ")";
                }
                out << "\n";
            }
            out.flush();
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif

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
    StartupMode resolveStartupModeFromCommandLine(PreferredRHI& preferred_rhi)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return StartupMode::Abort;
        }

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

        LocalFree(argv);
        return resolveStartupModeFromProjectPath(project_path, has_project_argument);
    }

    int runEditorWin32(PreferredRHI preferred_rhi)
    {
        RegisterRuntime(preferred_rhi);
        RegisterEditor();
        START_SYSTEM();
        GET_SYSTEM(Editor)->Run();
        SHUTDOWN_SYSTEM();
        return 0;
    }
#else
    StartupMode resolveStartupModeFromCommandLine(int argc, char** argv, std::filesystem::path& project_path, PreferredRHI& preferred_rhi)
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
        }

        return resolveStartupModeFromProjectPath(project_path, has_project_argument);
    }

    int runEditorUnix(const std::filesystem::path& project_path, PreferredRHI preferred_rhi)
    {
        const std::filesystem::path working_dir = resolveWorkingDirectory(project_path);
        trySetWorkingDirectory(working_dir);

        RegisterRuntime(preferred_rhi);
        RegisterEditor();
        START_SYSTEM();
        GET_SYSTEM(Editor)->Run();
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
    InstallCrashHandler();

    // Initialize UTF-8 locale to handle filesystem error messages properly
    Encoding::InitializeUtf8Locale();

    // Unity/Hub 风格：先决定本次启动是打开项目还是转到 Launcher，再进入编辑器初始化。
    PreferredRHI preferred_rhi = PreferredRHI::Default;
    switch (resolveStartupModeFromCommandLine(preferred_rhi))
    {
        case StartupMode::OpenLauncher:
            return reportMissingProject();
        case StartupMode::Abort:
            return 1;
        case StartupMode::OpenProject:
            return runEditorWin32(preferred_rhi);
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
    switch (resolveStartupModeFromCommandLine(argc, argv, project_path, preferred_rhi))
    {
        case StartupMode::OpenLauncher:
            return reportMissingProject();
        case StartupMode::Abort:
            return 1;
        case StartupMode::OpenProject:
            return runEditorUnix(project_path, preferred_rhi);
    }

    return 1;
}
#endif
