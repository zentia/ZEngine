#pragma once

#include "runtime/misc/input_event.h"

namespace Z
{
    class Event
    {
    private:
        InputEvent* m_input_event = nullptr;
    public:
        Event(/* args */);
        ~Event();
    };

} // namespace Z