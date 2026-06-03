#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Command/CommandParser.h"
#ifdef _WIN32
    #include <shellapi.h>
    #include <windows.h>
#endif
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CommandSystem : public IEngineSystem
{
public:
    // 使用 AUTO_GET_CLASS_NAME() 自动获取类名，无需手动输入
    std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override { return {}; }

    std::filesystem::path& getConfigFilePath() { return config_file_path; }
    std::filesystem::path& GetWorkingDir() { return working_dir; }
    std::filesystem::path& GetProjectPath() { return project_path; }

    // ============================================
    // 注册命令行参数的接口（可扩展）
    // ============================================

    // 注册一个命令行选项（option）
    // @param option_names 选项名称，支持多个名称用逗号分隔（如 "-c,--config"）
    // @param default_value 默认值（字符串形式）
    // @param description 选项描述
    // @return 返回 Option* 指针，可以进一步配置（如添加验证器）
    // 注意：现在可以在 Initialize() 之前调用，选项会在解析时注册
    Option* RegisterOption(const std::string& option_names,
                           const std::string& default_value = "",
                           const std::string& description = "");

    // 注册一个命令行选项（option），绑定到外部变量
    // @param option_names 选项名称，支持多个名称用逗号分隔（如 "-c,--config"）
    // @param variable 绑定到的变量引用
    // @param description 选项描述
    // @return 返回 Option* 指针，可以进一步配置
    template<typename T>
    Option* RegisterOption(const std::string& option_names, T& variable, const std::string& description = "");

    // 注册一个命令行标志（flag）
    // @param flag_names 标志名称，支持多个名称用逗号分隔（如 "--version,-v"）
    // @param description 标志描述
    // @return 返回 Option* 指针，可以进一步配置
    // 注意：现在可以在 Initialize() 之前调用，标志会在解析时注册
    Option* RegisterFlag(const std::string& flag_names, const std::string& description = "");

    // 注册一个命令行标志（flag），绑定到外部变量
    // @param flag_names 标志名称，支持多个名称用逗号分隔（如 "--version,-v"）
    // @param variable 绑定到的变量引用
    // @param description 标志描述
    // @return 返回 Option* 指针，可以进一步配置
    template<typename T>
    Option* RegisterFlag(const std::string& flag_names, T& variable, const std::string& description = "");

    // 注册一个初始化回调函数，用于在命令行解析前注册参数
    // 这个回调函数会在 Initialize() 中创建 CommandParser 后立即调用
    // @param callback 回调函数，接收 CommandParser* 作为参数
    void RegisterInitCallback(std::function<void(CommandParser*)> callback);

    // ============================================
    // 查询命令行参数的接口
    // ============================================

    // 获取命令行标志（flag）的值
    // @param flag_name 标志名称，支持短名称（如 "v"）或长名称（如 "version"），可以带或不带前缀 "-" 或 "--"
    // @return 如果标志存在返回 true，否则返回 false
    bool GetFlag(const std::string& flag_name) const;

    // 获取命令行选项（option）的值
    // @param option_name 选项名称，支持短名称（如 "c"）或长名称（如 "config"），可以带或不带前缀 "-" 或 "--"
    // @return 如果选项存在返回其值（std::optional<std::string>），否则返回 std::nullopt
    std::optional<std::string> GetOption(const std::string& option_name) const;

    // 获取 Option 指针（用于高级用法）
    // @param option_name 选项名称
    // @return 如果选项存在返回 Option*，否则返回 nullptr
    Option* GetOptionPtr(const std::string& option_name);
    const Option* GetOptionPtr(const std::string& option_name) const;

protected:
    bool Initialize() override;
    void Shutdown() override;

private:
#ifdef _WIN32
    LPWSTR* wargv {nullptr};
#endif
    std::filesystem::path config_file_path;
    std::filesystem::path working_dir;
    std::filesystem::path project_path;

    // CommandParser 实例（在 Initialize() 中创建）
    std::unique_ptr<CommandParser> parser;

    // 延迟注册的选项和标志（在 Initialize() 之前注册的）
    struct PendingRegistration
    {
        enum class Type
        {
            Option,
            Flag
        };
        Type type;
        std::string names;
        std::string default_value;
        std::string description;
        std::function<void(Option*)> callback;  // 用于绑定变量的回调
    };
    std::vector<PendingRegistration> pending_registrations;

    // 初始化回调函数列表
    std::vector<std::function<void(CommandParser*)>> init_callbacks;

    // 规范化参数名称（移除前缀，统一格式）
    std::string NormalizeName(const std::string& name) const;
};

// ============================================
// 模板方法实现（必须在头文件中）
// ============================================

template<typename T>
Option* CommandSystem::RegisterOption(const std::string& option_names, T& variable, const std::string& description)
{
    if (parser)
    {
        // 如果解析器已创建，直接注册并绑定变量
        if constexpr (std::is_same_v<T, std::string>)
        {
            return parser->AddOption(option_names, variable, description);
        }
        else
        {
            // 对于其他类型，使用内部存储，然后通过回调设置
            Option* opt = parser->AddOption(option_names, description);
            if (opt)
            {
                opt->SetVariableCallback([&variable](const std::vector<std::string>& results) {
                    if (!results.empty())
                    {
                        // 简化处理：只支持string类型
                        if constexpr (std::is_convertible_v<std::string, T>)
                        {
                            variable = T(results[0]);
                        }
                    }
                });
            }
            return opt;
        }
    }
    else
    {
        // 延迟注册
        PendingRegistration reg;
        reg.type = PendingRegistration::Type::Option;
        reg.names = option_names;
        reg.description = description;
        reg.callback = [&variable](Option* opt) {
            if (opt && opt->count() > 0)
            {
                if constexpr (std::is_same_v<T, std::string>)
                {
                    variable = opt->results()[0];
                }
                else if constexpr (std::is_convertible_v<std::string, T>)
                {
                    variable = T(opt->results()[0]);
                }
            }
        };
        pending_registrations.push_back(reg);
        return nullptr;  // 延迟注册时返回nullptr
    }
}

template<typename T>
Option* CommandSystem::RegisterFlag(const std::string& flag_names, T& variable, const std::string& description)
{
    if (parser)
    {
        // 如果解析器已创建，直接注册并绑定变量
        if constexpr (std::is_same_v<T, bool>)
        {
            return parser->AddFlag(flag_names, variable, description);
        }
        else
        {
            // 对于其他类型，使用内部存储，然后通过回调设置
            Option* opt = parser->AddFlag(flag_names, description);
            if (opt)
            {
                opt->SetVariableCallback([&variable](const std::vector<std::string>& results) {
                    if constexpr (std::is_same_v<T, bool>)
                    {
                        variable = !results.empty();
                    }
                });
            }
            return opt;
        }
    }
    else
    {
        // 延迟注册
        PendingRegistration reg;
        reg.type = PendingRegistration::Type::Flag;
        reg.names = flag_names;
        reg.description = description;
        reg.callback = [&variable](Option* opt) {
            if (opt)
            {
                if constexpr (std::is_same_v<T, bool>)
                {
                    variable = opt->is_present();
                }
            }
        };
        pending_registrations.push_back(reg);
        return nullptr;  // 延迟注册时返回nullptr
    }
}
