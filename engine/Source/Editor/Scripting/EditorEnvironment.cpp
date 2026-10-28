#include "EditorEnvironment.h"

#include "Editor/EditorApplication/EditorApplication.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Project/ProjectInfo.h"

eastl::string EditorEnvironment::GetProjectRoot()
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (!project_info)
        return {};
    const std::string root = project_info->GetProjectRoot().generic_string();
    return eastl::string(root.c_str(), root.size());
}

bool EditorEnvironment::IsPlaying()
{
    auto editor = GET_SYSTEM(Editor);
    return editor && editor->isPlaying();
}

bool EditorEnvironment::IsPaused()
{
    auto editor = GET_SYSTEM(Editor);
    return editor && editor->isPaused();
}
