#include "CommandParser.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <sstream>

// ============================================
// Option 实现
// ============================================

Option::Option(OptionType type, const std::string& names, const std::string& description)
    : m_Type(type), m_Description(description), m_Present(false)
{
    // 解析名称
    std::istringstream iss(names);
    std::string name;
    while (std::getline(iss, name, ','))
    {
        // 移除前后空格
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (!name.empty())
        {
            m_Names.push_back(name);
        }
    }
}

Option* Option::DefaultStr(const std::string& default_value)
{
    m_DefaultValue = default_value;
    if (!default_value.empty() && m_Results.empty())
    {
        m_Results.push_back(default_value);
    }
    return this;
}

void Option::SetValue(const std::string& value)
{
    m_Present = true;
    m_Results.push_back(value);
}

void Option::SetPresent(bool present)
{
    m_Present = present;
    if (present && m_Type == OptionType::Flag && m_Results.empty())
    {
        m_Results.push_back("true");
    }
}

void Option::ClearResults()
{
    m_Results.clear();
    m_Present = false;
    // 注意：不清除默认值，默认值会在解析时重新应用
}

Option* Option::Check(ValidatorFunc validator)
{
    if (validator)
    {
        m_Validators.push_back(validator);
    }
    return this;
}

bool Option::Validate(const std::string& value) const
{
    for (const auto& validator : m_Validators)
    {
        if (!validator(value))
        {
            return false;
        }
    }
    return true;
}

void Option::SetVariableCallback(std::function<void(const std::vector<std::string>&)> callback)
{
    m_VariableCallback = callback;
}

void Option::ApplyVariableBinding() const
{
    if (m_VariableCallback)
    {
        m_VariableCallback(m_Results);
    }
}

// ============================================
// CommandParser 实现
// ============================================

CommandParser::CommandParser(const std::string& name, const std::string& description)
    : m_Name(name), m_Description(description)
{
}

std::vector<std::string> CommandParser::ParseNames(const std::string& names_str)
{
    std::vector<std::string> names;
    std::istringstream iss(names_str);
    std::string name;
    while (std::getline(iss, name, ','))
    {
        // 移除前后空格
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (!name.empty())
        {
            names.push_back(name);
        }
    }
    return names;
}

std::string CommandParser::NormalizeName(const std::string& name) const
{
    std::string result = name;
    // 移除前缀 "-" 或 "--"
    while (!result.empty() && result[0] == '-')
    {
        result.erase(0, 1);
    }
    return result;
}

Option* CommandParser::FindOption(const std::string& name)
{
    auto it = m_NameMap.find(name);
    if (it != m_NameMap.end())
    {
        return it->second;
    }

    // 尝试规范化名称
    std::string normalized = NormalizeName(name);
    it = m_NameMap.find(normalized);
    if (it != m_NameMap.end())
    {
        return it->second;
    }

    // 尝试带前缀的名称
    if (name.size() > 2 && name[0] == '-' && name[1] == '-')
    {
        it = m_NameMap.find(name.substr(2));
        if (it != m_NameMap.end())
        {
            return it->second;
        }
    }
    else if (name.size() > 1 && name[0] == '-')
    {
        it = m_NameMap.find(name.substr(1));
        if (it != m_NameMap.end())
        {
            return it->second;
        }
    }

    return nullptr;
}

const Option* CommandParser::FindOption(const std::string& name) const
{
    return const_cast<CommandParser*>(this)->FindOption(name);
}

Option*
CommandParser::AddOption(const std::string& option_names, std::string& variable, const std::string& description)
{
    auto option = std::make_unique<Option>(OptionType::Option, option_names, description);
    Option* ptr = option.get();

    // 注册所有名称
    std::vector<std::string> names = ParseNames(option_names);
    for (const auto& name : names)
    {
        m_NameMap[NormalizeName(name)] = ptr;
        m_NameMap[name] = ptr;  // 也注册完整名称
    }

    // 绑定变量（解析后设置）
    ptr->SetVariableCallback([&variable](const std::vector<std::string>& results) {
        if (!results.empty())
        {
            variable = results[0];
        }
    });

    m_Options.push_back(std::move(option));
    return ptr;
}

Option* CommandParser::AddOption(const std::string& option_names, const std::string& description)
{
    auto option = std::make_unique<Option>(OptionType::Option, option_names, description);
    Option* ptr = option.get();

    // 注册所有名称
    std::vector<std::string> names = ParseNames(option_names);
    for (const auto& name : names)
    {
        m_NameMap[NormalizeName(name)] = ptr;
        m_NameMap[name] = ptr;  // 也注册完整名称
    }

    m_Options.push_back(std::move(option));
    return ptr;
}

Option* CommandParser::AddFlag(const std::string& flag_names, bool& variable, const std::string& description)
{
    auto option = std::make_unique<Option>(OptionType::Flag, flag_names, description);
    Option* ptr = option.get();

    // 注册所有名称
    std::vector<std::string> names = ParseNames(flag_names);
    for (const auto& name : names)
    {
        m_NameMap[NormalizeName(name)] = ptr;
        m_NameMap[name] = ptr;  // 也注册完整名称
    }

    // 绑定变量（解析后设置）
    ptr->SetVariableCallback([&variable](const std::vector<std::string>& results) { variable = !results.empty(); });

    m_Options.push_back(std::move(option));
    return ptr;
}

Option* CommandParser::AddFlag(const std::string& flag_names, const std::string& description)
{
    auto option = std::make_unique<Option>(OptionType::Flag, flag_names, description);
    Option* ptr = option.get();

    // 注册所有名称
    std::vector<std::string> names = ParseNames(flag_names);
    for (const auto& name : names)
    {
        m_NameMap[NormalizeName(name)] = ptr;
        m_NameMap[name] = ptr;  // 也注册完整名称
    }

    m_Options.push_back(std::move(option));
    return ptr;
}

Option* CommandParser::GetOptionNoThrow(const std::string& name)
{
    return FindOption(name);
}

const Option* CommandParser::GetOptionNoThrow(const std::string& name) const
{
    return FindOption(name);
}

Option* CommandParser::FindOrAddOption(const std::string& name, bool has_value)
{
    Option* opt = FindOption(name);
    if (opt)
    {
        return opt;
    }

    // 如果不存在，自动添加
    if (has_value)
    {
        // 有值，添加为 Option 类型
        return AddOption(name, "");
    }
    else
    {
        // 无值，添加为 Flag 类型
        return AddFlag(name, "");
    }
}

int CommandParser::Parse(int argc, char** argv)
{
    if (argc <= 0 || !argv)
    {
        return 0;
    }

    // 重置所有选项
    for (auto& option : m_Options)
    {
        option->ClearResults();
    }

    // 解析参数
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg.empty())
        {
            continue;
        }

        // 检查是否是选项或标志
        if (arg[0] == '-')
        {
            std::string option_name = arg;
            std::string value;

            // 处理 --option=value 格式
            size_t eq_pos = arg.find('=');
            if (eq_pos != std::string::npos)
            {
                option_name = arg.substr(0, eq_pos);
                value = arg.substr(eq_pos + 1);
            }

            // 判断是否有值（用于自动注册选项）
            bool has_value = !value.empty();
            if (!has_value && i + 1 < argc)
            {
                // 检查下一个参数是否是值（不是选项）
                std::string next_arg = argv[i + 1];
                if (!next_arg.empty() && next_arg[0] != '-')
                {
                    has_value = true;
                }
            }

            Option* opt = FindOrAddOption(option_name, has_value);
            if (opt)
            {
                if (opt->get_type() == OptionType::Flag)
                {
                    // 标志
                    opt->SetPresent(true);
                }
                else
                {
                    // 选项，需要值
                    if (!value.empty())
                    {
                        // 从 =value 中获取
                        if (!opt->Validate(value))
                        {
                            LOG_ERROR(ZCommandSystem, "Invalid value '{}' for option '{}'", value, option_name);
                            return 1;
                        }
                        opt->SetValue(value);
                    }
                    else
                    {
                        // 从下一个参数获取
                        if (i + 1 < argc)
                        {
                            std::string next_arg = argv[i + 1];
                            // 检查下一个参数是否是另一个选项
                            if (next_arg.empty() || next_arg[0] != '-')
                            {
                                if (!opt->Validate(next_arg))
                                {
                                    LOG_ERROR(
                                        ZCommandSystem, "Invalid value '{}' for option '{}'", next_arg, option_name);
                                    return 1;
                                }
                                opt->SetValue(next_arg);
                                ++i;  // 跳过下一个参数
                            }
                            else
                            {
                                LOG_ERROR(ZCommandSystem, "Option '{}' requires a value", option_name);
                                return 1;
                            }
                        }
                        else
                        {
                            LOG_ERROR(ZCommandSystem, "Option '{}' requires a value", option_name);
                            return 1;
                        }
                    }
                }
            }
        }
        else
        {
            // 位置参数，暂时忽略
        }
    }

    // 应用默认值（如果选项未出现）
    for (auto& option : m_Options)
    {
        if (!option->is_present() && !option->get_default().empty() && option->get_type() == OptionType::Option)
        {
            option->SetValue(option->get_default());
        }
    }

    // 应用变量绑定
    for (auto& option : m_Options)
    {
        option->ApplyVariableBinding();
    }

    return 0;
}

int CommandParser::exit(const std::exception& e) const
{
    LOG_ERROR(ZCommandSystem, "Parse error: {}", e.what());
    return 1;
}
