#pragma once

#include <cstdint>

struct TransformHierarchy;

struct TransformAccessReadOnly
{
    TransformHierarchy* hierarchy {nullptr};
    uint32_t index {0};

    TransformAccessReadOnly() = default;
    TransformAccessReadOnly(TransformHierarchy* in_hierarchy, uint32_t in_index)
        : hierarchy(in_hierarchy), index(in_index)
    {
    }

    bool IsValid() const { return hierarchy != nullptr; }
};

struct TransformAccess : TransformAccessReadOnly
{
    TransformAccess() = default;
    TransformAccess(TransformHierarchy* in_hierarchy, uint32_t in_index)
        : TransformAccessReadOnly(in_hierarchy, in_index)
    {
    }

    static TransformAccess Null() { return TransformAccess(); }
};
