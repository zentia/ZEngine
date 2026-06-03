#include "CommandSystem.h"

#include "Runtime/Platform/Encoding/EncodingUtils.h"

#include <memory>
#include <unordered_map>
#ifdef __APPLE__
extern "C"
{
    int* _NSGetArgc(void);
    char*** _NSGetArgv(void);
}
#endif

bool CommandSystem::Initialize()
{
    int argc = 0;
    std::vector<std::string> argv_strings;
    std::vector<char*> argv_ptrs;
    std::filesystem::path executable_path;

#ifdef _WIN32
    wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv)
    {
        return false;
    }

    argv_strings.reserve(argc);
    argv_ptrs.reserve(argc + 1);
    for (int i = 0; i < argc; ++i)
    {
        int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        if (utf8_size > 0)
        {
            std::string utf8_str(utf8_size - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, utf8_str.data(), utf8_size, nullptr, nullptr);
            argv_strings.push_back(std::move(utf8_str));
            argv_ptrs.push_back(const_cast<char*>(argv_strings.back().c_str()));
        }
    }

    char editor_path[MAX_PATH];
    DWORD path_length = GetModuleFileNameA(nullptr, editor_path, MAX_PATH);
    if (path_length == 0 || path_length >= MAX_PATH)
    {
        LocalFree(wargv);
        return false;
    }
    executable_path = std::filesystem::path(editor_path);
#elif defined(__APPLE__)
    int* apple_argc = _NSGetArgc();
    char*** apple_argv = _NSGetArgv();
    argc = (apple_argc != nullptr) ? *apple_argc : 0;
    char** raw_argv = (apple_argv != nullptr) ? *apple_argv : nullptr;

    argv_strings.reserve(argc > 0 ? argc : 1);
    argv_ptrs.reserve((argc > 0 ? argc : 1) + 1);
    if (raw_argv != nullptr && argc > 0)
    {
        for (int i = 0; i < argc; ++i)
        {
            argv_strings.emplace_back(raw_argv[i] != nullptr ? raw_argv[i] : "");
            argv_ptrs.push_back(argv_strings.back().data());
        }
    }
    else
    {
        argv_strings.emplace_back("ZEditor");
        argv_ptrs.push_back(argv_strings.back().data());
        argc = 1;
    }

    executable_path = std::filesystem::path(argv_strings.front());
    if (executable_path.is_relative())
    {
        executable_path = std::filesystem::absolute(executable_path);
    }
#else
    argv_strings.emplace_back("ZRuntime");
    argv_ptrs.push_back(argv_strings.back().data());
    argc = static_cast<int>(argv_strings.size());
    executable_path = std::filesystem::current_path() / "ZRuntime";
#endif

    argv_ptrs.push_back(nullptr);
    char** argv = argv_ptrs.data();

    // 创建 CommandParser 实例
    parser = std::make_unique<CommandParser>("ZEditor - ZEngine Editor", "ZEditor");

    // 处理延迟注册的选项和标志
    std::vector<std::function<void()>> post_parse_callbacks;
    for (auto& reg : pending_registrations)
    {
        Option* opt = nullptr;
        if (reg.type == PendingRegistration::Type::Option)
        {
            opt = parser->AddOption(reg.names, reg.description);
            if (!reg.default_value.empty() && opt)
            {
                opt->DefaultStr(reg.default_value);
            }
        }
        else
        {
            opt = parser->AddFlag(reg.names, reg.description);
        }
        // 保存回调以便解析后调用
        if (opt && reg.callback)
        {
            post_parse_callbacks.push_back([opt, callback = reg.callback]() { callback(opt); });
        }
    }
    pending_registrations.clear();

    // 执行所有注册的回调函数
    for (auto& callback : init_callbacks)
    {
        callback(parser.get());
    }

    // 注册内置的命令行参数
    std::string config_file_path_str;
    parser->AddOption("-c,--config", config_file_path_str, "Path to configuration file (ZEditor.ini)")
        ->DefaultStr("");

    std::string project_path_str;
    parser->AddOption("-p,--project", project_path_str, "Path to project directory")->DefaultStr("");

    std::string log_level = "info";
    Option* log_level_opt =
        parser->AddOption("-l,--log-level", log_level, "Log level (verbose, debug, info, warning, error, fatal)");
    log_level_opt->DefaultStr("info");
    log_level_opt->Check([](const std::string& value) {
        return value == "verbose" || value == "debug" || value == "info" || value == "warning" || value == "error" ||
               value == "fatal";
    });

    bool show_version = false;
    parser->AddFlag("--version", show_version, "Show version information");

    // Parse command line arguments
    int parse_result = parser->Parse(argc, argv);
    if (parse_result != 0)
    {
        LOG_ERROR(ZCommandSystem, "ZEditor - parse error");
#ifdef _WIN32
        LocalFree(wargv);
#endif
        return false;
    }

    // 执行延迟注册的回调（解析后）
    for (auto& callback : post_parse_callbacks)
    {
        callback();
    }

    // 变量已经通过绑定自动更新，无需手动获取

    // Handle version flag
    if (show_version)
    {
        LOG_ERROR(ZCommandSystem, "ZEditor - ZEngine Editor");
        LOG_ERROR(ZCommandSystem, "Version: 0.1.0");
#ifdef _WIN32
        LocalFree(wargv);
#endif
        return false;
    }

    // Determine configuration file path
    if (!config_file_path_str.empty())
    {
        config_file_path = config_file_path_str;
    }
    else
    {
        config_file_path = executable_path.parent_path() / "ZEditor.ini";
    }

    LOG_INFO(ZEditor, "Configuration file: {}", config_file_path.string());
    // Set working directory to the directory containing project_path_str
    project_path = project_path_str;
    if (!project_path.empty())
    {
        if (std::filesystem::is_directory(project_path))
        {
            working_dir = project_path;
        }
        else
        {
            working_dir = project_path.parent_path();
        }

        try
        {
            std::filesystem::current_path(working_dir);
            LOG_INFO(ZCommandSystem, "Working directory set to: {}", working_dir.generic_string());
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            LOG_ERROR(ZCommandSystem,
                      "Failed to set working directory to {}: {}",
                      working_dir.generic_string(),
                      Encoding::GetFilesystemErrorMessage(e));
        }
    }

    LOG_INFO(ZCommandSystem, "Log level: {}", log_level);
    return true;
}

void CommandSystem::Shutdown()
{
    parser.reset();
    pending_registrations.clear();
#ifdef _WIN32
    if (wargv)
    {
        LocalFree(wargv);
        wargv = nullptr;
    }
#endif
}

Option* CommandSystem::RegisterOption(const std::string& option_names,
                                      const std::string& default_value,
                                      const std::string& description)
{
    if (parser)
    {
        // 如果解析器已创建，直接注册
        Option* opt = parser->AddOption(option_names, description);
        if (opt && !default_value.empty())
        {
            opt->DefaultStr(default_value);
        }
        return opt;
    }
    else
    {
        // 延迟注册
        PendingRegistration reg;
        reg.type = PendingRegistration::Type::Option;
        reg.names = option_names;
        reg.default_value = default_value;
        reg.description = description;
        pending_registrations.push_back(reg);
        return nullptr;  // 延迟注册时返回nullptr，解析后会创建
    }
}

Option* CommandSystem::RegisterFlag(const std::string& flag_names, const std::string& description)
{
    if (parser)
    {
        // 如果解析器已创建，直接注册
        return parser->AddFlag(flag_names, description);
    }
    else
    {
        // 延迟注册
        PendingRegistration reg;
        reg.type = PendingRegistration::Type::Flag;
        reg.names = flag_names;
        reg.description = description;
        pending_registrations.push_back(reg);
        return nullptr;  // 延迟注册时返回nullptr，解析后会创建
    }
}

void CommandSystem::RegisterInitCallback(std::function<void(CommandParser*)> callback)
{
    if (parser)
    {
        // 如果 CommandParser 已经创建，立即执行回调
        callback(parser.get());
    }
    else
    {
        // 否则保存回调，在 Initialize() 中执行
        init_callbacks.push_back(callback);
    }
}

bool CommandSystem::GetFlag(const std::string& flag_name) const
{
    if (!parser)
    {
        return false;
    }

    std::string normalized = NormalizeName(flag_name);

    // 尝试多种格式
    std::vector<std::string> test_names = {normalized, "--" + normalized, "-" + normalized};

    for (const auto& name : test_names)
    {
        const Option* opt = parser->GetOptionNoThrow(name);
        if (opt && opt->is_present())
        {
            return true;
        }
    }

    return false;
}

std::optional<std::string> CommandSystem::GetOption(const std::string& option_name) const
{
    if (!parser)
    {
        return std::nullopt;
    }

    std::string normalized = NormalizeName(option_name);

    // 尝试多种格式
    std::vector<std::string> test_names = {normalized, "--" + normalized, "-" + normalized};

    for (const auto& name : test_names)
    {
        const Option* opt = parser->GetOptionNoThrow(name);
        if (opt && opt->count() > 0)
        {
            // 获取选项的结果（字符串形式）
            const auto& results = opt->results();
            if (!results.empty())
            {
                return results[0];
            }
        }
    }

    return std::nullopt;
}

Option* CommandSystem::GetOptionPtr(const std::string& option_name)
{
    if (!parser)
    {
        return nullptr;
    }

    std::string normalized = NormalizeName(option_name);
    std::vector<std::string> test_names = {normalized, "--" + normalized, "-" + normalized};

    for (const auto& name : test_names)
    {
        Option* opt = parser->GetOptionNoThrow(name);
        if (opt)
        {
            return opt;
        }
    }

    return nullptr;
}

const Option* CommandSystem::GetOptionPtr(const std::string& option_name) const
{
    if (!parser)
    {
        return nullptr;
    }

    std::string normalized = NormalizeName(option_name);
    std::vector<std::string> test_names = {normalized, "--" + normalized, "-" + normalized};

    for (const auto& name : test_names)
    {
        const Option* opt = parser->GetOptionNoThrow(name);
        if (opt)
        {
            return opt;
        }
    }

    return nullptr;
}

std::string CommandSystem::NormalizeName(const std::string& name) const
{
    std::string result = name;
    // 移除前缀 "-" 或 "--"
    while (!result.empty() && result[0] == '-')
    {
        result.erase(0, 1);
    }
    return result;
}