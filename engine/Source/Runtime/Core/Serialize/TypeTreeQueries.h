#pragma once
#include "Runtime/Utility/Hash128.h"
#include "TypeTree.h"
class TypeTreeQueries
{
public:
    static bool IsStreamedBinaryCompatible(const TypeTreeIterator& lhs, const TypeTreeIterator& rhs);
    static Hash128 HashTypeTree(TypeTreeIterator& iterator);
};