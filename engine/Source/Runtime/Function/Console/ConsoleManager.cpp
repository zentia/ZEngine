#include "ConsoleManager.h"

#include "RuntimeConsoleCommands.h"

#include <algorithm>
#include <cctype>
#include <fstream>

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

    RegisterCommand("cvar.save",
                    "Write every console variable to a file as name=value: cvar.save <file>",
                    [this](const std::vector<std::string>& args) -> bool {
                        if (args.empty())
                        {
                            LOG_ERROR(ZConsole, "Usage: cvar.save <file>");
                            return false;
                        }
                        const int written = SaveVariablesToFile(args[0]);
                        if (written < 0)
                            return false;
                        LOG_INFO(ZConsole, "cvar.save: wrote {} variable(s) to '{}'", written, args[0]);
                        return true;
                    });

    RegisterCommand("cvar.load",
                    "Apply name=value lines from a file (UE ConsoleVariables.ini style): cvar.load <file>",
                    [this](const std::vector<std::string>& args) -> bool {
                        if (args.empty())
                        {
                            LOG_ERROR(ZConsole, "Usage: cvar.load <file>");
                            return false;
                        }
                        return LoadVariablesFromFile(args[0]) >= 0;
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

// ============================================
// CVar 配置文件
// ============================================

namespace
{
    std::string TrimCopy(const std::string& text)
    {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            --end;
        return text.substr(begin, end - begin);
    }
}  // namespace

std::unordered_map<std::string, std::string> ConsoleManager::SnapshotVariableValues() const
{
    std::unordered_map<std::string, std::string> values;
    values.reserve(m_Variables.size());
    for (const auto& [key, var] : m_Variables)
    {
        if (var)
            values[key] = var->getStringValue();
    }
    return values;
}

int ConsoleManager::SaveVariablesToFile(const std::string& path,
                                        const std::unordered_map<std::string, std::string>* only_changed_from) const
{
    std::vector<std::pair<std::string, std::string>> lines;
    lines.reserve(m_Variables.size());
    for (const auto& [key, var] : m_Variables)
    {
        if (!var)
            continue;
        std::string value = var->getStringValue();
        if (only_changed_from != nullptr)
        {
            const auto it = only_changed_from->find(key);
            if (it != only_changed_from->end() && it->second == value)
                continue;
        }
        // Load trims blanks around the value and strips ONE pair of surrounding
        // double quotes. Quote any value that would otherwise change on the way back:
        // edge whitespace, or a value that itself starts and ends with a quote.
        const bool needs_quotes =
            !value.empty() &&
            (std::isspace(static_cast<unsigned char>(value.front())) ||
             std::isspace(static_cast<unsigned char>(value.back())) ||
             (value.size() >= 2 && value.front() == '"' && value.back() == '"'));
        if (needs_quotes)
            value = '"' + value + '"';
        lines.emplace_back(var->GetName(), std::move(value));
    }
    std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
        return NormalizeKey(a.first) < NormalizeKey(b.first);
    });

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LOG_ERROR(ZConsole, "cvar.save: cannot open '{}' for writing", path);
        return -1;
    }
    out << "; ZEngine console variables (name=value). Load with: cvar.load <file>\n";
    for (const auto& [name, value] : lines)
        out << name << '=' << value << '\n';
    out.flush();
    if (!out)
    {
        LOG_ERROR(ZConsole, "cvar.save: write to '{}' failed", path);
        return -1;
    }
    return static_cast<int>(lines.size());
}

int ConsoleManager::LoadVariablesFromFile(const std::string& path, std::vector<std::string>* out_failed)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        LOG_ERROR(ZConsole, "cvar.load: cannot open '{}'", path);
        return -1;
    }

    int applied = 0;
    std::string raw;
    int line_number = 0;
    while (std::getline(in, raw))
    {
        ++line_number;
        const std::string line = TrimCopy(raw);  // also drops a CR of CRLF files
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            LOG_WARNING(ZConsole, "cvar.load: {}:{}: expected name=value, got '{}'", path, line_number, line);
            if (out_failed != nullptr)
                out_failed->push_back(line);
            continue;
        }
        const std::string name = TrimCopy(line.substr(0, eq));
        std::string value = TrimCopy(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);

        auto var = FindVariable(name);
        if (!var || !var->setStringValue(value))
        {
            LOG_WARNING(ZConsole,
                        "cvar.load: {}:{}: {} '{}'",
                        path,
                        line_number,
                        var ? "invalid value for" : "unknown variable",
                        var ? line : name);
            if (out_failed != nullptr)
                out_failed->push_back(line);
            continue;
        }
        ++applied;
    }
    LOG_INFO(ZConsole, "cvar.load: applied {} value(s) from '{}'", applied, path);
    return applied;
}
