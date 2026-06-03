#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include "core/base/Macro.h"

std::atomic<GObjectID> ObjectIDAllocator::m_NextId {0};

GObjectID ObjectIDAllocator::Alloc()
{
    std::atomic<GObjectID> new_object_ret = m_NextId.load();
    m_NextId++;
    if (m_NextId >= k_invalid_gobject_id)
    {
        LOG_FATAL(ZObject, "gobject id overflow");
    }

    return new_object_ret;
}