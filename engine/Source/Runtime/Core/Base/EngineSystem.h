#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <vector>

#define GET_CLASS_NAME(name)  #name
#define GET_SYSTEM_TYPE(name) std::type_index(typeid(name))

// 自动获取类名的宏（在类的方法中使用，无需重载 getName）
// 使用方式：在 GetName() 中返回 AUTO_GET_CLASS_NAME();
// 或者直接使用：std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }
#define AUTO_GET_CLASS_NAME() GetTypeName<std::decay_t<decltype(*this)>>()

// ============================================
// 自动提取类名的辅助函数（编译时提取）
// ============================================
namespace Detail
{
    // 从函数签名字符串中提取类名
    inline std::string ExtractClassNameFromSignature(const char* signature)
    {
#if defined(__GNUC__) || defined(__clang__)
        // GCC/Clang: __PRETTY_FUNCTION__ 格式类似
        // "const char* Engine::detail::getTypeName() [with T = Engine::CommandSystem]"
        const char* prefix = "with T = ";
        const char* suffix = "]";

        const char* start = std::strstr(signature, prefix);
        if (start)
        {
            start += std::strlen(prefix);
            const char* end = std::strstr(start, suffix);
            if (end)
            {
                std::string name(start, end);
                // 移除命名空间前缀 "Engine::"
                if (name.size() >= 3 && name.substr(0, 3) == "Engine::")
                {
                    name = name.substr(3);
                }
                return name;
            }
        }
#elif defined(_MSC_VER)
        // MSVC: __FUNCSIG__ 格式类似
        // "const char *__cdecl Engine::getTypeName<Engine::CommandSystem>(void)"
        const char* prefix = "getTypeName<";
        const char* suffix = ">";

        const char* start = std::strstr(signature, prefix);
        if (start)
        {
            start += std::strlen(prefix);
            const char* end = std::strchr(start, '>');
            if (end)
            {
                std::string name(start, end);
                // 移除命名空间前缀 "Engine::"
                if (name.size() >= 3 && name.substr(0, 3) == "Engine::")
                {
                    name = name.substr(3);
                }
                return name;
            }
        }
#endif
        // 回退到 typeid
        return std::string(signature);
    }
}  // namespace Detail

// 获取类型名称的模板函数（自动提取类名，无需重载）
// 使用方式：getTypeName<MyClass>() 返回 "MyClass"
template<typename T>
std::string GetTypeName()
{
#if defined(__GNUC__) || defined(__clang__)
    return Detail::ExtractClassNameFromSignature(__PRETTY_FUNCTION__);
#elif defined(_MSC_VER)
    return Detail::ExtractClassNameFromSignature(__FUNCSIG__);
#else
    return std::string(typeid(T).name());
#endif
}
// ============================================
// 系统初始化阶段枚举
// ============================================
enum class SystemInitPhase : int
{
    PreInit = 0,    // 配置、文件系统、日志等基础系统
    Core = 1,       // 线程、内存管理等核心系统
    Resource = 2,   // 资源管理（AssetManager, PreloadManager）
    Platform = 3,   // 平台相关（Window, Input）
    Rendering = 4,  // 渲染系统（RHI, RenderSystem）
    Gameplay = 5,   // 游戏系统（World, Physics, Particle）
    PostInit = 6    // 最后初始化（ModuleManager, Canvas）
};

// ============================================
// 统一的系统接口
// ============================================
class SystemRegistry;  // 前向声明

class IEngineSystem
{
    friend class SystemRegistry;  // 允许 SystemRegistry 访问 protected 成员

public:
    virtual ~IEngineSystem() = default;

    // 获取系统是否已初始化
    bool IsInitialized() const { return m_Initialized; }

    // 获取系统是否已关闭
    bool IsShutdown() const { return m_Shutdown; }

    void SetInitialized(bool value) { m_Initialized = value; }

    void SetShutdown(bool value) { m_Shutdown = value; }

protected:
    // 获取系统名称（可选，用于日志输出）
    // 默认实现使用 typeid，返回的可能是编译器修饰的名称
    // 如果需要友好的类名，可以在子类中这样使用：
    //   std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }
    virtual std::string GetName() const
    {
        // 默认实现：使用 typeid 获取类型名称
        // 注意：这是编译器生成的名称，可能不够友好（如 "N2Z12CommandSystemE"）
        // 如果需要友好的类名，建议在子类中使用 AUTO_GET_CLASS_NAME 宏
        return std::string(typeid(*this).name());
    }

    // 获取初始化阶段
    virtual SystemInitPhase GetInitPhase() const = 0;

    // 获取依赖的系统类型列表（类型安全，编译时检查）
    virtual std::vector<std::type_index> GetDependencies() const { return {}; }

    // 初始化系统
    // 返回 true 表示成功，false 表示失败
    virtual bool Initialize() = 0;

    // 关闭系统
    virtual void Shutdown() = 0;

private:
    bool m_Initialized = false;
    bool m_Shutdown = false;
};