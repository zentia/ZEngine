#pragma once

#include "Runtime/Core/Log/LogSystem.h"

#include <chrono>
#include <thread>

#define LOG_HELPER(LOG_LEVEL, CategoryName, ...) \
    LogSystem::GetInstance().log(LOG_LEVEL, LogSystem::GetInstance().m_Logger->cat.CategoryName, __VA_ARGS__)

#define LOG_VERBOSE(CategoryName, ...) LOG_HELPER(LogSystem::LogLevel::verbose, CategoryName, __VA_ARGS__);

#define LOG_DEBUG(CategoryName, ...) LOG_HELPER(LogSystem::LogLevel::debug, CategoryName, __VA_ARGS__);

#define LOG_INFO(CategoryName, ...) LOG_HELPER(LogSystem::LogLevel::info, CategoryName, __VA_ARGS__);

#define LOG_WARNING(CategoryName, ...) LOG_HELPER(LogSystem::LogLevel::warning, CategoryName, __VA_ARGS__);

#define LOG_ERROR(CategoryName, ...) LOG_HELPER(LogSystem::LogLevel::error, CategoryName, __VA_ARGS__)

#define LOG_FATAL(CategoryName, ...) LOG_HELPER(LogSystem::LogLevel::fatal, CategoryName, __VA_ARGS__)

#define LOG_FLUSH() LogSystem::GetInstance().m_Logger->force_flush()

#ifdef NDEBUG
    #define ASSERT(statement)
#else
    #define ASSERT(statement) assert(statement)
#endif

// Branch prediction optimization hints
#if defined(__GNUC__) || defined(__clang__)
    #define OPTIMIZER_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define OPTIMIZER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
// MSVC doesn't support __builtin_expect, use no-op for compatibility
// Note: C++20 [[likely]]/[[unlikely]] are statement attributes and work differently
    #define OPTIMIZER_LIKELY(x)   (x)
    #define OPTIMIZER_UNLIKELY(x) (x)
#else
// Fallback for other compilers
    #define OPTIMIZER_LIKELY(x)   (x)
    #define OPTIMIZER_UNLIKELY(x) (x)
#endif

// Unity-style cast helpers (project-wide naming convention).
#define staticCast      static_cast
#define reinterpretCast reinterpret_cast
#define constCast       const_cast
#define dynamicCast     dynamic_cast
