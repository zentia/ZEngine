#include "RegisterEditor.h"

#include "Editor/AssetPipeline/AssetImporter.h"
#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorUI/EditorUI.h"
#include "Editor/PackageManager/PackageManager.h"
#include "Editor/Scripting/TypeScriptCompiler.h"

namespace
{
    void RegisterEditorSystem()
    {
        REGISTER_SYSTEM(EditorInputManager);
        REGISTER_SYSTEM(EditorSceneManager);
        REGISTER_SYSTEM(Editor);
        REGISTER_SYSTEM_AS(EditorAssetManager, AssetManager);
        REGISTER_SYSTEM(TypeScriptCompiler);
        REGISTER_SYSTEM(PackageManager);
    }
}  // namespace

void RegisterEditor()
{
    RegisterEditorSystem();
}