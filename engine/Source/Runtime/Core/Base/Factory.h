#pragma once

#include "Runtime/Core/Base/Singleton.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 工厂接口基类
 *
 * 所有工厂类都应该继承此接口，提供统一的创建接口。
 */
template<typename BaseType>
class IFactory
{
public:
    virtual ~IFactory() = default;

    /**
     * @brief 创建产品实例
     * @return 产品实例的智能指针
     */
    virtual std::shared_ptr<BaseType> create() = 0;

    /**
     * @brief 获取工厂名称
     * @return 工厂名称
     */
    virtual std::string GetName() const = 0;
};

/**
 * @brief 工厂注册表
 *
 * 单例模式的工厂注册表，用于管理和注册所有工厂。
 * 支持通过字符串ID注册和创建产品。
 */
template<typename BaseType>
class FactoryRegistry : public Singleton<FactoryRegistry<BaseType>>
{
public:
    /**
     * @brief 注册工厂
     * @param name 工厂名称（唯一标识符）
     * @param factory 工厂实例的智能指针
     * @return 是否注册成功（如果名称已存在则返回false）
     */
    bool registerFactory(const std::string& name, std::shared_ptr<IFactory<BaseType>> factory)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Factories.find(name) != m_Factories.end())
        {
            return false;  // 名称已存在
        }
        m_Factories[name] = factory;
        return true;
    }

    /**
     * @brief 注销工厂
     * @param name 工厂名称
     * @return 是否注销成功
     */
    bool unregisterFactory(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Factories.erase(name) > 0;
    }

    /**
     * @brief 通过名称创建产品
     * @param name 工厂名称
     * @return 产品实例的智能指针，如果工厂不存在则返回nullptr
     */
    std::shared_ptr<BaseType> create(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Factories.find(name);
        if (it != m_Factories.end())
        {
            return it->second->create();
        }
        return nullptr;
    }

    /**
     * @brief 检查工厂是否已注册
     * @param name 工厂名称
     * @return 是否已注册
     */
    bool isRegistered(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Factories.find(name) != m_Factories.end();
    }

    /**
     * @brief 获取所有已注册的工厂名称
     * @return 工厂名称列表
     */
    std::vector<std::string> getRegisteredNames() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::vector<std::string> names;
        names.reserve(m_Factories.size());
        for (const auto& pair : m_Factories)
        {
            names.push_back(pair.first);
        }
        return names;
    }

    /**
     * @brief 清空所有注册的工厂
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Factories.clear();
    }

    /**
     * @brief 获取已注册工厂的数量
     * @return 工厂数量
     */
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Factories.size();
    }

private:
    std::unordered_map<std::string, std::shared_ptr<IFactory<BaseType>>> m_Factories;
    mutable std::mutex m_Mutex;
};

/**
 * @brief 工厂创建器模板类
 *
 * 用于创建具体产品类型的工厂实现。
 * 自动处理产品的创建逻辑。
 */
template<typename BaseType, typename DerivedType>
class FactoryCreator : public IFactory<BaseType>
{
public:
    /**
     * @brief 构造函数
     * @param name 工厂名称
     */
    explicit FactoryCreator(const std::string& name)
        : m_Name(name) {}

    /**
     * @brief 创建产品实例
     * @return 产品实例的智能指针
     */
    std::shared_ptr<BaseType> create() override { return std::make_shared<DerivedType>(); }

    /**
     * @brief 获取工厂名称
     * @return 工厂名称
     */
    std::string GetName() const override { return m_Name; }

private:
    std::string m_Name;
};

/**
 * @brief 带参数的工厂创建器模板类
 *
 * 用于创建需要初始化参数的产品类型。
 */
template<typename BaseType, typename DerivedType, typename InitInfo>
class FactoryCreatorWithInit : public IFactory<BaseType>
{
public:
    /**
     * @brief 构造函数
     * @param name 工厂名称
     * @param init_func 初始化函数，用于创建产品时传递初始化参数
     */
    explicit FactoryCreatorWithInit(const std::string& name, std::function<void(DerivedType*)> init_func = nullptr)
        : m_Name(name), m_InitFunc(init_func)
    {
    }

    /**
     * @brief 创建产品实例
     * @return 产品实例的智能指针
     */
    std::shared_ptr<BaseType> create() override
    {
        auto instance = std::make_shared<DerivedType>();
        if (m_InitFunc)
        {
            m_InitFunc(instance.get());
        }
        return instance;
    }

    /**
     * @brief 获取工厂名称
     * @return 工厂名称
     */
    std::string GetName() const override { return m_Name; }

private:
    std::string m_Name;
    std::function<void(DerivedType*)> m_InitFunc;
};

/**
 * @brief 工厂注册辅助类
 *
 * 用于在全局作用域自动注册工厂。
 * 在构造函数中自动注册，在析构函数中自动注销。
 */
template<typename BaseType, typename DerivedType>
class FactoryRegistrar
{
public:
    /**
     * @brief 构造函数，自动注册工厂
     * @param name 工厂名称
     */
    explicit FactoryRegistrar(const std::string& name)
        : m_Name(name)
    {
        auto factory = std::make_shared<FactoryCreator<BaseType, DerivedType>>(name);
        FactoryRegistry<BaseType>::GetInstance().registerFactory(name, factory);
    }

    /**
     * @brief 析构函数，自动注销工厂
     */
    ~FactoryRegistrar() { FactoryRegistry<BaseType>::GetInstance().unregisterFactory(m_Name); }

    // 禁止拷贝
    FactoryRegistrar(const FactoryRegistrar&) = delete;
    FactoryRegistrar& operator=(const FactoryRegistrar&) = delete;

private:
    std::string m_Name;
};

/**
 * @brief 自动注册工厂宏
 *
 * 在全局作用域使用此宏可以自动注册工厂。
 * 注意：需要在对应的.cpp文件中使用，避免多重定义。
 *
 * @param BaseType 基类类型
 * @param DerivedType 派生类类型
 * @param FactoryName 工厂名称（字符串）
 *
 * @example
 * // 在 MyClass.cpp 中
 * REGISTER_FACTORY(IRenderPass, MyRenderPass, "MyRenderPass");
 */
#define REGISTER_FACTORY(BaseType, DerivedType, FactoryName) \
    static FactoryRegistrar<BaseType, DerivedType> g_##DerivedType##_registrar(FactoryName)

/**
 * @brief 通过名称创建产品的便捷宏
 *
 * @param BaseType 基类类型
 * @param FactoryName 工厂名称
 *
 * @example
 * auto pass = CREATE_FROM_FACTORY(IRenderPass, "MyRenderPass");
 */
#define CREATE_FROM_FACTORY(BaseType, FactoryName) FactoryRegistry<BaseType>::GetInstance().create(FactoryName)
