// 改进示例：统一的系统接口和注册机制
// 这是一个参考实现，展示如何改进当前的 global_context

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>

namespace Z
{
    // ============================================
    // 1. 系统初始化阶段枚举
    // ============================================
    enum class SystemInitPhase : int
    {
        PreInit = 0,      // 配置、文件系统、日志等基础系统
        Core = 1,         // 线程、内存管理等核心系统
        Resource = 2,     // 资源管理（AssetManager, PreloadManager）
        Platform = 3,     // 平台相关（Window, Input）
        Rendering = 4,    // 渲染系统（RHI, RenderSystem）
        Gameplay = 5,     // 游戏系统（World, Physics, Particle）
        PostInit = 6      // 最后初始化（ModuleManager, Canvas）
    };

    // ============================================
    // 2. 统一的系统接口
    // ============================================
    class IEngineSystem
    {
    public:
        virtual ~IEngineSystem() = default;
        
        // 获取系统名称（用于依赖解析和日志）
        virtual std::string getName() const = 0;
        
        // 获取初始化阶段
        virtual SystemInitPhase getInitPhase() const = 0;
        
        // 获取依赖的系统名称列表
        virtual std::vector<std::string> getDependencies() const { return {}; }
        
        // 初始化系统
        // 返回 true 表示成功，false 表示失败
        virtual bool initialize() = 0;
        
        // 关闭系统
        virtual void shutdown() = 0;
        
        // 获取系统是否已初始化
        bool isInitialized() const { return m_initialized; }
        
        // 获取系统是否已关闭
        bool isShutdown() const { return m_shutdown; }
        
    protected:
        void setInitialized(bool value) { m_initialized = value; }
        void setShutdown(bool value) { m_shutdown = value; }
        
    private:
        bool m_initialized = false;
        bool m_shutdown = false;
    };

    // ============================================
    // 3. 系统注册表（管理所有系统）
    // ============================================
    class SystemRegistry
    {
    public:
        static SystemRegistry& getInstance()
        {
            static SystemRegistry instance;
            return instance;
        }
        
        // 注册系统
        void registerSystem(std::shared_ptr<IEngineSystem> system)
        {
            if (!system)
                return;
                
            std::string name = system->getName();
            if (m_system_map.find(name) != m_system_map.end())
            {
                // 系统已注册，可以选择替换或报错
                return;
            }
            
            m_systems.push_back(system);
            m_system_map[name] = system;
        }
        
        // 初始化所有系统（自动处理依赖关系）
        bool initializeAll()
        {
            // 按阶段和依赖关系排序
            auto sorted_systems = topologicalSort();
            
            // 按顺序初始化
            for (auto& system : sorted_systems)
            {
                if (system->isInitialized())
                    continue;
                
                // 检查依赖是否都已初始化
                for (const auto& dep_name : system->getDependencies())
                {
                    auto it = m_system_map.find(dep_name);
                    if (it == m_system_map.end() || !it->second->isInitialized())
                    {
                        // 依赖未满足，记录错误
                        return false;
                    }
                }
                
                // 初始化系统
                if (!system->initialize())
                {
                    // 初始化失败，回滚已初始化的系统
                    rollbackInitialization();
                    return false;
                }
                
                system->setInitialized(true);
            }
            
            return true;
        }
        
        // 关闭所有系统（按相反顺序）
        void shutdownAll()
        {
            // 按相反顺序关闭
            auto sorted_systems = topologicalSort();
            std::reverse(sorted_systems.begin(), sorted_systems.end());
            
            for (auto& system : sorted_systems)
            {
                if (system->isInitialized() && !system->isShutdown())
                {
                    system->shutdown();
                    system->setShutdown(true);
                }
            }
        }
        
        // 获取系统（类型安全）
        template<typename T>
        std::shared_ptr<T> getSystem() const
        {
            // T 必须继承自 IEngineSystem
            static_assert(std::is_base_of_v<IEngineSystem, T>, 
                         "T must inherit from IEngineSystem");
            
            for (const auto& system : m_systems)
            {
                auto typed_system = std::dynamic_pointer_cast<T>(system);
                if (typed_system)
                    return typed_system;
            }
            return nullptr;
        }
        
        // 按名称获取系统
        std::shared_ptr<IEngineSystem> getSystem(const std::string& name) const
        {
            auto it = m_system_map.find(name);
            if (it != m_system_map.end())
                return it->second;
            return nullptr;
        }
        
    private:
        // 拓扑排序，确定初始化顺序
        std::vector<std::shared_ptr<IEngineSystem>> topologicalSort()
        {
            std::vector<std::shared_ptr<IEngineSystem>> result;
            std::unordered_map<std::string, bool> visited;
            std::unordered_map<std::string, bool> in_stack;
            
            // 按阶段分组
            std::vector<std::vector<std::shared_ptr<IEngineSystem>>> phase_groups;
            phase_groups.resize(7); // SystemInitPhase 有 7 个阶段
            
            for (const auto& system : m_systems)
            {
                int phase = static_cast<int>(system->getInitPhase());
                if (phase >= 0 && phase < 7)
                    phase_groups[phase].push_back(system);
            }
            
            // 按阶段排序，每个阶段内按依赖关系排序
            std::function<void(std::shared_ptr<IEngineSystem>)> dfs = 
                [&](std::shared_ptr<IEngineSystem> system) {
                    std::string name = system->getName();
                    if (visited[name])
                        return;
                    if (in_stack[name])
                        return; // 循环依赖检测
                    
                    in_stack[name] = true;
                    
                    // 先处理依赖
                    for (const auto& dep_name : system->getDependencies())
                    {
                        auto dep = getSystem(dep_name);
                        if (dep)
                            dfs(dep);
                    }
                    
                    in_stack[name] = false;
                    visited[name] = true;
                    result.push_back(system);
                };
            
            // 按阶段顺序处理
            for (auto& phase_group : phase_groups)
            {
                for (auto& system : phase_group)
                {
                    if (!visited[system->getName()])
                        dfs(system);
                }
            }
            
            return result;
        }
        
        // 回滚初始化（初始化失败时调用）
        void rollbackInitialization()
        {
            // 按相反顺序关闭已初始化的系统
            auto sorted_systems = topologicalSort();
            std::reverse(sorted_systems.begin(), sorted_systems.end());
            
            for (auto& system : sorted_systems)
            {
                if (system->isInitialized())
                {
                    system->shutdown();
                    system->setInitialized(false);
                }
            }
        }
        
        std::vector<std::shared_ptr<IEngineSystem>> m_systems;
        std::unordered_map<std::string, std::shared_ptr<IEngineSystem>> m_system_map;
    };

    // ============================================
    // 4. 改进的 GlobalContext
    // ============================================
    class RuntimeGlobalContext
    {
    public:
        RuntimeGlobalContext() = default;
        ~RuntimeGlobalContext() = default;
        
        // 初始化所有系统
        bool startSystems()
        {
            if (m_initialized)
                return true;
            
            // 注册所有系统
            registerAllSystems();
            
            // 初始化所有系统
            bool success = SystemRegistry::getInstance().initializeAll();
            m_initialized = success;
            
            return success;
        }
        
        // 关闭所有系统
        void shutdownSystems()
        {
            if (!m_initialized)
                return;
            
            SystemRegistry::getInstance().shutdownAll();
            m_initialized = false;
        }
        
        // 获取系统（类型安全，推荐使用）
        template<typename T>
        std::shared_ptr<T> getSystem() const
        {
            return SystemRegistry::getInstance().getSystem<T>();
        }
        
        // 向后兼容的访问器（可选，用于平滑迁移）
        std::shared_ptr<InputSystem> getInputSystem() const
        {
            return getSystem<InputSystem>();
        }
        
        std::shared_ptr<FileSystem> getFileSystem() const
        {
            return getSystem<FileSystem>();
        }
        
        // ... 其他系统的访问器
        
    private:
        // 注册所有系统（可以在这里集中管理）
        void registerAllSystems()
        {
            auto& registry = SystemRegistry::getInstance();
            
            // 注册各个系统
            // registry.registerSystem(std::make_shared<ConfigSystem>());
            // registry.registerSystem(std::make_shared<FileSystem>());
            // registry.registerSystem(std::make_shared<ThreadManagerSystem>());
            // ... 等等
        }
        
        bool m_initialized = false;
    };

    // ============================================
    // 5. 系统实现示例（以 InputSystem 为例）
    // ============================================
    /*
    class InputSystem : public IEngineSystem
    {
    public:
        std::string getName() const override { return "InputSystem"; }
        
        SystemInitPhase getInitPhase() const override 
        { 
            return SystemInitPhase::Platform; 
        }
        
        std::vector<std::string> getDependencies() const override
        {
            return {"WindowSystem"}; // 依赖窗口系统
        }
        
        bool initialize() override
        {
            // 初始化逻辑
            // 如果失败返回 false
            return true;
        }
        
        void shutdown() override
        {
            // 清理逻辑
        }
    };
    */
}

