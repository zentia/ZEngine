#pragma once

#include "ConsoleManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"

#include <memory>

// Static registrars run before ConsoleManager::Initialize; prefer RegisterRuntimeConsoleCommands /
// RegisterEditorConsoleCommands, or call Register* from your module's Initialize().

#define REGISTER_CONSOLE_VARIABLE_INT(name, description, initial_value, storage)              \
    namespace                                                                                 \
    {                                                                                         \
        struct ConsoleVarRegistrar_##name                                                     \
        {                                                                                     \
            ConsoleVarRegistrar_##name()                                                      \
            {                                                                                 \
                if (auto console = GET_SYSTEM(ConsoleManager))                                \
                {                                                                             \
                    console->RegisterIntVariable(#name, description, initial_value, storage); \
                }                                                                             \
            }                                                                                 \
        };                                                                                    \
        static ConsoleVarRegistrar_##name g_console_var_##name;                               \
    }

#define REGISTER_CONSOLE_VARIABLE_FLOAT(name, description, initial_value, storage)              \
    namespace                                                                                 \
    {                                                                                         \
        struct ConsoleVarRegistrar_##name                                                     \
        {                                                                                     \
            ConsoleVarRegistrar_##name()                                                      \
            {                                                                                   \
                if (auto console = GET_SYSTEM(ConsoleManager))                                  \
                {                                                                               \
                    console->RegisterFloatVariable(#name, description, initial_value, storage); \
                }                                                                               \
            }                                                                                   \
        };                                                                                      \
        static ConsoleVarRegistrar_##name g_console_var_##name;                                 \
    }

#define REGISTER_CONSOLE_VARIABLE_BOOL(name, description, initial_value, storage)              \
    namespace                                                                                  \
    {                                                                                          \
        struct ConsoleVarRegistrar_##name                                                      \
        {                                                                                      \
            ConsoleVarRegistrar_##name()                                                       \
            {                                                                                  \
                if (auto console = GET_SYSTEM(ConsoleManager))                                 \
                {                                                                              \
                    console->RegisterBoolVariable(#name, description, initial_value, storage); \
                }                                                                              \
            }                                                                                  \
        };                                                                                     \
        static ConsoleVarRegistrar_##name g_console_var_##name;                                \
    }

#define REGISTER_CONSOLE_VARIABLE_STRING(name, description, initial_value, storage)              \
    namespace                                                                                    \
    {                                                                                            \
        struct ConsoleVarRegistrar_##name                                                        \
        {                                                                                        \
            ConsoleVarRegistrar_##name()                                                         \
            {                                                                                    \
                if (auto console = GET_SYSTEM(ConsoleManager))                                   \
                {                                                                                \
                    console->RegisterStringVariable(#name, description, initial_value, storage); \
                }                                                                                \
            }                                                                                    \
        };                                                                                       \
        static ConsoleVarRegistrar_##name g_console_var_##name;                                  \
    }

#define REGISTER_CONSOLE_COMMAND(name, description, delegate)               \
    namespace                                                               \
    {                                                                       \
        struct ConsoleCmdRegistrar_##name                                   \
        {                                                                   \
            ConsoleCmdRegistrar_##name()                                    \
            {                                                               \
                if (auto console = GET_SYSTEM(ConsoleManager))              \
                {                                                           \
                    console->RegisterCommand(#name, description, delegate); \
                }                                                           \
            }                                                               \
        };                                                                  \
        static ConsoleCmdRegistrar_##name g_console_cmd_##name;             \
    }

#define REGISTER_CONSOLE_COMMAND_STATIC(name, description, func) \
    REGISTER_CONSOLE_COMMAND(name, description, [](const std::vector<std::string>& args) -> bool { return func(args); })

#define REGISTER_CONSOLE_COMMAND_MEMBER(name, description, obj, func) \
    REGISTER_CONSOLE_COMMAND(                                         \
        name, description, [obj](const std::vector<std::string>& args) -> bool { return (obj->*func)(args); })

#define GET_CONSOLE_MANAGER() GET_SYSTEM(ConsoleManager)

#define EXEC_CONSOLE_COMMAND(cmd)                   \
    do                                              \
    {                                               \
        if (auto console = GET_CONSOLE_MANAGER())   \
        {                                           \
            console->ExecuteString(cmd);            \
        }                                           \
    } while (0)

#define GET_CONSOLE_VARIABLE_INT(name)                                              \
    ([]() -> std::shared_ptr<ConsoleVariable<int>> {                                 \
        if (auto console = GET_CONSOLE_MANAGER())                                   \
        {                                                                           \
            auto var = console->FindVariable(#name);                                \
            if (var && var->getType() == ConsoleVariableType::Int)                  \
            {                                                                       \
                return std::static_pointer_cast<ConsoleVariable<int>>(var);         \
            }                                                                       \
        }                                                                           \
        return nullptr;                                                             \
    }())

#define GET_CONSOLE_VARIABLE_FLOAT(name)                                              \
    ([]() -> std::shared_ptr<ConsoleVariable<float>> {                                \
        if (auto console = GET_CONSOLE_MANAGER())                                     \
        {                                                                             \
            auto var = console->FindVariable(#name);                                  \
            if (var && var->getType() == ConsoleVariableType::Float)                  \
            {                                                                         \
                return std::static_pointer_cast<ConsoleVariable<float>>(var);         \
            }                                                                         \
        }                                                                             \
        return nullptr;                                                               \
    }())

#define GET_CONSOLE_VARIABLE_BOOL(name)                                              \
    ([]() -> std::shared_ptr<ConsoleVariable<bool>> {                                 \
        if (auto console = GET_CONSOLE_MANAGER())                                    \
        {                                                                            \
            auto var = console->FindVariable(#name);                                 \
            if (var && var->getType() == ConsoleVariableType::Bool)                  \
            {                                                                        \
                return std::static_pointer_cast<ConsoleVariable<bool>>(var);         \
            }                                                                        \
        }                                                                            \
        return nullptr;                                                              \
    }())

#define GET_CONSOLE_VARIABLE_STRING(name)                                                   \
    ([]() -> std::shared_ptr<ConsoleVariable<std::string>> {                               \
        if (auto console = GET_CONSOLE_MANAGER())                                           \
        {                                                                                   \
            auto var = console->FindVariable(#name);                                        \
            if (var && var->getType() == ConsoleVariableType::String)                       \
            {                                                                               \
                return std::static_pointer_cast<ConsoleVariable<std::string>>(var);         \
            }                                                                               \
        }                                                                                   \
        return nullptr;                                                                     \
    }())
