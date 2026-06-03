#pragma once

#include <string>

/**
 * @brief Interface for game modules
 * Game modules are loaded and initialized by the engine during startup
 */
class IGameModule
{
public:
    virtual ~IGameModule() = default;

    /**
     * @brief Get the name of this module
     */
    virtual const char* GetName() const = 0;

    /**
     * @brief Initialize the module
     * Called after engine systems are initialized
     */
    virtual void Initialize() = 0;

    /**
     * @brief Shutdown the module
     * Called before engine systems are shut down
     */
    virtual void Shutdown() = 0;

    /**
     * @brief Tick the module (optional)
     * Called every frame if the module needs per-frame updates
     * @param delta_time Time since last frame
     */
    virtual void Tick(float delta_time) {}
};