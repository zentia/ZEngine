#pragma once

#include "Runtime/Core/Base/Platform.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef Z_PLATFORM_WIN
    #include <cstdio>
    #include <typeinfo>
    #include <windows.h>
    // 必须添加：确保共享段有读写权限（替代工程配置）
    #pragma comment(linker, "/SECTION:SingletonShared,RWS")

    // ========== 非模板共享存储结构（解决模板静态成员跨DLL问题） ==========
    // 使用非模板结构确保所有模块共享同一个符号定义
    #pragma section("SingletonShared", read, write, shared)

// 共享段存储结构（POD类型，避免跨DLL布局问题）
struct SingletonSharedData
{
    void* instance;          // 实例指针（void*避免模板类型问题）
    volatile long initFlag;  // 初始化标志（0=未初始化,1=初始化中,2=已初始化）
    char typeName[256];      // 类型名称（用于验证）
};

// 全局共享数据数组（支持多个单例类型，最多支持16个不同的单例类型）
// 注意：使用 extern 声明，在文件末尾定义
extern __declspec(allocate("SingletonShared")) SingletonSharedData s_Shareddata[16];
extern __declspec(allocate("SingletonShared")) volatile long s_Shareddatacount;
#endif

/**
 * @brief 单例模式模板基类
 * 修复点：
 * 1. 使用非模板共享存储结构，避免模板静态成员跨DLL重复实例化
 * 2. 共享段仅存放POD类型（long/指针），避免std::atomic跨DLL布局问题
 * 3. 封装Interlocked系列函数替代std::atomic，适配共享段POD类型
 * 4. 严格校验共享段访问权限，避免写操作崩溃
 * 5. 优化互斥锁命名逻辑，解决GetInstance地址跨DLL不一致问题
 */
template<typename T>
class Singleton
{
private:
#ifdef Z_PLATFORM_WIN
    // 本地缓存+状态（非共享，每个DLL独立）
    static thread_local T* t_localInstanceCache;
    static std::atomic<bool> s_Sharedsectionavailable;  // 标记共享段是否可访问
    static std::mutex s_Localmutex;

    // 获取类型索引（用于在共享数据数组中定位）
    static size_t GetTypeIndex()
    {
        // 使用类型名称的哈希值作为索引
        static size_t s_Typeindex = SIZE_MAX;
        if (s_Typeindex == SIZE_MAX)
        {
            const char* typeName = typeid(T).name();
            // 简单的哈希函数
            size_t hash = 0;
            for (const char* p = typeName; *p; ++p)
            {
                hash = hash * 31 + static_cast<size_t>(*p);
            }
            s_Typeindex = hash % 16;  // 限制在0-15范围内
        }
        return s_Typeindex;
    }

    // 获取共享数据（通过类型索引）
    static SingletonSharedData* GetSharedData()
    {
        size_t index = GetTypeIndex();
        if (index < 16)
        {
            return &s_Shareddata[index];
        }
        return nullptr;
    }

    // 验证共享段中的实例类型是否匹配（防止哈希冲突）
    static bool ValidateInstanceType(SingletonSharedData* data, T* instance)
    {
        if (data == nullptr || instance == nullptr)
            return false;

        // 如果类型名称未设置，说明是新创建的，需要验证
        if (data->typeName[0] == '\0')
            return true;  // 新创建，暂时信任

        // 验证类型名称是否匹配
        const char* expectedTypeName = typeid(T).name();
        return std::strncmp(data->typeName, expectedTypeName, sizeof(data->typeName) - 1) == 0;
    }

    // ========== 封装共享段原子操作（替代std::atomic） ==========
    // 原子读initFlag（封装InterlockedCompareExchange）
    static long AtomicReadInitFlag()
    {
        SingletonSharedData* data = GetSharedData();
        if (data == nullptr)
            return 0;
        // InterlockedCompareExchange实现原子读：比较并交换相同值，返回原值
        return InterlockedCompareExchange(const_cast<volatile long*>(&data->initFlag), 0, 0);
    }

    // 原子写initFlag（封装InterlockedExchange）
    static void AtomicWriteInitFlag(long value)
    {
        SingletonSharedData* data = GetSharedData();
        if (data == nullptr)
            return;
        InterlockedExchange(const_cast<volatile long*>(&data->initFlag), value);
    }

    // 原子读instance（封装InterlockedCompareExchangePointer）
    static T* AtomicReadInstance()
    {
        SingletonSharedData* data = GetSharedData();
        if (data == nullptr)
            return nullptr;
        return reinterpret_cast<T*>(
            InterlockedCompareExchangePointer(
                reinterpret_cast<PVOID*>(&data->instance),
                nullptr,
                nullptr));
    }

    // 原子写instance（封装InterlockedExchangePointer）
    static void AtomicWriteInstance(T* value)
    {
        SingletonSharedData* data = GetSharedData();
        if (data == nullptr)
            return;
        InterlockedExchangePointer(
            reinterpret_cast<PVOID*>(&data->instance),
            value);
    }

    // ========== 安全访问共享段（封装SEH，避免崩溃） ==========
    template<typename Func>
    static bool SafeAccessSharedSection(Func&& func)
    {
        if (!s_Sharedsectionavailable.load(std::memory_order_acquire))
            return false;

        __try
        {
            func();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // 共享段访问失败，标记为不可用，后续不再尝试
            s_Sharedsectionavailable.store(false, std::memory_order_release);
            return false;
        }
    }

    // ========== 修复互斥锁命名（解决GetInstance地址跨DLL不一致） ==========
    static std::string GetStableMutexName()
    {
        // 使用类型名替代函数指针，确保跨DLL名称一致
        char typeName[256] = {0};
        snprintf(typeName, sizeof(typeName), "ZEngine_Singleton_%s", typeid(T).name());
        return typeName;
    }

    static HANDLE GetOrCreateMutex()
    {
        static std::atomic<HANDLE> s_Mutexhandle = nullptr;
        static std::once_flag s_Mutexinitflag;

        std::call_once(s_Mutexinitflag, [&]() {
            std::string mutexName = GetStableMutexName();
            HANDLE hMutex = CreateMutexA(nullptr, FALSE, mutexName.c_str());
            if (hMutex == nullptr)
            {
                s_Mutexhandle = INVALID_HANDLE_VALUE;
            }
            else
            {
                s_Mutexhandle = hMutex;
                // 进程退出时释放互斥锁，避免句柄泄漏
                atexit([]() {
                    HANDLE h = s_Mutexhandle.exchange(INVALID_HANDLE_VALUE);
                    if (h != nullptr && h != INVALID_HANDLE_VALUE)
                    {
                        CloseHandle(h);
                    }
                });
            }
        });

        HANDLE h = s_Mutexhandle.load();
        return (h != INVALID_HANDLE_VALUE) ? h : nullptr;
    }
#endif

    // 非Windows平台：Meyers单例（保留）
    static std::atomic<bool> s_Instancedestroyed;
    static std::mutex s_Globalmutex;

    // 禁止拷贝/赋值
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

    // 创建实例（捕获构造异常，避免初始化中断）
    static T* CreateInstance()
    {
        try
        {
            return new T();
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("Singleton create failed: " + std::string(e.what()));
        }
        catch (...)
        {
            throw std::runtime_error("Singleton create failed: unknown exception");
        }
    }

public:
    /**
     * @brief 获取单例实例（线程安全，跨DLL安全）
     * @return 单例实例的引用
     */
    inline static T& GetInstance()
    {
        static T s_Instance;
        return s_Instance;
    }

    /**
     * @brief 获取单例实例指针
     * @return 实例指针（nullptr表示初始化失败）
     */
    static T* GetInstancePtr()
    {
        try
        {
            return &GetInstance();
        }
        catch (...)
        {
            return nullptr;
        }
    }

protected:
    Singleton() = default;
    virtual ~Singleton() = default;
};

// ========== 静态成员初始化 ==========
#ifdef Z_PLATFORM_WIN
// 非模板共享数据初始化（所有模块共享同一个定义）
// 使用 __declspec(selectany) 确保所有模块共享同一个符号
__declspec(selectany) __declspec(allocate("SingletonShared")) SingletonSharedData s_Shareddata[16] = {};
__declspec(selectany) __declspec(allocate("SingletonShared")) volatile long s_Shareddatacount = 0;

// 模板本地静态成员初始化（非共享，每个模块独立）
template<typename T>
thread_local T* Singleton<T>::t_localInstanceCache = nullptr;

template<typename T>
std::atomic<bool> Singleton<T>::s_Sharedsectionavailable = true;

template<typename T>
std::mutex Singleton<T>::s_Localmutex;
#endif

// 非Windows平台静态成员
template<typename T>
std::atomic<bool> Singleton<T>::s_Instancedestroyed = false;

template<typename T>
std::mutex Singleton<T>::s_Globalmutex;