#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Log/generated/engine_log.h"

#include <functional>
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
    bool setStringValue(const std::string& value) override
    {
        try
        {
            int int_value = std::stoi(value);
            SetValue(int_value);
            return true;
        }
        catch (...)
        {
            return false;
        }
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
    std::string getStringValue() const override { return std::to_string(getValue()); }
    bool setStringValue(const std::string& value) override
    {
        try
        {
            float float_value = std::stof(value);
            SetValue(float_value);
            return true;
        }
        catch (...)
        {
            return false;
        }
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