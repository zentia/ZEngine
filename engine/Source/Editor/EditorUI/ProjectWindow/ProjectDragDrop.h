#pragma once

#include "Editor/EditorUI/ProjectWindow/ProjectWindowContext.h"

#include <string>
#include <vector>

namespace ProjectDragDrop
{
    void OnOsFilesDropped(ProjectWindowContext& ctx, const std::vector<std::string>& paths);
    void ExecutePendingOsDropImports(ProjectWindowContext& ctx);
}
