#pragma once

#include <string>

// Inspector label helpers. Formerly this module also exposed immediate-mode
// ImGui property-row widgets (DrawVecControl / DrawFloatRow / ...), but those
// were superseded by the native ZSlate inspector (ZSlateInspectorWindow) and
// removed along with their only consumer (EditorSerializedFieldDrawer). Only
// the pure string helpers remain -- they carry no ImGui dependency.
namespace EditorPropertyDrawer
{
    std::string MakeDisplayLabel(const char* raw_name);

    // Strips common type suffixes (Component, Parameter, Res) for inspector headers.
    std::string MakeTypeHeaderLabel(const char* raw_type_name);
}
