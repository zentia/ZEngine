#pragma once

#include <cstdint>

using TransformChangeSystemMask = uint64_t;

struct TransformChangeSystemHandle
{
    uint32_t index {0xFFFFFFFFu};

    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    TransformChangeSystemHandle() = default;
    explicit TransformChangeSystemHandle(uint32_t in_index) : index(in_index) {}

    TransformChangeSystemMask Mask() const
    {
        return (index != kInvalidIndex) ? (TransformChangeSystemMask(1) << index) : TransformChangeSystemMask(0);
    }

    bool IsValid() const { return index != kInvalidIndex; }
};

static constexpr int kMaxTransformChangeSystems = 64;
