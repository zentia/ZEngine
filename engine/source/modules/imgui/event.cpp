#include "event.h"
#include "runtime/core/memory/memory_manager.h"

namespace Z
{
    Event::Event(/* args */)
    {
        m_input_event = MemoryManager::createObject<InputEvent>();
    }

    Event::~Event()
    {
        MemoryManager::destroyObject(m_input_event);
        m_input_event = nullptr;
    }

} // namespace Z