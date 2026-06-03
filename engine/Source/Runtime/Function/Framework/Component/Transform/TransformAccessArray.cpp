#include "Runtime/Function/Framework/Component/Transform/TransformAccessArray.h"

#include "Runtime/Function/Framework/Component/Transform/Transform.h"

TransformAccessArray* TransformAccessArray::Create(uint32_t capacity)
{
    return new TransformAccessArray(capacity);
}

void TransformAccessArray::Destroy()
{
    delete this;
}

TransformAccessArray::TransformAccessArray(uint32_t capacity) : m_Capacity(capacity), m_Accesses(capacity)
{
}

void TransformAccessArray::SetTransform(uint32_t index, Transform* transform)
{
    if (index >= m_Capacity || transform == nullptr)
    {
        return;
    }

    m_Accesses[index] = transform->GetTransformAccessReadOnly();
}
