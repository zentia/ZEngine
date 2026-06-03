#pragma once

#include "core/base/Macro.h"

#include <stdint.h>

enum class StringRepresentation : uint8_t
{
    HEAP,
    EMBEDDED,
    EXTERNAL,
};

template<typename TChar>
class StringStorageDefault
{
public:
    using ValueType = TChar;
    using SizeType = size_t;
    using Pointer = ValueType*;
    using ConstPointer = const ValueType*;

    Pointer GetData() { return (m_Datarepr == StringRepresentation::EMBEDDED) ? embedded.data : heap.data; }

    ConstPointer GetData() const { return (m_Datarepr == StringRepresentation::EMBEDDED) ? embedded.data : heap.data; }

    SizeType Size() const
    {
        return (m_Datarepr == StringRepresentation::EMBEDDED)
                   ? INTERNAL_BUFFER_CAPACITY - embedded.data[INTERNAL_BUFFER_CAPACITY]
                   : heap.size;
    }

    Pointer Grow(SizeType newCapacity)
    {
        if (newCapacity <= InternalCapacity())
            return GetData();

        if (OPTIMIZER_LIKELY(m_Datarepr == StringRepresentation::HEAP))
        {
            heap.data =
                static_cast<ValueType*>(MemoryManager::Realloc(heap.data, (newCapacity + 1) * sizeof(ValueType)));
            heap.capacity = newCapacity;
            return heap.data;
        }

        const bool goingToHeap = (newCapacity > INTERNAL_BUFFER_CAPACITY);

        if (goingToHeap)
        {
            Pointer src = heap.data;
            SizeType size = heap.size;

            if (OPTIMIZER_LIKELY(m_Datarepr == StringRepresentation::EMBEDDED))
            {
                src = embedded.data;
                size = INTERNAL_BUFFER_CAPACITY - embedded.data[INTERNAL_BUFFER_CAPACITY];
            }

            Pointer dest = AllocateNewBuffer(newCapacity);
            memcpy(dest, src, size * sizeof(value_type));
            dest[size] = static_cast<ValueType>(0);

            heap.capacity = newCapacity;
            heap.data = dest;
            heap.size = size;
            m_Datarepr = StringRepresentation::HEAP;
            return heap.data;
        }

        if (m_Datarepr == StringRepresentation::EXTERNAL)
        {
            Pointer src = heap.data;
            SizeType size = heap.size;

            Pointer dest = embedded.data;
            memcpy(dest, src, size * sizeof(ValueType));
            dest[size] = static_cast<ValueType>(0);

            embedded.data[INTERNAL_BUFFER_CAPACITY] = static_cast<ValueType>(INTERNAL_BUFFER_CAPACITY - size);
            m_Datarepr = StringRepresentation::EMBEDDED;
        }

        return embedded.data;
    }

    Pointer AllocateNewBuffer(size_t lenWithoutNullTermintor) const
    {
        const SizeType toAlloc = sizeof(ValueType) * (lenWithoutNullTermintor + 1);
        void* heapPtr = MemoryManager::Malloc(toAlloc);
        return static_cast<Pointer>(heapPtr);
    }

    void MakeUnique()
    {
        if (m_Datarepr == StringRepresentation::EXTERNAL)
            Grow(Size());
    }

private:
    SizeType InternalCapacity() const
    {
        return (m_Datarepr == StringRepresentation::EMBEDDED) ? INTERNAL_BUFFER_CAPACITY : heap.capacity;
    }

    struct HeapAllocatedRepresentation
    {
        Pointer data;
        SizeType capacity;
        SizeType size;
    };

    static constexpr SizeType InternalCapMax(const SizeType& a, const SizeType& b) { return (a < b) ? b : a; }

    static constexpr SizeType INTERNAL_BUFFER_CAPACITY =
        InternalCapMax(sizeof(ValueType) == sizeof(int32_t)   ? static_cast<SizeType>(7)
                       : sizeof(ValueType) == sizeof(int16_t) ? static_cast<SizeType>(9)
                                                              : static_cast<SizeType>(19),
                       sizeof(HeapAllocatedRepresentation) / sizeof(ValueType));

    struct StackAllocatedRepresentation
    {
        ValueType data[INTERNAL_BUFFER_CAPACITY + 1];
    };

    union
    {
        StackAllocatedRepresentation embedded;
        HeapAllocatedRepresentation heap;
    };

    StringRepresentation m_Datarepr;
};