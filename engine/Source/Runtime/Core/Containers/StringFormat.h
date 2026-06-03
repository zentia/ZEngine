#pragma once

#include "Runtime/Core/Containers/String.h"

#include <utility>

// 检查是否支持C++20 format
#if __has_include(<format>)
    #include <format>
    #define Z_HAS_STD_FORMAT 1
#else
    #define Z_HAS_STD_FORMAT 0
#endif

/// @brief EASTL字符串格式化函数（类似C++20的std::format，但返回eastl::string）
///
/// 如果支持C++20 std::format，使用std::format语法:
/// ```cpp
/// Engine::string msg = Engine::format("Hello {}, you have {} messages", "Alice", 5);
/// Engine::string result = Engine::format("Value: {:.2f}", 3.14159);  // "Value: 3.14"
/// ```
///
/// 如果不支持，则使用传统printf语法:
/// ```cpp
/// Engine::string msg = Engine::format("Hello %s, you have %d messages", "Alice", 5);
/// ```
template<typename... Args>
inline eastl::string format(const char* fmt, Args&&... args)
{
#if Z_HAS_STD_FORMAT && __cpp_lib_format >= 201907L
    // C++20 std::format可用，使用它并转换为eastl::string
    std::string result = std::format(fmt, std::forward<Args>(args)...);
    return string(result.c_str(), result.size());
#else
    // 使用eastl::string::sprintf（传统printf风格）
    eastl::string result;
    result.sprintf(fmt, args...);
    return result;
#endif
}

/// @brief 使用eastl::string::sprintf进行格式化（传统printf风格）
///
/// 使用示例:
/// ```cpp
/// Engine::string msg = Engine::sprintf_format("Hello %s, you have %d messages", "Alice", 5);
/// ```
template<typename... Args>
inline eastl::string sprintf_format(const char* fmt, Args... args)
{
    eastl::string result;
    result.sprintf(fmt, args...);
    return result;
}

/// @brief 使用eastl::string::append_sprintf进行格式化（追加到现有字符串）
///
/// 使用示例:
/// ```cpp
/// Engine::string msg = "Prefix: ";
/// Engine::append_format(msg, "value = %d", 42);
/// ```
template<typename... Args>
inline void append_format(string& str, const char* fmt, Args... args)
{
    str.append_sprintf(fmt, args...);
}

#undef Z_HAS_STD_FORMAT
