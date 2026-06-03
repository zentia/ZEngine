#pragma once

#include "Editor/EditorUI/ContentBrowser/ContentBrowserContext.h"

#include <string>
#include <vector>

namespace ContentBrowserDragDrop
{
    void OnOsFilesDropped(ContentBrowserContext& ctx, const std::vector<std::string>& paths);
    void ExecutePendingOsDropImports(ContentBrowserContext& ctx);
}
