#pragma once

#include "Runtime/Function/Framework/Component/Transform/TransformAccess.h"

#include <cstdint>
#include <vector>

class Transform;

/// Batch of TransformAccess entries (Unity TransformAccessArray subset).
class TransformAccessArray
{
public:
    static TransformAccessArray* Create(uint32_t capacity);
    void Destroy();

    uint32_t GetCapacity() const { return m_Capacity; }
    void SetTransform(uint32_t index, Transform* transform);

    const std::vector<TransformAccessReadOnly>& GetAccessArray() const { return m_Accesses; }

private:
    explicit TransformAccessArray(uint32_t capacity);

    uint32_t m_Capacity {0};
    std::vector<TransformAccessReadOnly> m_Accesses;
};
