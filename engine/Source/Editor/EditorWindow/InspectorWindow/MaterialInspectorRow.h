#pragma once

#include "Runtime/Core/Math/Vector3.h"

#include <EASTL/string.h>

#include <string>

// UI-agnostic description of one editable material property row, shared by the
// legacy ImGui drawer and the native ZSlate inspector. Each row carries the row
// kind plus raw pointers into the live MaterialRes fields/property structs, so a
// UI layer can render an editor widget and write back without duplicating the
// (fairly involved) shader-property -> material-field mapping rules.
//
// This type intentionally lives in its own header (with NO function
// declarations) so InspectorShaderInspector.cpp can include it at the top --
// next to the InspectorShaderDetail namespace that produces these rows -- WITHOUT
// pulling in the global inspector function declarations (which would collide with
// the same-named functions defined inside that namespace).
enum class MaterialInspectorRowKind
{
    Color,
    Float,
    Bool,
    String
};

struct MaterialInspectorRow
{
    MaterialInspectorRowKind kind {MaterialInspectorRowKind::Float};
    std::string label;

    Vector3* color {nullptr};  // Color: rgb
    float* alpha {nullptr};    // Color: alpha (may be null => no alpha channel)

    float* value {nullptr};    // Float
    float range_min {0.0f};
    float range_max {1.0f};

    bool* boolean {nullptr};       // Bool
    eastl::string* str {nullptr};  // String (texture path)
};
