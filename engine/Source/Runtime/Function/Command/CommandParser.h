#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// 命令行选项类型
enum class OptionType
{
    Option,  // 需要值的选项（如 -c value）
    Flag     // 布尔标志（如 --version）
};

// 验证函数类型
using ValidatorFunc = std::function<bool(const std::string&)>;

// 命令行选项类（替代CLI::Option）
class Option
{
public:
    Option(OptionType type, const std::string& names, const std::string& description);

    // 获取选项类型
    OptionType get_type() const { return m_Type; }

    // 获取所有名称（短名称和长名称）
    const std::vector<std::string>& get_names() const { return m_Names; }

    // 获取描述
    const std::string& get_description() const { return m_Description; }

    // 设置默认值
    Option* DefaultStr(const std::string& default_value);
    const std::string& get_default() const { return m_DefaultValue; }

    // 设置值（解析时调用）
    void SetValue(const std::string& value);
    void SetPresent(bool present = true);
    void ClearResults();

    // 获取值
    const std::vector<std::string>& results() const { return m_Results; }
    size_t count() const { return m_Results.size(); }
    bool is_present() const { return m_Present; }

    // 添加验证器
    Option* Check(ValidatorFunc validator);

    // 验证值
    bool Validate(const std::string& value) const;

    // 设置变量绑定回调（解析后调用）
    void SetVariableCallback(std::function<void(const std::vector<std::string>&)> callback);
    void ApplyVariableBinding() const;

private:
    OptionType m_Type;
    std::vector<std::string> m_Names;  // 所有名称（如 ["-c", "--config"]）
    std::string m_Description;
    std::string m_DefaultValue;
    std::vector<std::string> m_Results;                                       // 解析后的值
    bool m_Present;                                                           // 是否在命令行中出现
    std::vector<ValidatorFunc> m_Validators;                                  // 验证器列表
    std::function<void(const std::vector<std::string>&)> m_VariableCallback;  // 变量绑定回调
};

// 命令行解析器类（替代CLI::App）
class CommandParser
{
public:
    CommandParser(const std::string& name, const std::string& description);

    // 添加选项
    Option* AddOption(const std::string& option_names, std::string& variable, const std::string& description = "");

    // 添加选项（使用内部存储）
    Option* AddOption(const std::string& option_names, const std::string& description = "");

    // 添加标志
    Option* AddFlag(const std::string& flag_names, bool& variable, const std::string& description = "");

    // 添加标志（使用内部存储）
    Option* AddFlag(const std::string& flag_names, const std::string& description = "");

    // 解析命令行参数
    int Parse(int argc, char** argv);

    // 获取选项（通过名称）
    Option* GetOptionNoThrow(const std::string& name);
    const Option* GetOptionNoThrow(const std::string& name) const;

    // 获取所有选项
    const std::vector<std::unique_ptr<Option>>& get_options() const { return m_Options; }

    // 获取应用名称和描述
    const std::string& get_name() const { return m_Name; }
    const std::string& get_description() const { return m_Description; }

    // 退出处理（用于错误情况）
    int exit(const std::exception& e) const;

private:
    std::string m_Name;
    std::string m_Description;
    std::vector<std::unique_ptr<Option>> m_Options;
    std::unordered_map<std::string, Option*> m_NameMap;  // 名称到选项的映射

    // 解析选项名称（如 "-c,--config" -> ["-c", "--config"]）
    std::vector<std::string> ParseNames(const std::string& names_str);

    // 规范化名称（移除前缀）
    std::string NormalizeName(const std::string& name) const;

    // 查找选项
    Option* FindOption(const std::string& name);
    const Option* FindOption(const std::string& name) const;

    // 查找选项，如果不存在则自动添加
    Option* FindOrAddOption(const std::string& name, bool has_value = false);
};