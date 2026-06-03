#pragma once

#include <iterator>

template<typename T>
class ContiguousIterator
{
public:
    using ValueType = T;
    using Pointer = ValueType*;

    ContiguousIterator()
        : m_Ptr(nullptr) {}
    explicit ContiguousIterator(Pointer ptr)
        : m_Ptr(ptr) {}

private:
    Pointer m_Ptr;
};

template<typename T>
class ConstContiguousIterator
{
public:
    using ValueType = T;
    using Pointer = ValueType*;

    ConstContiguousIterator()
        : m_Ptr(nullptr) {}
    explicit ConstContiguousIterator(Pointer ptr)
        : m_Ptr(ptr) {}

private:
    Pointer m_Ptr;
};