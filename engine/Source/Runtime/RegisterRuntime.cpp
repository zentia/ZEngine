#include "RegisterRuntime.h"

#include "CommonPCH/pch.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Core/Thread/TaskGraph.h"
#include "Runtime/Core/Thread/ThreadManager.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Module/ModuleManager.h"
#include "Runtime/Function/Particle/ParticleManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif
#if defined(Z_PLATFORM_ANDROID) || defined(__ANDROID__) || defined(Z_PLATFORM_OHOS) || defined(__OHOS__)
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#endif
#if defined(Z_PLATFORM_WINDOWS) || defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
    #if defined(Z_HAS_VULKAN)
        #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
    #endif
#endif
#if defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_IOS) || defined(__APPLE__)
    #include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
#endif
#if defined(__EMSCRIPTEN__)
    #include "Runtime/Function/Render/Interface/WebGL2/WebGL2RHI.h"
#endif
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Serialize/TypeTreeCache.h"
#include "Runtime/File/AsyncReadManagerThreaded.h"
#include "Runtime/File/FileSystem.h"
#include "Runtime/Function/CommonStringTable/CommonStringTable.h"
#include "Runtime/Function/Render/RenderDebugConfig.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderingThread/RenderingThread.h"
#include "Runtime/Function/Render/ShaderRegistry.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UI/Core/WindowUI.h"
#include "Runtime/UI/UISystem.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Resource/Preload/PreloadManager.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResourceManager.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Scripting/ScriptingManager.h"

namespace
{
    void RegisterRuntimeType()
    {
        TypeManager::GetInstance().Initialize();
        // 鑷姩娉ㄥ唽鎵€鏈変娇鐢?IMPLEMENT_REGISTER_CLASS 鐨勭被
        AutoTypeRegistration::ExecuteAllRegistrations();
        TypeManager::GetInstance().InitializeAllTypes();
    }

    void RegisterSystem(PreferredRHI preferred_rhi)
    {
        REGISTER_SYSTEM(ConfigManager);
        REGISTER_SYSTEM(ThreadManager);
        REGISTER_SYSTEM(WindowSystem);
        REGISTER_SYSTEM(PreloadManager);
        REGISTER_SYSTEM(PhysicsManager);
        REGISTER_SYSTEM(WorldManager);
        REGISTER_SYSTEM(InputSystem);
        REGISTER_SYSTEM(ParticleManager);
        // Register exactly one RHI implementation based on platform.
#if defined(Z_PLATFORM_ANDROID) || defined(__ANDROID__) || defined(Z_PLATFORM_OHOS) || defined(__OHOS__)
        (void)preferred_rhi;  // single-RHI platform
        REGISTER_SYSTEM_AS(VulkanRHI, RHI);
#elif defined(Z_PLATFORM_WINDOWS) || defined(_WIN32)
    #if defined(Z_HAS_VULKAN)
        if (preferred_rhi == PreferredRHI::Vulkan)
        {
            LOG_INFO(ZEngine, "RHI selection: Vulkan (requested via --rhi vulkan)");
            REGISTER_SYSTEM_AS(VulkanRHI, RHI);
        }
        else
        {
            LOG_INFO(ZEngine, "RHI selection: DirectX 12 (default on Windows)");
            REGISTER_SYSTEM_AS(DX12RHI, RHI);
        }
    #else
        (void)preferred_rhi;  // Vulkan not compiled in
        REGISTER_SYSTEM_AS(DX12RHI, RHI);
    #endif
#elif defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_IOS) || defined(__APPLE__)
        (void)preferred_rhi;  // single-RHI platform
        REGISTER_SYSTEM_AS(MetalRHI, RHI);
#elif defined(__EMSCRIPTEN__)
        (void)preferred_rhi;  // single-RHI platform
        REGISTER_SYSTEM_AS(ZEngine::WebGL2::WebGL2RHI, RHI);
#else
        (void)preferred_rhi;
#endif
        REGISTER_SYSTEM(RenderSystem);
#if !defined(__APPLE__)
        REGISTER_SYSTEM(DebugDrawManager);
#endif
        REGISTER_SYSTEM(RenderDebugConfig);
        REGISTER_SYSTEM(ModuleManager);
        REGISTER_SYSTEM(UISystem);
        REGISTER_SYSTEM(PlayerSettings);
        REGISTER_SYSTEM(TaskGraph);
        REGISTER_SYSTEM(CommandSystem);
        REGISTER_SYSTEM(ConsoleManager);
        REGISTER_SYSTEM(Application);
        REGISTER_SYSTEM(UserPreferences);
        REGISTER_SYSTEM(ProjectInfo);
        REGISTER_SYSTEM(ScriptRegistry);
        REGISTER_SYSTEM(ShaderRegistry);
        REGISTER_SYSTEM(ResourceManager);
        REGISTER_SYSTEM(ScriptingManager);
    }
}  // namespace

void RegisterCore()
{
    RegisterRuntimeType();
    REGISTER_SYSTEM(CommonStringTable);
    REGISTER_SYSTEM(TypeTreeCache);
    REGISTER_SYSTEM(MemoryManager);
    REGISTER_SYSTEM(LLMTracker);
    REGISTER_SYSTEM(FileSystem);
    REGISTER_SYSTEM(AsyncReadManagerThreaded);
    REGISTER_SYSTEM(ObjectManager);
}

void RegisterRuntime(PreferredRHI preferred_rhi)
{
    RegisterCore();
    RegisterSystem(preferred_rhi);
}

void RegisterPlatform()
{
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);
}
