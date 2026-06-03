#pragma once

#include <functional>
#include <typeindex>
#include <vector>

class EditorUI;
class EditorWindow;

// Unity EditorWindow registration table: one row per built-in panel.
enum class EditorWindowCategory
{
    General,
    Scene,
    Animation,
    Package,
};

struct EditorWindowDescriptor
{
    const char* dock_title {nullptr};
    EditorWindowCategory category {EditorWindowCategory::General};
    bool default_open {true};
    std::type_index type_id {typeid(void)};
    std::function<EditorWindow*(EditorUI*)> factory;
};

class EditorWindowRegistry
{
public:
    static void RegisterBuiltinWindows(EditorUI& editor_ui);
    static const std::vector<EditorWindowDescriptor>& GetBuiltinDescriptors();
};
