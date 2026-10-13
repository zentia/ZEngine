#pragma once

#include "DiagnosisTypes.h"

#include <cstdint>

namespace zengine::envcheck::diagnosis
{
struct EventLogReadOptions
{
    UtcTicks rangeStart = 0;
    UtcTicks rangeEnd = 0;
    // GetTickCount64() value after which reading stops; 0 means no deadline.
    std::uint64_t deadlineTickCount = 0;
};

// Reads the System and Application channels in-process through wevtapi.
// Never throws for "log not readable": it is reported through Observability.
EventCollection ReadEventLogs(const EventLogReadOptions& options);
} // namespace zengine::envcheck::diagnosis
