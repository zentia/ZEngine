#include "RegisterEditor.h"

#include "Editor/AssetPipeline/AssetImporter.h"
#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorUI/EditorUI.h"
#include "Editor/PackageManager/PackageManager.h"
#include "Editor/Prefab/PrefabSceneBinding.h"
#include "Editor/RenderDoc/RenderDocLoader.h"
#include "Editor/Scripting/TypeScriptCompiler.h"
#include "Editor/Undo/EditorUndo.h"
#include "Runtime/Function/Framework/GameFramework/GameEngine.h"

namespace
{
    void RegisterEditorSystem()
    {
#if defined(_WIN32)
        REGISTER_SYSTEM(RenderDocLoader);
#endif
        REGISTER_SYSTEM(EditorInputManager);
        REGISTER_SYSTEM(EditorSceneManager);
        REGISTER_SYSTEM(EditorUndoSystem);
        REGISTER_SYSTEM(Editor);
        REGISTER_SYSTEM_AS(EditorAssetManager, AssetManager);
        REGISTER_SYSTEM(TypeScriptCompiler);
        REGISTER_SYSTEM(PackageManager);
        // PIE 的游戏层。RegisterGameRuntime()（独立进程专用）也会注册它，
        // 但编辑器不走那条路，而 PIE 需要 GameInstance / LocalPlayer /
        // PlayerController。注册本身是无害的：GameEngine::Initialize() 只在
        // 有活动关卡时才去接管角色，编辑器刚启动时没有关卡，所以不会抢走
        // 编辑器正在操作的实体。真正的接管发生在 BeginPIE()。
        REGISTER_SYSTEM(GameEngine);
    }
}  // namespace

void RegisterEditor()
{
    RegisterEditorSystem();
    // Scene save / load carry the editor-only PrefabInstance bookkeeping.
    PrefabSceneBinding::Install();
}