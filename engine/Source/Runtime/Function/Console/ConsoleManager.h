#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Log/generated/engine_log.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <type_traits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================
// 控制台变量类型
// ============================================
enum class ConsoleVariableType
{
    Int,
    Float,
    Bool,
    String
};

// ============================================
// 控制台变量基类
// ============================================
class IConsoleVariable
{
public:
    virtual ~IConsoleVariable() = default;
    virtual std::string GetName() const = 0;
    virtual std::string getDescription() const = 0;
    virtual ConsoleVariableType getType() const = 0;
    virtual std::string getStringValue() const = 0;
    virtual bool setStringValue(const std::string& value) = 0;
    virtual void setOnChangedCallback(std::function<void()> callback) = 0;
};

// Parse the whole string as a number (leading '+' and surrounding blanks allowed).
// Exception-free, so it behaves the same with or without -fexceptions.
template<typename Number>
inline bool ConsoleParseWhole(const std::string& text, Number& out)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    if (begin < end && text[begin] == '+')
        ++begin;
    if (begin == end)
        return false;
    const char* first = text.data() + begin;
    const char* last = text.data() + end;
    if constexpr (std::is_floating_point_v<Number>)
    {
        // Floating-point std::from_chars is not in every libc++ we build with
        // (emscripten's is deleted), so use strtod on a NUL-terminated copy and
        // require it to consume everything. Rejects inf/nan and out-of-range.
        const std::string token(first, last);
        errno = 0;
        char* stop = nullptr;
        const double parsed = std::strtod(token.c_str(), &stop);
        if (stop != token.c_str() + token.size() || errno == ERANGE || !std::isfinite(parsed) ||
            std::fabs(parsed) > static_cast<double>(std::numeric_limits<Number>::max()))
            return false;
        out = static_cast<Number>(parsed);
        return true;
    }
    else
    {
        const auto result = std::from_chars(first, last, out);
        return result.ec == std::errc() && result.ptr == last;
    }
}

// ============================================
// 控制台变量模板实现
// ============================================
template<typename T>
class ConsoleVariable : public IConsoleVariable
{
public:
    ConsoleVariable(const std::string& name, const std::string& description, T initial_value, T* storage = nullptr)
        : m_Name(name), m_Description(description), m_Storage(storage)
    {
        if (m_Storage)
        {
            *m_Storage = initial_value;
        }
        else
        {
            m_Value = initial_value;
        }
    }

    std::string GetName() const override { return m_Name; }
    std::string getDescription() const override { return m_Description; }

    T getValue() const { return m_Storage ? *m_Storage : m_Value; }

    void SetValue(const T& value)
    {
        if (m_Storage)
        {
            *m_Storage = value;
        }
        else
        {
            m_Value = value;
        }
        if (m_OnChangedCallback)
        {
            m_OnChangedCallback();
        }
    }

    void setOnChangedCallback(std::function<void()> callback) override { m_OnChangedCallback = callback; }

private:
    std::string m_Name;
    std::string m_Description;
    T* m_Storage;
    T m_Value;
    std::function<void()> m_OnChangedCallback;
};

// 特化：Int
template<>
class ConsoleVariable<int> : public IConsoleVariable
{
public:
    ConsoleVariable(const std::string& name, const std::string& description, int initial_value, int* storage = nullptr)
        : m_Name(name), m_Description(description), m_Storage(storage)
    {
        if (m_Storage)
        {
            *m_Storage = initial_value;
        }
        else
        {
            m_Value = initial_value;
        }
    }

    std::string GetName() const override { return m_Name; }
    std::string getDescription() const override { return m_Description; }
    ConsoleVariableType getType() const override { return ConsoleVariableType::Int; }
    std::string getStringValue() const override { return std::to_string(getValue()); }
    // std::from_chars, not std::stoi: no exceptions (the web build compiles with
    // exception catching off, so stoi's throw on "abc" aborted the process instead of
    // reaching the catch), and the WHOLE value must parse ("12abc" used to become 12).
    bool setStringValue(const std::string& value) override
    {
        int int_value = 0;
        if (!ConsoleParseWhole(value, int_value))
            return false;
        SetValue(int_value);
        return true;
    }

    void setOnChangedCallback(std::function<void()> callback) override { m_OnChangedCallback = callback; }

    int getValue() const { return m_Storage ? *m_Storage : m_Value; }
    void SetValue(int value)
    {
        if (m_Storage)
        {
            *m_Storage = value;
        }
        else
        {
            m_Value = value;
        }
        if (m_OnChangedCallback)
        {
            m_OnChangedCallback();
        }
    }

private:
    std::string m_Name;
    std::string m_Description;
    int* m_Storage;
    int m_Value;
    std::function<void()> m_OnChangedCallback;
};

// 特化：Float
template<>
class ConsoleVariable<float> : public IConsoleVariable
{
public:
    ConsoleVariable(const std::string& name,
                    const std::string& description,
                    float initial_value,
                    float* storage = nullptr)
        : m_Name(name), m_Description(description), m_Storage(storage)
    {
        if (m_Storage)
        {
            *m_Storage = initial_value;
        }
        else
        {
            m_Value = initial_value;
        }
    }

    std::string GetName() const override { return m_Name; }
    std::string getDescription() const override { return m_Description; }
    ConsoleVariableType getType() const override { return ConsoleVariableType::Float; }
    // Shortest of %.6g .. %.9g that parses back to the same float (9 significant
    // digits always round-trip a float). std::to_string printed "%f": six fixed
    // decimals, so 1e-7 became "0.000000" and a value saved with cvar.save did not
    // come back as itself. (Floating-point std::to_chars is not available in every
    // libc++ this builds with.)
    std::string getStringValue() const override
    {
        const float value = getValue();
        char buffer[48];
        for (int precision = 6; precision <= 9; ++precision)
        {
            std::snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<double>(value));
            if (std::strtof(buffer, nullptr) == value)
                break;
        }
        return buffer;
    }
    bool setStringValue(const std::string& value) override
    {
        float float_value = 0.0f;
        if (!ConsoleParseWhole(value, float_value))
            return false;
        SetValue(float_value);
        return true;
    }

    void setOnChangedCallback(std::function<void()> callback) override { m_OnChangedCallback = callback; }

    float getValue() const { return m_Storage ? *m_Storage : m_Value; }
    void SetValue(float value)
    {
        if (m_Storage)
        {
            *m_Storage = value;
        }
        else
        {
            m_Value = value;
        }
        if (m_OnChangedCallback)
        {
            m_OnChangedCallback();
        }
    }

private:
    std::string m_Name;
    std::string m_Description;
    float* m_Storage;
    float m_Value;
    std::function<void()> m_OnChangedCallback;
};

// 特化：Bool
template<>
class ConsoleVariable<bool> : public IConsoleVariable
{
public:
    ConsoleVariable(const std::string& name,
                    const std::string& description,
                    bool initial_value,
                    bool* storage = nullptr)
        : m_Name(name), m_Description(description), m_Storage(storage)
    {
        if (m_Storage)
        {
            *m_Storage = initial_value;
        }
        else
        {
            m_Value = initial_value;
        }
    }

    std::string GetName() const override { return m_Name; }
    std::string getDescription() const override { return m_Description; }
    ConsoleVariableType getType() const override { return ConsoleVariableType::Bool; }
    std::string getStringValue() const override { return getValue() ? "1" : "0"; }
    bool setStringValue(const std::string& value) override
    {
        if (value == "1" || value == "true" || value == "True" || value == "TRUE")
        {
            SetValue(true);
            return true;
        }
        else if (value == "0" || value == "false" || value == "False" || value == "FALSE")
        {
            SetValue(false);
            return true;
        }
        return false;
    }

    void setOnChangedCallback(std::function<void()> callback) override { m_OnChangedCallback = callback; }

    bool getValue() const { return m_Storage ? *m_Storage : m_Value; }
    void SetValue(bool value)
    {
        if (m_Storage)
        {
            *m_Storage = value;
        }
        else
        {
            m_Value = value;
        }
        if (m_OnChangedCallback)
        {
            m_OnChangedCallback();
        }
    }

private:
    std::string m_Name;
    std::string m_Description;
    bool* m_Storage;
    bool m_Value;
    std::function<void()> m_OnChangedCallback;
};

// 特化：String
template<>
class ConsoleVariable<std::string> : public IConsoleVariable
{
public:
    ConsoleVariable(const std::string& name,
                    const std::string& description,
                    const std::string& initial_value,
                    std::string* storage = nullptr)
        : m_Name(name), m_Description(description), m_Storage(storage)
    {
        if (m_Storage)
        {
            *m_Storage = initial_value;
        }
        else
        {
            m_Value = initial_value;
        }
    }

    std::string GetName() const override { return m_Name; }
    std::string getDescription() const override { return m_Description; }
    ConsoleVariableType getType() const override { return ConsoleVariableType::String; }
    std::string getStringValue() const override { return getValue(); }
    bool setStringValue(const std::string& value) override
    {
        SetValue(value);
        return true;
    }

    void setOnChangedCallback(std::function<void()> callback) override { m_OnChangedCallback = callback; }

    std::string getValue() const { return m_Storage ? *m_Storage : m_Value; }
    void SetValue(const std::string& value)
    {
        if (m_Storage)
        {
            *m_Storage = value;
        }
        else
        {
            m_Value = value;
        }
        if (m_OnChangedCallback)
        {
            m_OnChangedCallback();
        }
    }

private:
    std::string m_Name;
    std::string m_Description;
    std::string* m_Storage;
    std::string m_Value;
    std::function<void()> m_OnChangedCallback;
};

// ============================================
// 控制台命令
// ============================================
class IConsoleCommand
{
public:
    virtual ~IConsoleCommand() = default;
    virtual std::string GetName() const = 0;
    virtual std::string getDescription() const = 0;
    virtual bool Execute(const std::vector<std::string>& args) = 0;
};

// ============================================
// 控制台命令实现
// ============================================
class ConsoleCommand : public IConsoleCommand
{
public:
    using CommandDelegate = std::function<bool(const std::vector<std::string>&)>;

    ConsoleCommand(const std::string& name, const std::string& description, CommandDelegate delegate)
        : m_Name(name), m_Description(description), m_Delegate(delegate)
    {
    }

    std::string GetName() const override { return m_Name; }
    std::string getDescription() const override { return m_Description; }
    bool Execute(const std::vector<std::string>& args) override
    {
        if (m_Delegate)
        {
            return m_Delegate(args);
        }
        return false;
    }

private:
    std::string m_Name;
    std::string m_Description;
    CommandDelegate m_Delegate;
};

// ============================================
// ConsoleManager - 控制台管理器
// ============================================
class ConsoleManager : public IEngineSystem
{
public:
    std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override { return {}; }

    bool Initialize() override;
    void Shutdown() override;

    // ============================================
    // 控制台变量注册
    // ============================================

    // 注册Int类型变量
    std::shared_ptr<ConsoleVariable<int>> RegisterIntVariable(const std::string& name,
                                                              const std::string& description,
                                                              int initial_value,
                                                              int* storage = nullptr);

    // 注册Float类型变量
    std::shared_ptr<ConsoleVariable<float>> RegisterFloatVariable(const std::string& name,
                                                                  const std::string& description,
                                                                  float initial_value,
                                                                  float* storage = nullptr);

    // 注册Bool类型变量
    std::shared_ptr<ConsoleVariable<bool>> RegisterBoolVariable(const std::string& name,
                                                                const std::string& description,
                                                                bool initial_value,
                                                                bool* storage = nullptr);

    // 注册String类型变量
    std::shared_ptr<ConsoleVariable<std::string>> RegisterStringVariable(const std::string& name,
                                                                         const std::string& description,
                                                                         const std::string& initial_value,
                                                                         std::string* storage = nullptr);

    // 获取变量
    std::shared_ptr<IConsoleVariable> FindVariable(const std::string& name) const;

    // ============================================
    // 控制台命令注册
    // ============================================

    // 注册命令
    std::shared_ptr<IConsoleCommand> RegisterCommand(const std::string& name,
                                                     const std::string& description,
                                                     std::function<bool(const std::vector<std::string>&)> delegate);

    // 获取命令
    std::shared_ptr<IConsoleCommand> FindCommand(const std::string& name) const;

    // ============================================
    // 命令执行
    // ============================================

    // 执行命令字符串（解析并执行）
    bool ExecuteString(const std::string& command_line);

    // ============================================
    // 命令历史记录
    // ============================================

    // 添加命令到历史记录
    void AddToHistory(const std::string& command);

    // 获取历史记录
    const std::vector<std::string>& getHistory() const { return m_CommandHistory; }

    // 清空历史记录
    void clearHistory() { m_CommandHistory.clear(); }

    // ============================================
    // 自动补全
    // ============================================

    // 获取匹配的命令/变量名称列表
    std::vector<std::string> GetAutoCompleteList(const std::string& prefix) const;

    // ============================================
    // 帮助信息
    // ============================================

    // 打印所有命令和变量
    void PrintHelp() const;

    // 打印特定命令/变量的帮助
    void PrintHelp(const std::string& name) const;

    // ============================================
    // CVar 配置文件（UE ConsoleVariables.ini 风格）
    // ============================================
    // 文本格式：每行 `name=value`；空行和以 `#` / `;` 开头的行忽略；`[Section]` 行忽略
    // （兼容 UE 的 [Startup] / [ConsoleVariables] 分节写法）。value 两侧的空白去掉，
    // 成对的双引号去掉。

    /// 把当前所有变量写成 `name=value`，按名字排序（输出稳定，便于 diff）。
    /// 只写值与 `only_changed_from` 里不同的变量（传 nullptr 写全部）。返回写出的条数，
    /// 打不开文件返回 -1。
    int SaveVariablesToFile(const std::string& path,
                            const std::unordered_map<std::string, std::string>* only_changed_from = nullptr) const;

    /// 逐行应用文件里的 `name=value`。未知变量 / 非法值记一条警告并跳过，不中断后面的行。
    /// 返回成功应用的条数；打不开文件返回 -1。`out_failed` 非空时追加失败行的原文。
    int LoadVariablesFromFile(const std::string& path, std::vector<std::string>* out_failed = nullptr);

    /// 当前所有变量的值快照（键为规范化小写名），配合 SaveVariablesToFile 只存改过的值。
    std::unordered_map<std::string, std::string> SnapshotVariableValues() const;

    // ============================================
    // 启动 CVar 文件（UE 的 Config/ConsoleVariables.ini）
    // ============================================
    // 启动顺序：所有系统 Initialize 完（CVar 都已注册）→ LoadStartupVariables() →
    // 命令行 `--cvar name=value`。后者覆盖前者，和 UE 一样命令行优先。

    static constexpr const char* k_startup_variables_file_name = "ConsoleVariables.ini";
    static constexpr const char* k_startup_variables_env = "ZENGINE_CVAR_FILE";

    /// 决定启动时读哪个文件。`override_spec` 来自 `--cvar-file` 或环境变量 ZENGINE_CVAR_FILE：
    ///   nullptr / 空串                     → <project_root>/Config/ConsoleVariables.ini
    ///   "none" / "off" / "0"（不分大小写） → 关闭，返回空路径
    ///   其它                               → 当成文件路径（相对路径相对当前目录）
    static std::filesystem::path ResolveStartupVariablesFile(const std::filesystem::path& project_root,
                                                             const char* override_spec);

    /// 应用启动文件。路径为空（已关闭）或文件不存在都不算错误，返回 0；
    /// 存在时等同 LoadVariablesFromFile（返回应用条数，打不开 -1）。
    int LoadStartupVariablesFile(const std::filesystem::path& file);

    /// 一步到位：`override_spec` 为 nullptr 时读环境变量 ZENGINE_CVAR_FILE；项目根取
    /// ProjectInfo::GetProjectRoot()，没有项目时取当前目录。
    int LoadStartupVariables(const char* override_spec = nullptr);

private:
    static std::string NormalizeKey(const std::string& name);

    // 解析命令字符串
    std::vector<std::string> ParseCommandLine(const std::string& command_line) const;

    bool TrySetVariable(const std::string& var_name, const std::string& var_value);
    bool TryPrintVariable(const std::string& var_name) const;

    // 变量映射表
    std::unordered_map<std::string, std::shared_ptr<IConsoleVariable>> m_Variables;

    // 命令映射表
    std::unordered_map<std::string, std::shared_ptr<IConsoleCommand>> m_Commands;

    // 命令历史记录（最多保存100条）
    std::vector<std::string> m_CommandHistory;
    static constexpr size_t k_max_history_size = 100;
};