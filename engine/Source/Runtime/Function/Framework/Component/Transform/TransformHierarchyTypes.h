#pragma once

#include "Runtime/Core/Math/LocalTransform.h"
#include "Runtime/Function/Framework/Component/Transform/TransformAccess.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeSystemMask.h"

class Transform;

struct TransformHierarchy
{
    uint32_t transform_capacity {0};
    int32_t first_free_index {0};

    LocalTransform* local_transforms {nullptr};
    int32_t* parent_indices {nullptr};
    uint32_t* deep_child_count {nullptr};
    Transform** transform_pointers {nullptr};
    TransformChangeSystemMask* system_changed {nullptr};
    TransformChangeSystemMask* system_interested {nullptr};
    int32_t* next_indices {nullptr};
    int32_t* prev_indices {nullptr};

    int32_t change_dispatch_index {-1};
    TransformChangeSystemMask combined_system_changed {0};
    TransformChangeSystemMask combined_system_interest {0};
};

enum TransformType : uint8_t
{
    kNoScaleTransform = 0,
    kUniformScaleTransform = 1 << 0,
    kNonUniformScaleTransform = 1 << 1,
    kOddNegativeScaleTransform = 1 << 2,
};
