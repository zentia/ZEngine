#pragma once

#include <atomic>
#include <limits>

using GObjectID = std::size_t;

constexpr GObjectID k_invalid_gobject_id = std::numeric_limits<std::size_t>::max();

class ObjectIDAllocator
{
public:
    static GObjectID Alloc();

private:
    static std::atomic<GObjectID> m_NextId;
};