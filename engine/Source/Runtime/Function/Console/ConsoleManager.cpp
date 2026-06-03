#include "ConsoleManager.h"

#include "RuntimeConsoleCommands.h"

#include <algorithm>
#include <cctype>

std::string ConsoleManager::NormalizeKey(const std::string& name)
{
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

bool ConsoleManager::TrySetVariable(const std::string& var_name, const std::string& var_value)
{
    auto var = FindVariable(var_name);
    if (!var)
    {
        LOG_ERROR(ZConsole, "Variable '{}' not found", var_name);
        return false;
    }
    if (!var->setStringValue(var_value))
    {
        LOG_ERROR(ZConsole, "Failed to set {} = {} (invalid value)", var_name, var_value);
        return false;
    }
    LOG_INFO(ZConsole, "Set {} = {}", var->GetName(), var->getStringValue());
    return true;
}

bool ConsoleManager::TryPrintVariable(const std::string& var_name) const
{
    auto var = FindVariable(var_name);
    if (!var)
    {
        return false;
    }
    LOG_INFO(ZConsole, "{} = {} ({})", var->GetName(), var->getStringValue(), var->getDescription());
    return true;
}

bool ConsoleManager::Initialize()
{
    LOG_INFO(ZConsole, "ConsoleManager initialized");

    RegisterCommand("help",
                    "Show help for all commands/variables, or 'help <name>' for one entry",
                    [this](const std::vector<std::string>& args) -> bool {
                        if (args.empty())
                        {
                            PrintHelp();
                        }
                        else
                        {
                            PrintHelp(args[0]);
                        }
                        return true;
                    });

    RegisterCommand("history", "Show command history", [this](const std::vector<std::string>&) -> bool {
        LOG_INFO(ZConsole, "Command History ({} commands):", m_CommandHistory.size());
        for (size_t i = 0; i < m_CommandHistory.size(); ++i)
        {
            LOG_INFO(ZConsole, "  {}: {}", i + 1, m_CommandHistory[i]);
        }
        return true;
    });

    RegisterCommand("clear", "Clear command history", [this](const std::vector<std::string>&) -> bool {
        clearHistory();
        LOG_INFO(ZConsole, "Command history cleared");
        return true;
    });

    // UE IConsoleManager::Set / Get
    RegisterCommand("set",
                    "Set a console variable: set <Name> <Value>",
                    [this](const std::vector<std::string>& args) -> bool {
                        if (args.size() < 2)
                        {
                            LOG_ERROR(ZConsole, "Usage: set <Name> <Value>");
                            return false;
                        }
                        std::string value = args[1];
                        for (size_t i = 2; i < args.size(); ++i)
                        {
                            value += ' ';
                            value += args[i];
                        }
                        return TrySetVariable(args[0], value);
                    });

    RegisterCommand("get",
                    "Print a console variable: get <Name>",
                    [this](const std::vector<std::string>& args) -> bool {
                        if (args.empty())
                        {
                            LOG_ERROR(ZConsole, "Usage: get <Name>");
                            return false;
                        }
                        if (!TryPrintVariable(args[0]))
                        {
                            LOG_ERROR(ZConsole, "Variable '{}' not found", args[0]);
                            return false;
                        }
                        return true;
                    });

    RegisterRuntimeConsoleCommands(*this);
    return true;
}

void ConsoleManager::Shutdown()
{
    m_Variables.clear();
    m_Commands.clear();
    m_CommandHistory.clear();
    LOG_INFO(ZConsole, "ConsoleManager shutdown");
}

std::shared_ptr<ConsoleVariable<int>> ConsoleManager::RegisterIntVariable(const std::string& name,
                                                                          const std::string& description,
                                                                          int initial_value,
                                                                          int* storage)
{
    auto var = std::make_shared<ConsoleVariable<int>>(name, description, initial_value, storage);
    m_Variables[NormalizeKey(name)] = var;
    LOG_DEBUG(ZConsole, "Registered int variable: {} = {}", name, initial_value);
    return var;
}

std::shared_ptr<ConsoleVariable<float>> ConsoleManager::RegisterFloatVariable(const std::string& name,
                                                                              const std::string& description,
                                                                              float initial_value,
                                                                              float* storage)
{
    auto var = std::make_shared<ConsoleVariable<float>>(name, description, initial_value, storage);
    m_Variables[NormalizeKey(name)] = var;
    LOG_DEBUG(ZConsole, "Registered float variable: {} = {}", name, initial_value);
    return var;
}

std::shared_ptr<ConsoleVariable<bool>> ConsoleManager::RegisterBoolVariable(const std::string& name,
                                                                            const std::string& description,
                                                                            bool initial_value,
                                                                            bool* storage)
{
    auto var = std::make_shared<ConsoleVariable<bool>>(name, description, initial_value, storage);
    m_Variables[NormalizeKey(name)] = var;
    LOG_DEBUG(ZConsole, "Registered bool variable: {} = {}", name, initial_value);
    return var;
}

std::shared_ptr<ConsoleVariable<std::string>> ConsoleManager::RegisterStringVariable(const std::string& name,
                                                                                     const std::string& description,
                                                                                     const std::string& initial_value,
                                                                                     std::string* storage)
{
    auto var = std::make_shared<ConsoleVariable<std::string>>(name, description, initial_value, storage);
    m_Variables[NormalizeKey(name)] = var;
    LOG_DEBUG(ZConsole, "Registered string variable: {} = {}", name, initial_value);
    return var;
}

std::shared_ptr<IConsoleVariable> ConsoleManager::FindVariable(const std::string& name) const
{
    auto it = m_Variables.find(NormalizeKey(name));
    if (it != m_Variables.end())
    {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<IConsoleCommand>
ConsoleManager::RegisterCommand(const std::string& name,
                                const std::string& description,
                                std::function<bool(const std::vector<std::string>&)> delegate)
{
    auto cmd = std::make_shared<ConsoleCommand>(name, description, delegate);
    m_Commands[NormalizeKey(name)] = cmd;
    LOG_DEBUG(ZConsole, "Registered command: {}", name);
    return cmd;
}

std::shared_ptr<IConsoleCommand> ConsoleManager::FindCommand(const std::string& name) const
{
    auto it = m_Commands.find(NormalizeKey(name));
    if (it != m_Commands.end())
    {
        return it->second;
    }
    return nullptr;
}

bool ConsoleManager::ExecuteString(const std::string& command_line)
{
    if (command_line.empty())
    {
        return false;
    }

    auto args = ParseCommandLine(command_line);
    if (args.empty())
    {
        return false;
    }

    AddToHistory(command_line);

    const std::string& command_name = args[0];
    std::vector<std::string> command_args(args.begin() + 1, args.end());

    // name=value on first token (UE: r.MaxFPS=60)
    size_t equal_pos = command_name.find('=');
    if (equal_pos != std::string::npos)
    {
        std::string var_name = command_name.substr(0, equal_pos);
        std::string var_value = command_name.substr(equal_pos + 1);
        if (var_value.size() >= 2 && var_value.front() == '"' && var_value.back() == '"')
        {
            var_value = var_value.substr(1, var_value.size() - 2);
        }
        return TrySetVariable(var_name, var_value);
    }

    // Built-in set/get are handled by registered commands below.

    auto cmd = FindCommand(command_name);
    if (cmd)
    {
        const bool success = cmd->Execute(command_args);
        if (!success)
        {
            LOG_ERROR(ZConsole, "Command '{}' execution failed", command_name);
        }
        return success;
    }

    // UE: "r.MaxFPS 60" (variable then value, no '=')
    if (command_args.size() == 1)
    {
        if (FindVariable(command_name))
        {
            return TrySetVariable(command_name, command_args[0]);
        }
    }

    // Bare variable name prints current value.
    if (command_args.empty() && TryPrintVariable(command_name))
    {
        return true;
    }

    LOG_ERROR(ZConsole, "Unknown command or variable: '{}'", command_name);
    LOG_INFO(ZConsole, "Type 'help' for available commands and variables");
    return false;
}

void ConsoleManager::AddToHistory(const std::string& command)
{
    if (!m_CommandHistory.empty() && m_CommandHistory.back() == command)
    {
        return;
    }

    m_CommandHistory.push_back(command);
    if (m_CommandHistory.size() > k_max_history_size)
    {
        m_CommandHistory.erase(m_CommandHistory.begin());
    }
}

std::vector<std::string> ConsoleManager::GetAutoCompleteList(const std::string& prefix) const
{
    std::vector<std::string> results;
    const std::string prefix_key = NormalizeKey(prefix);

    for (const auto& [name, cmd] : m_Commands)
    {
        if (name.find(prefix_key) == 0)
        {
            results.push_back(cmd->GetName());
        }
    }

    for (const auto& [name, var] : m_Variables)
    {
        if (name.find(prefix_key) == 0)
        {
            results.push_back(var->GetName());
        }
    }

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}

void ConsoleManager::PrintHelp() const
{
    LOG_INFO(ZConsole, "=== Console Commands ===");
    for (const auto& [name, cmd] : m_Commands)
    {
        (void)name;
        LOG_INFO(ZConsole, "  {} - {}", cmd->GetName(), cmd->getDescription());
    }

    LOG_INFO(ZConsole, "=== Console Variables ===");
    for (const auto& [name, var] : m_Variables)
    {
        (void)name;
        const char* type_str = "";
        switch (var->getType())
        {
            case ConsoleVariableType::Int:
                type_str = "int";
                break;
            case ConsoleVariableType::Float:
                type_str = "float";
                break;
            case ConsoleVariableType::Bool:
                type_str = "bool";
                break;
            case ConsoleVariableType::String:
                type_str = "string";
                break;
        }
        LOG_INFO(ZConsole, "  {} ({}) = {} - {}", var->GetName(), type_str, var->getStringValue(), var->getDescription());
    }
}

void ConsoleManager::PrintHelp(const std::string& name) const
{
    auto cmd = FindCommand(name);
    if (cmd)
    {
        LOG_INFO(ZConsole, "Command: {}", cmd->GetName());
        LOG_INFO(ZConsole, "  Description: {}", cmd->getDescription());
        return;
    }

    auto var = FindVariable(name);
    if (var)
    {
        const char* type_str = "";
        switch (var->getType())
        {
            case ConsoleVariableType::Int:
                type_str = "int";
                break;
            case ConsoleVariableType::Float:
                type_str = "float";
                break;
            case ConsoleVariableType::Bool:
                type_str = "bool";
                break;
            case ConsoleVariableType::String:
                type_str = "string";
                break;
        }
        LOG_INFO(ZConsole, "Variable: {} ({})", var->GetName(), type_str);
        LOG_INFO(ZConsole, "  Current Value: {}", var->getStringValue());
        LOG_INFO(ZConsole, "  Description: {}", var->getDescription());
        LOG_INFO(ZConsole, "  Usage: set {} <value>  |  {} = <value>  |  {} <value>", var->GetName(), var->GetName(),
                 var->GetName());
        return;
    }

    LOG_ERROR(ZConsole, "Command or variable '{}' not found", name);
}

std::vector<std::string> ConsoleManager::ParseCommandLine(const std::string& command_line) const
{
    std::vector<std::string> args;
    bool in_quotes = false;
    std::string current_arg;

    for (char c : command_line)
    {
        if (c == '"')
        {
            in_quotes = !in_quotes;
        }
        else if (c == ' ' && !in_quotes)
        {
            if (!current_arg.empty())
            {
                args.push_back(current_arg);
                current_arg.clear();
            }
        }
        else
        {
            current_arg += c;
        }
    }

    if (!current_arg.empty())
    {
        args.push_back(current_arg);
    }

    return args;
}
