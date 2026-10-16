#pragma once

#include "EngineSystem.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/generated/engine_log.h"
#include "Runtime/Function/Render/SplashScreen.h"
#include "Singleton.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

#define GET_SYSTEM(system_name)           SystemRegistry::GetInstance().GetSystem<system_name>()
#define REGISTER_SYSTEM(system_name)      SystemRegistry::GetInstance().RegisterSystem(new system_name())
#define REGISTER_SYSTEM_AS(derived, base) SystemRegistry::GetInstance().RegisterSystemAs<derived, base>()
#define START_SYSTEM()                    SystemRegistry::GetInstance().InitializeAll(true)
#define START_SYSTEM_WITHOUT_UI()         SystemRegistry::GetInstance().InitializeAll(false)
#define SHUTDOWN_SYSTEM()                 SystemRegistry::GetInstance().ShutdownAll();

// ============================================
// System registry (owns all IEngineSystem instances)
// ============================================
class SystemRegistry : public Singleton<SystemRegistry>
{
public:
    void RegisterSystem(IEngineSystem* system)
    {
        if (system == nullptr)
        {
            LOG_ERROR(ZSystemRegistry, "Attempted to register null system");
            return;
        }

        std::type_index type_id(typeid(*system));
        if (m_TypeMap.find(type_id) != m_TypeMap.end())
        {
            LOG_WARNING(ZSystemRegistry, "System type '{}' already registered, skipping", type_id.name());
            delete system;
            return;
        }

        m_Systems.push_back(system);
        m_TypeMap[type_id] = system;

        LOG_INFO(ZSystemRegistry, "Registered system: {} (phase: {})", type_id.name(), static_cast<int>(system->GetInitPhase()));
    }

    template<typename Derived, typename Base>
    void RegisterSystemAs()
    {
        static_assert(std::is_base_of_v<IEngineSystem, Derived>, "Derived must inherit from IEngineSystem");
        static_assert(std::is_base_of_v<Base, Derived>, "Derived must inherit from Base");

        IEngineSystem* system = new Derived();
        RegisterSystem(system);
        RegisterSystemAlias<Base>(system);
    }

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

        auto sorted_systems = TopologicalSort();

        if (sorted_systems.empty())
        {
            LOG_WARNING(ZSystemRegistry, "No systems registered");
            return true;
        }

        LOG_INFO(ZSystemRegistry, "Initializing {} system(s)...", sorted_systems.size());
        const auto& system_count = sorted_systems.size();
        int index = 0;
        for (IEngineSystem* system : sorted_systems)
        {
            if (system->IsInitialized())
            {
                std::type_index type_id(typeid(*system));
                LOG_DEBUG(ZSystemRegistry, "System '{}' already initialized, skipping", type_id.name());
                continue;
            }

            std::type_index type_id(typeid(*system));

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

    void ShutdownAll()
    {
        if (!m_Initialized)
        {
            LOG_DEBUG(ZEngine, "Systems not initialized, skipping shutdown");
            return;
        }

        auto sorted_systems = TopologicalSort();
        std::reverse(sorted_systems.begin(), sorted_systems.end());

        LOG_INFO(ZEngine, "Shutting down {} system(s)...", sorted_systems.size());

        for (IEngineSystem* system : sorted_systems)
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
        // Systems are deleted below: invalidate every thread's cached system pointer
        // BEFORE any of them can delete the object or observe freed memory.
        m_Generation.fetch_add(1, std::memory_order_acq_rel);
        for (IEngineSystem* system : m_Systems)
        {
            delete system;
        }
        m_Systems.clear();
        m_TypeMap.clear();
        m_TypeCache.clear();
        LOG_INFO(ZEngine, "All systems shut down");
    }

    template<typename T>
    inline T* GetSystem() const
    {
        static_assert(std::is_base_of_v<IEngineSystem, T>, "T must inherit from IEngineSystem");

        // Fast path: a per-thread, write-free cache.
        //
        // Why this exists: GetSystem<T>() used to populate the shared m_TypeCache on
        // every cache miss. m_TypeCache is an unsynchronised std::unordered_map, so a
        // background thread (e.g. AssetRegistry::ScanAssetsAsync freeing a rapidjson
        // document through MemoryManager::Free -> GET_SYSTEM(LLMTracker)) could write
        // to it while the main thread was registering systems / rehashing it. The
        // in-flight lookup then ran type_info::operator== against a type_index whose
        // storage had been released, and strcmp dereferenced a dangling pointer
        // (0xC0000005). See crash_stack.txt: strcmp <- _Hash::_Find_last <- GetSystem.
        //
        // The per-thread slot below keeps hot callers (allocators) off the shared map
        // entirely. It is tagged with the registry generation so that a thread which
        // cached a system before ShutdownAll() / re-registration does not hand back a
        // dangling pointer afterwards.
        static thread_local IEngineSystem* t_cached = nullptr;
        static thread_local uint32_t t_cached_generation = 0;
        if (t_cached != nullptr && t_cached_generation == m_Generation)
        {
            return static_cast<T*>(t_cached);
        }
        t_cached = nullptr;

        std::type_index type_id(typeid(T));
        auto cache_it = m_TypeCache.find(type_id);
        if (cache_it != m_TypeCache.end())
        {
            t_cached = cache_it->second;
            t_cached_generation = m_Generation;
            return static_cast<T*>(cache_it->second);
        }

        auto alias_it = m_TypeMap.find(type_id);
        if (alias_it != m_TypeMap.end())
        {
            m_TypeCache[type_id] = alias_it->second;
            t_cached = alias_it->second;
            t_cached_generation = m_Generation;
            return static_cast<T*>(alias_it->second);
        }

        for (IEngineSystem* system : m_Systems)
        {
            if (T* typed_system = dynamic_cast<T*>(system))
            {
                m_TypeCache[type_id] = system;
                t_cached = system;
                t_cached_generation = m_Generation;
                return typed_system;
            }
        }
        return nullptr;
    }

    IEngineSystem* GetSystem(const std::type_index& typeID) const
    {
        auto it = m_TypeMap.find(typeID);
        if (it != m_TypeMap.end())
            return it->second;
        return nullptr;
    }

    template<typename T>
    bool isSystemRegistered() const
    {
        static_assert(std::is_base_of_v<IEngineSystem, T>, "T must inherit from IEngineSystem");
        std::type_index type_id(typeid(T));
        return m_TypeMap.find(type_id) != m_TypeMap.end();
    }

private:
    template<typename Base>
    void RegisterSystemAlias(IEngineSystem* system)
    {
        static_assert(std::is_base_of_v<IEngineSystem, Base>, "Base must inherit from IEngineSystem");

        if (system == nullptr)
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

    std::vector<IEngineSystem*> TopologicalSort() const
    {
        std::vector<IEngineSystem*> result;
        std::unordered_map<std::type_index, bool> visited;
        std::unordered_map<std::type_index, bool> in_stack;

        std::vector<std::vector<IEngineSystem*>> phase_groups;
        phase_groups.resize(7);

        for (IEngineSystem* system : m_Systems)
        {
            int phase = static_cast<int>(system->GetInitPhase());
            if (phase >= 0 && phase < 7)
                phase_groups[phase].push_back(system);
        }

        std::function<void(IEngineSystem*)> dfs = [&](IEngineSystem* system) {
            std::type_index type_id(typeid(*system));
            if (visited[type_id])
                return;
            if (in_stack[type_id])
            {
                LOG_ERROR(ZEngine, "Circular dependency detected involving system '{}'", type_id.name());
                return;
            }

            in_stack[type_id] = true;

            for (const auto& dep_type : system->GetDependencies())
            {
                auto it = m_TypeMap.find(dep_type);
                if (it != m_TypeMap.end())
                {
                    IEngineSystem* dep = it->second;
                    if (dep->GetInitPhase() == system->GetInitPhase())
                        dfs(dep);
                }
            }

            in_stack[type_id] = false;
            visited[type_id] = true;
            result.push_back(system);
        };

        for (auto& phase_group : phase_groups)
        {
            for (IEngineSystem* system : phase_group)
            {
                std::type_index type_id(typeid(*system));
                if (!visited[type_id])
                    dfs(system);
            }
        }

        return result;
    }

    void rollbackInitialization()
    {
        LOG_ERROR(ZEngine, "Rolling back system initialization...");

        auto sorted_systems = TopologicalSort();
        std::reverse(sorted_systems.begin(), sorted_systems.end());

        for (IEngineSystem* system : sorted_systems)
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

    std::vector<IEngineSystem*> m_Systems;
    std::unordered_map<std::type_index, IEngineSystem*> m_TypeMap;
    mutable std::unordered_map<std::type_index, IEngineSystem*> m_TypeCache;
    // Bumped whenever the set of live systems changes (ShutdownAll). Threads compare
    // their cached system against this to avoid using a destroyed instance.
    mutable std::atomic<uint32_t> m_Generation {0};
    bool m_Initialized = false;
};
