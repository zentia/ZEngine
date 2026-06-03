#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Module/GameModule.h"

#include <memory>

/**
 * @brief Manages game modules registration and lifecycle
 */
class ModuleManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "ModuleManager"; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override { return true; }
    void Shutdown() override {}
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    /**
     * @brief Register a game module
     * Modules should call this during static initialization
     * @param module Shared pointer to the module instance
     */
    void RegisterModule(std::shared_ptr<IGameModule> module);

    /**
     * @brief Initialize all registered modules
     * Called by the engine during startup
     */
    void InitializeAllModules();

    /**
     * @brief Shutdown all registered modules
     * Called by the engine during shutdown
     */
    void ShutdownAllModules();

    /**
     * @brief Tick all registered modules
     * Called every frame by the engine
     * @param delta_time Time since last frame
     */
    void TickAllModules(float delta_time);

    /**
     * @brief Get a module by name
     * @param name Module name
     * @return Module instance or nullptr if not found
     */
    std::shared_ptr<IGameModule> GetModule(const std::string& name) const;

    /**
     * @brief Get all registered modules
     */
    const std::vector<std::shared_ptr<IGameModule>>& getAllModules() const { return m_Modules; }

private:
    std::vector<std::shared_ptr<IGameModule>> m_Modules;
    std::unordered_map<std::string, std::shared_ptr<IGameModule>> m_ModuleMap;
    bool m_Initialized = false;
};