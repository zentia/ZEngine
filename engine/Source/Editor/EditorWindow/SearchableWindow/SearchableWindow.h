#pragma once

#include "Editor/EditorWindow/EditorWindow.h"

namespace Runtime
{
    class SearchableWindow : public EditorWindow
    {
    public:
        explicit SearchableWindow(EditorUI* editor_ui, const char* name);
    };
}  // namespace Runtime