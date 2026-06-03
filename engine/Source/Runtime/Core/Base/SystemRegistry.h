#pragma once

#include "EngineSystem.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/generated/engine_log.h"
#include "Runtime/Function/Render/SplashScreen.h"
#include "Singleton.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

#define GET_SYSTEM(system_name)           SystemRegistry::GetInstance().GetSystem<system_name>()
#define REGISTER_SYSTEM(system_name)      SystemRegistry::GetInstance().RegisterSystem(std::make_shared<system_name>())
#define REGISTER_SYSTEM_AS(derived, base) SystemRegistry::GetInstance().RegisterSystemAs<derived, base>()
#define START_SYSTEM()                    SystemRegistry::GetInstance().InitializeAll(true)
#define START_SYSTEM_WITHOUT_UI()         SystemRegistry::GetInstance().InitializeAll(false)
#define SHUTDOWN_SYSTEM()                 SystemRegistry::GetInstance().ShutdownAll();

// ============================================
// 系统注册表（管理所有系统）
// ============================================
class SystemRegistry : public Singleton<SystemRegistry>
{
public:
    // 注册系统
    void RegisterSystem(std::shared_ptr<IEngineSystem> system)
    {
        if (!system)
        {
            LOG_ERROR(ZSystemRegistry, "Attempted to register null system");
            return;
        }

        // 使用类型索引作为唯一标识（类型安全）
        std::type_index type_id(typeid(*system));
        if (m_TypeMap.find(type_id) != m_TypeMap.end())
        {
            LOG_WARNING(ZSystemRegistry, "System type '{}' already registered, skipping", type_id.name());
            return;
        }

        m_Systems.push_back(system);
        m_TypeMap[type_id] = system;

        LOG_INFO(ZSystemRegistry, "Registered system: {} (phase: {})", type_id.name(), static_cast<int>(system->GetInitPhase()));
    }

    // 注册系统并为基类建立别名（无 RTTI 也能按基类获取）
    template<typename Derived, typename Base>
    void RegisterSystemAs()
    {
        static_assert(std::is_base_of_v<IEngineSystem, Derived>, "Derived must inherit from IEngineSystem");
        static_assert(std::is_base_of_v<Base, Derived>, "Derived must inherit from Base");

        auto system = std::make_shared<Derived>();
        RegisterSystem(system);
        RegisterSystemAlias<Base>(system);
    }

    // 初始化所有系统（自动处理依赖关系）
    bool InitializeAll(bool showUI)
    {
        if (m_Initialized)
        {
            LOG_WARNING(ZSystemRegistry, "Systems already initialized");
            return true;
        }
        if (showUI)
        {
            SplashScreen::GetInstance().Initialize();
        }

        // 按阶段和依赖关系排序
        auto sorted_systems = TopologicalSort();

        if (sorted_systems.empty())
        {
            LOG_WARNING(ZSystemRegistry, "No systems registered");
            return true;
        }

        LOG_INFO(ZSystemRegistry, "Initializing {} system(s)...", sorted_systems.size());
        const auto& system_count = sorted_systems.size();
        int index = 0;
        // 按顺序初始化
        for (auto& system : sorted_systems)
        {
            if (system->IsInitialized())
            {
                std::type_index type_id(typeid(*system));
                LOG_DEBUG(ZSystemRegistry, "System '{}' already initialized, skipping", type_id.name());
                continue;
            }

            std::type_index type_id(typeid(*system));

            // 检查依赖是否都已初始化（使用类型索引，类型安全）
            bool dependencies_met = true;
            for (const auto& dep_type : system->GetDependencies())
            {
                auto it = m_TypeMap.find(dep_type);
                if (it == m_TypeMap.end())
                {
                    LOG_FATAL(ZSystemRegistry, "System '{}' depends on a system type which is not registered", type_id.name());
                    dependencies_met = false;
                    break;
                }
                if (!it->second->IsInitialized())
                {
                    LOG_FATAL(ZSystemRegistry, "System '{}' depends on '{}' which is not initialized", type_id.name(), dep_type.name());
                    dependencies_met = false;
                    break;
                }
            }

            if (!dependencies_met)
            {
                LOG_FATAL(ZEngine, "Dependencies not met for system '{}', aborting initialization", type_id.name());
                rollbackInitialization();
                return false;
            }

            // 初始化系统
            LOG_INFO(ZEngine, "Initializing system: {}", type_id.name());
            SplashScreen::GetInstance().Update(((float)index++) / (float)system_count,
                                               std::string("Initializing system: ") + type_id.name());
            SplashScreen::PumpPendingMessages();

            if (!system->Initialize())
            {
                LOG_FATAL(ZEngine, "Failed to initialize system '{}', rolling back", system->GetName());
                rollbackInitialization();
                return false;
            }

            system->SetInitialized(true);
        }
        if (showUI)
        {
            SplashScreen::GetInstance().Shutdown();
        }
        m_Initialized = true;
        LOG_INFO(ZEngine, "All systems initialized successfully");
        return true;
    }

    // 关闭所有系统（按相反顺序）
    void ShutdownAll()
    {
        if (!m_Initialized)
        {
            LOG_DEBUG(ZEngine, "Systems not initialized, skipping shutdown");
            return;
        }

        // 按相反顺序关闭
        auto sorted_systems = TopologicalSort();
        std::reverse(sorted_systems.begin(), sorted_systems.end());

        LOG_INFO(ZEngine, "Shutting down {} system(s)...", sorted_systems.size());

        for (auto& system : sorted_systems)
        {
            if (system->IsInitialized() && !system->IsShutdown())
            {
                std::type_index type_id(typeid(*system));
                LOG_INFO(ZEngine, "Shutting down system: {}", type_id.name());
                system->Shutdown();
                system->SetShutdown(true);
                system->SetInitialized(false);
            }
        }

        m_Initialized = false;
        m_Systems.clear();
        m_TypeMap.clear();
        m_TypeCache.clear();
        LOG_INFO(ZEngine, "All systems shut down");
    }

    // 获取系统（类型安全，带缓存优化）
    template<typename T>
    inline std::shared_ptr<T> GetSystem() const
    {
        // T 必须继承自 IEngineSystem
        static_assert(std::is_base_of_v<IEngineSystem, T>, "T must inherit from IEngineSystem");

        // 先查缓存 O(1)
        std::type_index type_id(typeid(T));
        auto cache_it = m_TypeCache.find(type_id);
        if (cache_it != m_TypeCache.end())
            return std::static_pointer_cast<T>(cache_it->second);

        // 如果有别名注册，直接返回
        auto alias_it = m_TypeMap.find(type_id);
        if (alias_it != m_TypeMap.end())
        {
            m_TypeCache[type_id] = alias_it->second;
            return std::static_pointer_cast<T>(alias_it->second);
        }

        // 缓存未命中，遍历查找 O(n)
        for (const auto& system : m_Systems)
        {
            auto typed_system = std::dynamic_pointer_cast<T>(system);
            if (typed_system)
            {
                // 存入缓存
                m_TypeCache[type_id] = system;
                return typed_system;
            }
        }
        return nullptr;
    }

    // 按类型索引获取系统（用于向后兼容，推荐使用模板版本 getSystem<T>()）
    std::shared_ptr<IEngineSystem> GetSystem(const std::type_index& typeID) const
    {
        auto it = m_TypeMap.find(typeID);
        if (it != m_TypeMap.end())
            return it->second;
        return nullptr;
    }

    // 检查系统是否已注册（使用类型索引）
    template<typename T>
    bool isSystemRegistered() const
    {
        static_assert(std::is_base_of_v<IEngineSystem, T>, "T must inherit from IEngineSystem");
        std::type_index type_id(typeid(T));
        return m_TypeMap.find(type_id) != m_TypeMap.end();
    }

private:
    template<typename Base, typename Derived>
    void RegisterSystemAlias(const std::shared_ptr<Derived>& system)
    {
        static_assert(std::is_base_of_v<IEngineSystem, Base>, "Base must inherit from IEngineSystem");
        static_assert(std::is_base_of_v<Base, Derived>, "Derived must inherit from Base");

        if (!system)
        {
            LOG_ERROR(ZEngine, "Attempted to register null system alias");
            return;
        }

        std::type_index base_id(typeid(Base));
        if (m_TypeMap.find(base_id) != m_TypeMap.end())
        {
            LOG_WARNING(ZEngine, "System alias '{}' already registered, skipping", base_id.name());
            return;
        }

        m_TypeMap[base_id] = system;
        m_TypeCache[base_id] = system;
    }

    // 拓扑排序，确定初始化顺序（使用类型索引）
    std::vector<std::shared_ptr<IEngineSystem>> TopologicalSort() const
    {
        std::vector<std::shared_ptr<IEngineSystem>> result;
        std::unordered_map<std::type_index, bool> visited;
        std::unordered_map<std::type_index, bool> in_stack;

        // 按阶段分组
        std::vector<std::vector<std::shared_ptr<IEngineSystem>>> phase_groups;
        phase_groups.resize(7);  // SystemInitPhase 有 7 个阶段

        for (const auto& system : m_Systems)
        {
            int phase = static_cast<int>(system->GetInitPhase());
            if (phase >= 0 && phase < 7)
                phase_groups[phase].push_back(system);
        }

        // 深度优先搜索，处理依赖关系（使用类型索引）
        std::function<void(std::shared_ptr<IEngineSystem>)> dfs = [&](std::shared_ptr<IEngineSystem> system) {
            std::type_index type_id(typeid(*system));
            if (visited[type_id])
                return;
            if (in_stack[type_id])
            {
                LOG_ERROR(ZEngine, "Circular dependency detected involving system '{}'", type_id.name());
                return;  // 循环依赖检测
            }

            in_stack[type_id] = true;

            // 先处理依赖（在同一阶段内，使用类型索引）
            for (const auto& dep_type : system->GetDependencies())
            {
                auto it = m_TypeMap.find(dep_type);
                if (it != m_TypeMap.end())
                {
                    auto dep = it->second;
                    // 只处理同一阶段的依赖
                    if (dep->GetInitPhase() == system->GetInitPhase())
                        dfs(dep);
                }
            }

            in_stack[type_id] = false;
            visited[type_id] = true;
            result.push_back(system);
        };

        // 按阶段顺序处理
        for (auto& phase_group : phase_groups)
        {
            for (auto& system : phase_group)
            {
                std::type_index type_id(typeid(*system));
                if (!visited[type_id])
                    dfs(system);
            }
        }

        return result;
    }

    // 回滚初始化（初始化失败时调用）
    void rollbackInitialization()
    {
        LOG_ERROR(ZEngine, "Rolling back system initialization...");

        // 按相反顺序关闭已初始化的系统
        auto sorted_systems = TopologicalSort();
        std::reverse(sorted_systems.begin(), sorted_systems.end());

        for (auto& system : sorted_systems)
        {
            if (system->IsInitialized())
            {
                std::type_index type_id(typeid(*system));
                LOG_INFO(ZEngine, "Rolling back system: {}", type_id.name());
                system->Shutdown();
                system->SetInitialized(false);
            }
        }
    }

    std::vector<std::shared_ptr<IEngineSystem>> m_Systems;
    std::unordered_map<std::type_index, std::shared_ptr<IEngineSystem>> m_TypeMap;            // 类型索引映射（用于依赖查找和注册检查）
    mutable std::unordered_map<std::type_index, std::shared_ptr<IEngineSystem>> m_TypeCache;  // 类型缓存
    bool m_Initialized = false;
};