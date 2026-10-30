#include "RegisterRuntime.h"

#include "CommonPCH/pch.h"
#include "Runtime/Application/Application.h"
#include "Runtime/AssetBundle/AssetBundleManager.h"
#include "Runtime/Core/Thread/TaskGraph.h"
#include "Runtime/Core/Thread/ThreadManager.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/AI/AISystem.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Framework/GameFramework/GameEngine.h"
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
#if (defined(Z_PLATFORM_LINUX) || defined(__linux__)) && !defined(__EMSCRIPTEN__)
    // Desktop Linux uses Vulkan. Emscripten reports Z_PLATFORM_LINUX but has no
    // Vulkan SDK and uses the WebGL2 backend, so it must not pull vulkan.h.
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
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Resource/ResType/Data/Material.h"
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
#include "Runtime/Function/Render/MinimalRenderSystem.h"
#include "Runtime/UI/UISystem.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Resource/Preload/PreloadManager.h"
#include "Runtime/Resource/Addressables/AsyncLoadManager.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResourceManager.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Scripting/ScriptingManager.h"
#include "Runtime/Function/Audio/AudioSystem.h"
#include "Runtime/Function/Audio/Wwise/WwiseAudioSystem.h"
#include "Runtime/Function/Network/NetworkSystem.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"

namespace
{
    /// Old authoring name -> current runtime class.
    ///
    /// The legacy .object.json definitions name components with the authoring
    /// names rather than the runtime class names, so deserialization-by-name
    /// needs these to resolve. Kept as one table so the whole set is visible
    /// and can be checked against the serializer's expectations.
    void RegisterLegacyTypeAliases()
    {
        TypeManager& type_manager = TypeManager::GetInstance();
        type_manager.RegisterClassNameAlias("TransformComponent", TypeOf<Transform>());
        type_manager.RegisterClassNameAlias("MaterialRes", TypeOf<Material>());
    }

    void RegisterRuntimeType()
    {
        TypeManager::GetInstance().Initialize();
        // 鑷姩娉ㄥ唽鎵€鏈変娇鐢?IMPLEMENT_REGISTER_CLASS 鐨勭被
        AutoTypeRegistration::ExecuteAllRegistrations();
        TypeManager::GetInstance().InitializeAllTypes();

        // Legacy .object.json / .level.json refer to components by their old
        // authoring names ("TransformComponent" for Transform, ...). Those
        // aliases must be registered AFTER InitializeAllTypes(): Initialize()
        // calls Reset(), which clears the name->type map, so an alias
        // registered by a file-scope static at load time is silently dropped.
        RegisterLegacyTypeAliases();
    }

    void RegisterSystem(PreferredRHI preferred_rhi)
    {
        REGISTER_SYSTEM(ConfigManager);
        REGISTER_SYSTEM(ThreadManager);
        REGISTER_SYSTEM(WindowSystem);
        REGISTER_SYSTEM(PreloadManager);
        // Addressables-style async loading. Declared with a fully-qualified
        // name because this function lives in the global namespace while the
        // system itself sits in zengine::addressables; SystemRegistry keys on
        // the full type_index, so the namespace is just part of the name.
        REGISTER_SYSTEM(zengine::addressables::AsyncLoadManager);
        REGISTER_SYSTEM(PhysicsManager);
        REGISTER_SYSTEM(WorldManager);
        REGISTER_SYSTEM(AISystem);
        REGISTER_SYSTEM(NavigationSystem);
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
#elif defined(Z_PLATFORM_LINUX) || defined(__linux__)
        // audit #1：Linux 桌面走 Vulkan（Z_HAS_VULKAN 已在 engine/CMakeLists.txt
        // 对 Linux 默认开启）。过去这里落在 #else 空分支，GET_SYSTEM(RHI)==nullptr。
        (void)preferred_rhi;  // single-RHI platform on Linux
        REGISTER_SYSTEM_AS(VulkanRHI, RHI);
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
        // PR-PAK1: mounted .zbundle registry. Registered here (not in the
        // platform pass) so both the editor and standalone players have it --
        // SerializedFile::InitializeRead asks it before touching the disk.
        REGISTER_SYSTEM(AssetBundleManager);
        REGISTER_SYSTEM(UserPreferences);
        REGISTER_SYSTEM(ProjectInfo);
        REGISTER_SYSTEM(ScriptRegistry);
        REGISTER_SYSTEM(ShaderRegistry);
        REGISTER_SYSTEM(ResourceManager);
        REGISTER_SYSTEM(ScriptingManager);
        // Wwise must come *before* AudioSystem: AudioSystem::Initialize() asks
        // whether Wwise took the output, and if so deliberately opens the null
        // device so the two never drive the same speakers. Wwise is a no-op
        // unless ZENGINE_WWISE_ENABLE=1 and the SDK was linked in.
        REGISTER_SYSTEM(WwiseAudioSystem);
        // Audio is registered last so a device failure can never block the rest
        // of the startup sequence: AudioSystem::Initialize() degrades to the
        // null device rather than returning false.
        REGISTER_SYSTEM(AudioSystem);
        // Network holds the same "never fail startup" contract as Audio: it owns
        // curl_global_init()/curl_multi_init() and, when either fails (or the
        // build has no libcurl at all), it still initializes and reports every
        // request as NetworkStatus::Unsupported. Registered in the Core phase,
        // before gameplay, so a game can fire requests from its own
        // Initialize().
        REGISTER_SYSTEM(NetworkSystem);
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
    // Core phase, before gameplay: physics queries read the layer collision
    // matrix and components read `layer` in their own Initialize(). Registered
    // in RegisterCore() (not the platform pass) so RegisterRuntimeLight() --
    // the standalone tools -- gets it too: a tool that deserialises a scene
    // must resolve tags and layers exactly the way the editor does, or it will
    // silently rewrite them.
    REGISTER_SYSTEM(TagLayerManager);
}

void RegisterRuntime(PreferredRHI preferred_rhi)
{
    RegisterCore();
    RegisterSystem(preferred_rhi);
}

// TexPreview 等独立工具专用：只注册渲染/UI 必需系统，不包含
// ProjectInfo / ScriptingManager / ResourceManager 等编辑器系统。
//
// 注意：PlayerSettings 和 UserPreferences 依赖 ProjectInfo，因此这里不注册。
// Application 已修改为支持无 PlayerSettings 的情况（使用默认名称）。
void RegisterRuntimeLight(PreferredRHI preferred_rhi)
{
    RegisterCore();

    // ---- 基础系统（WindowSystem 依赖）-----------------
    REGISTER_SYSTEM(ThreadManager);
    REGISTER_SYSTEM(CommandSystem);      // ConfigManager::GetDependencies()
    REGISTER_SYSTEM(ConfigManager);       // WindowSystem::GetDependencies()
    REGISTER_SYSTEM(Application);        // 主循环需要（已支持无 PlayerSettings）

    // ---- 渲染 / UI 必需系统 -----------------
    REGISTER_SYSTEM(WindowSystem);
    REGISTER_SYSTEM(PreloadManager);
        REGISTER_SYSTEM(PhysicsManager);
        REGISTER_SYSTEM(WorldManager);
        REGISTER_SYSTEM(AISystem);
        REGISTER_SYSTEM(NavigationSystem);
        REGISTER_SYSTEM(InputSystem);

    // 注册一个 RHI 实现（按平台选择）
#if defined(Z_PLATFORM_ANDROID) || defined(__ANDROID__) || defined(Z_PLATFORM_OHOS) || defined(__OHOS__)
    (void)preferred_rhi;
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
        (void)preferred_rhi;
        REGISTER_SYSTEM_AS(DX12RHI, RHI);
    #endif
#elif defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_IOS) || defined(__APPLE__)
    (void)preferred_rhi;
    REGISTER_SYSTEM_AS(MetalRHI, RHI);
#elif defined(__EMSCRIPTEN__)
    (void)preferred_rhi;
    REGISTER_SYSTEM_AS(ZEngine::WebGL2::WebGL2RHI, RHI);
#elif defined(Z_PLATFORM_LINUX) || defined(__linux__)
    // audit #1：Light 变体在 Linux 上同样注册 VulkanRHI。
    (void)preferred_rhi;
    REGISTER_SYSTEM_AS(VulkanRHI, RHI);
#endif

    REGISTER_SYSTEM_AS(MinimalRenderSystem, RenderSystem);
    REGISTER_SYSTEM(DebugDrawManager);
    REGISTER_SYSTEM(RenderDebugConfig);
    REGISTER_SYSTEM(ModuleManager);
    REGISTER_SYSTEM(UISystem);
}

void RegisterPlatform()
{
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);
}

void RegisterGameRuntime(PreferredRHI preferred_rhi)
{
    // Everything the editor-free game needs, plus the game framework layer.
    // GameEngine is registered last: its Initialize() reads the active level and
    // possesses the character, so WorldManager must already be up.
    RegisterRuntime(preferred_rhi);

    // AssetManager lives in RegisterPlatform() rather than in RegisterSystem()
    // so that the editor keeps its own EditorAssetManager: RegisterRuntime() is
    // shared with the editor, and SystemRegistry::RegisterSystemAlias() skips an
    // alias whose base type is already present, so registering RuntimeAssetManager
    // inside RegisterSystem() would silently displace EditorAssetManager.
    // RegisterPlatform() is therefore game-only, and must run before GameEngine
    // because Level::load / WorldManager resolve assets through GET_SYSTEM(AssetManager).
    RegisterPlatform();

    REGISTER_SYSTEM(GameEngine);
}