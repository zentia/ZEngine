#include "ModuleManager.h"

#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/Render/RenderSystem.h"

std::vector<std::type_index> ModuleManager::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(RenderSystem), GET_SYSTEM_TYPE(WorldManager), GET_SYSTEM_TYPE(PhysicsManager)};
}

void ModuleManager::RegisterModule(std::shared_ptr<IGameModule> module)
{
    if (!module)
    {
        LOG_ERROR(ZEngine, "Attempted to register null module");
        return;
    }

    const char* module_name = module->GetName();
    if (module_name == nullptr || strlen(module_name) == 0)
    {
        LOG_ERROR(ZEngine, "Attempted to register module with empty name");
        return;
    }

    std::string name(module_name);

    // Check if module with same name already exists
    if (m_ModuleMap.find(name) != m_ModuleMap.end())
    {
        LOG_WARNING(ZEngine, "Module '{}' is already registered, skipping duplicate registration", module_name);
        return;
    }

    m_Modules.push_back(module);
    m_ModuleMap[name] = module;

    LOG_INFO(ZEngine, "Registered game module: {}", module_name);
}

void ModuleManager::InitializeAllModules()
{
    if (m_Initialized)
    {
        LOG_WARNING(ZEngine, "Modules already initialized");
        return;
    }

    LOG_INFO(ZEngine, "Initializing {} game module(s)...", m_Modules.size());

    for (auto& module : m_Modules)
    {
        if (module)
        {
            try
            {
                LOG_INFO(ZEngine, "Initializing module: {}", module->GetName());
                module->Initialize();
            }
            catch (const std::exception& e)
            {
                LOG_ERROR(ZEngine, "Failed to initialize module '{}': {}", module->GetName(), e.what());
            }
            catch (...)
            {
                LOG_ERROR(ZEngine, "Failed to initialize module '{}': Unknown exception", module->GetName());
            }
        }
    }

    m_Initialized = true;
    LOG_INFO(ZEngine, "All game modules initialized");
}

void ModuleManager::ShutdownAllModules()
{
    if (!m_Initialized)
    {
        return;
    }

    LOG_INFO(ZEngine, "Shutting down {} game module(s)...", m_Modules.size());

    // Shutdown in reverse order
    for (auto it = m_Modules.rbegin(); it != m_Modules.rend(); ++it)
    {
        auto& module = *it;
        if (module)
        {
            try
            {
                LOG_INFO(ZEngine, "Shutting down module: {}", module->GetName());
                module->Shutdown();
            }
            catch (const std::exception& e)
            {
                LOG_ERROR(ZEngine, "Failed to shutdown module '{}': {}", module->GetName(), e.what());
            }
            catch (...)
            {
                LOG_ERROR(ZEngine, "Failed to shutdown module '{}': Unknown exception", module->GetName());
            }
        }
    }

    m_Initialized = false;
    LOG_INFO(ZEngine, "All game modules shut down");
}

void ModuleManager::TickAllModules(float delta_time)
{
    if (!m_Initialized)
    {
        return;
    }

    for (auto& module : m_Modules)
    {
        if (module)
        {
            try
            {
                module->Tick(delta_time);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR(ZEngine, "Error ticking module '{}': {}", module->GetName(), e.what());
            }
            catch (...)
            {
                LOG_ERROR(ZEngine, "Error ticking module '{}': Unknown exception", module->GetName());
            }
        }
    }
}

std::shared_ptr<IGameModule> ModuleManager::GetModule(const std::string& name) const
{
    auto it = m_ModuleMap.find(name);
    if (it != m_ModuleMap.end())
    {
        return it->second;
    }
    return nullptr;
}